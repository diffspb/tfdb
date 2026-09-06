TEST(append_checkpoint_exact_io_and_visibility) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  const std::uint8_t message[] = {1, 2, 3, 4};
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(message, sizeof message), 10));
  tfdb::StorageCounters counters = fixture.storage->counters();
  REQUIRE_EQ(counters.write_calls, 0u);
  REQUIRE_EQ(counters.flush_calls, 0u);
  REQUIRE(collect_blocks(*fixture.store).empty());
  REQUIRE_OK(fixture.store->checkpoint());
  counters = fixture.storage->counters();
  REQUIRE_EQ(counters.write_calls, 1u);
  REQUIRE_EQ(counters.flush_calls, 1u);
  const std::vector<tfdb::StorageEvent> all_checkpoint_events =
      fixture.storage->events();
  const auto checkpoint_event = std::find_if(
      all_checkpoint_events.begin(), all_checkpoint_events.end(),
      [](const tfdb::StorageEvent& event) {
        return event.operation == tfdb::StorageOperation::write;
      });
  REQUIRE(checkpoint_event != all_checkpoint_events.end());
  REQUIRE_EQ(checkpoint_event->requested,
             tfdb::internal::kBlockHeaderEncodedSize + sizeof message);
  REQUIRE_EQ(checkpoint_event->transferred, checkpoint_event->requested);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE(blocks[0] == std::vector<std::uint8_t>(message, message + sizeof message));
  fixture.storage->reset_counters();
  REQUIRE_OK(fixture.store->checkpoint());
  counters = fixture.storage->counters();
  REQUIRE_EQ(counters.write_calls, 0u);
  REQUIRE_EQ(counters.flush_calls, 0u);
}

TEST(format_open_and_seal_have_exact_write_sequences) {
  const std::uint64_t partition_size = 64u * 1024u;
  std::shared_ptr<tfdb::MemoryStorage> storage(new tfdb::MemoryStorage(
      tfdb::internal::kVolumePrefixSize + partition_size * 3));
  tfdb::VolumeOptions volume;
  volume.partition_size = partition_size;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 512;
  volume.volume_id_high = 1;
  volume.volume_id_low = 2;
  volume.allow_explicit_volume_id_for_testing = true;
  storage->reset_counters();
  REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  std::vector<tfdb::StorageEvent> events = storage->events();
  REQUIRE_EQ(events.size(), 3u);
  REQUIRE(events[0].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[0].offset, 0u);
  REQUIRE_EQ(events[0].requested,
             tfdb::internal::kVolumeHeaderEncodedSize);
  REQUIRE_EQ(events[0].transferred, events[0].requested);
  REQUIRE(events[1].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[1].offset, tfdb::internal::kVolumeHeaderCopySize);
  REQUIRE_EQ(events[1].requested,
             tfdb::internal::kVolumeHeaderEncodedSize);
  REQUIRE_EQ(events[1].transferred, events[1].requested);
  REQUIRE(events[2].operation == tfdb::StorageOperation::flush);

  storage->reset_counters();
  tfdb::OpenOptions open;
  open.writable = true;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(storage, open, &store));
  events = storage->events();
  REQUIRE(events.size() >= 2);
  REQUIRE(events[events.size() - 2].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[events.size() - 2].offset,
             tfdb::internal::kVolumePrefixSize);
  REQUIRE_EQ(events[events.size() - 2].requested,
             tfdb::internal::kPartitionHeaderEncodedSize);
  REQUIRE_EQ(events[events.size() - 2].transferred,
             events[events.size() - 2].requested);
  REQUIRE(events.back().operation == tfdb::StorageOperation::flush);

  const char value = 'w';
  REQUIRE_OK(store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(store->checkpoint());
  storage->reset_counters();
  REQUIRE_OK(store->rotate(tfdb::PartitionOptions()));
  events = storage->events();
  REQUIRE_EQ(events.size(), 5u);
  const std::uint64_t index_offset = tfdb::internal::kVolumePrefixSize +
      partition_size - tfdb::internal::kPartitionFooterRegionSize -
      volume.index_region_size;
  const std::uint64_t footer_offset = tfdb::internal::kVolumePrefixSize +
      partition_size - tfdb::internal::kPartitionFooterRegionSize;
  REQUIRE(events[0].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[0].offset, index_offset);
  REQUIRE_EQ(events[0].requested, tfdb::internal::kIndexEntryEncodedSize);
  REQUIRE_EQ(events[0].transferred, events[0].requested);
  REQUIRE(events[1].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[1].offset, footer_offset);
  REQUIRE_EQ(events[1].requested,
             tfdb::internal::kPartitionFooterEncodedSize);
  REQUIRE_EQ(events[1].transferred, events[1].requested);
  REQUIRE(events[2].operation == tfdb::StorageOperation::flush);
  REQUIRE(events[3].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[3].offset,
             tfdb::internal::kVolumePrefixSize + partition_size);
  REQUIRE_EQ(events[3].requested,
             tfdb::internal::kPartitionHeaderEncodedSize);
  REQUIRE_EQ(events[3].transferred, events[3].requested);
  REQUIRE(events[4].operation == tfdb::StorageOperation::flush);
}

TEST(no_record_crosses_block_boundary) {
  Fixture fixture(64u * 1024u, 10, 512);
  fixture.open();
  std::vector<std::uint8_t> a(7, 'a'), b(7, 'b');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(b), 2));
  REQUIRE_OK(fixture.store->checkpoint());
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE(blocks[0] == a);
  REQUIRE(blocks[1] == b);
  std::vector<std::uint8_t> oversized(11, 0);
  REQUIRE(fixture.store->append(tfdb::ByteView(oversized), 3).code() == tfdb::StatusCode::out_of_range);
}

TEST(checkpointed_data_survives_and_volatile_tail_is_lost) {
  Fixture fixture;
  fixture.open();
  const char a = 'A', b = 'B';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
  REQUIRE_OK(fixture.store->publish());
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
}

TEST(torn_tail_sweep_recovers_previous_checkpoint) {
  const std::size_t frame_bytes = tfdb::internal::kBlockHeaderEncodedSize + 1;
  for (std::size_t cut = 0; cut <= frame_bytes; ++cut) {
    Fixture fixture;
    fixture.open();
    const char a = 'A', b = 'B';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
    REQUIRE_OK(fixture.store->publish());
    std::vector<tfdb::WriteFragment> persisted;
    if (cut != 0) persisted.push_back({1, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(persisted));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    REQUIRE_EQ(blocks.size(), cut < frame_bytes ? 1u : 2u);
    REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
    if (blocks.size() == 2)
      REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
  }
}

TEST(compressed_block_torn_sweep_recovers_exact_prefix) {
  for (std::size_t cut = 0; cut <= 132; ++cut) {
    Fixture fixture;
    fixture.partition.compression = tfdb::CompressionId::packbits;
    fixture.open();
    const std::vector<std::uint8_t> baseline(200, 'A');
    const std::vector<std::uint8_t> pending(200, 'B');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(baseline), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(pending), 2));
    REQUIRE_OK(fixture.store->publish());
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 1u);
    REQUIRE(events[0].operation == tfdb::StorageOperation::write);
    const std::size_t frame_bytes = events[0].requested;
    REQUIRE_EQ(frame_bytes, 132u);
    std::vector<tfdb::WriteFragment> persisted;
    if (cut != 0) persisted.push_back({1, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(persisted));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    REQUIRE_EQ(blocks.size(), cut < frame_bytes ? 1u : 2u);
    REQUIRE(blocks[0] == baseline);
    if (blocks.size() == 2) REQUIRE(blocks[1] == pending);
  }
}

TEST(multiple_dirty_blocks_subset_and_reorder_preserve_prefix) {
  // All subsets of three complete writes. A valid later block never bridges
  // a missing earlier sequence. Materialization order is separately reversed
  // for the all-complete case below.
  for (unsigned mask = 0; mask != 8; ++mask) {
    Fixture fixture;
    fixture.open();
    const std::vector<std::uint8_t> a(100, 'A');
    const std::vector<std::uint8_t> b(100, 'B');
    const std::vector<std::uint8_t> c(100, 'C');
    const std::vector<std::uint8_t> d(100, 'D');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(b), 2));
    REQUIRE_OK(fixture.store->publish());
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(c), 3));
    REQUIRE_OK(fixture.store->publish());
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(d), 4));
    REQUIRE_OK(fixture.store->publish());
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 3u);
    std::vector<tfdb::WriteFragment> fragments;
    for (unsigned bit = 0; bit != 3; ++bit)
      if ((mask & (1u << bit)) != 0)
        fragments.push_back({bit + 1u, 0, events[bit].requested});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    std::size_t expected = 1;
    while (expected <= 3 && (mask & (1u << (expected - 1))) != 0)
      ++expected;
    REQUIRE_EQ(blocks.size(), expected);
    REQUIRE(blocks[0] == a);
    if (expected >= 2) REQUIRE(blocks[1] == b);
    if (expected >= 3) REQUIRE(blocks[2] == c);
    if (expected == 4) REQUIRE(blocks[3] == d);
  }

  enum class Special { header_only, first_and_torn_second, reverse_complete };
  for (Special special : {Special::header_only,
                          Special::first_and_torn_second,
                          Special::reverse_complete}) {
    Fixture fixture;
    fixture.open();
    const std::vector<std::uint8_t> a(100, 'A');
    const std::vector<std::uint8_t> b(100, 'B');
    const std::vector<std::uint8_t> c(100, 'C');
    const std::vector<std::uint8_t> d(100, 'D');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    for (const std::vector<std::uint8_t>* payload : {&b, &c, &d}) {
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(*payload), 2));
      REQUIRE_OK(fixture.store->publish());
    }
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    std::vector<tfdb::WriteFragment> fragments;
    if (special == Special::header_only) {
      fragments.push_back({1, 0,
                           tfdb::internal::kBlockHeaderEncodedSize});
    } else if (special == Special::first_and_torn_second) {
      fragments.push_back({1, 0, events[0].requested});
      fragments.push_back({2, 0, events[1].requested - 1});
    } else {
      for (std::size_t i = 3; i != 0; --i)
        fragments.push_back({i, 0, events[i - 1].requested});
    }
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    const std::size_t expected = special == Special::reverse_complete ? 4u :
                                 special == Special::first_and_torn_second ? 2u : 1u;
    REQUIRE_EQ(blocks.size(), expected);
    REQUIRE(blocks[0] == a);
    if (expected >= 2) REQUIRE(blocks[1] == b);
    if (expected >= 3) REQUIRE(blocks[2] == c);
    if (expected == 4) REQUIRE(blocks[3] == d);
  }
}

TEST(failed_flush_may_persist_none_torn_or_complete_block) {
  for (unsigned outcome = 0; outcome != 3; ++outcome) {
    Fixture fixture;
    fixture.open();
    const std::vector<std::uint8_t> a(100, 'A');
    const std::vector<std::uint8_t> b(100, 'B');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 1, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(b), 2));
    REQUIRE(fixture.store->checkpoint().code() == tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE(events.size() >= 2);
    REQUIRE(events[0].operation == tfdb::StorageOperation::write);
    const std::size_t frame_size = events[0].requested;
    std::vector<tfdb::WriteFragment> fragments;
    if (outcome == 1) fragments.push_back({1, 0, frame_size - 1});
    if (outcome == 2) fragments.push_back({1, 0, frame_size});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    REQUIRE_EQ(blocks.size(), outcome == 2 ? 2u : 1u);
    REQUIRE(blocks[0] == a);
    if (outcome == 2) REQUIRE(blocks[1] == b);
  }
}

TEST(crash_after_backend_write_before_writer_state_update_recovers_block) {
  Fixture fixture;
  fixture.open();
  const char a = 'A', b = 'B';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.storage->reset_counters();
  bool captured_boundary = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t call,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::write ||
        point != tfdb::HookPoint::after || call != 1) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    fixture.storage->crash_persist_all_dirty();
    captured_boundary = true;
  });
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
  REQUIRE_OK(fixture.store->publish());
  REQUIRE(captured_boundary);
  // No later backend operation occurred: durable bytes are exactly the image
  // captured by HookPoint::after, before write_at() returned to RingStore.
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
}

TEST(writer_incarnation_chain_blocks_stale_suffix_after_second_crash) {
  Fixture fixture;
  tfdb::OpenOptions first_open;
  first_open.writable = true;
  first_open.next_partition = fixture.partition;
  first_open.writer_id_high = 0x1111;
  first_open.writer_id_low = 0x2222;
  first_open.allow_explicit_writer_id_for_testing = true;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, first_open,
                                   &fixture.store));

  const std::vector<std::uint8_t> baseline(200, 'A');
  const std::vector<std::uint8_t> torn(200, 'B');
  const std::vector<std::uint8_t> stale_suffix(200, 'C');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(baseline), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.storage->reset_counters();
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(torn), 2));
  REQUIRE_OK(fixture.store->publish());
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(stale_suffix), 3));
  REQUIRE_OK(fixture.store->publish());
  const std::size_t frame_size =
      tfdb::internal::kBlockHeaderEncodedSize + torn.size();
  REQUIRE_OK(fixture.storage->crash_materialize({
      {1, 0, 64}, {2, 0, frame_size}}));
  fixture.store.reset();

  tfdb::OpenOptions second_open = first_open;
  second_open.writer_id_high = 0x3333;
  second_open.writer_id_low = 0x4444;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, second_open,
                                   &fixture.store));
  const std::vector<std::uint8_t> replacement(200, 'D');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(replacement), 4));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.store.reset();

  tfdb::OpenOptions reader;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, reader, &fixture.store));
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('D'));
}

TEST(compression_is_independent_and_falls_back) {
  Fixture fixture(64u * 1024u, 512, 512);
  fixture.partition.compression = tfdb::CompressionId::packbits;
  fixture.open();
  std::vector<std::uint8_t> compressible(400, 0);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(compressible), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  bool saw_compressed = false;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data) {
      saw_compressed = event.metadata.compression == tfdb::CompressionId::packbits;
      REQUIRE_EQ(event.data.size(), compressible.size());
      REQUIRE(std::equal(event.data.data(), event.data.data() + event.data.size(), compressible.begin()));
    }
    return true;
  }));
  REQUIRE(saw_compressed);

  std::vector<std::uint8_t> random(400);
  std::mt19937 generator(7);
  for (auto& byte : random) byte = static_cast<std::uint8_t>(generator());
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(random), 2));
  REQUIRE_OK(fixture.store->checkpoint());
  bool saw_raw = false;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data && event.metadata.block_sequence == 1)
      saw_raw = event.metadata.compression == tfdb::CompressionId::none;
    return true;
  }));
  REQUIRE(saw_raw);
}

TEST(custom_codec_registry_is_partition_local_and_versioned) {
  Fixture fixture(64u * 1024u, 512, 512);
  const std::shared_ptr<const tfdb::CompressionCodec> codec(
      new RepeatedByteCodec());
  tfdb::OpenOptions writable;
  writable.writable = true;
  writable.next_partition.compression = codec->id();
  writable.next_partition.compression_version = codec->version();
  writable.compression_codecs.push_back(codec);
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writable, &fixture.store));
  std::vector<std::uint8_t> payload(400, 0xab);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(payload), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE(blocks[0] == payload);
  fixture.store.reset();

  tfdb::OpenOptions missing;
  std::unique_ptr<tfdb::RingStore> without_codec;
  REQUIRE(tfdb::RingStore::open(fixture.storage, missing, &without_codec).code() ==
          tfdb::StatusCode::unsupported);
  tfdb::OpenOptions readable;
  readable.compression_codecs.push_back(codec);
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, readable, &fixture.store));
  REQUIRE(collect_blocks(*fixture.store)[0] == payload);
}
