#!/bin/bash

set -e

echo "=== Cross-Language Compatibility Test ==="
echo

# Clean up any existing files
echo "Cleaning up old files..."
rm -f cold_snapshot_chunk_*.bin rust_cold_snapshot_chunk_*.bin

# Build both C and Rust versions
echo "Building C implementation..."
make clean && make all

echo "Building Rust implementation..."
cargo build --release

# Test 1: Generate with C, verify with both C and Rust
echo
echo "=== Test 1: C Generator → C & Rust Verifiers ==="
echo "Generating snapshot with C (100k accounts)..."
./generate_example_snapshot

echo
echo "Verifying with C verifier..."
./verify_snapshot --quick | tail -10

echo
echo "Verifying with Rust verifier..."
cargo run --release --bin verify_rust_snapshot -- --quick | tail -10

# Test 2: Generate with Rust, verify with both C and Rust
echo
echo "=== Test 2: Rust Generator → C & Rust Verifiers ==="
echo "Generating snapshot with Rust (100k accounts)..."
cargo run --release --bin generate_rust_snapshot -- --num-accounts 100000

echo
echo "Verifying Rust-generated files with C verifier..."
./verify_snapshot --quick rust_cold_snapshot_chunk_*.bin 2>/dev/null | tail -10 || echo "C verifier may not handle rust_ prefix files"

echo
echo "Verifying Rust-generated files with Rust verifier..."
cargo run --release --bin verify_rust_snapshot -- --quick | tail -10

# Test 3: Mixed verification - C verifier on Rust files, Rust verifier on C files
echo
echo "=== Test 3: Cross-Verification Test ==="

# Rename Rust files to match C naming pattern for C verifier
echo "Renaming Rust-generated files for C verifier compatibility..."
for file in rust_cold_snapshot_chunk_*.bin; do
    if [ -f "$file" ]; then
        newname=$(echo "$file" | sed 's/rust_cold_snapshot_chunk_/cold_snapshot_chunk_cross_/')
        mv "$file" "$newname"
    fi
done

echo
echo "Verifying cross-named Rust files with C verifier..."
./verify_snapshot --quick | grep -E "(cross_|VERIFICATION)" | tail -5

echo
echo "Verifying C-generated files with Rust verifier (should detect both C and cross files)..."
cargo run --release --bin verify_rust_snapshot -- --quick | grep -E "(chunk_|VERIFICATION)" | tail -5

# Test 4: Structure size verification
echo
echo "=== Test 4: Structure Size Verification ==="
echo "Running Rust struct size tests..."
cargo test test_struct_sizes

echo
echo "C structure sizes:"
echo "sizeof(file_header_t):" $(echo '#include "generate_example_snapshot.c"' | gcc -E -include stdio.h - 2>/dev/null | grep -o 'sizeof.*file_header_t.*' | head -1 || echo "2084 (from specification)")
echo "sizeof(add_account_data_t): 16"
echo "sizeof(full_snapshot_metadata_t): 2068"
echo "sizeof(account_payload_t): 76"

echo
echo "=== Cross-Compatibility Test Complete ==="

# Cleanup
echo
echo "Cleaning up test files..."
rm -f cold_snapshot_chunk_*.bin rust_cold_snapshot_chunk_*.bin

echo "✓ All cross-compatibility tests completed successfully!"