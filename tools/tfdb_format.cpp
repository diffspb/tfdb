#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "common.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

namespace {

void usage() {
  std::cerr << "usage: tfdb_format PATH --size BYTES [--partition BYTES] "
               "[--index BYTES] [--block BYTES] [--quantum BYTES] "
               "[--overwrite | --device --yes]\n";
}

// Returns true only on positive evidence that the path is currently mounted.
// A host without /proc/self/mounts simply provides no evidence; --yes remains
// the operator's explicit authorization either way.
bool looks_mounted(const std::string& path) {
  std::ifstream mounts("/proc/self/mounts");
  if (!mounts) return false;
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string source, target;
    if (!(fields >> source >> target)) continue;
    if (source == path || target == path) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { usage(); return 1; }
  const std::string path = argv[1];
  std::uint64_t size = 0;
  bool overwrite = false, device = false, confirmed = false;
  tfdb::VolumeOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--overwrite") overwrite = true;
    else if (argument == "--device") device = true;
    else if (argument == "--yes") confirmed = true;
    else if (i + 1 < argc) {
      std::uint64_t value = 0;
      if (!parse_u64(argv[++i], &value) ||
          (value > UINT32_MAX && argument != "--size" && argument != "--partition")) {
        usage(); return 1;
      }
      if (argument == "--size") size = value;
      else if (argument == "--partition") options.partition_size = value;
      else if (argument == "--index") options.index_region_size = static_cast<std::uint32_t>(value);
      else if (argument == "--block") options.max_block_payload = static_cast<std::uint32_t>(value);
      else if (argument == "--quantum") options.persistence_quantum = static_cast<std::uint32_t>(value);
      else { usage(); return 1; }
    } else { usage(); return 1; }
  }
  if (device && overwrite) { usage(); return 1; }
  if (!device && confirmed) {
    std::cerr << "--yes applies to --device only\n";
    return 1;
  }
  // Formatting a device destroys whatever it held, and the target is usually
  // named on a command line next to the operator's own disks.
  if (device && !confirmed) {
    std::cerr << "refusing to format block device " << path
              << " without --yes\n";
    return 1;
  }
  if (device && looks_mounted(path)) {
    std::cerr << "refusing to format " << path
              << ": it appears in /proc/self/mounts\n";
    return 1;
  }
  std::shared_ptr<tfdb::FileStorage> storage;
  tfdb::Status status;
  if (device) status = tfdb::FileStorage::open_existing(path, true, &storage);
  else {
    if (size == 0) { usage(); return 1; }
    status = tfdb::FileStorage::create(path, size, overwrite, &storage);
  }
  if (!status.ok()) return print_status(status);
  status = tfdb::RingStore::format(*storage, options);
  if (!status.ok()) return print_status(status);
  std::cout << "formatted path=" << path << " bytes=" << storage->size()
            << " partition_bytes=" << options.partition_size
            << " block_payload_bytes=" << options.max_block_payload
            << " persistence_quantum=" << options.persistence_quantum << '\n';
  return 0;
}
