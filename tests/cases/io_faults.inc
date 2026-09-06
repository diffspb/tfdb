// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

TEST(hard_write_error_faults_writer_until_reopen) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::write;
  fault.call = 1;
  fault.result = tfdb::StatusCode::no_space;
  fixture.storage->add_fault(fault);
  const char byte = 'z';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  const tfdb::Status original = fixture.store->publish();
  REQUIRE(original.code() == tfdb::StatusCode::no_space);
  REQUIRE(!original.message().empty());
  const auto require_original = [&](const tfdb::Status& status) {
    REQUIRE(status.code() == original.code());
    REQUIRE(status.message() == original.message());
  };
  require_original(
      fixture.store->append(tfdb::ByteView(&byte, 1), 2));
  require_original(fixture.store->writer_status());
  require_original(
      fixture.store->set_next_partition_options(fixture.partition));
  tfdb::AsyncWriterOptions async_options;
  tfdb::AsyncWriter fresh_writer(*fixture.store, async_options);
  require_original(fresh_writer.start());
  fixture.storage->clear_faults();
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open();
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 3));
  REQUIRE_OK(fixture.store->checkpoint());
}

TEST(short_write_is_completed_at_remaining_offset) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  tfdb::FaultRule short_write;
  short_write.operation = tfdb::StorageOperation::write;
  short_write.call = 1;
  short_write.transfer_before_error = 10;
  short_write.result = tfdb::StatusCode::ok;
  fixture.storage->add_fault(short_write);
  const char byte = 's';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_EQ(fixture.storage->counters().write_calls, 2u);
  const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
  std::vector<tfdb::StorageEvent> writes;
  for (const tfdb::StorageEvent& event : events)
    if (event.operation == tfdb::StorageOperation::write) writes.push_back(event);
  REQUIRE_EQ(writes.size(), 2u);
  REQUIRE_EQ(writes[0].transferred, 10u);
  REQUIRE_EQ(writes[1].offset, writes[0].offset + 10u);
  REQUIRE_EQ(writes[1].requested, writes[0].requested - 10u);
  REQUIRE_EQ(collect_blocks(*fixture.store).size(), 1u);
}

TEST(interrupted_and_zero_progress_writes_are_handled) {
  {
    Fixture fixture;
    fixture.open();
    fixture.storage->reset_counters();
    tfdb::FaultRule interrupted;
    interrupted.operation = tfdb::StorageOperation::write;
    interrupted.call = 1;
    interrupted.result = tfdb::StatusCode::interrupted;
    fixture.storage->add_fault(interrupted);
    const char value = 'i';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    REQUIRE_EQ(fixture.storage->counters().write_calls, 2u);
  }
  {
    Fixture fixture;
    fixture.open();
    fixture.storage->reset_counters();
    tfdb::FaultRule no_progress;
    no_progress.operation = tfdb::StorageOperation::write;
    no_progress.call = 1;
    no_progress.result = tfdb::StatusCode::ok;
    no_progress.transfer_before_error = 0;
    fixture.storage->add_fault(no_progress);
    const char value = 'z';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
    REQUIRE(fixture.store->publish().code() == tfdb::StatusCode::io_error);
  }
}

TEST(memory_fault_model_epochs_and_materialization_are_atomic) {
  tfdb::MemoryStorage storage(32);
  const std::uint8_t first[] = {1, 2, 3, 4};
  REQUIRE_OK(tfdb::write_exact(storage, 0, tfdb::ByteView(first, sizeof first)));
  storage.reset_counters();
  REQUIRE(storage.crash_materialize({{1, 0, 1}}).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(storage.durable_image() == std::vector<std::uint8_t>(32, 0));

  const std::uint8_t second[] = {5, 6, 7, 8};
  REQUIRE_OK(tfdb::write_exact(storage, 8, tfdb::ByteView(second, sizeof second)));
  const std::vector<std::uint8_t> durable_before = storage.durable_image();
  REQUIRE(storage.crash_materialize({{1, 0, 2}, {99, 0, 1}}).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(storage.durable_image() == durable_before);
}

TEST(memory_fault_rules_remain_installed_across_counter_epochs) {
  tfdb::MemoryStorage storage(16);
  storage.add_fault({tfdb::StorageOperation::write, 1, 0,
                     tfdb::StatusCode::io_error});
  const std::uint8_t value = 7;
  REQUIRE(tfdb::write_exact(storage, 0, tfdb::ByteView(&value, 1)).code() ==
          tfdb::StatusCode::io_error);
  storage.reset_counters();
  REQUIRE(tfdb::write_exact(storage, 0, tfdb::ByteView(&value, 1)).code() ==
          tfdb::StatusCode::io_error);
  storage.clear_faults();
  storage.reset_counters();
  REQUIRE_OK(tfdb::write_exact(storage, 0, tfdb::ByteView(&value, 1)));
}

TEST(exact_reads_handle_short_interrupted_zero_and_hard_errors) {
  tfdb::MemoryStorage storage(16);
  const std::uint8_t input[] = {1, 2, 3, 4};
  REQUIRE_OK(tfdb::write_exact(storage, 0, tfdb::ByteView(input, sizeof input)));
  REQUIRE_OK(storage.flush());
  std::uint8_t output[4] = {};

  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 0,
                     tfdb::StatusCode::interrupted});
  REQUIRE_OK(tfdb::read_exact(storage, 0,
                              tfdb::MutableByteView(output, sizeof output)));
  REQUIRE_EQ(storage.counters().read_calls, 2u);
  REQUIRE(std::equal(output, output + sizeof output, input));

  storage.clear_faults();
  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 2,
                     tfdb::StatusCode::ok});
  REQUIRE_OK(tfdb::read_exact(storage, 0,
                              tfdb::MutableByteView(output, sizeof output)));
  REQUIRE_EQ(storage.counters().read_calls, 2u);

  storage.clear_faults();
  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 0,
                     tfdb::StatusCode::ok});
  REQUIRE(tfdb::read_exact(storage, 0,
      tfdb::MutableByteView(output, sizeof output)).code() ==
      tfdb::StatusCode::io_error);

  storage.clear_faults();
  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 0,
                     tfdb::StatusCode::io_error});
  REQUIRE(tfdb::read_exact(storage, 0,
      tfdb::MutableByteView(output, sizeof output)).code() ==
      tfdb::StatusCode::io_error);
}

TEST(flush_failure_never_advances_durable_metrics) {
  Fixture fixture;
  fixture.open();
  const std::uint64_t sync_calls_before = fixture.store->metrics().sync_calls;
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::flush;
  fault.call = 1;
  fault.result = tfdb::StatusCode::io_error;
  fixture.storage->add_fault(fault);
  const char value = 'd';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE(fixture.store->checkpoint().code() == tfdb::StatusCode::io_error);
  const tfdb::StoreMetrics metrics = fixture.store->metrics();
  REQUIRE_EQ(metrics.durable_records, 0u);
  REQUIRE_EQ(metrics.durable_blocks, 0u);
  REQUIRE_EQ(metrics.sync_errors, 1u);
  REQUIRE_EQ(metrics.sync_calls, sync_calls_before + 1u);
}

TEST(rotation_commit_failpoints_preserve_last_checkpoint) {
  for (tfdb::StorageOperation operation :
       {tfdb::StorageOperation::write, tfdb::StorageOperation::flush}) {
    const std::uint64_t last_call = operation == tfdb::StorageOperation::write ? 3 : 2;
    for (std::uint64_t call = 1; call <= last_call; ++call) {
      Fixture fixture;
      fixture.open();
      const char value = 'c';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
      REQUIRE_OK(fixture.store->checkpoint());
      fixture.storage->reset_counters();
      tfdb::FaultRule fault;
      fault.operation = operation;
      fault.call = call;
      fault.result = tfdb::StatusCode::io_error;
      fixture.storage->add_fault(fault);
      const tfdb::Status rotation = fixture.store->rotate(fixture.partition);
      REQUIRE(rotation.code() == tfdb::StatusCode::io_error);
      fixture.storage->crash_discard_volatile();
      fixture.storage->clear_faults();
      fixture.store.reset();
      fixture.open(false);
      const auto blocks = collect_blocks(*fixture.store);
      REQUIRE_EQ(blocks.size(), 1u);
      REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('c'));
      fixture.store.reset();
      fixture.open();
      const char continued = 'n';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&continued, 1), 2));
      REQUIRE_OK(fixture.store->checkpoint());
    }
  }
}
TEST(dirty_rotation_failpoints_have_explicit_recovery_oracle) {
  for (tfdb::StorageOperation operation :
       {tfdb::StorageOperation::write, tfdb::StorageOperation::flush}) {
    const std::uint64_t last_call =
        operation == tfdb::StorageOperation::write ? 4 : 3;
    for (std::uint64_t call = 1; call <= last_call; ++call) {
      Fixture fixture;
      fixture.open();
      const char baseline = 'A', pending = 'B';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&baseline, 1), 1));
      REQUIRE_OK(fixture.store->checkpoint());
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&pending, 1), 2));
      fixture.storage->reset_counters();
      fixture.storage->add_fault(
          {operation, call, 0, tfdb::StatusCode::io_error});
      REQUIRE(fixture.store->rotate(fixture.partition).code() ==
              tfdb::StatusCode::io_error);
      fixture.storage->crash_discard_volatile();
      fixture.storage->clear_faults();
      fixture.store.reset();
      fixture.open(false);
      const auto blocks = collect_blocks(*fixture.store);
      const std::size_t expected =
          (operation == tfdb::StorageOperation::write && call == 1) ||
          (operation == tfdb::StorageOperation::flush && call == 1) ? 1u : 2u;
      REQUIRE_EQ(blocks.size(), expected);
      REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
      if (expected == 2)
        REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));

      fixture.store.reset();
      fixture.open();
      const char after = 'C';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&after, 1), 3));
      REQUIRE_OK(fixture.store->checkpoint());
    }
  }
}

TEST(valid_footer_persisted_without_index_falls_back_to_block_scan) {
  Fixture fixture;
  fixture.open();
  const char value = 'r';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.storage->reset_counters();
  tfdb::FaultRule failed_seal_flush;
  failed_seal_flush.operation = tfdb::StorageOperation::flush;
  failed_seal_flush.call = 1;
  failed_seal_flush.result = tfdb::StatusCode::io_error;
  fixture.storage->add_fault(failed_seal_flush);
  REQUIRE(fixture.store->rotate(fixture.partition).code() ==
          tfdb::StatusCode::io_error);
  // Write call 1 is the index and call 2 is the footer. Model a controller
  // that persisted the later footer write but not the earlier index write.
  REQUIRE_OK(fixture.storage->crash_materialize({
      {2, 0, tfdb::internal::kPartitionFooterEncodedSize}}));
  fixture.storage->clear_faults();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks_with_gaps(*fixture.store, 1);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('r'));
}

TEST(seal_index_footer_subsets_have_explicit_recovery_oracle) {
  enum class Persisted { none, index_prefix, index_only, footer_prefix,
                         footer_only, footer_with_torn_index,
                         footer_then_index, index_then_footer };
  for (Persisted persisted :
       {Persisted::none, Persisted::index_prefix, Persisted::index_only,
        Persisted::footer_prefix, Persisted::footer_only,
        Persisted::footer_with_torn_index, Persisted::footer_then_index,
        Persisted::index_then_footer}) {
    Fixture fixture;
    fixture.open();
    const char a = 'A';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 1, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE(fixture.store->rotate(fixture.partition).code() ==
            tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 3u);
    REQUIRE(events[0].operation == tfdb::StorageOperation::write);
    REQUIRE(events[1].operation == tfdb::StorageOperation::write);
    REQUIRE(events[2].operation == tfdb::StorageOperation::flush);
    REQUIRE_EQ(events[0].requested, tfdb::internal::kIndexEntryEncodedSize);
    REQUIRE_EQ(events[1].requested,
               tfdb::internal::kPartitionFooterEncodedSize);
    std::vector<tfdb::WriteFragment> fragments;
    if (persisted == Persisted::index_prefix)
      fragments.push_back({1, 0, events[0].requested - 1});
    else if (persisted == Persisted::index_only)
      fragments.push_back({1, 0, events[0].requested});
    else if (persisted == Persisted::footer_prefix)
      fragments.push_back({2, 0, 64});
    else if (persisted == Persisted::footer_only)
      fragments.push_back({2, 0, events[1].requested});
    else if (persisted == Persisted::footer_with_torn_index) {
      fragments.push_back({2, 0, events[1].requested});
      fragments.push_back({1, 0, 20});
    }
    else if (persisted == Persisted::footer_then_index) {
      fragments.push_back({2, 0, events[1].requested});
      fragments.push_back({1, 0, events[0].requested});
    } else if (persisted == Persisted::index_then_footer) {
      fragments.push_back({1, 0, events[0].requested});
      fragments.push_back({2, 0, events[1].requested});
    }
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    std::size_t gaps = 0;
    const auto blocks = [&] {
      std::vector<std::vector<std::uint8_t>> result;
      REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
          [&](const tfdb::BlockEvent& event) {
            if (event.kind == tfdb::BlockEventKind::data)
              result.emplace_back(event.data.data(),
                                  event.data.data() + event.data.size());
            else
              ++gaps;
            return true;
          }));
      return result;
    }();
    REQUIRE_EQ(blocks.size(), 1u);
    REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
    const bool invalid_seal = persisted == Persisted::footer_only ||
                              persisted == Persisted::footer_with_torn_index;
    REQUIRE_EQ(gaps, invalid_seal ? 1u : 0u);

    fixture.store.reset();
    fixture.open();
    const char c = 'C';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&c, 1), 3));
    REQUIRE_OK(fixture.store->checkpoint());
    std::size_t continued_gaps = 0;
    std::vector<std::vector<std::uint8_t>> continued;
    REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data)
            continued.emplace_back(event.data.data(),
                                   event.data.data() + event.data.size());
          else
            ++continued_gaps;
          return true;
        }));
    REQUIRE_EQ(continued.size(), 2u);
    REQUIRE_EQ(continued[0][0], static_cast<std::uint8_t>('A'));
    REQUIRE_EQ(continued[1][0], static_cast<std::uint8_t>('C'));
    REQUIRE_EQ(continued_gaps, invalid_seal ? 1u : 0u);
  }
}

TEST(new_partition_header_torn_boundaries_are_safe_or_explicit) {
  const std::size_t cuts[] = {0, 1, 7, 8, 16, 31, 32, 39, 40, 103,
                              104, 107, 108, 127, 128, 167, 168, 207,
                              208, 247, 248, 256, 257, 258, 511, 512,
                              1023, 1024};
  for (std::size_t cut : cuts) {
    Fixture fixture;
    fixture.open();
    const char a = 'A';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 2, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE(fixture.store->rotate(fixture.partition).code() ==
            tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 5u);
    REQUIRE(events[3].operation == tfdb::StorageOperation::write);
    REQUIRE_EQ(events[3].requested,
               tfdb::internal::kPartitionHeaderEncodedSize);
    std::vector<tfdb::WriteFragment> fragments;
    if (cut != 0) fragments.push_back({3, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    std::size_t gaps = 0;
    std::size_t data = 0;
    REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data) ++data;
          else ++gaps;
          return true;
        }));
    REQUIRE_EQ(data, 1u);
    // Prefix 32 proves the UUID; prefix 257 includes the last nonzero byte of
    // this default header. The unwritten suffix is canonical reserved zero.
    const bool identified_damage = cut >= 32 && cut < 257;
    REQUIRE_EQ(gaps, identified_damage ? 1u : 0u);

    fixture.store.reset();
    tfdb::OpenOptions writable;
    writable.writable = true;
    writable.next_partition = fixture.partition;
    const tfdb::Status reopened = tfdb::RingStore::open(
        fixture.storage, writable, &fixture.store);
    if (identified_damage) {
      REQUIRE(reopened.code() == tfdb::StatusCode::corrupt);
    } else {
      REQUIRE_OK(reopened);
      const char b = 'B';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
      REQUIRE_OK(fixture.store->checkpoint());
    }
  }
}

TEST(reused_partition_header_torn_boundaries_do_not_resurrect_old_slot) {
  const std::size_t cuts[] = {0, 31, 32, 33, 39, 40, 103, 104,
                              107, 108, 1024};
  for (std::size_t cut : cuts) {
    Fixture fixture(64u * 1024u, 256, 512, 2);
    fixture.open();
    const char a = 'A', b = 'B';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 2, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE(fixture.store->rotate(fixture.partition).code() ==
            tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 5u);
    REQUIRE(events[3].operation == tfdb::StorageOperation::write);
    std::vector<tfdb::WriteFragment> fragments;
    if (cut != 0) fragments.push_back({3, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    std::size_t gaps = 0;
    std::vector<std::uint8_t> values;
    REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data)
            values.push_back(event.data.data()[0]);
          else
            ++gaps;
          return true;
        }));
    const bool old_header_unchanged = cut <= 32;
    const bool identified_damage = cut >= 33 && cut < 108;
    REQUIRE_EQ(gaps, identified_damage ? 1u : 0u);
    if (old_header_unchanged) {
      REQUIRE(values == std::vector<std::uint8_t>({'A', 'B'}));
    } else {
      REQUIRE(values == std::vector<std::uint8_t>({'B'}));
    }
    fixture.store.reset();
    tfdb::OpenOptions writable;
    writable.writable = true;
    writable.next_partition = fixture.partition;
    const tfdb::Status reopened = tfdb::RingStore::open(
        fixture.storage, writable, &fixture.store);
    if (identified_damage) {
      REQUIRE(reopened.code() == tfdb::StatusCode::corrupt);
    } else {
      REQUIRE_OK(reopened);
      const char c = 'C';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&c, 1), 3));
      REQUIRE_OK(fixture.store->checkpoint());
      const auto current = collect_blocks(*fixture.store);
      REQUIRE_EQ(current.back()[0], static_cast<std::uint8_t>('C'));
    }
  }
}

TEST(health_snapshot_is_readable_while_a_flush_is_stalled) {
  // A watchdog must be able to observe a storage stall while it happens.
  // metrics() and writer_status() wait behind the flush by design; health()
  // must not.
  Fixture fixture;
  fixture.open();
  const char byte = 'h';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));

  std::mutex gate;
  std::condition_variable gate_signal;
  bool flush_entered = false;
  bool flush_released = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::flush ||
        point != tfdb::HookPoint::before)
      return;
    std::unique_lock<std::mutex> lock(gate);
    flush_entered = true;
    gate_signal.notify_all();
    gate_signal.wait(lock, [&] { return flush_released; });
  });

  std::thread writer([&] { (void)fixture.store->checkpoint(); });
  {
    std::unique_lock<std::mutex> lock(gate);
    REQUIRE(gate_signal.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return flush_entered; }));
  }

  // Observed from this thread while the writer holds the store lock inside
  // flush. A regression that takes that lock here would hang instead.
  std::atomic<bool> observed(false);
  tfdb::WriterHealth health;
  std::thread observer([&] {
    health = fixture.store->health();
    observed.store(true);
  });
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!observed.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool answered = observed.load();
  {
    std::lock_guard<std::mutex> lock(gate);
    flush_released = true;
  }
  gate_signal.notify_all();
  observer.join();
  writer.join();
  fixture.storage->set_hook(tfdb::StorageHook());

  REQUIRE(answered);
  REQUIRE(health.writable);
  REQUIRE(!health.closed);
  REQUIRE(!health.faulted);
  // The record accepted before the checkpoint was still undurable while the
  // flush was in flight.
  REQUIRE(health.oldest_undurable_age_ns > 0);

  // After the checkpoint completes the lag clears and the counters advance.
  const tfdb::WriterHealth settled = fixture.store->health();
  REQUIRE_EQ(settled.oldest_undurable_age_ns, 0u);
  REQUIRE(settled.sync_calls > 0);
  REQUIRE_EQ(settled.sync_errors, 0u);
  REQUIRE(settled.checkpoints > 0);
}

TEST(health_reports_fault_and_close_without_the_store_lock) {
  Fixture fixture;
  fixture.open();
  // Fault call numbers are one-based within the current counter epoch, and
  // the writable open already wrote a partition header.
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::write;
  fault.call = 1;
  fault.result = tfdb::StatusCode::no_space;
  fixture.storage->add_fault(fault);
  const char byte = 'z';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE(fixture.store->publish().code() == tfdb::StatusCode::no_space);

  const tfdb::WriterHealth faulted = fixture.store->health();
  REQUIRE(faulted.faulted);
  REQUIRE(faulted.fault_code == tfdb::StatusCode::no_space);
  REQUIRE(!faulted.closed);

  fixture.storage->clear_faults();
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const tfdb::WriterHealth read_only = fixture.store->health();
  REQUIRE(!read_only.writable);
  REQUIRE(!read_only.faulted);
  REQUIRE_OK(fixture.store->close());
  REQUIRE(fixture.store->health().closed);
}
