use std::process::ExitCode;

use tfdb_reader::{decode_framed_v1, Event, Reader, FRAMED_RECORD_V1_PROFILE_ID};

fn run() -> Result<bool, Box<dyn std::error::Error>> {
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
    let mut blocks = 0u64;
    let mut records = 0u64;
    let mut raw_bytes = 0u64;
    let mut gaps = 0u64;
    let mut profile_error = None;
    reader.scan_blocks(|event| {
        match event {
            Event::Data(block) => {
                blocks += 1;
                raw_bytes += block.data.len() as u64;
                if block.metadata.record_format_id == FRAMED_RECORD_V1_PROFILE_ID
                    && block.metadata.record_format_version == 1
                {
                    match decode_framed_v1(&block.data) {
                        Ok(decoded) if decoded.len() == block.metadata.record_count as usize => {
                            records += decoded.len() as u64;
                        }
                        Ok(_) => {
                            profile_error =
                                Some("FramedRecordV1 count differs from block header".to_string())
                        }
                        Err(error) => profile_error = Some(error.to_string()),
                    }
                }
            }
            Event::Gap(gap) => {
                gaps += 1;
                eprintln!(
                    "gap generation={} block={:?} reason={}",
                    gap.partition_generation, gap.block_sequence, gap.message
                );
            }
        }
        profile_error.is_none()
    })?;
    if let Some(error) = profile_error {
        return Err(error.into());
    }
    println!("verified blocks={blocks} records={records} raw_bytes={raw_bytes} gaps={gaps}");
    Ok(gaps == 0)
}

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(3),
        Err(error) => {
            eprintln!("tfdb-rs-verify: {error}");
            ExitCode::from(2)
        }
    }
}
