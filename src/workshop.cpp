// SPDX-License-Identifier: LGPL-3.0-or-later
#include "workshop.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "eresult.hpp"
#include "http.hpp"
#include "retry.hpp"

namespace pxsteamdl::detail {

namespace {

constexpr std::string_view kDetailsUrl = "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
// Items per GetPublishedFileDetails request.
constexpr std::size_t kBatchSize = 100;

// The Web API returns 64-bit numbers as strings.
std::uint64_t ParseU64(const nlohmann::json& value) {
  if (value.is_number()) return value.get<std::uint64_t>();
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    return text.empty() ? 0 : std::stoull(text);
  }
  return 0;
}

std::string StringField(const nlohmann::json& entry, const char* key) {
  auto it = entry.find(key);
  return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::uint64_t U64Field(const nlohmann::json& entry, const char* key) {
  auto it = entry.find(key);
  return it != entry.end() ? ParseU64(*it) : 0;
}

Item FailedItem(std::uint64_t id, ErrorKind kind, std::string error) {
  Item item;
  item.id = id;
  item.error = std::move(error);
  item.error_kind = kind;
  return item;
}

// One publishedfiledetails entry; a malformed entry fails only its own item.
Item ParseItem(std::uint64_t id, const nlohmann::json& entry) {
  Item item;
  item.id = id;
  try {
    if (auto result = static_cast<std::int64_t>(U64Field(entry, "result")); result != kEResultOk) {
      item.error = "Steam rejected the item: " + DescribeEResult(result);
      item.error_kind =
          result == static_cast<std::int64_t>(EResult::kFileNotFound) ? ErrorKind::kNotFound : ErrorKind::kRejected;
      return item;
    }
    item.manifest_id = U64Field(entry, "hcontent_file");
    item.app_id = static_cast<std::uint32_t>(U64Field(entry, "consumer_app_id"));
    item.title = StringField(entry, "title");
    item.file_url = StringField(entry, "file_url");
    item.filename = StringField(entry, "filename");
  } catch (const std::exception& e) {
    item.error = std::string("invalid item details: ") + e.what();
    item.error_kind = ErrorKind::kData;
  }
  return item;
}

std::string DetailsForm(std::span<const std::uint64_t> ids) {
  std::string form = "itemcount=" + std::to_string(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    form += "&publishedfileids%5B" + std::to_string(i) + "%5D=" + std::to_string(ids[i]);
  }
  return form;
}

// Fetches the details of up to kBatchSize items; returns one Item per ID, in order. Throws if the request fails,
// after retrying it if the failure looks transient.
std::vector<Item> FetchBatch(std::span<const std::uint64_t> ids, const HttpConfig& config,
                             const std::stop_token& stop) {
  std::string form = DetailsForm(ids);
  HttpResponse response = Retry(stop, [&] {
    HttpResponse reply = HttpRequest(kDetailsUrl, config, "POST", form, "application/x-www-form-urlencoded");
    CheckHttpStatus("GetPublishedFileDetails", reply.status);
    return reply;
  });

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(response.body.begin(), response.body.end());
  } catch (const nlohmann::json::exception& e) {
    Fail("GetPublishedFileDetails: invalid JSON: " + std::string(e.what()));
  }
  const nlohmann::json* found = nullptr;
  if (auto reply = root.find("response"); reply != root.end() && reply->is_object()) {
    if (auto list = reply->find("publishedfiledetails"); list != reply->end()) found = &*list;
  }
  if (!found || !found->is_array()) Fail("GetPublishedFileDetails: response lacks publishedfiledetails array");
  const auto& details = *found;

  std::unordered_map<std::uint64_t, const nlohmann::json*> by_id;
  for (const auto& entry : details) {
    try {
      if (entry.is_object() && entry.contains("publishedfileid")) {
        by_id.emplace(ParseU64(entry["publishedfileid"]), &entry);
      }
    } catch (const std::exception&) {
      // An unparsable ID matches no request.
    }
  }
  std::vector<Item> items;
  items.reserve(ids.size());
  for (std::uint64_t id : ids) {
    if (auto found = by_id.find(id); found != by_id.end()) {
      items.push_back(ParseItem(id, *found->second));
    } else {
      items.push_back(FailedItem(id, ErrorKind::kNotFound, "not returned by Steam"));
    }
  }
  return items;
}

}  // namespace

void FetchItems(std::span<const std::uint64_t> ids, const HttpConfig& config, const std::stop_token& stop,
                const std::function<void(Item)>& on_item) {
  for (std::size_t first = 0; first < ids.size(); first += kBatchSize) {
    std::span<const std::uint64_t> batch = ids.subspan(first, std::min(kBatchSize, ids.size() - first));
    std::vector<Item> items;
    if (stop.stop_requested()) {
      for (std::uint64_t id : batch) items.push_back(FailedItem(id, ErrorKind::kCancelled, kCancelled));
    } else {
      try {
        items = FetchBatch(batch, config, stop);
      } catch (const std::exception& e) {
        ErrorKind kind = KindOf(e);
        std::string error = kind == ErrorKind::kCancelled ? kCancelled : std::string("item details: ") + e.what();
        items.clear();
        for (std::uint64_t id : batch) items.push_back(FailedItem(id, kind, error));
      }
    }
    for (Item& item : items) on_item(std::move(item));
  }
}

}  // namespace pxsteamdl::detail
