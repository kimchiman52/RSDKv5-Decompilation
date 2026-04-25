# MiSTer HPS (DE10-Nano, Cyclone V, Cortex-A9 armhf) platform file.
#
# Phase 1 scope: selects the MiSTer render backend (Graphics/MiSTer/*) via
# RETRO_SUBSYSTEM=MiSTer -> RSDK_USE_MISTER=1 (emitted by the parent
# dependencies/RSDKv5/CMakeLists.txt at line 155). Backend stubs log-and-return;
# no /dev/mem mmap yet. Pixel output lands in Phase 2.
#
# Includes: pkg-config driven SDL2 + libogg + libtheora (for input/audio and
# cutscene decode), ARM hardening on armhf cross-builds, Cocoa framework link
# on Mac-host iteration builds, install() rule for `cmake --install`.
#
# Include-order reminder: this file is included from
# dependencies/RSDKv5/CMakeLists.txt at line `include(platforms/${PLATFORM}.cmake)`,
# BEFORE target compile-definitions at line 153-166 which rely on our
# RETRO_SUBSYSTEM setting. We call add_executable(RetroEngine ...) here so
# subsequent target_sources/target_link_libraries in this file and the parent
# resolve correctly.

find_package(PkgConfig REQUIRED)

# --- Subsystem pin --------------------------------------------------------
# Force RETRO_SUBSYSTEM=MiSTer so parent line 155 emits RSDK_USE_MISTER=1.
# Honors a user-passed value ONLY if it's literally MiSTer; any other value
# is overridden (plan step 4 + decision table).
set(RETRO_SUBSYSTEM "MiSTer" CACHE STRING "The subsystem to use" FORCE)

option(USE_SDL_AUDIO "Whether or not to use SDL for audio instead of the default MiniAudio." ON)

add_executable(RetroEngine ${RETRO_FILES})

# Some distros (Debian 11) need this to prevent link errors (used by std::thread
# in the audio path). Same as Linux.cmake.
find_package(Threads REQUIRED)
target_link_libraries(RetroEngine Threads::Threads)

# Mod loader + RSDKv5 core use dlopen/dlsym/dlerror. Bullseye's armhf sysroot
# with clang-20 + binutils 2.35 no longer auto-pulls libdl on the link line,
# so we must list it explicitly. See docs/phase-0-plan.md Step 4 troubleshoot.
target_link_libraries(RetroEngine ${CMAKE_DL_LIBS})

# --- Mac-host iteration build requirement ---------------------------------
# RetroEngine.hpp:475 unconditionally `#include "cocoaHelpers.hpp"` when
# RETRO_PLATFORM==RETRO_OSX. When we build this platform file on a Mac host
# (for the Phase 1 validation loop), RETRO_PLATFORM is still RETRO_OSX (the
# `__APPLE__` predef sets it), so cocoaHelpers.mm MUST be compiled and the
# Cocoa/Foundation frameworks MUST be linked — otherwise the Mac build fails
# at link. Mirrors Darwin.cmake lines 8-21.
if(APPLE)
    enable_language(OBJCXX)
    target_sources(RetroEngine PRIVATE dependencies/mac/cocoaHelpers.mm)
    target_include_directories(RetroEngine PRIVATE dependencies/mac)
    target_link_libraries(RetroEngine "-framework Cocoa" "-framework Foundation")
    message(STATUS "MiSTer.cmake: Mac-host build detected; linking Cocoa/Foundation for cocoaHelpers.mm")
endif()

# --- libogg ---------------------------------------------------------------
# Mania's Video.cpp links against ogg/theora when RETRO_REVISION >= 3 (REV0U),
# regardless of RETRO_DISABLE_PLUS. Rely on distro libogg-dev:armhf (Linux
# cross) or Homebrew libogg (Mac host).
#
# Non-static link vars: the armhf sysroot only ships runtime .so for
# SDL2/ogg/theora — the _STATIC variant drags in `-ldrm -lgbm -lwayland-*`
# which the sysroot doesn't provide. MiSTer's runtime rootfs ships these
# SONAMEs, so dynamic linking is correct and matches on-device resolution.
# Homebrew on Mac also provides these as dylibs, so the same path works.
pkg_check_modules(OGG ogg)

if(NOT OGG_FOUND)
    set(COMPILE_OGG TRUE)
    message(NOTICE "libogg not found, attempting to build from source")
else()
    message("found libogg (dynamic link)")
    target_link_libraries(RetroEngine ${OGG_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${OGG_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${OGG_LDFLAGS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${OGG_CFLAGS})
endif()

# --- libtheora ------------------------------------------------------------
# dependencies/android/libtheora is NOT vendored in this tree (only libogg is);
# COMPILE_THEORA=TRUE would fail with missing sources. Install
# libtheora-dev:armhf in the cross container (setup-build-container.sh) or
# `brew install theora` on Mac.
pkg_check_modules(THEORA theora theoradec)

if(NOT THEORA_FOUND)
    message(FATAL_ERROR
            "libtheora/theoradec not found via pkg-config. "
            "Install libtheora-dev (armhf in the cross container, or `brew install theora` on Mac). "
            "Upstream RSDKv5 would try dependencies/android/libtheora here, but "
            "those sources are not vendored in this tree.")
else()
    message("found libtheora (dynamic link)")
    target_link_libraries(RetroEngine ${THEORA_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${THEORA_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${THEORA_LDFLAGS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${THEORA_CFLAGS})
endif()

# --- SDL2 -----------------------------------------------------------------
# SDL2 is linked for input (+ audio when USE_SDL_AUDIO=ON, which we default
# ON). The MiSTer render backend does NOT use SDL — it writes to the native
# FPGA framebuffer directly in Phase 2. Dynamic link for the sysroot reason
# above.
#
# TODO(phase-2): linker emits "ignoring duplicate libraries" for SDL2/ogg/
# theora because the parent dependencies/RSDKv5/CMakeLists.txt also adds them
# unconditionally. Cosmetic warning — inherited, not new damage. Clean up by
# gating the parent's generic SDL2/ogg/theora target_link_libraries on
# `NOT PORT_MISTER`, so only this file's pkg-config-derived link is kept.
pkg_check_modules(SDL2 sdl2 REQUIRED)
target_link_libraries(RetroEngine ${SDL2_LIBRARIES})
target_link_directories(RetroEngine PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_options(RetroEngine PRIVATE ${SDL2_LDFLAGS_OTHER})
target_compile_options(RetroEngine PRIVATE ${SDL2_CFLAGS})

if(USE_SDL_AUDIO)
    target_compile_definitions(RetroEngine PRIVATE RETRO_AUDIODEVICE_SDL2=1)
endif()

# --- MiSTer backend source surface ----------------------------------------
# MiSTerRenderDevice.cpp is textually #include'd by Drawing.cpp (see
# Drawing.cpp:132-146 after our Phase 1 patch) — do NOT add the .cpp to
# target_sources (would cause duplicate-symbol errors for every
# RenderDevice:: method).
#
# NativeVideoWriter.c IS a normal compile unit (NOT textually included); add
# it here so the linker picks it up. Guarded internally with
# #if defined(__linux__) && defined(PORT_MISTER); Mac-host builds compile
# the stub branch (all functions are no-ops / return-false).
#
# The .hpp / .h headers are listed for IDE surface — no effect on the build.
target_sources(RetroEngine PRIVATE
    RSDKv5/RSDK/Graphics/MiSTer/MiSTerRenderDevice.hpp
    RSDKv5/RSDK/Graphics/MiSTer/NativeVideoWriter.h
    RSDKv5/RSDK/Graphics/MiSTer/NativeVideoWriter.c
    # Phase 6: pacer + profiling helpers. Must be a standalone TU because
    # MiSTerRenderDevice.cpp is textually #include'd from Drawing.cpp from
    # *inside* `namespace RSDK { ... }` scope — any C++ namespace blocks in
    # the helper would nest under ::RSDK::, ODR-splitting the types and
    # poisoning RSDK:: lookups in the consuming TU. The helpers therefore
    # use **C linkage with flat names** (`mister_pacer_*`, `mister_perf_*`)
    # which bind to global scope regardless of the caller's open
    # namespaces. See the long comment block at the top of MiSTerPacer.hpp.
    RSDKv5/RSDK/Graphics/MiSTer/MiSTerPacer.hpp
    RSDKv5/RSDK/Graphics/MiSTer/MiSTerPacer.cpp
    # Fatal-signal backtrace handler (diagnostics for wrapper-context crashes
    # that gdb masks). Standalone C TU; links against libc/glibc's
    # backtrace() / backtrace_symbols_fd() via <execinfo.h>.
    RSDKv5/RSDK/Graphics/MiSTer/SigHandler.h
    RSDKv5/RSDK/Graphics/MiSTer/SigHandler.c
)

# Link -rdynamic so backtrace_symbols_fd gets readable function names for
# symbols in the main executable (otherwise it only resolves shared-lib
# symbols). Minor binary-size cost; on by default in many distros anyway.
target_link_options(RetroEngine PRIVATE -rdynamic)

# --- MiSTer defines -------------------------------------------------------
# We do NOT set RETRO_RENDERDEVICE_MISTER=1 here via target_compile_definitions:
# the engine's RetroEngine.hpp handles that conditionally when RSDK_USE_MISTER
# is defined (which the parent CMakeLists.txt emits based on RETRO_SUBSYSTEM).
# Re-defining it here would trigger -Wmacro-redefined.
target_compile_definitions(RetroEngine PRIVATE
    PORT_MISTER=1
    RETRO_MISTER=1
    # RSDK_USE_MISTER: the parent dependencies/RSDKv5/CMakeLists.txt:155 emits
    # RSDK_USE_${RETRO_SUBSYSTEM}=1, which for our RETRO_SUBSYSTEM="MiSTer"
    # value produces the literal macro "RSDK_USE_MiSTer" (camelCase). That
    # mismatches the all-caps `defined(RSDK_USE_MISTER)` check in
    # RetroEngine.hpp. Emit the all-caps variant explicitly here so the
    # render-device selection arm fires. Adding the all-caps define does NOT
    # conflict with the parent's camelCase emission — they are distinct
    # preprocessor identifiers.
    RSDK_USE_MISTER=1
    # ENABLE_PERF_TELEMETRY: forwarded as a compile define so future perf-
    # gating (Phase 6) can pick it up. Harmless when unused.
    ENABLE_PERF_TELEMETRY=$<BOOL:${ENABLE_PERF_TELEMETRY}>
)

# Phase 6 — closed-loop pacer lead time. The pacer targets
# (next_vsync - lead_ns) as the frame deadline so WriteFrame arrives with a
# small safety margin before the FPGA's next scan-out. 3sx ships 2 ms; knob
# here so live tuning does not require a recompile-and-patch round trip.
set(MISTER_PACER_LEAD_TIME_US "2000" CACHE STRING
    "Lead time (microseconds) for the MiSTer closed-loop frame pacer")
target_compile_definitions(RetroEngine PRIVATE
    MISTER_PACER_LEAD_TIME_US=${MISTER_PACER_LEAD_TIME_US}
)

# --- ARM hardening --------------------------------------------------------
# Applied ONLY when the cross toolchain's target triple is arm-linux-gnueabihf
# (or armv7-ish), so a Mac-host iteration build of PLATFORM=MiSTer is a no-op
# rather than a build failure.
option(ENABLE_MISTER_ARM_HARDENING "Conservative ARMv7 hard-float / NEON flags for MiSTer" OFF)

string(TOLOWER "${CMAKE_C_COMPILER_TARGET}" _mister_compiler_target_lc)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mister_system_processor_lc)
set(_mister_armv7_target_detected OFF)

if((_mister_compiler_target_lc MATCHES "armv7") OR
   (_mister_compiler_target_lc MATCHES "arm-linux-gnueabihf"))
    set(_mister_armv7_target_detected ON)
elseif((_mister_system_processor_lc MATCHES "armv7") OR
       (_mister_system_processor_lc MATCHES "armhf"))
    set(_mister_armv7_target_detected ON)
endif()

if(_mister_armv7_target_detected)
    set(ENABLE_MISTER_ARM_HARDENING ON CACHE BOOL "" FORCE)
endif()

if(ENABLE_MISTER_ARM_HARDENING AND _mister_armv7_target_detected)
    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
        target_compile_options(RetroEngine PRIVATE
            -mcpu=cortex-a9
            -mfpu=neon-vfpv3
            -mfloat-abi=hard
        )
        target_link_options(RetroEngine PRIVATE
            -mcpu=cortex-a9
            -mfpu=neon-vfpv3
            -mfloat-abi=hard
        )
        message(STATUS "MiSTer ARM hardening flags enabled: -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard")
    else()
        message(WARNING "MiSTer ARM hardening supports Clang/GNU only; skipping (compiler=${CMAKE_C_COMPILER_ID}).")
    endif()
elseif(ENABLE_MISTER_ARM_HARDENING)
    message(WARNING
            "ENABLE_MISTER_ARM_HARDENING=ON but non-armv7 target detected "
            "(system='${CMAKE_SYSTEM_PROCESSOR}', "
            "compiler_target='${CMAKE_C_COMPILER_TARGET}'). "
            "Skipping hardening flags — Mac iteration build will still compile.")
else()
    message(STATUS
            "PORT_MISTER active but ARMv7 hard-float target not detected "
            "(system='${CMAKE_SYSTEM_PROCESSOR}', "
            "compiler_target='${CMAKE_C_COMPILER_TARGET}'). Skipping ARM hardening flags.")
endif()

# --- install() rule -------------------------------------------------------
# Upstream RSDKv5 v1.1.1 ships no install(TARGETS RetroEngine ...) rule on any
# platform. We add one here so `cmake --install <dir> --prefix <p>` populates
# <p>/bin/RSDKv5U. Scope: the RetroEngine target is defined above in this same
# file, at engine-subdir scope, so install() here sees the target correctly.
install(TARGETS RetroEngine RUNTIME DESTINATION bin)

message(STATUS "MiSTer.cmake loaded. RETRO_SUBSYSTEM=${RETRO_SUBSYSTEM} ENABLE_MISTER_ARM_HARDENING=${ENABLE_MISTER_ARM_HARDENING}")
