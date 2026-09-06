TEST(framed_record_crc_covers_metadata_and_signed_time) {
  std::vector<std::uint8_t> encoded;
  const char payload[] = {'l', 'o', 'g'};
  REQUIRE_OK(tfdb::FramedRecordV1::encode(
      std::numeric_limits<std::int64_t>::min(), 0x1122334455667788ull,
      7, tfdb::ByteView(payload, sizeof payload), &encoded));
  tfdb::FramedRecordV1 profile;
  std::int64_t decoded_time = 0;
  REQUIRE_OK(profile.decode_block(tfdb::ByteView(encoded),
      [&](const tfdb::RecordView& record) {
        decoded_time = record.index_time_ns;
        REQUIRE_EQ(record.selector, 0x1122334455667788ull);
        REQUIRE_EQ(record.flags, 7u);
        return true;
      }));
  REQUIRE_EQ(decoded_time, std::numeric_limits<std::int64_t>::min());
  std::vector<std::uint8_t> with_trailing_byte = encoded;
  with_trailing_byte.push_back(0);
  REQUIRE(profile.decode_block(tfdb::ByteView(with_trailing_byte),
      [](const tfdb::RecordView&) { return true; }).code() ==
      tfdb::StatusCode::corrupt);
  encoded[8] ^= 1;
  REQUIRE(profile.decode_block(tfdb::ByteView(encoded),
      [](const tfdb::RecordView&) { return true; }).code() ==
      tfdb::StatusCode::corrupt);
}

TEST(time_domains_are_never_compared_implicitly) {
  Fixture fixture;
  fixture.partition.time_domain_id = 2;
  fixture.open();
  const char value = 't';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 10));
  REQUIRE_OK(fixture.store->checkpoint());
  std::size_t count = 0;
  REQUIRE_OK(fixture.store->query_blocks({0, 20, 1}, tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++count;
        return true;
      }));
  REQUIRE_EQ(count, 0u);
  REQUIRE_OK(fixture.store->query_blocks({0, 20, 2}, tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++count;
        return true;
      }));
  REQUIRE_EQ(count, 1u);
  REQUIRE(fixture.store->query_blocks({0, 20, 0}, tfdb::QueryOptions(),
      [](const tfdb::BlockEvent&) { return true; }).code() ==
      tfdb::StatusCode::invalid_argument);
}

TEST(partition_configuration_changes_only_on_rotation) {
  Fixture fixture;
  fixture.open();
  tfdb::PartitionOptions next = fixture.partition;
  next.compression = tfdb::CompressionId::packbits;
  REQUIRE_OK(fixture.store->set_next_partition_options(next));
  tfdb::PartitionInfo active;
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE(active.options.compression == tfdb::CompressionId::none);
  REQUIRE_OK(fixture.store->rotate(next));
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE(active.options.compression == tfdb::CompressionId::packbits);
  fixture.store.reset();
  fixture.open(false);
  REQUIRE(fixture.store->set_next_partition_options(next).code() ==
          tfdb::StatusCode::io_error);
  REQUIRE(fixture.store->writer_status().code() ==
          tfdb::StatusCode::invalid_argument);
}

TEST(typed_append_rejects_profile_change_during_automatic_rotation) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  tfdb::PartitionOptions opaque = fixture.partition;
  opaque.record_format_id = 0;
  opaque.record_format_version = 0;
  REQUIRE_OK(fixture.store->set_next_partition_options(opaque));
  const char payload = 'x';
  for (std::int64_t i = 0; i != 6; ++i) {
    REQUIRE_OK(tfdb::append_framed_record(
        *fixture.store, i, static_cast<std::uint64_t>(i), 0,
        tfdb::ByteView(&payload, 1)));
    REQUIRE_OK(fixture.store->checkpoint());
  }
  REQUIRE(tfdb::append_framed_record(
      *fixture.store, 7, 7, 0, tfdb::ByteView(&payload, 1)).code() ==
      tfdb::StatusCode::invalid_argument);
  tfdb::PartitionInfo active;
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE_EQ(active.options.record_format_id, 0u);
  REQUIRE_EQ(active.block_count, 0u);
}

TEST(time_anomaly_comparison_resets_when_domain_changes) {
  Fixture fixture;
  fixture.partition.allowed_backward_skew_ns = 0;
  fixture.partition.allowed_forward_step_ns = 0;
  fixture.open();
  const char value = 't';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 100));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::PartitionOptions next = fixture.partition;
  next.time_domain_id = 2;
  REQUIRE_OK(fixture.store->rotate(next));
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), -100));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::PartitionInfo active;
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE((active.flags & tfdb::kBlockFlagTimeAnomaly) == 0);
}

TEST(log_payloads_use_the_same_exact_record_query_path) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  const std::string first = "service=drive severity=warn temperature high";
  const std::string second = "service=network severity=info link restored";
  REQUIRE_OK(tfdb::append_framed_record(*fixture.store, 100, 17, 0,
                                       tfdb::ByteView(first.data(), first.size())));
  REQUIRE_OK(tfdb::append_framed_record(*fixture.store, 101, 23, 0,
                                       tfdb::ByteView(second.data(), second.size())));
  REQUIRE_OK(fixture.store->checkpoint());
  const std::vector<std::uint64_t> selectors =
      collect_selectors(*fixture.store, 0, 200, {23});
  REQUIRE(selectors == std::vector<std::uint64_t>({23}));
}

TEST(reader_reports_overwrite_when_rotation_reuses_snapshot_slot) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  std::vector<std::uint8_t> payload(800, 'r');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(payload), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  std::atomic<bool> triggered(false);
  const std::uint64_t first_payload_offset =
      tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize +
      tfdb::internal::kBlockHeaderEncodedSize;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t offset,
                                std::size_t) {
    if (operation != tfdb::StorageOperation::read ||
        point != tfdb::HookPoint::after || offset != first_payload_offset ||
        triggered.exchange(true)) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    std::vector<std::uint8_t> newer(800, 'n');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(newer), 2));
    REQUIRE_OK(fixture.store->checkpoint());
  });
  std::size_t overwrite_gaps = 0, data_events = 0;
  std::uint32_t overwritten_slot = UINT32_MAX;
  std::uint64_t overwritten_generation = 0;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::overwritten_gap) {
          ++overwrite_gaps;
          overwritten_slot = event.metadata.partition_slot;
          overwritten_generation = event.metadata.partition_generation;
        }
        if (event.kind == tfdb::BlockEventKind::data) ++data_events;
        return true;
      }));
  REQUIRE(triggered.load());
  REQUIRE_EQ(overwrite_gaps, 1u);
  REQUIRE_EQ(data_events, 0u);
  REQUIRE_EQ(overwritten_slot, 0u);
  REQUIRE_EQ(overwritten_generation, 1u);

  triggered.store(false);
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::read ||
        point != tfdb::HookPoint::before || triggered.exchange(true)) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
  });
  tfdb::QueryOptions stop_on_gap;
  stop_on_gap.continue_on_gap = false;
  overwrite_gaps = 0;
  const tfdb::Status stopped = fixture.store->scan_blocks(
      stop_on_gap, [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::overwritten_gap)
          ++overwrite_gaps;
        return true;
      });
  REQUIRE(stopped.code() == tfdb::StatusCode::overwritten);
  REQUIRE_EQ(overwrite_gaps, 1u);
}

TEST(reader_reports_unchanged_generation_header_damage_as_corruption) {
  Fixture fixture;
  fixture.open();
  const char payload = 'c';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&payload, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  const std::uint64_t block_header_offset =
      tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize;
  std::atomic<bool> triggered(false);
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t offset, std::size_t) {
    if (operation != tfdb::StorageOperation::read ||
        point != tfdb::HookPoint::before || offset != block_header_offset ||
        triggered.exchange(true)) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    REQUIRE_OK(fixture.storage->corrupt_volatile(
        tfdb::internal::kVolumePrefixSize, 1));
  });
  std::size_t corrupt_gaps = 0, overwritten_gaps = 0, data_events = 0;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        if (event.kind == tfdb::BlockEventKind::overwritten_gap)
          ++overwritten_gaps;
        if (event.kind == tfdb::BlockEventKind::data) ++data_events;
        return true;
      }));
  REQUIRE(triggered.load());
  REQUIRE_EQ(corrupt_gaps, 1u);
  REQUIRE_EQ(overwritten_gaps, 0u);
  REQUIRE_EQ(data_events, 0u);
}

TEST(multiple_readers_remain_safe_during_hot_rotation) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  std::vector<std::uint8_t> initial(800, 0);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(initial), 0));
  REQUIRE_OK(fixture.store->checkpoint());

  std::atomic<bool> stop(false), failed(false);
  std::atomic<std::uint64_t> data_events(0), gap_events(0);
  std::atomic<std::uint64_t> unexpected_gap_events(0), status_errors(0);
  std::atomic<std::uint64_t> bad_sizes(0), mixed_payloads(0);
  std::mutex failure_detail_mutex;
  std::string first_failure_detail;
  std::vector<std::thread> readers;
  for (unsigned reader = 0; reader != 4; ++reader) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const tfdb::Status status = fixture.store->scan_blocks(
            tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
              if (event.kind == tfdb::BlockEventKind::data) {
                ++data_events;
                if (event.data.size() != 800) {
                  ++bad_sizes;
                  failed.store(true);
                }
                if (!event.data.empty()) {
                  const std::uint8_t expected = event.data.data()[0];
                  for (std::size_t i = 1; i != event.data.size(); ++i)
                    if (event.data.data()[i] != expected) {
                      ++mixed_payloads;
                      failed.store(true);
                      break;
                    }
                }
              } else {
                ++gap_events;
                if (event.kind != tfdb::BlockEventKind::overwritten_gap) {
                  ++unexpected_gap_events;
                  std::lock_guard<std::mutex> lock(failure_detail_mutex);
                  if (first_failure_detail.empty())
                    first_failure_detail = std::string("unexpected gap: ") +
                        tfdb::status_code_name(event.detail.code()) + ": " +
                        event.detail.message();
                  failed.store(true);
                }
              }
              return true;
            });
        if (!status.ok()) {
          ++status_errors;
          std::lock_guard<std::mutex> lock(failure_detail_mutex);
          if (first_failure_detail.empty())
            first_failure_detail = std::string("query status: ") +
                tfdb::status_code_name(status.code()) + ": " + status.message();
          failed.store(true);
        }
      }
    });
  }
  for (std::uint16_t ordinal = 1; ordinal <= 100; ++ordinal) {
    std::vector<std::uint8_t> payload(800,
        static_cast<std::uint8_t>(ordinal));
    if (!fixture.store->append(tfdb::ByteView(payload), ordinal).ok() ||
        !fixture.store->checkpoint().ok()) {
      failed.store(true);
      break;
    }
  }
  stop.store(true);
  for (std::thread& reader : readers) reader.join();
  if (unexpected_gap_events.load() != 0 || status_errors.load() != 0)
    throw Failure{first_failure_detail};
  REQUIRE_EQ(status_errors.load(), 0u);
  REQUIRE_EQ(bad_sizes.load(), 0u);
  REQUIRE_EQ(mixed_payloads.load(), 0u);
  REQUIRE(!failed.load());
  REQUIRE(data_events.load() != 0);
  REQUIRE(gap_events.load() <= data_events.load() + 400u);
}
