cmake_minimum_required(VERSION 3.24)
project(PxSteamDL LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.12.0
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
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

add_executable(pxsteamdl-cli cli/main.cpp)
set_target_properties(pxsteamdl-cli PROPERTIES OUTPUT_NAME pxsteamdl)
target_link_libraries(pxsteamdl-cli PRIVATE PxSteamDL::pxsteamdl)
target_compile_options(pxsteamdl-cli PRIVATE -Wall -Wextra)
