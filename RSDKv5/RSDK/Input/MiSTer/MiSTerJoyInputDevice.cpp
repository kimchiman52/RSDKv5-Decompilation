// MiSTer joy input device — reads consolidated joypad state from the
// shared-memory region populated by the HPS-side wrapper
// (vendor/Main_MiSTer/sonicmania_wrapper.cpp:2815-2834).
//
// Why this exists: MiSTercade and other MiSTer-front-panel inputs do NOT
// always surface as evdev /dev/input/event* devices that SDL2 can pick up.
// The MiSTer firmware reads them through its own GPIO/serial poll loop and
// only exposes them through input_get_joy_mask(). Without consuming that,
// our SDL2InputDevice would see nothing on a JAMMA cabinet even though
// MiSTer-menu navigation works. This device fills the gap so MiSTercade
// users have *any* in-game input at all. Regular USB pads connected to the
// DE10-Nano keep working through SDL2InputDevice in parallel; the two
// devices OR-merge harmlessly into the same controller[] state.
//
// SHM layout / magic / version are duplicated from
// vendor/Main_MiSTer/mister_joy_shm.h to avoid pulling that header into the
// engine submodule's include path. Keep the constants in lock-step on both
// sides if either schema changes.
//
// Textually #include'd from Input.cpp under `#if defined(RSDK_USE_MISTER)`,
// so this file inherits Input.cpp's `using namespace RSDK;` and the engine
// symbol surface (controller[], stickL[], inputDeviceList, PrintLog, …).

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cmath>

namespace {

constexpr const char *MISTER_JOY_SHM_DEFAULT_PATH = "/dev/shm/sonicmania-joy";
constexpr uint32      MISTER_JOY_SHM_MAGIC        = 0x534D4E41;  // "SMNA"
constexpr uint32      MISTER_JOY_SHM_VERSION      = 1;
constexpr int         MISTER_JOY_MAX_PLAYERS      = 2;

struct MisterJoyShm {
    uint32 magic;
    uint32 version;
    uint32 joy_mask[MISTER_JOY_MAX_PLAYERS];
    int8   left_stick_x[MISTER_JOY_MAX_PLAYERS];
    int8   left_stick_y[MISTER_JOY_MAX_PLAYERS];
    int8   right_stick_x[MISTER_JOY_MAX_PLAYERS];
    int8   right_stick_y[MISTER_JOY_MAX_PLAYERS];
};

// Layout-drift defense: this struct is duplicated by hand from
// vendor/Main_MiSTer/mister_joy_shm.h. If either side reorders or resizes
// fields without bumping MISTER_JOY_SHM_VERSION, MiSTercade users would see
// silently scrambled input. Catch the size/offset case at compile time.
static_assert(sizeof(MisterJoyShm) == 24, "MiSTer joy SHM size drift -- update both sides + bump version");
static_assert(offsetof(MisterJoyShm, joy_mask)      ==  8, "MiSTer joy SHM joy_mask offset drift");
static_assert(offsetof(MisterJoyShm, left_stick_x)  == 16, "MiSTer joy SHM left_stick_x offset drift");
static_assert(offsetof(MisterJoyShm, right_stick_x) == 20, "MiSTer joy SHM right_stick_x offset drift");

// Bit-to-button binding is governed by the CORE-SPECIFIC CONF_STR `J1` line,
// not by user_io.h's JOY_BTN1..4 aliases. For Sonic Mania
// (vendor/Menu_MiSTer/menu.sv:316):
//   J1,A,B,X,Y,Select,Start;
// → bit 4 = "A", bit 5 = "B", bit 6 = "X", bit 7 = "Y", bit 8 = "Select",
//   bit 9 = "Start". This is the slot order the user's MiSTer "Define joy
//   buttons" wizard uses (per-controller .map files at /media/fat/config/
//   inputs/Sonic_Mania_input_<vid>_<pid>_v3.map).
//
// Earlier this file used the legacy user_io.h convention where
// BTN3=Select / BTN4=Start, which mismatched the CONF_STR for cores that
// expose >4 face buttons. Symptom: pressing physical Start on the pad
// arrived at the engine as keyY (because slot 5 / bit 9 was being decoded
// as MJ_Y), and Y was decoded as keyStart, etc. The new constants are
// named after the CONF_STR slot label they correspond to so the contract
// is explicit.
constexpr uint32 MJ_RIGHT       = 0x0001;
constexpr uint32 MJ_LEFT        = 0x0002;
constexpr uint32 MJ_DOWN        = 0x0004;
constexpr uint32 MJ_UP          = 0x0008;
constexpr uint32 MJ_SLOT_A      = 0x0010;
constexpr uint32 MJ_SLOT_B      = 0x0020;
constexpr uint32 MJ_SLOT_X      = 0x0040;
constexpr uint32 MJ_SLOT_Y      = 0x0080;
constexpr uint32 MJ_SLOT_SELECT = 0x0100;
constexpr uint32 MJ_SLOT_START  = 0x0200;

static MisterJoyShm *g_mister_joy_shm    = nullptr;
static int           g_mister_joy_shm_fd = -1;

// Convert SHM int8 stick (-128..127) to float [-1, 1] with the same XInput-
// style radial deadzone the SDL2 backend uses (7864/24903 on int16 scale).
// Stick values are int8 so we shift up by 256 to compare on the int16 grid.
static inline void mister_axis_to_delta(int8 raw_x, int8 raw_y, float &out_x, float &out_y)
{
    float fx = (float)raw_x * 256.0f;
    float fy = (float)raw_y * 256.0f;
    float div = sqrtf(fx * fx + fy * fy);
    if (div <= 7864.0f) {
        out_x = 0.0f;
        out_y = 0.0f;
    }
    else {
        float scale = (fminf(32767.0f, div) - 7864.0f) / 24903.0f;
        out_x = (fx / div) * scale;
        out_y = (fy / div) * scale;
    }
}

}  // anonymous namespace

namespace RSDK
{
namespace SKU
{

struct InputDeviceMiSTerJoy : InputDevice {
    int    player;       // 0 = P1, 1 = P2
    uint32 prevMask;
    int8   prev_lsx, prev_lsy, prev_rsx, prev_rsy;

    void UpdateInput();
    void ProcessInput(int32 controllerID);
    void CloseDevice();
};

void InputDeviceMiSTerJoy::UpdateInput()
{
    if (!g_mister_joy_shm)
        return;
    // Defensive: wrapper may TRUNC + re-init the SHM on a relaunch.
    if (g_mister_joy_shm->magic != MISTER_JOY_SHM_MAGIC)
        return;

    uint32 mask = g_mister_joy_shm->joy_mask[player];
    int8   lsx  = g_mister_joy_shm->left_stick_x[player];
    int8   lsy  = g_mister_joy_shm->left_stick_y[player];
    int8   rsx  = g_mister_joy_shm->right_stick_x[player];
    int8   rsy  = g_mister_joy_shm->right_stick_y[player];

    bool changed = (mask != prevMask) || (lsx != prev_lsx) || (lsy != prev_lsy) || (rsx != prev_rsx) || (rsy != prev_rsy);
    if (changed) {
        this->inactiveTimer[0] = 0;
        this->anyPress         = true;
    }
    else {
        ++this->inactiveTimer[0];
        this->anyPress = false;
    }

    // inactiveTimer[1] tracks confirm-button activity (A/Start), used by the
    // RSDK menu auto-assign heuristic in GetFilteredInputDeviceID().
    bool confirm_now  = (mask & (MJ_SLOT_A | MJ_SLOT_START)) != 0;
    bool confirm_prev = (prevMask & (MJ_SLOT_A | MJ_SLOT_START)) != 0;
    if (confirm_now && !confirm_prev)
        this->inactiveTimer[1] = 0;
    else
        ++this->inactiveTimer[1];

    prevMask = mask;
    prev_lsx = lsx;
    prev_lsy = lsy;
    prev_rsx = rsx;
    prev_rsy = rsy;

    ProcessInput(CONT_ANY);
}

void InputDeviceMiSTerJoy::ProcessInput(int32 controllerID)
{
    if (!g_mister_joy_shm)
        return;

    uint32 mask = g_mister_joy_shm->joy_mask[player];

    bool stateUp     = (mask & MJ_UP)          != 0;
    bool stateDown   = (mask & MJ_DOWN)        != 0;
    bool stateLeft   = (mask & MJ_LEFT)        != 0;
    bool stateRight  = (mask & MJ_RIGHT)       != 0;
    bool stateA      = (mask & MJ_SLOT_A)      != 0;
    bool stateB      = (mask & MJ_SLOT_B)      != 0;
    bool stateX      = (mask & MJ_SLOT_X)      != 0;
    bool stateY      = (mask & MJ_SLOT_Y)      != 0;
    bool stateSelect = (mask & MJ_SLOT_SELECT) != 0;
    bool stateStart  = (mask & MJ_SLOT_START)  != 0;

    controller[controllerID].keyUp.press     |= stateUp;
    controller[controllerID].keyDown.press   |= stateDown;
    controller[controllerID].keyLeft.press   |= stateLeft;
    controller[controllerID].keyRight.press  |= stateRight;
    controller[controllerID].keyA.press      |= stateA;
    controller[controllerID].keyB.press      |= stateB;
    controller[controllerID].keyX.press      |= stateX;
    controller[controllerID].keyY.press      |= stateY;
    controller[controllerID].keyStart.press  |= stateStart;
    controller[controllerID].keySelect.press |= stateSelect;
    // keyC has no CONF_STR slot in Sonic Mania (J1 is A,B,X,Y,Select,Start)
    // but Mania's UFO_Player.c:243 jump check OR's keyA|keyB|keyC|keyX, and
    // some other game state checks read keyC. OR all 4 face buttons into
    // keyC so any of them counts as a "jump" key for code that polls keyC.
    controller[controllerID].keyC.press      |= stateA | stateB | stateX | stateY;

    float hL, vL, hR, vR;
    mister_axis_to_delta(g_mister_joy_shm->left_stick_x[player],
                         g_mister_joy_shm->left_stick_y[player], hL, vL);
    mister_axis_to_delta(g_mister_joy_shm->right_stick_x[player],
                         g_mister_joy_shm->right_stick_y[player], hR, vR);
    // Match SDL2 backend's Y convention (positive = up).
    vL = -vL;
    vR = -vR;

#if RETRO_REV02
    stickL[controllerID].hDelta = hL;
    stickL[controllerID].vDelta = vL;
    stickL[controllerID].keyUp.press    |= vL >  INPUT_DEADZONE;
    stickL[controllerID].keyDown.press  |= vL < -INPUT_DEADZONE;
    stickL[controllerID].keyLeft.press  |= hL < -INPUT_DEADZONE;
    stickL[controllerID].keyRight.press |= hL >  INPUT_DEADZONE;

    // Match SDL2InputDevice.cpp:138-139 right-stick h/v swap (long-standing
    // upstream bug, but parity matters more than correctness here -- Mania
    // doesn't use the right stick for gameplay).
    stickR[controllerID].hDelta = vR;
    stickR[controllerID].vDelta = hR;
    stickR[controllerID].keyUp.press    |= hR >  INPUT_DEADZONE;
    stickR[controllerID].keyDown.press  |= hR < -INPUT_DEADZONE;
    stickR[controllerID].keyLeft.press  |= vR < -INPUT_DEADZONE;
    stickR[controllerID].keyRight.press |= vR >  INPUT_DEADZONE;

    // Mania's CONF_STR has no L/R/L2/R2/L3/R3 slots, so triggerL/R and
    // stickL/R clicks are not exposed via SHM. Leave their state alone.
#endif
}

void InputDeviceMiSTerJoy::CloseDevice()
{
    this->active     = false;
    this->isAssigned = false;
}

static InputDeviceMiSTerJoy *InitMiSTerJoyInputDevice(uint32 id, int player)
{
    if (inputDeviceCount >= INPUTDEVICE_COUNT)
        return nullptr;

    if (inputDeviceList[inputDeviceCount] && inputDeviceList[inputDeviceCount]->active)
        return nullptr;

    if (inputDeviceList[inputDeviceCount])
        delete inputDeviceList[inputDeviceCount];

    inputDeviceList[inputDeviceCount] = new InputDeviceMiSTerJoy();

    InputDeviceMiSTerJoy *device = (InputDeviceMiSTerJoy *)inputDeviceList[inputDeviceCount];

    device->player   = player;
    device->prevMask = 0;
    device->prev_lsx = device->prev_lsy = device->prev_rsx = device->prev_rsy = 0;

    device->active   = true;
    device->disabled = false;
    // Pretend Xbox-class so existing controller-type queries don't trip.
    device->gamepadType = (DEVICE_API_SDL2 << 16) | (DEVICE_TYPE_CONTROLLER << 8) | (DEVICE_XBOX << 0);
    device->id          = id;

    for (int32 i = 0; i < PLAYER_COUNT; ++i) {
        if (inputSlots[i] == id) {
            inputSlotDevices[i] = device;
            device->isAssigned  = true;
        }
    }

    inputDeviceCount++;
    return device;
}

static void InitMiSTerJoyInputAPI()
{
    const char *path = std::getenv("SONIC_MANIA_JOY_SHM");
    if (!path || !*path)
        path = MISTER_JOY_SHM_DEFAULT_PATH;

    // Read-only: the engine never writes to the SHM. Reduces blast radius if
    // anything ever goes wrong with the wrapper-side allocator and rules out
    // "engine corrupted my SHM" as a hypothesis during MiSTercade triage.
    g_mister_joy_shm_fd = open(path, O_RDONLY);
    if (g_mister_joy_shm_fd < 0) {
        PrintLog(PRINT_NORMAL,
                 "MiSTerJoy: SHM not available at %s (errno=%d) -- wrapper not running, skipping",
                 path, errno);
        return;
    }

    void *ptr = mmap(NULL, sizeof(MisterJoyShm), PROT_READ, MAP_SHARED, g_mister_joy_shm_fd, 0);
    if (ptr == MAP_FAILED) {
        PrintLog(PRINT_NORMAL, "MiSTerJoy: mmap failed (errno=%d), skipping", errno);
        close(g_mister_joy_shm_fd);
        g_mister_joy_shm_fd = -1;
        return;
    }

    g_mister_joy_shm = (MisterJoyShm *)ptr;
    if (g_mister_joy_shm->magic != MISTER_JOY_SHM_MAGIC || g_mister_joy_shm->version != MISTER_JOY_SHM_VERSION) {
        PrintLog(PRINT_NORMAL,
                 "MiSTerJoy: SHM header mismatch (magic=0x%08x version=%u), skipping",
                 g_mister_joy_shm->magic, g_mister_joy_shm->version);
        munmap(g_mister_joy_shm, sizeof(MisterJoyShm));
        g_mister_joy_shm = nullptr;
        close(g_mister_joy_shm_fd);
        g_mister_joy_shm_fd = -1;
        return;
    }

    PrintLog(PRINT_NORMAL,
             "MiSTerJoy: SHM ready at %s (magic=0x%08x v%u), registering %d player devices",
             path, g_mister_joy_shm->magic, g_mister_joy_shm->version, MISTER_JOY_MAX_PLAYERS);

    // Synthetic IDs: high half "MJ" (0x4D4A), low half = player index. SDL2
    // controller IDs come from SDL_JoystickID (small ints), so this avoids
    // collision and is recognizable in PrintLog dumps.
    for (int p = 0; p < MISTER_JOY_MAX_PLAYERS; ++p) {
        InitMiSTerJoyInputDevice(0x4D4A0000u | (uint32)p, p);
    }
}

static void ReleaseMiSTerJoyInputAPI()
{
    // Unmap and close the SHM. Matters when the engine gets relaunched
    // in-place (MiSTer core reload of the same binary) -- without this,
    // each restart would leak the mapping + fd, and after enough cycles
    // the process would exhaust its file-descriptor limit. The InputDevice
    // instances themselves are owned by inputDeviceList and freed by
    // RemoveInputDevice / engine shutdown elsewhere.
    if (g_mister_joy_shm) {
        munmap(g_mister_joy_shm, sizeof(MisterJoyShm));
        g_mister_joy_shm = nullptr;
    }
    if (g_mister_joy_shm_fd >= 0) {
        close(g_mister_joy_shm_fd);
        g_mister_joy_shm_fd = -1;
    }
}

}  // namespace SKU
}  // namespace RSDK
