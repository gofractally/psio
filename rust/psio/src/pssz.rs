//! pSSZ (PsiSSZ) — hybrid of SSZ's implicit sizing and fracpack's
//! extensibility. See `.issues/pssz-format-design.md` (top of repo) for the
//! full specification. This module is the Rust counterpart of
//! `libraries/psio1/cpp/include/psio/to_pssz.hpp` and should round-trip
//! byte-identically with it for shared shapes.
//!
//! Status: bootstrap port — primitives, `String`, `Vec<T>`, `Option<T>`.
//! Reflected-struct support via a derive macro is deferred.
//!
//! The C++ side parameterizes the format via template tags (`pssz8`,
//! `pssz16`, `pssz32`); here we use a trait with associated constants.
//!
//! Wire rules (summary):
//!   - Container-relative offsets (the offset of a variable field is from
//!     the start of the container's fixed region, not from the offset slot).
//!   - No length prefix on variable payloads — size of a variable field is
//!     `offset[i+1] - offset[i]` (implicit sizing, SSZ-style).
//!   - `Option<T>` uses a 1-byte Union selector iff `min_encoded_size<T> == 0`
//!     (strings, vecs, nested optionals). For T where min > 0, `None` is 0
//!     bytes and `Some` is the encoded T; the enclosing span tells the
//!     decoder which.

use std::convert::TryFrom;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PsszError(pub &'static str);

pub type PsszResult<T> = Result<T, PsszError>;

/// A pSSZ format variant. Offset and header widths come from this trait.
pub trait PsszFormat {
    const OFFSET_BYTES: usize;
    const HEADER_BYTES: usize;
    const MAX_TOTAL: u64;
}

pub struct Pssz8;
impl PsszFormat for Pssz8 {
    const OFFSET_BYTES: usize = 1;
    const HEADER_BYTES: usize = 1;
    const MAX_TOTAL: u64 = 0xff;
}

pub struct Pssz16;
impl PsszFormat for Pssz16 {
    const OFFSET_BYTES: usize = 2;
    const HEADER_BYTES: usize = 2;
    const MAX_TOTAL: u64 = 0xffff;
}

pub struct Pssz32;
impl PsszFormat for Pssz32 {
    const OFFSET_BYTES: usize = 4;
    const HEADER_BYTES: usize = 4;
    const MAX_TOTAL: u64 = 0xffff_ffff;
}

/// Encoding trait. `F` picks the offset/header width.
pub trait PsszPack<F: PsszFormat> {
    /// `min_encoded_size` — used by enclosing `Option<T>` to decide whether
    /// a 1-byte Union selector is needed. When this is 0, adjacency can't
    /// disambiguate None from Some(empty) and the selector is required.
    const MIN_ENCODED_SIZE: usize;

    /// Maximum possible encoded size when known at compile time.
    /// Mirrors C++ `max_encoded_size<T>()` which returns
    /// `std::optional<std::size_t>`. `None` means unbounded (e.g. `Vec<T>`
    /// without an enclosing `BoundedList`) — auto-format selection falls
    /// back to the widest format when max is None.
    const MAX_ENCODED_SIZE: Option<usize>;

    /// True iff the type has a compile-time constant wire size.
    const IS_FIXED_SIZE: bool;

    /// Fixed size in bytes (only valid when `IS_FIXED_SIZE`).
    const FIXED_SIZE: usize;

    /// Size of this value's encoding (not counting any enclosing selector).
    fn pssz_size(&self) -> usize;

    /// Append this value's encoded bytes to `out`.
    fn pssz_pack(&self, out: &mut Vec<u8>);
}

/// Compile-time helper: pick the narrowest pssz format that fits max size.
/// Use in `const` context or via the `auto_pssz_format!` macro wrapper.
pub const fn choose_pssz_format_width(max: Option<usize>) -> PsszWidth {
    match max {
        Some(m) if m <= 0xff       => PsszWidth::W8,
        Some(m) if m <= 0xffff     => PsszWidth::W16,
        _                           => PsszWidth::W32,
    }
}

/// Width discriminator for runtime dispatch. Parallel to C++'s
/// `auto_pssz_format_t<T>` alias template.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PsszWidth { W8, W16, W32 }

/// Decoding trait. `bytes` is the span belonging to exactly this value —
/// caller is responsible for trimming to the right sub-span via offsets.
pub trait PsszUnpack<F: PsszFormat>: Sized {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self>;
}

// ── Macro-driven primitive impls ────────────────────────────────────────────

macro_rules! pssz_impl_primitive_le {
    ($($ty:ty),* $(,)?) => {$(
        impl<F: PsszFormat> PsszPack<F> for $ty {
            const MIN_ENCODED_SIZE: usize         = std::mem::size_of::<$ty>();
            const MAX_ENCODED_SIZE: Option<usize> = Some(std::mem::size_of::<$ty>());
            const IS_FIXED_SIZE:    bool  = true;
            const FIXED_SIZE:       usize = std::mem::size_of::<$ty>();
            fn pssz_size(&self) -> usize { std::mem::size_of::<$ty>() }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                out.extend_from_slice(&self.to_le_bytes());
            }
        }
        impl<F: PsszFormat> PsszUnpack<F> for $ty {
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                const S: usize = std::mem::size_of::<$ty>();
                if bytes.len() < S { return Err(PsszError("pssz: primitive underrun")); }
                let mut buf = [0u8; S];
                buf.copy_from_slice(&bytes[..S]);
                Ok(<$ty>::from_le_bytes(buf))
            }
        }
    )*};
}

pssz_impl_primitive_le!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

// Uint256 — re-exported from ssz module since it's the same on-wire shape.
pub use crate::ssz::Uint256;

impl<F: PsszFormat> PsszPack<F> for Uint256 {
    const MIN_ENCODED_SIZE: usize         = 32;
    const MAX_ENCODED_SIZE: Option<usize> = Some(32);
    const IS_FIXED_SIZE:    bool  = true;
    const FIXED_SIZE:       usize = 32;
    fn pssz_size(&self) -> usize { 32 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        for l in &self.limb { out.extend_from_slice(&l.to_le_bytes()); }
    }
}
impl<F: PsszFormat> PsszUnpack<F> for Uint256 {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if bytes.len() < 32 { return Err(PsszError("pssz: uint256 underrun")); }
        let mut limb = [0u64; 4];
        for i in 0..4 {
            let mut buf = [0u8; 8];
            buf.copy_from_slice(&bytes[i * 8 .. (i + 1) * 8]);
            limb[i] = u64::from_le_bytes(buf);
        }
        Ok(Uint256 { limb })
    }
}

impl<F: PsszFormat> PsszPack<F> for bool {
    const MIN_ENCODED_SIZE: usize         = 1;
    const MAX_ENCODED_SIZE: Option<usize> = Some(1);
    const IS_FIXED_SIZE:    bool  = true;
    const FIXED_SIZE:       usize = 1;
    fn pssz_size(&self) -> usize { 1 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.push(if *self { 1 } else { 0 });
    }
}
impl<F: PsszFormat> PsszUnpack<F> for bool {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if bytes.is_empty() { return Err(PsszError("pssz: bool underrun")); }
        match bytes[0] {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(PsszError("pssz: invalid bool encoding")),
        }
    }
}

// ── String (no length prefix, size derived from span) ───────────────────────

impl<F: PsszFormat> PsszPack<F> for String {
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;  // unbounded Vec/String/Option
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { self.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self.as_bytes());
    }
}
impl<F: PsszFormat> PsszUnpack<F> for String {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        std::str::from_utf8(bytes)
            .map(|s| s.to_owned())
            .map_err(|_| PsszError("pssz: invalid UTF-8 in string"))
    }
}

// ── [T; N] fixed-length array ───────────────────────────────────────────────

impl<F: PsszFormat, T: PsszPack<F>, const N: usize> PsszPack<F> for [T; N] {
    const MIN_ENCODED_SIZE: usize = if T::IS_FIXED_SIZE { N * T::FIXED_SIZE }
                                    else { N * 1 /* ≥ 1-byte offset slot */ };
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => Some(if T::IS_FIXED_SIZE { N * m } else { N * (F::OFFSET_BYTES + m) }),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = T::IS_FIXED_SIZE;
    const FIXED_SIZE:       usize = if T::IS_FIXED_SIZE { N * T::FIXED_SIZE } else { 0 };
    fn pssz_size(&self) -> usize {
        if T::IS_FIXED_SIZE {
            N * T::FIXED_SIZE
        } else {
            let mut total = N * F::OFFSET_BYTES;
            for e in self { total += e.pssz_size(); }
            total
        }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        if T::IS_FIXED_SIZE {
            // Memcpy fast path: when T's wire representation equals its
            // in-memory representation (true on LE for SSZ primitives
            // where FIXED_SIZE == size_of::<T>()), copy the whole array
            // in one go instead of per-element. Mirrors the SSZ side —
            // without this [u8; 48] pubkey takes 48 u8 pushes per
            // validator × 21k validators in BeaconState encode.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let byte_count = N * T::FIXED_SIZE;
                // SAFETY: LE target + size match guarantee in-memory
                // bytes equal SSZ/pSSZ wire bytes for primitives.
                let bytes = unsafe {
                    std::slice::from_raw_parts(
                        self.as_ptr() as *const u8, byte_count)
                };
                out.extend_from_slice(bytes);
                return;
            }
            for e in self { e.pssz_pack(out); }
        } else {
            let ob = F::OFFSET_BYTES;
            let slot_start = out.len();
            out.resize(slot_start + N * ob, 0);
            for (i, e) in self.iter().enumerate() {
                let rel = out.len() - slot_start;
                write_offset::<F>(&mut out[slot_start + i * ob .. slot_start + (i + 1) * ob], rel);
                e.pssz_pack(out);
            }
        }
    }
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F> for [T; N] {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let v: Vec<T> = if T::IS_FIXED_SIZE {
            let es = T::FIXED_SIZE;
            if bytes.len() != N * es { return Err(PsszError("pssz: array span mismatch")); }
            // Matching memcpy fast path for decode.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let byte_count = N * T::FIXED_SIZE;
                let mut out: std::mem::MaybeUninit<[T; N]> = std::mem::MaybeUninit::uninit();
                // SAFETY: same invariants as the encode fast path. Byte
                // count checked against input len above.
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        bytes.as_ptr(),
                        out.as_mut_ptr() as *mut u8,
                        byte_count);
                    return Ok(out.assume_init());
                }
            }
            (0..N).map(|i| <T as PsszUnpack<F>>::pssz_unpack(&bytes[i * es .. (i + 1) * es]))
                  .collect::<PsszResult<Vec<T>>>()?
        } else {
            let ob = F::OFFSET_BYTES;
            if bytes.len() < N * ob { return Err(PsszError("pssz: variable array truncated")); }
            let offsets: Vec<u32> = (0..N).map(|i| read_offset::<F>(&bytes[i * ob .. (i + 1) * ob])).collect();
            (0..N).map(|i| {
                let beg  = offsets[i] as usize;
                let stop = if i + 1 < N { offsets[i + 1] as usize } else { bytes.len() };
                if beg > stop || stop > bytes.len() {
                    return Err(PsszError("pssz: array element offset out of range"));
                }
                <T as PsszUnpack<F>>::pssz_unpack(&bytes[beg .. stop])
            }).collect::<PsszResult<Vec<T>>>()?
        };
        v.try_into().map_err(|_| PsszError("pssz: array length conversion failed"))
    }
}

// ── BoundedList / BoundedString (re-exports ssz variants) ────────────────────

pub use crate::ssz::{Bitlist, Bitvector, BoundedBytes, BoundedList, BoundedString};

impl<F: PsszFormat, const N: usize> PsszPack<F> for Bitvector<N> {
    const MIN_ENCODED_SIZE: usize         = (N + 7) / 8;
    const MAX_ENCODED_SIZE: Option<usize> = Some((N + 7) / 8);
    const IS_FIXED_SIZE:    bool  = true;
    const FIXED_SIZE:       usize = (N + 7) / 8;
    fn pssz_size(&self) -> usize { (N + 7) / 8 }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(&self.bytes); }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for Bitvector<N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let nb = (N + 7) / 8;
        if bytes.len() < nb { return Err(PsszError("pssz: bitvector underrun")); }
        Ok(Bitvector { bytes: bytes[..nb].to_vec() })
    }
}

impl<F: PsszFormat, const MAX_N: usize> PsszPack<F> for Bitlist<MAX_N> {
    const MIN_ENCODED_SIZE: usize         = 1;  // always ≥ delimiter byte
    const MAX_ENCODED_SIZE: Option<usize> = Some(1 + (MAX_N + 7) / 8);
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { (self.bits.len() / 8) + 1 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        use crate::ssz::SszPack;
        <Self as SszPack>::ssz_pack(self, out);  // wire-identical
    }
}
impl<F: PsszFormat, const MAX_N: usize> PsszUnpack<F> for Bitlist<MAX_N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        use crate::ssz::SszUnpack;
        <Self as SszUnpack>::ssz_unpack(bytes).map_err(|e| PsszError(e.0))
    }
}

impl<F: PsszFormat, T: PsszPack<F>, const N: usize> PsszPack<F> for BoundedList<T, N> {
    const MIN_ENCODED_SIZE: usize = 0;
    // If element has a known max, bounded list max = N × (offset + max<T>).
    // If the element's max is unknown (None), we can't bound the list.
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => Some(if T::IS_FIXED_SIZE { N * m } else { N * (F::OFFSET_BYTES + m) }),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { self.0.pssz_size() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { self.0.pssz_pack(out) }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F>
    for BoundedList<T, N>
{
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let v = <Vec<T> as PsszUnpack<F>>::pssz_unpack(bytes)?;
        if v.len() > N { return Err(PsszError("pssz: BoundedList overflow")); }
        Ok(BoundedList(v))
    }
}

impl<F: PsszFormat, const N: usize> PsszPack<F> for BoundedString<N> {
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = Some(N);
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.0.as_bytes()); }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for BoundedString<N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if bytes.len() > N { return Err(PsszError("pssz: BoundedString overflow")); }
        let s = <String as PsszUnpack<F>>::pssz_unpack(bytes)?;
        Ok(BoundedString(s))
    }
}

// ── Vec<T> ──────────────────────────────────────────────────────────────────

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for Vec<T> {
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;  // unbounded Vec/String/Option
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize {
        if T::IS_FIXED_SIZE {
            self.len() * T::FIXED_SIZE
        } else {
            let ob = F::OFFSET_BYTES;
            let mut total = self.len() * ob;
            for e in self { total += e.pssz_size(); }
            total
        }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        if T::IS_FIXED_SIZE {
            // Whole-vec memcpy fast path — mirror of the SSZ side.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let byte_count = self.len() * T::FIXED_SIZE;
                let bytes = unsafe {
                    std::slice::from_raw_parts(
                        self.as_ptr() as *const u8, byte_count)
                };
                out.extend_from_slice(bytes);
                return;
            }
            for e in self { e.pssz_pack(out); }
        } else {
            // Single-pass backpatching: reserve offset slots, then write
            // payloads while back-filling each slot with (current len −
            // slot_start) converted to the format's offset width.
            let ob = F::OFFSET_BYTES;
            let slot_start = out.len();
            out.resize(slot_start + self.len() * ob, 0);
            for (i, e) in self.iter().enumerate() {
                let rel = out.len() - slot_start;
                write_offset::<F>(&mut out[slot_start + i * ob .. slot_start + (i + 1) * ob], rel);
                e.pssz_pack(out);
            }
        }
    }
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Vec<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if bytes.is_empty() { return Ok(Vec::new()); }
        if T::IS_FIXED_SIZE {
            let es = T::FIXED_SIZE;
            if bytes.len() % es != 0 {
                return Err(PsszError("pssz: vector span not divisible by fixed size"));
            }
            let n = bytes.len() / es;
            // Whole-vec memcpy decode fast path.
            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                let mut v: Vec<T> = Vec::with_capacity(n);
                // SAFETY: capacity matches write size; all bit patterns
                // are valid for T under the layout-match invariant.
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
                out.push(<T as PsszUnpack<F>>::pssz_unpack(&bytes[i * es .. (i + 1) * es])?);
            }
            Ok(out)
        } else {
            let ob = F::OFFSET_BYTES;
            if bytes.len() < ob { return Err(PsszError("pssz: vector too short")); }
            let first = read_offset::<F>(&bytes[..ob]);
            if first as usize % ob != 0 || first as usize > bytes.len() {
                return Err(PsszError("pssz: invalid first offset in vector"));
            }
            let n = first as usize / ob;
            let mut offsets = Vec::with_capacity(n);
            for i in 0..n {
                offsets.push(read_offset::<F>(&bytes[i * ob .. (i + 1) * ob]));
            }
            let mut out = Vec::with_capacity(n);
            for i in 0..n {
                let beg  = offsets[i] as usize;
                let stop = if i + 1 < n { offsets[i + 1] as usize } else { bytes.len() };
                if beg > stop || stop > bytes.len() {
                    return Err(PsszError("pssz: vector element offset out of range"));
                }
                out.push(<T as PsszUnpack<F>>::pssz_unpack(&bytes[beg .. stop])?);
            }
            Ok(out)
        }
    }
}

// ── Option<T> ───────────────────────────────────────────────────────────────
// Needs Union selector iff inner type's MIN_ENCODED_SIZE == 0.

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for Option<T> {
    const MIN_ENCODED_SIZE: usize = 0;
    // Option max = (selector_byte iff inner min == 0) + max<T>.
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => Some((if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 }) + m),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize {
        let selector = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
        match self { Some(v) => selector + v.pssz_size(), None => selector }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        if T::MIN_ENCODED_SIZE == 0 {
            out.push(if self.is_some() { 1 } else { 0 });
            if let Some(v) = self { v.pssz_pack(out); }
        } else if let Some(v) = self {
            v.pssz_pack(out);
        }
    }
}

// ── Box<[T; N]> — heap-allocated fixed array (stack-safe for big N) ────────
//
// Mirrors the SSZ side: randao_mixes = 2 MiB inline would blow the default
// 8 MiB thread stack if materialized by value. Box<[T; N]> decodes
// directly into a heap allocation, no intermediate stack copy.

impl<F: PsszFormat, T: PsszPack<F>, const N: usize> PsszPack<F> for Box<[T; N]> {
    const MIN_ENCODED_SIZE: usize         = <[T; N] as PsszPack<F>>::MIN_ENCODED_SIZE;
    const MAX_ENCODED_SIZE: Option<usize> = <[T; N] as PsszPack<F>>::MAX_ENCODED_SIZE;
    const IS_FIXED_SIZE:    bool          = <[T; N] as PsszPack<F>>::IS_FIXED_SIZE;
    const FIXED_SIZE:       usize         = <[T; N] as PsszPack<F>>::FIXED_SIZE;
    fn pssz_size(&self) -> usize { (**self).pssz_size() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { (**self).pssz_pack(out) }
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F>
    for Box<[T; N]>
{
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if !T::IS_FIXED_SIZE {
            // Variable-element path — fall back to the [T;N] decoder which
            // still materializes on the stack. Not used by BeaconState
            // (all boxed arrays are fixed-element).
            return Ok(Box::new(<[T; N] as PsszUnpack<F>>::pssz_unpack(bytes)?));
        }
        let es = T::FIXED_SIZE;
        if bytes.len() != N * es {
            return Err(PsszError("pssz: boxed array span mismatch"));
        }
        let layout = std::alloc::Layout::new::<[T; N]>();
        // SAFETY: layout is non-zero; pointer is either boxed back or freed.
        unsafe {
            let raw = std::alloc::alloc(layout) as *mut [T; N];
            if raw.is_null() { std::alloc::handle_alloc_error(layout); }

            #[cfg(target_endian = "little")]
            if T::FIXED_SIZE == std::mem::size_of::<T>() {
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr(), raw as *mut u8, N * es);
                return Ok(Box::from_raw(raw));
            }
            let elem_ptr = raw as *mut T;
            for i in 0..N {
                match <T as PsszUnpack<F>>::pssz_unpack(&bytes[i * es .. (i + 1) * es]) {
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

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Option<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if T::MIN_ENCODED_SIZE == 0 {
            if bytes.is_empty() { return Err(PsszError("pssz: optional selector missing")); }
            match bytes[0] {
                0 => {
                    if bytes.len() != 1 { return Err(PsszError("pssz: optional None must be empty")); }
                    Ok(None)
                }
                1 => Ok(Some(<T as PsszUnpack<F>>::pssz_unpack(&bytes[1..])?)),
                _ => Err(PsszError("pssz: invalid optional selector")),
            }
        } else if bytes.is_empty() {
            Ok(None)
        } else {
            Ok(Some(<T as PsszUnpack<F>>::pssz_unpack(bytes)?))
        }
    }
}

// ── Offset width helpers ────────────────────────────────────────────────────

fn write_offset<F: PsszFormat>(dst: &mut [u8], value: usize) {
    // Store `value` as little-endian into `dst` which is exactly
    // F::OFFSET_BYTES long (1, 2, or 4 bytes).
    debug_assert_eq!(dst.len(), F::OFFSET_BYTES);
    let v = u32::try_from(value).expect("pssz: offset exceeds u32");
    match F::OFFSET_BYTES {
        1 => dst[0] = v as u8,
        2 => dst.copy_from_slice(&(v as u16).to_le_bytes()),
        4 => dst.copy_from_slice(&v.to_le_bytes()),
        _ => unreachable!(),
    }
}

fn read_offset<F: PsszFormat>(src: &[u8]) -> u32 {
    debug_assert_eq!(src.len(), F::OFFSET_BYTES);
    match F::OFFSET_BYTES {
        1 => src[0] as u32,
        2 => u16::from_le_bytes([src[0], src[1]]) as u32,
        4 => u32::from_le_bytes([src[0], src[1], src[2], src[3]]),
        _ => unreachable!(),
    }
}

// ── Convenience top-level API ───────────────────────────────────────────────

pub fn to_pssz<F: PsszFormat, T: PsszPack<F>>(value: &T) -> Vec<u8> {
    let mut out = Vec::with_capacity(value.pssz_size());
    value.pssz_pack(&mut out);
    out
}

pub fn from_pssz<F: PsszFormat, T: PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<T> {
    <T as PsszUnpack<F>>::pssz_unpack(bytes)
}

/// Structural validator — walks the pSSZ tree checking offsets, selectors,
/// and bounds without materializing. Parallel to SszValidate.
pub trait PsszValidate<F: PsszFormat> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()>;
}

macro_rules! pssz_validate_primitive {
    ($($ty:ty),* $(,)?) => {$(
        impl<F: PsszFormat> PsszValidate<F> for $ty {
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                if bytes.len() < std::mem::size_of::<$ty>() {
                    return Err(PsszError("pssz validate: primitive underrun"));
                }
                Ok(())
            }
        }
    )*};
}
pssz_validate_primitive!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

impl<F: PsszFormat> PsszValidate<F> for bool {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.is_empty() { return Err(PsszError("pssz validate: bool underrun")); }
        if bytes[0] > 1 { return Err(PsszError("pssz validate: invalid bool")); }
        Ok(())
    }
}

impl<F: PsszFormat> PsszValidate<F> for Uint256 {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.len() < 32 { return Err(PsszError("pssz validate: uint256 underrun")); }
        Ok(())
    }
}

impl<F: PsszFormat> PsszValidate<F> for String {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        std::str::from_utf8(bytes).map(|_| ()).map_err(|_| PsszError("pssz validate: invalid UTF-8"))
    }
}

impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Vec<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.is_empty() { return Ok(()); }
        if T::IS_FIXED_SIZE {
            if bytes.len() % T::FIXED_SIZE != 0 {
                return Err(PsszError("pssz validate: vec span not divisible"));
            }
            let n = bytes.len() / T::FIXED_SIZE;
            for i in 0..n {
                T::pssz_validate(&bytes[i * T::FIXED_SIZE .. (i + 1) * T::FIXED_SIZE])?;
            }
            Ok(())
        } else {
            let ob = F::OFFSET_BYTES;
            if bytes.len() < ob { return Err(PsszError("pssz validate: vec short")); }
            let first = read_offset::<F>(&bytes[..ob]);
            if first as usize % ob != 0 || first as usize > bytes.len() {
                return Err(PsszError("pssz validate: bad first offset"));
            }
            let n = (first as usize) / ob;
            let mut prev: u32 = first;
            for i in 1..n {
                let off = read_offset::<F>(&bytes[i * ob .. (i + 1) * ob]);
                if off < prev || (off as usize) > bytes.len() {
                    return Err(PsszError("pssz validate: non-monotone offset"));
                }
                prev = off;
            }
            for i in 0..n {
                let beg = read_offset::<F>(&bytes[i * ob .. (i + 1) * ob]) as usize;
                let stop = if i + 1 < n {
                    read_offset::<F>(&bytes[(i + 1) * ob .. (i + 2) * ob]) as usize
                } else { bytes.len() };
                T::pssz_validate(&bytes[beg .. stop])?;
            }
            Ok(())
        }
    }
}

impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Option<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if T::MIN_ENCODED_SIZE == 0 {
            if bytes.is_empty() {
                return Err(PsszError("pssz validate: option selector missing"));
            }
            match bytes[0] {
                0 => {
                    if bytes.len() != 1 {
                        Err(PsszError("pssz validate: None must have empty payload"))
                    } else { Ok(()) }
                }
                1 => T::pssz_validate(&bytes[1..]),
                _ => Err(PsszError("pssz validate: bad selector")),
            }
        } else {
            if bytes.is_empty() { return Ok(()); }
            T::pssz_validate(bytes)
        }
    }
}

pub fn pssz_validate<F: PsszFormat, T: PsszValidate<F>>(bytes: &[u8]) -> PsszResult<()> {
    T::pssz_validate(bytes)
}

// ── Tests ──────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn primitives_round_trip() {
        let b = to_pssz::<Pssz32, _>(&0xDEADBEEFu32);
        assert_eq!(b.len(), 4);
        assert_eq!(from_pssz::<Pssz32, u32>(&b).unwrap(), 0xDEADBEEF);

        let b = to_pssz::<Pssz32, _>(&-1_234_567_890i64);
        assert_eq!(from_pssz::<Pssz32, i64>(&b).unwrap(), -1_234_567_890);

        let b = to_pssz::<Pssz32, _>(&3.14159f64);
        assert_eq!(from_pssz::<Pssz32, f64>(&b).unwrap(), 3.14159);

        let b = to_pssz::<Pssz32, _>(&true);
        assert_eq!(b, vec![1]);
        assert_eq!(from_pssz::<Pssz32, bool>(&b).unwrap(), true);
    }

    #[test]
    fn string_no_length_prefix() {
        let s = "hello".to_string();
        let b = to_pssz::<Pssz32, _>(&s);
        assert_eq!(b, b"hello");
        assert_eq!(from_pssz::<Pssz32, String>(&b).unwrap(), s);
    }

    #[test]
    fn vec_fixed_elements_flat() {
        let v: Vec<u32> = vec![1, 2, 3];
        let b = to_pssz::<Pssz32, _>(&v);
        assert_eq!(b.len(), 12);  // 3 × 4 bytes, no offsets
        assert_eq!(from_pssz::<Pssz32, Vec<u32>>(&b).unwrap(), v);
    }

    #[test]
    fn vec_variable_elements_backpatched_offsets() {
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        let b = to_pssz::<Pssz32, _>(&v);
        // 3 × 4-byte offsets = 12, then "a"+"bc"+"def" = 6 bytes → total 18.
        assert_eq!(b.len(), 18);
        assert_eq!(from_pssz::<Pssz32, Vec<String>>(&b).unwrap(), v);
    }

    #[test]
    fn option_fixed_inner_no_selector() {
        let some: Option<u32> = Some(42);
        let b = to_pssz::<Pssz32, _>(&some);
        assert_eq!(b.len(), 4);
        assert_eq!(from_pssz::<Pssz32, Option<u32>>(&b).unwrap(), some);

        let none: Option<u32> = None;
        let b = to_pssz::<Pssz32, _>(&none);
        assert_eq!(b.len(), 0);
        assert_eq!(from_pssz::<Pssz32, Option<u32>>(&b).unwrap(), None);
    }

    #[test]
    fn option_variable_inner_with_selector() {
        let some: Option<String> = Some("x".into());
        let b = to_pssz::<Pssz32, _>(&some);
        assert_eq!(b, vec![1, b'x']);
        assert_eq!(from_pssz::<Pssz32, Option<String>>(&b).unwrap(), some);

        let none: Option<String> = None;
        let b = to_pssz::<Pssz32, _>(&none);
        assert_eq!(b, vec![0]);
        assert_eq!(from_pssz::<Pssz32, Option<String>>(&b).unwrap(), None);
    }

    #[test]
    fn max_encoded_size_propagation() {
        // Primitives: fixed size.
        assert_eq!(<u32 as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(4));
        assert_eq!(<bool as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(1));
        assert_eq!(<Uint256 as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(32));

        // Unbounded types: None.
        assert_eq!(<String as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, None);
        assert_eq!(<Vec<u32> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, None);

        // Bounded wrappers: concrete max.
        assert_eq!(<BoundedString<16> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(16));
        assert_eq!(<BoundedList<u32, 8> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(32));

        // Bitlist<N>: 1 delimiter byte + ceil(N/8).
        assert_eq!(<Bitlist<16> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(3));
        assert_eq!(<Bitvector<12> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(2));

        // Option<fixed> max: 0 selector + 4 payload = 4.
        assert_eq!(<Option<u32> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(4));
        // Option<string> max: 1 selector + unbounded → None.
        assert_eq!(<Option<String> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, None);
        // Option<BoundedString<16>> max: 1 selector + 16.
        assert_eq!(<Option<BoundedString<16>> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE, Some(17));
    }

    #[test]
    fn auto_pssz_format_picks_narrowest_width() {
        // Uint256 fits in pssz8 (32 ≤ 255).
        assert_eq!(choose_pssz_format_width(<Uint256 as PsszPack<Pssz32>>::MAX_ENCODED_SIZE),
                    PsszWidth::W8);
        // BoundedList<u32, 8> max is 32 → pssz8.
        assert_eq!(choose_pssz_format_width(<BoundedList<u32, 8> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE),
                    PsszWidth::W8);
        // BoundedString<1000> max 1000 → pssz16.
        assert_eq!(choose_pssz_format_width(<BoundedString<1000> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE),
                    PsszWidth::W16);
        // Unbounded types → pssz32.
        assert_eq!(choose_pssz_format_width(<String as PsszPack<Pssz32>>::MAX_ENCODED_SIZE),
                    PsszWidth::W32);
        assert_eq!(choose_pssz_format_width(<Vec<u32> as PsszPack<Pssz32>>::MAX_ENCODED_SIZE),
                    PsszWidth::W32);
    }

    #[test]
    fn pssz16_narrower_offsets() {
        let v: Vec<String> = vec!["a".into(), "bc".into()];
        let b = to_pssz::<Pssz16, _>(&v);
        // 2 × 2-byte offsets = 4, then "a"+"bc" = 3 → total 7.
        assert_eq!(b.len(), 7);
        assert_eq!(from_pssz::<Pssz16, Vec<String>>(&b).unwrap(), v);
    }

    // ── Cross-validation with C++ pSSZ (Pssz32) ─────────────────────────────
    //
    // Fixtures below were generated by the C++ encoder
    // (libraries/psio1/cpp/include/psio/to_pssz.hpp) on 2026-04-23. If either
    // side's wire format changes, these assertions will fail and the mismatch
    // must be resolved.

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i+2], 16).unwrap()).collect()
    }

    #[test]
    fn cpp_pssz32_u32_cross() {
        let b = hex("efbeadde");  // C++ emitted for u32{0xDEADBEEF}
        assert_eq!(from_pssz::<Pssz32, u32>(&b).unwrap(), 0xDEADBEEF);
        assert_eq!(to_pssz::<Pssz32, _>(&0xDEADBEEFu32), b);
    }

    #[test]
    fn cpp_pssz32_string_cross() {
        let b = hex("68656c6c6f");  // "hello"
        assert_eq!(from_pssz::<Pssz32, String>(&b).unwrap(), "hello");
        assert_eq!(to_pssz::<Pssz32, _>(&"hello".to_string()), b);
    }

    #[test]
    fn cpp_pssz32_vec_u32_cross() {
        let b = hex("010000000200000003000000");  // vec![1u32, 2, 3]
        let v = from_pssz::<Pssz32, Vec<u32>>(&b).unwrap();
        assert_eq!(v, vec![1u32, 2, 3]);
        assert_eq!(to_pssz::<Pssz32, _>(&v), b);
    }

    #[test]
    fn cpp_pssz32_vec_string_cross() {
        // vec!["a","bc","def"] → 3 × u32 offset + "abcdef"
        let b = hex("0c0000000d0000000f000000616263646566");
        let v = from_pssz::<Pssz32, Vec<String>>(&b).unwrap();
        assert_eq!(v, vec!["a", "bc", "def"].into_iter().map(String::from).collect::<Vec<_>>());
        assert_eq!(to_pssz::<Pssz32, _>(&v), b);
    }

    #[test]
    fn cpp_pssz32_option_u32_cross() {
        // Some(42): 4 bytes, no selector
        let b = hex("2a000000");
        assert_eq!(from_pssz::<Pssz32, Option<u32>>(&b).unwrap(), Some(42u32));

        // None: 0 bytes
        let b: Vec<u8> = vec![];
        assert_eq!(from_pssz::<Pssz32, Option<u32>>(&b).unwrap(), None);
    }

    #[test]
    fn validate_primitives_and_containers() {
        use super::pssz_validate;
        assert!(pssz_validate::<Pssz32, u32>(&hex("efbeadde")).is_ok());
        assert!(pssz_validate::<Pssz32, String>(&hex("68656c6c6f")).is_ok());
        assert!(pssz_validate::<Pssz32, Vec<u32>>(
            &hex("010000000200000003000000")).is_ok());
        assert!(pssz_validate::<Pssz32, Vec<String>>(
            &hex("0c0000000d0000000f000000616263646566")).is_ok());
        // None pSSZ<fixed inner>: empty span → ok.
        assert!(pssz_validate::<Pssz32, Option<u32>>(&[]).is_ok());
        // Some pSSZ<fixed inner>: 4 bytes → ok.
        assert!(pssz_validate::<Pssz32, Option<u32>>(&hex("2a000000")).is_ok());
        // Malformed selector on Option<String> (min==0 → needs selector)
        assert!(pssz_validate::<Pssz32, Option<String>>(&hex("02")).is_err());
    }

    #[test]
    fn cpp_pssz32_option_string_cross() {
        // Some("x"): 01 + 'x'
        let b = hex("0178");
        assert_eq!(from_pssz::<Pssz32, Option<String>>(&b).unwrap(), Some("x".into()));

        // None: just selector 00
        let b = hex("00");
        assert_eq!(from_pssz::<Pssz32, Option<String>>(&b).unwrap(), None);
    }
}
