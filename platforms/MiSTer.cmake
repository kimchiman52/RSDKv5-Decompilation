# MiSTer HPS (DE10-Nano, Cyclone V, Cortex-A9 armhf) platform file.
#
# Phase 0 scope: this file is a Linux.cmake clone trimmed to only the SDL2 +
# SDL_AUDIO + libogg + libtheora dependency paths, plus an ARM-hardening block
# and an install() rule for RetroEngine. It deliberately falls back to the
# upstream RETRO_RENDERDEVICE_SDL2 backend — no RETRO_RENDERDEVICE_MISTER yet;
# that lands in Phase 1.
#
# Include-order reminder: this file is included from
# dependencies/RSDKv5/CMakeLists.txt at line `include(platforms/${PLATFORM}.cmake)`,
# AFTER `add_executable(RetroEngine ...)` in this same platform file. We carry
# the same pkg-config pattern Linux.cmake uses, but mirror Darwin.cmake's
# `_STATIC_LIBRARY_DIRS` forwarding so cross-sysroot link search paths are
# respected by the linker under a cross toolchain.

find_package(PkgConfig REQUIRED)

add_executable(RetroEngine ${RETRO_FILES})

set(RETRO_SUBSYSTEM "SDL2" CACHE STRING "The subsystem to use")
option(USE_SDL_AUDIO "Whether or not to use SDL for audio instead of the default MiniAudio." ON)

# Some distros (Debian 11) need this to prevent link errors (used by std::thread
# in the audio path). Same as Linux.cmake.
find_package(Threads REQUIRED)
target_link_libraries(RetroEngine Threads::Threads)

# Mod loader + RSDKv5 core use dlopen/dlsym/dlerror. Bullseye's armhf sysroot
# with clang-20 + binutils 2.35 no longer auto-pulls libdl on the link line,
# so we must list it explicitly. See docs/phase-0-plan.md Step 4 troubleshoot.
target_link_libraries(RetroEngine ${CMAKE_DL_LIBS})

# ---- libogg ---------------------------------------------------------------
# Mania's Video.cpp links against ogg/theora when RETRO_REVISION >= 3 (REV0U),
# regardless of RETRO_DISABLE_PLUS. Rely on distro libogg-dev:armhf from Step 1
# of the container bootstrap (see tools/mister/setup-build-container.sh).
#
# We deliberately use the NON-static pkg-config vars (no _STATIC suffix). The
# armhf sysroot ships only the runtime `.so` for SDL2/ogg/theora — the static
# pkg-config variant drags in `-ldrm -lgbm -lwayland-*` etc. which the sysroot
# doesn't provide. MiSTer's runtime rootfs ships these SONAMEs, so dynamic
# linking is the correct choice and matches what the binary will actually
# resolve on device.
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

# ---- libtheora ------------------------------------------------------------
# dependencies/android/libtheora is NOT vendored in this tree (only libogg is);
# COMPILE_THEORA=TRUE would fail with missing sources. Install
# libtheora-dev:armhf instead (setup-build-container.sh does this).
pkg_check_modules(THEORA theora theoradec)

if(NOT THEORA_FOUND)
    message(FATAL_ERROR
            "libtheora/theoradec not found via pkg-config. "
            "Install libtheora-dev:armhf in the build container "
            "(see tools/mister/setup-build-container.sh). "
            "Upstream RSDKv5 would try dependencies/android/libtheora here, but "
            "those sources are not vendored in this tree.")
else()
    message("found libtheora (dynamic link)")
    target_link_libraries(RetroEngine ${THEORA_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${THEORA_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${THEORA_LDFLAGS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${THEORA_CFLAGS})
endif()

# ---- SDL2 -----------------------------------------------------------------
# MiSTer supports SDL2 subsystem only. No OGL/VK/GLFW/GLEW branches — those
# are intentionally dropped. Dynamic link for the reason given above.
if(RETRO_SUBSYSTEM STREQUAL "SDL2")
    pkg_check_modules(SDL2 sdl2 REQUIRED)
    target_link_libraries(RetroEngine ${SDL2_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${SDL2_LDFLAGS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${SDL2_CFLAGS})
else()
    message(FATAL_ERROR
            "PLATFORM=MiSTer currently supports RETRO_SUBSYSTEM=SDL2 only "
            "(requested: ${RETRO_SUBSYSTEM}).")
endif()

# Parallel SDL2 audio pickup if the subsystem is not already SDL2 (defensive;
# matches Linux.cmake/Darwin.cmake shape, even though MiSTer only accepts
# SDL2 above — keeps future flexibility).
if(NOT RETRO_SUBSYSTEM STREQUAL SDL2)
    if(USE_SDL_AUDIO)
        pkg_check_modules(SDL2 sdl2 REQUIRED)
        target_link_libraries(RetroEngine ${SDL2_LIBRARIES})
        target_link_directories(RetroEngine PRIVATE ${SDL2_LIBRARY_DIRS})
        target_link_options(RetroEngine PRIVATE ${SDL2_LDFLAGS_OTHER})
        target_compile_options(RetroEngine PRIVATE ${SDL2_CFLAGS})
        target_compile_definitions(RetroEngine PRIVATE RETRO_AUDIODEVICE_SDL2=1)
    endif()
endif()

# ---- MiSTer defines -------------------------------------------------------
# Phase 0 only forwards PORT_MISTER / RETRO_MISTER. We intentionally DO NOT
# set RETRO_RENDERDEVICE_MISTER here — that backend is Phase 1. For Phase 0
# we want the engine to pick up the stock SDL2 backend so it can at least
# attempt to boot (expected behavior on device: SDL_CreateRenderer fails or
# returns NULL under SDL_VIDEODRIVER=dummy; engine logs and exits).
#
# RSDK_USE_SDL2 is already set by dependencies/RSDKv5/CMakeLists.txt based on
# RETRO_SUBSYSTEM, which the engine header translates into
# RETRO_RENDERDEVICE_SDL2 — so we MUST NOT re-define that ourselves (would
# trigger -Wmacro-redefined).
target_compile_definitions(RetroEngine PRIVATE
    PORT_MISTER=1
    RETRO_MISTER=1
    # ENABLE_PERF_TELEMETRY: Phase 0 forwards the option value as a compile
    # define so future perf-gating (Phase 6) can pick it up. Harmless when
    # unused. Intentionally NOT wrapped in $<BOOL:>, so a plain ON/OFF macro
    # value is visible in the preprocessor.
    ENABLE_PERF_TELEMETRY=$<BOOL:${ENABLE_PERF_TELEMETRY}>
)

# ---- ARM hardening --------------------------------------------------------
# Mirrors 3sx-mister's CMakeLists.txt:169-199 pattern. Applied ONLY when the
# cross toolchain's target triple is arm-linux-gnueabihf (or armv7-ish), so a
# macOS host-build of PLATFORM=MiSTer (hypothetical) is a no-op rather than a
# build failure.
if(PORT_MISTER)
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
        message(STATUS
                "PORT_MISTER active but ARMv7 hard-float target not detected "
                "(system='${CMAKE_SYSTEM_PROCESSOR}', "
                "compiler_target='${CMAKE_C_COMPILER_TARGET}'). Skipping ARM hardening flags.")
    endif()
endif()

# ---- install() rule -------------------------------------------------------
# Upstream RSDKv5 v1.1.1 ships NO install(TARGETS RetroEngine ...) rule on any
# platform. We add one here so `cmake --install <dir> --prefix <p>` populates
# <p>/bin/RSDKv5U. Scope: the RetroEngine target is defined ABOVE in this same
# file, at engine-subdir scope, so install() here sees the target correctly.
install(TARGETS RetroEngine RUNTIME DESTINATION bin)
