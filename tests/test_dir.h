#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace lldstore::testing {

// A fresh, empty directory under the system temp dir, removed when the object dies.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = (std::filesystem::temp_directory_path() /
             ("lldstore-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++)))
                .string();
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string File(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

}  // namespace lldstore::testing
