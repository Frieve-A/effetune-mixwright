#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace effetune::vst {

class ConfigStore {
public:
  ConfigStore();
  explicit ConfigStore(std::filesystem::path path);

  [[nodiscard]] bool load(std::string &content, std::string *error = nullptr) const;
  [[nodiscard]] bool save(std::string_view content, std::string *error = nullptr) const;
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  [[nodiscard]] static std::filesystem::path resolvePath();
  static void setError(std::string *destination, std::string value);

  std::filesystem::path path_;
};

} // namespace effetune::vst
