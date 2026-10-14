cmake_minimum_required(VERSION 3.20)
project(OAR LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The Nebula menu project provides the ImGui theme/widgets and the vendored Dear ImGui + FreeType.
set(NEBULA_DIR ${CMAKE_SOURCE_DIR}/../menu CACHE PATH "Path to the Nebula menu project")
set(IMGUI_DIR ${NEBULA_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
    message(FATAL_ERROR "Dear ImGui not found in ${IMGUI_DIR}: set NEBULA_DIR to the menu project")
endif()

set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
add_subdirectory(${NEBULA_DIR}/third_party/freetype ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
set_source_files_properties(${NEBULA_DIR}/third_party/freetype/src/base/ftver.rc DIRECTORY ${NEBULA_DIR}/third_party/freetype PROPERTIES HEADER_FILE_ONLY ON)

set(COMMON_DEFINITIONS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
                       IMGUI_DEFINE_MATH_OPERATORS IMGUI_ENABLE_FREETYPE)

# --- Game access: process, Unreal reflection, scanner thread, features (no UI) -------------------------
add_library(oar_core STATIC
    src/util/paths.cpp
    src/game/process.cpp
    src/game/cache.cpp
    src/game/unreal.cpp
    src/game/scanner.cpp
    src/features/checkin.cpp
    src/features/ghost.cpp
    src/settings.cpp
)
target_include_directories(oar_core PUBLIC src ${IMGUI_DIR})
target_compile_definitions(oar_core PUBLIC ${COMMON_DEFINITIONS})
target_link_libraries(oar_core PUBLIC ole32 shell32 user32 advapi32 uuid)
if(MSVC)
    target_compile_options(oar_core PUBLIC /utf-8 /W3)
else()
    target_compile_options(oar_core PUBLIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# --- The application: Nebula menu window + ESP overlay + scanner ---------------------------------------
add_executable(oar WIN32
    src/main.cpp
    src/demo.cpp
    src/menu/shell.cpp
    src/menu/stats.cpp
    src/menu/config_store.cpp
    src/menu/page_overview.cpp
    src/menu/page_esp.cpp
    src/menu/page_heist.cpp
    src/menu/page_configs.cpp
    src/menu/page_settings.cpp
    src/overlay/overlay.cpp
    src/overlay/esp.cpp
    src/util/keys.cpp
    src/util/screenshot.cpp
    ${NEBULA_DIR}/src/theme.cpp     # shared with the Nebula menu project, unmodified
    ${NEBULA_DIR}/src/widgets.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    oar.rc
)
# `src` first, so our headers win over same-named ones in the Nebula project (menu.h, app.h, ...).
target_include_directories(oar PRIVATE src ${NEBULA_DIR}/src ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/freetype)
target_link_libraries(oar PRIVATE oar_core freetype d3d9 dwmapi shell32 psapi ole32 uuid advapi32 gdi32 user32)
set_source_files_properties(oar.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/oar.manifest)
# Not "oar.exe": that is the name of the game's own launcher, and Windows process names are case-insensitive.
set_target_properties(oar PROPERTIES OUTPUT_NAME oar_external)
if(MSVC)
    target_compile_options(oar PRIVATE /utf-8)
else()
    target_link_options(oar PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# --- Self-test: everything that can be verified without the game ---------------------------------------
add_executable(oar_selftest tools/selftest.cpp src/menu/config_store.cpp)
target_link_libraries(oar_selftest PRIVATE oar_core)
if(NOT MSVC)
    target_link_options(oar_selftest PRIVATE -static -static-libgcc -static-libstdc++)
endif()
enable_testing()
add_test(NAME selftest COMMAND oar_selftest)

# --- Console probe: prints what the scanner sees (development aid) ------------------------------------
add_executable(oar_probe tools/probe.cpp)
target_link_libraries(oar_probe PRIVATE oar_core)
if(NOT MSVC)
    target_link_options(oar_probe PRIVATE -static -static-libgcc -static-libstdc++)
endif()
