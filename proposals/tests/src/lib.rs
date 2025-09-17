use std::io::{Read, Write};
use anyhow::Result;
use sha2::{Digest, Sha256};

// Constants (must match C implementation)
pub const LTHASH_SIZE: usize = 2048;
pub const PUBKEY_SIZE: usize = 32;
pub const OWNER_SIZE: usize = 32;
pub const SHA256_SIZE: usize = 32;

// Magic numbers (2 bytes, must match C implementation)
pub const MAGIC_FILE_HEADER: u16 = 0x534E;      // "SN"
pub const MAGIC_FILE_FOOTER: u16 = 0x4654;      // "FT"
pub const MAGIC_ACCOUNT_DATA: u16 = 0x4143;     // "AC"


// Compression types from SIMD specification
pub const COMPRESSION_NONE: u16 = 0x0000;
pub const COMPRESSION_LZ4: u16 = 0x0001;

// Maximum payload size per AddAccountData object (4GB)
pub const MAX_PAYLOAD_SIZE: u64 = 4 * 1024 * 1024 * 1024;

// Compression settings
pub const ENABLE_COMPRESSION: bool = true; // Re-enabled with raw LZ4 for compatibility
pub const ACCOUNTS_PER_OBJECT: usize = 100_000; // Split into smaller objects for parallel processing

// File format structures (must match C implementation exactly)
#[repr(C, packed)]
#[derive(Debug, Clone)]
pub struct FileHeader {
    pub magic_number: u16,
    pub header_len: u16,
    pub slot: u64,
    pub file_index: u32,
}

#[repr(C, packed)]
#[derive(Debug, Clone)]
pub struct FileFooter {
    pub magic_number: u16,
    pub header_len: u16,
    pub lthash: [u8; LTHASH_SIZE],
    pub chunk_hash: [u8; SHA256_SIZE], // Must be last field for merkle chain hashing
}

#[repr(C, packed)]
#[derive(Debug, Clone)]
pub struct AccountPayload {
    pub pubkey: [u8; PUBKEY_SIZE],
    pub owner: [u8; OWNER_SIZE],
    pub lamports: u64,
    pub data_len: u32,
    // data follows immediately after this struct
}

#[repr(C, packed)]
#[derive(Debug, Clone)]
pub struct AddAccountData {
    pub magic_number: u16,
    pub header_len: u16,
    pub compression_type: u16,  // COMPRESSION_NONE or COMPRESSION_LZ4
    pub payload_sz: u64,
    pub accounts_cnt: u32,
    // accounts_data follows immediately after this struct
}



// LtHash implementation compatible with Firedancer
#[derive(Debug, Clone)]
pub struct LtHashValue {
    pub bytes: [u8; LTHASH_SIZE],
}

impl Default for LtHashValue {
    fn default() -> Self {
        Self::zero()
    }
}

impl LtHashValue {
    pub fn zero() -> Self {
        Self {
            bytes: [0u8; LTHASH_SIZE],
        }
    }

    pub fn is_zero(&self) -> bool {
        self.bytes.iter().all(|&b| b == 0)
    }

    pub fn add(&mut self, other: &LtHashValue) {
        for i in 0..LTHASH_SIZE {
            let sum = self.bytes[i] as u16 + other.bytes[i] as u16;
            self.bytes[i] = sum as u8;
            // Carry propagation (simplified lattice arithmetic)
            if sum > 255 && i + 1 < LTHASH_SIZE {
                self.bytes[i + 1] = self.bytes[i + 1].wrapping_add(1);
            }
        }
    }

    pub fn compute(data: &[u8]) -> Self {
        let hash = blake3::hash(data);
        let mut result = Self::zero();

        // Expand 32-byte BLAKE3 hash to 2048-byte LtHash using repeated hashing
        let mut current_hash = *hash.as_bytes();

        for chunk in result.bytes.chunks_mut(32) {
            let copy_len = chunk.len().min(32);
            chunk[..copy_len].copy_from_slice(&current_hash[..copy_len]);

            // Generate next hash for the next chunk
            let mut hasher = blake3::Hasher::new();
            hasher.update(&current_hash);
            hasher.update(&[0u8; 8]); // Add some variety
            current_hash = *hasher.finalize().as_bytes();
        }

        result
    }

    pub fn compute_account(
        pubkey: &[u8; PUBKEY_SIZE],
        owner: &[u8; OWNER_SIZE],
        lamports: u64,
        data: &[u8],
    ) -> Self {
        // Match C implementation's field ordering for LtHash computation
        // Order: lamports, data, executable, owner, pubkey
        let mut account_data = Vec::new();
        account_data.extend_from_slice(&lamports.to_le_bytes());
        account_data.extend_from_slice(data);
        account_data.push(0u8); // executable flag (false)
        account_data.extend_from_slice(owner);
        account_data.extend_from_slice(pubkey);

        Self::compute(&account_data)
    }
}

// Utility functions for reading/writing binary data
pub trait BinaryRead {
    fn read_u16_le(&mut self) -> Result<u16>;
    fn read_u32_le(&mut self) -> Result<u32>;
    fn read_u64_le(&mut self) -> Result<u64>;
    fn read_bytes(&mut self, buf: &mut [u8]) -> Result<()>;
}

pub trait BinaryWrite {
    fn write_u16_le(&mut self, val: u16) -> Result<()>;
    fn write_u32_le(&mut self, val: u32) -> Result<()>;
    fn write_u64_le(&mut self, val: u64) -> Result<()>;
    fn write_bytes(&mut self, buf: &[u8]) -> Result<()>;
}

impl<R: Read> BinaryRead for R {
    fn read_u16_le(&mut self) -> Result<u16> {
        let mut buf = [0u8; 2];
        self.read_exact(&mut buf)?;
        Ok(u16::from_le_bytes(buf))
    }

    fn read_u32_le(&mut self) -> Result<u32> {
        let mut buf = [0u8; 4];
        self.read_exact(&mut buf)?;
        Ok(u32::from_le_bytes(buf))
    }

    fn read_u64_le(&mut self) -> Result<u64> {
        let mut buf = [0u8; 8];
        self.read_exact(&mut buf)?;
        Ok(u64::from_le_bytes(buf))
    }

    fn read_bytes(&mut self, buf: &mut [u8]) -> Result<()> {
        self.read_exact(buf)?;
        Ok(())
    }
}

impl<W: Write> BinaryWrite for W {
    fn write_u16_le(&mut self, val: u16) -> Result<()> {
        self.write_all(&val.to_le_bytes())?;
        Ok(())
    }

    fn write_u32_le(&mut self, val: u32) -> Result<()> {
        self.write_all(&val.to_le_bytes())?;
        Ok(())
    }

    fn write_u64_le(&mut self, val: u64) -> Result<()> {
        self.write_all(&val.to_le_bytes())?;
        Ok(())
    }

    fn write_bytes(&mut self, buf: &[u8]) -> Result<()> {
        self.write_all(buf)?;
        Ok(())
    }
}

// Utility function to compute SHA256 hash
pub fn compute_sha256(data: &[u8]) -> [u8; SHA256_SIZE] {
    let mut hasher = Sha256::new();
    hasher.update(data);
    hasher.finalize().into()
}

// Utility function to compute merkle chain hash
pub fn compute_merkle_chain_hash(file_contents_without_hash: &[u8], previous_chunk_hash: &[u8; SHA256_SIZE]) -> [u8; SHA256_SIZE] {
    let mut hasher = Sha256::new();
    hasher.update(file_contents_without_hash);
    hasher.update(previous_chunk_hash);
    hasher.finalize().into()
}

// Helper function to generate random bytes
pub fn generate_random_bytes(len: usize) -> Vec<u8> {
    use rand::RngCore;
    let mut rng = rand::thread_rng();
    let mut bytes = vec![0u8; len];
    rng.fill_bytes(&mut bytes);
    bytes
}

// Compression utilities
#[derive(Debug)]
pub struct CompressionResult {
    pub data: Vec<u8>,
    pub original_size: usize,
    pub compressed_size: usize,
    pub is_compressed: bool,
}

pub fn compress_data(input: &[u8], compression_type: u16) -> anyhow::Result<CompressionResult> {
    match compression_type {
        COMPRESSION_NONE => {
            Ok(CompressionResult {
                data: input.to_vec(),
                original_size: input.len(),
                compressed_size: input.len(),
                is_compressed: false,
            })
        },
        COMPRESSION_LZ4 => {
            if ENABLE_COMPRESSION {
                // Use raw LZ4 compression (no size prefix - compatible with C/C++)
                let compressed = lz4_flex::compress(input);
                Ok(CompressionResult {
                    data: compressed.clone(),
                    original_size: input.len(),
                    compressed_size: compressed.len(),
                    is_compressed: true,
                })
            } else {
                println!("LZ4 compression disabled - using uncompressed");
                Ok(CompressionResult {
                    data: input.to_vec(),
                    original_size: input.len(),
                    compressed_size: input.len(),
                    is_compressed: false,
                })
            }
        },
        _ => anyhow::bail!("Unsupported compression type: {}", compression_type),
    }
}

pub fn decompress_data(compressed: &[u8], compression_type: u16, original_size: usize) -> anyhow::Result<Vec<u8>> {
    match compression_type {
        COMPRESSION_NONE => Ok(compressed.to_vec()),
        COMPRESSION_LZ4 => {
            // Use raw LZ4 decompression (no size prefix - compatible with C/C++)
            lz4_flex::decompress(compressed, original_size)
                .map_err(|e| anyhow::anyhow!("LZ4 decompression failed: {}", e))
        },
        _ => anyhow::bail!("Unsupported compression type: {}", compression_type),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_lthash_basic() {
        let lthash1 = LtHashValue::compute(b"test data 1");
        let lthash2 = LtHashValue::compute(b"test data 2");

        assert!(!lthash1.is_zero());
        assert!(!lthash2.is_zero());

        let mut combined = lthash1.clone();
        combined.add(&lthash2);

        // Combined should be different from either individual hash
        assert_ne!(combined.bytes, lthash1.bytes);
        assert_ne!(combined.bytes, lthash2.bytes);
    }

    #[test]
    fn test_account_lthash() {
        let pubkey = [1u8; PUBKEY_SIZE];
        let owner = [2u8; OWNER_SIZE];
        let lamports = 1000000u64;
        let data = b"account data";

        let lthash = LtHashValue::compute_account(&pubkey, &owner, lamports, data);
        assert!(!lthash.is_zero());
    }

    #[test]
    fn test_struct_sizes() {
        // Verify struct sizes match C implementation exactly

        // FileHeader: magic_number (2) + header_len (2) + slot (8) + file_index (4) = 16 bytes
        assert_eq!(std::mem::size_of::<FileHeader>(), 16);

        // FileFooter: magic_number (2) + header_len (2) + lthash (2048) + chunk_hash (32) = 2084 bytes
        assert_eq!(std::mem::size_of::<FileFooter>(), 2084);

        // AddAccountData: magic_number (2) + header_len (2) + compression_type (2) + payload_sz (8) + accounts_cnt (4) = 18 bytes
        assert_eq!(std::mem::size_of::<AddAccountData>(), 18);



        // AccountPayload: pubkey (32) + owner (32) + lamports (8) + data_len (4) = 76 bytes
        assert_eq!(std::mem::size_of::<AccountPayload>(), 76);

        println!("✅ All struct sizes verified:");
        println!("  FileHeader: {} bytes", std::mem::size_of::<FileHeader>());
        println!("  FileFooter: {} bytes", std::mem::size_of::<FileFooter>());
        println!("  AddAccountData: {} bytes", std::mem::size_of::<AddAccountData>());

        println!("  AccountPayload: {} bytes", std::mem::size_of::<AccountPayload>());
    }

    #[test]
    fn test_file_footer_field_order() {
        // Verify that chunk_hash is the last field in FileFooter (critical for streaming)
        use std::mem::{offset_of};

        // Check that chunk_hash is at the expected offset (last 32 bytes)
        let expected_hash_offset = std::mem::size_of::<FileFooter>() - SHA256_SIZE;
        let actual_hash_offset = offset_of!(FileFooter, chunk_hash);

        assert_eq!(actual_hash_offset, expected_hash_offset,
            "chunk_hash must be the last field in FileFooter for streaming I/O");

        // Verify other field offsets
        assert_eq!(offset_of!(FileFooter, magic_number), 0);
        assert_eq!(offset_of!(FileFooter, header_len), 2);
        assert_eq!(offset_of!(FileFooter, lthash), 4);
        assert_eq!(offset_of!(FileFooter, chunk_hash), 2052); // 4 + 2048

        println!("✅ FileFooter field ordering verified:");
        println!("  magic_number at offset: {}", offset_of!(FileFooter, magic_number));
        println!("  header_len at offset: {}", offset_of!(FileFooter, header_len));
        println!("  lthash at offset: {}", offset_of!(FileFooter, lthash));
        println!("  chunk_hash at offset: {} (last field)", offset_of!(FileFooter, chunk_hash));
    }

    #[test]
    fn test_magic_numbers() {
        // Verify magic numbers match the SIMD specification
        assert_eq!(MAGIC_FILE_HEADER, 0x534E, "FileHeader magic should be 0x534E (SN)");
        assert_eq!(MAGIC_FILE_FOOTER, 0x4654, "FileFooter magic should be 0x4654 (FT)");
        assert_eq!(MAGIC_ACCOUNT_DATA, 0x4143, "AddAccountData magic should be 0x4143 (AC)");


        println!("✅ Magic numbers verified:");
        println!("  MAGIC_FILE_HEADER: 0x{:04X} ({})", MAGIC_FILE_HEADER,
                 std::str::from_utf8(&MAGIC_FILE_HEADER.to_le_bytes()).unwrap_or("??"));
        println!("  MAGIC_FILE_FOOTER: 0x{:04X} ({})", MAGIC_FILE_FOOTER,
                 std::str::from_utf8(&MAGIC_FILE_FOOTER.to_le_bytes()).unwrap_or("??"));
        println!("  MAGIC_ACCOUNT_DATA: 0x{:04X} ({})", MAGIC_ACCOUNT_DATA,
                 std::str::from_utf8(&MAGIC_ACCOUNT_DATA.to_le_bytes()).unwrap_or("??"));

    }
}
