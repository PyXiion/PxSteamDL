// SPDX-License-Identifier: LGPL-3.0-or-later
// Anonymous Steam CM session over a WebSocket.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "common.hpp"

namespace pxsteamdl::detail {

// Thread-safe: calls from several threads share one connection.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Logs in anonymously; a no-op while connected. Rpc() and Request() call it to recover from a dropped
    // connection, so a long-lived session survives Steam closing the socket between downloads.
    void Connect();

    // Calls a unified service method (e.g. "ContentServerDirectory.GetManifestRequestCode#1") and returns the
    // response body; throws unless Steam reports success.
    Bytes Rpc(std::string_view method, ByteSpan body);

    // Sends a message of the given EMsg as a job and returns the body of the reply.
    Bytes Request(std::uint32_t emsg, ByteSpan body);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pxsteamdl::detail
