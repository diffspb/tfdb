TEST(async_checkpoint_is_a_durable_fifo_barrier) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 16;
  options.queue_capacity_bytes = 1024;
  options.checkpoint_interval = std::chrono::milliseconds(20);
  tfdb::AsyncWriter writer(*fixture.store, options);
  fixture.storage->reset_counters();
  REQUIRE_OK(writer.start());
  for (char value : {'a', 'b', 'c'})
    REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), value));
  REQUIRE_OK(writer.checkpoint());
  REQUIRE(fixture.storage->counters().flush_calls >= 1);
  REQUIRE_EQ(fixture.store->metrics().durable_records, 3u);
  const tfdb::AsyncWriterMetrics async_metrics = writer.metrics();
  REQUIRE_EQ(async_metrics.submitted_records, 3u);
  REQUIRE_EQ(async_metrics.processed_records, 3u);
  REQUIRE_EQ(async_metrics.durable_sequence, 3u);
  REQUIRE_EQ(async_metrics.oldest_undurable_age_ns, 0u);
  REQUIRE_OK(writer.stop());
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0].size(), 3u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('a'));
  REQUIRE_EQ(blocks[0][1], static_cast<std::uint8_t>('b'));
  REQUIRE_EQ(blocks[0][2], static_cast<std::uint8_t>('c'));
}

TEST(async_checked_submit_revalidates_partition_contract) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 4;
  options.queue_capacity_bytes = 1024;
  options.checkpoint_interval = std::chrono::milliseconds(20);
  tfdb::AsyncWriter writer(*fixture.store, options);
  tfdb::AppendContract contract;
  contract.record_format_id = tfdb::kFramedRecordV1ProfileId;
  contract.record_format_version = 1;
  contract.time_domain_id = 1;
  const char valid = 'v';
  REQUIRE_OK(writer.start());
  REQUIRE_OK(writer.submit_checked(tfdb::ByteView(&valid, 1), 1, contract));
  REQUIRE_OK(writer.checkpoint());
  REQUIRE_OK(writer.stop());

  REQUIRE_OK(fixture.store->rotate(tfdb::PartitionOptions()));
  const char mislabeled = 'x';
  REQUIRE_OK(writer.start());
  REQUIRE_OK(writer.submit_checked(tfdb::ByteView(&mislabeled, 1), 2,
                                   contract));
  const tfdb::Status rejected = writer.checkpoint();
  REQUIRE(rejected.code() == tfdb::StatusCode::invalid_argument);
  REQUIRE(writer.stop().code() == tfdb::StatusCode::invalid_argument);
  REQUIRE_EQ(fixture.store->metrics().accepted_records, 1u);
}

TEST(async_stop_drains_queue_and_clean_writer_can_restart) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 256;
  options.queue_capacity_bytes = 4096;
  options.checkpoint_interval = std::chrono::seconds(10);
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  for (std::uint16_t i = 0; i != 100; ++i) {
    const std::uint8_t value = static_cast<std::uint8_t>(i);
    tfdb::Status submit_status;
    do {
      submit_status = writer.submit(tfdb::ByteView(&value, 1), i);
      if (submit_status.code() == tfdb::StatusCode::busy)
        std::this_thread::yield();
    } while (submit_status.code() == tfdb::StatusCode::busy);
    REQUIRE_OK(submit_status);
  }
  REQUIRE_OK(writer.stop());
  REQUIRE_EQ(writer.metrics().durable_sequence, 100u);
  REQUIRE_OK(writer.start());
  const std::uint8_t last = 100;
  REQUIRE_OK(writer.submit(tfdb::ByteView(&last, 1), 100));
  REQUIRE_OK(writer.checkpoint());
  REQUIRE_OK(writer.stop());
  REQUIRE_EQ(fixture.store->metrics().durable_records, 101u);
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  std::vector<std::uint8_t> recovered;
  for (const std::vector<std::uint8_t>& block : blocks)
    recovered.insert(recovered.end(), block.begin(), block.end());
  REQUIRE_EQ(recovered.size(), 101u);
  for (std::uint16_t i = 0; i != 101; ++i)
    REQUIRE_EQ(recovered[i], static_cast<std::uint8_t>(i));
}

TEST(async_stall_has_exact_backpressure_and_durability_age_oracle) {
  Fixture fixture(64u * 1024u, 1, 512);
  fixture.open();
  std::atomic<std::uint64_t> now_ns(10);
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 1;
  options.queue_capacity_bytes = 1;
  options.checkpoint_interval = std::chrono::seconds(10);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char a = 'A', b = 'B', c = 'C', d = 'D';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&a, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool backend_blocked = false;
  bool release_backend = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::write ||
        point != tfdb::HookPoint::before) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    std::unique_lock<std::mutex> lock(gate_mutex);
    backend_blocked = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_backend; });
  });
  now_ns.store(20);
  REQUIRE_OK(writer.submit(tfdb::ByteView(&b, 1), 2));
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    const bool entered = gate_condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return backend_blocked; });
    if (!entered) {
      release_backend = true;
      gate_condition.notify_all();
    }
    REQUIRE(entered);
  }
  now_ns.store(30);
  REQUIRE_OK(writer.submit(tfdb::ByteView(&c, 1), 3));
  REQUIRE(writer.submit(tfdb::ByteView(&d, 1), 4).code() ==
          tfdb::StatusCode::busy);
  REQUIRE_EQ(writer.queued_records(), 1u);
  REQUIRE_EQ(writer.queued_bytes(), 1u);

  tfdb::Status checkpoint_status;
  std::thread checkpoint_thread([&] { checkpoint_status = writer.checkpoint(); });
  now_ns.store(110);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_backend = true;
  }
  gate_condition.notify_all();
  checkpoint_thread.join();
  REQUIRE_OK(checkpoint_status);
  const tfdb::AsyncWriterMetrics metrics = writer.metrics();
  REQUIRE_EQ(metrics.submitted_records, 3u);
  REQUIRE_EQ(metrics.processed_records, 3u);
  REQUIRE_EQ(metrics.durable_sequence, 3u);
  REQUIRE_EQ(metrics.backpressure_events, 1u);
  REQUIRE_EQ(metrics.maximum_queued_records, 1u);
  REQUIRE_EQ(metrics.maximum_queued_bytes, 1u);
  REQUIRE_EQ(metrics.maximum_queue_residence_ns, 80u);
  REQUIRE_EQ(metrics.maximum_accepted_to_durable_ns, 100u);
  REQUIRE_EQ(metrics.oldest_undurable_age_ns, 0u);
  REQUIRE_OK(writer.stop());

  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 3u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
  REQUIRE_EQ(blocks[2][0], static_cast<std::uint8_t>('C'));
}

TEST(async_custom_clock_drives_timer_and_includes_backend_stall) {
  Fixture fixture;
  fixture.open();
  const std::vector<std::uint8_t> durable_baseline =
      fixture.storage->durable_image();
  fixture.storage->reset_counters();
  const std::uint64_t interval_ns = 1000000;
  const std::uint64_t stall_ns = 2000000;
  std::atomic<std::uint64_t> now_ns(0);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(1);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char a = 'A', b = 'B';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&a, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);
  REQUIRE_EQ(fixture.storage->counters().flush_calls, 0u);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool flush_blocked = false;
  bool release_flush = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::flush ||
        point != tfdb::HookPoint::before) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    std::unique_lock<std::mutex> lock(gate_mutex);
    flush_blocked = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_flush; });
  });
  now_ns.store(interval_ns);
  writer.notify_clock_advanced();
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    const bool entered = gate_condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return flush_blocked; });
    if (!entered) {
      release_flush = true;
      gate_condition.notify_all();
    }
    REQUIRE(entered);
  }
  tfdb::AsyncWriterMetrics blocked_metrics = writer.metrics();
  REQUIRE_EQ(blocked_metrics.durable_sequence, 0u);
  REQUIRE_EQ(blocked_metrics.oldest_undurable_age_ns, interval_ns);
  REQUIRE(fixture.storage->durable_image() == durable_baseline);
  REQUIRE_OK(writer.submit(tfdb::ByteView(&b, 1), 2));

  now_ns.store(interval_ns + stall_ns);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_flush = true;
  }
  gate_condition.notify_all();
  const auto durable_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
  while (writer.metrics().durable_sequence < 1 &&
         std::chrono::steady_clock::now() < durable_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().durable_sequence, 1u);
  REQUIRE_OK(writer.checkpoint());
  const tfdb::AsyncWriterMetrics metrics = writer.metrics();
  REQUIRE_EQ(metrics.durable_sequence, 2u);
  REQUIRE_EQ(metrics.maximum_queue_residence_ns, stall_ns);
  REQUIRE_EQ(metrics.maximum_accepted_to_durable_ns,
             interval_ns + stall_ns);
  REQUIRE_EQ(metrics.oldest_undurable_age_ns, 0u);
  REQUIRE_OK(writer.stop());

  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
}

TEST(async_custom_timer_flush_failure_never_claims_durability) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  fixture.storage->add_fault({tfdb::StorageOperation::flush, 1, 0,
                              tfdb::StatusCode::io_error});
  std::atomic<std::uint64_t> now_ns(0);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(1);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char value = 'E';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);
  now_ns.store(1000000);
  writer.notify_clock_advanced();
  const auto failure_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
  while (writer.background_status().ok() &&
         std::chrono::steady_clock::now() < failure_deadline)
    std::this_thread::yield();
  REQUIRE(writer.background_status().code() == tfdb::StatusCode::io_error);
  const char later = 'L';
  REQUIRE(writer.submit(tfdb::ByteView(&later, 1), 2).code() ==
          tfdb::StatusCode::io_error);
  REQUIRE(writer.checkpoint().code() == tfdb::StatusCode::io_error);
  const tfdb::AsyncWriterMetrics metrics = writer.metrics();
  REQUIRE_EQ(metrics.durable_sequence, 0u);
  REQUIRE_EQ(metrics.maximum_accepted_to_durable_ns, 0u);
  REQUIRE_EQ(metrics.background_errors, 1u);
  REQUIRE(writer.stop().code() == tfdb::StatusCode::io_error);
  fixture.storage->crash_discard_volatile();
  fixture.storage->clear_faults();
  fixture.store.reset();
  fixture.open(false);
  REQUIRE(collect_blocks(*fixture.store).empty());
}

TEST(async_rejects_unrepresentable_timer_interval) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds::max();
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE(writer.start().code() == tfdb::StatusCode::invalid_argument);

  options.checkpoint_interval = std::chrono::milliseconds(
      UINT64_MAX / static_cast<std::uint64_t>(1000000));
  tfdb::AsyncWriter largest_representable(*fixture.store, options);
  REQUIRE_OK(largest_representable.start());
  REQUIRE_OK(largest_representable.stop());
}

TEST(async_timer_at_uint64_max_checkpoints_once_without_busy_loop) {
  Fixture fixture;
  fixture.open();
  constexpr std::uint64_t interval_ns = 1000000;
  std::atomic<std::uint64_t> now_ns(UINT64_MAX - interval_ns);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(1);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char value = 'M';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);

  now_ns.store(UINT64_MAX);
  writer.notify_clock_advanced();
  const auto durable_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
  while (writer.metrics().durable_sequence != 1 &&
         std::chrono::steady_clock::now() < durable_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().durable_sequence, 1u);
  REQUIRE_EQ(writer.metrics().checkpoint_calls, 1u);
  for (unsigned i = 0; i != 1000; ++i) {
    writer.notify_clock_advanced();
    std::this_thread::yield();
  }
  REQUIRE_EQ(writer.metrics().checkpoint_calls, 1u);
  REQUIRE_OK(writer.stop());
}

TEST(async_sparse_timer_publishes_partial_block) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(10);
  tfdb::AsyncWriter writer(*fixture.store, options);
  fixture.storage->reset_counters();
  REQUIRE_OK(writer.start());
  const char value = 'q';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (fixture.storage->counters().flush_calls == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(fixture.storage->counters().flush_calls >= 1);
  REQUIRE_EQ(fixture.store->metrics().durable_records, 1u);
  REQUIRE_OK(writer.stop());
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  REQUIRE_EQ(collect_blocks(*fixture.store).size(), 1u);
}

TEST(async_background_failure_is_joined_and_reported) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::flush;
  fault.call = 1;
  fault.result = tfdb::StatusCode::no_space;
  fixture.storage->add_fault(fault);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::seconds(1);
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char value = 'e';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const tfdb::Status original = writer.checkpoint();
  REQUIRE(original.code() == tfdb::StatusCode::no_space);
  REQUIRE(!original.message().empty());
  const auto require_original = [&](const tfdb::Status& status) {
    REQUIRE(status.code() == original.code());
    REQUIRE(status.message() == original.message());
  };
  require_original(writer.background_status());
  require_original(writer.submit(tfdb::ByteView(&value, 1), 2));
  require_original(writer.checkpoint());
  require_original(writer.stop());
  require_original(writer.start());
  require_original(fixture.store->writer_status());
  tfdb::AsyncWriter fresh_writer(*fixture.store, options);
  require_original(fresh_writer.start());
}

TEST(explicit_close_durably_flushes_tail_and_closes_writer) {
  Fixture fixture;
  fixture.open();
  const char value = 'x';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(fixture.store->close());
  REQUIRE_OK(fixture.store->close());
  REQUIRE(fixture.store->append(tfdb::ByteView(&value, 1), 2).code() ==
          tfdb::StatusCode::closed);
  REQUIRE(fixture.store->writer_status().code() == tfdb::StatusCode::closed);
  tfdb::AsyncWriterOptions options;
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE(writer.start().code() == tfdb::StatusCode::closed);
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  REQUIRE_EQ(collect_blocks(*fixture.store).size(), 1u);
}

TEST(deterministic_crash_model_preserves_every_checkpointed_record) {
  Fixture fixture(1024u * 1024u, 256, 512, 4);
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  std::mt19937 random(0x5eed);
  std::vector<std::uint64_t> required;
  std::vector<std::uint64_t> pending;
  for (std::uint64_t i = 1; i <= 500; ++i) {
    const std::uint8_t byte = static_cast<std::uint8_t>(i);
    REQUIRE_OK(tfdb::append_framed_record(*fixture.store, static_cast<std::int64_t>(i),
                                         i, 0, tfdb::ByteView(&byte, 1)));
    pending.push_back(i);
    if (random() % 13 == 0) {
      REQUIRE_OK(fixture.store->checkpoint());
      required.insert(required.end(), pending.begin(), pending.end());
      pending.clear();
    }
    if (random() % 47 == 0) {
      fixture.storage->crash_discard_volatile();
      fixture.store.reset();
      fixture.open();
      pending.clear();
      const auto actual = collect_selectors(*fixture.store, 0, 1000);
      REQUIRE(actual == required);
    }
  }
  REQUIRE_OK(fixture.store->checkpoint());
  required.insert(required.end(), pending.begin(), pending.end());
  REQUIRE(collect_selectors(*fixture.store, 0, 1000) == required);
}

TEST(file_backend_integration_reopens_data) {
  std::ostringstream path_builder;
  path_builder << "/tmp/tfdb-integration-" << ::getpid() << ".tfdb";
  const std::string path = path_builder.str();
  ::unlink(path.c_str());
  std::shared_ptr<tfdb::FileStorage> storage;
  const std::uint64_t partition_size = 64u * 1024u;
  const std::uint64_t total_size = tfdb::internal::kVolumePrefixSize + partition_size * 3;
  REQUIRE_OK(tfdb::FileStorage::create(path, total_size, false, &storage));
  std::shared_ptr<tfdb::FileStorage> competing_writer;
  REQUIRE(tfdb::FileStorage::open_existing(path, true, &competing_writer).code() ==
          tfdb::StatusCode::busy);
  std::shared_ptr<tfdb::FileStorage> concurrent_reader;
  REQUIRE_OK(tfdb::FileStorage::open_existing(path, false, &concurrent_reader));
  concurrent_reader.reset();
  tfdb::VolumeOptions volume;
  volume.partition_size = partition_size;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 4096;
  REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  tfdb::OpenOptions options;
  options.writable = true;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(storage, options, &store));
  const char value = 'f';
  REQUIRE_OK(store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(store->checkpoint());
  store.reset(); storage.reset();
  REQUIRE_OK(tfdb::FileStorage::open_existing(path, false, &storage));
  options.writable = false;
  REQUIRE_OK(tfdb::RingStore::open(storage, options, &store));
  const auto blocks = collect_blocks(*store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('f'));
  ::unlink(path.c_str());
}
