---
simd: 'XXXX'
title: Deterministic Cold Snapshots
authors:
  - Josh Siegel
  - Brooks Prumo
category: Standard
type: Core
status: Idea
created: 2025-09-17
feature:
supersedes:
superseded-by:
extends:
---

## Summary

This proposal defines a new future-proof cold snapshot format optimized for large data sets and parallel operations. Cold snapshots will store long-term inactive account data in a deterministic, verifiable format that can be distributed efficiently across the network.

## Motivation

Approximately 80% of accounts have not been modified in the last 6 months. This, combined with the desire to reduce rent costs (which will increase the total number of accounts by orders of magnitude), suggests that the current snapshot split of full/incremental is not long-term sustainable. Additionally, due to early design choices, the full snapshot is currently not deterministic and fully verifiable across the cluster.

Another motivation is ensuring that the cold snapshot format does not require a complete redesign of the existing snapshot system, but can instead augment it, allowing for more rapid adoption.

## New Terminology

**Cold Snapshot**: A deterministic snapshot format containing long-term inactive account data, optimized for parallel processing and network distribution.

**Warm Snapshot**: A snapshot written using the existing snapshot mechanism using the existing intermediate snapshot format, intended to be layered on top of the Cold Snapshot

**Hot Snapshot**: A snapshot written using the existing snapshot mechanism using the existing intermediate snapshot format, intended to be layered on top of the Warm Snapshot

**Chunk**: An individual file within a cold snapshot containing a subset of account data with its own SHA256 verification hash and LtHash.

**LtHash**: Lattice hash as defined in SIMD-0215, used for incremental verification of account data. This is a BLAKE3-based cryptographic hash distinct from SHA256.

**SHA256 Hash**: Used for file integrity verification through merkle chain hashing. Each chunk's `chunk_hash` field contains a SHA256 hash of the file contents mixed with the previous chunk's hash.

## Detailed Design

### Design Goals

- **Future-proof**: Cold snapshot files may exist indefinitely, so validator/version-specific information should be kept to a minimum. The data format specification should be simple enough that parsers can be written easily in multiple languages.

- **Optimized for parallel operations**: Snapshot chunks should be as independent as possible, allowing for parallel processing and decompression in any order.

- **Optimized for network distribution**: The design supports distribution via mechanisms like BitTorrent. Each chunk should be verifiable independently without waiting for all chunks to arrive, leading to storing each chunk as a separate file.

- **Optimized for file system storage**: Similar to BitTorrent requirements, file systems perform better with reasonably-sized files. Chunks should be kept at manageable sizes and stored in separate files, allowing chunks to be spread across multiple file systems to accommodate smaller disks.  No file should be larger than 128gb

- **Fully verifiable**: It should be possible to verify the LtHash of all accounts in a chunk to validate the chunk, and then combine the LtHashes of all chunks to verify successful recovery of the complete snapshot state.

- **Deterministic**: A cold snapshot should be bit-for-bit identical regardless of its source. This enables both verification and parallel seeding of torrents from multiple sources.

### File Format Specification

Each chunk is stored as a separate file with the following structure:

#### Magic Numbers

All structures use 2-byte magic numbers for identification and versioning. Implementations MUST use these exact values:

| Structure | Magic Number | Hex Value | ASCII |
|-----------|-------------|-----------|-------|
| `FileHeader` | `0x534E` | `534E` | "SN" (Snapshot) |
| `FileFooter` | `0x4654` | `4654` | "FT" (Footer) |
| `AddAccountData` | `0x4143` | `4143` | "AC" (Account) |

**Note**: All magic numbers are stored in little-endian byte order. The ASCII representation is for mnemonic purposes only.

#### Compression Constants

| Compression Type | Value | Description |
|------------------|-------|-------------|
| `COMPRESSION_NONE` | `0x0000` | No compression applied |
| `COMPRESSION_LZ4` | `0x0001` | LZ4 compression algorithm (see detailed specification below) |
| Reserved | `0x0002-0xFFFF` | Future compression algorithms |

#### File Header
```rust
#[repr(C)]
struct FileHeader {
    magic_number: u16,         // Version identifier
    header_len: u16,           // size_of::<FileHeader>()
    slot: u64,                 // Snapshot slot number
    file_index: u32,           // Chunk file index (0, 1, 2, ...)
}
```

#### File Footer
```rust
#[repr(C)]
struct FileFooter {
    magic_number: u16,         // Footer identifier
    header_len: u16,           // size_of::<FileFooter>()
    lthash: [u8; 2048],        // Accumulated LtHashes of all accounts in this chunk
    chunk_hash: [u8; 32],      // Merkle chain SHA256 hash (must be last field)
}
```

**Merkle Chain Hash Protocol**: The `chunk_hash` field implements a merkle chain where each chunk's SHA256 hash depends on the previous chunk:

1. **SHA256 Hash Computation**: `chunk_hash = SHA256(file_contents_without_hash || previous_chunk_hash)`
2. **First Chunk**: Uses `previous_chunk_hash = [0; 32]` (all zeros)
3. **Subsequent Chunks**: Uses the `chunk_hash` from the previous chunk file
4. **SHA256 Hash Field Position**: The `chunk_hash` must be the last field in the FileFooter to enable consistent SHA256 computation

**File Generation Protocol**:
1. Initialize FileFooter with `chunk_hash` set to all zeros
2. Write FileHeader, account data, and metadata
3. Compute SHA256 hash over entire file contents (excluding the zeroed SHA256 hash field) concatenated with previous chunk hash
4. Update `chunk_hash` field with computed SHA256 hash
5. Write final file to storage

**Streaming I/O Optimization**: Since the `chunk_hash` is the last field in the FileFooter, implementations can use streaming I/O where:
- Data is written directly to disk as it's processed
- SHA256 hash is computed incrementally during writing
- Only the final 32-byte SHA256 hash field requires seeking back to update
- No large file buffers need to be held in memory
- This enables processing of arbitrarily large chunks with minimal RAM usage

After the header, the file contains a series of chunk payloads containing account data, followed by the FileFooter. The magic numbers drive parsers to understand the payload contents.

#### Chunk Payload Structure
```rust
#[repr(C)]
struct ChunkPayload {
    magic_number: u16,        // Payload type identifier
    header_len: u16,          // size_of::<ChunkPayload>() header
    payload_sz: u64,          // Full size including header (allows skipping unknown payloads)
    data: Vec<u8>,            // Variable-length payload data
}
```

#### Account Data Payload
```rust
#[repr(C)]
struct AccountPayload {
   pubkey: [u8; 32],
   owner: [u8; 32],
    // ... the other metadata fields here too, like lamports, blah blah,
   data_len: u32, // <-- can we use u32 to start with? accounts are 10 MiB max. We can use a larger value later and bump the file header version
   data: &[u8],
}
```

#### Accounts Data Payload
```rust
#[repr(C)]
struct AddAccountData {
    magic_number: u16,
    header_len: u16,
    compression_type: u16,      // Compression algorithm identifier
    payload_sz: u64,            // Size of compressed data (max 4GB per object)
    accounts_cnt: u32,
    accounts_data: Vec<AccountPayload>,    // Serialized account data (possibly compressed)
}
```

**Compression Types**:
- `0x0000`: Uncompressed data
- `0x0001`: LZ4 compressed data
- `0x0002-0xFFFF`: Reserved for future compression algorithms

#### LZ4 Compression Specification

**Format Requirements**: When `compression_type = 0x0001`, implementations MUST use the **raw LZ4 format** as specified below to ensure cross-language compatibility.

**Raw LZ4 Format**:
- Use the standard LZ4 block format WITHOUT any frame headers or size prefixes
- Compatible with `LZ4_compress_default()` and `LZ4_decompress_safe()` from liblz4
- Compatible with `lz4_flex::compress()` and `lz4_flex::decompress()` from Rust
- **MUST NOT** include any size prefixes, frame headers, or custom wrappers

**Compression Protocol**:
1. **Input**: Raw account data (concatenated `AccountPayload` structures with their data)
2. **Compression**: Apply LZ4 compression directly to the raw input
3. **Output**: Store compressed bytes directly in the `AddAccountData` payload
4. **Size**: Update `payload_sz` to reflect the compressed data size

**Multi-Object Support**: Multiple `AddAccountData` objects may exist in a single chunk file to enable parallel decompression and account insertion. Each object MUST NOT exceed 4GB of compressed data (`payload_sz ≤ 4,294,967,296 bytes`).


### Organization Rules

- All accounts are sorted globally by public key across the entire snapshot, then emitted sequentially into chunks
- Accounts are distributed across chunks in sorted order (chunk 0 contains accounts with the smallest pubkeys, chunk N contains accounts with the largest pubkeys)
- No duplicate public keys are allowed within or across chunks (each account appears exactly once)
- No chunk exceeds the maximum file size limit (128GB)
- Every chunk begins with a `FileHeader` structure and ends with a `FileFooter` structure
- Multiple `AddAccountData` objects may exist within a single chunk for parallel processing
- Each `AddAccountData` object MUST NOT exceed 4GB of compressed data
- Account data within each `AddAccountData` object MUST maintain global sorting order
- No manifest file, transaction cache, or other metadata is stored in the cold snapshot
- This global sorting and uniqueness constraint ensures deterministic chunk boundaries and reproducible snapshots
- **LtHash Algorithm**: Use BLAKE3-based lattice hash as defined in SIMD-0215 (not SHA256)

### File Generation Order

The following order of operations ensures correct merkle chain SHA256 hash computation and metadata consistency:

1. **Initialize file buffer** with `FileHeader` and zeroed `FileFooter` (including `chunk_hash` field)
2. **Write account data objects** in globally sorted order:
   - Group accounts into 4GB or smaller segments
   - Apply compression (if enabled) to each segment
   - Write `AddAccountData` header with compression type and compressed size
   - Write compressed account payloads
   - Repeat for additional objects as needed
3. **Compute merkle chain SHA256 hash** over entire file contents (excluding the zeroed `chunk_hash` field) concatenated with the previous chunk's SHA256 hash
4. **Update `chunk_hash` field** in the FileFooter with computed SHA256 hash
5. **Write final file** to storage

This ordering ensures that the merkle chain SHA256 hash includes all file content while maintaining the protocol requirement that the SHA256 hash field itself is zeroed during computation. The resulting SHA256 hash creates a verifiable chain linking all chunks in sequence.

### Integration with Existing Snapshots

#### Full Snapshot Modification
The existing full snapshot file will be converted to use the incremental snapshot mechanism, emitting only accounts that have changed since the cold snapshot.

#### Incremental Snapshot Enhancement
The existing incremental snapshot format will gain a new field containing the LtHash changes in that snapshot. This enables easy verification by combining:
- Cold snapshot LtHash
- Warm snapshot LtHash
- Hot snapshot LtHash

The result can be compared against the total LtHash stored in the finish snapshot file to quickly verify that layering all snapshots together produces a verifiable complete state.

## Reference Implementation

A complete reference implementation demonstrating cross-language compatibility is available in the `tests/` directory. This includes:

- **C Implementation**: Generator and verifier with Firedancer LtHash integration
- **C++ Implementation**: Generator and verifier based purely on SIMD specification
- **Rust Implementation**: Generator and verifier with exact binary compatibility
- **Cross-compatibility tests**: Proving language-agnostic file format
- **Performance benchmarks**: Validation with 1M+ account datasets

The implementations validate that the specification is precise enough for independent implementation across different programming languages while maintaining full binary compatibility.

### Implementation Validation Requirements

To prevent compatibility bugs, all implementations MUST pass these validation tests:

#### Struct Size Validation
Implementations MUST verify that structure sizes match exactly:
- `FileHeader`: 16 bytes (magic_number + header_len + slot + file_index)
- `FileFooter`: 2084 bytes
- `AddAccountData`: 18 bytes (updated with compression_type field)
- `AccountPayload`: 76 bytes

#### LtHash Compatibility Test
Implementations MUST produce identical LtHash values for the verification test vector specified in the LtHash Computation section. This test MUST be included in the implementation's test suite.

#### Cross-Language File Reading
Implementations SHOULD be able to parse file structures (magic numbers, headers, account data) generated by other implementations, even if LtHash algorithms differ. This validates binary format compatibility.

#### Magic Number Validation
Implementations MUST use the exact magic number values specified in the Magic Numbers table. Verification tools SHOULD reject files with incorrect magic numbers.

## Alternatives Considered

*[This section would be filled in during the proposal process with alternative approaches that were considered]*

## Impact

Validator operators who download snapshots outside of the validator itself will need to retrieve both cold snapshots and the existing snapshot types. This may require updates to existing snapshot tooling.

## Security Considerations

- Each chunk is independently verifiable via SHA256 merkle chain hash
- Account data integrity is maintained through LtHash verification
- Deterministic format prevents malicious modifications during distribution
- BitTorrent-style distribution provides redundancy and reduces single points of failure
- **Implementation Compatibility**: The detailed LtHash computation specification prevents implementation bugs that could lead to false verification failures or security vulnerabilities from LtHash mismatches

## Drawbacks

- Adds complexity to the snapshot system
- Requires updates to existing snapshot tooling
- Initial implementation overhead for chunk management
- Storage overhead from file headers and verification data

## Backwards Compatibility

This proposal extends the existing snapshot system without breaking changes. Existing full and incremental snapshots continue to work, with cold snapshots providing an additional layer for long-term inactive accounts.
