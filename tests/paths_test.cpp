// SPDX-License-Identifier: LGPL-3.0-or-later
#include "paths.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace pxsteamdl::detail {
namespace {

TEST(PathsTest, NormalizesSeparators) {
    EXPECT_EQ(NormalizeManifestPath("About\\About.xml"), "About/About.xml");
    EXPECT_EQ(NormalizeManifestPath("a/b/c.txt"), "a/b/c.txt");
    EXPECT_EQ(NormalizeManifestPath(".hidden/..name"), ".hidden/..name");
}

TEST(PathsTest, RejectsUnsafePaths) {
    for (std::string path : {"", "/etc/passwd", "\\a", "a/", "a//b", ".", "..", "a/../b", "a/./b", "../a"}) {
        EXPECT_THROW(NormalizeManifestPath(path), std::runtime_error) << path;
    }
    EXPECT_THROW(NormalizeManifestPath(std::string("a\0b", 3)), std::runtime_error);
}

TEST(PathsTest, RoundTripsUtf8) {
    std::string name = "Textures/\xD0\x9C\xD0\xBE\xD0\xB4.png";  // "Мод"
    EXPECT_EQ(ToUtf8(Utf8Path(name)), name);
}

#ifdef _WIN32
TEST(PathsTest, RejectsNamesWindowsCannotRepresent) {
    for (std::string path : {"a:b", "CON", "con.txt", "dir/LPT1.log", "trailing.", "trailing "}) {
        EXPECT_THROW(NormalizeManifestPath(path), std::runtime_error) << path;
    }
    EXPECT_EQ(NormalizeManifestPath("COM10"), "COM10");
}
#else
TEST(PathsTest, AcceptsNamesOnlyWindowsRejects) { EXPECT_EQ(NormalizeManifestPath("a:b/CON"), "a:b/CON"); }
#endif

TEST(PathsTest, FoldsCaseWhereFileSystemsDo) {
#if defined(_WIN32) || defined(__APPLE__)
    EXPECT_EQ(PathKey("Textures/A.PNG"), "textures/a.png");
#else
    EXPECT_EQ(PathKey("Textures/A.PNG"), "Textures/A.PNG");
#endif
}

}  // namespace
}  // namespace pxsteamdl::detail
