//! pSSZ (PsiSSZ) — successor to the legacy fracpack benchmark prototype.
//! Combines SSZ's implicit sizing and fracpack's
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

impl std::fmt::Display for PsszError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(self.0) }
}
impl std::error::Error for PsszError {}

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

    /// Fallible append used by untrusted protocol entry points. Containers
    /// with offsets or bounds override this to report an unrepresentable
    /// value instead of panicking; on error nothing is appended to `out`.
    /// Decoding is unaffected and stays on the safety-reviewed typed path.
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        self.pssz_pack(out);
        Ok(())
    }
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

// Shared input-layout checks. Public only so exported record macros can call
// them in downstream crates; they do not describe Rust memory layouts.
#[doc(hidden)]
pub mod input {
    use super::{PsszError, PsszFormat, PsszPack, PsszResult, PsszUnpack};
    use std::ops::Range;

    pub fn format<F: PsszFormat>() -> PsszResult<()> {
        if !matches!(F::OFFSET_BYTES, 1 | 2 | 4) || F::HEADER_BYTES != F::OFFSET_BYTES {
            return Err(PsszError("pssz: unsupported offset/header width"));
        }
        Ok(())
    }

    pub fn bitlist_len(bytes: &[u8], maximum: usize) -> PsszResult<usize> {
        let last = bytes.iter().rposition(|byte| *byte != 0)
            .ok_or(PsszError("pssz: bitlist missing delimiter"))?;
        let hi = 7 - bytes[last].leading_zeros() as usize;
        let len = last.checked_mul(8).and_then(|n| n.checked_add(hi))
            .ok_or(PsszError("pssz: bitlist length overflow"))?;
        if len > maximum { return Err(PsszError("pssz: bitlist bound exceeded")); }
        Ok(len)
    }

    pub fn offset<F: PsszFormat>(bytes: &[u8]) -> PsszResult<usize> {
        format::<F>()?;
        let b = bytes.get(..F::OFFSET_BYTES).ok_or(PsszError("pssz: offset underrun"))?;
        Ok(match F::OFFSET_BYTES {
            1 => b[0] as usize,
            2 => u16::from_le_bytes([b[0], b[1]]) as usize,
            4 => u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize,
            _ => return Err(PsszError("pssz: invalid offset width")),
        })
    }

    pub fn fixed_child<F: PsszFormat, T: PsszPack<F>>(bytes: &[u8]) -> PsszResult<()> {
        if T::IS_FIXED_SIZE && bytes.len() != T::FIXED_SIZE {
            return Err(PsszError("pssz: fixed child span mismatch"));
        }
        Ok(())
    }

    pub fn sequence_len<F: PsszFormat, T: PsszPack<F>>(
        bytes: &[u8], count: Option<usize>,
    ) -> PsszResult<usize> {
        format::<F>()?;
        if T::IS_FIXED_SIZE {
            let size = T::FIXED_SIZE;
            let n = match count {
                Some(n) => n,
                None if bytes.is_empty() => 0,
                None if size == 0 => return Err(PsszError("pssz: zero-size vector element")),
                None if bytes.len() % size == 0 => bytes.len() / size,
                None => return Err(PsszError("pssz: fixed sequence span mismatch")),
            };
            if n.checked_mul(size) != Some(bytes.len()) {
                return Err(PsszError("pssz: fixed sequence span mismatch or overflow"));
            }
            return Ok(n);
        }
        let width = F::OFFSET_BYTES;
        let n = match count {
            Some(n) => n,
            None if bytes.is_empty() => 0,
            None => {
                let first = offset::<F>(bytes)?;
                if first == 0 || first % width != 0 || first > bytes.len() {
                    return Err(PsszError("pssz: invalid sequence offset table"));
                }
                first / width
            }
        };
        let table = n.checked_mul(width).ok_or(PsszError("pssz: sequence table overflow"))?;
        if table > bytes.len() || (n == 0 && !bytes.is_empty()) {
            return Err(PsszError("pssz: sequence table mismatch"));
        }
        let mut previous = table;
        for i in 0..n {
            let begin = offset::<F>(&bytes[i * width..])?;
            if (i == 0 && begin != table) || begin < previous || begin > bytes.len() {
                return Err(PsszError("pssz: sequence offset out of range"));
            }
            previous = begin;
        }
        Ok(n)
    }

    // Callers first check sequence_len on this same byte span and count.
    pub fn sequence_span<F: PsszFormat, T: PsszPack<F>>(
        bytes: &[u8], count: usize, index: usize,
    ) -> PsszResult<Range<usize>> {
        if index >= count { return Err(PsszError("pssz: sequence index out of range")); }
        if T::IS_FIXED_SIZE {
            let begin = index.checked_mul(T::FIXED_SIZE).ok_or(PsszError("pssz: element offset overflow"))?;
            let end = begin.checked_add(T::FIXED_SIZE).ok_or(PsszError("pssz: element end overflow"))?;
            if end > bytes.len() { return Err(PsszError("pssz: element truncated")); }
            return Ok(begin..end);
        }
        format::<F>()?;
        let slot = index.checked_mul(F::OFFSET_BYTES).ok_or(PsszError("pssz: offset slot overflow"))?;
        let begin = offset::<F>(bytes.get(slot..).ok_or(PsszError("pssz: offset slot truncated"))?)?;
        let end = if index + 1 < count {
            let next = slot.checked_add(F::OFFSET_BYTES).ok_or(PsszError("pssz: next slot overflow"))?;
            offset::<F>(bytes.get(next..).ok_or(PsszError("pssz: next slot truncated"))?)?
        } else { bytes.len() };
        if begin > end || end > bytes.len() {
            return Err(PsszError("pssz: element span out of range"));
        }
        Ok(begin..end)
    }

    pub fn decode_sequence<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
        bytes: &[u8], count: Option<usize>,
    ) -> PsszResult<Vec<T>> {
        let n = sequence_len::<F, T>(bytes, count)?;
        let mut result = Vec::new();
        result.try_reserve(n).map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
        for i in 0..n {
            result.push(T::pssz_unpack(&bytes[sequence_span::<F, T>(bytes, n, i)?])?);
        }
        Ok(result)
    }

    // Some(size) is a fixed wire field; None is an offset slot. Stack arrays
    // keep generated borrowed view access free of heap allocation.
    pub fn record_spans<F: PsszFormat, const N: usize>(
        bytes: &[u8], fields: [Option<usize>; N], dwnc: bool,
    ) -> PsszResult<[Range<usize>; N]> {
        format::<F>()?;
        if dwnc && fields.iter().any(Option::is_none) {
            return Err(PsszError("pssz: DWNC record contains a variable field"));
        }
        let fixed = fields.iter().try_fold(0usize, |n, field|
            n.checked_add(field.unwrap_or(F::OFFSET_BYTES)).ok_or(PsszError("pssz: fixed region overflow")))?;
        let header = if dwnc { 0 } else { F::HEADER_BYTES };
        if !dwnc && offset::<F>(bytes)? != fixed {
            return Err(PsszError("pssz: unsupported or malformed record fixed region"));
        }
        let data = bytes.get(header..).ok_or(PsszError("pssz: record header truncated"))?;
        if fixed > data.len() { return Err(PsszError("pssz: record fixed region truncated")); }
        let mut spans = std::array::from_fn(|_| 0..0);
        let mut cursor = 0;
        let mut previous: Option<usize> = None;
        for (i, field) in fields.iter().enumerate() {
            if let Some(size) = field {
                spans[i] = header + cursor..header + cursor + size;
                cursor += size;
            } else {
                let begin = offset::<F>(&data[cursor..])?;
                if begin > data.len() || begin < fixed || (previous.is_none() && begin != fixed) {
                    return Err(PsszError("pssz: record offset out of range"));
                }
                if let Some(prev) = previous {
                    if begin < spans[prev].start - header {
                        return Err(PsszError("pssz: record offsets not monotone"));
                    }
                    spans[prev].end = header + begin;
                }
                spans[i] = header + begin..bytes.len();
                previous = Some(i);
                cursor += F::OFFSET_BYTES;
            }
        }
        if previous.is_none() && data.len() != fixed {
            return Err(PsszError("pssz: trailing fixed record bytes"));
        }
        Ok(spans)
    }
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
                if bytes.len() != S { return Err(PsszError("pssz: primitive span mismatch")); }
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
        if bytes.len() != 32 { return Err(PsszError("pssz: uint256 span mismatch")); }
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
        if bytes.len() != 1 { return Err(PsszError("pssz: bool span mismatch")); }
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
            N.checked_mul(T::FIXED_SIZE).expect("pssz: array size overflow")
        } else {
            let mut total = N.checked_mul(F::OFFSET_BYTES).expect("pssz: array table overflow");
            for e in self { total = total.checked_add(e.pssz_size()).expect("pssz: array size overflow"); }
            total
        }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        if T::IS_FIXED_SIZE {
            for e in self { e.pssz_pack(out); }
        } else {
            let ob = F::OFFSET_BYTES;
            let slot_start = out.len();
            out.resize(slot_start.checked_add(N.checked_mul(ob).expect("pssz: array table overflow")).expect("pssz: output size overflow"), 0);
            for (i, e) in self.iter().enumerate() {
                let rel = out.len() - slot_start;
                write_offset::<F>(&mut out[slot_start + i * ob .. slot_start + (i + 1) * ob], rel);
                e.pssz_pack(out);
            }
        }
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        crate::pssz_checked::encode_sequence::<F, T>(self, out)
    }
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F> for [T; N] {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        input::decode_sequence::<F, T>(bytes, Some(N))?
            .try_into().map_err(|_| PsszError("pssz: array length conversion failed"))
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
        if bytes.len() != nb { return Err(PsszError("pssz: bitvector span mismatch")); }
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
        input::bitlist_len(bytes, MAX_N)?;
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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        if self.0.len() > N { return Err(PsszError("pssz: BoundedList overflow")); }
        self.0.pssz_pack_checked(out)
    }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F>
    for BoundedList<T, N>
{
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if input::sequence_len::<F, T>(bytes, None)? > N {
            return Err(PsszError("pssz: BoundedList overflow"));
        }
        let v = <Vec<T> as PsszUnpack<F>>::pssz_unpack(bytes)?;
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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        if self.0.len() > N { return Err(PsszError("pssz: BoundedString overflow")); }
        out.extend_from_slice(self.0.as_bytes());
        Ok(())
    }
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
            assert!(self.is_empty() || T::FIXED_SIZE != 0, "pssz: nonempty zero-size vector is unsupported");
            self.len().checked_mul(T::FIXED_SIZE).expect("pssz: vector size overflow")
        } else {
            let ob = F::OFFSET_BYTES;
            let mut total = self.len().checked_mul(ob).expect("pssz: vector table overflow");
            for e in self { total = total.checked_add(e.pssz_size()).expect("pssz: vector size overflow"); }
            total
        }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        if T::IS_FIXED_SIZE {
            assert!(self.is_empty() || T::FIXED_SIZE != 0, "pssz: nonempty zero-size vector is unsupported");
            for e in self { e.pssz_pack(out); }
        } else {
            // Single-pass backpatching: reserve offset slots, then write
            // payloads while back-filling each slot with (current len −
            // slot_start) converted to the format's offset width.
            let ob = F::OFFSET_BYTES;
            let slot_start = out.len();
            out.resize(slot_start.checked_add(self.len().checked_mul(ob).expect("pssz: vector table overflow")).expect("pssz: output size overflow"), 0);
            for (i, e) in self.iter().enumerate() {
                let rel = out.len() - slot_start;
                write_offset::<F>(&mut out[slot_start + i * ob .. slot_start + (i + 1) * ob], rel);
                e.pssz_pack(out);
            }
        }
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        crate::pssz_checked::encode_sequence::<F, T>(self, out)
    }
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Vec<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        input::decode_sequence::<F, T>(bytes, None)
    }
}

// ── Option<T> ───────────────────────────────────────────────────────────────
// docs/pssz-spec.md 5.5: a selector byte iff the inner type's minimum
// encoded size is 0 (it could encode to zero bytes). Otherwise None is zero
// bytes and Some is the bare inner encoding.
//
// The option's own minimum follows from that: with a selector it is never
// empty (1); without one its None is empty (0). So Option<Option<String>>
// has no outer selector, and Option<Option<u32>> has one.

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for Option<T> {
    const MIN_ENCODED_SIZE: usize = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
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
    // Same presence rule as pssz_pack above; only the failure mode differs.
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let mut result = Vec::new();
        if T::MIN_ENCODED_SIZE == 0 { result.push(if self.is_some() { 1 } else { 0 }); }
        if let Some(v) = self { v.pssz_pack_checked(&mut result)?; }
        out.extend_from_slice(&result);
        Ok(())
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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        (**self).pssz_pack_checked(out)
    }
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F>
    for Box<[T; N]>
{
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        // Vec owns each initialized T during errors and unwinding. Converting
        // its boxed slice avoids raw allocation and any large inline array.
        input::decode_sequence::<F, T>(bytes, Some(N))?.into_boxed_slice()
            .try_into().map_err(|_| PsszError("pssz: boxed array length conversion failed"))
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
                1 => {
                    input::fixed_child::<F, T>(&bytes[1..])?;
                    Ok(Some(<T as PsszUnpack<F>>::pssz_unpack(&bytes[1..])?))
                },
                _ => Err(PsszError("pssz: invalid optional selector")),
            }
        } else if bytes.is_empty() {
            Ok(None)
        } else {
            input::fixed_child::<F, T>(bytes)?;
            Ok(Some(<T as PsszUnpack<F>>::pssz_unpack(bytes)?))
        }
    }
}

// ── Offset width helpers ────────────────────────────────────────────────────

#[doc(hidden)]
pub fn write_offset<F: PsszFormat>(dst: &mut [u8], value: usize) {
    // Store `value` as little-endian into `dst` which is exactly
    // F::OFFSET_BYTES long (1, 2, or 4 bytes).
    input::format::<F>().expect("pssz: invalid output format");
    assert_eq!(dst.len(), F::OFFSET_BYTES);
    assert!(value as u128 <= F::MAX_TOTAL as u128, "pssz: offset exceeds format limit");
    match F::OFFSET_BYTES {
        1 => dst[0] = u8::try_from(value).expect("pssz: offset exceeds u8"),
        2 => dst.copy_from_slice(&u16::try_from(value).expect("pssz: offset exceeds u16").to_le_bytes()),
        4 => dst.copy_from_slice(&u32::try_from(value).expect("pssz: offset exceeds u32").to_le_bytes()),
        _ => unreachable!(),
    }
}

// ── Convenience top-level API ───────────────────────────────────────────────

pub fn to_pssz<F: PsszFormat, T: PsszPack<F>>(value: &T) -> Vec<u8> {
    let mut out = Vec::with_capacity(value.pssz_size());
    value.pssz_pack(&mut out);
    out
}

/// Checked encode. Offset-width and bound overflows are returned as errors
/// instead of panicking as the infallible `to_pssz` convenience does.
pub fn try_to_pssz<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> PsszResult<Vec<u8>> {
    input::format::<F>()?;
    let mut out = Vec::new();
    value.pssz_pack_checked(&mut out)?;
    Ok(out)
}

pub fn from_pssz<F: PsszFormat, T: PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<T> {
    input::format::<F>()?;
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
                if bytes.len() != std::mem::size_of::<$ty>() {
                    return Err(PsszError("pssz validate: primitive span mismatch"));
                }
                Ok(())
            }
        }
    )*};
}
pssz_validate_primitive!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

impl<F: PsszFormat> PsszValidate<F> for bool {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.len() != 1 { return Err(PsszError("pssz validate: bool span mismatch")); }
        if bytes[0] > 1 { return Err(PsszError("pssz validate: invalid bool")); }
        Ok(())
    }
}

impl<F: PsszFormat> PsszValidate<F> for Uint256 {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.len() != 32 { return Err(PsszError("pssz validate: uint256 span mismatch")); }
        Ok(())
    }
}

impl<F: PsszFormat> PsszValidate<F> for String {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        std::str::from_utf8(bytes).map(|_| ()).map_err(|_| PsszError("pssz validate: invalid UTF-8"))
    }
}

fn validate_sequence<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>>(
    bytes: &[u8], count: Option<usize>,
) -> PsszResult<()> {
    let n = input::sequence_len::<F, T>(bytes, count)?;
    for i in 0..n {
        T::pssz_validate(&bytes[input::sequence_span::<F, T>(bytes, n, i)?])?;
    }
    Ok(())
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Vec<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        validate_sequence::<F, T>(bytes, None)
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for [T; N] {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        validate_sequence::<F, T>(bytes, Some(N))
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for Box<[T; N]> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <[T; N] as PsszValidate<F>>::pssz_validate(bytes)
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for BoundedList<T, N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if input::sequence_len::<F, T>(bytes, None)? > N {
            return Err(PsszError("pssz validate: BoundedList overflow"));
        }
        validate_sequence::<F, T>(bytes, None)
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for BoundedString<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.len() > N { return Err(PsszError("pssz validate: BoundedString overflow")); }
        <String as PsszValidate<F>>::pssz_validate(bytes)
    }
}

impl<F: PsszFormat, const N: usize> PsszValidate<F> for Bitvector<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let size = N.checked_add(7).ok_or(PsszError("pssz validate: bitvector size overflow"))? / 8;
        if bytes.len() != size { return Err(PsszError("pssz validate: bitvector span mismatch")); }
        Ok(())
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for Bitlist<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        input::bitlist_len(bytes, N).map(|_| ())
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
                1 => {
                    input::fixed_child::<F, T>(&bytes[1..])?;
                    T::pssz_validate(&bytes[1..])
                },
                _ => Err(PsszError("pssz validate: bad selector")),
            }
        } else {
            if bytes.is_empty() { return Ok(()); }
            input::fixed_child::<F, T>(bytes)?;
            T::pssz_validate(bytes)
        }
    }
}

pub fn pssz_validate<F: PsszFormat, T: PsszValidate<F>>(bytes: &[u8]) -> PsszResult<()> {
    input::format::<F>()?;
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
