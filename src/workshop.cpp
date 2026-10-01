// SPDX-License-Identifier: LGPL-3.0-or-later
#include "workshop.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "http.hpp"

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

// One publishedfiledetails entry; a malformed entry fails only its own item.
Item ParseItem(std::uint64_t id, const nlohmann::json& entry) {
    Item item;
    item.id = id;
    try {
        if (std::uint64_t result = U64Field(entry, "result"); result != kEResultOk) {
            item.error = "Steam result " + std::to_string(result);
            return item;
        }
        item.manifest_id = U64Field(entry, "hcontent_file");
        item.app_id = static_cast<std::uint32_t>(U64Field(entry, "consumer_app_id"));
        item.title = StringField(entry, "title");
        item.file_url = StringField(entry, "file_url");
        item.filename = StringField(entry, "filename");
    } catch (const std::exception& e) {
        item.error = std::string("invalid item details: ") + e.what();
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

// Fetches the details of up to kBatchSize items and appends one Item per ID to items.
void FetchBatch(std::span<const std::uint64_t> ids, std::vector<Item>& items,
                const std::function<void(const Item&)>& on_item) {
    HttpResponse response = HttpRequest(kDetailsUrl, "POST", DetailsForm(ids), "application/x-www-form-urlencoded");
    if (response.status != 200) Fail("GetPublishedFileDetails: HTTP " + std::to_string(response.status));

    auto root = nlohmann::json::parse(response.body.begin(), response.body.end());
    const auto& details = root.at("response").at("publishedfiledetails");
    if (!details.is_array()) Fail("GetPublishedFileDetails: response lacks publishedfiledetails array");

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
    for (std::uint64_t id : ids) {
        if (auto found = by_id.find(id); found != by_id.end()) {
            items.push_back(ParseItem(id, *found->second));
        } else {
            Item& item = items.emplace_back();
            item.id = id;
            item.error = "not returned by Steam";
        }
        if (on_item) on_item(items.back());
    }
}

}  // namespace

std::vector<Item> FetchItems(std::span<const std::uint64_t> ids, const std::function<void(const Item&)>& on_item) {
    std::vector<Item> items;
    items.reserve(ids.size());
    for (std::size_t first = 0; first < ids.size(); first += kBatchSize) {
        FetchBatch(ids.subspan(first, std::min(kBatchSize, ids.size() - first)), items, on_item);
    }
    return items;
}

}  // namespace pxsteamdl::detail
