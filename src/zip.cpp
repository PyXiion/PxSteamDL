// SPDX-License-Identifier: LGPL-3.0-or-later
#include "zip.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>

#include <zlib.h>

namespace pxsteamdl::detail {

namespace {

// End of central directory record.
constexpr std::uint32_t kEndSignature = 0x06054b50;
constexpr std::size_t kEndSize = 22;
constexpr std::size_t kMaxCommentSize = 65535;

constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::size_t kCentralSize = 46;

constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::size_t kLocalSize = 30;

constexpr std::uint16_t kFlagEncrypted = 1;
constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodDeflated = 8;

struct Entry {
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t original_size = 0;
  std::uint32_t local_offset = 0;
};

// Returns the offset of the end of central directory record, whose comment must end the archive.
std::size_t FindEnd(ByteSpan zip) {
  if (zip.size() < kEndSize) Fail("ZIP: missing end of central directory");
  std::size_t first = zip.size() > kEndSize + kMaxCommentSize ? zip.size() - kEndSize - kMaxCommentSize : 0;
  for (std::size_t pos = zip.size() - kEndSize;; --pos) {
    if (ReadLe32(zip, pos) == kEndSignature && pos + kEndSize + ReadLe16(zip, pos + 20) == zip.size()) return pos;
    if (pos == first) break;
  }
  Fail("ZIP: expected exactly one entry on one disk");
}

// Reads the central directory, which must describe exactly one entry.
Entry ReadSingleEntry(ByteSpan zip) {
  std::size_t end = FindEnd(zip);
  bool one_entry_on_one_disk = ReadLe16(zip, end + 4) == 0 && ReadLe16(zip, end + 6) == 0 &&
                               ReadLe16(zip, end + 8) == 1 && ReadLe16(zip, end + 10) == 1;
  if (!one_entry_on_one_disk) Fail("ZIP: expected exactly one entry on one disk");

  ByteSpan central = Slice(zip, ReadLe32(zip, end + 16), ReadLe32(zip, end + 12));
  if (central.size() < kCentralSize || ReadLe32(central, 0) != kCentralSignature) {
    Fail("ZIP: invalid central directory");
  }
  std::uint16_t flags = ReadLe16(central, 8);
  Entry entry;
  entry.method = ReadLe16(central, 10);
  entry.crc = ReadLe32(central, 16);
  entry.compressed_size = ReadLe32(central, 20);
  entry.original_size = ReadLe32(central, 24);
  entry.local_offset = ReadLe32(central, 42);
  std::size_t entry_size = kCentralSize + ReadLe16(central, 28) + ReadLe16(central, 30) + ReadLe16(central, 32);
  // UINT32_MAX sizes mean ZIP64.
  if (entry_size != central.size() || entry.compressed_size == UINT32_MAX || entry.original_size == UINT32_MAX ||
      (flags & kFlagEncrypted) || (entry.method != kMethodStored && entry.method != kMethodDeflated)) {
    Fail("ZIP: unsupported entry");
  }
  return entry;
}

void Inflate(ByteSpan packed, Bytes& result) {
  if (packed.size() > UINT_MAX || result.size() > UINT_MAX) Fail("ZIP: entry too large");
  z_stream stream{};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) Fail("ZIP: inflate init failed");
  stream.next_in = const_cast<Bytef*>(packed.data());
  stream.avail_in = static_cast<uInt>(packed.size());
  std::uint8_t empty = 0;
  stream.next_out = result.empty() ? &empty : result.data();
  stream.avail_out = result.empty() ? 1 : static_cast<uInt>(result.size());
  int status = inflate(&stream, Z_FINISH);
  auto written = stream.total_out;
  auto consumed = stream.total_in;
  inflateEnd(&stream);
  if (status != Z_STREAM_END || written != result.size() || consumed != packed.size()) {
    Fail("ZIP: deflate data or length is invalid");
  }
}

}  // namespace

Bytes UnzipSingleEntry(ByteSpan zip) {
  // Sizes are taken from the central directory: local headers may carry zeros when a data descriptor follows.
  Entry entry = ReadSingleEntry(zip);
  ByteSpan local = Slice(zip, entry.local_offset, kLocalSize);
  if (ReadLe32(local, 0) != kLocalSignature || ReadLe16(local, 8) != entry.method ||
      (ReadLe16(local, 6) & kFlagEncrypted)) {
    Fail("ZIP: invalid local header");
  }
  std::size_t data_offset = std::size_t{entry.local_offset} + kLocalSize + ReadLe16(local, 26) + ReadLe16(local, 28);
  ByteSpan packed = Slice(zip, data_offset, entry.compressed_size);

  Bytes result(entry.original_size);
  if (entry.method == kMethodStored) {
    if (entry.compressed_size != entry.original_size) Fail("ZIP: stored entry size mismatch");
    std::copy(packed.begin(), packed.end(), result.begin());
  } else {
    Inflate(packed, result);
  }
  if (crc32_z(0, result.data(), result.size()) != entry.crc) Fail("ZIP: CRC mismatch");
  return result;
}

}  // namespace pxsteamdl::detail
