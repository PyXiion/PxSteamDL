// SPDX-License-Identifier: LGPL-3.0-or-later
#include "eresult.hpp"

#include <cstdint>

namespace pxsteamdl::detail {

std::string_view EResultDescription(std::int64_t code) {
  if (code < INT32_MIN || code > INT32_MAX) return "unknown result";
  switch (static_cast<EResult>(code)) {
    case EResult::kInvalid:
      return "no result";
    case EResult::kOk:
      return "success";
    case EResult::kFail:
      return "generic failure";
    case EResult::kNoConnection:
      return "no connection to Steam";
    case EResult::kInvalidPassword:
      return "invalid password";
    case EResult::kLoggedInElsewhere:
      return "logged in elsewhere";
    case EResult::kInvalidProtocolVersion:
      return "unsupported protocol version";
    case EResult::kInvalidParam:
      return "invalid parameter";
    case EResult::kFileNotFound:
      return "not found";
    case EResult::kBusy:
      return "Steam is busy";
    case EResult::kInvalidState:
      return "invalid state";
    case EResult::kInvalidName:
      return "invalid name";
    case EResult::kAccessDenied:
      return "access denied";
    case EResult::kTimeout:
      return "timed out";
    case EResult::kBanned:
      return "banned";
    case EResult::kAccountNotFound:
      return "account not found";
    case EResult::kInvalidSteamId:
      return "invalid Steam ID";
    case EResult::kServiceUnavailable:
      return "service unavailable";
    case EResult::kNotLoggedOn:
      return "not logged on";
    case EResult::kPending:
      return "request pending";
    case EResult::kEncryptionFailure:
      return "encryption failure";
    case EResult::kInsufficientPrivilege:
      return "insufficient privilege";
    case EResult::kLimitExceeded:
      return "limit exceeded";
    case EResult::kRevoked:
      return "access revoked";
    case EResult::kExpired:
      return "expired";
    case EResult::kDuplicateRequest:
      return "duplicate request";
    case EResult::kIpNotFound:
      return "IP address not found";
    case EResult::kPersistFailed:
      return "Steam failed to store the change";
    case EResult::kLockingFailed:
      return "Steam failed to acquire a lock";
    case EResult::kLogonSessionReplaced:
      return "logon session replaced";
    case EResult::kConnectFailed:
      return "connection failed";
    case EResult::kHandshakeFailed:
      return "handshake failed";
    case EResult::kIoFailure:
      return "I/O failure";
    case EResult::kRemoteDisconnect:
      return "remote disconnect";
    case EResult::kBlocked:
      return "blocked";
    case EResult::kIgnored:
      return "ignored";
    case EResult::kNoMatch:
      return "no match";
    case EResult::kAccountDisabled:
      return "account disabled";
    case EResult::kServiceReadOnly:
      return "service is read-only";
    case EResult::kContentVersion:
      return "content version mismatch";
    case EResult::kTryAnotherCm:
      return "try another connection manager";
    case EResult::kSuspended:
      return "suspended";
    case EResult::kCancelled:
      return "cancelled by Steam";
    case EResult::kDataCorruption:
      return "data corruption";
    case EResult::kDiskFull:
      return "disk full";
    case EResult::kRemoteCallFailed:
      return "remote call failed";
    case EResult::kExternalAccountUnlinked:
      return "external account unlinked";
    case EResult::kIpBanned:
      return "IP address banned";
    case EResult::kUnexpectedError:
      return "unexpected error";
    case EResult::kDisabled:
      return "disabled";
    case EResult::kRateLimitExceeded:
      return "rate limit exceeded";
    case EResult::kAccountLockedDown:
      return "account locked down";
    case EResult::kNotModified:
      return "not modified";
    case EResult::kTooManyPending:
      return "too many pending requests";
    case EResult::kNoSiteLicensesFound:
      return "no site licenses found";
    case EResult::kWgNetworkSendExceeded:
      return "network send limit exceeded";
  }
  return "unknown result";
}

std::string DescribeEResult(std::int64_t code) {
  return std::string(EResultDescription(code)) + " (EResult " + std::to_string(code) + ")";
}

}  // namespace pxsteamdl::detail
