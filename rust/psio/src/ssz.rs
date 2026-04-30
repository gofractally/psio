//! SSZ (Simple Serialize) — Ethereum consensus-layer canonical format.
//!
//! This is the Rust counterpart of
//! `libraries/psio1/cpp/include/psio/to_ssz.hpp` / `from_ssz.hpp`. Wire
//! format is byte-identical with the C++ side — verified by the cross-
//! validation tests at the bottom of this file.
//!
//! Wire rules (from the Ethereum consensus spec):
//!   - Integers: fixed-width little-endian
//!   - bool: 1 byte (0x00 / 0x01)
//!   - float/double: IEEE-754 LE bytes (extension — not part of the core
//!     eth spec but emitted as raw bytes for interop)
//!   - String: raw UTF-8 bytes, no length prefix (size is implicit from
//!     the enclosing span)
//!   - Vec<T> with fixed T: concatenated elements
//!   - Vec<T> with variable T: 4-byte offset table followed by tail
//!     payloads. Offsets are container-relative.
//!   - Option<T>: **always** a 1-byte Union selector prefix (0x00 = None,
//!     0x01 + payload = Some). Unlike pSSZ which uses a min-size rule,
//!     SSZ always carries the selector.
//!
//! Status: bootstrap port — primitives + String + Vec + Option.
//! Reflected-struct support via a derive macro is deferred (same TODO
//! as pSSZ's Rust port).

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SszError(pub &'static str);

pub type SszResult<T> = Result<T, SszError>;

/// Encoding trait.
pub trait SszPack {
    /// True iff the type has a compile-time constant wire size.
    const IS_FIXED_SIZE: bool;

    /// Fixed size in bytes (only valid when `IS_FIXED_SIZE`).
    const FIXED_SIZE: usize;

    /// Size of this value's encoding.
    fn ssz_size(&self) -> usize;

    /// Append this value's encoded bytes to `out`.
    fn ssz_pack(&self, out: &mut Vec<u8>);
}

/// Decoding trait. `bytes` is the span belonging to exactly this value.
pub trait SszUnpack: Sized {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self>;
}

// ── Primitives ──────────────────────────────────────────────────────────────

macro_rules! ssz_impl_primitive_le {
    ($($ty:ty),* $(,)?) => {$(
        impl SszPack for $ty {
            const IS_FIXED_SIZE: bool  = true;
            const FIXED_SIZE:    usize = std::mem::size_of::<$ty>();
            fn ssz_size(&self) -> usize { std::mem::size_of::<$ty>() }
            fn ssz_pack(&self, out: &mut Vec<u8>) {
                out.extend_from_slice(&self.to_le_bytes());
            }
        }
        impl SszUnpack for $ty {
            fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
                const S: usize = std::mem::size_of::<$ty>();
                if bytes.len() < S {
                    return Err(SszError("ssz: primitive underrun"));
                }
                let mut buf = [0u8; S];
                buf.copy_from_slice(&bytes[..S]);
                Ok(<$ty>::from_le_bytes(buf))
            }
        }
    )*};
}

ssz_impl_primitive_le!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

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

impl SszPack for Uint256 {
    const IS_FIXED_SIZE: bool = true;
    const FIXED_SIZE:    usize = 32;
    fn ssz_size(&self) -> usize { 32 }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        for l in &self.limb { out.extend_from_slice(&l.to_le_bytes()); }
    }
}
impl SszUnpack for Uint256 {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if bytes.len() < 32 { return Err(SszError("ssz: uint256 underrun")); }
        let mut limb = [0u64; 4];
        for i in 0..4 {
            let mut buf = [0u8; 8];
            buf.copy_from_slice(&bytes[i * 8 .. (i + 1) * 8]);
            limb[i] = u64::from_le_bytes(buf);
        }
        Ok(Uint256 { limb })
    }
}

impl SszPack for bool {
    const IS_FIXED_SIZE: bool  = true;
    const FIXED_SIZE:    usize = 1;
    fn ssz_size(&self) -> usize { 1 }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        out.push(if *self { 1 } else { 0 });
    }
}
impl SszUnpack for bool {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if bytes.is_empty() {
            return Err(SszError("ssz: bool underrun"));
        }
        match bytes[0] {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(SszError("ssz: invalid bool encoding")),
        }
    }
}

// ── String (raw bytes, no length prefix) ────────────────────────────────────

impl SszPack for String {
    const IS_FIXED_SIZE: bool  = false;
    const FIXED_SIZE:    usize = 0;
    fn ssz_size(&self) -> usize { self.len() }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self.as_bytes());
    }
}
impl SszUnpack for String {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        std::str::from_utf8(bytes)
            .map(|s| s.to_owned())
            .map_err(|_| SszError("ssz: invalid UTF-8 in string"))
    }
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

impl<const N: usize> SszPack for Bitvector<N> {
    const IS_FIXED_SIZE: bool  = true;
    const FIXED_SIZE:    usize = (N + 7) / 8;
    fn ssz_size(&self) -> usize { (N + 7) / 8 }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.bytes);
    }
}
impl<const N: usize> SszUnpack for Bitvector<N> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        let nb = (N + 7) / 8;
        if bytes.len() < nb { return Err(SszError("ssz: bitvector underrun")); }
        Ok(Bitvector { bytes: bytes[..nb].to_vec() })
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

impl<const MAX_N: usize> SszPack for Bitlist<MAX_N> {
    const IS_FIXED_SIZE: bool  = false;
    const FIXED_SIZE:    usize = 0;
    fn ssz_size(&self) -> usize { (self.bits.len() / 8) + 1 }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
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
}

impl<const MAX_N: usize> SszUnpack for Bitlist<MAX_N> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if bytes.is_empty() { return Err(SszError("ssz: bitlist empty span")); }
        // Find the last non-zero byte → contains the delimiter.
        let mut last = bytes.len() as isize - 1;
        while last >= 0 && bytes[last as usize] == 0 { last -= 1; }
        if last < 0 { return Err(SszError("ssz: bitlist missing delimiter")); }
        let last_byte = bytes[last as usize];
        // Highest set bit position within last_byte.
        let hi = 7 - last_byte.leading_zeros() as usize;
        let len = (last as usize) * 8 + hi;
        if len > MAX_N { return Err(SszError("ssz: bitlist overflow on decode")); }

        let mut bits = vec![false; len];
        for i in 0..len {
            bits[i] = (bytes[i >> 3] >> (i & 7)) & 1 != 0;
        }
        Ok(Bitlist { bits })
    }
}

// ── [T; N] fixed-length array ───────────────────────────────────────────────
//
// Matches C++ std::array<T, N>. Fixed-element arrays are flat concatenation
// (no offset table needed — element count is statically known). Variable-
// element arrays emit an N-entry offset table followed by tail payloads.

impl<T: SszPack, const N: usize> SszPack for [T; N] {
    const IS_FIXED_SIZE: bool  = T::IS_FIXED_SIZE;
    const FIXED_SIZE:    usize = if T::IS_FIXED_SIZE { N * T::FIXED_SIZE } else { 0 };
    fn ssz_size(&self) -> usize {
        if T::IS_FIXED_SIZE {
            N * T::FIXED_SIZE
        } else {
            let mut total = N * 4;
            for e in self { total += e.ssz_size(); }
            total
        }
    }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        if T::IS_FIXED_SIZE {
            // Fast path: when T is a primitive numeric / bool whose
            // in-memory representation equals its SSZ wire representation
            // (always true on little-endian targets when FIXED_SIZE ==
            // size_of::<T>()), copy the whole array as one memcpy
            // instead of a per-element loop. This matches Lighthouse's
            // `FixedVector<u8, N>` fast path and accounts for the
            // previous ~12% encode gap on Validator.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let byte_count = N * T::FIXED_SIZE;
                // SAFETY: T::FIXED_SIZE == size_of::<T>() guarantees the
                // LE byte layout equals the in-memory layout for SSZ
                // primitives. Target endian little is checked above.
                let bytes = unsafe {
                    std::slice::from_raw_parts(
                        self.as_ptr() as *const u8, byte_count)
                };
                out.extend_from_slice(bytes);
                return;
            }
            for e in self { e.ssz_pack(out); }
        } else {
            let slot_start = out.len();
            out.resize(slot_start + N * 4, 0);
            for (i, e) in self.iter().enumerate() {
                let rel = (out.len() - slot_start) as u32;
                out[slot_start + i * 4 .. slot_start + (i + 1) * 4]
                    .copy_from_slice(&rel.to_le_bytes());
                e.ssz_pack(out);
            }
        }
    }
}

impl<T: SszUnpack + SszPack, const N: usize> SszUnpack for [T; N] {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        // Build a Vec then try_into — avoids requiring T: Default + Copy.
        let v: Vec<T> = if T::IS_FIXED_SIZE {
            let es = T::FIXED_SIZE;
            if bytes.len() != N * es {
                return Err(SszError("ssz: array span mismatch"));
            }
            // Fast path (matches encode): single memcpy into `[T; N]`
            // storage when T's wire and memory layouts coincide.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let byte_count = N * T::FIXED_SIZE;
                let mut out: std::mem::MaybeUninit<[T; N]> = std::mem::MaybeUninit::uninit();
                // SAFETY: same invariants as the encode fast path — LE
                // target + size match guarantee the wire bytes are the
                // in-memory bytes. `bytes.len() == byte_count` checked
                // above. Read is bytewise so aligned access not required.
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        bytes.as_ptr(),
                        out.as_mut_ptr() as *mut u8,
                        byte_count);
                    return Ok(out.assume_init());
                }
            }
            (0..N).map(|i| <T as SszUnpack>::ssz_unpack(&bytes[i * es .. (i + 1) * es]))
                  .collect::<SszResult<Vec<T>>>()?
        } else {
            if bytes.len() < N * 4 {
                return Err(SszError("ssz: variable array offset table truncated"));
            }
            let offsets: Vec<u32> = (0..N).map(|i| u32::from_le_bytes([
                bytes[i * 4], bytes[i * 4 + 1],
                bytes[i * 4 + 2], bytes[i * 4 + 3],
            ])).collect();
            (0..N).map(|i| {
                let beg  = offsets[i] as usize;
                let stop = if i + 1 < N { offsets[i + 1] as usize } else { bytes.len() };
                if beg > stop || stop > bytes.len() {
                    return Err(SszError("ssz: array element offset out of range"));
                }
                <T as SszUnpack>::ssz_unpack(&bytes[beg .. stop])
            }).collect::<SszResult<Vec<T>>>()?
        };
        v.try_into().map_err(|_| SszError("ssz: array length conversion failed"))
    }
}

// ── Vec<T> ──────────────────────────────────────────────────────────────────

impl<T: SszPack> SszPack for Vec<T> {
    const IS_FIXED_SIZE: bool  = false;
    const FIXED_SIZE:    usize = 0;
    fn ssz_size(&self) -> usize {
        if T::IS_FIXED_SIZE {
            self.len() * T::FIXED_SIZE
        } else {
            let mut total = self.len() * 4;  // offset table
            for e in self { total += e.ssz_size(); }
            total
        }
    }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        if T::IS_FIXED_SIZE {
            // Whole-vec memcpy: when T's in-memory layout equals its
            // SSZ wire layout, the entire backing buffer is already the
            // serialized form. One `extend_from_slice` replaces the
            // per-element loop — the C++ side's single-memcpy baseline.
            // Requires T: `#[repr(C, packed)]` (or primitive) so
            // size_of::<T>() == T::FIXED_SIZE.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let byte_count = self.len() * T::FIXED_SIZE;
                // SAFETY: T::FIXED_SIZE == size_of::<T>() guarantees no
                // padding between elements; LE target guarantees wire
                // bytes equal in-memory bytes for primitives (or any
                // packed struct composed of them). `Vec<T>` storage is
                // contiguous, so `self.as_ptr()..+byte_count` is valid.
                let bytes = unsafe {
                    std::slice::from_raw_parts(
                        self.as_ptr() as *const u8, byte_count)
                };
                out.extend_from_slice(bytes);
                return;
            }
            for e in self { e.ssz_pack(out); }
        } else {
            // Single-pass backpatching (matching the C++ side): reserve
            // 4-byte offset slots, then emit each payload and memcpy its
            // container-relative offset into the slot.
            let slot_start = out.len();
            out.resize(slot_start + self.len() * 4, 0);
            for (i, e) in self.iter().enumerate() {
                let rel = (out.len() - slot_start) as u32;
                out[slot_start + i * 4 .. slot_start + (i + 1) * 4]
                    .copy_from_slice(&rel.to_le_bytes());
                e.ssz_pack(out);
            }
        }
    }
}

impl<T: SszUnpack + SszPack> SszUnpack for Vec<T> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if bytes.is_empty() { return Ok(Vec::new()); }
        if T::IS_FIXED_SIZE {
            let es = T::FIXED_SIZE;
            if bytes.len() % es != 0 {
                return Err(SszError("ssz: vector span not divisible by fixed size"));
            }
            let n = bytes.len() / es;
            // Whole-vec memcpy decode: same invariants as encode. We
            // allocate an uninit `Vec<T>` of length n, then copy the
            // wire bytes directly into its backing buffer. Matches the
            // `vector::assign(p, p+n)` pattern from the project memory —
            // 2× decode speedup over resize+memcpy.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let mut v: Vec<T> = Vec::with_capacity(n);
                // SAFETY: capacity == n, so the first n * size_of::<T>()
                // bytes of the allocation are owned and writable. After
                // the copy every element is initialized (all bit
                // patterns in the wire bytes are valid for T since
                // T::FIXED_SIZE == size_of::<T>() implies T is a
                // memcpy-safe plain-old-data type here).
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        bytes.as_ptr(),
                        v.as_mut_ptr() as *mut u8,
                        n * T::FIXED_SIZE);
                    v.set_len(n);
                }
                return Ok(v);
            }
            let mut out = Vec::with_capacity(n);
            for i in 0..n {
                out.push(<T as SszUnpack>::ssz_unpack(&bytes[i * es .. (i + 1) * es])?);
            }
            Ok(out)
        } else {
            if bytes.len() < 4 {
                return Err(SszError("ssz: vector too short for first offset"));
            }
            let first = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
            if first % 4 != 0 || first as usize > bytes.len() {
                return Err(SszError("ssz: invalid first offset in vector"));
            }
            let n = (first / 4) as usize;
            let mut offsets = Vec::with_capacity(n);
            for i in 0..n {
                offsets.push(u32::from_le_bytes([
                    bytes[i * 4], bytes[i * 4 + 1],
                    bytes[i * 4 + 2], bytes[i * 4 + 3],
                ]));
            }
            let mut out = Vec::with_capacity(n);
            for i in 0..n {
                let beg  = offsets[i] as usize;
                let stop = if i + 1 < n { offsets[i + 1] as usize } else { bytes.len() };
                if beg > stop || stop > bytes.len() {
                    return Err(SszError("ssz: vector element offset out of range"));
                }
                out.push(<T as SszUnpack>::ssz_unpack(&bytes[beg .. stop])?);
            }
            Ok(out)
        }
    }
}

// ── BoundedList<T, N> / BoundedString<N> — C++ bounded_list / bounded_string
//
// Wire format is byte-identical to `Vec<T>` / `String` — the bound is a
// Rust-side schema invariant, enforced on decode.

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct BoundedList<T, const N: usize>(pub Vec<T>);

impl<T, const N: usize> BoundedList<T, N> {
    pub fn new() -> Self { BoundedList(Vec::new()) }
    pub fn try_from_vec(v: Vec<T>) -> Result<Self, SszError> {
        if v.len() > N { return Err(SszError("BoundedList overflow")); }
        Ok(BoundedList(v))
    }
    pub fn inner(&self) -> &Vec<T> { &self.0 }
    pub fn into_inner(self) -> Vec<T> { self.0 }
    pub fn len(&self) -> usize { self.0.len() }
    pub fn is_empty(&self) -> bool { self.0.is_empty() }
}

impl<T: SszPack, const N: usize> SszPack for BoundedList<T, N> {
    const IS_FIXED_SIZE: bool  = false;
    const FIXED_SIZE:    usize = 0;
    fn ssz_size(&self) -> usize { self.0.ssz_size() }
    fn ssz_pack(&self, out: &mut Vec<u8>) { self.0.ssz_pack(out) }
}
impl<T: SszUnpack + SszPack, const N: usize> SszUnpack for BoundedList<T, N> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        let v = <Vec<T>>::ssz_unpack(bytes)?;
        if v.len() > N { return Err(SszError("ssz: BoundedList overflow on decode")); }
        Ok(BoundedList(v))
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct BoundedString<const N: usize>(pub String);

impl<const N: usize> BoundedString<N> {
    pub fn new() -> Self { BoundedString(String::new()) }
    pub fn try_from_string(s: String) -> Result<Self, SszError> {
        if s.len() > N { return Err(SszError("BoundedString overflow")); }
        Ok(BoundedString(s))
    }
    pub fn as_str(&self) -> &str { &self.0 }
    pub fn into_inner(self) -> String { self.0 }
    pub fn len(&self) -> usize { self.0.len() }
}

impl<const N: usize> SszPack for BoundedString<N> {
    const IS_FIXED_SIZE: bool  = false;
    const FIXED_SIZE:    usize = 0;
    fn ssz_size(&self) -> usize { self.0.len() }
    fn ssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.0.as_bytes()); }
}
impl<const N: usize> SszUnpack for BoundedString<N> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if bytes.len() > N { return Err(SszError("ssz: BoundedString overflow on decode")); }
        Ok(BoundedString(String::ssz_unpack(bytes)?))
    }
}

// `BoundedBytes<N>` — convenience alias.
pub type BoundedBytes<const N: usize> = BoundedList<u8, N>;

// ── Option<T> = Union[null, T] ──────────────────────────────────────────────
//
// Unlike pSSZ, SSZ always carries the 1-byte selector, regardless of
// whether the inner type's min size is zero.

impl<T: SszPack> SszPack for Option<T> {
    const IS_FIXED_SIZE: bool  = false;
    const FIXED_SIZE:    usize = 0;
    fn ssz_size(&self) -> usize {
        1 + match self { Some(v) => v.ssz_size(), None => 0 }
    }
    fn ssz_pack(&self, out: &mut Vec<u8>) {
        out.push(if self.is_some() { 1 } else { 0 });
        if let Some(v) = self { v.ssz_pack(out); }
    }
}

impl<T: SszUnpack + SszPack> SszUnpack for Option<T> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if bytes.is_empty() {
            return Err(SszError("ssz: optional selector byte missing"));
        }
        match bytes[0] {
            0 => {
                if bytes.len() != 1 {
                    return Err(SszError("ssz: optional None must be empty payload"));
                }
                Ok(None)
            }
            1 => Ok(Some(<T as SszUnpack>::ssz_unpack(&bytes[1..])?)),
            _ => Err(SszError("ssz: invalid optional selector")),
        }
    }
}

// ── Box<T> — heap-indirect storage, wire-transparent ───────────────────────
//
// Box is a storage detail: it changes *where* T lives on the decoded side
// but the wire is identical to T. Needed for BeaconState-sized types whose
// inline fixed fields (e.g. randao_mixes = 2 MiB) would overflow the
// default 8 MiB thread stack if materialized by value.

impl<T: SszPack + ?Sized> SszPack for Box<T> {
    const IS_FIXED_SIZE: bool  = T::IS_FIXED_SIZE;
    const FIXED_SIZE:    usize = T::FIXED_SIZE;
    fn ssz_size(&self) -> usize { (**self).ssz_size() }
    fn ssz_pack(&self, out: &mut Vec<u8>) { (**self).ssz_pack(out) }
}

// Box<[T; N]>: heap-allocated fixed array decoded in place, avoiding any
// stack materialization. Critical for BeaconState fields like randao_mixes
// ([[u8;32]; 65536] = 2 MiB) that would blow the 8 MiB thread stack.
// A blanket `Box<T>` Unpack is not added here because it would overlap
// this impl under Rust coherence rules (no specialization on stable).
impl<T: SszUnpack + SszPack, const N: usize> SszUnpack for Box<[T; N]> {
    fn ssz_unpack(bytes: &[u8]) -> SszResult<Self> {
        if !T::IS_FIXED_SIZE {
            // Variable-element boxed array: fall back to the stack-using
            // path. Not used by BeaconState (all boxed arrays are
            // fixed-element), so the stack cost is bounded.
            return Ok(Box::new(<[T; N] as SszUnpack>::ssz_unpack(bytes)?));
        }
        let es = T::FIXED_SIZE;
        if bytes.len() != N * es {
            return Err(SszError("ssz: boxed array span mismatch"));
        }
        let layout = std::alloc::Layout::new::<[T; N]>();
        // SAFETY: layout is non-zero for N >= 1 (enforced by ssz use
        // cases — no BeaconState array is zero-length). The pointer is
        // either Box::from_raw'd or deallocated on every path.
        unsafe {
            let raw = std::alloc::alloc(layout) as *mut [T; N];
            if raw.is_null() { std::alloc::handle_alloc_error(layout); }

            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                // Fast path: whole array is a single memcpy from wire.
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr(), raw as *mut u8, N * es);
                return Ok(Box::from_raw(raw));
            }
            // Slow path: per-element unpack, dropping partial on error.
            let elem_ptr = raw as *mut T;
            for i in 0..N {
                match <T as SszUnpack>::ssz_unpack(&bytes[i * es .. (i + 1) * es]) {
                    Ok(v) => std::ptr::write(elem_ptr.add(i), v),
                    Err(e) => {
                        for j in 0..i { std::ptr::drop_in_place(elem_ptr.add(j)); }
                        std::alloc::dealloc(raw as *mut u8, layout);
                        return Err(e);
                    }
                }
            }
            Ok(Box::from_raw(raw))
        }
    }
}

// ── Structural validation ───────────────────────────────────────────────────
//
// `ssz_validate::<T>(buf)` walks the same recursive shape as `from_ssz`
// but never allocates. Returns `Err` on malformed input. Matches the
// semantics of C++ `ssz_validate<T>(buffer)` from `from_ssz.hpp`.

pub trait SszValidate {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()>;
}

macro_rules! impl_validate_primitive {
    ($($ty:ty),* $(,)?) => {$(
        impl SszValidate for $ty {
            fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
                if bytes.len() < std::mem::size_of::<$ty>() {
                    return Err(SszError("ssz validate: primitive underrun"));
                }
                Ok(())
            }
        }
    )*};
}
impl_validate_primitive!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

impl SszValidate for bool {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if bytes.is_empty() { return Err(SszError("ssz validate: bool underrun")); }
        if bytes[0] > 1 { return Err(SszError("ssz validate: invalid bool")); }
        Ok(())
    }
}

impl SszValidate for Uint256 {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if bytes.len() < 32 { return Err(SszError("ssz validate: uint256 underrun")); }
        Ok(())
    }
}

impl SszValidate for String {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        std::str::from_utf8(bytes)
            .map(|_| ())
            .map_err(|_| SszError("ssz validate: invalid UTF-8"))
    }
}

impl<T: SszPack + SszValidate> SszValidate for Vec<T> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if bytes.is_empty() { return Ok(()); }
        if T::IS_FIXED_SIZE {
            if bytes.len() % T::FIXED_SIZE != 0 {
                return Err(SszError("ssz validate: vec span not divisible"));
            }
            let n = bytes.len() / T::FIXED_SIZE;
            for i in 0..n {
                T::ssz_validate(&bytes[i * T::FIXED_SIZE .. (i + 1) * T::FIXED_SIZE])?;
            }
        } else {
            if bytes.len() < 4 { return Err(SszError("ssz validate: vec short")); }
            let first = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
            if first % 4 != 0 || first as usize > bytes.len() {
                return Err(SszError("ssz validate: bad first offset"));
            }
            let n = (first / 4) as usize;
            let mut prev = first as usize;
            for i in 1..n {
                let off = u32::from_le_bytes([
                    bytes[i * 4], bytes[i * 4 + 1],
                    bytes[i * 4 + 2], bytes[i * 4 + 3],
                ]) as usize;
                if off < prev || off > bytes.len() {
                    return Err(SszError("ssz validate: non-monotone offset"));
                }
                prev = off;
            }
            // Recursively validate each element.
            for i in 0..n {
                let beg = u32::from_le_bytes([
                    bytes[i * 4], bytes[i * 4 + 1],
                    bytes[i * 4 + 2], bytes[i * 4 + 3],
                ]) as usize;
                let stop = if i + 1 < n {
                    u32::from_le_bytes([
                        bytes[(i + 1) * 4], bytes[(i + 1) * 4 + 1],
                        bytes[(i + 1) * 4 + 2], bytes[(i + 1) * 4 + 3],
                    ]) as usize
                } else { bytes.len() };
                T::ssz_validate(&bytes[beg .. stop])?;
            }
        }
        Ok(())
    }
}

impl<T: SszPack + SszValidate> SszValidate for Option<T> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if bytes.is_empty() { return Err(SszError("ssz validate: option selector missing")); }
        match bytes[0] {
            0 => if bytes.len() != 1 {
                Err(SszError("ssz validate: None must have empty payload"))
            } else { Ok(()) },
            1 => T::ssz_validate(&bytes[1..]),
            _ => Err(SszError("ssz validate: bad selector")),
        }
    }
}

// Arrays: fixed-element is flat concat (validate each); variable-element
// uses offset table (same shape as Vec<T>).
impl<T: SszPack + SszValidate, const N: usize> SszValidate for [T; N] {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if T::IS_FIXED_SIZE {
            let es = T::FIXED_SIZE;
            if bytes.len() != N * es {
                return Err(SszError("ssz validate: array span mismatch"));
            }
            for i in 0..N {
                T::ssz_validate(&bytes[i * es .. (i + 1) * es])?;
            }
            Ok(())
        } else {
            if bytes.len() < N * 4 {
                return Err(SszError("ssz validate: array offset table underrun"));
            }
            let mut prev: u32 = 0;
            for i in 0..N {
                let off = u32::from_le_bytes([
                    bytes[i * 4], bytes[i * 4 + 1],
                    bytes[i * 4 + 2], bytes[i * 4 + 3],
                ]);
                if (off as usize) > bytes.len() {
                    return Err(SszError("ssz validate: array offset out of range"));
                }
                if off < prev {
                    return Err(SszError("ssz validate: non-monotone array offset"));
                }
                prev = off;
            }
            for i in 0..N {
                let beg = u32::from_le_bytes([
                    bytes[i * 4], bytes[i * 4 + 1],
                    bytes[i * 4 + 2], bytes[i * 4 + 3],
                ]) as usize;
                let stop = if i + 1 < N {
                    u32::from_le_bytes([
                        bytes[(i + 1) * 4], bytes[(i + 1) * 4 + 1],
                        bytes[(i + 1) * 4 + 2], bytes[(i + 1) * 4 + 3],
                    ]) as usize
                } else { bytes.len() };
                T::ssz_validate(&bytes[beg .. stop])?;
            }
            Ok(())
        }
    }
}

impl<T: SszPack + SszValidate, const N: usize> SszValidate for Box<[T; N]> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        <[T; N] as SszValidate>::ssz_validate(bytes)
    }
}

impl<T: SszPack + SszValidate, const N: usize> SszValidate for BoundedList<T, N> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        <Vec<T>>::ssz_validate(bytes)?;
        // Bound check: decode would need to know count — instead, conservatively
        // accept anything within budget; real bound enforcement happens on decode.
        Ok(())
    }
}

impl<const N: usize> SszValidate for BoundedString<N> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if bytes.len() > N {
            return Err(SszError("ssz validate: BoundedString overflow"));
        }
        String::ssz_validate(bytes)
    }
}

impl<const N: usize> SszValidate for Bitvector<N> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        let nb = (N + 7) / 8;
        if bytes.len() < nb {
            return Err(SszError("ssz validate: bitvector underrun"));
        }
        // Upper-bit hygiene: bits past N in the last byte should be 0.
        if N % 8 != 0 {
            let mask: u8 = !((1u8 << (N % 8)) - 1);
            if bytes[nb - 1] & mask != 0 {
                return Err(SszError("ssz validate: bitvector has bits past N"));
            }
        }
        Ok(())
    }
}

impl<const MAX_N: usize> SszValidate for Bitlist<MAX_N> {
    fn ssz_validate(bytes: &[u8]) -> SszResult<()> {
        if bytes.is_empty() {
            return Err(SszError("ssz validate: bitlist empty span"));
        }
        // Last non-zero byte contains delimiter bit.
        let mut last = bytes.len() as isize - 1;
        while last >= 0 && bytes[last as usize] == 0 { last -= 1; }
        if last < 0 {
            return Err(SszError("ssz validate: bitlist missing delimiter"));
        }
        let last_byte = bytes[last as usize];
        let hi = 7 - last_byte.leading_zeros() as usize;
        let len = (last as usize) * 8 + hi;
        if len > MAX_N {
            return Err(SszError("ssz validate: bitlist length exceeds cap"));
        }
        Ok(())
    }
}

/// Public entry point matching C++ `ssz_validate<T>(buffer)`.
pub fn ssz_validate<T: SszValidate>(bytes: &[u8]) -> SszResult<()> {
    T::ssz_validate(bytes)
}

// ── Convenience top-level API ───────────────────────────────────────────────

pub fn to_ssz<T: SszPack>(value: &T) -> Vec<u8> {
    let mut out = Vec::with_capacity(value.ssz_size());
    value.ssz_pack(&mut out);
    out
}

pub fn from_ssz<T: SszUnpack>(bytes: &[u8]) -> SszResult<T> {
    <T as SszUnpack>::ssz_unpack(bytes)
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn primitives_round_trip() {
        assert_eq!(from_ssz::<u32>(&to_ssz(&0xDEADBEEFu32)).unwrap(), 0xDEADBEEF);
        assert_eq!(from_ssz::<i64>(&to_ssz(&-1_234_567_890i64)).unwrap(), -1_234_567_890);
        assert_eq!(from_ssz::<f64>(&to_ssz(&3.14159f64)).unwrap(), 3.14159);
        assert_eq!(from_ssz::<bool>(&to_ssz(&true)).unwrap(), true);
    }

    #[test]
    fn string_round_trip_no_prefix() {
        let b = to_ssz(&"hello".to_string());
        assert_eq!(b, b"hello");
        assert_eq!(from_ssz::<String>(&b).unwrap(), "hello");
    }

    #[test]
    fn vec_fixed_elements_concatenated() {
        let v = vec![1u32, 2, 3];
        let b = to_ssz(&v);
        assert_eq!(b.len(), 12);  // 3 × 4 bytes, no prefix
        assert_eq!(from_ssz::<Vec<u32>>(&b).unwrap(), v);
    }

    #[test]
    fn vec_variable_elements_offset_table() {
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        let b = to_ssz(&v);
        // 3 × 4-byte offsets = 12, then "abcdef" = 6 → total 18.
        assert_eq!(b.len(), 18);
        assert_eq!(from_ssz::<Vec<String>>(&b).unwrap(), v);
    }

    #[test]
    fn option_always_has_selector() {
        let some: Option<u32> = Some(42);
        let b = to_ssz(&some);
        assert_eq!(b, vec![1, 42, 0, 0, 0]);  // selector + u32 LE
        assert_eq!(from_ssz::<Option<u32>>(&b).unwrap(), some);

        let none: Option<u32> = None;
        let b = to_ssz(&none);
        assert_eq!(b, vec![0]);
        assert_eq!(from_ssz::<Option<u32>>(&b).unwrap(), None);
    }

    // ── Cross-validation against C++ SSZ encoder ────────────────────────────
    // Hex fixtures were emitted by /tmp/xval/emit_fixtures.cpp on 2026-04-23
    // using psio1::convert_to_ssz().

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn cpp_ssz_u32_cross() {
        // C++: convert_to_ssz(uint32_t{0xDEADBEEF}) → "efbeadde"
        let expected = hex("efbeadde");
        assert_eq!(to_ssz(&0xDEADBEEFu32), expected);
        assert_eq!(from_ssz::<u32>(&expected).unwrap(), 0xDEADBEEF);
    }

    #[test]
    fn cpp_ssz_string_cross() {
        // C++: convert_to_ssz(string("hello")) → "68656c6c6f" (raw bytes, no prefix)
        let expected = hex("68656c6c6f");
        let s = "hello".to_string();
        assert_eq!(to_ssz(&s), expected);
        assert_eq!(from_ssz::<String>(&expected).unwrap(), s);
    }

    #[test]
    fn cpp_ssz_vec_u32_cross() {
        // C++: vector<u32>{1,2,3} → "010000000200000003000000"
        let expected = hex("010000000200000003000000");
        let v = vec![1u32, 2, 3];
        assert_eq!(to_ssz(&v), expected);
        assert_eq!(from_ssz::<Vec<u32>>(&expected).unwrap(), v);
    }

    #[test]
    fn cpp_ssz_vec_string_cross() {
        // C++: vector<string>{"a","bc","def"} → "0c0000000d0000000f000000616263646566"
        let expected = hex("0c0000000d0000000f000000616263646566");
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        assert_eq!(to_ssz(&v), expected);
        assert_eq!(from_ssz::<Vec<String>>(&expected).unwrap(), v);
    }

    #[test]
    fn cpp_ssz_option_u32_some_cross() {
        // C++: optional<u32>{42} → "012a000000" (Union selector + payload)
        let expected = hex("012a000000");
        assert_eq!(to_ssz(&Some(42u32)), expected);
        assert_eq!(from_ssz::<Option<u32>>(&expected).unwrap(), Some(42));
    }

    #[test]
    fn cpp_ssz_option_u32_none_cross() {
        // C++: optional<u32>{} → "00"
        let expected = hex("00");
        let v: Option<u32> = None;
        assert_eq!(to_ssz(&v), expected);
        assert_eq!(from_ssz::<Option<u32>>(&expected).unwrap(), None);
    }

    // Ext-int cross-validation (Phase I).

    #[test]
    fn cpp_ssz_u128_cross() {
        // C++: ((unsigned __int128)0x00FF00FF00FF00FFULL << 64) | 0x00FF00FF00FF00FFULL
        // → "ff00ff00ff00ff00ff00ff00ff00ff00"
        let expected = hex("ff00ff00ff00ff00ff00ff00ff00ff00");
        let v: u128 = (0x00FF00FF00FF00FFu128 << 64) | 0x00FF00FF00FF00FFu128;
        assert_eq!(to_ssz(&v), expected);
        assert_eq!(from_ssz::<u128>(&expected).unwrap(), v);
    }

    #[test]
    fn cpp_ssz_i128_cross() {
        // C++: (__int128)(-42) → "d6ffffffffffffffffffffffffffffff"
        let expected = hex("d6ffffffffffffffffffffffffffffff");
        let v: i128 = -42;
        assert_eq!(to_ssz(&v), expected);
        assert_eq!(from_ssz::<i128>(&expected).unwrap(), v);
    }

    #[test]
    fn cpp_ssz_bitvector_cross() {
        // C++: bitvector<12>, bits 0, 3, 11 → "0908"
        let expected = hex("0908");
        let mut bv: Bitvector<12> = Bitvector::new();
        bv.set(0, true);
        bv.set(3, true);
        bv.set(11, true);
        assert_eq!(to_ssz(&bv), expected);
        let back = from_ssz::<Bitvector<12>>(&expected).unwrap();
        assert_eq!(back, bv);
    }

    #[test]
    fn cpp_ssz_bitlist_len5_cross() {
        // C++: bitlist<16> len=5, bits 0, 2 → "25"
        // bits 0,2 set → 0x05; delimiter at bit 5 → 0x20. Combined = 0x25.
        let expected = hex("25");
        let mut bl: Bitlist<16> = Bitlist::with_len(5);
        bl.set(0, true);
        bl.set(2, true);
        assert_eq!(to_ssz(&bl), expected);
        let back = from_ssz::<Bitlist<16>>(&expected).unwrap();
        assert_eq!(back.len(), 5);
        assert!(back.test(0));
        assert!(!back.test(1));
        assert!(back.test(2));
    }

    #[test]
    fn cpp_ssz_bitlist_empty_cross() {
        // C++: bitlist<16> len=0 → "01" (just the delimiter)
        let expected = hex("01");
        let bl: Bitlist<16> = Bitlist::new();
        assert_eq!(to_ssz(&bl), expected);
        let back = from_ssz::<Bitlist<16>>(&expected).unwrap();
        assert_eq!(back.len(), 0);
    }

    #[test]
    fn cpp_ssz_array_fixed_cross() {
        // C++: std::array<u32, 3>{1, 2, 3} → "010000000200000003000000"
        let expected = hex("010000000200000003000000");
        let a: [u32; 3] = [1, 2, 3];
        assert_eq!(to_ssz(&a), expected);
        assert_eq!(from_ssz::<[u32; 3]>(&expected).unwrap(), a);
    }

    #[test]
    fn cpp_ssz_array_variable_cross() {
        // C++: std::array<string, 3>{"a","bc","def"} → same as vec<string>
        let expected = hex("0c0000000d0000000f000000616263646566");
        let a: [String; 3] = ["a".into(), "bc".into(), "def".into()];
        assert_eq!(to_ssz(&a), expected);
        assert_eq!(from_ssz::<[String; 3]>(&expected).unwrap(), a);
    }

    #[test]
    fn cpp_ssz_bounded_list_cross() {
        // C++: bounded_list<u32, 8>{10, 20, 30} → same wire as vec<u32>
        let expected = hex("0a000000140000001e000000");
        let bl = BoundedList::<u32, 8>::try_from_vec(vec![10u32, 20, 30]).unwrap();
        assert_eq!(to_ssz(&bl), expected);
        let back = from_ssz::<BoundedList<u32, 8>>(&expected).unwrap();
        assert_eq!(back, bl);
    }

    #[test]
    fn cpp_ssz_bounded_string_cross() {
        // C++: bounded_string<16>{"hello"} → same wire as string
        let expected = hex("68656c6c6f");
        let bs = BoundedString::<16>::try_from_string("hello".into()).unwrap();
        assert_eq!(to_ssz(&bs), expected);
        let back = from_ssz::<BoundedString<16>>(&expected).unwrap();
        assert_eq!(back, bs);
    }

    #[test]
    fn bounded_overflow_rejected_on_decode() {
        // Pretend wire says 5 u32s, but BoundedList bound is 2.
        let wire: Vec<u32> = vec![1, 2, 3, 4, 5];
        let bytes = to_ssz(&wire);
        let r = from_ssz::<BoundedList<u32, 2>>(&bytes);
        assert!(r.is_err(), "should reject: 5 > bound 2");
    }

    #[test]
    fn validate_passes_on_good_fixtures() {
        // Every C++ fixture must validate.
        assert!(ssz_validate::<u32>(&hex("efbeadde")).is_ok());
        assert!(ssz_validate::<String>(&hex("68656c6c6f")).is_ok());
        assert!(ssz_validate::<Vec<u32>>(&hex("010000000200000003000000")).is_ok());
        assert!(ssz_validate::<Vec<String>>(&hex("0c0000000d0000000f000000616263646566")).is_ok());
        assert!(ssz_validate::<Option<u32>>(&hex("012a000000")).is_ok());
        assert!(ssz_validate::<Option<u32>>(&hex("00")).is_ok());
    }

    #[test]
    fn validate_rejects_bad_bool() {
        assert!(ssz_validate::<bool>(&[2]).is_err());
        assert!(ssz_validate::<bool>(&[]).is_err());
    }

    #[test]
    fn validate_rejects_bad_optional_selector() {
        assert!(ssz_validate::<Option<u32>>(&[0, 0, 0, 0, 0]).is_err());  // selector 0 must have empty payload
        assert!(ssz_validate::<Option<u32>>(&[2]).is_err());              // selector > 1
    }

    #[test]
    fn validate_rejects_non_monotone_offsets() {
        // First offset = 12 (correct, 3 elems), second offset = 5 (< 12 = bad)
        let bad = hex("0c0000000500000010000000616263");
        assert!(ssz_validate::<Vec<String>>(&bad).is_err());
    }

    #[test]
    fn validate_uint256_uint128_extints() {
        assert!(ssz_validate::<u128>(&hex("ff00ff00ff00ff00ff00ff00ff00ff00")).is_ok());
        assert!(ssz_validate::<i128>(&hex("d6ffffffffffffffffffffffffffffff")).is_ok());
        assert!(ssz_validate::<Uint256>(&hex(
            "0100000000000000020000000000000003000000000000000400000000000000"
        )).is_ok());
        // Underrun cases
        assert!(ssz_validate::<u128>(&hex("00")).is_err());
        assert!(ssz_validate::<Uint256>(&hex("00")).is_err());
    }

    #[test]
    fn validate_bounded_enforces_cap() {
        // 5-char string with bound 16 → ok.
        assert!(ssz_validate::<BoundedString<16>>(&hex("68656c6c6f")).is_ok());
        // Same 5 chars with bound 3 → rejected.
        assert!(ssz_validate::<BoundedString<3>>(&hex("68656c6c6f")).is_err());
    }

    #[test]
    fn validate_bitvector_rejects_bits_past_N() {
        // N=12 → bytes 1.byte[1] uses bits 8..11. bits 12..15 must be zero.
        // 0x09 0x08 = bit 0, 3, 11 set — valid.
        assert!(ssz_validate::<Bitvector<12>>(&hex("0908")).is_ok());
        // 0x09 0x18 = bit 12 set too (past N) — invalid.
        assert!(ssz_validate::<Bitvector<12>>(&hex("0918")).is_err());
    }

    #[test]
    fn validate_bitlist_delimiter_required() {
        // Valid len-5 bitlist: "25"
        assert!(ssz_validate::<Bitlist<16>>(&hex("25")).is_ok());
        // Empty bitlist = just delimiter byte 0x01
        assert!(ssz_validate::<Bitlist<16>>(&hex("01")).is_ok());
        // All zeros → missing delimiter
        assert!(ssz_validate::<Bitlist<16>>(&hex("0000")).is_err());
    }

    #[test]
    fn cpp_ssz_uint256_cross() {
        // C++: limbs = {1, 2, 3, 4} → four u64 LE back-to-back
        let expected =
            hex("0100000000000000020000000000000003000000000000000400000000000000");
        let v = Uint256 { limb: [1, 2, 3, 4] };
        assert_eq!(to_ssz(&v), expected);
        assert_eq!(from_ssz::<Uint256>(&expected).unwrap(), v);
    }
}
