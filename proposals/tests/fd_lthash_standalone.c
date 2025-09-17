#define _GNU_SOURCE
#include "fd_lthash_standalone.h"
#include <string.h>

// Define endian conversion functions if not available
#if defined(__GLIBC__)
#include <endian.h>
#elif defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define htole32(x) OSSwapHostToLittleInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#else
// Fallback implementation for other systems
#include <stdint.h>
static inline uint32_t htole32(uint32_t host_32bits) {
    union { uint32_t i; uint8_t c[4]; } u = { .i = 0x01020304 };
    if (u.c[0] == 0x04) return host_32bits; // little endian
    return ((host_32bits & 0xFF000000) >> 24) |
           ((host_32bits & 0x00FF0000) >> 8)  |
           ((host_32bits & 0x0000FF00) << 8)  |
           ((host_32bits & 0x000000FF) << 24);
}
static inline uint32_t le32toh(uint32_t little_endian_32bits) {
    return htole32(little_endian_32bits); // same operation
}
#endif

// Simplified Blake3 implementation
// Based on the reference implementation but adapted for our standalone use

// Blake3 IV
static const uint32_t IV[8] = {
  0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
  0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL,
};

// Blake3 constants
static const uint32_t MSG_PERMUTATION[16] = {
  2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

// Blake3 flags
#define CHUNK_START (1 << 0)
#define CHUNK_END   (1 << 1)
#define PARENT      (1 << 2)
#define ROOT        (1 << 3)
#define KEYED_HASH  (1 << 4)
#define DERIVE_KEY_CONTEXT (1 << 5)
#define DERIVE_KEY_MATERIAL (1 << 6)

static inline uint32_t rotr32(uint32_t w, unsigned c) {
  return (w >> c) | (w << (32 - c));
}

static void g(uint32_t *state, size_t a, size_t b, size_t c, size_t d,
              uint32_t x, uint32_t y) {
  state[a] = state[a] + state[b] + x;
  state[d] = rotr32(state[d] ^ state[a], 16);
  state[c] = state[c] + state[d];
  state[b] = rotr32(state[b] ^ state[c], 12);
  state[a] = state[a] + state[b] + y;
  state[d] = rotr32(state[d] ^ state[a], 8);
  state[c] = state[c] + state[d];
  state[b] = rotr32(state[b] ^ state[c], 7);
}

static void round_fn(uint32_t state[16], const uint32_t *msg, size_t round) {
  // Select the message word selection permutation for this round.
  const uint32_t *schedule = MSG_PERMUTATION;
  if (round > 0) {
    for (size_t i = 0; i < round; i++) {
      for (size_t j = 0; j < 16; j++) {
        schedule = &MSG_PERMUTATION[schedule[j]];
      }
    }
  }

  g(state, 0, 4, 8, 12, msg[0], msg[1]);
  g(state, 1, 5, 9, 13, msg[2], msg[3]);
  g(state, 2, 6, 10, 14, msg[4], msg[5]);
  g(state, 3, 7, 11, 15, msg[6], msg[7]);
  g(state, 0, 5, 10, 15, msg[8], msg[9]);
  g(state, 1, 6, 11, 12, msg[10], msg[11]);
  g(state, 2, 7, 8, 13, msg[12], msg[13]);
  g(state, 3, 4, 9, 14, msg[14], msg[15]);
}

static void permute(uint32_t *msg) {
  uint32_t permuted[16];
  for (size_t i = 0; i < 16; i++) {
    permuted[i] = msg[MSG_PERMUTATION[i]];
  }
  memcpy(msg, permuted, sizeof(permuted));
}

static void compress(const uint32_t chaining_value[8],
                    const uint8_t block[64],
                    uint8_t block_len,
                    uint64_t counter,
                    uint8_t flags,
                    uint32_t out[16]) {
  uint32_t state[16] = {
    chaining_value[0],
    chaining_value[1],
    chaining_value[2],
    chaining_value[3],
    chaining_value[4],
    chaining_value[5],
    chaining_value[6],
    chaining_value[7],
    IV[0],
    IV[1],
    IV[2],
    IV[3],
    (uint32_t)counter,
    (uint32_t)(counter >> 32),
    (uint32_t)block_len,
    (uint32_t)flags,
  };

  uint32_t block_words[16];
  for (size_t i = 0; i < 16; i++) {
    block_words[i] = le32toh(((uint32_t*)block)[i]);
  }

  for (size_t round = 0; round < 7; round++) {
    round_fn(state, block_words, round);
    permute(block_words);
  }

  for (size_t i = 0; i < 8; i++) {
    out[i] = state[i] ^ state[i + 8];
  }
  for (size_t i = 8; i < 16; i++) {
    out[i] = state[i] ^ chaining_value[i - 8];
  }
}

// Simple Blake3 implementation for our needs
void fd_blake3_init(fd_blake3_t * blake) {
  memcpy(blake->h, IV, sizeof(IV));
  memset(blake->buf, 0, sizeof(blake->buf));
  blake->buflen = 0;
  blake->counter = 0;
  blake->flags = 0;
}

void fd_blake3_append(fd_blake3_t * blake, void const * data, ulong sz) {
  const uint8_t *input = (const uint8_t *)data;
  size_t input_len = sz;

  while (input_len > 0) {
    if (blake->buflen == BLAKE3_BLOCK_LEN) {
      // Process full block
      uint32_t out[16];
      uint8_t flags = blake->flags;
      if (blake->counter == 0) flags |= CHUNK_START;

      compress(blake->h, blake->buf, BLAKE3_BLOCK_LEN, blake->counter, flags, out);
      memcpy(blake->h, out, 8 * sizeof(uint32_t));
      blake->counter++;
      blake->buflen = 0;
    }

    size_t take = BLAKE3_BLOCK_LEN - blake->buflen;
    if (take > input_len) take = input_len;

    memcpy(blake->buf + blake->buflen, input, take);
    blake->buflen += take;
    input += take;
    input_len -= take;
  }
}

void fd_blake3_fini_2048(fd_blake3_t * blake, void * out) {
  // Process final block
  uint32_t final_out[16];
  uint8_t flags = blake->flags | CHUNK_END | ROOT;
  if (blake->counter == 0) flags |= CHUNK_START;

  compress(blake->h, blake->buf, blake->buflen, blake->counter, flags, final_out);

  // Generate 2048 bytes of output using XOF mode
  uint8_t *output = (uint8_t *)out;
  for (size_t i = 0; i < FD_LTHASH_LEN_BYTES / 64; i++) {
    uint32_t block_out[16];
    compress(blake->h, blake->buf, blake->buflen, i, flags, block_out);

    // Convert to little-endian bytes
    for (size_t j = 0; j < 16; j++) {
      uint32_t word = htole32(block_out[j]);
      memcpy(output + i * 64 + j * 4, &word, 4);
    }
  }
}

// LtHash computation functions
void fd_lthash_compute(const void* data, size_t len, fd_lthash_value_t* out) {
  fd_blake3_t blake;
  fd_blake3_init(&blake);
  fd_blake3_append(&blake, data, len);
  fd_blake3_fini_2048(&blake, out->bytes);
}

// Solana account LtHash matching Firedancer's format exactly
void fd_lthash_compute_account(
    const void * pubkey,      // 32 bytes
    const void * owner,       // 32 bytes
    uint64_t lamports,        // 8 bytes
    const void * data,        // variable length
    size_t data_len,
    bool executable,          // 1 byte
    fd_lthash_value_t * out
) {
  fd_blake3_t blake;
  fd_blake3_init(&blake);

  // Match Firedancer's exact serialization order:
  // 1. lamports (8 bytes)
  // 2. data (variable length)
  // 3. executable flag (1 byte)
  // 4. owner (32 bytes)
  // 5. pubkey (32 bytes)

  fd_blake3_append(&blake, &lamports, sizeof(uint64_t));
  if (data_len > 0) {
    fd_blake3_append(&blake, data, data_len);
  }

  uint8_t footer[65];
  footer[0] = executable ? 1 : 0;
  memcpy(footer + 1, owner, 32);
  memcpy(footer + 33, pubkey, 32);
  fd_blake3_append(&blake, footer, sizeof(footer));

  fd_blake3_fini_2048(&blake, out->bytes);
}