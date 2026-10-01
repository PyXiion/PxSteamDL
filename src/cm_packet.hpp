// SPDX-License-Identifier: LGPL-3.0-or-later
// Framing of Steam CM (connection manager) messages with a protobuf header.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"

namespace pxsteamdl::detail {

// Steam CM message types (SteamKit2 EMsg).
namespace emsg {
inline constexpr std::uint32_t kMulti = 1;
inline constexpr std::uint32_t kServiceMethodCall = 151;
inline constexpr std::uint32_t kClientHeartbeat = 703;
inline constexpr std::uint32_t kClientLogOff = 706;
inline constexpr std::uint32_t kClientLogonResponse = 751;
inline constexpr std::uint32_t kClientLoggedOff = 757;
inline constexpr std::uint32_t kClientGetDepotDecryptionKey = 5438;
inline constexpr std::uint32_t kClientLogon = 5514;
}  // namespace emsg

// Job ID meaning "none": the packet neither starts nor answers a job.
inline constexpr std::uint64_t kNoJob = ~0ULL;

// One CM packet: the CMsgProtoBufHeader fields used here and the message body.
struct Packet {
    std::uint32_t emsg = 0;
    std::uint64_t steamid = 0;
    std::int32_t sessionid = 0;
    std::uint64_t jobid_source = kNoJob;
    std::uint64_t jobid_target = kNoJob;
    // Received only.
    std::int32_t eresult = 0;
    std::string target_job_name;
    // Received only.
    std::string error_message;
    Bytes body;
};

// Encodes an outgoing packet: its EMsg, the header fields that are set, and the body.
Bytes SerializePacket(const Packet& packet);

// Decodes one packet; throws on a truncated or malformed header.
Packet ParsePacket(ByteSpan data);

// Decodes one CM message, expanding a Multi into the packets it carries.
std::vector<Packet> ParseMessage(ByteSpan message);

}  // namespace pxsteamdl::detail
