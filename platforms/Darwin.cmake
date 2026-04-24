# macOS build file — mirrors Linux.cmake (pkg-config driven) and adds
# the Mac-specific cocoaHelpers.mm source + Cocoa framework link.
#
# Install deps via Homebrew:  brew install sdl2 libogg theora pkg-config

find_package(PkgConfig REQUIRED)

enable_language(OBJCXX)

add_executable(RetroEngine
    ${RETRO_FILES}
    dependencies/mac/cocoaHelpers.mm
)

target_include_directories(RetroEngine PRIVATE dependencies/mac)

set(RETRO_SUBSYSTEM "SDL2" CACHE STRING "The subsystem to use")
option(USE_SDL_AUDIO "Whether or not to use SDL for audio instead of the default MiniAudio." OFF)

find_package(Threads REQUIRED)
target_link_libraries(RetroEngine Threads::Threads "-framework Cocoa" "-framework Foundation")

pkg_check_modules(OGG ogg)
if(NOT OGG_FOUND)
    set(COMPILE_OGG TRUE)
    message(NOTICE "libogg not found, attempting to build from source")
else()
    message("found libogg")
    target_link_libraries(RetroEngine ${OGG_STATIC_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${OGG_STATIC_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${OGG_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${OGG_STATIC_CFLAGS})
endif()

pkg_check_modules(THEORA theora theoradec)
if(NOT THEORA_FOUND)
    message("could not find libtheora, attempting to build manually")
    set(COMPILE_THEORA TRUE)
else()
    message("found libtheora")
    target_link_libraries(RetroEngine ${THEORA_STATIC_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${THEORA_STATIC_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${THEORA_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${THEORA_STATIC_CFLAGS})
endif()

if(RETRO_SUBSYSTEM STREQUAL "SDL2")
    pkg_check_modules(SDL2 sdl2 REQUIRED)
    target_link_libraries(RetroEngine ${SDL2_STATIC_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${SDL2_STATIC_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${SDL2_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${SDL2_STATIC_CFLAGS})
elseif(RETRO_SUBSYSTEM STREQUAL "OGL")
    pkg_check_modules(GLFW glfw3 REQUIRED)
    target_link_libraries(RetroEngine ${GLFW_STATIC_LIBRARIES} "-framework OpenGL")
    target_link_directories(RetroEngine PRIVATE ${GLFW_STATIC_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${GLFW_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${GLFW_STATIC_CFLAGS})

    pkg_check_modules(GLEW glew REQUIRED)
    target_link_libraries(RetroEngine ${GLEW_STATIC_LIBRARIES})
    target_link_directories(RetroEngine PRIVATE ${GLEW_STATIC_LIBRARY_DIRS})
    target_link_options(RetroEngine PRIVATE ${GLEW_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${GLEW_STATIC_CFLAGS})
endif()

if(NOT RETRO_SUBSYSTEM STREQUAL SDL2)
    if(USE_SDL_AUDIO)
        pkg_check_modules(SDL2 sdl2 REQUIRED)
        target_link_libraries(RetroEngine ${SDL2_STATIC_LIBRARIES})
        target_link_directories(RetroEngine PRIVATE ${SDL2_STATIC_LIBRARY_DIRS})
        target_link_options(RetroEngine PRIVATE ${SDL2_STATIC_LDLIBS_OTHER})
        target_compile_options(RetroEngine PRIVATE ${SDL2_STATIC_CFLAGS})
        target_compile_definitions(RetroEngine PRIVATE RETRO_AUDIODEVICE_SDL2=1)
    endif()
endif()
