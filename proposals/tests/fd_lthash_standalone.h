#ifndef FD_LTHASH_STANDALONE_H
#define FD_LTHASH_STANDALONE_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

// Basic types (simplified from Firedancer)
typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned long  ulong;
typedef unsigned int   uint;

// LtHash constants
#define FD_LTHASH_ALIGN       (64UL)
#define FD_LTHASH_LEN_BYTES (2048UL)
#define FD_LTHASH_LEN_ELEMS (1024UL)

// Blake3 constants we need
#define FD_BLAKE3_HASH_SZ (32UL)

// LtHash value structure
union __attribute__((aligned(FD_LTHASH_ALIGN))) fd_lthash_value {
  uchar  bytes[FD_LTHASH_LEN_BYTES];
  ushort words[FD_LTHASH_LEN_ELEMS];
};
typedef union fd_lthash_value fd_lthash_value_t;

// Blake3 hasher structure (simplified)
typedef struct fd_blake3 {
  uint h[8];           // state
  uchar buf[64];       // buffer
  uint buflen;         // buffer length
  uint counter;        // counter
  uint flags;          // flags
} fd_blake3_t;

// Blake3 constants
#define BLAKE3_OUT_LEN 32
#define BLAKE3_KEY_LEN 32
#define BLAKE3_BLOCK_LEN 64
#define BLAKE3_CHUNK_LEN 1024

// Blake3 implementation (minimal for our needs)
void blake3_hasher_init(fd_blake3_t *self);
void blake3_hasher_update(fd_blake3_t *self, const void *input, size_t input_len);
void blake3_hasher_finalize(const fd_blake3_t *self, uint8_t *out, size_t out_len);

// LtHash functions
static inline fd_lthash_value_t *
fd_lthash_zero(fd_lthash_value_t * r) {
  memset(r->bytes, 0, FD_LTHASH_LEN_BYTES);
  return r;
}

static inline int
fd_lthash_is_zero(fd_lthash_value_t const * r) {
  for (ulong i = 0; i < FD_LTHASH_LEN_ELEMS; i++) {
    if (r->words[i] != 0) {
      return 0; // not zero
    }
  }
  return 1;
}

static inline fd_lthash_value_t *
fd_lthash_add(fd_lthash_value_t *r, const fd_lthash_value_t *a) {
  for (ulong i = 0; i < FD_LTHASH_LEN_ELEMS; i++) {
    r->words[i] = (ushort)(r->words[i] + a->words[i]);
  }
  return r;
}

static inline fd_lthash_value_t *
fd_lthash_sub(fd_lthash_value_t *r, const fd_lthash_value_t *a) {
  for (ulong i = 0; i < FD_LTHASH_LEN_ELEMS; i++) {
    r->words[i] = (ushort)(r->words[i] - a->words[i]);
  }
  return r;
}

// Helper functions
void fd_blake3_init(fd_blake3_t * blake);
void fd_blake3_append(fd_blake3_t * blake, void const * data, ulong sz);
void fd_blake3_fini_2048(fd_blake3_t * blake, void * out);

// Compute LtHash for data
void fd_lthash_compute(const void* data, size_t len, fd_lthash_value_t* out);

// Solana account LtHash (matches Firedancer's format)
void fd_lthash_compute_account(
    const void * pubkey,      // 32 bytes
    const void * owner,       // 32 bytes
    uint64_t lamports,        // 8 bytes
    const void * data,        // variable length
    size_t data_len,
    bool executable,          // 1 byte
    fd_lthash_value_t * out
);

#endif /* FD_LTHASH_STANDALONE_H */