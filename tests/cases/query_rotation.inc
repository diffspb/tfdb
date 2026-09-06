TEST(disordered_time_index_has_no_false_negative) {
  Fixture fixture(128u * 1024u, 70, 512);
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.partition.allowed_backward_skew_ns = 20;
  fixture.partition.allowed_forward_step_ns = 1000;
  fixture.open();
  const std::int64_t times[] = {100, 95, 105, 90, 110, 1000000, 111};
  for (std::uint64_t i = 0; i != 7; ++i) {
    const char payload = static_cast<char>('a' + i);
    REQUIRE_OK(tfdb::append_framed_record(*fixture.store, times[i], i + 1, 0,
                                         tfdb::ByteView(&payload, 1)));
  }
  REQUIRE_OK(fixture.store->checkpoint());
  for (std::uint64_t i = 0; i != 7; ++i) {
    const auto result = collect_selectors(*fixture.store, times[i], times[i] + 1);
    REQUIRE(std::find(result.begin(), result.end(), i + 1) != result.end());
  }
  REQUIRE(fixture.store->metrics().time_anomalies >= 1);
}

TEST(record_query_filters_time_and_selector_in_physical_order) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  struct Input { std::uint64_t selector; std::int64_t time; char payload; };
  const Input inputs[] = {{2, 30, 'a'}, {1, 10, 'b'},
                          {2, 20, 'c'}, {3, 40, 'd'}};
  for (const Input& input : inputs)
    REQUIRE_OK(tfdb::append_framed_record(
        *fixture.store, input.time, input.selector, 0,
        tfdb::ByteView(&input.payload, 1)));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time = {0, 100, 1};
  query.selectors = {2};
  std::vector<char> payloads;
  REQUIRE_OK(tfdb::query_records(*fixture.store, query, profile,
      [&](const tfdb::RecordEvent& event) {
        REQUIRE(event.kind == tfdb::BlockEventKind::data);
        REQUIRE_EQ(event.record.payload.size(), 1u);
        payloads.push_back(static_cast<char>(event.record.payload.data()[0]));
        return true;
      }));
  REQUIRE(payloads == std::vector<char>({'a', 'c'}));
}

TEST(record_profile_mismatch_is_explicit) {
  Fixture fixture;
  fixture.partition.record_format_id = 123;
  fixture.partition.record_format_version = 1;
  fixture.open();
  const char byte = 1;
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time = {0, 10};
  const tfdb::Status status = tfdb::query_records(*fixture.store, query, profile,
      [](const tfdb::RecordEvent&) { return true; });
  REQUIRE(status.code() == tfdb::StatusCode::unsupported);
}

TEST(mixed_record_profiles_require_block_level_dispatch) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  const char framed_payload = 'a';
  REQUIRE_OK(tfdb::append_framed_record(
      *fixture.store, 10, 1, 0, tfdb::ByteView(&framed_payload, 1)));
  REQUIRE_OK(fixture.store->checkpoint());

  tfdb::PartitionOptions version_two = fixture.partition;
  version_two.record_format_version = 2;
  REQUIRE_OK(fixture.store->rotate(version_two));
  const char native_version_two = 'b';
  tfdb::AppendContract contract;
  contract.record_format_id = tfdb::kFramedRecordV1ProfileId;
  contract.record_format_version = 2;
  contract.time_domain_id = 1;
  REQUIRE_OK(fixture.store->append_checked(
      tfdb::ByteView(&native_version_two, 1), 20, contract));
  REQUIRE_OK(fixture.store->checkpoint());

  tfdb::RecordQuery query;
  query.time = {0, 30, 1};
  tfdb::FramedRecordV1 profile;
  std::size_t decoded_v1 = 0;
  REQUIRE(tfdb::query_records(
      *fixture.store, query, profile,
      [&](const tfdb::RecordEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++decoded_v1;
        return true;
      }).code() == tfdb::StatusCode::unsupported);
  REQUIRE_EQ(decoded_v1, 1u);

  std::vector<std::uint16_t> versions;
  REQUIRE_OK(fixture.store->query_blocks(
      query.time, tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
        REQUIRE(event.kind == tfdb::BlockEventKind::data);
        versions.push_back(event.metadata.record_format_version);
        return true;
      }));
  REQUIRE(versions == std::vector<std::uint16_t>({1, 2}));
}

TEST(rotation_reuses_slots_without_resurrecting_old_blocks) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  // Reformat with corrected smaller index region.
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  std::vector<std::uint8_t> payload(800, 0x5a);
  for (int i = 0; i != 30; ++i) {
    payload[0] = static_cast<std::uint8_t>(i);
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(payload), i));
    REQUIRE_OK(fixture.store->checkpoint());
  }
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE(info.partitions.size() <= 2);
  REQUIRE(fixture.store->metrics().rotations >= 2);
  std::vector<int> values;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data) values.push_back(event.data.data()[0]);
    return true;
  }));
  REQUIRE(!values.empty());
  REQUIRE(std::is_sorted(values.begin(), values.end()));
  REQUIRE(values == std::vector<int>({18, 19, 20, 21, 22, 23,
                                      24, 25, 26, 27, 28, 29}));
  REQUIRE(values.back() == 29);
}

TEST(index_reserve_rotates_exactly_at_n_plus_one_entry) {
  Fixture fixture(64u * 1024u, 256, 512, 3);
  fixture.volume.index_region_size = 512;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  const char value = 'i';
  const std::size_t capacity = fixture.volume.index_region_size /
                               tfdb::internal::kIndexEntryEncodedSize;
  for (std::size_t i = 0; i != capacity; ++i) {
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1),
                                     static_cast<std::int64_t>(i)));
    REQUIRE_OK(fixture.store->checkpoint());
  }
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partitions.size(), 1u);
  REQUIRE_EQ(info.partitions[0].block_count, capacity);
  REQUIRE(!info.partitions[0].sealed);

  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 100));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partitions.size(), 2u);
  REQUIRE(info.partitions[0].sealed);
  REQUIRE_EQ(info.partitions[0].block_count, capacity);
  REQUIRE_EQ(info.partitions[1].block_count, 1u);
}

TEST(corrupt_sealed_index_is_rebuilt_from_blocks) {
  Fixture fixture;
  fixture.open();
  const char byte = 'x';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  const std::uint64_t index_absolute = tfdb::internal::kVolumePrefixSize +
      fixture.volume.partition_size - tfdb::internal::kPartitionFooterRegionSize -
      fixture.volume.index_region_size;
  REQUIRE_OK(fixture.storage->corrupt_volatile(index_absolute, 0x80));
  REQUIRE_OK(fixture.storage->corrupt_durable(index_absolute, 0x80));
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks_with_gaps(*fixture.store, 1);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('x'));
}

TEST(historical_footerless_corruption_after_open_is_an_explicit_gap) {
  Fixture fixture;
  fixture.open();
  const char value = 'h';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 10));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  const std::uint64_t footer_offset = tfdb::internal::kVolumePrefixSize +
      fixture.volume.partition_size -
      tfdb::internal::kPartitionFooterRegionSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(footer_offset, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(footer_offset, 1));
  fixture.store.reset();
  fixture.open(false);

  const std::uint64_t payload_offset = tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize +
      tfdb::internal::kBlockHeaderEncodedSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(payload_offset, 1));
  std::size_t data_events = 0, corrupt_gaps = 0;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++data_events;
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        return true;
      }));
  REQUIRE_EQ(data_events, 0u);
  REQUIRE_EQ(corrupt_gaps, 1u);
}

TEST(eager_open_verification_rejects_corrupt_sealed_payload) {
  Fixture fixture;
  fixture.open();
  const char byte = 'p';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  const std::uint64_t payload_offset = tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize +
      tfdb::internal::kBlockHeaderEncodedSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(payload_offset, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(payload_offset, 1));
  fixture.store.reset();

  tfdb::OpenOptions lazy;
  std::unique_ptr<tfdb::RingStore> lazy_store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &lazy_store));
  bool saw_corruption = false;
  REQUIRE_OK(lazy_store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap)
          saw_corruption = true;
        return true;
      }));
  REQUIRE(saw_corruption);
  tfdb::QueryOptions stop_on_gap;
  stop_on_gap.continue_on_gap = false;
  REQUIRE(lazy_store->scan_blocks(stop_on_gap,
      [](const tfdb::BlockEvent&) { return true; }).code() ==
      tfdb::StatusCode::corrupt);
  lazy_store.reset();

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);

}

TEST(eager_open_verification_rejects_valid_crc_wrong_footer_summary) {
  Fixture fixture;
  fixture.open();
  const char value = 'f';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 42));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  fixture.store.reset();

  const std::uint64_t footer_offset = tfdb::internal::kVolumePrefixSize +
      fixture.volume.partition_size -
      tfdb::internal::kPartitionFooterRegionSize;
  std::vector<std::uint8_t> footer_bytes(
      tfdb::internal::kPartitionFooterEncodedSize);
  REQUIRE_OK(tfdb::read_exact(*fixture.storage, footer_offset,
      tfdb::MutableByteView(footer_bytes.data(), footer_bytes.size())));
  tfdb::internal::PartitionFooter footer;
  REQUIRE_OK(tfdb::internal::decode_partition_footer(
      tfdb::ByteView(footer_bytes), &footer));
  footer.min_time_ns = 41;
  footer_bytes = tfdb::internal::encode_partition_footer(footer);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage, footer_offset,
                               tfdb::ByteView(footer_bytes)));
  REQUIRE_OK(fixture.storage->flush());

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict,
                                &strict_store).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(eager_open_verification_exercises_registered_decoder) {
  Fixture fixture;
  fixture.partition.compression = static_cast<tfdb::CompressionId>(100);
  fixture.partition.compression_version = 7;
  tfdb::OpenOptions writer_options;
  writer_options.writable = true;
  writer_options.next_partition = fixture.partition;
  writer_options.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writer_options,
                                   &fixture.store));
  const std::vector<std::uint8_t> repeated(100, 0x5a);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(repeated), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  fixture.store.reset();

  tfdb::OpenOptions lazy;
  lazy.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(
          new RejectingRepeatedByteDecoder()));
  std::unique_ptr<tfdb::RingStore> lazy_store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &lazy_store));
  lazy_store.reset();

  tfdb::OpenOptions strict = lazy;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);

}

TEST(eager_open_verification_decodes_footerless_prefix) {
  Fixture fixture;
  fixture.partition.compression = static_cast<tfdb::CompressionId>(100);
  fixture.partition.compression_version = 7;
  tfdb::OpenOptions writer_options;
  writer_options.writable = true;
  writer_options.next_partition = fixture.partition;
  writer_options.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writer_options,
                                   &fixture.store));
  const std::vector<std::uint8_t> repeated(100, 0x33);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(repeated), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.store.reset();

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  strict.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(
          new RejectingRepeatedByteDecoder()));
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(lazy_read_rejects_codec_not_declared_by_partition) {
  Fixture fixture;
  fixture.partition.compression = static_cast<tfdb::CompressionId>(100);
  fixture.partition.compression_version = 7;
  tfdb::OpenOptions writer_options;
  writer_options.writable = true;
  writer_options.next_partition = fixture.partition;
  writer_options.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writer_options,
                                   &fixture.store));
  const std::vector<std::uint8_t> repeated(100, 0x44);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(repeated), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  fixture.store.reset();

  std::vector<std::uint8_t> partition_bytes(
      tfdb::internal::kPartitionHeaderEncodedSize);
  REQUIRE_OK(tfdb::read_exact(
      *fixture.storage, tfdb::internal::kVolumePrefixSize,
      tfdb::MutableByteView(partition_bytes.data(), partition_bytes.size())));
  tfdb::internal::PartitionHeader header;
  REQUIRE_OK(tfdb::internal::decode_partition_header(
      tfdb::ByteView(partition_bytes), &header));
  header.options.compression = tfdb::CompressionId::none;
  header.options.compression_version = 1;
  partition_bytes = tfdb::internal::encode_partition_header(
      header, fixture.volume.partition_size, fixture.volume.index_region_size);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage,
      tfdb::internal::kVolumePrefixSize, tfdb::ByteView(partition_bytes)));
  REQUIRE_OK(fixture.storage->flush());

  tfdb::OpenOptions lazy;
  lazy.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &store));
  std::size_t corrupt_gaps = 0;
  REQUIRE_OK(store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        return true;
      }));
  REQUIRE_EQ(corrupt_gaps, 1u);
}

TEST(one_valid_volume_header_copy_is_sufficient) {
  Fixture fixture;
  REQUIRE_OK(fixture.storage->corrupt_volatile(0, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(0, 1));
  fixture.open(false);
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
}

TEST(damaged_partition_magic_is_found_from_current_block_evidence) {
  Fixture fixture;
  fixture.open();
  const char value = 'h';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  const std::uint64_t header_offset = tfdb::internal::kVolumePrefixSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(header_offset, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(header_offset, 1));
  fixture.store.reset();

  tfdb::OpenOptions lazy;
  std::unique_ptr<tfdb::RingStore> lazy_store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &lazy_store));
  std::size_t corrupt_gaps = 0;
  REQUIRE_OK(lazy_store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        return true;
      }));
  REQUIRE_EQ(corrupt_gaps, 1u);
  lazy_store.reset();

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);

  tfdb::OpenOptions writable;
  writable.writable = true;
  std::unique_ptr<tfdb::RingStore> writable_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, writable,
                                &writable_store).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(valid_disagreeing_volume_headers_are_corruption) {
  Fixture fixture;
  tfdb::internal::VolumeHeader alternate;
  alternate.volume_id_high = fixture.volume.volume_id_high;
  alternate.volume_id_low = fixture.volume.volume_id_low + 1;
  alternate.volume_size = fixture.storage->size();
  alternate.partition_size = fixture.volume.partition_size;
  alternate.partition_count = 3;
  alternate.index_region_size = fixture.volume.index_region_size;
  alternate.max_block_payload = fixture.volume.max_block_payload;
  alternate.persistence_quantum = fixture.volume.persistence_quantum;
  const auto bytes = tfdb::internal::encode_volume_header(alternate);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage, tfdb::internal::kVolumeHeaderCopySize,
                               tfdb::ByteView(bytes)));
  REQUIRE_OK(fixture.storage->flush());
  tfdb::OpenOptions options;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, options, &store).code() == tfdb::StatusCode::corrupt);
}

TEST(generation_exhaustion_is_explicit_and_preserves_readability) {
  Fixture fixture;
  tfdb::internal::PartitionHeader header;
  header.volume_id_high = fixture.volume.volume_id_high;
  header.volume_id_low = fixture.volume.volume_id_low;
  header.generation = UINT64_MAX;
  header.slot = 0;
  header.max_block_payload = fixture.volume.max_block_payload;
  header.persistence_quantum = fixture.volume.persistence_quantum;
  header.options = fixture.partition;
  const std::vector<std::uint8_t> bytes =
      tfdb::internal::encode_partition_header(
          header, fixture.volume.partition_size,
          fixture.volume.index_region_size);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage,
      tfdb::internal::kVolumePrefixSize, tfdb::ByteView(bytes)));
  REQUIRE_OK(fixture.storage->flush());
  fixture.open();
  REQUIRE(fixture.store->rotate(fixture.partition).code() ==
          tfdb::StatusCode::generation_exhausted);
  fixture.store.reset();
  fixture.open(false);
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partitions.size(), 1u);
  REQUIRE_EQ(info.partitions[0].generation, UINT64_MAX);
}
