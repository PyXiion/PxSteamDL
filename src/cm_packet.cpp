// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cm_packet.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <zlib.h>

#include "proto.hpp"

namespace pxsteamdl::detail {

namespace {

// Set on the EMsg of packets with a protobuf header.
constexpr std::uint32_t kProtoFlag = 0x80000000u;
// EMsg (4 bytes) and header length (4 bytes) precede the header.
constexpr std::size_t kPrefixSize = 8;

// CMsgProtoBufHeader field numbers.
namespace header_field {
constexpr std::uint32_t kSteamId = 1;
constexpr std::uint32_t kSessionId = 2;
constexpr std::uint32_t kJobIdSource = 10;
constexpr std::uint32_t kJobIdTarget = 11;
constexpr std::uint32_t kTargetJobName = 12;
constexpr std::uint32_t kEResult = 13;
constexpr std::uint32_t kErrorMessage = 14;
}  // namespace header_field

// CMsgMulti field numbers.
namespace multi_field {
constexpr std::uint32_t kSizeUnzipped = 1;
constexpr std::uint32_t kMessageBody = 2;
}  // namespace multi_field

Bytes DecompressGzip(ByteSpan data, std::size_t uncompressed_hint) {
  // The hint comes from the network: cap the initial allocation, the buffer grows as needed.
  constexpr std::size_t kMinInitialSize = 1024;
  constexpr std::size_t kMaxInitialSize = 16 << 20;
  constexpr int kGzipWindowBits = 16 + MAX_WBITS;
  Bytes out(std::clamp<std::size_t>(uncompressed_hint ? uncompressed_hint : data.size() * 4, kMinInitialSize,
                                    kMaxInitialSize));
  z_stream stream{};
  if (inflateInit2(&stream, kGzipWindowBits) != Z_OK) Fail("inflateInit2 failed");

  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());
  while (true) {
    stream.next_out = reinterpret_cast<Bytef*>(out.data() + stream.total_out);
    stream.avail_out = static_cast<uInt>(out.size() - stream.total_out);
    int status = inflate(&stream, Z_NO_FLUSH);
    if (status == Z_STREAM_END) break;
    if (status != Z_OK) {
      inflateEnd(&stream);
      Fail("gzip decompression failed (" + std::to_string(status) + ")");
    }
    if (stream.avail_out == 0) out.resize(out.size() * 2);
  }
  out.resize(stream.total_out);
  inflateEnd(&stream);
  return out;
}

}  // namespace

Bytes SerializePacket(const Packet& packet) {
  Bytes header;
  auto append = [&](const Bytes& field) { header.insert(header.end(), field.begin(), field.end()); };
  if (packet.steamid) append(EncodeFixed64(header_field::kSteamId, packet.steamid));
  if (packet.sessionid) append(EncodeUint(header_field::kSessionId, static_cast<std::uint32_t>(packet.sessionid)));
  if (packet.jobid_source != kNoJob) append(EncodeFixed64(header_field::kJobIdSource, packet.jobid_source));
  if (packet.jobid_target != kNoJob) append(EncodeFixed64(header_field::kJobIdTarget, packet.jobid_target));
  if (!packet.target_job_name.empty()) append(EncodeString(header_field::kTargetJobName, packet.target_job_name));

  Bytes out;
  out.reserve(kPrefixSize + header.size() + packet.body.size());
  AppendLe32(out, packet.emsg | kProtoFlag);
  AppendLe32(out, static_cast<std::uint32_t>(header.size()));
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), packet.body.begin(), packet.body.end());
  return out;
}

Packet ParsePacket(ByteSpan data) {
  if (data.size() < kPrefixSize) Fail("packet too short for header");
  std::uint32_t raw_emsg = ReadLe32(data, 0);
  std::uint32_t header_size = ReadLe32(data, 4);
  if (data.size() - kPrefixSize < header_size) Fail("packet header truncated");

  Packet packet;
  packet.emsg = raw_emsg & ~kProtoFlag;
  ProtoReader reader(data.subspan(kPrefixSize, header_size));
  while (auto field = reader.next()) {
    if (field->is(header_field::kSteamId, WireType::kFixed64)) {
      packet.steamid = field->integer;
    } else if (field->is(header_field::kSessionId, WireType::kVarint)) {
      packet.sessionid = static_cast<std::int32_t>(field->integer);
    } else if (field->is(header_field::kJobIdSource, WireType::kFixed64)) {
      packet.jobid_source = field->integer;
    } else if (field->is(header_field::kJobIdTarget, WireType::kFixed64)) {
      packet.jobid_target = field->integer;
    } else if (field->is(header_field::kTargetJobName, WireType::kLengthDelimited)) {
      packet.target_job_name = AsString(field->bytes);
    } else if (field->is(header_field::kEResult, WireType::kVarint)) {
      packet.eresult = static_cast<std::int32_t>(field->integer);
    } else if (field->is(header_field::kErrorMessage, WireType::kLengthDelimited)) {
      packet.error_message = AsString(field->bytes);
    }
  }
  ByteSpan body = data.subspan(kPrefixSize + header_size);
  packet.body.assign(body.begin(), body.end());
  return packet;
}

std::vector<Packet> ParseMessage(ByteSpan message) {
  Packet packet = ParsePacket(message);
  std::vector<Packet> packets;
  if (packet.emsg != emsg::kMulti) {
    packets.push_back(std::move(packet));
    return packets;
  }

  std::uint64_t unzipped_size = 0;
  ByteSpan payload;
  ProtoReader reader(packet.body);
  while (auto field = reader.next()) {
    if (field->is(multi_field::kSizeUnzipped, WireType::kVarint)) {
      unzipped_size = field->integer;
    } else if (field->is(multi_field::kMessageBody, WireType::kLengthDelimited)) {
      payload = field->bytes;
    }
  }
  Bytes unzipped;
  if (unzipped_size > 0) {
    unzipped = DecompressGzip(payload, unzipped_size);
    payload = unzipped;
  }

  // The payload is a sequence of length-prefixed packets; a truncated tail is ignored.
  std::size_t offset = 0;
  while (payload.size() - offset >= 4) {
    std::uint32_t size = ReadLe32(payload, offset);
    offset += 4;
    if (size > payload.size() - offset) break;
    packets.push_back(ParsePacket(payload.subspan(offset, size)));
    offset += size;
  }
  return packets;
}

}  // namespace pxsteamdl::detail
