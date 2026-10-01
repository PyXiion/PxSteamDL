// SPDX-License-Identifier: LGPL-3.0-or-later
// A stand-in for src/session.cpp: the unit tests link this file instead, so nothing connects to Steam.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "session.hpp"

namespace pxsteamdl::detail::testing {

struct SessionHandlers {
  // Answer Session::request() and Session::rpc(), or throw to fail the call.
  std::function<Bytes(std::uint32_t emsg, ByteSpan body)> request;
  std::function<Bytes(std::string_view method, ByteSpan body)> rpc;
};

// Installs the handlers the fake Session calls, possibly from several threads at once. Without them, calls throw.
void SetSessionHandlers(SessionHandlers handlers);
void ClearSessionHandlers();

// How many times Session::connect() was called since the last ClearSessionHandlers().
int ConnectCount();

}  // namespace pxsteamdl::detail::testing
