//! Value types shared by the binary formats: `Uint256`, the bit
//! containers and the bounded list and string. pSSZ re-exports them as
//! `psio::pssz::{Uint256, Bitvector, Bitlist, BoundedList, BoundedString,
//! BoundedBytes}`; with the `ssz` feature they are also `psio::ssz::...`.

/// A value has more elements or bytes than its bound allows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BoundError(pub &'static str);

impl std::fmt::Display for BoundError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}
impl std::error::Error for BoundError {}

// ── Uint256 (C++ psio1::uint256) — 32-byte LE, 4 × u64 limbs ────────────────
// Wire layout: limb[0] (lsb 64 bits) LE, then limb[1], [2], [3] (msb).

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Uint256 {
    /// `limb[0]` = least-significant 64 bits; `limb[3]` = most-significant.
    pub limb: [u64; 4],
}

impl Uint256 {
    pub const ZERO: Uint256 = Uint256 { limb: [0; 4] };
    pub const fn from_u64(v: u64) -> Self { Uint256 { limb: [v, 0, 0, 0] } }
}

// ── Bitvector<N> — C++ psio1::bitvector<N> ──────────────────────────────────
// Packed bits, LSB-first, (N + 7) / 8 bytes, no delimiter.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bitvector<const N: usize> {
    pub bytes: Vec<u8>,  // length = (N + 7) / 8
}

impl<const N: usize> Default for Bitvector<N> {
    fn default() -> Self {
        Bitvector { bytes: vec![0u8; (N + 7) / 8] }
    }
}

impl<const N: usize> Bitvector<N> {
    pub fn new() -> Self { Self::default() }
    pub const BYTE_COUNT: usize = (N + 7) / 8;
    pub const SIZE: usize = N;

    pub fn test(&self, i: usize) -> bool {
        (self.bytes[i >> 3] >> (i & 7)) & 1 != 0
    }
    pub fn set(&mut self, i: usize, v: bool) {
        if v { self.bytes[i >> 3] |= 1u8 << (i & 7); }
        else { self.bytes[i >> 3] &= !(1u8 << (i & 7)); }
    }
}

// ── Bitlist<N> — C++ psio1::bitlist<N> ──────────────────────────────────────
// Packed bits + a single delimiter bit at position = len (always ≥ 1 byte).

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Bitlist<const MAX_N: usize> {
    pub bits: Vec<bool>,  // logical length — not what gets encoded directly
}

impl<const MAX_N: usize> Bitlist<MAX_N> {
    pub fn new() -> Self { Bitlist { bits: Vec::new() } }
    pub fn with_len(n: usize) -> Self { Bitlist { bits: vec![false; n] } }
    pub fn len(&self) -> usize { self.bits.len() }
    pub fn test(&self, i: usize) -> bool { self.bits[i] }
    pub fn set(&mut self, i: usize, v: bool) { self.bits[i] = v; }
    pub fn push(&mut self, v: bool) { self.bits.push(v); }
}

// ── BoundedList<T, N> / BoundedString<N> — C++ bounded_list / bounded_string
//
// Wire format is byte-identical to `Vec<T>` / `String` — the bound is a
// Rust-side schema invariant, enforced on decode.

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct BoundedList<T, const N: usize>(pub Vec<T>);

impl<T, const N: usize> BoundedList<T, N> {
    pub fn new() -> Self { BoundedList(Vec::new()) }
    pub fn try_from_vec(v: Vec<T>) -> Result<Self, BoundError> {
        if v.len() > N { return Err(BoundError("BoundedList overflow")); }
        Ok(BoundedList(v))
    }
    pub fn inner(&self) -> &Vec<T> { &self.0 }
    pub fn into_inner(self) -> Vec<T> { self.0 }
    pub fn len(&self) -> usize { self.0.len() }
    pub fn is_empty(&self) -> bool { self.0.is_empty() }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct BoundedString<const N: usize>(pub String);

impl<const N: usize> BoundedString<N> {
    pub fn new() -> Self { BoundedString(String::new()) }
    pub fn try_from_string(s: String) -> Result<Self, BoundError> {
        if s.len() > N { return Err(BoundError("BoundedString overflow")); }
        Ok(BoundedString(s))
    }
    pub fn as_str(&self) -> &str { &self.0 }
    pub fn into_inner(self) -> String { self.0 }
    pub fn len(&self) -> usize { self.0.len() }
}

// `BoundedBytes<N>` — convenience alias.
pub type BoundedBytes<const N: usize> = BoundedList<u8, N>;

impl<const MAX_N: usize> Bitlist<MAX_N> {
    /// The packed bits followed by the delimiter bit at position `len`
    /// (the SSZ and pSSZ wire form).
    pub(crate) fn pack_bits(&self, out: &mut Vec<u8>) {
        let n = self.bits.len();
        let total_bytes = (n / 8) + 1;
        let start = out.len();
        out.resize(start + total_bytes, 0);
        for (i, &b) in self.bits.iter().enumerate() {
            if b { out[start + (i >> 3)] |= 1u8 << (i & 7); }
        }
        // delimiter bit
        out[start + (n >> 3)] |= 1u8 << (n & 7);
    }
    /// The inverse of [`pack_bits`](Self::pack_bits).
    pub(crate) fn unpack_bits(bytes: &[u8]) -> Result<Self, &'static str> {
        if bytes.is_empty() { return Err("ssz: bitlist empty span"); }
        // Find the last non-zero byte → contains the delimiter.
        let mut last = bytes.len() as isize - 1;
        while last >= 0 && bytes[last as usize] == 0 { last -= 1; }
        if last < 0 { return Err("ssz: bitlist missing delimiter"); }
        let last_byte = bytes[last as usize];
        // Highest set bit position within last_byte.
        let hi = 7 - last_byte.leading_zeros() as usize;
        let len = (last as usize) * 8 + hi;
        if len > MAX_N { return Err("ssz: bitlist overflow on decode"); }

        let mut bits = vec![false; len];
        for i in 0..len {
            bits[i] = (bytes[i >> 3] >> (i & 7)) & 1 != 0;
        }
        Ok(Bitlist { bits })
    }
}
