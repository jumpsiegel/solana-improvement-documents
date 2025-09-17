#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <openssl/sha.h>
#include <lz4.h>

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

// Compression types from SIMD specification
constexpr uint16_t COMPRESSION_NONE = 0x0000;
constexpr uint16_t COMPRESSION_LZ4 = 0x0001;

// Maximum payload size per AddAccountData object (4GB)
constexpr uint64_t MAX_PAYLOAD_SIZE = 4ULL * 1024 * 1024 * 1024;

// Structures from SIMD specification (exact C++ translations)
#pragma pack(push, 1)

struct FileHeader {
    uint16_t magic_number;     // Version identifier
    uint16_t header_len;       // sizeof(FileHeader)
    uint64_t slot;             // Snapshot slot number
    uint32_t file_index;       // Chunk file index (0, 1, 2, ...)
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

// Simple LtHash implementation (placeholder - would use real implementation in production)
class LtHash {
public:
    std::array<uint8_t, LTHASH_SIZE> bytes;

    LtHash() {
        bytes.fill(0);
    }

    void add(const LtHash& other) {
        // Simplified XOR for demonstration (real implementation would use lattice operations)
        for (size_t i = 0; i < LTHASH_SIZE; ++i) {
            bytes[i] ^= other.bytes[i];
        }
    }

        static LtHash compute_account(const uint8_t* pubkey, const uint8_t* owner,
                                 uint64_t lamports, const uint8_t* data, size_t data_len) {
        LtHash result;
        // Match C implementation's field ordering for LtHash computation
        // Order: lamports, data, executable, owner, pubkey
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, &lamports, sizeof(lamports));
        SHA256_Update(&ctx, data, data_len);

        uint8_t executable = 0; // executable flag (false)
        SHA256_Update(&ctx, &executable, 1);
        SHA256_Update(&ctx, owner, OWNER_SIZE);
        SHA256_Update(&ctx, pubkey, PUBKEY_SIZE);

        uint8_t temp_hash[SHA256_SIZE];
        SHA256_Final(temp_hash, &ctx);

        // Expand SHA256 to LTHASH_SIZE (simplified)
        for (size_t i = 0; i < LTHASH_SIZE; ++i) {
            result.bytes[i] = temp_hash[i % SHA256_SIZE];
        }

        return result;
    }

    bool operator==(const LtHash& other) const {
        return bytes == other.bytes;
    }
};

// Verification result structure
struct ChunkVerificationResult {
    std::string filename;
    bool is_valid = false;
    std::string error_message;
    uint32_t accounts_count = 0;
    std::array<uint8_t, PUBKEY_SIZE> first_pubkey;
    std::array<uint8_t, PUBKEY_SIZE> last_pubkey;
    LtHash chunk_lthash;
    bool has_metadata = false;
    uint64_t slot = 0;
    LtHash global_lthash_from_metadata;
    std::array<uint8_t, SHA256_SIZE> chunk_hash;

    ChunkVerificationResult(const std::string& fname) : filename(fname) {
        first_pubkey.fill(0);
        last_pubkey.fill(0);
        chunk_hash.fill(0);
    }
};

// Compute SHA256 hash
std::array<uint8_t, SHA256_SIZE> compute_sha256(const uint8_t* data, size_t len) {
    std::array<uint8_t, SHA256_SIZE> result;
    SHA256(data, len, result.data());
    return result;
}

// Extract chunk index from filename
uint32_t extract_chunk_index(const std::string& filename) {
    auto pos = filename.rfind("chunk_");
    if (pos != std::string::npos) {
        pos += 6; // skip "chunk_"
        auto end_pos = filename.find('.', pos);
        if (end_pos != std::string::npos) {
            std::string index_str = filename.substr(pos, end_pos - pos);
            try {
                return static_cast<uint32_t>(std::stoul(index_str));
            } catch (...) {
                return UINT32_MAX;
            }
        }
    }
    return UINT32_MAX;
}

// Verify chunk file according to SIMD specification
ChunkVerificationResult verify_chunk_file(const std::string& filename, bool quick_mode) {
    ChunkVerificationResult result(filename);

    try {
        // Read entire file
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file) {
            result.error_message = "Failed to open file";
            return result;
        }

        auto file_size = file.tellg();
        file.seekg(0);

        if (file_size < static_cast<std::streamoff>(sizeof(FileHeader) + sizeof(FileFooter))) {
            result.error_message = "File too small to contain header and footer";
            return result;
        }

        std::vector<uint8_t> file_data(static_cast<size_t>(file_size));
        file.read(reinterpret_cast<char*>(file_data.data()), file_size);

        size_t offset = 0;

        // 1. Parse FileHeader
        if (offset + sizeof(FileHeader) > file_data.size()) {
            result.error_message = "Invalid file header";
            return result;
        }

        const FileHeader* header = reinterpret_cast<const FileHeader*>(file_data.data() + offset);
        if (header->magic_number != MAGIC_FILE_HEADER) {
            result.error_message = "Invalid header magic number";
            return result;
        }

        if (header->header_len != sizeof(FileHeader)) {
            result.error_message = "Invalid header length";
            return result;
        }

        offset += sizeof(FileHeader);

        // 2. Parse FileFooter (from end)
        if (file_data.size() < sizeof(FileFooter)) {
            result.error_message = "File too small for footer";
            return result;
        }

        const FileFooter* footer = reinterpret_cast<const FileFooter*>(
            file_data.data() + file_data.size() - sizeof(FileFooter));

        if (footer->magic_number != MAGIC_FILE_FOOTER) {
            result.error_message = "Invalid footer magic number";
            return result;
        }

        if (footer->header_len != sizeof(FileFooter)) {
            result.error_message = "Invalid footer length";
            return result;
        }

        // Store chunk hash (skip merkle chain verification for now)
        std::memcpy(result.chunk_hash.data(), footer->chunk_hash, SHA256_SIZE);

        // 3. Parse AddAccountData
        if (offset + sizeof(AddAccountData) > file_data.size()) {
            result.error_message = "Invalid account data header";
            return result;
        }

        const AddAccountData* account_data = reinterpret_cast<const AddAccountData*>(file_data.data() + offset);
        if (account_data->magic_number != MAGIC_ACCOUNT_DATA) {
            result.error_message = "Invalid account data magic number";
            return result;
        }

        if (account_data->header_len != sizeof(AddAccountData)) {
            result.error_message = "Invalid account data header length";
            return result;
        }

        result.accounts_count = account_data->accounts_cnt;

        // Get compressed payload data
        size_t compressed_size = account_data->payload_sz - sizeof(AddAccountData);
        offset += sizeof(AddAccountData);

        if (offset + compressed_size > file_data.size() - sizeof(FileFooter)) {
            result.error_message = "Compressed data extends beyond file";
            return result;
        }

        const uint8_t* compressed_data = file_data.data() + offset;
        std::vector<uint8_t> decompressed_data;

                // 4. Decompress data if needed
        if (account_data->compression_type == COMPRESSION_LZ4) {
            // Use progressive buffer sizing for LZ4 decompression
            size_t estimated_size = compressed_size * 8; // Start with 8x compression ratio
            decompressed_data.resize(estimated_size);

            int decompressed_size = LZ4_decompress_safe(
                reinterpret_cast<const char*>(compressed_data),
                reinterpret_cast<char*>(decompressed_data.data()),
                static_cast<int>(compressed_size),
                static_cast<int>(estimated_size));

            // If initial estimate was too small, try larger buffer
            if (decompressed_size < 0) {
                estimated_size = compressed_size * 20; // Try 20x compression ratio
                decompressed_data.resize(estimated_size);

                decompressed_size = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(compressed_data),
                    reinterpret_cast<char*>(decompressed_data.data()),
                    static_cast<int>(compressed_size),
                    static_cast<int>(estimated_size));
            }

            if (decompressed_size < 0) {
                result.error_message = "LZ4 decompression failed";
                return result;
            }

            decompressed_data.resize(decompressed_size);
        } else if (account_data->compression_type == COMPRESSION_NONE) {
            // No compression, copy data directly
            decompressed_data.assign(compressed_data, compressed_data + compressed_size);
        } else {
            result.error_message = "Unsupported compression type";
            return result;
        }

        // 5. Parse and verify accounts from decompressed data
        LtHash computed_lthash;
        std::array<uint8_t, PUBKEY_SIZE> prev_pubkey;
        prev_pubkey.fill(0);
        bool first_account = true;

        // Parse accounts from decompressed data buffer
        size_t decompressed_offset = 0;
        for (uint32_t i = 0; i < account_data->accounts_cnt; ++i) {
            if (decompressed_offset + sizeof(AccountPayload) > decompressed_data.size()) {
                result.error_message = "Account payload extends beyond decompressed data";
                return result;
            }

            const AccountPayload* account = reinterpret_cast<const AccountPayload*>(
                decompressed_data.data() + decompressed_offset);
            decompressed_offset += sizeof(AccountPayload);

            // Check data bounds
            if (decompressed_offset + account->data_len > decompressed_data.size()) {
                result.error_message = "Account data extends beyond decompressed data";
                return result;
            }

            const uint8_t* account_data_ptr = decompressed_data.data() + decompressed_offset;
            decompressed_offset += account->data_len;

            // Store first and last pubkeys
            if (first_account) {
                std::memcpy(result.first_pubkey.data(), account->pubkey, PUBKEY_SIZE);
                first_account = false;
            }
            if (i == account_data->accounts_cnt - 1) {
                std::memcpy(result.last_pubkey.data(), account->pubkey, PUBKEY_SIZE);
            }

            // Verify sorting
            if (i > 0) {
                if (std::memcmp(prev_pubkey.data(), account->pubkey, PUBKEY_SIZE) >= 0) {
                    result.error_message = "Accounts not sorted at position " + std::to_string(i);
                    return result;
                }
            }

            std::memcpy(prev_pubkey.data(), account->pubkey, PUBKEY_SIZE);

            // Compute LtHash (skip in quick mode)
            if (!quick_mode) {
                auto account_lthash = LtHash::compute_account(
                    account->pubkey, account->owner, account->lamports,
                    account_data_ptr, account->data_len);
                computed_lthash.add(account_lthash);
            }
        }

        // Update file offset to skip over compressed data
        offset += compressed_size;

        // 5. Verify chunk LtHash (skip in quick mode)
        if (!quick_mode) {
            LtHash footer_lthash;
            std::memcpy(footer_lthash.bytes.data(), footer->lthash, LTHASH_SIZE);

            if (!(computed_lthash == footer_lthash)) {
                result.error_message = "Chunk LtHash mismatch";
                return result;
            }
            result.chunk_lthash = computed_lthash;
        } else {
            std::memcpy(result.chunk_lthash.bytes.data(), footer->lthash, LTHASH_SIZE);
        }

        // 6. Metadata removed from SIMD specification

        result.is_valid = true;
        return result;

    } catch (const std::exception& e) {
        result.error_message = "Exception: " + std::string(e.what());
        return result;
    }
}

// Print hex string
void print_hex(const uint8_t* data, size_t len, size_t max_len = 16) {
    for (size_t i = 0; i < std::min(len, max_len); ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(data[i]);
    }
    if (len > max_len) {
        std::cout << "...";
    }
}

int main(int argc, char* argv[]) {
    try {
        bool quick_mode = false;
        std::vector<std::string> file_patterns;

        // Parse command line arguments
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--quick") {
                quick_mode = true;
            } else {
                file_patterns.push_back(arg);
            }
        }

        if (file_patterns.empty()) {
            file_patterns.push_back("*.bin");
        }

        // Find chunk files
        std::vector<std::string> chunk_files;

        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".bin" &&
                    filename.find("chunk") != std::string::npos) {
                    chunk_files.push_back(filename);
                }
            }
        }

        if (chunk_files.empty()) {
            std::cout << "No chunk files found" << std::endl;
            return 0;
        }

        // Sort chunk files by index
        std::sort(chunk_files.begin(), chunk_files.end(), [](const std::string& a, const std::string& b) {
            return extract_chunk_index(a) < extract_chunk_index(b);
        });

        std::cout << "🔍 Verifying " << chunk_files.size() << " chunk files (C++)..." << std::endl;
        if (quick_mode) {
            std::cout << "⚡ Using quick mode (skipping LtHash computation)" << std::endl;
        }
        std::cout << std::endl;

        // Verify chunks
        std::vector<ChunkVerificationResult> results;
        uint64_t total_accounts = 0;
        uint32_t valid_chunks = 0;
        LtHash global_lthash;
        LtHash expected_global_lthash;
        bool has_global_metadata = false;

        for (const auto& filename : chunk_files) {
            std::cout << "📁 Verifying " << filename << "..." << std::endl;
            auto result = verify_chunk_file(filename, quick_mode);
            results.push_back(result);

            if (result.is_valid) {
                valid_chunks++;
                total_accounts += result.accounts_count;
                global_lthash.add(result.chunk_lthash);

                if (result.has_metadata) {
                    has_global_metadata = true;
                    expected_global_lthash = result.global_lthash_from_metadata;
                }
            }
        }

        // Print results
        std::cout << "📊 Verification Results:" << std::endl;
        std::cout << "========================" << std::endl;

        for (const auto& result : results) {
            if (result.is_valid) {
                std::cout << "✅ " << result.filename << ": " << result.accounts_count << " accounts" << std::endl;
                if (result.has_metadata) {
                    std::cout << "   📦 Contains metadata for slot " << result.slot << std::endl;
                }
            } else {
                std::cout << "❌ " << result.filename << ": " << result.error_message << std::endl;
            }
        }

        std::cout << std::endl;
        std::cout << "📈 Summary:" << std::endl;
        std::cout << "===========" << std::endl;
        std::cout << "✅ Valid chunks: " << valid_chunks << "/" << chunk_files.size() << std::endl;
        std::cout << "📊 Total accounts: " << total_accounts << std::endl;

        // Global sorting verification
        std::cout << "\n🔄 Verifying global account ordering..." << std::endl;
        bool sorting_valid = true;

        for (size_t i = 1; i < results.size(); ++i) {
            if (results[i-1].is_valid && results[i].is_valid) {
                if (std::memcmp(results[i-1].last_pubkey.data(),
                               results[i].first_pubkey.data(), PUBKEY_SIZE) >= 0) {
                    std::cout << "❌ Global sorting violation between "
                              << results[i-1].filename << " and " << results[i].filename << std::endl;
                    sorting_valid = false;
                }
            }
        }

        if (sorting_valid) {
            std::cout << "✅ Global account ordering verified" << std::endl;
        }

        // Global LtHash verification
        if (!quick_mode && has_global_metadata) {
            std::cout << "\n🧮 Verifying global LtHash..." << std::endl;
            std::cout << "Computed global LtHash: ";
            print_hex(global_lthash.bytes.data(), LTHASH_SIZE, 16);
            std::cout << std::endl;

            if (global_lthash == expected_global_lthash) {
                std::cout << "✅ Global LtHash matches metadata" << std::endl;
            } else {
                std::cout << "❌ Global LtHash mismatch with metadata" << std::endl;
                std::cout << "Expected: ";
                print_hex(expected_global_lthash.bytes.data(), LTHASH_SIZE, 16);
                std::cout << std::endl;
            }
        }

        // Final result
        if (valid_chunks == chunk_files.size() && sorting_valid) {
            std::cout << "\n🎉 All chunks verified successfully!" << std::endl;
            std::cout << "✅ File format compliance verified" << std::endl;
            std::cout << "✅ Account sorting and uniqueness verified" << std::endl;
            std::cout << "✅ Merkle chain hashes stored (verification framework ready)" << std::endl;
            if (!quick_mode) {
                std::cout << "✅ LtHash integrity verified" << std::endl;
            }
        } else {
            std::cout << "\n❌ Some chunks failed verification" << std::endl;
        }

        return (valid_chunks == chunk_files.size() && sorting_valid) ? 0 : 1;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}