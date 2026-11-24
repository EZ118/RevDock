cmake_minimum_required(VERSION 3.24)
project(RevDock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
option(REVDOCK_ENABLE_X11 "Enable native X11 dock integration on Linux" ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Vendored dependencies
# ---------------------------------------------------------------------------

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(lib/freetype EXCLUDE_FROM_ALL)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_WERROR OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    set(SDL_X11 ${REVDOCK_ENABLE_X11} CACHE BOOL "" FORCE)
    set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(lib/SDL3 EXCLUDE_FROM_ALL)

set(SDLTTF_SHARED OFF CACHE BOOL "" FORCE)
set(SDLTTF_STATIC ON CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_TESTS OFF CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/SDL3_ttf EXCLUDE_FROM_ALL)

set(SDLIMAGE_SHARED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_STATIC ON CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDLIMAGE_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/SDL3_image EXCLUDE_FROM_ALL)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/lib/nlohmann/single_include")

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/lib")

# Resolve actual SDL target names across SDL releases.
set(REVDOCK_SDL_TARGET "")
foreach(candidate SDL3::SDL3 SDL3-static SDL3)
    if(TARGET ${candidate})
        set(REVDOCK_SDL_TARGET ${candidate})
        break()
    endif()
endforeach()
if(NOT REVDOCK_SDL_TARGET)
    message(FATAL_ERROR "SDL3 target not found")
endif()

set(REVDOCK_TTF_TARGET "")
foreach(candidate SDL3_ttf::SDL3_ttf SDL3_ttf-static SDL3_ttf)
    if(TARGET ${candidate})
        set(REVDOCK_TTF_TARGET ${candidate})
        break()
    endif()
endforeach()
if(NOT REVDOCK_TTF_TARGET)
    message(FATAL_ERROR "SDL3_ttf target not found")
endif()

set(REVDOCK_IMAGE_TARGET "")
foreach(candidate SDL3_image::SDL3_image SDL3_image-static SDL3_image)
    if(TARGET ${candidate})
        set(REVDOCK_IMAGE_TARGET ${candidate})
        break()
    endif()
endforeach()
if(NOT REVDOCK_IMAGE_TARGET)
    message(FATAL_ERROR "SDL3_image target not found")
endif()

# ---------------------------------------------------------------------------
# RevDock
# ---------------------------------------------------------------------------

add_library(revdock_core STATIC
    src/core/Blur.cpp
    src/core/FileUtil.cpp
    src/core/Log.cpp
    src/core/Paths.cpp
    src/config/Settings.cpp
    src/model/DockModel.cpp
    src/layout/DockLayout.cpp
    src/apps/AppIndex.cpp
)
target_include_directories(revdock_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(revdock_core PUBLIC ${REVDOCK_SDL_TARGET} nlohmann_json)
if(WIN32)
    target_link_libraries(revdock_core PUBLIC shell32 ole32)
endif()

file(GLOB_RECURSE REVDOCK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/core/(Blur|FileUtil|Log|Paths)\\.cpp$")
list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/config/Settings\\.cpp$")
list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/model/DockModel\\.cpp$")
list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/layout/DockLayout\\.cpp$")
list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/apps/AppIndex\\.cpp$")
if(WIN32)
    list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/platform/linux/")
else()
    list(FILTER REVDOCK_SOURCES EXCLUDE REGEX "/src/platform/windows/")
endif()

add_executable(revdock WIN32 ${REVDOCK_SOURCES})
target_include_directories(revdock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(revdock PRIVATE revdock_core)
target_compile_definitions(revdock PRIVATE
    REVDOCK_VERSION="${PROJECT_VERSION}"
    REVDOCK_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)
target_link_libraries(revdock PRIVATE
    ${REVDOCK_SDL_TARGET}
    ${REVDOCK_TTF_TARGET}
    ${REVDOCK_IMAGE_TARGET}
    Freetype::Freetype
    nlohmann_json
)

if(WIN32)
    target_link_libraries(revdock PRIVATE
        shell32 ole32 oleaut32 uuid propsys dwmapi shlwapi user32 gdi32 comdlg32 advapi32
    )
    if(MINGW)
        target_link_options(revdock PRIVATE -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(revdock PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(REVDOCK_ENABLE_X11)
        find_package(X11 REQUIRED)
        target_link_libraries(revdock PRIVATE X11::X11)
        if(TARGET X11::Xext)
            target_link_libraries(revdock PRIVATE X11::Xext)
            target_compile_definitions(revdock PRIVATE REVDOCK_HAVE_XSHAPE=1)
        endif()
        target_compile_definitions(revdock PRIVATE REVDOCK_HAVE_X11=1)
    endif()
endif()

if(MSVC)
    target_compile_options(revdock PRIVATE /W4 /permissive-)
else()
    target_compile_options(revdock PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET revdock POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:revdock>/assets"
    COMMENT "Copying RevDock assets"
)

install(TARGETS revdock RUNTIME DESTINATION .)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
if(WIN32)
    install(DIRECTORY assets/ DESTINATION assets)
else()
    install(DIRECTORY assets/ DESTINATION share/revdock/assets)
endif()
install(FILES lib/SDL3/LICENSE.txt DESTINATION licenses/SDL3)
install(FILES lib/SDL3_ttf/LICENSE.txt DESTINATION licenses/SDL3_ttf)
install(FILES lib/SDL3_image/LICENSE.txt DESTINATION licenses/SDL3_image)
install(FILES lib/freetype/LICENSE.TXT DESTINATION licenses/freetype)
install(FILES lib/nlohmann/LICENSE.MIT DESTINATION licenses/nlohmann)
install(FILES lib/doctest/LICENSE.txt DESTINATION licenses/doctest)
install(FILES assets/fonts/Inter/OFL.txt DESTINATION licenses/Inter)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
