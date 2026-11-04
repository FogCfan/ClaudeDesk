cmake_minimum_required(VERSION 3.24)
project(ClaudeDesk VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CLAUDEDESK_BUILD_TESTS "Construire les tests unitaires" ON)

include(FetchContent)

# --- Dépendances --------------------------------------------------------------
find_package(SDL3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Freetype REQUIRED)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp)
# Polices rendues par FreeType (plus nettes, emoji en couleur) ; stb_truetype reste disponible.
target_compile_definitions(imgui_lib PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_ENABLE_STB_TRUETYPE)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}/misc/freetype
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3 Freetype::Freetype)

function(desk_warnings tgt)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endfunction()

# Copie les DLL du runtime MinGW à côté de l'exécutable (lancement hors MSYS2).
function(desk_copy_runtime tgt)
    if(NOT WIN32)
        return()
    endif()
    get_filename_component(_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${_bin}/${dll}")
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_bin}/${dll}" $<TARGET_FILE_DIR:${tgt}>)
        endif()
    endforeach()
endfunction()

# --- Coeur (sans SDL ni ImGui : testable) -------------------------------------
add_library(desk_core STATIC
    src/util/Strings.cpp
    src/util/Paths.cpp
    src/claude/Process.cpp
    src/claude/CommandLine.cpp
    src/claude/SessionConfig.cpp
    src/claude/Transcript.cpp
    src/claude/ClaudeSession.cpp
    src/claude/OneShot.cpp
    src/claude/UsageTracker.cpp
    src/claude/History.cpp
    src/workspace/Workspace.cpp
    src/app/Settings.cpp)
target_include_directories(desk_core PUBLIC src)
target_link_libraries(desk_core PUBLIC nlohmann_json::nlohmann_json)
desk_warnings(desk_core)

# --- Application ---------------------------------------------------------------
add_executable(ClaudeDesk
    src/main.cpp
    src/app/App.cpp
    src/app/AppMenus.cpp
    src/ui/Theme.cpp
    src/ui/Markdown.cpp
    src/ui/ConversationView.cpp
    src/ui/PromptEditor.cpp
    src/ui/UsagePanel.cpp
    src/ui/ConfigPanel.cpp
    src/ui/WorkspacePanel.cpp
    src/ui/PermissionPanel.cpp
    src/ui/Dashboard.cpp
    src/ui/Tutorial.cpp
    src/ui/Anim.cpp)
target_link_libraries(ClaudeDesk PRIVATE desk_core imgui_lib SDL3::SDL3)
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(ClaudeDesk PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
desk_warnings(ClaudeDesk)
if(WIN32)
    # Toutes les DLL nécessaires (SDL3, FreeType + dépendances, runtime) à côté de l'exe :
    # l'application se lance ensuite hors de MSYS2 (double-clic).
    get_filename_component(_msys_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET ClaudeDesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:ClaudeDesk> -DSEARCH_DIR=${_msys_bin}
                -P ${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDeps.cmake)
endif()

# --- Tests ---------------------------------------------------------------------
if(CLAUDEDESK_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _header_only)
    FetchContent_MakeAvailable(doctest)

    enable_testing()
    add_executable(desk_tests
        tests/test_main.cpp
        tests/test_strings.cpp
        tests/test_protocol.cpp
        tests/test_workspace.cpp)
    target_include_directories(desk_tests PRIVATE ${doctest_SOURCE_DIR})
    target_link_libraries(desk_tests PRIVATE desk_core)
    desk_warnings(desk_tests)
    desk_copy_runtime(desk_tests)
    add_test(NAME desk_tests COMMAND desk_tests)
endif()
