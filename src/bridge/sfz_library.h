#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace effetune::vst {

// Control-thread direct references. Samples remain in the user-selected folder.
class SfzLibrary {
public:
  static constexpr std::size_t maximumBytes = 1024u * 1024u * 1024u;
  static constexpr std::size_t chunkBytes = 192u * 1024u;
  explicit SfzLibrary(std::filesystem::path registryPath);
  ~SfzLibrary();
  SfzLibrary(const SfzLibrary &) = delete;
  SfzLibrary &operator=(const SfzLibrary &) = delete;

  // Returns the version-one {ok,data}/{ok,code} envelope. Only select consumes
  // selectedFolder, which must come from a native folder chooser.
  [[nodiscard]] std::string request(std::string_view operation,
                                    std::string_view payload = "{}",
                                    const std::filesystem::path &selectedFolder = {});
  void closeReads();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace effetune::vst
