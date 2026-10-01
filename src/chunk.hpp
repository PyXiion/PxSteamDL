// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "common.hpp"
#include "crypto.hpp"
#include "manifest.hpp"

namespace pxsteamdl::detail {

// Decrypts and decompresses a chunk as served by the CDN (VZstd, VZip/LZMA or ZIP). Throws unless the result has the
// size and Adler-32 the manifest lists.
Bytes DecodeChunk(ByteSpan encrypted, const AesKey& key, const ManifestChunk& chunk);

}  // namespace pxsteamdl::detail
