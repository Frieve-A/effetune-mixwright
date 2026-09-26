#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace effetune::vst::bridge {

// A control-thread export writes only to the destination chosen by a native dialog.
class BackupExport {
public:
  static constexpr std::size_t maximumBytes = 256u * 1024u * 1024u;
  static constexpr std::size_t chunkBytes = 192u * 1024u;
  ~BackupExport() { cancel(); }
  [[nodiscard]] bool begin(const std::filesystem::path &destination,
                           std::size_t totalBytes, std::string *error = nullptr);
  [[nodiscard]] bool append(std::size_t offset, std::string_view base64,
                            std::string *error = nullptr);
  [[nodiscard]] bool commit(std::string *error = nullptr);
  void cancel();

private:
  [[nodiscard]] bool fail(std::string *error, const char *message);
  std::filesystem::path destination_;
  std::filesystem::path stagingDirectory_;
  std::filesystem::path stagingFile_;
  std::ofstream stream_;
  std::size_t totalBytes_ = 0;
  std::size_t writtenBytes_ = 0;
};

} // namespace effetune::vst::bridge
