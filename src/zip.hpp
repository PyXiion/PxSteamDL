// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "common.hpp"

namespace pxsteamdl::detail {

// Extracts the only entry of a ZIP archive (stored or deflated), as Steam uses for manifests and some chunks.
// Throws unless the archive has exactly one valid, unencrypted entry whose CRC-32 matches.
Bytes UnzipSingleEntry(ByteSpan zip);

}  // namespace pxsteamdl::detail
