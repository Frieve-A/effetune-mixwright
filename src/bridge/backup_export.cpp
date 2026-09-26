#include "bridge/backup_export.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include <choc/memory/choc_Base64.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace effetune::vst::bridge {

bool BackupExport::fail(std::string *error, const char *message) {
  cancel();
  if (error != nullptr) *error = message;
  return false;
}

bool BackupExport::begin(const std::filesystem::path &destination,
                         const std::size_t totalBytes, std::string *error) {
  cancel();
  if (totalBytes == 0 || totalBytes > maximumBytes || !destination.is_absolute() ||
      destination.filename().empty()) {
    return fail(error, "Invalid backup export size or destination");
  }
  static std::atomic_uint64_t sequence{0};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const auto candidate = destination.parent_path() /
        (".effetune-backup-" + std::to_string(tick) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::error_code filesystemError;
    if (std::filesystem::create_directory(candidate, filesystemError)) {
      stagingDirectory_ = candidate;
      break;
    }
    if (filesystemError) return fail(error, "Unable to create backup export staging directory");
  }
  if (stagingDirectory_.empty()) return fail(error, "Unable to reserve backup export staging directory");
  stagingFile_ = stagingDirectory_ / "archive.tmp";
  stream_.open(stagingFile_, std::ios::binary | std::ios::trunc);
  if (!stream_) return fail(error, "Unable to open backup export staging file");
  destination_ = destination;
  totalBytes_ = totalBytes;
  writtenBytes_ = 0;
  return true;
}

bool BackupExport::append(const std::size_t offset, const std::string_view base64,
                          std::string *error) {
  if (!stream_.is_open() || offset != writtenBytes_ || base64.empty() ||
      base64.size() > ((chunkBytes + 2) / 3) * 4) {
    return fail(error, "Invalid backup export chunk or offset");
  }
  std::vector<std::uint8_t> bytes;
  if (!choc::base64::decodeToContainer(bytes, base64) || bytes.empty() ||
      bytes.size() > chunkBytes || bytes.size() > totalBytes_ - writtenBytes_) {
    return fail(error, "Invalid backup export chunk data");
  }
  stream_.write(reinterpret_cast<const char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  if (!stream_) return fail(error, "Unable to write backup export chunk");
  writtenBytes_ += bytes.size();
  return true;
}

bool BackupExport::commit(std::string *error) {
  if (!stream_.is_open() || writtenBytes_ != totalBytes_) {
    return fail(error, "Backup export is incomplete");
  }
  stream_.flush();
  const auto flushed = static_cast<bool>(stream_);
  stream_.close();
  if (!flushed || stream_.fail()) return fail(error, "Unable to finish backup export staging file");
#if defined(_WIN32)
  if (MoveFileExW(stagingFile_.c_str(), destination_.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return fail(error, "Unable to replace backup destination");
  }
#else
  std::error_code filesystemError;
  std::filesystem::rename(stagingFile_, destination_, filesystemError);
  if (filesystemError) return fail(error, "Unable to replace backup destination");
#endif
  cancel();
  return true;
}

void BackupExport::cancel() {
  if (stream_.is_open()) stream_.close();
  stream_.clear();
  std::error_code ignored;
  if (!stagingFile_.empty()) std::filesystem::remove(stagingFile_, ignored);
  if (!stagingDirectory_.empty()) std::filesystem::remove(stagingDirectory_, ignored);
  stagingFile_.clear();
  stagingDirectory_.clear();
  destination_.clear();
  totalBytes_ = 0;
  writtenBytes_ = 0;
}

} // namespace effetune::vst::bridge
