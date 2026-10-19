# SPDX-License-Identifier: LGPL-3.0-or-later
# Benchmark programs (PXSTEAMDL_BUILD_BENCH): the client every version of the library is measured with, and, where
# the library is built on asio, the local test server it downloads from.
add_executable(bench-client client.cpp)
target_link_libraries(bench-client PRIVATE PxSteamDL::pxsteamdl)

if(TARGET asio::asio)
    add_executable(bench-server server.cpp)
    target_include_directories(bench-server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(bench-server PRIVATE PxSteamDL::pxsteamdl asio::asio OpenSSL::SSL OpenSSL::Crypto
                                               MbedTLS::mbedcrypto ZLIB::ZLIB nlohmann_json::nlohmann_json Threads::Threads)
endif()
