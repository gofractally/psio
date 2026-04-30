//! xxh64 hash function -- byte-identical to the C++ psio constexpr implementation.
//!
//! Used for field-name hashing in dynamic schema lookups.
//!
//! The implementation mirrors `psio1::xxh64` from `capnp_view.hpp` exactly,
//! producing identical output for all inputs. All functions are `const fn`
//! (the Rust equivalent of C++ constexpr).

const PRIME1: u64 = 0x9E3779B185EBCA87;
const PRIME2: u64 = 0xC2B2AE3D27D4EB4F;
const PRIME3: u64 = 0x165667B19E3779F9;
const PRIME4: u64 = 0x85EBCA77C2B2AE63;
const PRIME5: u64 = 0x27D4EB2F165667C5;

/// Compute the xxh64 hash of a byte slice with a given seed (default 0).
///
/// Produces output identical to `psio1::xxh64::hash(input, len, seed)` in C++.
pub const fn hash(input: &[u8], seed: u64) -> u64 {
    let len = input.len();
    if len >= 32 {
        let acc = h32bytes(input, 0, len, seed);
        let consumed = len & !31;
        finalize(acc, input, consumed, len - consumed)
    } else {
        finalize(
            seed.wrapping_add(PRIME5).wrapping_add(len as u64),
            input,
            0,
            len,
        )
    }
}

/// Convenience: hash a string (as bytes) with seed 0.
pub const fn hash_str(s: &str) -> u64 {
    hash(s.as_bytes(), 0)
}

const fn rotl(x: u64, r: u32) -> u64 {
    (x << r) | (x >> (64 - r))
}

const fn round(acc: u64, input: u64) -> u64 {
    rotl(acc.wrapping_add(input.wrapping_mul(PRIME2)), 31).wrapping_mul(PRIME1)
}

const fn merge_round(acc: u64, val: u64) -> u64 {
    (acc ^ round(0, val)).wrapping_mul(PRIME1).wrapping_add(PRIME4)
}

const fn read64(p: &[u8], off: usize) -> u64 {
    (p[off] as u64)
        | ((p[off + 1] as u64) << 8)
        | ((p[off + 2] as u64) << 16)
        | ((p[off + 3] as u64) << 24)
        | ((p[off + 4] as u64) << 32)
        | ((p[off + 5] as u64) << 40)
        | ((p[off + 6] as u64) << 48)
        | ((p[off + 7] as u64) << 56)
}

const fn read32(p: &[u8], off: usize) -> u32 {
    (p[off] as u32)
        | ((p[off + 1] as u32) << 8)
        | ((p[off + 2] as u32) << 16)
        | ((p[off + 3] as u32) << 24)
}

const fn avalanche(mut h: u64) -> u64 {
    h ^= h >> 33;
    h = h.wrapping_mul(PRIME2);
    h ^= h >> 29;
    h = h.wrapping_mul(PRIME3);
    h ^= h >> 32;
    h
}

/// Iterative finalize -- processes remaining bytes after the 32-byte-block loop.
const fn finalize(mut h: u64, p: &[u8], mut off: usize, mut len: usize) -> u64 {
    // Process 8-byte chunks
    while len >= 8 {
        h = rotl(h ^ round(0, read64(p, off)), 27)
            .wrapping_mul(PRIME1)
            .wrapping_add(PRIME4);
        off += 8;
        len -= 8;
    }
    // Process 4-byte chunk
    if len >= 4 {
        h = rotl(
            h ^ (read32(p, off) as u64).wrapping_mul(PRIME1),
            23,
        )
        .wrapping_mul(PRIME2)
        .wrapping_add(PRIME3);
        off += 4;
        len -= 4;
    }
    // Process remaining bytes one at a time
    while len > 0 {
        h = rotl(h ^ (p[off] as u64).wrapping_mul(PRIME5), 11).wrapping_mul(PRIME1);
        off += 1;
        len -= 1;
    }
    avalanche(h)
}

/// Process full 32-byte blocks, returning the merged accumulator + remaining length.
///
/// Mirrors the C++ recursive `h32bytes`: the length parameter is decremented by 32
/// each iteration, and the *remaining* length (after all blocks) is added to the
/// merged accumulator. The caller (`hash`) passes the total input length, which
/// gets whittled down to `total_len % 32`.
const fn h32bytes(p: &[u8], mut off: usize, mut len: usize, seed: u64) -> u64 {
    let mut v1 = seed.wrapping_add(PRIME1).wrapping_add(PRIME2);
    let mut v2 = seed.wrapping_add(PRIME2);
    let mut v3 = seed;
    let mut v4 = seed.wrapping_sub(PRIME1);

    while len >= 32 {
        v1 = round(v1, read64(p, off));
        v2 = round(v2, read64(p, off + 8));
        v3 = round(v3, read64(p, off + 16));
        v4 = round(v4, read64(p, off + 24));
        off += 32;
        len -= 32;
    }

    let acc = rotl(v1, 1)
        .wrapping_add(rotl(v2, 7))
        .wrapping_add(rotl(v3, 12))
        .wrapping_add(rotl(v4, 18));

    // Add `len` (the remainder after block processing), matching the C++ base case.
    merge_round(merge_round(merge_round(merge_round(acc, v1), v2), v3), v4)
        .wrapping_add(len as u64)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Test vectors generated from the C++ psio1::xxh64 implementation.
    #[test]
    fn known_vectors_seed_0() {
        let cases: &[(&str, u64)] = &[
            ("", 0xef46db3751d8e999),
            ("a", 0xd24ec4f1a98c6e5b),
            ("abc", 0x44bc2cf5ad770999),
            ("name", 0x6c5f7b2e4a79fd6d),
            ("hello", 0x26c7827d889f6da3),
            ("balance", 0x47887cdfc8c8a122),
            ("username", 0x0400e5dc95ec9594),
            ("field_name", 0x1f9992f4280b53cf),
            ("Hello, World!", 0xc49aacf8080fe47f),
            // Exactly 32 bytes -- exercises the h32bytes path
            ("abcdefghijklmnopqrstuvwxyz012345", 0xd87beb27fb6b2023),
            // 42 bytes -- h32bytes + 10-byte finalize remainder
            (
                "abcdefghijklmnopqrstuvwxyz0123456789ABCDEF",
                0xf61e973fb752589c,
            ),
        ];

        for &(input, expected) in cases {
            let got = hash(input.as_bytes(), 0);
            assert_eq!(
                got, expected,
                "xxh64 mismatch for {:?}: got 0x{:016x}, expected 0x{:016x}",
                input, got, expected
            );
        }
    }

    #[test]
    fn known_vector_with_seed() {
        let got = hash(b"hello", 42);
        assert_eq!(got, 0xc3629e6318d53932);
    }

    #[test]
    fn hash_str_matches_hash() {
        let s = "field_name";
        assert_eq!(hash_str(s), hash(s.as_bytes(), 0));
    }

    // Verify const-evaluation works (compile-time test).
    const _CONST_CHECK: u64 = hash_str("hello");

    #[test]
    fn const_eval() {
        assert_eq!(_CONST_CHECK, 0x26c7827d889f6da3);
    }
}
