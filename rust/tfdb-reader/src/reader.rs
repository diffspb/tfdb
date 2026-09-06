// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use std::collections::HashSet;
use std::fs::File;
use std::io::{self, Seek, SeekFrom};
use std::os::unix::fs::FileExt;
use std::path::Path;

use crate::format::{
    align_up, decode_block_header, decode_footer, decode_index_entry, decode_partition_header,
    decode_volume_header, BlockHeader, IndexEntry, PartitionFooter, PartitionHeader, VolumeHeader,
    BLOCK_HEADER_SIZE, FOOTER_REGION_SIZE, FOOTER_SIZE, INDEX_ENTRY_SIZE,
    PARTITION_HEADER_REGION_SIZE, PARTITION_HEADER_SIZE, VOLUME_HEADER_COPY_SIZE,
    VOLUME_HEADER_SIZE, VOLUME_PREFIX_SIZE,
};
use crate::{crc32c, decompress_packbits, Error, ErrorKind, Result};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GapKind {
    Corrupt,
    Overwritten,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Gap {
    pub kind: GapKind,
    pub partition_slot: u32,
    pub partition_generation: u64,
    pub block_sequence: Option<u32>,
    pub message: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BlockMetadata {
    pub partition_slot: u32,
    pub partition_generation: u64,
    pub block_sequence: u32,
    pub record_count: u32,
    pub min_time_ns: i64,
    pub max_time_ns: i64,
    pub flags: u32,
    pub compression_id: u16,
    pub stored_size: u32,
    pub raw_size: u32,
    pub physical_offset: u64,
    pub record_format_id: u64,
    pub record_format_version: u16,
    pub time_domain_id: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Block {
    pub metadata: BlockMetadata,
    pub data: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Event {
    Data(Block),
    Gap(Gap),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeRange {
    pub begin_ns: i64,
    pub end_ns: i64,
    pub time_domain_id: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PartitionInfo {
    pub slot: u32,
    pub generation: u64,
    pub sealed: bool,
    pub block_count: u32,
    pub data_span_bytes: u64,
    pub min_time_ns: i64,
    pub max_time_ns: i64,
    pub flags: u32,
    pub compression_id: u16,
    pub compression_version: u16,
    pub record_format_id: u64,
    pub record_format_version: u16,
    pub time_domain_id: u64,
}

#[derive(Clone, Debug)]
struct PartitionState {
    header: PartitionHeader,
    footer: Option<PartitionFooter>,
    active_entries: Vec<IndexEntry>,
    data_end: u64,
    block_count: u32,
    min_time_ns: i64,
    max_time_ns: i64,
    flags: u32,
}

struct ScanResult {
    entries: Vec<IndexEntry>,
    data_end: u64,
    stopped_on_corruption: bool,
}

pub struct Reader {
    file: File,
    backend_size: u64,
    volume: VolumeHeader,
    partitions: Vec<PartitionState>,
    header_gaps: Vec<Gap>,
}

impl Reader {
    /// Open a regular file or a seekable Linux block device read-only.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let file = File::open(path)?;
        let metadata_size = file.metadata()?.len();
        let backend_size = if metadata_size != 0 {
            metadata_size
        } else {
            let mut duplicate = file.try_clone()?;
            duplicate.seek(SeekFrom::End(0))?
        };
        Self::from_file_with_size(file, backend_size)
    }

    /// Open an already-owned descriptor with an explicit size. This is useful
    /// for block-device wrappers where seeking to the end is unavailable.
    pub fn from_file_with_size(file: File, backend_size: u64) -> Result<Self> {
        let first = read_decode(&file, 0, VOLUME_HEADER_SIZE, decode_volume_header);
        let second = read_decode(
            &file,
            VOLUME_HEADER_COPY_SIZE,
            VOLUME_HEADER_SIZE,
            decode_volume_header,
        );
        let volume = match (first, second) {
            (Ok(a), Ok(b)) => {
                if a != b {
                    return Err(Error::corrupt("valid volume header copies disagree"));
                }
                a
            }
            (Ok(value), Err(_)) | (Err(_), Ok(value)) => value,
            (Err(first), Err(second)) => {
                if first.kind() == ErrorKind::Unsupported {
                    return Err(first);
                }
                if second.kind() == ErrorKind::Unsupported {
                    return Err(second);
                }
                if first.kind() == ErrorKind::Io {
                    return Err(first);
                }
                if second.kind() == ErrorKind::Io {
                    return Err(second);
                }
                return Err(Error::corrupt("both volume headers are invalid"));
            }
        };
        validate_volume_geometry(&volume, backend_size)?;

        let mut partitions = Vec::new();
        let mut header_gaps = Vec::new();
        let mut generations = HashSet::new();
        for slot in 0..volume.partition_count {
            let base = partition_base(&volume, slot)?;
            let raw = read_vec(&file, base, PARTITION_HEADER_SIZE)?;
            let header = match decode_partition_header(&raw).and_then(|header| {
                validate_partition_header(&volume, slot, &header)?;
                Ok(header)
            }) {
                Ok(header) => header,
                Err(error) => {
                    let evidence =
                        partition_has_current_volume_evidence(&file, &volume, slot, &raw)?;
                    if evidence {
                        if error.kind() == ErrorKind::Unsupported {
                            return Err(error);
                        }
                        header_gaps.push(Gap {
                            kind: GapKind::Corrupt,
                            partition_slot: slot,
                            partition_generation: 0,
                            block_sequence: None,
                            message: "partition header is damaged".into(),
                        });
                    }
                    continue;
                }
            };
            if !generations.insert(header.generation) {
                return Err(Error::corrupt("duplicate partition generation"));
            }

            let footer_offset = base
                .checked_add(volume.partition_size - FOOTER_REGION_SIZE)
                .ok_or_else(|| Error::corrupt("partition footer offset overflow"))?;
            let footer_result = read_decode(&file, footer_offset, FOOTER_SIZE, decode_footer);
            let footer = match footer_result {
                Ok(footer) => match validate_footer(&volume, &header, &footer) {
                    Ok(()) => Some(footer),
                    Err(error) if error.kind() == ErrorKind::Corrupt => None,
                    Err(error) => return Err(error),
                },
                Err(error) if error.kind() == ErrorKind::Corrupt => None,
                Err(error) => return Err(error),
            };

            let state = if let Some(footer) = footer {
                PartitionState {
                    header,
                    data_end: footer.data_end,
                    block_count: footer.block_count,
                    min_time_ns: footer.min_time_ns,
                    max_time_ns: footer.max_time_ns,
                    flags: footer.flags,
                    footer: Some(footer),
                    active_entries: Vec::new(),
                }
            } else {
                let scan = scan_partition(&file, &volume, &header, index_begin(&volume)?)?;
                let (flags, min_time_ns, max_time_ns) = summarize(&scan.entries);
                PartitionState {
                    header,
                    footer: None,
                    block_count: scan
                        .entries
                        .len()
                        .try_into()
                        .map_err(|_| Error::corrupt("block count exceeds u32"))?,
                    data_end: scan.data_end,
                    min_time_ns,
                    max_time_ns,
                    flags,
                    active_entries: scan.entries,
                }
            };
            partitions.push(state);
        }
        partitions.sort_by_key(|partition| partition.header.generation);

        Ok(Self {
            file,
            backend_size,
            volume,
            partitions,
            header_gaps,
        })
    }

    pub fn volume(&self) -> &VolumeHeader {
        &self.volume
    }
    pub fn backend_size(&self) -> u64 {
        self.backend_size
    }
    pub fn initial_gaps(&self) -> &[Gap] {
        &self.header_gaps
    }

    pub fn partitions(&self) -> Vec<PartitionInfo> {
        self.partitions
            .iter()
            .map(|state| PartitionInfo {
                slot: state.header.slot,
                generation: state.header.generation,
                sealed: state.footer.is_some(),
                block_count: state.block_count,
                data_span_bytes: state.data_end - PARTITION_HEADER_REGION_SIZE,
                min_time_ns: state.min_time_ns,
                max_time_ns: state.max_time_ns,
                flags: state.flags,
                compression_id: state.header.compression_id,
                compression_version: state.header.compression_version,
                record_format_id: state.header.record_format_id,
                record_format_version: state.header.record_format_version,
                time_domain_id: state.header.time_domain_id,
            })
            .collect()
    }

    pub fn scan_blocks<F>(&self, visitor: F) -> Result<()>
    where
        F: FnMut(Event) -> bool,
    {
        self.visit_blocks(None, visitor)
    }

    pub fn query_blocks<F>(&self, range: TimeRange, visitor: F) -> Result<()>
    where
        F: FnMut(Event) -> bool,
    {
        if range.end_ns <= range.begin_ns {
            return Err(Error::invalid("time range must be nonempty [begin,end)"));
        }
        if range.time_domain_id == 0 {
            return Err(Error::invalid(
                "time-domain zero is reserved for physical scans",
            ));
        }
        self.visit_blocks(Some(range), visitor)
    }

    fn visit_blocks<F>(&self, range: Option<TimeRange>, mut visitor: F) -> Result<()>
    where
        F: FnMut(Event) -> bool,
    {
        for gap in &self.header_gaps {
            if !visitor(Event::Gap(gap.clone())) {
                return Ok(());
            }
        }
        for partition in &self.partitions {
            if let Some(range) = range {
                if partition.header.time_domain_id != range.time_domain_id {
                    continue;
                }
            }
            let (entries, acquisition_gap) = self.acquire_entries(partition)?;
            if let Some(gap) = acquisition_gap {
                if !visitor(Event::Gap(gap)) {
                    return Ok(());
                }
            }
            for entry in entries {
                if let Some(range) = range {
                    if !(entry.min_time_ns < range.end_ns && entry.max_time_ns >= range.begin_ns) {
                        continue;
                    }
                }
                match self.read_block(partition, &entry) {
                    Ok(block) => {
                        if !visitor(Event::Data(block)) {
                            return Ok(());
                        }
                    }
                    Err(error) => {
                        let overwritten = error.kind() == ErrorKind::Overwritten;
                        let gap = Gap {
                            kind: if overwritten {
                                GapKind::Overwritten
                            } else {
                                GapKind::Corrupt
                            },
                            partition_slot: partition.header.slot,
                            partition_generation: partition.header.generation,
                            block_sequence: Some(entry.sequence),
                            message: error.message().into(),
                        };
                        if !visitor(Event::Gap(gap)) {
                            return Ok(());
                        }
                        if overwritten {
                            break;
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn acquire_entries(&self, state: &PartitionState) -> Result<(Vec<IndexEntry>, Option<Gap>)> {
        let Some(footer) = &state.footer else {
            return Ok((state.active_entries.clone(), None));
        };
        match load_index(&self.file, &self.volume, &state.header, footer) {
            Ok(entries) => Ok((entries, None)),
            Err(error) if error.kind() == ErrorKind::Corrupt => {
                let scan =
                    scan_partition(&self.file, &self.volume, &state.header, footer.data_end)?;
                let complete = !scan.stopped_on_corruption
                    && scan.data_end == footer.data_end
                    && scan.entries.len() == footer.block_count as usize;
                let message = if complete {
                    format!(
                        "sealed time index is invalid and was rebuilt: {}",
                        error.message()
                    )
                } else {
                    "sealed partition has an invalid data/index prefix".into()
                };
                Ok((
                    scan.entries,
                    Some(Gap {
                        kind: GapKind::Corrupt,
                        partition_slot: state.header.slot,
                        partition_generation: state.header.generation,
                        block_sequence: None,
                        message,
                    }),
                ))
            }
            Err(error) => Err(error),
        }
    }

    fn read_block(&self, state: &PartitionState, entry: &IndexEntry) -> Result<Block> {
        let before = match self.read_current_partition_header(state.header.slot) {
            Ok(value) => value,
            Err(error) => return self.classify_snapshot_failure(state, error),
        };
        if before.generation != state.header.generation {
            return Err(Error::overwritten("partition changed before block read"));
        }
        let base = partition_base(&self.volume, state.header.slot)?;
        let physical_offset = base
            .checked_add(entry.offset)
            .ok_or_else(|| Error::corrupt("block physical offset overflow"))?;
        let raw_header = read_vec(&self.file, physical_offset, BLOCK_HEADER_SIZE)?;
        let block = match decode_block_header(&raw_header) {
            Ok(value) => value,
            Err(error) => return self.classify_snapshot_failure(state, error),
        };
        if let Err(error) = validate_block_against_entry(&self.volume, &state.header, &block, entry)
        {
            return self.classify_snapshot_failure(state, error);
        }
        let stored = read_vec(
            &self.file,
            physical_offset
                .checked_add(BLOCK_HEADER_SIZE as u64)
                .ok_or_else(|| Error::corrupt("block payload offset overflow"))?,
            block.stored_size as usize,
        )?;
        let after = match self.read_current_partition_header(state.header.slot) {
            Ok(value) => value,
            Err(error) => return self.classify_snapshot_failure(state, error),
        };
        if after.generation != state.header.generation {
            return Err(Error::overwritten(
                "partition changed while reading block payload",
            ));
        }
        if crc32c(&stored) != block.payload_crc {
            return self
                .classify_snapshot_failure(state, Error::corrupt("block payload CRC mismatch"));
        }
        let data = match block.compression_id {
            0 => {
                if block.compression_version != 1 || block.stored_size != block.raw_size {
                    return Err(Error::corrupt("invalid uncompressed block sizes/version"));
                }
                stored
            }
            1 if block.compression_version == 1 => {
                decompress_packbits(&stored, block.raw_size as usize)?
            }
            _ => {
                return Err(Error::unsupported(
                    "block compression codec/version unavailable",
                ))
            }
        };
        Ok(Block {
            metadata: BlockMetadata {
                partition_slot: state.header.slot,
                partition_generation: state.header.generation,
                block_sequence: block.sequence,
                record_count: block.record_count,
                min_time_ns: block.min_time_ns,
                max_time_ns: block.max_time_ns,
                flags: block.flags,
                compression_id: block.compression_id,
                stored_size: block.stored_size,
                raw_size: block.raw_size,
                physical_offset,
                record_format_id: state.header.record_format_id,
                record_format_version: state.header.record_format_version,
                time_domain_id: state.header.time_domain_id,
            },
            data,
        })
    }

    fn read_current_partition_header(&self, slot: u32) -> Result<PartitionHeader> {
        let base = partition_base(&self.volume, slot)?;
        let bytes = read_vec(&self.file, base, PARTITION_HEADER_SIZE)?;
        let header = decode_partition_header(&bytes)?;
        validate_partition_header(&self.volume, slot, &header)?;
        Ok(header)
    }

    fn classify_snapshot_failure<T>(&self, state: &PartitionState, fallback: Error) -> Result<T> {
        match self.read_current_partition_header(state.header.slot) {
            Ok(header) if header.generation != state.header.generation => {
                Err(Error::overwritten("partition changed while reading block"))
            }
            _ => Err(fallback),
        }
    }
}

fn read_decode<T>(
    file: &File,
    offset: u64,
    size: usize,
    decoder: fn(&[u8]) -> Result<T>,
) -> Result<T> {
    let bytes = read_vec(file, offset, size)?;
    decoder(&bytes)
}

fn read_vec(file: &File, offset: u64, size: usize) -> Result<Vec<u8>> {
    let mut bytes = vec![0u8; size];
    let mut completed = 0usize;
    while completed < size {
        let current = offset
            .checked_add(completed as u64)
            .ok_or_else(|| Error::corrupt("read offset overflow"))?;
        match file.read_at(&mut bytes[completed..], current) {
            Ok(0) => {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "short TFDB read").into())
            }
            Ok(amount) => completed += amount,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(bytes)
}

fn partition_base(volume: &VolumeHeader, slot: u32) -> Result<u64> {
    VOLUME_PREFIX_SIZE
        .checked_add(
            u64::from(slot)
                .checked_mul(volume.partition_size)
                .ok_or_else(|| Error::corrupt("partition offset multiplication overflow"))?,
        )
        .ok_or_else(|| Error::corrupt("partition offset overflow"))
}

fn index_begin(volume: &VolumeHeader) -> Result<u64> {
    volume
        .partition_size
        .checked_sub(FOOTER_REGION_SIZE)
        .and_then(|value| value.checked_sub(u64::from(volume.index_region_size)))
        .ok_or_else(|| Error::corrupt("invalid partition reserved regions"))
}

fn validate_volume_geometry(volume: &VolumeHeader, backend_size: u64) -> Result<()> {
    let quantum = volume.persistence_quantum;
    if volume.volume_size < VOLUME_PREFIX_SIZE
        || volume.volume_size > backend_size
        || quantum == 0
        || !quantum.is_power_of_two()
        || quantum > 4096
        || VOLUME_PREFIX_SIZE % u64::from(quantum) != 0
        || PARTITION_HEADER_REGION_SIZE % u64::from(quantum) != 0
        || volume.partition_size % u64::from(quantum) != 0
        || u64::from(volume.index_region_size) % u64::from(quantum) != 0
        || volume.index_region_size < INDEX_ENTRY_SIZE as u32
    {
        return Err(Error::corrupt("invalid volume geometry/alignment"));
    }
    let fixed = PARTITION_HEADER_REGION_SIZE
        .checked_add(FOOTER_REGION_SIZE)
        .and_then(|value| value.checked_add(u64::from(volume.index_region_size)))
        .ok_or_else(|| Error::corrupt("volume fixed-region size overflow"))?;
    if fixed >= volume.partition_size || volume.partition_size - fixed < u64::from(quantum) {
        return Err(Error::corrupt("partition data region is too small"));
    }
    let occupied = u64::from(volume.partition_count)
        .checked_mul(volume.partition_size)
        .and_then(|value| value.checked_add(VOLUME_PREFIX_SIZE))
        .ok_or_else(|| Error::corrupt("volume partition geometry overflow"))?;
    if occupied > volume.volume_size {
        return Err(Error::corrupt("partitions exceed recorded volume size"));
    }
    let maximum_span = align_up(
        BLOCK_HEADER_SIZE as u64 + u64::from(volume.max_block_payload),
        quantum,
    )
    .ok_or_else(|| Error::corrupt("maximum block span overflow"))?;
    if maximum_span > volume.partition_size - fixed {
        return Err(Error::corrupt("maximum block cannot fit partition"));
    }
    Ok(())
}

fn validate_partition_header(
    volume: &VolumeHeader,
    slot: u32,
    header: &PartitionHeader,
) -> Result<()> {
    if header.volume_id_high != volume.volume_id_high
        || header.volume_id_low != volume.volume_id_low
        || header.slot != slot
        || header.generation == 0
        || header.max_block_payload != volume.max_block_payload
        || header.persistence_quantum != volume.persistence_quantum
        || header.time_domain_id == 0
    {
        return Err(Error::corrupt("partition identity/geometry mismatch"));
    }
    match (header.compression_id, header.compression_version) {
        (0, 1) | (1, 1) => {}
        _ => {
            return Err(Error::unsupported(
                "partition compression codec/version unavailable",
            ))
        }
    }
    let expected_index = index_begin(volume)?;
    let mut saw_time_index = false;
    for feature in &header.features {
        let end = feature
            .region_offset
            .checked_add(feature.region_length)
            .ok_or_else(|| Error::corrupt("feature region overflow"))?;
        if end > volume.partition_size {
            return Err(Error::corrupt("feature region outside partition"));
        }
        if feature.kind == 2 {
            saw_time_index = true;
            if feature.region_offset != expected_index
                || feature.region_length != u64::from(volume.index_region_size)
            {
                return Err(Error::corrupt("time-index region mismatch"));
            }
        } else if !matches!(feature.kind, 1 | 3 | 4) && feature.region_length != 0 {
            return Err(Error::unsupported("unknown feature reserves media space"));
        }
    }
    if !saw_time_index {
        return Err(Error::corrupt("missing time-index feature"));
    }
    Ok(())
}

fn validate_footer(
    volume: &VolumeHeader,
    header: &PartitionHeader,
    footer: &PartitionFooter,
) -> Result<()> {
    if footer.volume_id_high != volume.volume_id_high
        || footer.volume_id_low != volume.volume_id_low
        || footer.slot != header.slot
        || footer.generation != header.generation
        || footer.index_offset != index_begin(volume)?
        || footer.index_size > volume.index_region_size
        || footer.data_end < PARTITION_HEADER_REGION_SIZE
        || footer.data_end > index_begin(volume)?
        || footer.data_end % u64::from(volume.persistence_quantum) != 0
        || u64::from(footer.index_size) != u64::from(footer.block_count) * INDEX_ENTRY_SIZE as u64
        || (footer.block_count == 0 && footer.data_end != PARTITION_HEADER_REGION_SIZE)
        || (footer.block_count != 0 && footer.min_time_ns > footer.max_time_ns)
    {
        return Err(Error::corrupt("partition footer identity/bounds mismatch"));
    }
    Ok(())
}

fn partition_has_current_volume_evidence(
    file: &File,
    volume: &VolumeHeader,
    slot: u32,
    raw_header: &[u8],
) -> Result<bool> {
    if raw_header.iter().all(|&byte| byte == 0) {
        return Ok(false);
    }
    let same_magic = raw_header.get(..8) == Some(b"TFDBPAR1");
    let same_id = raw_header.len() >= 32
        && u64::from_le_bytes(raw_header[16..24].try_into().unwrap()) == volume.volume_id_high
        && u64::from_le_bytes(raw_header[24..32].try_into().unwrap()) == volume.volume_id_low;
    if same_magic && same_id {
        return Ok(true);
    }

    let base = partition_base(volume, slot)?;
    let footer_bytes = read_vec(
        file,
        base + volume.partition_size - FOOTER_REGION_SIZE,
        FOOTER_SIZE,
    )?;
    if let Ok(footer) = decode_footer(&footer_bytes) {
        if footer.slot == slot
            && footer.volume_id_high == volume.volume_id_high
            && footer.volume_id_low == volume.volume_id_low
        {
            return Ok(true);
        }
    }
    let block_bytes = read_vec(file, base + PARTITION_HEADER_REGION_SIZE, BLOCK_HEADER_SIZE)?;
    if let Ok(block) = decode_block_header(&block_bytes) {
        if block.slot == slot
            && block.volume_id_high == volume.volume_id_high
            && block.volume_id_low == volume.volume_id_low
        {
            return Ok(true);
        }
    }
    Ok(false)
}

fn scan_partition(
    file: &File,
    volume: &VolumeHeader,
    header: &PartitionHeader,
    limit: u64,
) -> Result<ScanResult> {
    let index_limit = index_begin(volume)?;
    if limit < PARTITION_HEADER_REGION_SIZE || limit > index_limit {
        return Err(Error::corrupt("partition scan limit outside data region"));
    }
    let base = partition_base(volume, header.slot)?;
    let mut cursor = PARTITION_HEADER_REGION_SIZE;
    let mut expected_sequence = 0u32;
    let mut expected_writer = (0u64, 0u64);
    let mut entries = Vec::new();
    let mut stopped = false;
    while cursor
        .checked_add(BLOCK_HEADER_SIZE as u64)
        .map_or(false, |end| end <= limit)
    {
        let raw = read_vec(file, base + cursor, BLOCK_HEADER_SIZE)?;
        let block = match decode_block_header(&raw) {
            Ok(value) => value,
            Err(_) => {
                stopped = true;
                break;
            }
        };
        let expected_span = align_up(
            BLOCK_HEADER_SIZE as u64 + u64::from(block.stored_size),
            volume.persistence_quantum,
        );
        let valid = block.generation == header.generation
            && block.slot == header.slot
            && block.volume_id_high == volume.volume_id_high
            && block.volume_id_low == volume.volume_id_low
            && (block.previous_writer_id_high, block.previous_writer_id_low) == expected_writer
            && block.sequence == expected_sequence
            && block.record_count != 0
            && block.min_time_ns <= block.max_time_ns
            && block.stored_size <= volume.max_block_payload
            && block.raw_size <= volume.max_block_payload
            && (if block.compression_id == 0 {
                block.compression_version == 1 && block.stored_size == block.raw_size
            } else {
                block.compression_id == header.compression_id
                    && block.compression_version == header.compression_version
            })
            && block.frame_span % volume.persistence_quantum == 0
            && cursor % u64::from(volume.persistence_quantum) == 0
            && u64::from(block.frame_span) <= limit - cursor
            && expected_span == Some(u64::from(block.frame_span));
        if !valid {
            stopped = true;
            break;
        }
        let stored = read_vec(
            file,
            base + cursor + BLOCK_HEADER_SIZE as u64,
            block.stored_size as usize,
        )?;
        if crc32c(&stored) != block.payload_crc {
            stopped = true;
            break;
        }
        entries.push(IndexEntry {
            offset: cursor,
            frame_size: BLOCK_HEADER_SIZE as u32 + block.stored_size,
            frame_span: block.frame_span,
            sequence: block.sequence,
            record_count: block.record_count,
            min_time_ns: block.min_time_ns,
            max_time_ns: block.max_time_ns,
            flags: block.flags,
            raw_size: block.raw_size,
            writer_id_high: block.writer_id_high,
            writer_id_low: block.writer_id_low,
        });
        expected_writer = (block.writer_id_high, block.writer_id_low);
        cursor += u64::from(block.frame_span);
        expected_sequence = expected_sequence
            .checked_add(1)
            .ok_or_else(|| Error::corrupt("block sequence overflow"))?;
    }
    Ok(ScanResult {
        entries,
        data_end: cursor,
        stopped_on_corruption: stopped,
    })
}

fn load_index(
    file: &File,
    volume: &VolumeHeader,
    header: &PartitionHeader,
    footer: &PartitionFooter,
) -> Result<Vec<IndexEntry>> {
    if footer.index_size == 0 {
        if footer.block_count != 0 {
            return Err(Error::corrupt("empty index has blocks"));
        }
        return Ok(Vec::new());
    }
    let bytes = read_vec(
        file,
        partition_base(volume, header.slot)? + footer.index_offset,
        footer.index_size as usize,
    )?;
    if crc32c(&bytes) != footer.index_crc {
        return Err(Error::corrupt("partition time-index CRC mismatch"));
    }
    let mut entries = Vec::with_capacity(footer.block_count as usize);
    for chunk in bytes.chunks_exact(INDEX_ENTRY_SIZE) {
        entries.push(decode_index_entry(chunk)?);
    }
    if entries.len() != footer.block_count as usize {
        return Err(Error::corrupt("partition time-index count mismatch"));
    }
    let mut expected_offset = PARTITION_HEADER_REGION_SIZE;
    for (sequence, entry) in entries.iter().enumerate() {
        let expected_span = align_up(u64::from(entry.frame_size), volume.persistence_quantum);
        let remaining = index_begin(volume)?.checked_sub(entry.offset);
        if entry.sequence as usize != sequence
            || entry.offset != expected_offset
            || entry.record_count == 0
            || entry.min_time_ns > entry.max_time_ns
            || entry.frame_size < BLOCK_HEADER_SIZE as u32
            || entry.frame_size > BLOCK_HEADER_SIZE as u32 + volume.max_block_payload
            || entry.raw_size > volume.max_block_payload
            || entry.frame_span % volume.persistence_quantum != 0
            || remaining.map_or(true, |bytes| u64::from(entry.frame_span) > bytes)
            || expected_span != Some(u64::from(entry.frame_span))
        {
            return Err(Error::corrupt(
                "partition time-index sequence/bounds mismatch",
            ));
        }
        expected_offset = expected_offset
            .checked_add(u64::from(entry.frame_span))
            .ok_or_else(|| Error::corrupt("time-index offset overflow"))?;
    }
    if expected_offset != footer.data_end {
        return Err(Error::corrupt("partition time-index data end mismatch"));
    }
    let (flags, min_time, max_time) = summarize(&entries);
    if flags != footer.flags || min_time != footer.min_time_ns || max_time != footer.max_time_ns {
        return Err(Error::corrupt(
            "partition footer summary disagrees with index",
        ));
    }
    Ok(entries)
}

fn validate_block_against_entry(
    volume: &VolumeHeader,
    partition: &PartitionHeader,
    block: &BlockHeader,
    entry: &IndexEntry,
) -> Result<()> {
    let identity_ok = block.generation == partition.generation
        && block.slot == partition.slot
        && block.volume_id_high == volume.volume_id_high
        && block.volume_id_low == volume.volume_id_low
        && block.sequence == entry.sequence
        && ((entry.writer_id_high == 0 && entry.writer_id_low == 0)
            || (block.writer_id_high == entry.writer_id_high
                && block.writer_id_low == entry.writer_id_low));
    let metadata_ok = BLOCK_HEADER_SIZE as u32 + block.stored_size == entry.frame_size
        && block.frame_span == entry.frame_span
        && block.raw_size <= volume.max_block_payload
        && block.stored_size <= volume.max_block_payload
        && block.record_count == entry.record_count
        && block.raw_size == entry.raw_size
        && block.min_time_ns == entry.min_time_ns
        && block.max_time_ns == entry.max_time_ns
        && block.flags == entry.flags
        && ((block.compression_id == 0
            && block.compression_version == 1
            && block.stored_size == block.raw_size)
            || (block.compression_id == partition.compression_id
                && block.compression_version == partition.compression_version));
    let span_ok = align_up(
        BLOCK_HEADER_SIZE as u64 + u64::from(block.stored_size),
        volume.persistence_quantum,
    ) == Some(u64::from(block.frame_span));
    if !identity_ok || !metadata_ok || !span_ok {
        return Err(Error::corrupt(
            "block metadata disagrees with partition/index",
        ));
    }
    Ok(())
}

fn summarize(entries: &[IndexEntry]) -> (u32, i64, i64) {
    if entries.is_empty() {
        return (0, 0, 0);
    }
    let mut flags = 0u32;
    let mut minimum = entries[0].min_time_ns;
    let mut maximum = entries[0].max_time_ns;
    for entry in entries {
        flags |= entry.flags;
        minimum = minimum.min(entry.min_time_ns);
        maximum = maximum.max(entry.max_time_ns);
    }
    (flags, minimum, maximum)
}
