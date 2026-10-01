// SPDX-License-Identifier: LGPL-3.0-or-later
#include "paths.hpp"

#include <algorithm>
#include <cstddef>

#include "common.hpp"

namespace pxsteamdl::detail {

namespace {

char ToAsciiUpper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

[[maybe_unused]] char ToAsciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

[[maybe_unused]] bool IsWindowsDeviceName(std::string_view name) {
    std::string stem(name.substr(0, name.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), ToAsciiUpper);
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9';
}

}  // namespace

std::filesystem::path Utf8Path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string ToUtf8(const std::filesystem::path& path) {
    std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

bool IsWindowsUnsafeName([[maybe_unused]] std::string_view name) {
#ifdef _WIN32
    bool has_control =
        std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    return name.find_first_of("<>:\"|?*") != std::string_view::npos || name.back() == '.' || name.back() == ' ' ||
           has_control || IsWindowsDeviceName(name);
#else
    return false;
#endif
}

std::string PathKey(std::string path) {
#if defined(_WIN32) || defined(__APPLE__)
    std::transform(path.begin(), path.end(), path.begin(), ToAsciiLower);
#endif
    return path;
}

std::string NormalizeManifestPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\0') != std::string::npos ||
        Utf8Path(path).has_root_path()) {
        Fail("manifest: unsafe path: " + path);
    }
    for (std::size_t from = 0; from <= path.size();) {
        std::size_t end = std::min(path.find('/', from), path.size());
        std::string_view name(path.data() + from, end - from);
        if (name.empty() || name == "." || name == "..") Fail("manifest: unsafe path: " + path);
        if (IsWindowsUnsafeName(name)) Fail("manifest: path not representable on Windows: " + path);
        from = end + 1;
    }
    return path;
}

}  // namespace pxsteamdl::detail
