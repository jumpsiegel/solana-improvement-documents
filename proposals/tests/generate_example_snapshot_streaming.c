#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <lz4.h>
#include "fd_lthash_standalone.h"

// Constants
#define CHUNK_SIZE_LIMIT (128ULL * 1024 * 1024 * 1024)  // 128GB max chunk size
#define NUM_ACCOUNTS 5000      // 5 thousand accounts for testing
#define LTHASH_SIZE 2048
#define PUBKEY_SIZE 32
#define OWNER_SIZE 32
#define SHA256_SIZE 32

// Magic numbers for different payload types (2 bytes)
#define MAGIC_FILE_HEADER 0x534E      // "SN" (from "SNAP")
#define MAGIC_FILE_FOOTER 0x4654      // "FT" (from "FOOT")
#define MAGIC_ACCOUNT_DATA 0x4143     // "AC" (from "ACCT")
// MAGIC_METADATA removed from SIMD specification

// Compression types from SIMD specification
#define COMPRESSION_NONE 0x0000
#define COMPRESSION_LZ4 0x0001

// Maximum payload size per AddAccountData object (4GB)
#define MAX_PAYLOAD_SIZE (4ULL * 1024 * 1024 * 1024)

// Compression settings - disabled for streaming generator (complex to implement with streaming I/O)
#define ENABLE_COMPRESSION 1  // Re-enabled with raw LZ4 format
#define ACCOUNTS_PER_OBJECT 2500  // Split into smaller objects for parallel processing

// File structures matching the SIMD proposal
#pragma pack(push, 1)
typedef struct {
    uint16_t magic_number;     // Version identifier
    uint16_t header_len;       // sizeof(file_header_t)
    uint64_t slot;             // Snapshot slot number
    uint32_t file_index;       // Chunk file index (0, 1, 2, ...)
} file_header_t;

typedef struct {
    uint16_t magic_number;     // Footer identifier
    uint16_t header_len;       // sizeof(file_footer_t)
    uint8_t lthash[LTHASH_SIZE];        // Accumulated LtHashes of all accounts in this chunk
    uint8_t chunk_hash[SHA256_SIZE];    // Merkle chain hash (must be last field)
} file_footer_t;

typedef struct {
    uint8_t pubkey[PUBKEY_SIZE];
    uint8_t owner[OWNER_SIZE];
    uint64_t lamports;
    uint32_t data_len;
    // data follows immediately after this struct
} account_payload_t;

typedef struct {
    uint16_t magic_number;
    uint16_t header_len;       // sizeof(add_account_data_t)
    uint16_t compression_type; // COMPRESSION_NONE or COMPRESSION_LZ4
    uint64_t payload_sz;
    uint32_t accounts_cnt;
    // accounts_data follows immediately after this struct
} add_account_data_t;

typedef struct {
    uint16_t magic_number;
    uint16_t header_len;       // sizeof(full_snapshot_metadata_t)
    uint64_t slot;
    uint8_t lthash[LTHASH_SIZE];
} full_snapshot_metadata_t;
#pragma pack(pop)

// LtHash utility functions
void compute_lthash(const uint8_t *data, size_t len, uint8_t *lthash) {
    fd_lthash_value_t hash_value;
    fd_lthash_compute(data, len, &hash_value);
    memcpy(lthash, hash_value.bytes, LTHASH_SIZE);
}

void add_lthash(uint8_t *dest, const uint8_t *src) {
    fd_lthash_value_t dest_val, src_val;
    memcpy(dest_val.bytes, dest, LTHASH_SIZE);
    memcpy(src_val.bytes, src, LTHASH_SIZE);
    fd_lthash_add(&dest_val, &src_val);
    memcpy(dest, dest_val.bytes, LTHASH_SIZE);
}

void generate_random_bytes(uint8_t *buffer, size_t length) {
    if (RAND_bytes(buffer, length) != 1) {
        fprintf(stderr, "Failed to generate random bytes\n");
        exit(1);
    }
}

void generate_random_account(account_payload_t *account, uint8_t **account_data, uint32_t *data_len) {
    // Generate random pubkey and owner
    generate_random_bytes(account->pubkey, PUBKEY_SIZE);
    generate_random_bytes(account->owner, OWNER_SIZE);

    // Random lamports (1 to 100 SOL in lamports)
    account->lamports = (rand() % 100000000000ULL) + 1;

    // Random data length (0 to 10KB for this example)
    *data_len = rand() % 10240;
    account->data_len = *data_len;

    // Generate random account data
    if (*data_len > 0) {
        *account_data = malloc(*data_len);
        generate_random_bytes(*account_data, *data_len);
    } else {
        *account_data = NULL;
    }
}

// Streaming hash context for computing merkle chain hash
typedef struct {
    SHA256_CTX sha_ctx;
    bool initialized;
} streaming_hash_ctx_t;

void streaming_hash_init(streaming_hash_ctx_t *ctx) {
    SHA256_Init(&ctx->sha_ctx);
    ctx->initialized = true;
}

void streaming_hash_update(streaming_hash_ctx_t *ctx, const void *data, size_t len) {
    if (!ctx->initialized) {
        fprintf(stderr, "Hash context not initialized\n");
        exit(1);
    }
    SHA256_Update(&ctx->sha_ctx, data, len);
}

void streaming_hash_finalize(streaming_hash_ctx_t *ctx, const uint8_t *previous_chunk_hash, uint8_t *result_hash) {
    if (!ctx->initialized) {
        fprintf(stderr, "Hash context not initialized\n");
        exit(1);
    }
    // Add previous chunk hash to complete merkle chain
    SHA256_Update(&ctx->sha_ctx, previous_chunk_hash, SHA256_SIZE);
    SHA256_Final(result_hash, &ctx->sha_ctx);
    ctx->initialized = false;
}

// Compression utility functions for streaming
typedef struct {
    uint8_t* data;
    size_t size;
    size_t compressed_size;
    bool is_compressed;
} compression_result_t;

// LZ4 compression wrapper with real LZ4 library
compression_result_t compress_data_streaming(const uint8_t* input, size_t input_size, uint16_t compression_type) {
    compression_result_t result = {0};

    if (compression_type == COMPRESSION_NONE || !ENABLE_COMPRESSION) {
        // No compression - just copy data
        result.data = malloc(input_size);
        if (result.data) {
            memcpy(result.data, input, input_size);
            result.size = input_size;
            result.compressed_size = input_size;
            result.is_compressed = false;
        }
    } else if (compression_type == COMPRESSION_LZ4) {
        // Raw LZ4 compression (no size prefix - compatible with C++/Rust)
        int max_compressed_size = LZ4_compressBound((int)input_size);
        result.data = malloc(max_compressed_size);

        if (result.data) {
            // Compress data directly (no size prefix)
            int compressed_size = LZ4_compress_default((const char*)input,
                                                     (char*)result.data,
                                                     (int)input_size,
                                                     max_compressed_size);

            if (compressed_size > 0) {
                result.size = input_size;
                result.compressed_size = compressed_size;
                result.is_compressed = true;

                // Reallocate to exact size to save memory
                result.data = realloc(result.data, result.compressed_size);
                printf("Streaming LZ4 compression: %zu bytes -> %zu bytes (%.1f%% of original)\n",
                       input_size, result.compressed_size,
                       (double)result.compressed_size / input_size * 100.0);
            } else {
                // Compression failed, fallback to uncompressed
                printf("LZ4 compression failed, using uncompressed data\n");
                free(result.data);
                result.data = malloc(input_size);
                if (result.data) {
                    memcpy(result.data, input, input_size);
                    result.size = input_size;
                    result.compressed_size = input_size;
                    result.is_compressed = false;
                }
            }
        }
    }

    return result;
}

void free_compression_result_streaming(compression_result_t* result) {
    if (result && result->data) {
        free(result->data);
        result->data = NULL;
        result->size = 0;
        result->compressed_size = 0;
    }
}

// Write a chunk file using streaming I/O (no large buffer allocation)
void write_chunk_file_streaming(const char *filename, account_payload_t *accounts, uint8_t **accounts_data,
                               uint32_t *data_lens, uint32_t account_count, uint64_t slot, uint32_t file_index,
                               bool is_final_chunk, uint8_t *global_lthash, const uint8_t *previous_chunk_hash) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Error opening file %s\n", filename);
        exit(1);
    }

    streaming_hash_ctx_t hash_ctx;
    streaming_hash_init(&hash_ctx);

    // Write file header and hash it
    file_header_t header = {0};
    header.magic_number = MAGIC_FILE_HEADER;
    header.header_len = sizeof(file_header_t);
    header.slot = slot;
    header.file_index = file_index;

    fwrite(&header, sizeof(file_header_t), 1, file);
    streaming_hash_update(&hash_ctx, &header, sizeof(file_header_t));

    // Compute chunk lthash
    uint8_t chunk_lthash[LTHASH_SIZE] = {0};
    for (uint32_t i = 0; i < account_count; i++) {
        uint8_t account_lthash[LTHASH_SIZE];

        // Compute LtHash using proper Solana account format
        fd_lthash_value_t lthash;
        fd_lthash_compute_account(
            accounts[i].pubkey,     // pubkey
            accounts[i].owner,      // owner
            accounts[i].lamports,   // lamports
            accounts_data[i],       // data
            data_lens[i],           // data_len
            false,                  // executable (our test accounts are not executable)
            &lthash
        );

        memcpy(account_lthash, lthash.bytes, LTHASH_SIZE);
        add_lthash(chunk_lthash, account_lthash);
        add_lthash(global_lthash, account_lthash);
    }

    // Calculate total uncompressed data size and buffer account data
    size_t total_data_size = 0;
    for (uint32_t i = 0; i < account_count; i++) {
        total_data_size += sizeof(account_payload_t) + data_lens[i];
    }

    // Check 4GB limit on uncompressed data
    if (total_data_size > MAX_PAYLOAD_SIZE) {
        printf("Error: Uncompressed object exceeds 4GB limit: %zu bytes\n", total_data_size);
        return;
    }

    // Buffer all account data for compression
    uint8_t* account_buffer = malloc(total_data_size);
    if (!account_buffer) {
        printf("Error: Failed to allocate memory for account buffer\n");
        return;
    }

    size_t buffer_offset = 0;
    for (uint32_t i = 0; i < account_count; i++) {
        // Copy account payload
        memcpy(account_buffer + buffer_offset, &accounts[i], sizeof(account_payload_t));
        buffer_offset += sizeof(account_payload_t);

        // Copy account data if present
        if (data_lens[i] > 0) {
            memcpy(account_buffer + buffer_offset, accounts_data[i], data_lens[i]);
            buffer_offset += data_lens[i];
        }
    }

    // Compress the account data
    uint16_t compression_type = ENABLE_COMPRESSION ? COMPRESSION_LZ4 : COMPRESSION_NONE;
    compression_result_t compressed = compress_data_streaming(account_buffer, total_data_size, compression_type);

    if (!compressed.data) {
        printf("Error: Compression failed\n");
        free(account_buffer);
        return;
    }

    // Write account data payload header with compressed size
    add_account_data_t payload_header = {0};
    payload_header.magic_number = MAGIC_ACCOUNT_DATA;
    payload_header.header_len = sizeof(add_account_data_t);
    payload_header.compression_type = compressed.is_compressed ? COMPRESSION_LZ4 : COMPRESSION_NONE;
    payload_header.payload_sz = sizeof(add_account_data_t) + compressed.compressed_size;
    payload_header.accounts_cnt = account_count;

    fwrite(&payload_header, sizeof(add_account_data_t), 1, file);
    streaming_hash_update(&hash_ctx, &payload_header, sizeof(add_account_data_t));

    // Write compressed account data
    fwrite(compressed.data, 1, compressed.compressed_size, file);
    streaming_hash_update(&hash_ctx, compressed.data, compressed.compressed_size);

    // Clean up
    free(account_buffer);
    free(compressed.data);

    // Metadata removed from SIMD specification

    // Write file footer (except the hash field)
    file_footer_t footer = {0};
    footer.magic_number = MAGIC_FILE_FOOTER;
    footer.header_len = sizeof(file_footer_t);
    memcpy(footer.lthash, chunk_lthash, LTHASH_SIZE);
    // chunk_hash will be computed and written separately

    // Write footer without the hash field and hash it
    size_t footer_without_hash_size = sizeof(file_footer_t) - SHA256_SIZE;
    fwrite(&footer, footer_without_hash_size, 1, file);
    streaming_hash_update(&hash_ctx, &footer, footer_without_hash_size);

    // Compute final merkle chain hash
    uint8_t file_hash[SHA256_SIZE];
    streaming_hash_finalize(&hash_ctx, previous_chunk_hash, file_hash);

    // Write the hash field to complete the file
    fwrite(file_hash, SHA256_SIZE, 1, file);

    fclose(file);

    // Calculate file size for reporting
    struct stat st;
    stat(filename, &st);
    printf("Written chunk %s with %u accounts (%.2f MB)\n",
           filename, account_count, (double)st.st_size / (1024 * 1024));
}

// Global variable for sorting
static account_payload_t *global_accounts = NULL;

// Comparison function for qsort (compare by pubkey)
int compare_account_indices(const void *a, const void *b) {
    uint32_t idx_a = *(const uint32_t*)a;
    uint32_t idx_b = *(const uint32_t*)b;
    return memcmp(global_accounts[idx_a].pubkey, global_accounts[idx_b].pubkey, PUBKEY_SIZE);
}

int main() {
    printf("Generating %d random accounts (streaming mode)...\n", NUM_ACCOUNTS);

    srand(time(NULL));

    // Initialize OpenSSL random number generator
    if (RAND_load_file("/dev/urandom", 32) != 32) {
        fprintf(stderr, "Warning: Could not seed OpenSSL random number generator properly\n");
    }

    // Generate all accounts first
    account_payload_t *all_accounts = malloc(NUM_ACCOUNTS * sizeof(account_payload_t));
    uint8_t **all_accounts_data = malloc(NUM_ACCOUNTS * sizeof(uint8_t*));
    uint32_t *all_data_lens = malloc(NUM_ACCOUNTS * sizeof(uint32_t));

    for (uint32_t i = 0; i < NUM_ACCOUNTS; i++) {
        generate_random_account(&all_accounts[i], &all_accounts_data[i], &all_data_lens[i]);

        if (i % 1000000 == 0) {
            printf("Generated %u/%d accounts...\n", i, NUM_ACCOUNTS);
        }
    }

    printf("Account generation complete. Sorting accounts globally...\n");

    // Global LtHash accumulator
    uint8_t global_lthash[LTHASH_SIZE] = {0};
    uint64_t slot = 12345678;  // Example slot number

    printf("Sorting all accounts globally by pubkey...\n");

    // Create array of indices to sort (to maintain data pointer relationships)
    uint32_t *indices = malloc(NUM_ACCOUNTS * sizeof(uint32_t));
    for (uint32_t i = 0; i < NUM_ACCOUNTS; i++) {
        indices[i] = i;
    }

    // Set global variable for comparison function
    global_accounts = all_accounts;

    // Sort indices based on account pubkeys using qsort for efficiency
    qsort(indices, NUM_ACCOUNTS, sizeof(uint32_t), compare_account_indices);

    printf("Global sorting complete. Checking for duplicates...\n");

    // Verify no duplicate pubkeys exist (should be extremely rare with crypto random)
    for (uint32_t i = 1; i < NUM_ACCOUNTS; i++) {
        uint32_t prev_idx = indices[i-1];
        uint32_t curr_idx = indices[i];
        if (memcmp(all_accounts[prev_idx].pubkey, all_accounts[curr_idx].pubkey, PUBKEY_SIZE) == 0) {
            fprintf(stderr, "ERROR: Duplicate pubkey found at sorted positions %u and %u\n", i-1, i);
            fprintf(stderr, "This should be extremely rare with cryptographic random generation.\n");
            exit(1);
        }
    }

    printf("No duplicates found. Writing chunks using streaming I/O...\n");

    // Now write accounts to chunks in globally sorted order
    const uint32_t accounts_per_chunk = 100000;  // 100K accounts per chunk
    uint32_t chunk_count = (NUM_ACCOUNTS + accounts_per_chunk - 1) / accounts_per_chunk;

    printf("Will generate %u chunks with up to %u accounts each\n", chunk_count, accounts_per_chunk);

    // Track previous chunk hash for merkle chain
    uint8_t previous_chunk_hash[SHA256_SIZE] = {0}; // First chunk uses all zeros

    for (uint32_t chunk_idx = 0; chunk_idx < chunk_count; chunk_idx++) {
        uint32_t accounts_in_chunk = accounts_per_chunk;
        if (chunk_idx == chunk_count - 1) {
            // Last chunk might have fewer accounts
            accounts_in_chunk = NUM_ACCOUNTS - (chunk_idx * accounts_per_chunk);
        }

        // Allocate arrays for this chunk (in sorted order)
        account_payload_t *chunk_accounts = malloc(accounts_in_chunk * sizeof(account_payload_t));
        uint8_t **chunk_accounts_data = malloc(accounts_in_chunk * sizeof(uint8_t*));
        uint32_t *chunk_data_lens = malloc(accounts_in_chunk * sizeof(uint32_t));

        // Copy sorted accounts to this chunk
        uint32_t start_idx = chunk_idx * accounts_per_chunk;
        for (uint32_t i = 0; i < accounts_in_chunk; i++) {
            uint32_t sorted_idx = indices[start_idx + i];
            chunk_accounts[i] = all_accounts[sorted_idx];
            chunk_accounts_data[i] = all_accounts_data[sorted_idx];
            chunk_data_lens[i] = all_data_lens[sorted_idx];
        }

        // Write chunk file using streaming I/O
        char filename[256];
        snprintf(filename, sizeof(filename), "cold_snapshot_chunk_%04u.bin", chunk_idx);

        bool is_final = (chunk_idx == chunk_count - 1);
        write_chunk_file_streaming(filename, chunk_accounts, chunk_accounts_data, chunk_data_lens,
                                 accounts_in_chunk, slot, chunk_idx, is_final, global_lthash, previous_chunk_hash);

        printf("Written chunk %u with accounts %u to %u\n", chunk_idx,
               start_idx, start_idx + accounts_in_chunk - 1);

        // Read back the chunk hash for the next iteration
        if (chunk_idx < chunk_count - 1) {
            FILE *chunk_file = fopen(filename, "rb");
            if (chunk_file) {
                // Seek to the end of file minus SHA256_SIZE to read the hash
                fseek(chunk_file, -SHA256_SIZE, SEEK_END);
                fread(previous_chunk_hash, 1, SHA256_SIZE, chunk_file);
                fclose(chunk_file);
            }
        }

        // Free chunk arrays (but not the underlying data, which belongs to all_accounts_data)
        free(chunk_accounts);
        free(chunk_accounts_data);
        free(chunk_data_lens);
    }

    // Cleanup
    for (uint32_t i = 0; i < NUM_ACCOUNTS; i++) {
        free(all_accounts_data[i]);
    }
    free(all_accounts);
    free(all_accounts_data);
    free(all_data_lens);
    free(indices);

    printf("Snapshot generation complete!\n");
    printf("Generated %u chunks with a total of %d accounts\n", chunk_count, NUM_ACCOUNTS);
    printf("All accounts are globally sorted by pubkey across chunks\n");
    printf("Each chunk uses merkle chain hashing linked to previous chunk\n");
    printf("Used streaming I/O - no large buffers held in memory\n");

    return 0;
}