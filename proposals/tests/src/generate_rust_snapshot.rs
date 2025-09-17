use std::fs::File;
use std::io::Write;
use std::path::Path;

use anyhow::{Result, Context};
use clap::Parser;
use rand::Rng;
use rayon::prelude::*;

// Use the library crate instead of mod
use cold_snapshot_tools::*;

#[derive(Parser)]
#[command(name = "generate_rust_snapshot")]
#[command(about = "Generate cold snapshot files in Rust")]
struct Args {
    /// Number of accounts to generate
    #[arg(long, default_value = "1000000")]
    num_accounts: u32,

    /// Accounts per chunk
    #[arg(long, default_value = "100000")]
    accounts_per_chunk: u32,

    /// Snapshot slot number
    #[arg(long, default_value = "12345678")]
    slot: u64,

    /// Output directory for chunk files
    #[arg(long, default_value = ".")]
    output_dir: String,
}

#[derive(Debug, Clone)]
struct Account {
    pubkey: [u8; PUBKEY_SIZE],
    owner: [u8; OWNER_SIZE],
    lamports: u64,
    data: Vec<u8>,
}

impl Account {
    fn generate_random() -> Self {
        let mut rng = rand::thread_rng();

        let mut pubkey = [0u8; PUBKEY_SIZE];
        let mut owner = [0u8; OWNER_SIZE];
        rng.fill(&mut pubkey);
        rng.fill(&mut owner);

        let lamports = rng.gen_range(1_000_000..100_000_000);
        let data_len = rng.gen_range(0..10240); // 0 to 10KB
        let data = generate_random_bytes(data_len);

        Self {
            pubkey,
            owner,
            lamports,
            data,
        }
    }

    fn to_payload(&self) -> AccountPayload {
        AccountPayload {
            pubkey: self.pubkey,
            owner: self.owner,
            lamports: self.lamports,
            data_len: self.data.len() as u32,
        }
    }

    fn compute_lthash(&self) -> LtHashValue {
        LtHashValue::compute_account(&self.pubkey, &self.owner, self.lamports, &self.data)
    }

    fn total_serialized_size(&self) -> usize {
        std::mem::size_of::<AccountPayload>() + self.data.len()
    }
}

fn write_chunk_file_multi_object(
    filename: &str,
    accounts: &[Account],
    slot: u64,
    chunk_index: u32,
    is_final_chunk: bool,
    global_lthash: &mut LtHashValue,
    previous_chunk_hash: &[u8; SHA256_SIZE],
) -> Result<()> {
    println!("Writing chunk with multi-object segmentation ({} accounts)...", accounts.len());

    let mut file_buffer = Vec::new();

    // Write file header
    let header = FileHeader {
        magic_number: MAGIC_FILE_HEADER,
        header_len: std::mem::size_of::<FileHeader>() as u16,
        slot,
        file_index: chunk_index,
    };
    file_buffer.write_u16_le(header.magic_number)?;
    file_buffer.write_u16_le(header.header_len)?;
    file_buffer.write_u64_le(header.slot)?;
    file_buffer.write_u32_le(header.file_index)?;

    // Segment accounts into multiple objects
    let mut objects_written = 0;
    let mut accounts_processed = 0;

    while accounts_processed < accounts.len() {
        let accounts_in_this_object = std::cmp::min(
            ACCOUNTS_PER_OBJECT,
            accounts.len() - accounts_processed
        );

        let object_accounts = &accounts[accounts_processed..accounts_processed + accounts_in_this_object];

        // Serialize accounts for this object
        let mut object_data = Vec::new();
        for account in object_accounts {
            let account_payload = AccountPayload {
                pubkey: account.pubkey,
                owner: account.owner,
                lamports: account.lamports,
                data_len: account.data.len() as u32,
            };

            // Write account payload
            object_data.write_all(&account_payload.pubkey)?;
            object_data.write_all(&account_payload.owner)?;
            object_data.write_u64_le(account_payload.lamports)?;
            object_data.write_u32_le(account_payload.data_len)?;

            // Write account data
            object_data.write_all(&account.data)?;
        }

        // Compress the object data
        let compression_type = if ENABLE_COMPRESSION { COMPRESSION_LZ4 } else { COMPRESSION_NONE };
        let compressed = compress_data(&object_data, compression_type)?;

        // Check 4GB limit
        if compressed.compressed_size as u64 > MAX_PAYLOAD_SIZE {
            anyhow::bail!("Compressed object exceeds 4GB limit: {} bytes", compressed.compressed_size);
        }

        // Write AddAccountData header
        let object_header = AddAccountData {
            magic_number: MAGIC_ACCOUNT_DATA,
            header_len: std::mem::size_of::<AddAccountData>() as u16,
            compression_type,
            payload_sz: (std::mem::size_of::<AddAccountData>() + compressed.compressed_size) as u64,
            accounts_cnt: accounts_in_this_object as u32,
        };

        file_buffer.write_u16_le(object_header.magic_number)?;
        file_buffer.write_u16_le(object_header.header_len)?;
        file_buffer.write_u16_le(object_header.compression_type)?;
        file_buffer.write_u64_le(object_header.payload_sz)?;
        file_buffer.write_u32_le(object_header.accounts_cnt)?;

        // Write compressed account data
        file_buffer.write_all(&compressed.data)?;

        println!("  Object {}: {} accounts, {} bytes -> {} bytes (compression: {})",
                 objects_written + 1, accounts_in_this_object, compressed.original_size,
                 compressed.compressed_size, if compressed.is_compressed { "LZ4" } else { "none" });

        // Update chunk LtHash
        for account in object_accounts {
            let account_lthash = account.compute_lthash();
            global_lthash.add(&account_lthash);
        }

        accounts_processed += accounts_in_this_object;
        objects_written += 1;
    }



    // Write file footer (placeholder - hash will be computed later)
    let footer = FileFooter {
        magic_number: MAGIC_FILE_FOOTER,
        header_len: std::mem::size_of::<FileFooter>() as u16,
        lthash: [0u8; LTHASH_SIZE], // TODO: Compute actual chunk LtHash
        chunk_hash: [0u8; SHA256_SIZE], // Will be computed and updated
    };

    file_buffer.write_u16_le(footer.magic_number)?;
    file_buffer.write_u16_le(footer.header_len)?;
    file_buffer.write_all(&footer.lthash)?;
    file_buffer.write_all(&footer.chunk_hash)?;

    // Write to file
    let mut file = File::create(filename)?;
    file.write_all(&file_buffer)?;

    println!("Wrote {} objects to chunk file", objects_written);
    Ok(())
}

fn write_chunk_file(
    filename: &str,
    accounts: &[Account],
    slot: u64,
    chunk_index: u32,
    is_final_chunk: bool,
    global_lthash: &mut LtHashValue,
    previous_chunk_hash: &[u8; SHA256_SIZE],
) -> Result<()> {
    let mut file_buffer = Vec::new();

    // Write file header
    let header = FileHeader {
        magic_number: MAGIC_FILE_HEADER,
        header_len: std::mem::size_of::<FileHeader>() as u16,
        slot,
        file_index: chunk_index,
    };
    file_buffer.write_u16_le(header.magic_number)?;
    file_buffer.write_u16_le(header.header_len)?;
    file_buffer.write_u64_le(header.slot)?;
    file_buffer.write_u32_le(header.file_index)?;

    // Compute chunk LtHash
    let mut chunk_lthash = LtHashValue::zero();
    for account in accounts {
        let account_lthash = account.compute_lthash();
        chunk_lthash.add(&account_lthash);
        global_lthash.add(&account_lthash);
    }

    // Write account data payload header
    let total_data_size: usize = accounts.iter()
        .map(|acc| acc.total_serialized_size())
        .sum();

    let account_data_header = AddAccountData {
        magic_number: MAGIC_ACCOUNT_DATA,
        header_len: std::mem::size_of::<AddAccountData>() as u16,
        compression_type: COMPRESSION_NONE, // No compression for now
        payload_sz: (std::mem::size_of::<AddAccountData>() + total_data_size) as u64,
        accounts_cnt: accounts.len() as u32,
    };

    // Write account data header
    file_buffer.write_u16_le(account_data_header.magic_number)?;
    file_buffer.write_u16_le(account_data_header.header_len)?;
    file_buffer.write_u64_le(account_data_header.payload_sz)?;
    file_buffer.write_u32_le(account_data_header.accounts_cnt)?;

    // Write all accounts
    for account in accounts {
        let payload = account.to_payload();

        // Write account payload
        file_buffer.write_bytes(&payload.pubkey)?;
        file_buffer.write_bytes(&payload.owner)?;
        file_buffer.write_u64_le(payload.lamports)?;
        file_buffer.write_u32_le(payload.data_len)?;

        // Write account data
        file_buffer.write_bytes(&account.data)?;
    }



    // Write file footer (with zero hash initially)
    let footer = FileFooter {
        magic_number: MAGIC_FILE_FOOTER,
        header_len: std::mem::size_of::<FileFooter>() as u16,
        lthash: chunk_lthash.bytes,
        chunk_hash: [0u8; SHA256_SIZE], // Will be filled in after hash computation
    };

    file_buffer.write_u16_le(footer.magic_number)?;
    file_buffer.write_u16_le(footer.header_len)?;
    file_buffer.write_bytes(&footer.lthash)?;
    file_buffer.write_bytes(&footer.chunk_hash)?; // Zeros for now

    // Compute merkle chain hash of entire file content (excluding the hash field)
    let content_size = file_buffer.len() - SHA256_SIZE;
    let file_hash = compute_merkle_chain_hash(&file_buffer[..content_size], previous_chunk_hash);

    // Update the hash field at the end of the buffer
    let hash_offset = file_buffer.len() - SHA256_SIZE;
    file_buffer[hash_offset..].copy_from_slice(&file_hash);

    // Write file to disk
    let mut file = File::create(filename)
        .with_context(|| format!("Failed to create file: {}", filename))?;
    file.write_all(&file_buffer)
        .with_context(|| format!("Failed to write to file: {}", filename))?;

    let file_size_mb = file_buffer.len() as f64 / (1024.0 * 1024.0);
    println!(
        "Written chunk {} with {} accounts ({:.2} MB)",
        filename,
        accounts.len(),
        file_size_mb
    );

    Ok(())
}

fn main() -> Result<()> {
    let args = Args::parse();

    println!("Generating cold snapshot with {} accounts...", args.num_accounts);

    // Generate all accounts first
    println!("Generating {} accounts...", args.num_accounts);
    let mut all_accounts = Vec::with_capacity(args.num_accounts as usize);

    // Use parallel generation for better performance
    let accounts_batch_size = 100_000;
    let num_batches = (args.num_accounts + accounts_batch_size - 1) / accounts_batch_size;

    for batch in 0..num_batches {
        let batch_start = batch * accounts_batch_size;
        let batch_size = std::cmp::min(accounts_batch_size, args.num_accounts - batch_start);

        let batch_accounts: Vec<Account> = (0..batch_size)
            .into_par_iter()
            .map(|_| Account::generate_random())
            .collect();

        all_accounts.extend(batch_accounts);

        if (batch + 1) % 10 == 0 || batch == num_batches - 1 {
            println!("Generated {} accounts...", all_accounts.len());
        }
    }

    // Sort ALL accounts globally by pubkey
    println!("Sorting all accounts globally by pubkey...");
    all_accounts.sort_by(|a, b| a.pubkey.cmp(&b.pubkey));

    // Check for duplicates (should be extremely rare)
    println!("Checking for duplicates...");
    for i in 1..all_accounts.len() {
        if all_accounts[i - 1].pubkey == all_accounts[i].pubkey {
            anyhow::bail!(
                "Duplicate pubkey found at positions {} and {}. This should be extremely rare!",
                i - 1,
                i
            );
        }
    }
    println!("No duplicates found. Writing chunks...");

    // Write accounts to chunks in globally sorted order
    let chunk_count = (args.num_accounts + args.accounts_per_chunk - 1) / args.accounts_per_chunk;
    println!(
        "Will generate {} chunks with up to {} accounts each",
        chunk_count, args.accounts_per_chunk
    );

    let mut global_lthash = LtHashValue::zero();
    let mut previous_chunk_hash = [0u8; SHA256_SIZE]; // First chunk uses all zeros

    for chunk_idx in 0..chunk_count {
        let start_idx = (chunk_idx * args.accounts_per_chunk) as usize;
        let end_idx = std::cmp::min(
            ((chunk_idx + 1) * args.accounts_per_chunk) as usize,
            all_accounts.len(),
        );

        let chunk_accounts = &all_accounts[start_idx..end_idx];
        let is_final = chunk_idx == chunk_count - 1;

        let filename = Path::new(&args.output_dir)
            .join(format!("rust_cold_snapshot_chunk_{:04}.bin", chunk_idx));

        write_chunk_file_multi_object(
            filename.to_str().unwrap(),
            chunk_accounts,
            args.slot,
            chunk_idx as u32,
            is_final,
            &mut global_lthash,
            &previous_chunk_hash,
        )?;

        // Read back the chunk hash for the next iteration
        if chunk_idx < chunk_count - 1 {
            let file_data = std::fs::read(&filename)?;
            if file_data.len() >= SHA256_SIZE {
                let hash_offset = file_data.len() - SHA256_SIZE;
                previous_chunk_hash.copy_from_slice(&file_data[hash_offset..]);
            }
        }

        println!(
            "Written chunk {} with accounts {} to {}",
            chunk_idx,
            start_idx,
            end_idx - 1
        );
    }

    println!("\nSnapshot generation complete!");
    println!("Generated {} chunk files with {} total accounts", chunk_count, args.num_accounts);
    println!("Accounts are globally sorted across all chunks");
    println!("Final global LtHash: {}...", hex::encode(&global_lthash.bytes[0..32]));

    Ok(())
}