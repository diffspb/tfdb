// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use crate::{crc32c, Error, Result};

pub const VOLUME_HEADER_SIZE: usize = 256;
pub const VOLUME_HEADER_COPY_SIZE: u64 = 4096;
pub const VOLUME_PREFIX_SIZE: u64 = 8192;
pub const PARTITION_HEADER_SIZE: usize = 1024;
pub const PARTITION_HEADER_REGION_SIZE: u64 = 4096;
pub const FOOTER_REGION_SIZE: u64 = 4096;
pub const FOOTER_SIZE: usize = 256;
pub const BLOCK_HEADER_SIZE: usize = 128;
pub const INDEX_ENTRY_SIZE: usize = 48;
pub const FEATURE_DESCRIPTOR_SIZE: usize = 40;
pub const FRAMED_RECORD_V1_PROFILE_ID: u64 = 0x5446_4442_5246_5631;

const FEATURE_REQUIRED: u16 = 1;
const FEATURE_COMPRESSION: u16 = 1;
const FEATURE_TIME_INDEX: u16 = 2;
const FEATURE_RECORD_PROFILE: u16 = 3;
const FEATURE_INTEGRITY: u16 = 4;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VolumeHeader {
    pub volume_id_high: u64,
    pub volume_id_low: u64,
    pub volume_size: u64,
    pub partition_size: u64,
    pub partition_count: u32,
    pub index_region_size: u32,
    pub max_block_payload: u32,
    pub persistence_quantum: u32,
    pub created_time_ns: i64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Feature {
    pub kind: u16,
    pub flags: u16,
    pub algorithm: u32,
    pub version: u16,
    pub config_offset: u32,
    pub config_length: u32,
    pub region_offset: u64,
    pub region_length: u64,
}

impl Feature {
    pub fn required(&self) -> bool {
        self.flags & FEATURE_REQUIRED != 0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PartitionHeader {
    pub volume_id_high: u64,
    pub volume_id_low: u64,
    pub generation: u64,
    pub slot: u32,
    pub created_time_ns: i64,
    pub max_block_payload: u32,
    pub persistence_quantum: u32,
    pub allowed_backward_skew_ns: i64,
    pub allowed_forward_step_ns: i64,
    pub time_domain_id: u64,
    pub compression_id: u16,
    pub compression_version: u16,
    pub record_format_id: u64,
    pub record_format_version: u16,
    pub features: Vec<Feature>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BlockHeader {
    pub generation: u64,
    pub sequence: u32,
    pub flags: u32,
    pub record_count: u32,
    pub stored_size: u32,
    pub raw_size: u32,
    pub frame_span: u32,
    pub min_time_ns: i64,
    pub max_time_ns: i64,
    pub compression_id: u16,
    pub compression_version: u16,
    pub payload_crc: u32,
    pub slot: u32,
    pub volume_id_low: u64,
    pub volume_id_high: u64,
    pub writer_id_high: u64,
    pub writer_id_low: u64,
    pub previous_writer_id_high: u64,
    pub previous_writer_id_low: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IndexEntry {
    pub offset: u64,
    pub frame_size: u32,
    pub frame_span: u32,
    pub sequence: u32,
    pub record_count: u32,
    pub min_time_ns: i64,
    pub max_time_ns: i64,
    pub flags: u32,
    pub raw_size: u32,
    pub(crate) writer_id_high: u64,
    pub(crate) writer_id_low: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PartitionFooter {
    pub volume_id_high: u64,
    pub volume_id_low: u64,
    pub generation: u64,
    pub slot: u32,
    pub flags: u32,
    pub block_count: u32,
    pub index_offset: u64,
    pub index_size: u32,
    pub data_end: u64,
    pub min_time_ns: i64,
    pub max_time_ns: i64,
    pub index_crc: u32,
}

fn get16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn get32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn get64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

fn geti64(bytes: &[u8], offset: usize) -> i64 {
    i64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

fn reserved_zero(bytes: &[u8], begin: usize, end: usize, what: &str) -> Result<()> {
    if bytes[begin..end].iter().any(|&value| value != 0) {
        Err(Error::corrupt(format!("{what} reserved bytes are nonzero")))
    } else {
        Ok(())
    }
}

fn common<'a>(
    bytes: &'a [u8],
    magic: &[u8; 8],
    encoded_size: usize,
    crc_offset: usize,
) -> Result<&'a [u8]> {
    if bytes.len() < encoded_size {
        return Err(Error::corrupt("truncated persistent structure"));
    }
    let bytes = &bytes[..encoded_size];
    if &bytes[..8] != magic {
        return Err(Error::corrupt("persistent structure magic mismatch"));
    }
    if get16(bytes, 8) != 1 {
        return Err(Error::unsupported("unsupported TFDB format major"));
    }
    if get16(bytes, 10) > 0 {
        return Err(Error::unsupported("unsupported TFDB format minor"));
    }
    if get32(bytes, 12) as usize != encoded_size {
        return Err(Error::corrupt("persistent structure size mismatch"));
    }
    let expected = get32(bytes, crc_offset);
    let mut checked = bytes.to_vec();
    checked[crc_offset..crc_offset + 4].fill(0);
    if crc32c(&checked) != expected {
        return Err(Error::corrupt("persistent structure CRC32C mismatch"));
    }
    Ok(bytes)
}

pub fn decode_volume_header(input: &[u8]) -> Result<VolumeHeader> {
    let bytes = common(input, b"TFDBVOL1", VOLUME_HEADER_SIZE, 72)?;
    reserved_zero(bytes, 76, VOLUME_HEADER_SIZE, "volume header")?;
    let result = VolumeHeader {
        volume_id_high: get64(bytes, 16),
        volume_id_low: get64(bytes, 24),
        volume_size: get64(bytes, 32),
        partition_size: get64(bytes, 40),
        partition_count: get32(bytes, 48),
        index_region_size: get32(bytes, 52),
        max_block_payload: get32(bytes, 56),
        persistence_quantum: get32(bytes, 60),
        created_time_ns: geti64(bytes, 64),
    };
    if result.volume_id_high == 0 && result.volume_id_low == 0 {
        return Err(Error::corrupt("zero volume ID"));
    }
    if result.partition_count < 2
        || result.partition_size == 0
        || result.max_block_payload == 0
        || result.persistence_quantum == 0
    {
        return Err(Error::corrupt("invalid volume geometry"));
    }
    Ok(result)
}

pub fn decode_partition_header(input: &[u8]) -> Result<PartitionHeader> {
    let bytes = common(input, b"TFDBPAR1", PARTITION_HEADER_SIZE, 104)?;
    reserved_zero(bytes, 44, 48, "partition header")?;
    reserved_zero(bytes, 108, 128, "partition header")?;

    let count = usize::from(get16(bytes, 88));
    let descriptor_size = usize::from(get16(bytes, 90));
    let descriptor_offset = get32(bytes, 92) as usize;
    let config_offset = get32(bytes, 96) as usize;
    let config_length = get32(bytes, 100) as usize;
    if !(4..=8).contains(&count)
        || descriptor_size != FEATURE_DESCRIPTOR_SIZE
        || descriptor_offset != 128
        || config_offset != 512
        || config_length != 16
        || descriptor_offset + count * descriptor_size > 448
    {
        return Err(Error::corrupt("invalid partition feature directory"));
    }

    let mut features = Vec::with_capacity(count);
    let mut ranges: Vec<(usize, usize)> = Vec::new();
    let mut compression: Option<(u16, u16)> = None;
    let mut time_index = false;
    let mut record_profile: Option<(u64, u16)> = None;
    let mut integrity = false;
    for index in 0..count {
        let offset = descriptor_offset + index * descriptor_size;
        reserved_zero(bytes, offset + 10, offset + 12, "feature descriptor")?;
        reserved_zero(bytes, offset + 20, offset + 24, "feature descriptor")?;
        let feature = Feature {
            kind: get16(bytes, offset),
            flags: get16(bytes, offset + 2),
            algorithm: get32(bytes, offset + 4),
            version: get16(bytes, offset + 8),
            config_offset: get32(bytes, offset + 12),
            config_length: get32(bytes, offset + 16),
            region_offset: get64(bytes, offset + 24),
            region_length: get64(bytes, offset + 32),
        };
        if feature.flags & !FEATURE_REQUIRED != 0 {
            return Err(Error::corrupt("unknown feature flag"));
        }
        if feature.config_length == 0 {
            if feature.config_offset != 0 {
                return Err(Error::corrupt("empty feature config has nonzero offset"));
            }
        } else {
            let begin = feature.config_offset as usize;
            let end = begin
                .checked_add(feature.config_length as usize)
                .ok_or_else(|| Error::corrupt("feature config range overflow"))?;
            if begin < config_offset || end > config_offset + config_length {
                return Err(Error::corrupt("feature config outside configuration area"));
            }
            if ranges
                .iter()
                .any(|&(old_begin, old_end)| begin < old_end && old_begin < end)
            {
                return Err(Error::corrupt("overlapping feature configurations"));
            }
            ranges.push((begin, end));
        }

        match feature.kind {
            FEATURE_COMPRESSION => {
                if compression.is_some()
                    || !feature.required()
                    || feature.version == 0
                    || feature.algorithm > u32::from(u16::MAX)
                    || feature.config_length != 0
                    || feature.region_offset != 0
                    || feature.region_length != 0
                {
                    return Err(Error::corrupt("invalid compression feature"));
                }
                compression = Some((feature.algorithm as u16, feature.version));
            }
            FEATURE_TIME_INDEX => {
                if time_index {
                    return Err(Error::corrupt("duplicate time-index feature"));
                }
                if !feature.required() || feature.config_length != 0 || feature.region_length == 0 {
                    return Err(Error::corrupt("invalid time-index feature"));
                }
                if feature.algorithm != 1 || feature.version != 1 {
                    return Err(Error::unsupported("unsupported time index"));
                }
                time_index = true;
            }
            FEATURE_RECORD_PROFILE => {
                if record_profile.is_some()
                    || feature.flags != 0
                    || feature.algorithm != 0
                    || feature.config_offset != 512
                    || feature.config_length != 8
                    || feature.region_offset != 0
                    || feature.region_length != 0
                {
                    return Err(Error::corrupt("invalid record-profile feature"));
                }
                record_profile = Some((get64(bytes, 512), feature.version));
            }
            FEATURE_INTEGRITY => {
                if integrity
                    || !feature.required()
                    || feature.config_length != 0
                    || feature.region_offset != 0
                    || feature.region_length != 0
                {
                    return Err(Error::corrupt("invalid integrity feature"));
                }
                if feature.algorithm != 1 || feature.version != 1 {
                    return Err(Error::unsupported("unsupported integrity feature"));
                }
                integrity = true;
            }
            _ if feature.required() => {
                return Err(Error::unsupported("unknown required partition feature"));
            }
            _ => {}
        }
        features.push(feature);
    }
    let (compression_id, compression_version) =
        compression.ok_or_else(|| Error::corrupt("missing compression feature"))?;
    if !time_index || !integrity {
        return Err(Error::corrupt("missing core partition feature"));
    }
    let (record_format_id, record_format_version) =
        record_profile.ok_or_else(|| Error::corrupt("missing record-profile feature"))?;
    if (record_format_id == 0) != (record_format_version == 0) {
        return Err(Error::corrupt("record profile ID/version mismatch"));
    }

    Ok(PartitionHeader {
        volume_id_high: get64(bytes, 16),
        volume_id_low: get64(bytes, 24),
        generation: get64(bytes, 32),
        slot: get32(bytes, 40),
        created_time_ns: geti64(bytes, 48),
        max_block_payload: get32(bytes, 56),
        persistence_quantum: get32(bytes, 60),
        allowed_backward_skew_ns: geti64(bytes, 64),
        allowed_forward_step_ns: geti64(bytes, 72),
        time_domain_id: get64(bytes, 80),
        compression_id,
        compression_version,
        record_format_id,
        record_format_version,
        features,
    })
}

pub fn decode_block_header(input: &[u8]) -> Result<BlockHeader> {
    let bytes = common(input, b"TFDBBLK1", BLOCK_HEADER_SIZE, 72)?;
    let result = BlockHeader {
        generation: get64(bytes, 16),
        sequence: get32(bytes, 24),
        flags: get32(bytes, 28),
        record_count: get32(bytes, 32),
        stored_size: get32(bytes, 36),
        raw_size: get32(bytes, 40),
        frame_span: get32(bytes, 44),
        min_time_ns: geti64(bytes, 48),
        max_time_ns: geti64(bytes, 56),
        compression_id: get16(bytes, 64),
        compression_version: get16(bytes, 66),
        payload_crc: get32(bytes, 68),
        slot: get32(bytes, 76),
        volume_id_low: get64(bytes, 80),
        volume_id_high: get64(bytes, 88),
        writer_id_high: get64(bytes, 96),
        writer_id_low: get64(bytes, 104),
        previous_writer_id_high: get64(bytes, 112),
        previous_writer_id_low: get64(bytes, 120),
    };
    if result.stored_size == 0
        || result.raw_size == 0
        || result.record_count == 0
        || (result.writer_id_high == 0 && result.writer_id_low == 0)
        || result.min_time_ns > result.max_time_ns
        || u64::from(result.frame_span) < BLOCK_HEADER_SIZE as u64 + u64::from(result.stored_size)
    {
        return Err(Error::corrupt("invalid block bounds"));
    }
    Ok(result)
}

pub fn decode_index_entry(bytes: &[u8]) -> Result<IndexEntry> {
    if bytes.len() < INDEX_ENTRY_SIZE {
        return Err(Error::corrupt("truncated time-index entry"));
    }
    let result = IndexEntry {
        offset: get64(bytes, 0),
        frame_size: get32(bytes, 8),
        frame_span: get32(bytes, 12),
        sequence: get32(bytes, 16),
        record_count: get32(bytes, 20),
        min_time_ns: geti64(bytes, 24),
        max_time_ns: geti64(bytes, 32),
        flags: get32(bytes, 40),
        raw_size: get32(bytes, 44),
        writer_id_high: 0,
        writer_id_low: 0,
    };
    if result.frame_size < BLOCK_HEADER_SIZE as u32
        || result.frame_size > result.frame_span
        || result.record_count == 0
        || result.min_time_ns > result.max_time_ns
    {
        return Err(Error::corrupt("invalid time-index entry bounds"));
    }
    Ok(result)
}

pub fn decode_footer(input: &[u8]) -> Result<PartitionFooter> {
    let bytes = common(input, b"TFDBFTR1", FOOTER_SIZE, 100)?;
    reserved_zero(bytes, 68, 72, "partition footer")?;
    reserved_zero(bytes, 104, FOOTER_SIZE, "partition footer")?;
    if get32(bytes, 52) as usize != INDEX_ENTRY_SIZE {
        return Err(Error::unsupported("unsupported time-index entry size"));
    }
    let result = PartitionFooter {
        volume_id_high: get64(bytes, 16),
        volume_id_low: get64(bytes, 24),
        generation: get64(bytes, 32),
        slot: get32(bytes, 40),
        flags: get32(bytes, 44),
        block_count: get32(bytes, 48),
        index_offset: get64(bytes, 56),
        index_size: get32(bytes, 64),
        data_end: get64(bytes, 72),
        min_time_ns: geti64(bytes, 80),
        max_time_ns: geti64(bytes, 88),
        index_crc: get32(bytes, 96),
    };
    let expected_size = u64::from(result.block_count) * INDEX_ENTRY_SIZE as u64;
    if u64::from(result.index_size) != expected_size
        || (result.block_count != 0 && result.min_time_ns > result.max_time_ns)
    {
        return Err(Error::corrupt("invalid partition footer bounds"));
    }
    Ok(result)
}

pub fn align_up(value: u64, quantum: u32) -> Option<u64> {
    if quantum == 0 {
        return None;
    }
    let remainder = value % u64::from(quantum);
    if remainder == 0 {
        Some(value)
    } else {
        value.checked_add(u64::from(quantum) - remainder)
    }
}
