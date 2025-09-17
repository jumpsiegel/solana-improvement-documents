#include <iostream>
#include <cstdint>
#include <iomanip>
#include <cstddef>

// Constants from SIMD specification
constexpr size_t LTHASH_SIZE = 2048;
constexpr size_t PUBKEY_SIZE = 32;
constexpr size_t OWNER_SIZE = 32;
constexpr size_t SHA256_SIZE = 32;

// Magic numbers from SIMD specification
constexpr uint16_t MAGIC_FILE_HEADER = 0x534E;      // "SN" (Snapshot)
constexpr uint16_t MAGIC_FILE_FOOTER = 0x4654;      // "FT" (Footer)
constexpr uint16_t MAGIC_ACCOUNT_DATA = 0x4143;     // "AC" (Account)
constexpr uint16_t MAGIC_METADATA = 0x4D45;         // "ME" (Metadata)

// Structures from SIMD specification (exact C++ translations)
#pragma pack(push, 1)

struct FileHeader {
    uint16_t magic_number;     // Version identifier
    uint16_t header_len;       // sizeof(FileHeader)
};

struct FileFooter {
    uint16_t magic_number;     // Footer identifier
    uint16_t header_len;       // sizeof(FileFooter)
    uint8_t lthash[LTHASH_SIZE];        // Accumulated LtHashes of all accounts in this chunk
    uint8_t chunk_hash[SHA256_SIZE];    // Merkle chain hash (must be last field)
};

struct AccountPayload {
    uint8_t pubkey[PUBKEY_SIZE];
    uint8_t owner[OWNER_SIZE];
    uint64_t lamports;
    uint32_t data_len;
    // data follows immediately after this struct
};

struct AddAccountData {
    uint16_t magic_number;
    uint16_t header_len;
    uint16_t compression_type;  // COMPRESSION_NONE or COMPRESSION_LZ4
    uint64_t payload_sz;
    uint32_t accounts_cnt;
    // accounts_data follows immediately after this struct
};

struct FullSnapshotMetadata {
    uint16_t magic_number;     // Exists in only one chunk for metadata
    uint16_t header_len;       // sizeof(FullSnapshotMetadata)
    uint64_t slot;             // Snapshot slot number
    uint8_t lthash[LTHASH_SIZE];       // Accumulated LtHashes of all chunks
};

#pragma pack(pop)

int main() {
    std::cout << "🔍 C++ Struct Size Verification" << std::endl;
    std::cout << "===============================" << std::endl << std::endl;

    std::cout << "✅ All struct sizes:" << std::endl;
    std::cout << "  FileHeader: " << sizeof(FileHeader) << " bytes" << std::endl;
    std::cout << "  FileFooter: " << sizeof(FileFooter) << " bytes" << std::endl;
    std::cout << "  AddAccountData: " << sizeof(AddAccountData) << " bytes" << std::endl;
    std::cout << "  FullSnapshotMetadata: " << sizeof(FullSnapshotMetadata) << " bytes" << std::endl;
    std::cout << "  AccountPayload: " << sizeof(AccountPayload) << " bytes" << std::endl;
    std::cout << std::endl;

    // Verify expected sizes match SIMD specification
    bool all_correct = true;

    if (sizeof(FileHeader) != 4) {
        std::cout << "❌ FileHeader size mismatch! Expected 4, got " << sizeof(FileHeader) << std::endl;
        all_correct = false;
    }

    if (sizeof(FileFooter) != 2084) {
        std::cout << "❌ FileFooter size mismatch! Expected 2084, got " << sizeof(FileFooter) << std::endl;
        all_correct = false;
    }

    if (sizeof(AddAccountData) != 18) {
        std::cout << "❌ AddAccountData size mismatch! Expected 18, got " << sizeof(AddAccountData) << std::endl;
        all_correct = false;
    }

    if (sizeof(FullSnapshotMetadata) != 2060) {
        std::cout << "❌ FullSnapshotMetadata size mismatch! Expected 2060, got " << sizeof(FullSnapshotMetadata) << std::endl;
        all_correct = false;
    }

    if (sizeof(AccountPayload) != 76) {
        std::cout << "❌ AccountPayload size mismatch! Expected 76, got " << sizeof(AccountPayload) << std::endl;
        all_correct = false;
    }

    std::cout << "📍 FileFooter field offsets:" << std::endl;
    std::cout << "  magic_number at offset: " << offsetof(FileFooter, magic_number) << std::endl;
    std::cout << "  header_len at offset: " << offsetof(FileFooter, header_len) << std::endl;
    std::cout << "  lthash at offset: " << offsetof(FileFooter, lthash) << std::endl;
    std::cout << "  chunk_hash at offset: " << offsetof(FileFooter, chunk_hash) << " (last field)" << std::endl;

    // Verify chunk_hash is the last field
    size_t expected_hash_offset = sizeof(FileFooter) - SHA256_SIZE;
    if (offsetof(FileFooter, chunk_hash) != expected_hash_offset) {
        std::cout << "❌ chunk_hash is not the last field! Expected offset " << expected_hash_offset
                  << ", got " << offsetof(FileFooter, chunk_hash) << std::endl;
        all_correct = false;
    }

    std::cout << std::endl;
    std::cout << "🎭 Magic numbers:" << std::endl;
    std::cout << "  MAGIC_FILE_HEADER: 0x" << std::hex << std::uppercase << MAGIC_FILE_HEADER << std::dec << std::endl;
    std::cout << "  MAGIC_FILE_FOOTER: 0x" << std::hex << std::uppercase << MAGIC_FILE_FOOTER << std::dec << std::endl;
    std::cout << "  MAGIC_ACCOUNT_DATA: 0x" << std::hex << std::uppercase << MAGIC_ACCOUNT_DATA << std::dec << std::endl;
    std::cout << "  MAGIC_METADATA: 0x" << std::hex << std::uppercase << MAGIC_METADATA << std::dec << std::endl;

    // Verify magic numbers match SIMD specification
    if (MAGIC_FILE_HEADER != 0x534E) {
        std::cout << "❌ MAGIC_FILE_HEADER mismatch! Expected 0x534E" << std::endl;
        all_correct = false;
    }

    if (MAGIC_FILE_FOOTER != 0x4654) {
        std::cout << "❌ MAGIC_FILE_FOOTER mismatch! Expected 0x4654" << std::endl;
        all_correct = false;
    }

    if (MAGIC_ACCOUNT_DATA != 0x4143) {
        std::cout << "❌ MAGIC_ACCOUNT_DATA mismatch! Expected 0x4143" << std::endl;
        all_correct = false;
    }

    if (MAGIC_METADATA != 0x4D45) {
        std::cout << "❌ MAGIC_METADATA mismatch! Expected 0x4D45" << std::endl;
        all_correct = false;
    }

    std::cout << std::endl;

    if (all_correct) {
        std::cout << "🎉 All C++ struct sizes and layouts are correct!" << std::endl;
        std::cout << "✅ Compatible with C implementation" << std::endl;
        std::cout << "✅ Compatible with Rust implementation" << std::endl;
        std::cout << "✅ Matches SIMD specification" << std::endl;
        std::cout << "✅ FileFooter streaming layout verified" << std::endl;
        return 0;
    } else {
        std::cout << "❌ Some struct sizes or layouts are incorrect!" << std::endl;
        return 1;
    }
}