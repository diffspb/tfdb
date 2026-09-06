//! Independent, read-only implementation of the TFDB v1 candidate format.
//!
//! The crate intentionally uses only the Rust standard library and does not
//! call the C++ implementation. It validates structure and payload CRC32C,
//! rebuilds missing indexes by scanning the valid block prefix, decodes the
//! built-in PackBits codec, and can parse the optional FramedRecordV1 profile.

#![forbid(unsafe_code)]

mod codec;
mod crc32c;
mod error;
mod format;
mod reader;
mod records;

pub use crate::codec::decompress_packbits;
pub use crate::crc32c::crc32c;
pub use crate::error::{Error, ErrorKind, Result};
pub use crate::format::{
    decode_block_header, decode_footer, decode_index_entry, decode_partition_header,
    decode_volume_header, BlockHeader, Feature, IndexEntry, PartitionFooter, PartitionHeader,
    VolumeHeader, BLOCK_HEADER_SIZE, FOOTER_SIZE, FRAMED_RECORD_V1_PROFILE_ID, INDEX_ENTRY_SIZE,
    PARTITION_HEADER_REGION_SIZE, PARTITION_HEADER_SIZE, VOLUME_HEADER_SIZE, VOLUME_PREFIX_SIZE,
};
pub use crate::reader::{
    Block, BlockMetadata, Event, Gap, GapKind, PartitionInfo, Reader, TimeRange,
};
pub use crate::records::{decode_framed_v1, FramedRecord};
