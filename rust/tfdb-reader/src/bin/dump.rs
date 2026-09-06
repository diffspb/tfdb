// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use std::collections::HashSet;
use std::io::{self, Write};
use std::process::ExitCode;

use tfdb_reader::{decode_framed_v1, Error, Event, Reader, TimeRange, FRAMED_RECORD_V1_PROFILE_ID};

#[derive(Default)]
struct Options {
    path: String,
    framed: bool,
    raw_blocks: bool,
    from: Option<i64>,
    to: Option<i64>,
    time_domain: Option<u64>,
    selectors: HashSet<u64>,
}

fn usage() {
    eprintln!("usage: tfdb-rs-dump PATH [--from NS --to NS] [--selector ID] [--time-domain ID] [--framed-v1 | --raw-blocks]");
}

fn parse() -> Result<Options, String> {
    let mut arguments = std::env::args().skip(1);
    let mut options = Options::default();
    options.path = arguments.next().ok_or_else(|| "missing path".to_string())?;
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--framed-v1" => options.framed = true,
            "--raw-blocks" => options.raw_blocks = true,
            "--from" => {
                options.from = Some(
                    arguments
                        .next()
                        .ok_or("missing --from value")?
                        .parse()
                        .map_err(|_| "invalid --from value")?,
                )
            }
            "--to" => {
                options.to = Some(
                    arguments
                        .next()
                        .ok_or("missing --to value")?
                        .parse()
                        .map_err(|_| "invalid --to value")?,
                )
            }
            "--time-domain" => {
                let value = arguments
                    .next()
                    .ok_or("missing --time-domain value")?
                    .parse()
                    .map_err(|_| "invalid --time-domain value")?;
                if value == 0 {
                    return Err("time domain must be nonzero".into());
                }
                options.time_domain = Some(value);
            }
            "--selector" => {
                let value = arguments
                    .next()
                    .ok_or("missing --selector value")?
                    .parse()
                    .map_err(|_| "invalid --selector value")?;
                options.selectors.insert(value);
            }
            _ => return Err(format!("unknown argument {argument}")),
        }
    }
    if options.framed && options.raw_blocks {
        return Err("choose one output mode".into());
    }
    if options.from.is_some() != options.to.is_some() {
        return Err("--from and --to must be supplied together".into());
    }
    Ok(options)
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let count = bytes.len().min(64);
    let mut output = String::with_capacity(count * 2 + 3);
    for &byte in &bytes[..count] {
        output.push(DIGITS[(byte >> 4) as usize] as char);
        output.push(DIGITS[(byte & 15) as usize] as char);
    }
    if count != bytes.len() {
        output.push_str("...");
    }
    output
}

fn run() -> Result<bool, Box<dyn std::error::Error>> {
    let options = parse().map_err(|error| {
        usage();
        error
    })?;
    let reader = Reader::open(&options.path)?;
    let range = match (options.from, options.to) {
        (Some(begin_ns), Some(end_ns)) => Some(TimeRange {
            begin_ns,
            end_ns,
            time_domain_id: options.time_domain.unwrap_or(1),
        }),
        _ if options.framed => Some(TimeRange {
            begin_ns: i64::MIN,
            end_ns: i64::MAX,
            time_domain_id: options.time_domain.unwrap_or(1),
        }),
        _ => None,
    };
    let mut saw_gap = false;
    let mut callback_error: Option<Box<dyn std::error::Error>> = None;
    let stdout = io::stdout();
    let mut output = stdout.lock();
    let visit = |event: Event| {
        match event {
            Event::Gap(gap) => {
                saw_gap = true;
                eprintln!(
                    "gap generation={} block={:?} reason={}",
                    gap.partition_generation, gap.block_sequence, gap.message
                );
            }
            Event::Data(block) if options.framed => {
                if block.metadata.record_format_id != FRAMED_RECORD_V1_PROFILE_ID
                    || block.metadata.record_format_version != 1
                {
                    callback_error = Some(Box::new(Error::unsupported(
                        "selected partition is not FramedRecordV1",
                    )));
                    return false;
                }
                match decode_framed_v1(&block.data) {
                    Ok(records) if records.len() == block.metadata.record_count as usize => {
                        for record in records {
                            let selected_time = range.unwrap();
                            if record.index_time_ns < selected_time.begin_ns
                                || record.index_time_ns >= selected_time.end_ns
                                || (!options.selectors.is_empty()
                                    && !options.selectors.contains(&record.selector))
                            {
                                continue;
                            }
                            if writeln!(output,
                                "generation={} block={} time={} selector={} flags=0x{:x} payload={}",
                                block.metadata.partition_generation,
                                block.metadata.block_sequence, record.index_time_ns,
                                record.selector, record.flags, hex(record.payload)).is_err() {
                                callback_error = Some("stdout write failed".into());
                                return false;
                            }
                        }
                    }
                    Ok(_) => {
                        callback_error =
                            Some("FramedRecordV1 count differs from block header".into())
                    }
                    Err(error) => callback_error = Some(Box::new(error)),
                }
            }
            Event::Data(block) if options.raw_blocks => {
                let length = (block.data.len() as u32).to_le_bytes();
                if output
                    .write_all(&length)
                    .and_then(|_| output.write_all(&block.data))
                    .is_err()
                {
                    callback_error = Some("stdout write failed".into());
                }
            }
            Event::Data(block) => {
                if writeln!(output,
                    "generation={} block={} records={} min_time={} max_time={} raw_bytes={} data={}",
                    block.metadata.partition_generation, block.metadata.block_sequence,
                    block.metadata.record_count, block.metadata.min_time_ns,
                    block.metadata.max_time_ns, block.data.len(), hex(&block.data)).is_err() {
                    callback_error = Some("stdout write failed".into());
                }
            }
        }
        callback_error.is_none()
    };
    if let Some(range) = range {
        reader.query_blocks(range, visit)?;
    } else {
        reader.scan_blocks(visit)?;
    }
    if let Some(error) = callback_error {
        return Err(error);
    }
    Ok(!saw_gap)
}

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(3),
        Err(error) => {
            eprintln!("tfdb-rs-dump: {error}");
            ExitCode::from(2)
        }
    }
}
