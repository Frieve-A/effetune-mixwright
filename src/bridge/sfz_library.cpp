#include "bridge/sfz_library.h"

#include <choc/memory/choc_Base64.h>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace effetune::vst {
namespace {
namespace fs = std::filesystem;
using choc::value::Value;
using choc::value::ValueView;
constexpr std::size_t registryLimit = 1024u * 1024u;
constexpr std::size_t entryLimit = 10000;

struct TooLarge : std::runtime_error { TooLarge() : std::runtime_error("SFZ size limit exceeded") {} };
void require(bool condition, const char *detail) {
  if (!condition) throw std::runtime_error(detail);
}
std::string utf8(const fs::path &path) {
  const auto value = path.generic_u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}
fs::path fromUtf8(std::string_view value) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
bool validId(std::string_view id) {
  return id.size() == 24 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool samePath(const fs::path &a, const fs::path &b) {
#if defined(_WIN32)
  // Registry JSON uses generic separators while canonical Windows paths use
  // preferred separators. Compare the same spelling before folding case.
  const auto left = fs::path(a).make_preferred();
  const auto right = fs::path(b).make_preferred();
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
  return a == b;
#endif
}
bool within(const fs::path &root, const fs::path &file) {
  auto r = root.begin();
  auto f = file.begin();
  for (; r != root.end(); ++r, ++f)
    if (f == file.end() || !samePath(*r, *f)) return false;
  return f != file.end();
}
bool isSfz(const fs::path &path) {
  auto extension = utf8(path.extension());
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".sfz";
}
bool missing(const std::error_code &error) {
  return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
}
std::string randomId() {
  std::array<unsigned char, 12> bytes{};
#if defined(_WIN32)
  require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "SFZ random identifier failed");
#else
  arc4random_buf(bytes.data(), bytes.size());
#endif
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}

// Each operation reloads under this process-shared lock, including readers.
class RegistryLock {
public:
  explicit RegistryLock(const fs::path &registry) {
    fs::create_directories(registry.parent_path());
    auto path = registry; path += ".lock";
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle_ != INVALID_HANDLE_VALUE, "SFZ registry lock open failed");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap_)) {
      CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error("SFZ registry lock failed");
    }
#else
    handle_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(handle_ >= 0, "SFZ registry lock open failed");
    struct stat info{};
    if (fstat(handle_, &info) || !S_ISREG(info.st_mode) || flock(handle_, LOCK_EX)) {
      ::close(handle_); handle_ = -1;
      throw std::runtime_error("SFZ registry lock failed");
    }
#endif
  }
  ~RegistryLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) { UnlockFileEx(handle_, 0, 1, 0, &overlap_); CloseHandle(handle_); }
#else
    if (handle_ >= 0) { flock(handle_, LOCK_UN); ::close(handle_); }
#endif
  }
  RegistryLock(const RegistryLock &) = delete;
  RegistryLock &operator=(const RegistryLock &) = delete;
private:
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlap_{};
#else
  int handle_ = -1;
#endif
};

struct Entry { std::string id, name; fs::path path, root; };
Value publicEntry(const Entry &entry) {
  auto result = choc::value::createObject({});
  result.addMember("id", entry.id);
  result.addMember("name", entry.name);
  result.addMember("selectedPath", utf8(entry.path.lexically_relative(entry.root)));
  return result;
}
std::vector<Entry> load(const fs::path &path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (missing(error) || status.type() == fs::file_type::not_found) return {};
  require(!error && fs::is_regular_file(status), "Invalid SFZ registry file");
#if defined(_WIN32)
  require((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
          "Invalid SFZ registry reparse point");
#endif
  const auto size = fs::file_size(path);
  if (size > registryLimit) throw TooLarge();
  std::ifstream input(path, std::ios::binary);
  std::string text(static_cast<std::size_t>(size), '\0');
  require(static_cast<bool>(input.read(text.data(), static_cast<std::streamsize>(size))),
          "SFZ registry read failed");
  require(input.peek() == std::char_traits<char>::eof(), "SFZ registry changed while reading");
  const auto value = choc::json::parse(text);
  require(value.isArray() && value.size() <= entryLimit, "Invalid SFZ registry array");
  std::vector<Entry> entries;
  for (const auto item : value) {
    require(item.isObject() && item["id"].isString() && item["name"].isString() &&
            item["path"].isString() && item["root"].isString(), "Invalid SFZ registry entry");
    Entry entry{item["id"].get<std::string>(), item["name"].get<std::string>(),
                fromUtf8(item["path"].get<std::string>()), fromUtf8(item["root"].get<std::string>())};
    require(validId(entry.id) && entry.path.is_absolute() && entry.root.is_absolute() &&
            entry.path == entry.path.lexically_normal() && entry.root == entry.root.lexically_normal() &&
            within(entry.root, entry.path) && isSfz(entry.path) &&
            utf8(entry.path).find('\0') == std::string::npos &&
            utf8(entry.root).find('\0') == std::string::npos, "Invalid SFZ reference");
    require(std::none_of(entries.begin(), entries.end(), [&](const Entry &old) {
      return old.id == entry.id || samePath(old.path, entry.path);
    }), "Duplicate SFZ reference");
    entries.push_back(std::move(entry));
  }
  return entries;
}
void save(const fs::path &path, const std::vector<Entry> &entries) {
  if (entries.size() > entryLimit) throw TooLarge();
  auto array = choc::value::createEmptyArray();
  for (const auto &entry : entries) {
    auto value = choc::value::createObject({});
    value.addMember("id", entry.id); value.addMember("name", entry.name);
    value.addMember("path", utf8(entry.path)); value.addMember("root", utf8(entry.root));
    array.addArrayElement(std::move(value));
  }
  const auto text = choc::json::toString(array);
  if (text.size() > registryLimit) throw TooLarge();
  auto temporary = path; temporary += "." + randomId() + ".tmp";
  try {
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output.write(text.data(), static_cast<std::streamsize>(text.size())); output.flush();
      require(static_cast<bool>(output), "SFZ registry write failed");
    }
#if defined(_WIN32)
    require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING |
                       MOVEFILE_WRITE_THROUGH) != 0, "SFZ registry replacement failed");
#else
    fs::rename(temporary, path);
#endif
  } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
}
std::uint64_t integer(ValueView payload, const char *key, std::uint64_t minimum,
                      std::uint64_t maximum) {
  const auto value = payload[key];
  const auto number = value.getWithDefault<std::int64_t>(-1);
  require(value.isInt() && number >= 0 && static_cast<std::uint64_t>(number) >= minimum &&
          static_cast<std::uint64_t>(number) <= maximum, "Invalid SFZ read bounds");
  return static_cast<std::uint64_t>(number);
}
std::string identifier(ValueView payload, const char *key) {
  const auto value = payload[key].getWithDefault<std::string>({});
  require(validId(value), "Invalid SFZ identifier"); return value;
}

// The open handle survives the entire logical read. No request allocates a file-sized buffer.
struct ReadFile {
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~ReadFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
  int handle = -1;
  ~ReadFile() { if (handle >= 0) ::close(handle); }
#endif
  std::uint64_t size = 0;
  bool open(const fs::path &path, const fs::path &root) {
#if defined(_WIN32)
    handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                         FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
      throw std::runtime_error("SFZ source open failed");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "SFZ source is not regular");
    size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    std::wstring actual(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, actual.data(),
                                                static_cast<DWORD>(actual.size()), FILE_NAME_NORMALIZED);
    require(length > 0 && length < actual.size(), "SFZ opened path lookup failed");
    actual.resize(length);
    if (actual.starts_with(L"\\\\?\\UNC\\")) actual = L"\\\\" + actual.substr(8);
    else if (actual.starts_with(L"\\\\?\\")) actual.erase(0, 4);
    require(within(root, fs::path(actual)), "SFZ opened source escaped its folder");
#else
    handle = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (handle < 0) {
      if (errno == ENOENT || errno == ENOTDIR) return false;
      throw std::runtime_error("SFZ source open failed");
    }
    struct stat info{};
    require(!fstat(handle, &info) && S_ISREG(info.st_mode) && info.st_size >= 0,
            "SFZ source is not regular");
    size = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
    std::array<char, 4096> actual{};
    require(fcntl(handle, F_GETPATH, actual.data()) != -1 &&
            within(root, fs::canonical(actual.data())), "SFZ opened source escaped its folder");
#else
    require(within(root, fs::canonical("/proc/self/fd/" + std::to_string(handle))),
            "SFZ opened source escaped its folder");
#endif
#endif
    return true;
  }
  std::string read(std::uint64_t offset, std::size_t length) {
    std::vector<char> bytes(length);
    std::size_t done = 0;
#if defined(_WIN32)
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    require(SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0, "SFZ source seek failed");
#endif
    while (done < length) {
#if defined(_WIN32)
      DWORD count = 0;
      require(::ReadFile(handle, bytes.data() + done, static_cast<DWORD>(length - done),
                       &count, nullptr) != 0 && count > 0, "SFZ source changed while reading");
#else
      const auto count = pread(handle, bytes.data() + done, length - done,
                               static_cast<off_t>(offset + done));
      require(count > 0, "SFZ source changed while reading");
#endif
      done += static_cast<std::size_t>(count);
    }
    return choc::base64::encodeToString(bytes);
  }
};
} // namespace

struct SfzLibrary::Impl {
  explicit Impl(fs::path path) : registry(std::move(path)) {}
  fs::path registry;
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<ReadFile>> reads;

  Value perform(std::string_view operation, ValueView payload, const fs::path &folder) {
    if (operation == "closeRead") { reads.erase(identifier(payload, "readId")); return Value(true); }
    if (operation == "readChunk") {
      const auto found = reads.find(identifier(payload, "readId"));
      require(found != reads.end(), "Unknown SFZ read session");
      const auto offset = integer(payload, "offset", 0, maximumBytes);
      const auto length = integer(payload, "length", 1, chunkBytes);
      require(offset <= found->second->size && length <= found->second->size - offset,
              "SFZ chunk exceeds admitted file size");
      auto result = choc::value::createObject({});
      result.addMember("base64", found->second->read(offset, static_cast<std::size_t>(length)));
      return result;
    }
    // The dialog has already closed; no process-shared lock spans a modal dialog.
    RegistryLock lock(registry);
    auto entries = load(registry);
    if (operation == "list") {
      auto result = choc::value::createEmptyArray();
      for (const auto &entry : entries) result.addArrayElement(publicEntry(entry));
      return result;
    }
    if (operation == "remove") {
      const auto id = identifier(payload, "id");
      std::erase_if(entries, [&](const Entry &entry) { return entry.id == id; });
      save(registry, entries); return Value(true);
    }
    if (operation == "select") {
      if (folder.empty()) return {};
      const auto root = fs::canonical(folder);
      require(fs::is_directory(root), "Invalid SFZ selected folder");
      std::vector<fs::path> files;
      for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        require(it.depth() <= 32, "SFZ folder exceeds depth limit");
        bool link = it->is_symlink();
#if defined(_WIN32)
        link = link || (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#endif
        if (link) { it.disable_recursion_pending(); continue; }
        const auto path = fs::canonical(it->path());
        require(within(root, path), "SFZ scan escaped its folder");
        if (it->is_regular_file() && isSfz(path)) {
          if (files.size() >= entryLimit) throw TooLarge();
          files.push_back(path);
        }
      }
      std::sort(files.begin(), files.end());
      auto banks = choc::value::createEmptyArray();
      for (const auto &path : files) {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &entry) {
          return samePath(path, entry.path);
        });
        if (found == entries.end()) {
          if (entries.size() >= entryLimit) throw TooLarge();
          entries.push_back({randomId(), {}, path, root}); found = std::prev(entries.end());
        }
        found->path = path; found->root = root; found->name = utf8(path.lexically_relative(root));
        banks.addArrayElement(publicEntry(*found));
      }
      save(registry, entries);
      auto result = choc::value::createObject({}); result.addMember("banks", std::move(banks)); return result;
    }
    require(operation == "openRead", "Unknown SFZ operation");
    const auto id = identifier(payload, "id");
    const auto limit = integer(payload, "maxBytes", 1, maximumBytes);
    auto relative = payload["relativePath"].getWithDefault<std::string>({});
    require(!relative.empty() && relative.size() <= 32768 && relative.find('\0') == std::string::npos &&
            relative.find(':') == std::string::npos, "Invalid SFZ relative path");
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const auto relativePath = fromUtf8(relative);
    require(!relativePath.is_absolute() && !relativePath.has_root_name() &&
            !relativePath.has_root_directory(), "Absolute SFZ dependency path");
    for (const auto &part : relativePath) require(part != "..", "SFZ parent traversal");
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &entry) { return entry.id == id; });
    require(found != entries.end(), "Unknown SFZ reference");
    std::error_code error;
    const auto root = fs::canonical(found->root, error);
    if (missing(error)) return {};
    require(!error && samePath(root, found->root), "SFZ selected root changed");
    const auto path = fs::canonical(root / relativePath, error);
    if (missing(error)) return {};
    require(!error && within(root, path), "SFZ dependency escaped its folder");
    require(reads.size() < 16, "Too many SFZ read sessions");
    auto file = std::make_unique<ReadFile>();
    if (!file->open(path, root)) return {};
    if (file->size > limit) throw TooLarge();
    auto result = choc::value::createObject({});
    const auto readId = randomId(); result.addMember("readId", readId);
    result.addMember("size", static_cast<std::int64_t>(file->size));
    reads.emplace(readId, std::move(file)); return result;
  }
};

SfzLibrary::SfzLibrary(fs::path registryPath) : impl_(std::make_unique<Impl>(std::move(registryPath))) {}
SfzLibrary::~SfzLibrary() = default;
void SfzLibrary::closeReads() { std::scoped_lock lock(impl_->mutex); impl_->reads.clear(); }
std::string SfzLibrary::request(std::string_view operation, std::string_view payload,
                                const fs::path &selectedFolder) {
  auto result = choc::value::createObject({});
  try {
    require(payload.size() <= 64u * 1024u, "SFZ request exceeds size limit");
    const auto value = choc::json::parse(payload);
    require(value.isObject(), "Invalid SFZ request object");
    std::scoped_lock lock(impl_->mutex);
    auto data = impl_->perform(operation, value, selectedFolder);
    result.addMember("ok", true); result.addMember("data", std::move(data));
  } catch (const std::exception &error) {
    std::fprintf(stderr, "SFZ file reference diagnostic: %.*s: %s\n",
                 static_cast<int>(operation.size()), operation.data(), error.what());
    result.addMember("ok", false);
    result.addMember("code", dynamic_cast<const TooLarge *>(&error) ? "too-large" : "storage-failed");
  }
  return choc::json::toString(result);
}
} // namespace effetune::vst
