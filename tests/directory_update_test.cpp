// SPDX-License-Identifier: LGPL-3.0-or-later
#include "directory_update.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "common.hpp"

namespace pxsteamdl::detail {
namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path) {
  std::string text;
  std::ifstream(path) >> text;
  return text;
}

class DirectoryUpdateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_root = fs::temp_directory_path() / ("pxsteamdl-update-test-" + std::to_string(std::random_device{}()));
    m_destination = m_root / "111";
    fs::create_directories(m_destination);
    std::ofstream(m_destination / "old.txt") << "old";
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(m_root, ignored);
  }
  fs::path m_root;
  fs::path m_destination;
};

TEST_F(DirectoryUpdateTest, InstallationFailureRestoresTheRenamedOldDirectory) {
  {
    DirectoryUpdate update(m_destination, {});
    // Make the second rename fail after the first has moved the old directory aside.
    fs::remove_all(update.staging());
    EXPECT_THROW(update.commit({}), fs::filesystem_error);
    EXPECT_EQ(ReadFile(m_destination / "old.txt"), "old");
  }
  EXPECT_EQ(std::distance(fs::directory_iterator(m_root), fs::directory_iterator{}), 2);
}

TEST_F(DirectoryUpdateTest, CancellationBeforeInstallationKeepsTheOldDirectory) {
  DirectoryUpdate update(m_destination, {});
  std::ofstream(update.staging() / "new.txt") << "new";
  std::stop_source stop;
  stop.request_stop();
  try {
    update.commit(stop.get_token());
    FAIL() << "expected cancellation";
  } catch (const Error& error) {
    EXPECT_EQ(error.kind(), ErrorKind::kCancelled);
  }
  EXPECT_EQ(ReadFile(m_destination / "old.txt"), "old");
  EXPECT_FALSE(fs::exists(m_destination / "new.txt"));
}

TEST_F(DirectoryUpdateTest, InstallsTheNewDirectoryAndRemovesTheBackup) {
  {
    DirectoryUpdate update(m_destination, {});
    std::ofstream(update.staging() / "new.txt") << "new";
    update.commit({});
    EXPECT_EQ(ReadFile(m_destination / "new.txt"), "new");
    EXPECT_FALSE(fs::exists(m_destination / "old.txt"));
  }
  EXPECT_EQ(std::distance(fs::directory_iterator(m_root), fs::directory_iterator{}), 2);
}

}  // namespace
}  // namespace pxsteamdl::detail
