// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

TEST(incremental_crc32c_matches_one_shot_and_zeroed_field) {
  const std::string text = "123456789";
  REQUIRE_EQ(tfdb::internal::crc32c(
                 tfdb::ByteView(text.data(), text.size())),
             0xe3069283u);

  // Splitting an input at every boundary must not change the result.
  for (std::size_t split = 0; split <= text.size(); ++split) {
    std::uint32_t state = tfdb::internal::crc32c_update(
        tfdb::internal::kCrc32cInit, tfdb::ByteView(text.data(), split));
    state = tfdb::internal::crc32c_update(
        state, tfdb::ByteView(text.data() + split, text.size() - split));
    REQUIRE_EQ(tfdb::internal::crc32c_finish(state), 0xe3069283u);
  }

  // crc32c_with_zeroed_field must equal copying and zeroing the field, which
  // is what the decoders did before, at every legal field offset.
  std::vector<std::uint8_t> frame(64);
  for (std::size_t i = 0; i != frame.size(); ++i)
    frame[i] = static_cast<std::uint8_t>(i * 7 + 1);
  for (std::size_t offset = 0; offset + 4 <= frame.size(); ++offset) {
    std::vector<std::uint8_t> copy = frame;
    for (unsigned i = 0; i != 4; ++i) copy[offset + i] = 0;
    REQUIRE_EQ(tfdb::internal::crc32c_with_zeroed_field(
                   tfdb::ByteView(frame), offset),
               tfdb::internal::crc32c(tfdb::ByteView(copy)));
  }
}

TEST(library_version_header_and_binary_agree) {
  // Catches a stale libtfdb.a linked against freshly updated headers, and a
  // TFDB_VERSION_STRING that drifted away from its numeric components. The
  // VERSION file, CMake, and Meson are cross-checked in tool_smoke.sh.
  REQUIRE_EQ(std::string(tfdb::library_version()),
             std::string(TFDB_VERSION_STRING));
  std::ostringstream composed;
  composed << TFDB_VERSION_MAJOR << '.' << TFDB_VERSION_MINOR << '.'
           << TFDB_VERSION_PATCH;
  REQUIRE_EQ(std::string(TFDB_VERSION_STRING), composed.str());
  REQUIRE(TFDB_VERSION_NUMBER >= 10000);
}

TEST(crc32c_known_vector) {
  const char text[] = "123456789";
  REQUIRE_EQ(tfdb::internal::crc32c(tfdb::ByteView(text, 9)), 0xe3069283u);
}

TEST(all_status_codes_have_stable_names) {
  const tfdb::StatusCode codes[] = {
      tfdb::StatusCode::ok, tfdb::StatusCode::invalid_argument,
      tfdb::StatusCode::out_of_range, tfdb::StatusCode::io_error,
      tfdb::StatusCode::interrupted, tfdb::StatusCode::no_space,
      tfdb::StatusCode::corrupt, tfdb::StatusCode::unsupported,
      tfdb::StatusCode::not_found, tfdb::StatusCode::busy,
      tfdb::StatusCode::overwritten, tfdb::StatusCode::generation_exhausted,
      tfdb::StatusCode::closed, tfdb::StatusCode::internal_error};
  for (tfdb::StatusCode code : codes)
    REQUIRE(std::string(tfdb::status_code_name(code)) != "unknown");
  REQUIRE(std::string(tfdb::status_code_name(
      static_cast<tfdb::StatusCode>(999))) == "unknown");
}

TEST(format_v1_normative_golden_vectors) {
  tfdb::internal::VolumeHeader volume;
  volume.volume_id_high = 0x0102030405060708ull;
  volume.volume_id_low = 0x1112131415161718ull;
  volume.volume_size = 0x102000;
  volume.partition_size = 0x80000;
  volume.partition_count = 2;
  volume.index_region_size = 4096;
  volume.max_block_payload = 32768;
  volume.persistence_quantum = 4096;
  volume.created_time_ns = -2;
  std::vector<std::uint8_t> expected_volume(256, 0);
  golden_magic(&expected_volume, "TFDBVOL1");
  golden_put16(&expected_volume, 8, 1);
  golden_put32(&expected_volume, 12, 256);
  golden_put64(&expected_volume, 16, volume.volume_id_high);
  golden_put64(&expected_volume, 24, volume.volume_id_low);
  golden_put64(&expected_volume, 32, volume.volume_size);
  golden_put64(&expected_volume, 40, volume.partition_size);
  golden_put32(&expected_volume, 48, volume.partition_count);
  golden_put32(&expected_volume, 52, volume.index_region_size);
  golden_put32(&expected_volume, 56, volume.max_block_payload);
  golden_put32(&expected_volume, 60, volume.persistence_quantum);
  golden_put64(&expected_volume, 64, UINT64_MAX - 1);
  const std::uint8_t volume_crc[] = {0x4c, 0xee, 0x84, 0x05};
  std::copy(volume_crc, volume_crc + 4, expected_volume.begin() + 72);
  REQUIRE(tfdb::internal::encode_volume_header(volume) == expected_volume);

  tfdb::internal::PartitionHeader partition;
  partition.volume_id_high = volume.volume_id_high;
  partition.volume_id_low = volume.volume_id_low;
  partition.generation = 7;
  partition.slot = 1;
  partition.created_time_ns = -3;
  partition.max_block_payload = volume.max_block_payload;
  partition.persistence_quantum = volume.persistence_quantum;
  partition.options.compression = tfdb::CompressionId::packbits;
  partition.options.compression_version = 1;
  partition.options.record_format_id = tfdb::kFramedRecordV1ProfileId;
  partition.options.record_format_version = 1;
  partition.options.time_domain_id = 2;
  partition.options.allowed_backward_skew_ns = 10;
  partition.options.allowed_forward_step_ns = 20;
  std::vector<std::uint8_t> expected_partition(1024, 0);
  golden_magic(&expected_partition, "TFDBPAR1");
  golden_put16(&expected_partition, 8, 1);
  golden_put32(&expected_partition, 12, 1024);
  golden_put64(&expected_partition, 16, volume.volume_id_high);
  golden_put64(&expected_partition, 24, volume.volume_id_low);
  golden_put64(&expected_partition, 32, 7);
  golden_put32(&expected_partition, 40, 1);
  golden_put64(&expected_partition, 48, UINT64_MAX - 2);
  golden_put32(&expected_partition, 56, 32768);
  golden_put32(&expected_partition, 60, 4096);
  golden_put64(&expected_partition, 64, 10);
  golden_put64(&expected_partition, 72, 20);
  golden_put64(&expected_partition, 80, 2);
  golden_put16(&expected_partition, 88, 4);
  golden_put16(&expected_partition, 90, 40);
  golden_put32(&expected_partition, 92, 128);
  golden_put32(&expected_partition, 96, 512);
  golden_put32(&expected_partition, 100, 16);
  const std::uint8_t partition_crc[] = {0xa9, 0xac, 0xea, 0xd7};
  std::copy(partition_crc, partition_crc + 4,
            expected_partition.begin() + 104);
  // compression, min/max time index, record profile, CRC32C integrity
  golden_put16(&expected_partition, 128, 1); golden_put16(&expected_partition, 130, 1);
  golden_put32(&expected_partition, 132, 1); golden_put16(&expected_partition, 136, 1);
  golden_put16(&expected_partition, 168, 2); golden_put16(&expected_partition, 170, 1);
  golden_put32(&expected_partition, 172, 1); golden_put16(&expected_partition, 176, 1);
  golden_put64(&expected_partition, 192, 0x7e000); golden_put64(&expected_partition, 200, 4096);
  golden_put16(&expected_partition, 208, 3); golden_put16(&expected_partition, 216, 1);
  golden_put32(&expected_partition, 220, 512); golden_put32(&expected_partition, 224, 8);
  golden_put16(&expected_partition, 248, 4); golden_put16(&expected_partition, 250, 1);
  golden_put32(&expected_partition, 252, 1); golden_put16(&expected_partition, 256, 1);
  golden_put64(&expected_partition, 512, tfdb::kFramedRecordV1ProfileId);
  REQUIRE(tfdb::internal::encode_partition_header(partition, 0x80000, 4096) ==
          expected_partition);

  const std::vector<std::uint8_t> expected_block = from_hex(
      "54464442424c4b31010000008000000008070605040302010200000003000000"
      "04000000050000000600000000020000feffffffffffffff0900000000000000"
      "01000100443322114c9b18220d0c0b0a80706050403020101122334455667788"
      "0202020201010101040404040303030306060606050505050808080807070707");
  tfdb::internal::BlockHeader block;
  block.generation = 0x0102030405060708ull; block.slot = 0x0a0b0c0d;
  block.sequence = 2; block.flags = 3; block.record_count = 4;
  block.stored_size = 5; block.raw_size = 6; block.frame_span = 512;
  block.min_time_ns = -2; block.max_time_ns = 9;
  block.compression = tfdb::CompressionId::packbits;
  block.compression_version = 1; block.payload_crc = 0x11223344;
  block.volume_id_low = 0x1020304050607080ull;
  block.volume_id_high = 0x8877665544332211ull;
  block.writer_id_high = 0x0101010102020202ull;
  block.writer_id_low = 0x0303030304040404ull;
  block.previous_writer_id_high = 0x0505050506060606ull;
  block.previous_writer_id_low = 0x0707070708080808ull;
  REQUIRE(tfdb::internal::encode_block_header(block) == expected_block);

  tfdb::internal::IndexEntry index;
  index.offset = 4096; index.frame_size = 133; index.frame_span = 512;
  index.sequence = 2; index.record_count = 4; index.min_time_ns = -2;
  index.max_time_ns = 9; index.flags = 3; index.raw_size = 6;
  std::vector<std::uint8_t> index_bytes(48, 0);
  tfdb::internal::encode_index_entry(index, index_bytes.data());
  REQUIRE(index_bytes == from_hex(
      "001000000000000085000000000200000200000004000000feffffffffffffff"
      "09000000000000000300000006000000"));

  tfdb::internal::PartitionFooter footer;
  footer.volume_id_high = volume.volume_id_high;
  footer.volume_id_low = volume.volume_id_low;
  footer.generation = 7; footer.slot = 1; footer.flags = 3;
  footer.block_count = 1; footer.index_offset = 0x7e000;
  footer.index_size = 48; footer.data_end = 4608;
  footer.min_time_ns = -2; footer.max_time_ns = 9;
  footer.index_crc = 0x12345678;
  std::vector<std::uint8_t> expected_footer(256, 0);
  golden_magic(&expected_footer, "TFDBFTR1");
  golden_put16(&expected_footer, 8, 1); golden_put32(&expected_footer, 12, 256);
  golden_put64(&expected_footer, 16, volume.volume_id_high);
  golden_put64(&expected_footer, 24, volume.volume_id_low);
  golden_put64(&expected_footer, 32, 7); golden_put32(&expected_footer, 40, 1);
  golden_put32(&expected_footer, 44, 3); golden_put32(&expected_footer, 48, 1);
  golden_put32(&expected_footer, 52, 48); golden_put64(&expected_footer, 56, 0x7e000);
  golden_put32(&expected_footer, 64, 48); golden_put64(&expected_footer, 72, 4608);
  golden_put64(&expected_footer, 80, UINT64_MAX - 1); golden_put64(&expected_footer, 88, 9);
  golden_put32(&expected_footer, 96, 0x12345678);
  const std::uint8_t footer_crc[] = {0xba, 0xbb, 0x5b, 0x38};
  std::copy(footer_crc, footer_crc + 4, expected_footer.begin() + 100);
  REQUIRE(tfdb::internal::encode_partition_footer(footer) == expected_footer);

  std::vector<std::uint8_t> framed;
  const char framed_payload[] = {'l', 'o', 'g'};
  REQUIRE_OK(tfdb::FramedRecordV1::encode(
      std::numeric_limits<std::int64_t>::min(), 0x1122334455667788ull, 7,
      tfdb::ByteView(framed_payload, sizeof framed_payload), &framed));
  REQUIRE(framed == from_hex(
      "23000000200007000000000000000080887766554433221103000000e8c1198a"
      "6c6f67"));
}

TEST(persistent_decoders_reject_truncation_and_survive_mutation) {
  tfdb::internal::BlockHeader block;
  block.generation = 1; block.slot = 0; block.record_count = 1;
  block.stored_size = 1; block.raw_size = 1; block.frame_span = 512;
  block.min_time_ns = -1; block.max_time_ns = 1;
  block.volume_id_high = 1; block.volume_id_low = 2;
  block.writer_id_high = 3; block.writer_id_low = 4;
  const std::vector<std::uint8_t> original =
      tfdb::internal::encode_block_header(block);
  for (std::size_t length = 0; length != original.size(); ++length) {
    tfdb::internal::BlockHeader decoded;
    REQUIRE(!tfdb::internal::decode_block_header(
        tfdb::ByteView(original.data(), length), &decoded).ok());
  }
  std::mt19937 random(0xdec0de);
  for (unsigned trial = 0; trial != 10000; ++trial) {
    std::vector<std::uint8_t> mutated = original;
    const std::size_t offset = random() % mutated.size();
    mutated[offset] ^= static_cast<std::uint8_t>(1u << (random() % 8));
    tfdb::internal::BlockHeader decoded;
    const tfdb::Status status = tfdb::internal::decode_block_header(
        tfdb::ByteView(mutated), &decoded);
    REQUIRE(!status.ok());
  }
  const std::shared_ptr<const tfdb::CompressionCodec> codec =
      tfdb::packbits_codec();
  for (unsigned trial = 0; trial != 10000; ++trial) {
    const std::size_t size = random() % 64;
    std::vector<std::uint8_t> bytes(size);
    for (std::uint8_t& byte : bytes)
      byte = static_cast<std::uint8_t>(random());
    std::vector<std::uint8_t> output;
    const tfdb::Status status = codec->decompress(
        tfdb::ByteView(bytes), random() % 256, &output);
    if (status.ok()) REQUIRE(output.size() <= 255);
  }
}

TEST(all_persistent_decoders_have_truncation_and_crc_mutation_coverage) {
  tfdb::internal::VolumeHeader volume;
  volume.volume_id_high = 1; volume.volume_id_low = 2;
  volume.volume_size = tfdb::internal::kVolumePrefixSize + 2u * 65536u;
  volume.partition_size = 65536;
  volume.partition_count = 2;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 512;
  const std::vector<std::uint8_t> volume_bytes =
      tfdb::internal::encode_volume_header(volume);

  tfdb::internal::PartitionHeader partition;
  partition.volume_id_high = 1; partition.volume_id_low = 2;
  partition.generation = 1; partition.slot = 0;
  partition.max_block_payload = 256;
  partition.persistence_quantum = 512;
  const std::vector<std::uint8_t> partition_bytes =
      tfdb::internal::encode_partition_header(partition, 65536, 4096);

  tfdb::internal::BlockHeader block;
  block.generation = 1; block.slot = 0; block.record_count = 1;
  block.stored_size = 1; block.raw_size = 1; block.frame_span = 512;
  block.writer_id_high = 3; block.writer_id_low = 4;
  block.volume_id_high = 1; block.volume_id_low = 2;
  const std::vector<std::uint8_t> block_bytes =
      tfdb::internal::encode_block_header(block);

  tfdb::internal::PartitionFooter footer;
  footer.volume_id_high = 1; footer.volume_id_low = 2;
  footer.generation = 1; footer.slot = 0;
  footer.index_offset = 57344; footer.data_end = 4096;
  const std::vector<std::uint8_t> footer_bytes =
      tfdb::internal::encode_partition_footer(footer);

  const auto exercise_crc_structure = [](
      const std::vector<std::uint8_t>& bytes,
      const std::function<tfdb::Status(tfdb::ByteView)>& decode) {
    for (std::size_t length = 0; length != bytes.size(); ++length)
      REQUIRE(!decode(tfdb::ByteView(bytes.data(), length)).ok());
    for (std::size_t offset = 0; offset != bytes.size(); ++offset) {
      std::vector<std::uint8_t> mutated = bytes;
      mutated[offset] ^= 1;
      REQUIRE(!decode(tfdb::ByteView(mutated)).ok());
    }
    REQUIRE_OK(decode(tfdb::ByteView(bytes)));
  };
  exercise_crc_structure(volume_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::VolumeHeader value;
    return tfdb::internal::decode_volume_header(bytes, &value);
  });
  exercise_crc_structure(partition_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::PartitionHeader value;
    return tfdb::internal::decode_partition_header(bytes, &value);
  });
  exercise_crc_structure(block_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::BlockHeader value;
    return tfdb::internal::decode_block_header(bytes, &value);
  });
  exercise_crc_structure(footer_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::PartitionFooter value;
    return tfdb::internal::decode_partition_footer(bytes, &value);
  });

  tfdb::internal::IndexEntry index;
  index.offset = 4096; index.frame_size = 129; index.frame_span = 512;
  index.record_count = 1;
  std::vector<std::uint8_t> index_bytes(
      tfdb::internal::kIndexEntryEncodedSize);
  tfdb::internal::encode_index_entry(index, index_bytes.data());
  for (std::size_t length = 0; length != index_bytes.size(); ++length) {
    tfdb::internal::IndexEntry decoded;
    REQUIRE(!tfdb::internal::decode_index_entry(
        tfdb::ByteView(index_bytes.data(), length), &decoded).ok());
  }
  tfdb::internal::IndexEntry decoded_index;
  REQUIRE_OK(tfdb::internal::decode_index_entry(tfdb::ByteView(index_bytes),
                                                 &decoded_index));

  std::vector<std::uint8_t> frame;
  const char payload[] = {'a', '\0', 'b'};
  REQUIRE_OK(tfdb::FramedRecordV1::encode(1, 2, 3,
      tfdb::ByteView(payload, sizeof payload), &frame));
  tfdb::FramedRecordV1 profile;
  for (std::size_t length = 1; length != frame.size(); ++length)
    REQUIRE(!profile.decode_block(tfdb::ByteView(frame.data(), length),
        [](const tfdb::RecordView&) { return true; }).ok());
  for (std::size_t offset = 0; offset != frame.size(); ++offset) {
    std::vector<std::uint8_t> mutated = frame;
    mutated[offset] ^= 1;
    REQUIRE(!profile.decode_block(tfdb::ByteView(mutated),
        [](const tfdb::RecordView&) { return true; }).ok());
  }
}

TEST(partition_feature_directory_rejects_width_duplicates_and_profile_mismatch) {
  tfdb::internal::PartitionHeader partition;
  partition.volume_id_high = 1; partition.volume_id_low = 2;
  partition.generation = 1; partition.max_block_payload = 256;
  partition.persistence_quantum = 512;
  const auto with_crc = [](std::vector<std::uint8_t> bytes) {
    golden_put32(&bytes, 104, 0);
    golden_put32(&bytes, 104, tfdb::internal::crc32c(tfdb::ByteView(bytes)));
    return bytes;
  };
  std::vector<std::uint8_t> bytes =
      tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 132, 65536);
  bytes = with_crc(bytes);
  tfdb::internal::PartitionHeader decoded;
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put16(&bytes, 168, tfdb::internal::kFeatureCompression);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 92, 129);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 96, 520);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 220, 520);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  partition.options.record_format_version = 1;
  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(packbits_round_trip_and_bounds) {
  const std::shared_ptr<const tfdb::CompressionCodec> codec = tfdb::packbits_codec();
  std::vector<std::uint8_t> input;
  input.insert(input.end(), 300, 0);
  for (unsigned i = 0; i != 256; ++i) input.push_back(static_cast<std::uint8_t>(i));
  input.insert(input.end(), 17, 42);
  std::vector<std::uint8_t> compressed, decoded;
  REQUIRE_OK(codec->compress(tfdb::ByteView(input), &compressed));
  REQUIRE(compressed.size() < input.size());
  REQUIRE_OK(codec->decompress(tfdb::ByteView(compressed), input.size(), &decoded));
  REQUIRE(input == decoded);
  compressed.push_back(0x80);
  REQUIRE(!codec->decompress(tfdb::ByteView(compressed), input.size(), &decoded).ok());
}

TEST(public_codecs_and_profiles_reject_invalid_byte_views) {
  const tfdb::ByteView invalid(static_cast<const void*>(nullptr), 1);
  std::vector<std::uint8_t> output;
  const std::shared_ptr<const tfdb::CompressionCodec> none =
      tfdb::no_compression_codec();
  const std::shared_ptr<const tfdb::CompressionCodec> packbits =
      tfdb::packbits_codec();
  REQUIRE(none->compress(invalid, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(none->decompress(invalid, 1, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(packbits->compress(invalid, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(packbits->decompress(invalid, 1, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(tfdb::FramedRecordV1::encode(0, 0, 0, invalid, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  tfdb::FramedRecordV1 profile;
  REQUIRE(profile.decode_block(
      invalid, [](const tfdb::RecordView&) { return true; }).code() ==
      tfdb::StatusCode::invalid_argument);
}

TEST(public_encoders_support_aliased_input_and_output) {
  const std::shared_ptr<const tfdb::CompressionCodec> none =
      tfdb::no_compression_codec();
  std::vector<std::uint8_t> raw = {1, 2, 3, 4};
  REQUIRE_OK(none->compress(tfdb::ByteView(raw), &raw));
  REQUIRE(raw == std::vector<std::uint8_t>({1, 2, 3, 4}));
  REQUIRE_OK(none->decompress(tfdb::ByteView(raw), raw.size(), &raw));
  REQUIRE(raw == std::vector<std::uint8_t>({1, 2, 3, 4}));

  const std::shared_ptr<const tfdb::CompressionCodec> packbits =
      tfdb::packbits_codec();
  raw.assign(130, 0x5a);
  const std::vector<std::uint8_t> expected_raw = raw;
  REQUIRE_OK(packbits->compress(tfdb::ByteView(raw), &raw));
  REQUIRE(raw == std::vector<std::uint8_t>({0xff, 0x5a}));
  REQUIRE_OK(packbits->decompress(tfdb::ByteView(raw), expected_raw.size(),
                                  &raw));
  REQUIRE(raw == expected_raw);

  std::vector<std::uint8_t> framed_payload = {'a', 'l', 'i', 'a', 's'};
  const std::vector<std::uint8_t> expected_payload = framed_payload;
  REQUIRE_OK(tfdb::FramedRecordV1::encode(
      -5, 42, 7, tfdb::ByteView(framed_payload), &framed_payload));
  tfdb::FramedRecordV1 profile;
  std::vector<std::uint8_t> decoded_payload;
  REQUIRE_OK(profile.decode_block(
      tfdb::ByteView(framed_payload), [&](const tfdb::RecordView& record) {
        decoded_payload.assign(record.payload.data(),
                               record.payload.data() + record.payload.size());
        return true;
      }));
  REQUIRE(decoded_payload == expected_payload);
}

TEST(packbits_v1_normative_and_malformed_vectors) {
  const std::shared_ptr<const tfdb::CompressionCodec> codec =
      tfdb::packbits_codec();
  const auto encodes_as = [&](const std::vector<std::uint8_t>& input,
                              const std::vector<std::uint8_t>& expected) {
    std::vector<std::uint8_t> encoded, decoded;
    REQUIRE_OK(codec->compress(tfdb::ByteView(input), &encoded));
    REQUIRE(encoded == expected);
    REQUIRE_OK(codec->decompress(tfdb::ByteView(encoded), input.size(),
                                 &decoded));
    REQUIRE(decoded == input);
  };
  encodes_as({}, {});
  encodes_as({1, 2}, {1, 1, 2});
  encodes_as({0xaa, 0xaa, 0xaa}, {0x80, 0xaa});
  encodes_as({0x41, 0x41, 0x42, 0x42, 0x42, 0x43},
             {1, 0x41, 0x41, 0x80, 0x42, 0, 0x43});
  encodes_as(std::vector<std::uint8_t>(130, 0x7e), {0xff, 0x7e});
  encodes_as(std::vector<std::uint8_t>(131, 0x7e),
             {0xff, 0x7e, 0, 0x7e});

  const auto rejects = [&](const std::vector<std::uint8_t>& input,
                           std::size_t expected_size) {
    std::vector<std::uint8_t> output;
    REQUIRE(codec->decompress(tfdb::ByteView(input), expected_size,
                              &output).code() == tfdb::StatusCode::corrupt);
  };
  rejects({0}, 1);
  rejects({2, 1, 2}, 3);
  rejects({0x80}, 3);
  rejects({0x80, 1}, 2);
  rejects({0, 1, 0, 2}, 1);
}

TEST(format_headers_and_geometry_round_trip) {
  Fixture fixture;
  fixture.open(false);
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partition_count, 3u);
  REQUIRE_EQ(info.partition_size, fixture.volume.partition_size);
  REQUIRE_EQ(info.persistence_quantum, 512u);
  REQUIRE(info.partitions.empty());
}

TEST(format_accepts_exactly_one_quantum_data_region) {
  tfdb::VolumeOptions volume;
  volume.persistence_quantum = 256;
  volume.index_region_size = 256;
  volume.max_block_payload = 1;
  volume.partition_size = tfdb::internal::kPartitionHeaderRegionSize +
                          tfdb::internal::kPartitionFooterRegionSize +
                          volume.index_region_size +
                          volume.persistence_quantum;
  volume.volume_id_high = 1;
  volume.volume_id_low = 2;
  volume.allow_explicit_volume_id_for_testing = true;
  const std::uint64_t total_size = tfdb::internal::kVolumePrefixSize +
                                   2 * volume.partition_size;
  std::shared_ptr<tfdb::MemoryStorage> storage(
      new tfdb::MemoryStorage(total_size));
  REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  tfdb::OpenOptions open;
  open.writable = true;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(storage, open, &store));
  const std::uint8_t first = 0x11;
  const std::uint8_t second = 0x22;
  REQUIRE_OK(store->append(tfdb::ByteView(&first, 1), 1));
  REQUIRE_OK(store->checkpoint());
  REQUIRE_OK(store->append(tfdb::ByteView(&second, 1), 2));
  REQUIRE_OK(store->checkpoint());
  REQUIRE_OK(store->close());
  store.reset();
  storage->crash_discard_volatile();
  open.writable = false;
  REQUIRE_OK(tfdb::RingStore::open(storage, open, &store));
  const std::vector<std::vector<std::uint8_t>> blocks =
      collect_blocks(*store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0].size(), 1u);
  REQUIRE_EQ(blocks[0][0], first);
  REQUIRE_EQ(blocks[1].size(), 1u);
  REQUIRE_EQ(blocks[1][0], second);
}
