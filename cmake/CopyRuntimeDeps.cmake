cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
# Copie à côté de l'exécutable toutes les DLL dont il dépend (récursivement) et qui
# viennent de MSYS2 : SDL3, FreeType et ses dépendances, runtime MinGW…
# Usage : cmake -DEXE=<chemin.exe> -DSEARCH_DIR=<msys64/ucrt64/bin> -P CopyRuntimeDeps.cmake
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflict
    DIRECTORIES "${SEARCH_DIR}"
    PRE_EXCLUDE_REGEXES "^api-ms-.*" "^ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*")
# DLL présente à la fois à côté de l'exe et dans MSYS2 : on reprend celle de MSYS2.
foreach(name IN LISTS conflict_FILENAMES)
    foreach(p IN LISTS conflict_${name})
        string(FIND "${p}" "${SEARCH_DIR}" pos)
        if(pos EQUAL 0)
            list(APPEND resolved "${p}")
        endif()
    endforeach()
endforeach()
get_filename_component(dest "${EXE}" DIRECTORY)
foreach(dll IN LISTS resolved)
    get_filename_component(dir "${dll}" DIRECTORY)
    if(NOT dir STREQUAL dest)
        file(COPY "${dll}" DESTINATION "${dest}")
    endif()
endforeach()
