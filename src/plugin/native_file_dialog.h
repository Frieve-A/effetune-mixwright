#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace effetune::vst::plugin {

[[nodiscard]] std::optional<std::filesystem::path> choosePresetToOpen();
[[nodiscard]] std::optional<std::filesystem::path>
choosePresetToSave(std::string_view defaultName);
[[nodiscard]] std::string presetPathToUtf8(const std::filesystem::path &path);
[[nodiscard]] std::filesystem::path presetPathFromUtf8(std::string_view path);
[[nodiscard]] bool readPresetExchangeFile(const std::filesystem::path &path,
                                          std::string &content, std::string *error = nullptr);
[[nodiscard]] bool writePresetExchangeFile(const std::filesystem::path &path,
                                           std::string_view content,
                                           std::string *error = nullptr);

} // namespace effetune::vst::plugin
