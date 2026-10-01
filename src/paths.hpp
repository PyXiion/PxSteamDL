// SPDX-License-Identifier: LGPL-3.0-or-later
// Conversion and validation of the untrusted UTF-8 paths that manifests carry.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pxsteamdl::detail {

// Manifest names are UTF-8; converting through std::string would use the ANSI code page on Windows.
std::filesystem::path Utf8Path(std::string_view text);

// The path as UTF-8 with '/' separators.
std::string ToUtf8(const std::filesystem::path& path);

// Whether Win32 would reinterpret this non-empty path component: stream or drive separators, wildcards, device
// names, and trailing dots or spaces (silently stripped, so "a." would alias "a"). Always false elsewhere.
bool IsWindowsUnsafeName(std::string_view name);

// Key under which a manifest path is compared with others and with what is on disk. Windows and macOS file
// systems are case-insensitive by default, so "Textures/a.png" and "textures/a.png" name the same file there;
// ASCII folding covers the paths mods use in practice.
std::string PathKey(std::string path);

// Converts '\' to '/' and returns the path; throws unless it is a non-empty relative path whose components are
// representable on this system and none of which is empty, "." or "..".
std::string NormalizeManifestPath(std::string path);

}  // namespace pxsteamdl::detail
