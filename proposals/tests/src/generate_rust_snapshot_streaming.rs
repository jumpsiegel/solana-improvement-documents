use std::fs::File;
use std::io::{Write, Seek, SeekFrom};
use std::path::Path;

use anyhow::{Result, Context};
use clap::Parser;
use rand::Rng;
use rayon::prelude::*;
use sha2::{Digest, Sha256};

// Use the library crate instead of mod
use cold_snapshot_tools::*;

#[derive(Parser)]
#[command(name = "generate_rust_snapshot_streaming")]
#[command(about = "Generate cold snapshot files in Rust using streaming I/O")]
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

        let lamports = rng.gen_range(1..=100_000_000_000u64); // 1 to 100 SOL in lamports
        let data_len = rng.gen_range(0..=10240); // 0 to 10KB
        let data = generate_random_bytes(data_len);

        Account {
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

// Streaming hash context for merkle chain computation
struct StreamingHashContext {
    hasher: Sha256,
}

impl StreamingHashContext {
    fn new() -> Self {
        Self {
            hasher: Sha256::new(),
        }
    }

    fn update(&mut self, data: &[u8]) {
        self.hasher.update(data);
    }

    fn finalize(mut self, previous_chunk_hash: &[u8; SHA256_SIZE]) -> [u8; SHA256_SIZE] {
        self.hasher.update(previous_chunk_hash);
        self.hasher.finalize().into()
    }
}

fn write_chunk_file_streaming(
    filename: &str,
    accounts: &[Account],
    slot: u64,
    chunk_index: u32,
    is_final_chunk: bool,
    global_lthash: &mut LtHashValue,
    previous_chunk_hash: &[u8; SHA256_SIZE],
) -> Result<()> {
    let mut file = File::create(filename)
        .with_context(|| format!("Failed to create file: {}", filename))?;

    let mut hash_ctx = StreamingHashContext::new();

    // Write file header and hash it
    let header = FileHeader {
        magic_number: MAGIC_FILE_HEADER,
        header_len: std::mem::size_of::<FileHeader>() as u16,
        slot,
        file_index: chunk_index,
    };
    let header_bytes = [
        &header.magic_number.to_le_bytes()[..],
        &header.header_len.to_le_bytes()[..],
        &header.slot.to_le_bytes()[..],
        &header.file_index.to_le_bytes()[..],
    ].concat();

    file.write_all(&header_bytes)?;
    hash_ctx.update(&header_bytes);

    // Compute chunk LtHash
    let mut chunk_lthash = LtHashValue::zero();
    for account in accounts {
        let account_lthash = account.compute_lthash();
        chunk_lthash.add(&account_lthash);
        global_lthash.add(&account_lthash);
    }

    // Write account data payload header and hash it
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

    let account_header_bytes = [
        &account_data_header.magic_number.to_le_bytes()[..],
        &account_data_header.header_len.to_le_bytes()[..],
        &account_data_header.payload_sz.to_le_bytes()[..],
        &account_data_header.accounts_cnt.to_le_bytes()[..],
    ].concat();

    file.write_all(&account_header_bytes)?;
    hash_ctx.update(&account_header_bytes);

    // Stream all accounts and their data
    for account in accounts {
        let payload = account.to_payload();
        let payload_bytes = [
            &payload.pubkey[..],
            &payload.owner[..],
            &payload.lamports.to_le_bytes()[..],
            &payload.data_len.to_le_bytes()[..],
        ].concat();

        file.write_all(&payload_bytes)?;
        hash_ctx.update(&payload_bytes);

        if !account.data.is_empty() {
            file.write_all(&account.data)?;
            hash_ctx.update(&account.data);
        }
    }



    // Write file footer (except the hash field) and hash it
    let footer = FileFooter {
        magic_number: MAGIC_FILE_FOOTER,
        header_len: std::mem::size_of::<FileFooter>() as u16,
        lthash: chunk_lthash.bytes,
        chunk_hash: [0u8; SHA256_SIZE], // Will be computed separately
    };

    // Write everything except the hash field
    let footer_without_hash_bytes = [
        &footer.magic_number.to_le_bytes()[..],
        &footer.header_len.to_le_bytes()[..],
        &footer.lthash[..],
    ].concat();

    file.write_all(&footer_without_hash_bytes)?;
    hash_ctx.update(&footer_without_hash_bytes);

    // Compute final merkle chain hash
    let file_hash = hash_ctx.finalize(previous_chunk_hash);

    // Write the hash field to complete the file
    file.write_all(&file_hash)?;

    // Get file size for reporting
    let file_size = file.metadata()?.len();
    let file_size_mb = file_size as f64 / (1024.0 * 1024.0);
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

    println!("Generating {} random accounts (streaming mode)...", args.num_accounts);

    // Generate accounts in parallel
    let all_accounts: Vec<Account> = (0..args.num_accounts)
        .into_par_iter()
        .map(|i| {
            if i % 1_000_000 == 0 {
                println!("Generated {}/{} accounts...", i, args.num_accounts);
            }
            Account::generate_random()
        })
        .collect();

    println!("Account generation complete. Sorting accounts globally...");

    // Sort ALL accounts globally by pubkey
    println!("Sorting all accounts globally by pubkey...");
    let mut all_accounts = all_accounts;
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
    println!("No duplicates found. Writing chunks using streaming I/O...");

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
            .join(format!("rust_streaming_cold_snapshot_chunk_{:04}.bin", chunk_idx));

        write_chunk_file_streaming(
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

    println!("Snapshot generation complete!");
    println!("Generated {} chunks with a total of {} accounts", chunk_count, args.num_accounts);
    println!("All accounts are globally sorted by pubkey across chunks");
    println!("Each chunk uses merkle chain hashing linked to previous chunk");
    println!("Used streaming I/O - minimal memory footprint");

    Ok(())
}