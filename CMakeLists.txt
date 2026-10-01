cmake_minimum_required(VERSION 3.24)
project(PxSteamDL VERSION 0.2.0 LANGUAGES CXX)

option(PXSTEAMDL_BUILD_CLI "Build the pxsteamdl command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(PXSTEAMDL_BUILD_PYTHON "Build the Python extension module" OFF)
option(PXSTEAMDL_BUILD_TESTS "Build the unit tests (GoogleTest)" OFF)
option(PXSTEAMDL_INSTALL "Generate install rules: headers, one library with its dependencies merged in, find_package files" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# <pxsteamdl/version.hpp>, from the version above; the single source of the version (the Python package reads it too).
configure_file(cmake/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/pxsteamdl/version.hpp @ONLY)

# All dependencies are built from source as static libraries (the Python module links them too, hence PIC).
# CPM's find_package redirection is off: curl locates mbedTLS/zstd/zlib through the variables set below.
set(CPM_DONT_UPDATE_MODULE_PATH ON)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.12.0
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
)

CPMAddPackage(
    NAME zlib
    VERSION 1.3.2
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz
    URL_HASH SHA256=d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS "ZLIB_BUILD_SHARED OFF" "ZLIB_BUILD_TESTING OFF" "ZLIB_INSTALL OFF"
)
add_library(ZLIB::ZLIB ALIAS zlibstatic)
# Satisfies CMake's FindZLIB (used by curl) without searching the system.
set(ZLIB_INCLUDE_DIR ${zlib_SOURCE_DIR})
set(ZLIB_LIBRARY ZLIB::ZLIB)

CPMAddPackage(
    NAME xz
    VERSION 5.8.4
    URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.xz
    URL_HASH SHA256=4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS
        "XZ_NLS OFF"
        "XZ_THREADS no"
        "XZ_ENCODERS "
        "XZ_DECODERS lzma1"
        "XZ_MATCH_FINDERS "
        "XZ_CHECKS crc32"
        "XZ_LZIP_DECODER OFF"
        "XZ_MICROLZMA_ENCODER OFF"
        "XZ_MICROLZMA_DECODER OFF"
        "XZ_DOC OFF"
        "BUILD_TESTING OFF"
)
# xz exports its public headers only through the installed package config.
target_include_directories(liblzma SYSTEM INTERFACE $<BUILD_INTERFACE:${xz_SOURCE_DIR}/src/liblzma/api>)

CPMAddPackage(
    NAME zstd
    VERSION 1.5.7
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR build/cmake
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS
        "ZSTD_BUILD_PROGRAMS OFF"
        "ZSTD_BUILD_TESTS OFF"
        "ZSTD_BUILD_SHARED OFF"
        "ZSTD_BUILD_STATIC ON"
        "ZSTD_BUILD_COMPRESSION OFF"
        "ZSTD_BUILD_DICTBUILDER OFF"
        "ZSTD_LEGACY_SUPPORT OFF"
        "ZSTD_MULTITHREAD_SUPPORT OFF"
)
set(ZSTD_INCLUDE_DIR ${zstd_SOURCE_DIR}/lib)
set(ZSTD_LIBRARY libzstd_static)

if(WIN32)
    set(pxsteamdl_mbedtls_pthread OFF)
else()
    set(pxsteamdl_mbedtls_pthread ON)
endif()
CPMAddPackage(
    NAME mbedtls
    VERSION 3.6.7
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS
        "ENABLE_PROGRAMS OFF"
        "ENABLE_TESTING OFF"
        "USE_SHARED_MBEDTLS_LIBRARY OFF"
        "USE_STATIC_MBEDTLS_LIBRARY ON"
        "MBEDTLS_FATAL_WARNINGS OFF"
        "DISABLE_PACKAGE_CONFIG_AND_INSTALL ON"
        "LINK_WITH_PTHREAD ${pxsteamdl_mbedtls_pthread}"
        "MBEDTLS_USER_CONFIG_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls/pxsteamdl_mbedtls_config.h"
)
if(WIN32)
    # threading_alt.h, included by every mbedTLS consumer (mbedtls/threading.h).
    target_include_directories(mbedcrypto SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls)
endif()
# Picked up by curl's FindMbedTLS instead of pkg-config/system search.
set(MBEDTLS_INCLUDE_DIR ${mbedtls_SOURCE_DIR}/include)
set(MBEDTLS_LIBRARY mbedtls)
set(MBEDX509_LIBRARY mbedx509)
set(MBEDCRYPTO_LIBRARY mbedcrypto)
# curl would try_compile against the not-yet-built mbedcrypto; the probe only gates NTLM, which is disabled.
set(HAVE_MBEDTLS_DES_CRYPT_ECB 0)
# No build-host CA paths: the bundle below is the only trust store. These are cache entries, not CPM OPTIONS,
# because curl clears the "none" cache value and OPTIONS would leave a literal "none" behind.
set(CURL_CA_BUNDLE none CACHE STRING "")
set(CURL_CA_PATH none CACHE STRING "")

CPMAddPackage(
    NAME curl
    VERSION 8.22.0
    URL https://github.com/curl/curl/releases/download/curl-8_22_0/curl-8.22.0.tar.xz
    URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS
        "BUILD_CURL_EXE OFF"
        "BUILD_STATIC_LIBS ON"
        "BUILD_LIBCURL_DOCS OFF"
        "BUILD_MISC_DOCS OFF"
        "ENABLE_CURL_MANUAL OFF"
        "BUILD_TESTING OFF"
        "BUILD_EXAMPLES OFF"
        "CURL_DISABLE_INSTALL ON"
        "CURL_ENABLE_EXPORT_TARGET OFF"
        "PICKY_COMPILER OFF"
        "CURL_USE_PKGCONFIG OFF"
        "CURL_USE_CMAKECONFIG OFF"
        "CURL_USE_MBEDTLS ON"
        "CURL_USE_OPENSSL OFF"
        "CURL_ZLIB ON"
        "CURL_ZSTD ON"
        "CURL_BROTLI OFF"
        "USE_NGHTTP2 OFF"
        "USE_LIBIDN2 OFF"
        "USE_WIN32_IDN OFF"
        "USE_APPLE_IDN OFF"
        "CURL_USE_LIBPSL OFF"
        "CURL_USE_LIBSSH2 OFF"
        "CURL_USE_LIBSSH OFF"
        "CURL_USE_GSSAPI OFF"
        "ENABLE_UNIX_SOCKETS OFF"
        "CURL_DISABLE_DICT ON"
        "CURL_DISABLE_FILE ON"
        "CURL_DISABLE_FTP ON"
        "CURL_DISABLE_GOPHER ON"
        "CURL_DISABLE_IMAP ON"
        "CURL_DISABLE_IPFS ON"
        "CURL_DISABLE_LDAP ON"
        "CURL_DISABLE_LDAPS ON"
        "CURL_DISABLE_MQTT ON"
        "CURL_DISABLE_POP3 ON"
        "CURL_DISABLE_RTSP ON"
        "CURL_DISABLE_SMTP ON"
        "CURL_DISABLE_TELNET ON"
        "CURL_DISABLE_TFTP ON"
        "CURL_DISABLE_COOKIES ON"
        "CURL_DISABLE_MIME ON"
        "CURL_DISABLE_NETRC ON"
        "CURL_DISABLE_WEBSOCKETS OFF"
)

# Mozilla CA bundle (as extracted by curl), compiled in: mbedTLS cannot use the OS trust store.
set(PXSTEAMDL_CA_BUNDLE_DATE 2026-09-25)
CPMAddPackage(
    NAME cacert
    VERSION ${PXSTEAMDL_CA_BUNDLE_DATE}
    URL https://curl.se/ca/cacert-${PXSTEAMDL_CA_BUNDLE_DATE}.pem
    URL_HASH SHA256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505
    DOWNLOAD_ONLY YES
    DOWNLOAD_NO_EXTRACT YES
)
set(ca_bundle_pem ${cacert_SOURCE_DIR}/cacert-${PXSTEAMDL_CA_BUNDLE_DATE}.pem)
set(ca_bundle_cpp ${CMAKE_CURRENT_BINARY_DIR}/ca_bundle.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ca_bundle_pem})
file(READ ${ca_bundle_pem} ca_bundle_hex HEX)
string(REGEX REPLACE "(................................................................)" "\\1\n" ca_bundle_hex "${ca_bundle_hex}")
# A byte array, not a string literal: MSVC caps string literals at 64 KiB. The trailing 0 lets mbedTLS parse it in place.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ca_bundle_bytes "${ca_bundle_hex}")
file(CONFIGURE OUTPUT ${ca_bundle_cpp} @ONLY CONTENT [[
// Generated from cacert-@PXSTEAMDL_CA_BUNDLE_DATE@.pem by CMakeLists.txt.
#include <cstddef>
namespace pxsteamdl::detail {
extern const unsigned char kCaBundle[] = {
@ca_bundle_bytes@0x00};
extern const std::size_t kCaBundleSize = sizeof(kCaBundle);
}
]])

find_package(Threads REQUIRED)

function(pxsteamdl_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Everything but the three files that talk to the outside world (HTTP, the CM session, pauses). The unit tests build
# these sources against fakes of those three instead (tests/fakes), so the whole download pipeline runs offline.
set(PXSTEAMDL_CORE_SOURCES
    src/cdn.cpp
    src/chunk.cpp
    src/client.cpp
    src/cm_packet.cpp
    src/crypto.cpp
    src/downloader.cpp
    src/eresult.cpp
    src/manifest.cpp
    src/paths.cpp
    src/pending_file.cpp
    src/proto.cpp
    src/retry.cpp
    src/version.cpp
    src/workshop.cpp
    src/zip.cpp
)
add_library(pxsteamdl
    ${PXSTEAMDL_CORE_SOURCES}
    src/http.cpp
    src/session.cpp
    src/sleep.cpp
    ${ca_bundle_cpp}
)
add_library(PxSteamDL::pxsteamdl ALIAS pxsteamdl)
target_include_directories(pxsteamdl
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE src
)
target_link_libraries(pxsteamdl
    PRIVATE CURL::libcurl MbedTLS::mbedcrypto ZLIB::ZLIB liblzma libzstd_static nlohmann_json::nlohmann_json
    PUBLIC Threads::Threads
)
if(WIN32)
    target_compile_definitions(pxsteamdl PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(pxsteamdl PRIVATE ws2_32)
endif()
pxsteamdl_warnings(pxsteamdl)

if(PXSTEAMDL_BUILD_CLI)
    add_executable(pxsteamdl-cli cli/main.cpp)
    set_target_properties(pxsteamdl-cli PROPERTIES OUTPUT_NAME pxsteamdl)
    target_link_libraries(pxsteamdl-cli PRIVATE PxSteamDL::pxsteamdl)
    if(WIN32)
        target_sources(pxsteamdl-cli PRIVATE cli/utf8.manifest)
    endif()
    pxsteamdl_warnings(pxsteamdl-cli)
endif()

if(PXSTEAMDL_BUILD_PYTHON)
    # SKBUILD_SABI_COMPONENT (set by scikit-build-core for abi3 builds) lets nanobind target the stable ABI.
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT})
    CPMAddPackage(
        NAME nanobind
        GITHUB_REPOSITORY wjakob/nanobind
        GIT_TAG v2.15.0
        EXCLUDE_FROM_ALL YES
        SYSTEM YES
    )

    # STABLE_ABI takes effect on Python >= 3.12 (abi3 wheel); older versions get a version-specific module.
    nanobind_add_module(_pxsteamdl NB_STATIC STABLE_ABI python/pxsteamdl.cpp)
    target_link_libraries(_pxsteamdl PRIVATE PxSteamDL::pxsteamdl)
    pxsteamdl_warnings(_pxsteamdl)
    nanobind_add_stub(_pxsteamdl_stub
        MODULE _pxsteamdl
        OUTPUT _pxsteamdl.pyi
        PYTHON_PATH $<TARGET_FILE_DIR:_pxsteamdl>
        DEPENDS _pxsteamdl
    )

    # Wheel layout (scikit-build-core): python/pxsteamdl/ is packaged by the backend. The component keeps
    # dependency install rules out of the wheel.
    install(TARGETS _pxsteamdl LIBRARY DESTINATION pxsteamdl COMPONENT python)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/_pxsteamdl.pyi DESTINATION pxsteamdl COMPONENT python)
endif()

if(PXSTEAMDL_BUILD_TESTS)
    CPMAddPackage(
        NAME googletest
        VERSION 1.17.0
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        EXCLUDE_FROM_ALL YES
        SYSTEM YES
        OPTIONS "BUILD_GMOCK OFF" "INSTALL_GTEST OFF" "gtest_force_shared_crt ON"
    )
    enable_testing()
    include(GoogleTest)

    # Unit tests of the internal modules (src/*.hpp). They build the core sources again, with fakes in place of
    # src/http.cpp, src/session.cpp and src/sleep.cpp, so they need no network and no waiting.
    add_executable(pxsteamdl-tests
        ${PXSTEAMDL_CORE_SOURCES}
        tests/fakes/fake_http.cpp
        tests/fakes/fake_session.cpp
        tests/fakes/fake_sleep.cpp
        tests/fake_steam.cpp
        tests/chunk_test.cpp
        tests/client_test.cpp
        tests/cm_packet_test.cpp
        tests/crypto_test.cpp
        tests/eresult_test.cpp
        tests/manifest_test.cpp
        tests/paths_test.cpp
        tests/proto_test.cpp
        tests/retry_test.cpp
        tests/version_test.cpp
        tests/zip_test.cpp
    )
    target_include_directories(pxsteamdl-tests PRIVATE src tests tests/fakes include ${CMAKE_CURRENT_BINARY_DIR}/include)
    target_link_libraries(pxsteamdl-tests
        PRIVATE GTest::gtest_main ZLIB::ZLIB MbedTLS::mbedcrypto liblzma libzstd_static nlohmann_json::nlohmann_json
                Threads::Threads
    )
    if(WIN32)
        target_compile_definitions(pxsteamdl-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    pxsteamdl_warnings(pxsteamdl-tests)
    gtest_discover_tests(pxsteamdl-tests)

    # The real SleepFor(), which the fake above replaces.
    add_executable(pxsteamdl-sleep-tests src/sleep.cpp tests/sleep_test.cpp)
    target_include_directories(pxsteamdl-sleep-tests PRIVATE src)
    target_link_libraries(pxsteamdl-sleep-tests PRIVATE GTest::gtest_main Threads::Threads)
    pxsteamdl_warnings(pxsteamdl-sleep-tests)
    gtest_discover_tests(pxsteamdl-sleep-tests)
endif()

if(PXSTEAMDL_INSTALL)
    include(cmake/PxSteamDLInstall.cmake)
endif()
