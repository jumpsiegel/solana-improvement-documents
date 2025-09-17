# Cold Snapshot Generator and Verifier

This package contains C programs to generate and verify example cold snapshots following the format specified in SIMD XXXX (Deterministic Cold Snapshots).

## Features

### Generator (`generate_example_snapshot`)
- Generates 1 million random accounts by default with realistic data
- Creates multiple chunk files (100K accounts per chunk = 100 chunks)
- Follows the exact binary format specified in the SIMD proposal
- Includes proper SHA256 file hashing and LtHash computation
- Sorts accounts by public key as required by the specification
- Generates deterministic, verifiable snapshot files

### Verifier (`verify_snapshot`)
- Validates all chunk files in a directory
- Verifies SHA256 hashes for file integrity
- Recomputes and validates LtHashes for each chunk
- Checks account sorting within chunks
- Verifies magic numbers and file structure
- Validates global LtHash across all chunks
- Provides detailed error reporting

## Prerequisites

You need OpenSSL development libraries installed:

**Ubuntu/Debian:**
```bash
sudo apt-get install libssl-dev
```

**CentOS/RHEL/Fedora:**
```bash
sudo yum install openssl-devel
# or for newer versions:
sudo dnf install openssl-devel
```

**macOS:**
```bash
brew install openssl
```

## Building

Build both programs:
```bash
make all
```

Or build individually:
```bash
make generate_example_snapshot
make verify_snapshot
```

## Running

### Generate Snapshot Files
```bash
make generate
# or directly:
./generate_example_snapshot
```

### Verify Snapshot Files
```bash
make verify
# or directly:
./verify_snapshot

# For faster verification (skips expensive LtHash computation):
./verify_snapshot --quick
```

### Run Complete Test (Generate + Verify)
```bash
make test
```

You can also verify snapshots in a different directory:
```bash
./verify_snapshot /path/to/snapshot/directory
./verify_snapshot --quick /path/to/snapshot/directory
```

**Optimization Notes:**
- Regular verification: Complete validation including LtHash computation (~15 seconds for 1M accounts)
- Quick mode (`--quick`): Structure and hash verification only (~3 seconds for 1M accounts)
- Quick mode is recommended for routine verification where LtHash accuracy has been previously confirmed

## Output

The program will generate:
- 100 chunk files named `cold_snapshot_chunk_0000.bin` through `cold_snapshot_chunk_0099.bin`
- Each chunk contains up to 100,000 accounts
- The last chunk contains the global snapshot metadata
- Total size will be approximately 5-10 GB depending on random account data sizes

## File Format

Each chunk file follows this structure:

1. **File Header** (2084 bytes)
   - Magic number (2 bytes): 0x534E ("SN")
   - Header length (2 bytes): sizeof(file_header_t)
   - SHA256 hash of entire file (32 bytes)
   - LtHash of all accounts in chunk (2048 bytes)

2. **Account Data Payload Header** (16 bytes)
   - Magic number (2 bytes): 0x4143 ("AC")
   - Header length (2 bytes): sizeof(add_account_data_t)
   - Payload size (8 bytes)
   - Account count (4 bytes)

3. **Account Data** (variable size)
   - For each account:
     - Public key (32 bytes)
     - Owner (32 bytes)
     - Lamports (8 bytes)
     - Data length (4 bytes)
     - Account data (variable, 0-10KB per account)

4. **Snapshot Metadata** (2060 bytes, final chunk only)
   - Magic number (2 bytes): 0x4D45 ("ME")
   - Header length (2 bytes): sizeof(full_snapshot_metadata_t)
   - Slot number (8 bytes)
   - Global LtHash (2048 bytes)

## Implementation Notes

- Uses real Firedancer LtHash implementation (BLAKE3-based lattice hashing)
- Matches Solana's exact account serialization format for LtHash computation
- Accounts are globally sorted by public key across ALL chunks before writing
- Guarantees no duplicate public keys (verified during generation and verification)
- Each chunk is independently verifiable but maintains global sort order
- Global LtHash accumulates all account hashes across all chunks
- Compatible with live Solana validator LtHash calculations
- Deterministic chunk boundaries ensure reproducible snapshots

## Cleanup

Remove generated files:
```bash
make clean
```

## Verification Output

The verifier provides detailed output including:

```
Verifying cold snapshot files in directory: .

Found 100 chunk files:

Chunk 0000 (./cold_snapshot_chunk_0000.bin): ✓ VALID - 100000 accounts, 45.23 MB
Chunk 0001 (./cold_snapshot_chunk_0001.bin): ✓ VALID - 100000 accounts, 45.18 MB
...
Chunk 0099 (./cold_snapshot_chunk_0099.bin): ✓ VALID - 100000 accounts, 45.31 MB [METADATA: slot 12345678]

=== VERIFICATION SUMMARY ===
Total chunks: 100
Valid chunks: 100
Invalid chunks: 0
Total accounts: 10000000
Total size: 4.52 GB
Snapshot slot: 12345678

Computed global LtHash: a1b2c3d4e5f6789012345678901234567890abcd...
Expected global LtHash: a1b2c3d4e5f6789012345678901234567890abcd...
✓ Global LtHash verification: PASSED

=== VERIFICATION RESULT ===
✓ SNAPSHOT VERIFICATION: PASSED
```

The verifier checks:
1. File integrity via SHA256 hashes
2. Magic numbers and structure validity
3. Account sorting within chunks AND globally across all chunks
4. No duplicate public keys within or across chunks
5. LtHash computation and verification (can be skipped with `--quick`)
6. Global LtHash consistency across all chunks
7. Deterministic chunk boundaries (accounts are globally sorted before chunking)

**Performance Optimizations:**
- Eliminated O(n²) sorting verification algorithm (now O(n))
- Added progress reporting for large chunk verification
- Quick mode option skips expensive LtHash computation
- Efficient single-pass account parsing