// SPDX-License-Identifier: LGPL-3.0-or-later
// Workshop item details from the Steam Web API.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>

#include "http.hpp"
#include "pxsteamdl/error.hpp"

namespace pxsteamdl::detail {

struct Item {
  std::uint64_t id = 0;
  // SteamPipe manifest of the item's content; 0 for a legacy item.
  std::uint64_t manifest_id = 0;
  std::uint32_t app_id = 0;
  std::string title;
  // Direct download of a legacy (pre-SteamPipe) item.
  std::string file_url;
  std::string filename;
  // Why Steam rejected the item; empty if it was accepted.
  std::string error;
  ErrorKind error_kind = ErrorKind::kNone;
};

// Looks up the items in batches of up to 100 and calls on_item on the calling thread for each item, in order, as soon
// as its batch is answered. A rejected item carries Item::error, as does every item of a batch whose request failed.
// Once stop is requested, the remaining items are not looked up and carry kCancelled.
void FetchItems(std::span<const std::uint64_t> ids, const HttpConfig& config, const std::stop_token& stop,
                const std::function<void(Item)>& on_item);

}  // namespace pxsteamdl::detail
