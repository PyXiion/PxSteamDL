# SPDX-License-Identifier: LGPL-3.0-or-later
# A program that uses an installed PxSteamDL through find_package; CI builds it against the output of
# `cmake --install` to check that the installed package is complete. Build it with
#   cmake -S tests/install_consumer -B consumer -DCMAKE_PREFIX_PATH=<install prefix>
cmake_minimum_required(VERSION 3.24)
project(PxSteamDLConsumer LANGUAGES CXX)

find_package(PxSteamDL CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE PxSteamDL::pxsteamdl)
