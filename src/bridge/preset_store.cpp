#include "bridge/preset_store.h"

#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#elif defined(__APPLE__)
#include <cstdlib>
#endif

namespace effetune::vst {
namespace {

constexpr std::string_view kPresetFileName = "effetune_presets.json";

[[nodiscard]] std::string virtualFileName(const std::string_view path) {
  const auto separator = path.find_last_of("/\\");
  return std::string(path.substr(separator == std::string_view::npos ? 0 : separator + 1));
}

} // namespace

PresetStore::PresetStore() : presetPath_(resolvePresetPath()) {}

void PresetStore::setError(std::string *destination, std::string value) {
  if (destination != nullptr) {
    *destination = std::move(value);
  }
}

std::filesystem::path PresetStore::resolvePresetPath() {
#if defined(_WIN32)
  wchar_t appData[MAX_PATH]{};
  if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData) == S_OK) {
    const std::filesystem::path base(appData);
    const auto desktopDirectory = base / L"effetune";
    std::error_code error;
    if (std::filesystem::exists(desktopDirectory, error)) {
      return desktopDirectory / L"effetune_presets.json";
    }
    return base / L"Frieve" / L"EffeTunePlugin" / L"effetune_presets.json";
  }
#elif defined(__APPLE__)
  if (const auto *home = std::getenv("HOME"); home != nullptr) {
    const std::filesystem::path base(home);
    const auto desktopDirectory = base / "Library" / "Application Support" / "effetune";
    std::error_code error;
    if (std::filesystem::exists(desktopDirectory, error)) {
      return desktopDirectory / "effetune_presets.json";
    }
    return base / "Library" / "Application Support" / "Frieve" / "EffeTunePlugin" /
           "effetune_presets.json";
  }
#endif
  return std::filesystem::temp_directory_path() / "EffeTunePlugin" /
         "effetune_presets.json";
}

bool PresetStore::handles(const std::string_view virtualPath) const {
  return virtualFileName(virtualPath) == kPresetFileName;
}

bool PresetStore::exists(const std::string_view virtualPath) const {
  if (!handles(virtualPath)) {
    return false;
  }
  std::error_code error;
  return std::filesystem::is_regular_file(presetPath_, error);
}

bool PresetStore::read(const std::string_view virtualPath, std::string &content,
                       std::string *error) const {
  if (!handles(virtualPath)) {
    setError(error, "The requested virtual file is not allowed");
    return false;
  }
  std::ifstream input(presetPath_, std::ios::binary);
  if (!input) {
    setError(error, "Preset store does not exist");
    return false;
  }
  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    setError(error, "Unable to read preset store");
    return false;
  }
  return true;
}

bool PresetStore::write(const std::string_view virtualPath, const std::string_view content,
                        std::string *error) const {
  constexpr std::size_t maximumBytes = 8u * 1024u * 1024u;
  if (!handles(virtualPath) || content.size() > maximumBytes) {
    setError(error, "Preset store write is not allowed or is too large");
    return false;
  }

  std::error_code filesystemError;
  std::filesystem::create_directories(presetPath_.parent_path(), filesystemError);
  if (filesystemError) {
    setError(error, "Unable to create preset store directory");
    return false;
  }
  auto temporary = presetPath_;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
      setError(error, "Unable to write temporary preset store");
      return false;
    }
  }

#if defined(_WIN32)
  if (MoveFileExW(temporary.c_str(), presetPath_.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::filesystem::remove(temporary, filesystemError);
    setError(error, "Unable to atomically replace preset store");
    return false;
  }
#else
  std::filesystem::rename(temporary, presetPath_, filesystemError);
  if (filesystemError) {
    std::filesystem::remove(temporary, filesystemError);
    setError(error, "Unable to atomically replace preset store");
    return false;
  }
#endif
  return true;
}

} // namespace effetune::vst
