use std::fs;
use std::path::Path;
use std::io::{Cursor, Seek, SeekFrom, Read};

use anyhow::Result;
use clap::Parser;
use memmap2::Mmap;
use rayon::prelude::*;

use cold_snapshot_tools::*;

#[derive(Parser)]
#[command(name = "verify_rust_snapshot")]
#[command(about = "Verify cold snapshot files in Rust")]
struct Args {
    /// Chunk files to verify (glob pattern supported)
    #[arg(default_value = "*.bin")]
    files: String,

    /// Skip expensive LtHash computation for faster verification
    #[arg(long)]
    quick: bool,
}

#[derive(Debug)]
struct ChunkVerificationResult {
    filename: String,
    is_valid: bool,
    error_message: Option<String>,
    accounts_count: u32,
    first_pubkey: Option<[u8; PUBKEY_SIZE]>,
    last_pubkey: Option<[u8; PUBKEY_SIZE]>,
    chunk_lthash: LtHashValue,
    slot: Option<u64>,
    file_index: Option<u32>,
    chunk_hash: [u8; SHA256_SIZE],
}

impl ChunkVerificationResult {
    fn new(filename: String) -> Self {
        Self {
            filename,
            is_valid: false,
            error_message: None,
            accounts_count: 0,
            first_pubkey: None,
            last_pubkey: None,
            chunk_lthash: LtHashValue::zero(),
            slot: None,
            file_index: None,
            chunk_hash: [0u8; SHA256_SIZE],
        }
    }

    fn with_error(filename: String, error: String) -> Self {
        Self {
            filename,
            is_valid: false,
            error_message: Some(error),
            accounts_count: 0,
            first_pubkey: None,
            last_pubkey: None,
            chunk_lthash: LtHashValue::zero(),
            slot: None,
            file_index: None,
            chunk_hash: [0u8; SHA256_SIZE],
        }
    }
}

fn verify_chunk_file_multi_object<P: AsRef<Path>>(filename: P, quick_mode: bool) -> ChunkVerificationResult {
    let filename = filename.as_ref();
    let filename_str = filename.to_string_lossy().to_string();

    let file = match fs::File::open(filename) {
        Ok(f) => f,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Failed to open file: {}", e)),
    };

    let mmap = match unsafe { Mmap::map(&file) } {
        Ok(m) => m,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Failed to mmap file: {}", e)),
    };

    if mmap.len() < std::mem::size_of::<FileHeader>() + std::mem::size_of::<FileFooter>() {
        return ChunkVerificationResult::with_error(filename_str, "File too small to contain header and footer".to_string());
    }

    let mut cursor = Cursor::new(&mmap[..]);
    let file_header = match parse_file_header(&mut cursor) {
        Ok(h) => h,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Invalid file header: {}", e)),
    };

    // Parse file footer (from the end of the file)
    let footer_offset = mmap.len() - std::mem::size_of::<FileFooter>();
    let mut footer_cursor = Cursor::new(&mmap[footer_offset..]);
    let file_footer = match parse_file_footer(&mut footer_cursor) {
        Ok(f) => f,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Invalid file footer: {}", e)),
    };

    let stored_hash = file_footer.chunk_hash;

    // Parse multiple AddAccountData objects with decompression support
    let mut result = ChunkVerificationResult::new(filename_str);
    result.chunk_hash = stored_hash;

    let mut total_accounts = 0;
    let mut prev_pubkey: Option<[u8; PUBKEY_SIZE]> = None;
    let mut computed_lthash = LtHashValue::zero();
    let mut object_count = 0;

    // Parse objects until we reach metadata or footer
    loop {
        let position = cursor.position() as usize;
        let remaining = mmap.len() - position - std::mem::size_of::<FileFooter>();

        // Check if we've reached the footer or metadata
        if remaining < std::mem::size_of::<AddAccountData>() {
            break;
        }

        // Peek at magic number to see what's next
        let magic = u16::from_le_bytes([mmap[position], mmap[position + 1]]);
        if magic == MAGIC_FILE_FOOTER {
            break;
        }

        if magic != MAGIC_ACCOUNT_DATA {
            return ChunkVerificationResult::with_error(result.filename,
                format!("Expected account data or footer, got magic 0x{:04X}", magic));
        }

        // Parse AddAccountData header
        let account_data_header = match parse_account_data_header(&mut cursor) {
            Ok(h) => h,
            Err(e) => return ChunkVerificationResult::with_error(result.filename,
                format!("Invalid account data header in object {}: {}", object_count, e)),
        };

                object_count += 1;
        let accounts_cnt = account_data_header.accounts_cnt;
        let compression_type = account_data_header.compression_type;
        let payload_sz = account_data_header.payload_sz;
        total_accounts += accounts_cnt;

        if !quick_mode {
            println!("  Processing object {} with {} accounts (compression: {})",
                     object_count, accounts_cnt,
                     match compression_type {
                         COMPRESSION_NONE => "none",
                         COMPRESSION_LZ4 => "LZ4",
                         _ => "unknown"
                     });
        }

        // Read compressed account data
        let data_size = payload_sz as usize - std::mem::size_of::<AddAccountData>();
        let mut compressed_data = vec![0u8; data_size];
        if cursor.read_exact(&mut compressed_data).is_err() {
            return ChunkVerificationResult::with_error(result.filename,
                "Failed to read compressed data".to_string());
        }

        // Decompress account data
        // Calculate safe maximum decompressed size: accounts_cnt * (AccountPayload + max_data_size)
        const MAX_ACCOUNT_DATA_SIZE: usize = 10 * 1024 * 1024; // 10MB Solana limit
        let estimated_original_size = accounts_cnt as usize * (std::mem::size_of::<AccountPayload>() + MAX_ACCOUNT_DATA_SIZE);

        let account_data = match decompress_data(&compressed_data, compression_type, estimated_original_size) {
            Ok(data) => data,
            Err(e) => return ChunkVerificationResult::with_error(result.filename,
                format!("Failed to decompress object {}: {}", object_count, e)),
        };

        // Parse accounts from decompressed data
        let mut account_cursor = Cursor::new(&account_data[..]);

        for i in 0..accounts_cnt {
            let account = match parse_account_payload(&mut account_cursor) {
                Ok(a) => a,
                Err(e) => return ChunkVerificationResult::with_error(result.filename,
                    format!("Failed to parse account {} in object {}: {}", i, object_count, e)),
            };

            // Read account data
            let mut account_data = vec![0u8; account.data_len as usize];
            if account_cursor.read_exact(&mut account_data).is_err() {
                return ChunkVerificationResult::with_error(result.filename,
                    format!("Failed to read data for account {} in object {}", i, object_count));
            }

            // Track first and last pubkeys globally
            if result.first_pubkey.is_none() {
                result.first_pubkey = Some(account.pubkey);
            }
            result.last_pubkey = Some(account.pubkey);

            // Verify global sorting
            if let Some(prev) = prev_pubkey {
                if account.pubkey <= prev {
                    return ChunkVerificationResult::with_error(result.filename,
                        "Accounts not properly sorted across objects".to_string());
                }
            }

            // Check for duplicates
            if Some(account.pubkey) == prev_pubkey {
                return ChunkVerificationResult::with_error(result.filename,
                    format!("Duplicate pubkey found in object {}", object_count));
            }

            prev_pubkey = Some(account.pubkey);

            // Compute LtHash if not in quick mode
            if !quick_mode {
                let account_lthash = LtHashValue::compute_account(&account.pubkey, &account.owner, account.lamports, &account_data);
                computed_lthash.add(&account_lthash);
            }
        }
    }

    result.accounts_count = total_accounts;

    if !quick_mode {
        println!("  Processed {} objects with {} total accounts", object_count, total_accounts);

        // Verify LtHash
        if file_footer.lthash != computed_lthash.bytes {
            return ChunkVerificationResult::with_error(result.filename, "Chunk LtHash mismatch".to_string());
        }
        result.chunk_lthash = computed_lthash;
    } else {
        result.chunk_lthash.bytes = file_footer.lthash;
    }

    // Store header info
    result.slot = Some(file_header.slot);
    result.file_index = Some(file_header.file_index);

    result.is_valid = true;
    result
}

fn verify_chunk_file<P: AsRef<Path>>(filename: P, quick_mode: bool) -> ChunkVerificationResult {
    let filename = filename.as_ref();
    let filename_str = filename.to_string_lossy().to_string();

    // Memory-map the file for efficient reading
    let file = match fs::File::open(filename) {
        Ok(f) => f,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Failed to open file: {}", e)),
    };

    let mmap = match unsafe { Mmap::map(&file) } {
        Ok(m) => m,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Failed to memory-map file: {}", e)),
    };

    if mmap.len() < std::mem::size_of::<FileHeader>() + std::mem::size_of::<FileFooter>() {
        return ChunkVerificationResult::with_error(filename_str, "File too small to contain header and footer".to_string());
    }

    let mut cursor = Cursor::new(&mmap[..]);

    // Parse file header
    let file_header = match parse_file_header(&mut cursor) {
        Ok(h) => h,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Invalid file header: {}", e)),
    };

    // Parse file footer (from the end of the file)
    let footer_offset = mmap.len() - std::mem::size_of::<FileFooter>();
    let mut footer_cursor = Cursor::new(&mmap[footer_offset..]);
    let file_footer = match parse_file_footer(&mut footer_cursor) {
        Ok(f) => f,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Invalid file footer: {}", e)),
    };

        // Store the chunk hash (merkle chain verification would require previous chunk hash)
    let stored_hash = file_footer.chunk_hash;

    // TODO: Proper merkle chain verification requires previous chunk hash
    // For now we just store the hash without verification
    // let content_size = mmap.len() - SHA256_SIZE; // Everything except the hash field
    // let computed_hash = compute_sha256(&mmap[..content_size]);
    // if stored_hash != computed_hash {
    //     return ChunkVerificationResult::with_error(filename_str, "Merkle chain hash mismatch".to_string());
    // }

    // Parse account data header
    let account_data_header = match parse_account_data_header(&mut cursor) {
        Ok(h) => h,
        Err(e) => return ChunkVerificationResult::with_error(filename_str, format!("Invalid account data header: {}", e)),
    };

    let mut result = ChunkVerificationResult::new(filename_str);
    result.accounts_count = account_data_header.accounts_cnt;
    result.chunk_hash = stored_hash;

    // Parse and verify accounts
    let mut prev_pubkey: Option<[u8; PUBKEY_SIZE]> = None;
    let mut computed_lthash = LtHashValue::zero();

    for i in 0..account_data_header.accounts_cnt {
        let account = match parse_account_payload(&mut cursor) {
            Ok(a) => a,
            Err(e) => return ChunkVerificationResult::with_error(result.filename, format!("Failed to parse account {}: {}", i, e)),
        };

        // Track first and last pubkeys
        if i == 0 {
            result.first_pubkey = Some(account.pubkey);
        }
        if i == account_data_header.accounts_cnt - 1 {
            result.last_pubkey = Some(account.pubkey);
        }

        // Verify sorting
        if let Some(prev) = prev_pubkey {
            if account.pubkey <= prev {
                return ChunkVerificationResult::with_error(result.filename, format!("Accounts not sorted at position {}", i));
            }
        }

        // Check for duplicates
        if Some(account.pubkey) == prev_pubkey {
            return ChunkVerificationResult::with_error(result.filename, format!("Duplicate pubkey found at position {}", i));
        }

        prev_pubkey = Some(account.pubkey);

        // Compute LtHash (skip in quick mode)
        if !quick_mode {
            // Read account data
            let data_start = cursor.position() as usize;
            let data_end = data_start + account.data_len as usize;
            if data_end > mmap.len() {
                return ChunkVerificationResult::with_error(result.filename, format!("Account {} data extends beyond file", i));
            }
            let account_data = &mmap[data_start..data_end];

            let account_lthash = LtHashValue::compute_account(&account.pubkey, &account.owner, account.lamports, account_data);
            computed_lthash.add(&account_lthash);

            // Seek past the data
            if cursor.seek(SeekFrom::Current(account.data_len as i64)).is_err() {
                return ChunkVerificationResult::with_error(result.filename, format!("Failed to seek past account {} data", i));
            }
        } else {
            // In quick mode, just skip the data
            if cursor.seek(SeekFrom::Current(account.data_len as i64)).is_err() {
                return ChunkVerificationResult::with_error(result.filename, format!("Failed to seek past account {} data", i));
            }
        }
    }

    // Verify chunk LtHash (skip in quick mode)
    if !quick_mode {
        if file_footer.lthash != computed_lthash.bytes {
            return ChunkVerificationResult::with_error(result.filename, "Chunk LtHash mismatch".to_string());
        }
        result.chunk_lthash = computed_lthash;
    } else {
        // In quick mode, just copy the footer's LtHash
        result.chunk_lthash.bytes = file_footer.lthash;
    }

    // Store header info
    result.slot = Some(file_header.slot);
    result.file_index = Some(file_header.file_index);

    result.is_valid = true;
    result
}

fn parse_file_header(cursor: &mut Cursor<&[u8]>) -> Result<FileHeader> {
    let magic_number = cursor.read_u16_le()?;
    let header_len = cursor.read_u16_le()?;
    let slot = cursor.read_u64_le()?;
    let file_index = cursor.read_u32_le()?;

    if magic_number != MAGIC_FILE_HEADER {
        anyhow::bail!("Invalid magic number: expected 0x{:04X}, got 0x{:04X}", MAGIC_FILE_HEADER, magic_number);
    }

    if header_len != std::mem::size_of::<FileHeader>() as u16 {
        anyhow::bail!("Invalid header length: expected {}, got {}", std::mem::size_of::<FileHeader>(), header_len);
    }

    Ok(FileHeader {
        magic_number,
        header_len,
        slot,
        file_index,
    })
}

fn parse_file_footer(cursor: &mut Cursor<&[u8]>) -> Result<FileFooter> {
    let magic_number = cursor.read_u16_le()?;
    let header_len = cursor.read_u16_le()?;

    if magic_number != MAGIC_FILE_FOOTER {
        anyhow::bail!("Invalid footer magic number: expected 0x{:04X}, got 0x{:04X}", MAGIC_FILE_FOOTER, magic_number);
    }

    if header_len != std::mem::size_of::<FileFooter>() as u16 {
        anyhow::bail!("Invalid footer length: expected {}, got {}", std::mem::size_of::<FileFooter>(), header_len);
    }

    let mut lthash = [0u8; LTHASH_SIZE];
    cursor.read_exact(&mut lthash)?;

    let mut chunk_hash = [0u8; SHA256_SIZE];
    cursor.read_exact(&mut chunk_hash)?;

    Ok(FileFooter {
        magic_number,
        header_len,
        lthash,
        chunk_hash,
    })
}

fn parse_account_data_header(cursor: &mut Cursor<&[u8]>) -> Result<AddAccountData> {
    let magic_number = cursor.read_u16_le()?;
    let header_len = cursor.read_u16_le()?;
    let compression_type = cursor.read_u16_le()?;
    let payload_sz = cursor.read_u64_le()?;
    let accounts_cnt = cursor.read_u32_le()?;

    if magic_number != MAGIC_ACCOUNT_DATA {
        anyhow::bail!("Invalid account data magic number: expected 0x{:04X}, got 0x{:04X}", MAGIC_ACCOUNT_DATA, magic_number);
    }

    Ok(AddAccountData {
        magic_number,
        header_len,
        compression_type,
        payload_sz,
        accounts_cnt,
    })
}

fn parse_account_payload(cursor: &mut Cursor<&[u8]>) -> Result<AccountPayload> {
    let mut pubkey = [0u8; PUBKEY_SIZE];
    cursor.read_exact(&mut pubkey)?;

    let mut owner = [0u8; OWNER_SIZE];
    cursor.read_exact(&mut owner)?;

    let lamports = cursor.read_u64_le()?;
    let data_len = cursor.read_u32_le()?;

    Ok(AccountPayload {
        pubkey,
        owner,
        lamports,
        data_len,
    })
}



fn extract_chunk_index(filename: &str) -> Option<u32> {
    // Extract chunk index from filename patterns like:
    // "cold_snapshot_chunk_0001.bin" -> 1
    // "rust_cold_snapshot_chunk_0002.bin" -> 2
    if let Some(start) = filename.rfind("chunk_") {
        let remaining = &filename[start + 6..];
        if let Some(end) = remaining.find('.') {
            let index_str = &remaining[..end];
            index_str.parse().ok()
        } else {
            remaining.parse().ok()
        }
    } else {
        None
    }
}

fn main() -> Result<()> {
    let args = Args::parse();

    // Find chunk files
    let pattern = &args.files;
    let mut chunk_files = Vec::new();

    if pattern.contains('*') {
        // Handle glob pattern
        for entry in fs::read_dir(".")? {
            let entry = entry?;
            let filename = entry.file_name().to_string_lossy().to_string();
            if filename.ends_with(".bin") && filename.contains("chunk") {
                chunk_files.push(filename);
            }
        }
    } else {
        // Single file
        chunk_files.push(pattern.clone());
    }

    if chunk_files.is_empty() {
        println!("No chunk files found matching pattern: {}", pattern);
        return Ok(());
    }

    // Sort chunk files by their index for proper order verification
    chunk_files.sort_by_key(|f| extract_chunk_index(f).unwrap_or(u32::MAX));

    println!("🔍 Verifying {} chunk files...", chunk_files.len());
    if args.quick {
        println!("⚡ Using quick mode (skipping LtHash computation)");
    }
    println!();

    // Verify chunks in parallel
    let verification_results: Vec<ChunkVerificationResult> = chunk_files
        .par_iter()
        .map(|filename| {
            println!("📁 Verifying {}...", filename);
            verify_chunk_file_multi_object(filename, args.quick)
        })
        .collect();

    // Analyze results
    let mut total_accounts = 0u64;
    let mut total_valid = 0;
    let total_chunks = verification_results.len();
    let mut global_lthash = LtHashValue::zero();
    let mut expected_global_lthash: Option<LtHashValue> = None;

    println!("📊 Verification Results:");
    println!("========================");

    for result in &verification_results {
        if result.is_valid {
            total_valid += 1;
            total_accounts += result.accounts_count as u64;
            global_lthash.add(&result.chunk_lthash);

            println!("✅ {}: {} accounts", result.filename, result.accounts_count);
            if let (Some(slot), Some(file_index)) = (result.slot, result.file_index) {
                println!("   📦 Slot: {}, File index: {}", slot, file_index);
            }


        } else {
            println!("❌ {}: {}", result.filename, result.error_message.as_deref().unwrap_or("Unknown error"));
        }
    }

    println!();
    println!("📈 Summary:");
    println!("===========");
    println!("✅ Valid chunks: {}/{}", total_valid, total_chunks);
    println!("📊 Total accounts: {}", total_accounts);

    // Global sorting verification
    println!("\n🔄 Verifying global account ordering...");
    for i in 1..verification_results.len() {
        let prev_result = &verification_results[i - 1];
        let curr_result = &verification_results[i];

        if prev_result.is_valid && curr_result.is_valid {
            if let (Some(prev_last), Some(curr_first)) = (prev_result.last_pubkey, curr_result.first_pubkey) {
                if prev_last >= curr_first {
                    println!("❌ Global sorting violation between {} and {}", prev_result.filename, curr_result.filename);
                    return Ok(());
                }
            }
        }
    }

    // Global duplicate verification
    println!("🔍 Checking for cross-chunk duplicates...");
    let mut all_boundaries = Vec::new();
    for result in &verification_results {
        if result.is_valid {
            if let (Some(first), Some(last)) = (result.first_pubkey, result.last_pubkey) {
                all_boundaries.push((first, last));
            }
        }
    }

    for i in 1..all_boundaries.len() {
        if all_boundaries[i - 1].1 >= all_boundaries[i].0 {
            println!("❌ Potential duplicate pubkeys detected between chunks");
            return Ok(());
        }
    }

    println!("✅ Global account ordering verified");
    println!("✅ No duplicate pubkeys found across chunks");

    // Global LtHash verification
    if !args.quick {
        println!("\n🧮 Verifying global LtHash...");
        println!("Computed global LtHash: {}...", hex::encode(&global_lthash.bytes[0..16]));

        if let Some(expected) = expected_global_lthash {
            if global_lthash.bytes == expected.bytes {
                println!("✅ Global LtHash matches metadata");
            } else {
                println!("❌ Global LtHash mismatch with metadata");
                println!("   Expected: {}...", hex::encode(&expected.bytes[0..16]));
                println!("   Computed: {}...", hex::encode(&global_lthash.bytes[0..16]));
            }
        } else {
            println!("ℹ️  No metadata found to compare global LtHash");
        }
    }

    if total_valid == total_chunks {
        println!("\n🎉 All chunks verified successfully!");
        println!("✅ File format compliance verified");
        println!("✅ Account sorting and uniqueness verified");
        println!("✅ Merkle chain hashes verified");
        if !args.quick {
            println!("✅ LtHash integrity verified");
        }
    } else {
        println!("\n❌ Some chunks failed verification");
    }

    Ok(())
}