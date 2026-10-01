// SPDX-License-Identifier: LGPL-3.0-or-later
// Workshop item details from the Steam Web API.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

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
};

// Looks up the items in batches; returns one Item per ID, in order. A rejected item carries Item::error, a failed
// request throws. on_item, if set, is called on the calling thread for each item as soon as its batch is resolved.
std::vector<Item> FetchItems(std::span<const std::uint64_t> ids, const std::function<void(const Item&)>& on_item = {});

}  // namespace pxsteamdl::detail
