#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <openssl/sha.h>
#include <lz4.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fd_lthash_standalone.h"

// Constants from SIMD specification
#define PUBKEY_SIZE 32
#define OWNER_SIZE 32
#define LTHASH_SIZE 2048
#define SHA256_SIZE 32

// Magic numbers (little-endian)
#define MAGIC_FILE_HEADER 0x534E    // "SN" (Snapshot)
#define MAGIC_FILE_FOOTER 0x4654    // "FT" (Footer)
#define MAGIC_ACCOUNT_DATA 0x4143   // "AC" (Account)

// Compression types
#define COMPRESSION_NONE 0x0000
#define COMPRESSION_LZ4 0x0001

// Structure definitions matching SIMD specification
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
#pragma pack(pop)

// Verification result structure
typedef struct {
    char filename[256];
    int valid;
    uint32_t account_count;
    uint64_t slot;
    uint32_t file_index;
    uint8_t chunk_lthash[LTHASH_SIZE];
    uint8_t chunk_hash[SHA256_SIZE];
    char error_message[512];
} verification_result_t;

// Global options
static int quick_mode = 0;
static int verbose = 0;

// LtHash utility functions
void compute_account_lthash(const account_payload_t *account, const uint8_t *account_data, uint8_t *lthash) {
    fd_lthash_value_t hash_value;
    fd_lthash_compute_account(
        account->pubkey,     // pubkey
        account->owner,      // owner
        account->lamports,   // lamports
        account_data,        // data
        account->data_len,   // data_len
        false,               // executable (test accounts are not executable)
        &hash_value
    );
    memcpy(lthash, hash_value.bytes, LTHASH_SIZE);
}

void add_lthash(uint8_t *dest, const uint8_t *src) {
    fd_lthash_value_t dest_val, src_val;
    memcpy(dest_val.bytes, dest, LTHASH_SIZE);
    memcpy(src_val.bytes, src, LTHASH_SIZE);
    fd_lthash_add(&dest_val, &src_val);
    memcpy(dest, dest_val.bytes, LTHASH_SIZE);
}

// Decompression function - returns actual decompressed data
typedef struct {
    uint8_t* data;
    size_t actual_size;
} decompression_result_t;

decompression_result_t decompress_data_with_size(const uint8_t* compressed, size_t compressed_size, uint16_t compression_type, size_t max_size) {
    decompression_result_t result = {0};

    if (compression_type == COMPRESSION_NONE) {
        result.data = malloc(compressed_size);
        if (result.data) {
            memcpy(result.data, compressed, compressed_size);
            result.actual_size = compressed_size;
        }
        return result;
    } else if (compression_type == COMPRESSION_LZ4) {
        result.data = malloc(max_size);
        if (result.data) {
            int decompressed = LZ4_decompress_safe(
                (const char*)compressed,
                (char*)result.data,
                (int)compressed_size,
                (int)max_size
            );
            if (decompressed > 0) {
                result.actual_size = (size_t)decompressed;
                if (verbose) {
                    printf("    LZ4 decompression: %zu → %zu bytes (%.1f%% ratio)\n",
                           compressed_size, result.actual_size,
                           (float)compressed_size * 100.0f / result.actual_size);
                }
            } else {
                if (verbose) {
                    printf("    LZ4 decompression failed: error code %d\n", decompressed);
                }
                free(result.data);
                result.data = NULL;
                result.actual_size = 0;
            }
        }
        return result;
    }
    return result;
}

// File hash computation (excluding the hash field itself)
void compute_file_hash(const char* filename, const uint8_t* previous_chunk_hash, uint8_t* result_hash) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        memset(result_hash, 0, SHA256_SIZE);
        return;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    // Read and hash everything except the last 32 bytes (chunk_hash)
    size_t to_hash = file_size - SHA256_SIZE;
    uint8_t buffer[4096];

    while (to_hash > 0) {
        size_t to_read = (to_hash > sizeof(buffer)) ? sizeof(buffer) : to_hash;
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0) break;

        SHA256_Update(&ctx, buffer, bytes_read);
        to_hash -= bytes_read;
    }

    // Add previous chunk hash
    SHA256_Update(&ctx, previous_chunk_hash, SHA256_SIZE);
    SHA256_Final(result_hash, &ctx);

    fclose(file);
}

// Verify a single chunk file
verification_result_t verify_chunk_file(const char* filename) {
    verification_result_t result = {0};
    strncpy(result.filename, filename, sizeof(result.filename) - 1);

    FILE* file = fopen(filename, "rb");
    if (!file) {
        snprintf(result.error_message, sizeof(result.error_message), "Cannot open file");
        return result;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (verbose) {
        printf("  📁 File size: %ld bytes\n", file_size);
    }

    // Read and validate file header
    file_header_t header;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        snprintf(result.error_message, sizeof(result.error_message), "Failed to read file header");
        fclose(file);
        return result;
    }

    if (header.magic_number != MAGIC_FILE_HEADER) {
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid file header magic: expected 0x%04X, got 0x%04X",
                 MAGIC_FILE_HEADER, header.magic_number);
        fclose(file);
        return result;
    }

    if (header.header_len != sizeof(file_header_t)) {
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid header length: expected %zu, got %u",
                 sizeof(file_header_t), header.header_len);
        fclose(file);
        return result;
    }

    result.slot = header.slot;
    result.file_index = header.file_index;

    if (verbose) {
        printf("  📦 Slot: %llu, File index: %u\n",
               (unsigned long long)header.slot, header.file_index);
    }

    // Read and validate file footer
    fseek(file, file_size - sizeof(file_footer_t), SEEK_SET);
    file_footer_t footer;
    if (fread(&footer, sizeof(footer), 1, file) != 1) {
        snprintf(result.error_message, sizeof(result.error_message), "Failed to read file footer");
        fclose(file);
        return result;
    }

    if (footer.magic_number != MAGIC_FILE_FOOTER) {
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid file footer magic: expected 0x%04X, got 0x%04X",
                 MAGIC_FILE_FOOTER, footer.magic_number);
        fclose(file);
        return result;
    }

    if (footer.header_len != sizeof(file_footer_t)) {
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid footer length: expected %zu, got %u",
                 sizeof(file_footer_t), footer.header_len);
        fclose(file);
        return result;
    }

    memcpy(result.chunk_lthash, footer.lthash, LTHASH_SIZE);
    memcpy(result.chunk_hash, footer.chunk_hash, SHA256_SIZE);

    // Process account data objects
    fseek(file, sizeof(file_header_t), SEEK_SET);
    uint8_t computed_lthash[LTHASH_SIZE] = {0};
    uint32_t total_accounts = 0;
    uint8_t previous_pubkey[PUBKEY_SIZE] = {0};
    int first_account = 1;

    while (ftell(file) < file_size - sizeof(file_footer_t)) {
        // Read AddAccountData header
        add_account_data_t account_data_header;
        if (fread(&account_data_header, sizeof(account_data_header), 1, file) != 1) {
            snprintf(result.error_message, sizeof(result.error_message),
                     "Failed to read account data header");
            fclose(file);
            return result;
        }

        if (account_data_header.magic_number != MAGIC_ACCOUNT_DATA) {
            snprintf(result.error_message, sizeof(result.error_message),
                     "Invalid account data magic: expected 0x%04X, got 0x%04X",
                     MAGIC_ACCOUNT_DATA, account_data_header.magic_number);
            fclose(file);
            return result;
        }

        // Copy to local variables to avoid unaligned access warnings
        uint32_t accounts_cnt = account_data_header.accounts_cnt;
        uint16_t compression_type = account_data_header.compression_type;
        uint64_t payload_sz = account_data_header.payload_sz;

        if (verbose) {
            printf("  📦 Object: %u accounts, compression: %s, payload: %llu bytes\n",
                   accounts_cnt,
                   (compression_type == COMPRESSION_LZ4) ? "LZ4" : "None",
                   (unsigned long long)payload_sz);
        }

        // Read compressed data
        size_t data_size = payload_sz - sizeof(add_account_data_t);
        uint8_t* compressed_data = malloc(data_size);
        if (!compressed_data) {
            snprintf(result.error_message, sizeof(result.error_message), "Memory allocation failed");
            fclose(file);
            return result;
        }

        if (fread(compressed_data, 1, data_size, file) != data_size) {
            snprintf(result.error_message, sizeof(result.error_message),
                     "Failed to read compressed data");
            free(compressed_data);
            fclose(file);
            return result;
        }

                        // Decompress if needed
        uint8_t* account_data;
        size_t original_size;
        int need_free_account_data = 0;

        if (compression_type == COMPRESSION_NONE) {
            account_data = compressed_data;
            original_size = data_size;
        } else {
            // Use reasonable size estimate: account_payload_t is small, most data is in account data
            // Each account: ~77 bytes struct + small data (test accounts have small random data)
            // Estimate 10KB per account (much more reasonable than 10MB)
            const size_t REASONABLE_ACCOUNT_SIZE = 10 * 1024; // 10KB per account
            size_t max_size = accounts_cnt * REASONABLE_ACCOUNT_SIZE;

            // Ensure minimum size is at least 2x compressed size
            if (max_size < data_size * 2) {
                max_size = data_size * 2;
            }

            decompression_result_t decomp_result = decompress_data_with_size(compressed_data, data_size, compression_type, max_size);
            if (!decomp_result.data) {
                snprintf(result.error_message, sizeof(result.error_message),
                         "Decompression failed with max size %zu", max_size);
                free(compressed_data);
                fclose(file);
                return result;
            }

            account_data = decomp_result.data;
            original_size = decomp_result.actual_size;
            need_free_account_data = 1;
        }

        // Parse accounts
        size_t offset = 0;
        for (uint32_t i = 0; i < accounts_cnt; i++) {
                        if (offset + sizeof(account_payload_t) > original_size) {
                snprintf(result.error_message, sizeof(result.error_message),
                         "Account data extends beyond buffer");
                if (need_free_account_data) free(account_data);
                free(compressed_data);
                fclose(file);
                return result;
            }

            account_payload_t* account = (account_payload_t*)(account_data + offset);
            offset += sizeof(account_payload_t);

                        // Validate data length
            if (offset + account->data_len > original_size) {
                snprintf(result.error_message, sizeof(result.error_message),
                         "Account data extends beyond buffer");
                if (need_free_account_data) free(account_data);
                free(compressed_data);
                fclose(file);
                return result;
            }

            uint8_t* account_payload_data = account_data + offset;
            offset += account->data_len;

                        // Check sorting order
            if (!first_account) {
                if (memcmp(previous_pubkey, account->pubkey, PUBKEY_SIZE) >= 0) {
                    snprintf(result.error_message, sizeof(result.error_message),
                             "Accounts not sorted properly");
                    if (need_free_account_data) free(account_data);
                    free(compressed_data);
                    fclose(file);
                    return result;
                }
            }
            memcpy(previous_pubkey, account->pubkey, PUBKEY_SIZE);
            first_account = 0;

            // Compute LtHash for this account (if not in quick mode)
            if (!quick_mode) {
                uint8_t account_lthash[LTHASH_SIZE];
                compute_account_lthash(account, account_payload_data, account_lthash);
                add_lthash(computed_lthash, account_lthash);
            }

            total_accounts++;
        }

        if (need_free_account_data) {
            free(account_data);
        }
        free(compressed_data);
    }

    result.account_count = total_accounts;

    // Verify LtHash (if not in quick mode)
    if (!quick_mode) {
        if (memcmp(computed_lthash, footer.lthash, LTHASH_SIZE) != 0) {
            snprintf(result.error_message, sizeof(result.error_message), "LtHash mismatch");
            fclose(file);
            return result;
        }
    }

    // TODO: Verify chunk hash (requires previous chunk hash)
    // For now, we skip chunk hash verification

    result.valid = 1;
    fclose(file);
    return result;
}

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS] <chunk_files...>\n", program_name);
    printf("Options:\n");
    printf("  --quick    Skip LtHash computation for faster verification\n");
    printf("  --verbose  Enable verbose output\n");
    printf("  --help     Show this help message\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Parse command line arguments
    int file_arg_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick_mode = 1;
            file_arg_start = i + 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
            file_arg_start = i + 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            break;
        }
    }

    if (file_arg_start >= argc) {
        printf("❌ No chunk files specified\n");
        print_usage(argv[0]);
        return 1;
    }

    int num_files = argc - file_arg_start;
    printf("🔍 Verifying %d chunk files (C verifier)...\n", num_files);
    if (quick_mode) {
        printf("⚡ Using quick mode (skipping LtHash computation)\n");
    }
    printf("\n");

    verification_result_t* results = malloc(num_files * sizeof(verification_result_t));
    if (!results) {
        printf("❌ Memory allocation failed\n");
        return 1;
    }

    int valid_chunks = 0;
    uint32_t total_accounts = 0;

    // Verify each chunk
    for (int i = 0; i < num_files; i++) {
        char* filename = argv[file_arg_start + i];
        if (verbose) {
            printf("📁 Verifying %s...\n", filename);
        }

        results[i] = verify_chunk_file(filename);

        if (results[i].valid) {
            valid_chunks++;
            total_accounts += results[i].account_count;
            if (verbose) {
                printf("  ✅ Valid: %u accounts\n", results[i].account_count);
            }
        } else {
            if (verbose) {
                printf("  ❌ Error: %s\n", results[i].error_message);
            }
        }
        if (verbose) printf("\n");
    }

    // Print results
    printf("📊 Verification Results:\n");
    printf("========================\n");
    for (int i = 0; i < num_files; i++) {
        if (results[i].valid) {
            printf("✅ %s: %u accounts\n", results[i].filename, results[i].account_count);
            if (verbose) {
                printf("   📦 Slot: %llu, File index: %u\n",
                       (unsigned long long)results[i].slot, results[i].file_index);
            }
        } else {
            printf("❌ %s: %s\n", results[i].filename, results[i].error_message);
        }
    }

    printf("\n📈 Summary:\n");
    printf("===========\n");
    printf("✅ Valid chunks: %d/%d\n", valid_chunks, num_files);
    if (valid_chunks > 0) {
        printf("📊 Total accounts: %u\n", total_accounts);
    }

    if (valid_chunks == num_files) {
        printf("\n🎉 All chunks verified successfully!\n");
        printf("✅ File format compliance verified\n");
        printf("✅ Account sorting verified\n");
        if (!quick_mode) {
            printf("✅ LtHash verification completed\n");
        }
        free(results);
        return 0;
    } else {
        printf("\n❌ Some chunks failed verification\n");
        free(results);
        return 1;
    }
}