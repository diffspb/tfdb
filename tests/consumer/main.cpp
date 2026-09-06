// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>

#include "tfdb/codec.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/status.hpp"

int main() {
  tfdb::VolumeOptions volume;
  volume.max_block_payload = 1024;
  const tfdb::Status status = tfdb::Status::Ok();
  return status.ok() && tfdb::packbits_codec() &&
                 volume.max_block_payload == 1024
             ? 0
             : 1;
}
