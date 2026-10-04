// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pxsteamdl/error.hpp"

namespace pxsteamdl {

std::string_view ErrorKindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kNone:
      return "none";
    case ErrorKind::kCancelled:
      return "cancelled";
    case ErrorKind::kNotFound:
      return "not found";
    case ErrorKind::kRejected:
      return "rejected";
    case ErrorKind::kNetwork:
      return "network";
    case ErrorKind::kData:
      return "data";
    case ErrorKind::kFilesystem:
      return "filesystem";
    case ErrorKind::kOther:
      return "other";
  }
  return "other";
}

}  // namespace pxsteamdl
