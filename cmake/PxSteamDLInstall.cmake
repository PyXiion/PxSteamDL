# SPDX-License-Identifier: LGPL-3.0-or-later
# Install rules (included from CMakeLists.txt when PXSTEAMDL_INSTALL is ON).
#
# The library statically links curl, mbedTLS, zlib, liblzma and zstd, which are built from source here and are
# not installed themselves. Exporting `pxsteamdl` as it is would leave the installed package pointing at
# targets that do not exist, so instead the library and everything it links statically are merged into one
# archive, and a small hand-written config file describes that archive and the system libraries it needs.

include(CMakePackageConfigHelpers)

# Collects what the targets link, transitively: the static library targets (into <out_targets>) and what is not a
# target, i.e. system libraries such as ws2_32 or "-framework CoreFoundation" (into <out_system_libraries>).
# curl hides its dependencies behind CURL::* targets that are visible only inside its own directory; the libraries
# they stand for are therefore passed as roots, and the CURL::* names are skipped.
function(pxsteamdl_link_closure out_targets out_system_libraries)
    set(queue ${ARGN})
    set(seen "")
    set(static_targets "")
    set(system_libraries "")
    while(queue)
        list(POP_FRONT queue current)
        get_target_property(aliased ${current} ALIASED_TARGET)
        if(aliased)
            set(current ${aliased})
        endif()
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${current})
        get_target_property(type ${current} TYPE)
        if(type STREQUAL "STATIC_LIBRARY")
            list(APPEND static_targets ${current})
        endif()
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(items ${current} ${property})
            if(NOT items)
                continue()
            endif()
            foreach(item IN LISTS items)
                while(item MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):(.*)>$")
                    set(item "${CMAKE_MATCH_2}")
                endwhile()
                if(item STREQUAL "" OR item MATCHES "\\$<" OR item STREQUAL "Threads::Threads")
                    continue()  # empty, a condition that cannot be evaluated here, or handled by find_dependency
                elseif(TARGET ${item})
                    list(APPEND queue ${item})
                elseif(NOT item MATCHES "^CURL::")
                    list(APPEND system_libraries "${item}")
                endif()
            endforeach()
        endforeach()
    endwhile()
    list(REMOVE_DUPLICATES static_targets)
    list(REMOVE_DUPLICATES system_libraries)
    set(${out_targets} "${static_targets}" PARENT_SCOPE)
    set(${out_system_libraries} "${system_libraries}" PARENT_SCOPE)
endfunction()

pxsteamdl_link_closure(bundled_targets PXSTEAMDL_SYSTEM_LIBRARIES pxsteamdl mbedtls mbedx509)
list(REMOVE_ITEM bundled_targets pxsteamdl)
list(PREPEND bundled_targets pxsteamdl)
string(JOIN ";" PXSTEAMDL_INTERFACE_LINK_LIBRARIES Threads::Threads ${PXSTEAMDL_SYSTEM_LIBRARIES})
message(STATUS "PxSteamDL install: merging ${bundled_targets}; system libraries: ${PXSTEAMDL_SYSTEM_LIBRARIES}")

set(PXSTEAMDL_BUNDLE_NAME ${CMAKE_STATIC_LIBRARY_PREFIX}pxsteamdl${CMAKE_STATIC_LIBRARY_SUFFIX})
set(bundle_file ${CMAKE_CURRENT_BINARY_DIR}/bundle/${PXSTEAMDL_BUNDLE_NAME})
set(bundle_inputs "")
foreach(bundled IN LISTS bundled_targets)
    list(APPEND bundle_inputs "$<TARGET_FILE:${bundled}>")
endforeach()
# The inputs travel as one argument; "|" is the separator, since ";" would split it.
string(JOIN "|" bundle_inputs_argument ${bundle_inputs})
if(APPLE)
    # Apple's libtool, not GNU libtool (which would be called glibtool, but may come first in PATH).
    find_program(PXSTEAMDL_LIBTOOL NAMES libtool HINTS /usr/bin REQUIRED)
    set(merge_tool "${PXSTEAMDL_LIBTOOL}")
else()
    set(merge_tool "${CMAKE_AR}")  # ar, or lib.exe with MSVC
endif()
if(NOT merge_tool)
    message(FATAL_ERROR "PxSteamDL install: no tool found to merge static libraries; configure with -DPXSTEAMDL_INSTALL=OFF")
endif()
add_custom_command(
    OUTPUT ${bundle_file}
    COMMAND ${CMAKE_COMMAND}
        -DTOOL=${merge_tool} -DMSVC=${MSVC} -DAPPLE=${APPLE} -DOUTPUT=${bundle_file}
        "-DINPUTS=${bundle_inputs_argument}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergeArchives.cmake
    DEPENDS ${bundled_targets} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergeArchives.cmake
    COMMENT "Merging ${PXSTEAMDL_BUNDLE_NAME} with its dependencies"
    VERBATIM
)
add_custom_target(pxsteamdl_bundle ALL DEPENDS ${bundle_file})

install(FILES ${bundle_file} DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/pxsteamdl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/pxsteamdl/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pxsteamdl)
install(FILES COPYING COPYING.LESSER DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(TARGET pxsteamdl-cli)
    install(TARGETS pxsteamdl-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# 0.x versions may break between minor versions; from 1.0 on, only a new major version may.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(compatibility SameMinorVersion)
else()
    set(compatibility SameMajorVersion)
endif()
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/PxSteamDLConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY ${compatibility})
configure_package_config_file(cmake/PxSteamDLConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/PxSteamDLConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PxSteamDL)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PxSteamDLConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/PxSteamDLConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PxSteamDL)
