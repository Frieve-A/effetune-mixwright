#include "bridge/sfz_library.h"
#include <choc/memory/choc_Base64.h>
#include <choc/text/choc_JSON.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using effetune::vst::SfzLibrary;
using choc::value::Value;
void expect(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void write(const fs::path &path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary); output << text;
  expect(static_cast<bool>(output), "write fixture");
}
std::string contents(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
Value call(SfzLibrary &library, std::string_view operation, const Value &payload = choc::value::createObject({})) {
  return choc::json::parse(library.request(operation, choc::json::toString(payload)));
}
Value request(std::string_view id, std::string_view path, std::int64_t limit = SfzLibrary::maximumBytes) {
  auto value = choc::value::createObject({}); value.addMember("id", id);
  value.addMember("relativePath", path); value.addMember("maxBytes", limit); return value;
}
Value readRequest(std::string_view id, std::int64_t offset, std::int64_t length) {
  auto value = choc::value::createObject({}); value.addMember("readId", id);
  value.addMember("offset", offset); value.addMember("length", length); return value;
}
Value selectFolder(SfzLibrary &library, const fs::path &folder) {
  auto reply = choc::json::parse(library.request("select", "{}", folder));
  expect(reply["ok"].getBool(), "select folder"); return Value(reply["data"]["banks"]);
}
std::string readAll(SfzLibrary &library, std::string_view id, std::string_view path) {
  const auto opened = call(library, "openRead", request(id, path));
  expect(opened["ok"].getBool() && !opened["data"].isVoid(), "open relative file");
  const auto token = opened["data"]["readId"].get<std::string>();
  const auto size = opened["data"]["size"].getInt64();
  std::string text;
  for (std::int64_t offset = 0; offset < size;) {
    const auto length = std::min<std::int64_t>(SfzLibrary::chunkBytes, size - offset);
    const auto chunk = call(library, "readChunk", readRequest(token, offset, length));
    expect(chunk["ok"].getBool(), "read bounded chunk");
    expect(choc::base64::decodeToContainer(text, chunk["data"]["base64"].getString()), "decode chunk");
    offset += length;
  }
  auto close = choc::value::createObject({}); close.addMember("readId", token);
  expect(call(library, "closeRead", close)["data"].getBool(), "close logical read");
  return text;
}
void testLibrary(const fs::path &directory) {
  const auto root = directory / fs::path(u8"音源");
  write(root / "first.sfz", "#include \"include/regions.sfzinc\"\n");
  write(root / "nested/second.SFZ", "<region> sample=samples/tone.wav");
  write(root / "include/regions.sfzinc", "<region> sample=samples/tone.wav");
  const std::string sample(SfzLibrary::chunkBytes * 2 + 17, 'z');
  write(root / "samples/tone.wav", sample);
  const auto registry = directory / "settings/sfz-references.json";
  SfzLibrary library(registry);
  const auto selected = selectFolder(library, root);
  expect(selected.size() == 2 && selected[0]["selectedPath"].getString() == "first.sfz",
         "all instruments retain relative paths");
  const auto id = selected[0]["id"].get<std::string>();
  expect(id.size() == 24, "cryptographic reference identifier");
  expect(selectFolder(library, root / ".")[0]["id"].getString() == id, "canonical reselect preserves identifier");
  SfzLibrary reopened(registry);
  expect(call(reopened, "list")["data"].size() == 2, "registry survives reopening");
  expect(selectFolder(reopened, fs::canonical(root))[0]["id"].getString() == id, "identifier survives reopening");
  expect(readAll(library, id, "samples/tone.wav") == sample, "multi-chunk sample read is exact");
  expect(readAll(library, id, "include/regions.sfzinc").starts_with("<region>"), "include reads use same root");
  write(root / "samples/tone.wav", "changed source");
  expect(readAll(library, id, "samples\\tone.wav") == "changed source", "source edits are observable without import");
  write(root / "held.wav", "original");
  const auto held = call(library, "openRead", request(id, "held.wav"));
  expect(held["ok"].getBool() && held["data"]["size"].getInt64() == 8, "admit original held file size");
  const auto heldId = held["data"]["readId"].get<std::string>();
  fs::rename(root / "held.wav", root / "held-original.wav");
  write(root / "held.wav", "replacement contents");
  const auto oldChunk = call(library, "readChunk", readRequest(heldId, 0, 8));
  std::string oldBytes;
  expect(oldChunk["ok"].getBool() &&
         choc::base64::decodeToContainer(oldBytes, oldChunk["data"]["base64"].getString()) &&
         oldBytes == "original", "logical read keeps its admitted handle across source replacement");
  expect(readAll(library, id, "held.wav") == "replacement contents", "new read sees replacement source");
  write(root / "held-original.wav", "x");
  const auto shortRead = call(library, "readChunk", readRequest(heldId, 0, 8));
  expect(!shortRead["ok"].getBool() && shortRead["code"].getString() == "storage-failed",
         "truncated held source fails instead of returning partial bytes");
  auto closeHeld = choc::value::createObject({}); closeHeld.addMember("readId", heldId);
  expect(call(library, "closeRead", closeHeld)["data"].getBool(), "failed logical read can be closed");
  const auto metadata = choc::json::parse(contents(registry));
  expect(metadata[0].size() == 4 && metadata[0]["path"].isString() && metadata[0]["root"].isString(),
         "registry stores only reference metadata");
  for (const auto &file : fs::recursive_directory_iterator(directory))
    expect(file.path().extension() != ".sfzbank", "never creates copied SFZ banks");
  expect(std::distance(fs::directory_iterator(registry.parent_path()), fs::directory_iterator{}) == 2,
         "only registry and process lock persisted");
  for (const auto path : {"../outside.wav", "/absolute.wav", "C:\\absolute.wav", "samples/../../outside.wav", "samples/x:y"})
    expect(!call(library, "openRead", request(id, path))["ok"].getBool(), "reject path escape");
  const auto nulRequest = std::string(R"({"id":")") + id +
      R"(","relativePath":"x\u0000y","maxBytes":4096})";
  expect(!choc::json::parse(library.request("openRead", nulRequest))["ok"].getBool(), "reject NUL");
  expect(call(library, "openRead", request(id, "missing.wav"))["data"].isVoid(), "missing dependency returns null");
  expect(call(library, "openRead", request(id, "samples/tone.wav", 3))["code"].getString() == "too-large",
         "oversized dependency uses upstream size error");
  expect(!call(library, "openRead", request(id, "samples", 100))["ok"].getBool(), "reject directories");
  const auto opened = call(library, "openRead", request(id, "samples/tone.wav"));
  const auto token = opened["data"]["readId"].get<std::string>();
  expect(!call(library, "readChunk", readRequest(token, -1, 1))["ok"].getBool(), "reject negative offset");
  expect(!call(library, "readChunk", readRequest(token, 0, SfzLibrary::chunkBytes + 1))["ok"].getBool(), "reject oversized chunk");
  expect(!call(library, "readChunk", readRequest(token, 13, 2))["ok"].getBool(), "reject beyond admitted length");
  library.closeReads();
  expect(!call(library, "readChunk", readRequest(token, 0, 1))["ok"].getBool(), "page closure invalidates reads");
  for (int index = 0; index < 16; ++index)
    expect(call(library, "openRead", request(id, "first.sfz"))["ok"].getBool(), "admit bounded sessions");
  expect(!call(library, "openRead", request(id, "first.sfz"))["ok"].getBool(), "cap live sessions");
  library.closeReads();
  expect(call(library, "openRead", request(id, "first.sfz"))["ok"].getBool(), "cleanup frees session capacity");
  library.closeReads();
  const auto outside = directory / "outside";
  write(outside / "escape.sfz", "outside");
  std::error_code symlinkError;
  fs::create_directory_symlink(outside, root / "escape", symlinkError);
  if (!symlinkError) {
    expect(selectFolder(library, root).size() == 2, "scan does not follow directory links");
    expect(!call(library, "openRead", request(id, "escape/escape.sfz"))["ok"].getBool(), "reject canonical escape");
  } else std::cout << "Symlink fixture unavailable: " << symlinkError.message() << '\n';
  auto remove = choc::value::createObject({}); remove.addMember("id", id);
  expect(call(reopened, "remove", remove)["data"].getBool(), "remove reference");
  expect(call(library, "list")["data"].size() == 1, "other instances reread mutations");
  expect(contents(root / "first.sfz").starts_with("#include"), "remove preserves original source");
  const auto otherId = selected[1]["id"].get<std::string>();
  fs::rename(root, directory / "moved");
  expect(call(library, "openRead", request(otherId, "nested/second.SFZ"))["data"].isVoid(), "missing root returns null");
  write(registry, "invalid JSON");
  expect(!call(library, "remove", remove)["ok"].getBool() && contents(registry) == "invalid JSON",
         "corrupt registry cannot be overwritten by mutation");
  write(registry, std::string(1024 * 1024 + 1, ' '));
  expect(call(library, "list")["code"].getString() == "too-large", "bounded registry read");
}
void testConcurrent(const fs::path &directory) {
  const auto registry = directory / "concurrent/sfz-references.json";
  const auto a = directory / "a", b = directory / "b";
  write(a / "a.sfz", "a"); write(b / "b.sfz", "b");
  SfzLibrary first(registry), second(registry);
  std::atomic<int> ready{0}; std::atomic<bool> okay{true};
  auto run = [&](SfzLibrary &library, const fs::path &folder) {
    ready.fetch_add(1);
    while (ready.load() != 2) std::this_thread::yield();
    for (int count = 0; count < 5; ++count)
      if (!choc::json::parse(library.request("select", "{}", folder))["ok"].getBool()) okay.store(false);
  };
  std::thread one(run, std::ref(first), std::cref(a)), two(run, std::ref(second), std::cref(b));
  one.join(); two.join();
  expect(okay.load() && call(first, "list")["data"].size() == 2, "concurrent instances preserve both mutations");
}
// Launch only this test executable, without a shell, and retain ownership of
// each child until reaped. The barrier gives both processes a common start.
class RegistrationChild {
public:
  explicit RegistrationChild(const std::vector<fs::path> &arguments) {
#if defined(_WIN32)
    std::wstring command;
    for (const auto &argument : arguments) {
      if (!command.empty()) command += L' ';
      command += L'"';
      std::size_t slashes = 0;
      for (const auto character : argument.native()) {
        if (character == L'\\') { ++slashes; continue; }
        command.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'"') command += L'\\';
        command += character;
      }
      command.append(slashes * 2, L'\\'); command += L'"';
    }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    expect(CreateProcessW(arguments.front().c_str(), command.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
           "launch SFZ registration child");
    process_ = process.hProcess; CloseHandle(process.hThread);
#else
    std::vector<std::string> strings;
    for (const auto &argument : arguments) strings.push_back(argument.native());
    std::vector<char *> pointers;
    for (auto &argument : strings) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    process_ = fork();
    expect(process_ >= 0, "launch SFZ registration child");
    if (process_ == 0) { execv(pointers.front(), pointers.data()); _exit(127); }
#endif
  }
  ~RegistrationChild() {
#if defined(_WIN32)
    if (process_ != nullptr) {
      if (!finished_) { TerminateProcess(process_, 1); WaitForSingleObject(process_, 60000); }
      CloseHandle(process_);
    }
#else
    if (process_ > 0 && !finished_) {
      kill(process_, SIGKILL);
      while (waitpid(process_, nullptr, 0) < 0 && errno == EINTR) {}
    }
#endif
  }
  RegistrationChild(const RegistrationChild &) = delete;
  RegistrationChild &operator=(const RegistrationChild &) = delete;
  bool finished() {
    if (finished_) return true;
#if defined(_WIN32)
    const auto wait = WaitForSingleObject(process_, 0);
    expect(wait != WAIT_FAILED, "poll SFZ registration child");
    if (wait == WAIT_TIMEOUT) return false;
    DWORD result = 1;
    expect(GetExitCodeProcess(process_, &result) != 0, "read SFZ child result");
    finished_ = true;
    expect(result == 0, "SFZ registration child succeeded");
#else
    int result = 0;
    const auto waited = waitpid(process_, &result, WNOHANG);
    if (waited == 0 || (waited < 0 && errno == EINTR)) return false;
    expect(waited == process_, "poll SFZ registration child");
    finished_ = true;
    expect(WIFEXITED(result) && WEXITSTATUS(result) == 0, "SFZ registration child succeeded");
#endif
    return true;
  }
private:
#if defined(_WIN32)
  HANDLE process_ = nullptr;
#else
  pid_t process_ = -1;
#endif
  bool finished_ = false;
};

int registerInChild(const fs::path &registry, const fs::path &folder,
                    const fs::path &ready, const fs::path &start) {
  try {
    SfzLibrary library(registry);
    write(ready, "ready");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
    while (!fs::exists(start)) {
      expect(std::chrono::steady_clock::now() < deadline, "SFZ registration start timed out");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::string id;
    for (int count = 0; count < 5; ++count) {
      const auto selected = selectFolder(library, folder);
      expect(selected.size() == 1, "child selected its instrument");
      const auto next = selected[0]["id"].get<std::string>();
      expect(id.empty() || id == next, "child reselect keeps stable identifier"); id = next;
    }
    auto result = ready; result += ".id"; write(result, id);
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

void testConcurrentProcesses(const fs::path &directory, const fs::path &executable) {
  const auto registry = directory / "processes/sfz-references.json";
  const auto a = directory / "process-a", b = directory / "process-b";
  const auto readyA = directory / "process-a.ready", readyB = directory / "process-b.ready";
  const auto start = directory / "process-start";
  write(a / "a.sfz", "process a"); write(b / "b.sfz", "process b");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
  RegistrationChild first({executable, "--register", registry, a, readyA, start});
  RegistrationChild second({executable, "--register", registry, b, readyB, start});
  while (!fs::exists(readyA) || !fs::exists(readyB)) {
    expect(std::chrono::steady_clock::now() < deadline, "SFZ process readiness timed out");
    expect(!first.finished() && !second.finished(), "SFZ children wait for common start");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  write(start, "start");
  while (!first.finished() || !second.finished()) {
    expect(std::chrono::steady_clock::now() < deadline, "SFZ process completion timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SfzLibrary reopened(registry);
  expect(call(reopened, "list")["data"].size() == 2, "both processes preserve registry updates");
  const auto idA = contents(directory / "process-a.ready.id");
  const auto idB = contents(directory / "process-b.ready.id");
  expect(selectFolder(reopened, a)[0]["id"].getString() == idA &&
         selectFolder(reopened, b)[0]["id"].getString() == idB && idA != idB,
         "reopening retains both process-created identifiers");
  expect(readAll(reopened, idA, "a.sfz") == "process a" &&
         readAll(reopened, idB, "b.sfz") == "process b", "process-created references read original files");
}
} // namespace
#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  if (argc == 6 && fs::path(argv[1]) == "--register")
    return registerInChild(argv[2], argv[3], argv[4], argv[5]);

  auto directory = fs::temp_directory_path() / ("effetune-sfz-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(directory); directory = fs::canonical(directory);
    testLibrary(directory); testConcurrent(directory);
    testConcurrentProcesses(directory, fs::canonical(fs::absolute(argv[0])));
    fs::remove_all(directory);
    std::cout << "SFZ direct-reference tests passed\n"; return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; std::error_code ignored; fs::remove_all(directory, ignored); return 1;
  }
}
