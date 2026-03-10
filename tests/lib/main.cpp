#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>

#include "util.hpp"

namespace fs = std::filesystem;

class Environment : public ::testing::Environment {
 private:
  bool delete_mountpoint = false;

 public:
  ~Environment() override {}

  void SetUp() override {
    if (!fs::exists(TEST_ROOT)) {
      fs::create_directories(TEST_ROOT);
      delete_mountpoint = true;
    }
  }

  void TearDown() override {
    if (delete_mountpoint) {
      fs::remove(TEST_ROOT);
    }
  }
};

int main(int argc, char** argv) {
  // gtest now owns this environment, no need to delete
  testing::AddGlobalTestEnvironment(new Environment);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
