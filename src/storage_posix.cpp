#include "tfdb/storage.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tfdb {
namespace {

Status errno_status(const char* operation, int error) {
  StatusCode code = StatusCode::io_error;
  if (error == EINTR) code = StatusCode::interrupted;
  if (error == ENOSPC || error == EDQUOT) code = StatusCode::no_space;
  if (error == EINVAL) code = StatusCode::invalid_argument;
  if (error == EWOULDBLOCK || error == EAGAIN) code = StatusCode::busy;
  std::ostringstream message;
  message << operation << ": " << std::strerror(error);
  return Status::Error(code, message.str());
}

Status lock_writer(int fd) {
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
    return errno_status("exclusive writer lock", errno);
  return Status::Ok();
}

Status sync_parent_directory(const std::string& path) {
  const std::string::size_type slash = path.find_last_of('/');
  const std::string directory = slash == std::string::npos
      ? "." : (slash == 0 ? "/" : path.substr(0, slash));
  int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0) return errno_status("open parent directory", errno);
  Status status;
  while (::fsync(directory_fd) != 0) {
    if (errno == EINTR) continue;
    status = errno_status("fsync parent directory", errno);
    break;
  }
  ::close(directory_fd);
  return status;
}

Status determine_size(int fd, std::uint64_t* output) {
  struct stat info;
  if (::fstat(fd, &info) != 0) return errno_status("fstat", errno);
  if (S_ISBLK(info.st_mode)) {
    std::uint64_t bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      return errno_status("BLKGETSIZE64", errno);
    }
    *output = bytes;
    return Status::Ok();
  }
  if (!S_ISREG(info.st_mode))
    return Status::Error(StatusCode::invalid_argument,
                         "storage must be a regular file or block device");
  if (info.st_size < 0) {
    return Status::Error(StatusCode::out_of_range, "negative storage size");
  }
  *output = static_cast<std::uint64_t>(info.st_size);
  return Status::Ok();
}

}  // namespace

FileStorage::~FileStorage() {
  if (fd_ >= 0) ::close(fd_);
}

Status FileStorage::open_existing(const std::string& path, bool writable,
                                  std::shared_ptr<FileStorage>* output) {
  if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
  const int flags = writable ? O_RDWR | O_CLOEXEC : O_RDONLY | O_CLOEXEC;
  int fd = ::open(path.c_str(), flags);
  if (fd < 0) return errno_status("open", errno);
  if (writable) {
    Status lock_status = lock_writer(fd);
    if (!lock_status.ok()) {
      ::close(fd);
      return lock_status;
    }
  }
  std::uint64_t bytes = 0;
  Status status = determine_size(fd, &bytes);
  if (!status.ok()) {
    ::close(fd);
    return status;
  }
  output->reset(new FileStorage(fd, bytes, writable));
  return Status::Ok();
}

Status FileStorage::create(const std::string& path, std::uint64_t bytes,
                           bool overwrite,
                           std::shared_ptr<FileStorage>* output) {
  if (!output || bytes == 0 || bytes > static_cast<std::uint64_t>(INT64_MAX)) {
    return Status::Error(StatusCode::invalid_argument,
                         "invalid create output or size");
  }
  int flags = O_RDWR | O_CREAT | O_CLOEXEC;
  if (!overwrite) flags |= O_EXCL;
  int fd = ::open(path.c_str(), flags, 0640);
  if (fd < 0) return errno_status("create", errno);
  Status lock_status = lock_writer(fd);
  if (!lock_status.ok()) {
    ::close(fd);
    return lock_status;
  }
  if (overwrite && ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    Status status = errno_status("ftruncate", errno);
    ::close(fd);
    return status;
  }
  int result = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
  if (result != 0) {
    Status status = errno_status("posix_fallocate", result);
    ::close(fd);
    return status;
  }
  while (::fdatasync(fd) != 0) {
    if (errno == EINTR) continue;
    Status status = errno_status("fdatasync created file", errno);
    ::close(fd);
    return status;
  }
  Status directory_status = sync_parent_directory(path);
  if (!directory_status.ok()) {
    ::close(fd);
    return directory_status;
  }
  output->reset(new FileStorage(fd, bytes, true));
  return Status::Ok();
}

IoResult FileStorage::read_at(std::uint64_t offset, MutableByteView output) {
  if (!output.valid())
    return {0, Status::Error(StatusCode::invalid_argument,
                             "null non-empty read buffer")};
  if (offset > size_ || output.size() > size_ - offset ||
      offset > static_cast<std::uint64_t>(INT64_MAX) ||
      output.size() > static_cast<std::size_t>(SSIZE_MAX) ||
      output.size() > static_cast<std::uint64_t>(INT64_MAX) - offset) {
    return {0, Status::Error(StatusCode::out_of_range, "read offset too large")};
  }
  ssize_t result = ::pread(fd_, output.data(), output.size(),
                           static_cast<off_t>(offset));
  if (result < 0) return {0, errno_status("pread", errno)};
  return {static_cast<std::size_t>(result), Status::Ok()};
}

IoResult FileStorage::write_at(std::uint64_t offset, ByteView input) {
  if (!writable_) {
    return {0, Status::Error(StatusCode::io_error, "storage is read-only")};
  }
  if (!input.valid())
    return {0, Status::Error(StatusCode::invalid_argument,
                             "null non-empty write buffer")};
  if (offset > size_ || input.size() > size_ - offset) {
    return {0, Status::Error(StatusCode::no_space, "write outside storage")};
  }
  if (offset > static_cast<std::uint64_t>(INT64_MAX)) {
    return {0, Status::Error(StatusCode::out_of_range, "write offset too large")};
  }
  if (input.size() > static_cast<std::size_t>(SSIZE_MAX) ||
      input.size() > static_cast<std::uint64_t>(INT64_MAX) - offset) {
    return {0, Status::Error(StatusCode::out_of_range,
                             "write length exceeds POSIX range")};
  }
  ssize_t result = ::pwrite(fd_, input.data(), input.size(),
                            static_cast<off_t>(offset));
  if (result < 0) return {0, errno_status("pwrite", errno)};
  return {static_cast<std::size_t>(result), Status::Ok()};
}

Status FileStorage::flush() {
  if (!writable_) return Status::Error(StatusCode::io_error, "storage is read-only");
  while (::fdatasync(fd_) != 0) {
    if (errno == EINTR) continue;
    return errno_status("fdatasync", errno);
  }
  return Status::Ok();
}

Status read_exact(Storage& storage, std::uint64_t offset,
                  MutableByteView output) {
  if (!output.valid())
    return Status::Error(StatusCode::invalid_argument,
                         "null non-empty exact-read buffer");
  std::size_t done = 0;
  while (done < output.size()) {
    IoResult result = storage.read_at(
        offset + done, MutableByteView(output.data() + done, output.size() - done));
    if (!result.status.ok()) {
      if (result.status.code() == StatusCode::interrupted && result.transferred == 0)
        continue;
      return result.status;
    }
    if (result.transferred == 0) {
      return Status::Error(StatusCode::io_error,
                           "read made no progress before requested length");
    }
    if (result.transferred > output.size() - done) {
      return Status::Error(StatusCode::internal_error,
                           "backend over-reported read length");
    }
    done += result.transferred;
  }
  return Status::Ok();
}

Status write_exact(Storage& storage, std::uint64_t offset, ByteView input) {
  if (!input.valid())
    return Status::Error(StatusCode::invalid_argument,
                         "null non-empty exact-write buffer");
  std::size_t done = 0;
  while (done < input.size()) {
    IoResult result = storage.write_at(
        offset + done, ByteView(input.data() + done, input.size() - done));
    if (!result.status.ok()) {
      if (result.status.code() == StatusCode::interrupted && result.transferred == 0)
        continue;
      return result.status;
    }
    if (result.transferred == 0) {
      return Status::Error(StatusCode::io_error, "write made no progress");
    }
    if (result.transferred > input.size() - done) {
      return Status::Error(StatusCode::internal_error,
                           "backend over-reported write length");
    }
    done += result.transferred;
  }
  return Status::Ok();
}

}  // namespace tfdb
