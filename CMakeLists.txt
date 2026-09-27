cmake_minimum_required(VERSION 3.24)
project(PxSteamDL VERSION 0.1.0 LANGUAGES CXX)

option(PXSTEAMDL_BUILD_CLI "Build the pxsteamdl command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(PXSTEAMDL_BUILD_PYTHON "Build the Python extension module" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.12.0
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    EXCLUDE_FROM_ALL YES
)

find_package(CURL 8.11 REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
find_package(Threads REQUIRED)

add_library(pxsteamdl
    src/client.cpp
    src/content.cpp
    src/http.cpp
    src/proto.cpp
    src/session.cpp
)
add_library(PxSteamDL::pxsteamdl ALIAS pxsteamdl)
target_include_directories(pxsteamdl PUBLIC include PRIVATE src)
target_link_libraries(pxsteamdl
    PRIVATE CURL::libcurl OpenSSL::Crypto ZLIB::ZLIB LibLZMA::LibLZMA PkgConfig::ZSTD nlohmann_json::nlohmann_json
    PUBLIC Threads::Threads
)
target_compile_options(pxsteamdl PRIVATE -Wall -Wextra)
# Linked into the Python extension module.
set_target_properties(pxsteamdl PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PXSTEAMDL_BUILD_CLI)
    add_executable(pxsteamdl-cli cli/main.cpp)
    set_target_properties(pxsteamdl-cli PROPERTIES OUTPUT_NAME pxsteamdl)
    target_link_libraries(pxsteamdl-cli PRIVATE PxSteamDL::pxsteamdl)
    target_compile_options(pxsteamdl-cli PRIVATE -Wall -Wextra)
endif()

if(PXSTEAMDL_BUILD_PYTHON)
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
    CPMAddPackage(
        NAME nanobind
        GITHUB_REPOSITORY wjakob/nanobind
        GIT_TAG v2.15.0
        EXCLUDE_FROM_ALL YES
    )

    nanobind_add_module(_pxsteamdl NB_STATIC python/pxsteamdl.cpp)
    target_link_libraries(_pxsteamdl PRIVATE PxSteamDL::pxsteamdl)
    target_compile_options(_pxsteamdl PRIVATE -Wall -Wextra)
    nanobind_add_stub(_pxsteamdl_stub
        MODULE _pxsteamdl
        OUTPUT _pxsteamdl.pyi
        PYTHON_PATH $<TARGET_FILE_DIR:_pxsteamdl>
        DEPENDS _pxsteamdl
    )

    # Wheel layout (scikit-build-core): python/pxsteamdl/ is packaged by the backend.
    install(TARGETS _pxsteamdl LIBRARY DESTINATION pxsteamdl)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/_pxsteamdl.pyi DESTINATION pxsteamdl)
endif()
