// SPDX-License-Identifier: LGPL-3.0-or-later
// Steam's EResult codes and their descriptions.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pxsteamdl::detail {

// The EResult codes (SteamKit2 EResult) the library can meet: Web API item results, logon, depot keys and RPCs.
enum class EResult : std::int32_t {
  kInvalid = 0,
  kOk = 1,
  kFail = 2,
  kNoConnection = 3,
  kInvalidPassword = 5,
  kLoggedInElsewhere = 6,
  kInvalidProtocolVersion = 7,
  kInvalidParam = 8,
  kFileNotFound = 9,
  kBusy = 10,
  kInvalidState = 11,
  kInvalidName = 12,
  kAccessDenied = 15,
  kTimeout = 16,
  kBanned = 17,
  kAccountNotFound = 18,
  kInvalidSteamId = 19,
  kServiceUnavailable = 20,
  kNotLoggedOn = 21,
  kPending = 22,
  kEncryptionFailure = 23,
  kInsufficientPrivilege = 24,
  kLimitExceeded = 25,
  kRevoked = 26,
  kExpired = 27,
  kDuplicateRequest = 29,
  kIpNotFound = 31,
  kPersistFailed = 32,
  kLockingFailed = 33,
  kLogonSessionReplaced = 34,
  kConnectFailed = 35,
  kHandshakeFailed = 36,
  kIoFailure = 37,
  kRemoteDisconnect = 38,
  kBlocked = 40,
  kIgnored = 41,
  kNoMatch = 42,
  kAccountDisabled = 43,
  kServiceReadOnly = 44,
  kContentVersion = 47,
  kTryAnotherCm = 48,
  kSuspended = 51,
  kCancelled = 52,
  kDataCorruption = 53,
  kDiskFull = 54,
  kRemoteCallFailed = 55,
  kExternalAccountUnlinked = 58,
  kIpBanned = 61,
  kUnexpectedError = 79,
  kDisabled = 80,
  kRateLimitExceeded = 84,
  kAccountLockedDown = 87,
  kNotModified = 95,
  kTooManyPending = 103,
  kNoSiteLicensesFound = 104,
  kWgNetworkSendExceeded = 105,
};

inline constexpr std::int64_t kEResultOk = static_cast<std::int64_t>(EResult::kOk);

// A short lowercase description of the code, e.g. "not found" for 9; "unknown result" for codes not listed.
std::string_view EResultDescription(std::int64_t code);

// The description with the code, e.g. "not found (EResult 9)".
std::string DescribeEResult(std::int64_t code);

// Whether Steam may answer differently a little later: it is busy, rate-limiting us, or the connection broke.
bool IsTransientEResult(std::int64_t code);

// Throws "<what>: <description> (EResult N)", with detail in parentheses if it is not empty: a TransientError
// (kNetwork) if the code is transient, otherwise an Error of kind kNotFound for 9 and kRejected for the rest.
[[noreturn]] void FailEResult(const std::string& what, std::int64_t code, std::string_view detail = {});

}  // namespace pxsteamdl::detail
