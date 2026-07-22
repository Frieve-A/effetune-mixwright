#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace effetune::vst {

class PresetStore {
public:
  PresetStore();

  [[nodiscard]] bool handles(std::string_view virtualPath) const;
  [[nodiscard]] bool exists(std::string_view virtualPath) const;
  [[nodiscard]] bool read(std::string_view virtualPath, std::string &content,
                          std::string *error = nullptr) const;
  [[nodiscard]] bool write(std::string_view virtualPath, std::string_view content,
                           std::string *error = nullptr) const;
  [[nodiscard]] const std::filesystem::path &presetPath() const noexcept { return presetPath_; }

private:
  [[nodiscard]] static std::filesystem::path resolvePresetPath();
  static void setError(std::string *destination, std::string value);

  std::filesystem::path presetPath_;
};

} // namespace effetune::vst
