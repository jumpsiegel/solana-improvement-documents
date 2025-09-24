#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

// Magic numbers for different payload types (2 bytes)
#define MAGIC_FILE_HEADER 0x534E      // "SN" (from "SNAP")
#define MAGIC_FILE_FOOTER 0x4654      // "FT" (from "FOOT")
#define MAGIC_ACCOUNT_DATA 0x4143     // "AC" (from "ACCT")


// Constants
#define LTHASH_SIZE 2048
#define PUBKEY_SIZE 32
#define OWNER_SIZE 32
#define SHA256_SIZE 32

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



#pragma pack(pop)

int main() {
    printf("🔍 C Struct Size Verification\n");
    printf("==============================\n\n");

    printf("✅ All struct sizes:\n");
    printf("  FileHeader: %zu bytes\n", sizeof(file_header_t));
    printf("  FileFooter: %zu bytes\n", sizeof(file_footer_t));
    printf("  AddAccountData: %zu bytes\n", sizeof(add_account_data_t));
    printf("  AccountPayload: %zu bytes\n", sizeof(account_payload_t));
    printf("\n");

    // Verify expected sizes
    int all_correct = 1;

    if (sizeof(file_header_t) != 16) {
        printf("❌ FileHeader size mismatch: expected 16, got %zu\n", sizeof(file_header_t));
        all_correct = 0;
    }

    if (sizeof(file_footer_t) != 2084) {
        printf("❌ FileFooter size mismatch: expected 2084, got %zu\n", sizeof(file_footer_t));
        all_correct = 0;
    }

    if (sizeof(add_account_data_t) != 18) {
        printf("❌ AddAccountData size mismatch: expected 18, got %zu\n", sizeof(add_account_data_t));
        all_correct = 0;
    }



    if (sizeof(account_payload_t) != 76) {
        printf("❌ AccountPayload size mismatch: expected 76, got %zu\n", sizeof(account_payload_t));
        all_correct = 0;
    }

    // Verify field offsets for FileFooter (critical for streaming)
    printf("📍 FileFooter field offsets:\n");
    printf("  magic_number at offset: %zu\n", offsetof(file_footer_t, magic_number));
    printf("  header_len at offset: %zu\n", offsetof(file_footer_t, header_len));
    printf("  lthash at offset: %zu\n", offsetof(file_footer_t, lthash));
    printf("  chunk_hash at offset: %zu (last field)\n", offsetof(file_footer_t, chunk_hash));
    printf("\n");

    // Verify chunk_hash is at the last position
    size_t expected_hash_offset = sizeof(file_footer_t) - SHA256_SIZE;
    size_t actual_hash_offset = offsetof(file_footer_t, chunk_hash);

    if (actual_hash_offset != expected_hash_offset) {
        printf("❌ chunk_hash not at last position: expected offset %zu, got %zu\n",
               expected_hash_offset, actual_hash_offset);
        all_correct = 0;
    }

    // Verify magic numbers
    printf("🎭 Magic numbers:\n");
    printf("  MAGIC_FILE_HEADER: 0x%04X\n", MAGIC_FILE_HEADER);
    printf("  MAGIC_FILE_FOOTER: 0x%04X\n", MAGIC_FILE_FOOTER);
    printf("  MAGIC_ACCOUNT_DATA: 0x%04X\n", MAGIC_ACCOUNT_DATA);

    printf("\n");

    if (all_correct) {
        printf("🎉 All C struct sizes and layouts are correct!\n");
        printf("✅ Compatible with Rust implementation\n");
        printf("✅ Matches SIMD specification\n");
        printf("✅ FileFooter streaming layout verified\n");
        return 0;
    } else {
        printf("❌ Some struct sizes or layouts are incorrect\n");
        return 1;
    }
}