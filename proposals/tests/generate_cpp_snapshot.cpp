#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <openssl/sha.h>
#include <lz4.h>

// Constants from SIMD specification
constexpr size_t LTHASH_SIZE = 2048;
constexpr size_t PUBKEY_SIZE = 32;
constexpr size_t OWNER_SIZE = 32;
constexpr size_t SHA256_SIZE = 32;
constexpr uint32_t NUM_ACCOUNTS = 5000;
constexpr uint32_t ACCOUNTS_PER_CHUNK = 5000;

// Magic numbers from SIMD specification (Table in section "Magic Numbers")
constexpr uint16_t MAGIC_FILE_HEADER = 0x534E;      // "SN" (Snapshot)
constexpr uint16_t MAGIC_FILE_FOOTER = 0x4654;      // "FT" (Footer)
constexpr uint16_t MAGIC_ACCOUNT_DATA = 0x4143;     // "AC" (Account)


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



#pragma pack(pop)

// Account structure for generation
struct Account {
    std::array<uint8_t, PUBKEY_SIZE> pubkey;
    std::array<uint8_t, OWNER_SIZE> owner;
    uint64_t lamports;
    std::vector<uint8_t> data;

    // Comparison for sorting by pubkey
    bool operator<(const Account& other) const {
        return pubkey < other.pubkey;
    }
};

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
};

// Generate random account data
Account generate_random_account() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    static std::uniform_int_distribution<uint32_t> data_len_dist(0, 4096);
    static std::uniform_int_distribution<uint64_t> lamports_dist(0, 1000000000);

    Account account;

    // Generate random pubkey
    for (auto& byte : account.pubkey) {
        byte = byte_dist(gen);
    }

    // Generate random owner
    for (auto& byte : account.owner) {
        byte = byte_dist(gen);
    }

    // Generate random lamports
    account.lamports = lamports_dist(gen);

    // Generate random data
    uint32_t data_len = data_len_dist(gen);
    account.data.resize(data_len);
    for (auto& byte : account.data) {
        byte = byte_dist(gen);
    }

    return account;
}

// Compute SHA256 hash
std::array<uint8_t, SHA256_SIZE> compute_sha256(const uint8_t* data, size_t len) {
    std::array<uint8_t, SHA256_SIZE> result;
    SHA256(data, len, result.data());
    return result;
}

// Compute merkle chain hash according to SIMD protocol
std::array<uint8_t, SHA256_SIZE> compute_merkle_chain_hash(
    const std::vector<uint8_t>& file_contents_without_hash,
    const std::array<uint8_t, SHA256_SIZE>& previous_chunk_hash) {

    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, file_contents_without_hash.data(), file_contents_without_hash.size());
    SHA256_Update(&ctx, previous_chunk_hash.data(), SHA256_SIZE);

    std::array<uint8_t, SHA256_SIZE> result;
    SHA256_Final(result.data(), &ctx);
    return result;
}

// Write chunk file according to SIMD specification
void write_chunk_file(const std::string& filename,
                     const std::vector<Account>& accounts,
                     uint64_t slot,
                     uint32_t file_index,
                     bool is_final_chunk,
                     LtHash& global_lthash,
                     const std::array<uint8_t, SHA256_SIZE>& previous_chunk_hash) {

    std::vector<uint8_t> file_buffer;

    // 1. Write FileHeader
    FileHeader header;
    header.magic_number = MAGIC_FILE_HEADER;
    header.header_len = sizeof(FileHeader);
    header.slot = slot;
    header.file_index = file_index;

    file_buffer.insert(file_buffer.end(),
                      reinterpret_cast<uint8_t*>(&header),
                      reinterpret_cast<uint8_t*>(&header) + sizeof(header));

    // 2. Compute chunk LtHash
    LtHash chunk_lthash;
    for (const auto& account : accounts) {
        auto account_lthash = LtHash::compute_account(
            account.pubkey.data(), account.owner.data(),
            account.lamports, account.data.data(), account.data.size());
        chunk_lthash.add(account_lthash);
        global_lthash.add(account_lthash);
    }

    // 3. Serialize account data to buffer
    std::vector<uint8_t> uncompressed_data;
    for (const auto& account : accounts) {
        AccountPayload payload;
        std::memcpy(payload.pubkey, account.pubkey.data(), PUBKEY_SIZE);
        std::memcpy(payload.owner, account.owner.data(), OWNER_SIZE);
        payload.lamports = account.lamports;
        payload.data_len = static_cast<uint32_t>(account.data.size());

        uncompressed_data.insert(uncompressed_data.end(),
                               reinterpret_cast<uint8_t*>(&payload),
                               reinterpret_cast<uint8_t*>(&payload) + sizeof(payload));
        uncompressed_data.insert(uncompressed_data.end(), account.data.begin(), account.data.end());
    }

    // 4. Compress data using LZ4
    std::vector<uint8_t> compressed_data;
    uint16_t compression_type = COMPRESSION_LZ4;

    int max_compressed_size = LZ4_compressBound(static_cast<int>(uncompressed_data.size()));
    compressed_data.resize(max_compressed_size);

    int compressed_size = LZ4_compress_default(
        reinterpret_cast<const char*>(uncompressed_data.data()),
        reinterpret_cast<char*>(compressed_data.data()),
        static_cast<int>(uncompressed_data.size()),
        max_compressed_size);

    std::cout << "DEBUG: Uncompressed size: " << uncompressed_data.size()
              << ", Max compressed size: " << max_compressed_size
              << ", Actual compressed size: " << compressed_size << std::endl;

    if (compressed_size <= 0) {
        // Compression failed, use uncompressed data
        std::cout << "DEBUG: LZ4 compression FAILED, falling back to uncompressed" << std::endl;
        compression_type = COMPRESSION_NONE;
        compressed_data = uncompressed_data;
    } else {
        std::cout << "DEBUG: LZ4 compression SUCCESS, "
                  << (100.0 * compressed_size / uncompressed_data.size()) << "% of original size" << std::endl;
        compressed_data.resize(compressed_size);
    }

    // 5. Write AddAccountData header
    AddAccountData account_data_header;
    account_data_header.magic_number = MAGIC_ACCOUNT_DATA;
    account_data_header.header_len = sizeof(AddAccountData);
    account_data_header.compression_type = compression_type;
    account_data_header.accounts_cnt = static_cast<uint32_t>(accounts.size());
    account_data_header.payload_sz = sizeof(AddAccountData) + compressed_data.size();

    file_buffer.insert(file_buffer.end(),
                      reinterpret_cast<uint8_t*>(&account_data_header),
                      reinterpret_cast<uint8_t*>(&account_data_header) + sizeof(account_data_header));

    // 6. Write compressed account data
    file_buffer.insert(file_buffer.end(), compressed_data.begin(), compressed_data.end());

    // 7. Metadata removed from SIMD specification

    // 8. Prepare FileFooter with zeroed chunk_hash
    FileFooter footer;
    footer.magic_number = MAGIC_FILE_FOOTER;
    footer.header_len = sizeof(FileFooter);
    std::memcpy(footer.lthash, chunk_lthash.bytes.data(), LTHASH_SIZE);
    std::memset(footer.chunk_hash, 0, SHA256_SIZE);  // Zero for hash computation

    file_buffer.insert(file_buffer.end(),
                      reinterpret_cast<uint8_t*>(&footer),
                      reinterpret_cast<uint8_t*>(&footer) + sizeof(footer));

    // 7. Compute merkle chain hash (excluding the zeroed hash field)
    size_t content_size = file_buffer.size() - SHA256_SIZE;
    std::vector<uint8_t> content_for_hash(file_buffer.begin(), file_buffer.begin() + content_size);
    auto computed_hash = compute_merkle_chain_hash(content_for_hash, previous_chunk_hash);

    // 8. Update chunk_hash in buffer
    std::memcpy(&file_buffer[file_buffer.size() - SHA256_SIZE], computed_hash.data(), SHA256_SIZE);

    // 9. Write to file
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to create file: " + filename);
    }

    file.write(reinterpret_cast<const char*>(file_buffer.data()), file_buffer.size());
    if (!file) {
        throw std::runtime_error("Failed to write to file: " + filename);
    }

    double file_size_mb = static_cast<double>(file_buffer.size()) / (1024.0 * 1024.0);
    std::cout << "Written chunk " << filename << " with " << accounts.size()
              << " accounts (" << std::fixed << std::setprecision(2) << file_size_mb << " MB)" << std::endl;
}

int main() {
    try {
        std::cout << "Generating " << NUM_ACCOUNTS << " random accounts (C++)..." << std::endl;

        // Generate random accounts
        std::vector<Account> all_accounts;
        all_accounts.reserve(NUM_ACCOUNTS);

        for (uint32_t i = 0; i < NUM_ACCOUNTS; ++i) {
            all_accounts.push_back(generate_random_account());

            if ((i + 1) % 100000 == 0) {
                std::cout << "Generated " << (i + 1) << "/" << NUM_ACCOUNTS << " accounts..." << std::endl;
            }
        }

        std::cout << "Account generation complete. Sorting accounts globally..." << std::endl;

        // Global sorting by pubkey (as per SIMD Organization Rules)
        std::sort(all_accounts.begin(), all_accounts.end());

        std::cout << "Global sorting complete. Checking for duplicates..." << std::endl;

        // Check for duplicates (as per SIMD Organization Rules)
        for (size_t i = 1; i < all_accounts.size(); ++i) {
            if (all_accounts[i-1].pubkey == all_accounts[i].pubkey) {
                throw std::runtime_error("Duplicate pubkey found! This should be extremely rare.");
            }
        }

        std::cout << "No duplicates found. Writing chunks..." << std::endl;

        // Calculate number of chunks
        uint32_t chunk_count = (NUM_ACCOUNTS + ACCOUNTS_PER_CHUNK - 1) / ACCOUNTS_PER_CHUNK;
        std::cout << "Will generate " << chunk_count << " chunks with up to "
                  << ACCOUNTS_PER_CHUNK << " accounts each" << std::endl;

        LtHash global_lthash;
        std::array<uint8_t, SHA256_SIZE> previous_chunk_hash;
        previous_chunk_hash.fill(0);  // First chunk uses zeros per SIMD spec

        // Write chunks
        for (uint32_t chunk_idx = 0; chunk_idx < chunk_count; ++chunk_idx) {
            uint32_t start_idx = chunk_idx * ACCOUNTS_PER_CHUNK;
            uint32_t end_idx = std::min(start_idx + ACCOUNTS_PER_CHUNK, NUM_ACCOUNTS);
            bool is_final = (chunk_idx == chunk_count - 1);

            std::vector<Account> chunk_accounts(all_accounts.begin() + start_idx,
                                              all_accounts.begin() + end_idx);

            std::ostringstream filename;
            filename << "cpp_cold_snapshot_chunk_" << std::setfill('0') << std::setw(4) << chunk_idx << ".bin";

            write_chunk_file(filename.str(), chunk_accounts, 12345678, chunk_idx, is_final,
                           global_lthash, previous_chunk_hash);

            // Read back the chunk hash for next iteration
            if (chunk_idx < chunk_count - 1) {
                std::ifstream file(filename.str(), std::ios::binary | std::ios::ate);
                if (file) {
                    auto file_size = file.tellg();
                    file.seekg(static_cast<std::streamoff>(file_size) - SHA256_SIZE);
                    file.read(reinterpret_cast<char*>(previous_chunk_hash.data()), SHA256_SIZE);
                }
            }

            std::cout << "Written chunk " << chunk_idx << " with accounts "
                      << start_idx << " to " << (end_idx - 1) << std::endl;
        }

        std::cout << "\nSnapshot generation complete!" << std::endl;
        std::cout << "Generated " << chunk_count << " chunks with a total of "
                  << NUM_ACCOUNTS << " accounts" << std::endl;
        std::cout << "All accounts are globally sorted by pubkey across chunks" << std::endl;
        std::cout << "Each chunk uses merkle chain hashing linked to previous chunk" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}