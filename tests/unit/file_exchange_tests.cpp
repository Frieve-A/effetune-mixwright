#include "plugin/native_file_dialog.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../support/crt_dialog_suppression.h"

namespace {

using namespace effetune::vst::plugin;

void expect(const bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  const auto path = std::filesystem::current_path() / "effetune-file-exchange-test.effetune_preset";
  std::error_code cleanupError;
  std::filesystem::remove(path, cleanupError);
  try {
    const std::string content = R"({"pipeline":[{"name":"Volume","enabled":true}]})";
    std::string error;
    expect(writePresetExchangeFile(path, content, &error), "preset export: " + error);
    std::string restored;
    expect(readPresetExchangeFile(path, restored, &error), "preset import: " + error);
    expect(restored == content, "preset exchange round trip");
    expect(!writePresetExchangeFile(path.parent_path() / "invalid.json", content, &error),
           "non-preset extension must be rejected");

    const auto utf8 = std::string("日本語.effetune_preset");
    expect(presetPathToUtf8(presetPathFromUtf8(utf8)) == utf8, "UTF-8 path round trip");
    std::filesystem::remove(path, cleanupError);
    std::cout << "All EffeTune preset file exchange tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::filesystem::remove(path, cleanupError);
    std::cerr << "Preset file exchange test failure: " << exception.what() << '\n';
    return 1;
  }
}
