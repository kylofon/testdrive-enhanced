cmake_minimum_required(VERSION 3.24)
project(testdrive_enhanced C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)

file(GLOB_RECURSE TD_SOURCES CONFIGURE_DEPENDS src/*.c)
file(GLOB_RECURSE TD_ENHANCED_SOURCES CONFIGURE_DEPENDS src/enhanced/*.c)
# The Amiga engine (src/amiga, the --amiga features): its globals are renamed by a force-included header so that
# they do not clash with the DOS game's functions of the same names.
file(GLOB_RECURSE TD_AMIGA_SOURCES CONFIGURE_DEPENDS src/amiga/*.c)

add_executable(testdrive-enhanced ${TD_SOURCES})
target_include_directories(testdrive-enhanced PRIVATE src)
target_link_libraries(testdrive-enhanced PRIVATE SDL3::SDL3)

if(MSVC)
    target_compile_options(testdrive-enhanced PRIVATE /W3)
    set_source_files_properties(${TD_AMIGA_SOURCES} PROPERTIES COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/amiga/anames.h")
else()
    # The memory model reads multi-byte values through byte pointers (see src/mem.h).
    target_compile_options(testdrive-enhanced PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
    # The renderer does per-pixel work every frame; keep it optimised in Debug builds.
    set_source_files_properties(${TD_ENHANCED_SOURCES} PROPERTIES COMPILE_OPTIONS -O2)
    # The Amiga engine composes its screens and mixes Paula's four channels every frame: optimised too.
    set_source_files_properties(${TD_AMIGA_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/amiga/anames.h;-O2")
endif()

if(WIN32)
    add_custom_command(TARGET testdrive-enhanced POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:testdrive-enhanced> $<TARGET_FILE_DIR:testdrive-enhanced>
        COMMAND_EXPAND_LISTS)
endif()

# The game's launcher ("Test Drive Enhanced"): the version (PC or Amiga), the game folders, the options and the
# Amiga features; built beside testdrive-enhanced.exe. It needs wxWidgets 3.2 (MSYS2:
# mingw-w64-x86_64-wxwidgets3.2-msw) and a C++17 compiler.
option(TDE_LAUNCHER "Also build the launcher, Test Drive Enhanced.exe (wxWidgets)" ON)
if(TDE_LAUNCHER)
    # a wxWidgets package config (MSYS2, wxWidgets' own builds) or wx-config (distributions); launcher/ finds it
    find_package(wxWidgets 3.2 CONFIG QUIET COMPONENTS core base)
    find_program(TDE_WX_CONFIG NAMES wx-config wx-config-3.2)
    if(wxWidgets_FOUND OR TDE_WX_CONFIG)
        add_subdirectory(launcher)
    else()
        message(STATUS "wxWidgets 3.2 not found: the launcher is not built (-DTDE_LAUNCHER=OFF hides this)")
    endif()
endif()
