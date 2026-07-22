#include "native_file_dialog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#include <Windows.h>
#include <ShObjIdl.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace effetune::vst::plugin {
namespace {

constexpr std::uintmax_t kMaximumPresetBytes = 8u * 1024u * 1024u;

void setError(std::string *destination, std::string message) {
  if (destination != nullptr) {
    *destination = std::move(message);
  }
}

bool hasPresetExtension(const std::filesystem::path &path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return extension == ".effetune_preset";
}

#if defined(_WIN32)
class ComScope {
public:
  ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComScope() {
    if (result_ == S_OK || result_ == S_FALSE) {
      CoUninitialize();
    }
  }

private:
  HRESULT result_ = E_FAIL;
};

template <typename Dialog>
std::optional<std::filesystem::path> runWindowsDialog(const CLSID &classId,
                                                      const std::wstring &defaultName) {
  ComScope com;
  Dialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(classId, nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(Dialog), reinterpret_cast<void **>(&dialog)))) {
    return std::nullopt;
  }
  constexpr COMDLG_FILTERSPEC filter[] = {
      {L"EffeTune Mixwright preset", L"*.effetune_preset"}, {L"All files", L"*.*"}};
  (void)dialog->SetFileTypes(static_cast<UINT>(std::size(filter)), filter);
  (void)dialog->SetFileTypeIndex(1);
  (void)dialog->SetDefaultExtension(L"effetune_preset");
  if (!defaultName.empty()) {
    (void)dialog->SetFileName(defaultName.c_str());
  }
  if (dialog->Show(nullptr) != S_OK) {
    dialog->Release();
    return std::nullopt;
  }
  IShellItem *item = nullptr;
  if (FAILED(dialog->GetResult(&item)) || item == nullptr) {
    dialog->Release();
    return std::nullopt;
  }
  PWSTR value = nullptr;
  const auto result = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
  std::optional<std::filesystem::path> path;
  if (SUCCEEDED(result) && value != nullptr) {
    path = std::filesystem::path(value);
  }
  CoTaskMemFree(value);
  item->Release();
  dialog->Release();
  if (path.has_value() && !hasPresetExtension(*path)) {
    return std::nullopt;
  }
  return path;
}

std::wstring utf8ToWide(const std::string_view value) {
  if (value.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size);
  return result;
}
#elif defined(__APPLE__)
std::optional<std::filesystem::path> runMacDialog(const bool save,
                                                  const std::string_view defaultName) {
  using SendId = id (*)(id, SEL);
  using SendIdCString = id (*)(id, SEL, const char *);
  using SendVoidId = void (*)(id, SEL, id);
  using SendInteger = long (*)(id, SEL);
  using SendCString = const char *(*)(id, SEL);

  const auto panelClass = reinterpret_cast<id>(
      objc_getClass(save ? "NSSavePanel" : "NSOpenPanel"));
  auto panel = reinterpret_cast<SendId>(objc_msgSend)(
      panelClass, sel_registerName(save ? "savePanel" : "openPanel"));
  if (panel == nullptr) return std::nullopt;
  if (save && !defaultName.empty()) {
    const auto stringClass = reinterpret_cast<id>(objc_getClass("NSString"));
    const auto name = reinterpret_cast<SendIdCString>(objc_msgSend)(
        stringClass, sel_registerName("stringWithUTF8String:"),
        std::string(defaultName).c_str());
    reinterpret_cast<SendVoidId>(objc_msgSend)(
        panel, sel_registerName("setNameFieldStringValue:"), name);
  }
  if (reinterpret_cast<SendInteger>(objc_msgSend)(panel, sel_registerName("runModal")) != 1) {
    return std::nullopt;
  }
  const auto url = reinterpret_cast<SendId>(objc_msgSend)(panel, sel_registerName("URL"));
  const auto pathString = reinterpret_cast<SendId>(objc_msgSend)(url, sel_registerName("path"));
  const auto utf8 = reinterpret_cast<SendCString>(objc_msgSend)(
      pathString, sel_registerName("UTF8String"));
  if (utf8 == nullptr) return std::nullopt;
  auto path = presetPathFromUtf8(utf8);
  return hasPresetExtension(path) ? std::optional(path) : std::nullopt;
}
#endif

} // namespace

std::optional<std::filesystem::path> choosePresetToOpen() {
#if defined(_WIN32)
  return runWindowsDialog<IFileOpenDialog>(CLSID_FileOpenDialog, {});
#elif defined(__APPLE__)
  return runMacDialog(false, {});
#else
  return std::nullopt;
#endif
}

std::optional<std::filesystem::path> choosePresetToSave(const std::string_view defaultName) {
  auto name = std::string(defaultName);
  if (!name.ends_with(".effetune_preset")) {
    name += ".effetune_preset";
  }
#if defined(_WIN32)
  return runWindowsDialog<IFileSaveDialog>(CLSID_FileSaveDialog, utf8ToWide(name));
#elif defined(__APPLE__)
  return runMacDialog(true, name);
#else
  return std::nullopt;
#endif
}

std::string presetPathToUtf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::filesystem::path presetPathFromUtf8(const std::string_view path) {
  std::u8string value(path.size(), u8'\0');
  std::memcpy(value.data(), path.data(), path.size());
  return std::filesystem::path(value);
}

bool readPresetExchangeFile(const std::filesystem::path &path, std::string &content,
                            std::string *error) {
  std::error_code statusError;
  if (!hasPresetExtension(path) || !std::filesystem::is_regular_file(path, statusError)) {
    setError(error, "The selected preset file does not exist");
    return false;
  }
  const auto bytes = std::filesystem::file_size(path, statusError);
  if (statusError || bytes > kMaximumPresetBytes) {
    setError(error, "The selected preset file exceeds the 8 MB limit");
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    setError(error, "Unable to open the selected preset file");
    return false;
  }
  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return input.good() || input.eof();
}

bool writePresetExchangeFile(const std::filesystem::path &path, const std::string_view content,
                             std::string *error) {
  if (!hasPresetExtension(path) || content.size() > kMaximumPresetBytes) {
    setError(error, "Preset export must use .effetune_preset and be at most 8 MB");
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    setError(error, "Unable to create the selected preset file");
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output) {
    setError(error, "Unable to write the selected preset file");
    return false;
  }
  return true;
}

} // namespace effetune::vst::plugin
