// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use std::process::ExitCode;

use tfdb_reader::Reader;

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args_os();
    let program = arguments.next().unwrap_or_default();
    let Some(path) = arguments.next() else {
        eprintln!("usage: {} PATH", program.to_string_lossy());
        return Err("missing path".into());
    };
    if arguments.next().is_some() {
        return Err("unexpected argument".into());
    }
    let reader = Reader::open(path)?;
    let volume = reader.volume();
    println!(
        "volume id={:016x}{:016x} bytes={} partitions={} partition_bytes={} block_payload_bytes={} quantum={} index_bytes={}",
        volume.volume_id_high, volume.volume_id_low, volume.volume_size,
        volume.partition_count, volume.partition_size, volume.max_block_payload,
        volume.persistence_quantum, volume.index_region_size,
    );
    for partition in reader.partitions() {
        println!(
            "partition slot={} generation={} state={} blocks={} data_span_bytes={} min_time={} max_time={} flags=0x{:x} compression={} record_profile={}:{} time_domain={}",
            partition.slot, partition.generation,
            if partition.sealed { "sealed" } else { "active" },
            partition.block_count, partition.data_span_bytes, partition.min_time_ns,
            partition.max_time_ns, partition.flags, partition.compression_id,
            partition.record_format_id,
            partition.record_format_version, partition.time_domain_id,
        );
    }
    for gap in reader.initial_gaps() {
        eprintln!("gap slot={} reason={}", gap.partition_slot, gap.message);
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("tfdb-rs-inspect: {error}");
            ExitCode::from(2)
        }
    }
}
