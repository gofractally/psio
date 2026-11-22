//! pSSZ (PsiSSZ) — successor to the legacy fracpack benchmark prototype.
//! Combines SSZ's implicit sizing and fracpack's extensibility. The
//! specification is `docs/pssz-spec.md`; this module is the Rust counterpart
//! of C++ `psio/pssz.hpp` and must produce and accept byte-identical
//! encodings (see `conformance/pssz-vectors.json`).
//!
//! Entry points take a format tag:
//!   - [`Pssz`] is the automatic profile, equivalent to C++ `psio::pssz`: the
//!     offset width is a property of each logical TYPE. An explicit
//!     `max_dynamic_data` cap on the type selects the width class of the cap
//!     (<=255 → 1, <=65535 → 2, else 4); without a cap the class of the
//!     inferred bound (`PsszPack::AUTO_BOUND`, C++ `max_encoded_size<T>()`)
//!     applies, and an unbounded type uses 4. Each record / vector / array
//!     owns its header and offset slots at its own width; children use theirs.
//!   - [`Pssz8`], [`Pssz16`], [`Pssz32`] are the explicit regression /
//!     compatibility tags (C++ `psio::pssz_<1|2|4>`): one width is inherited
//!     through the whole recursion.
//!
//! Wire rules (summary):
//!   - Container-relative offsets; no length prefixes (implicit sizing).
//!   - Extensible records carry a fixed-region-size header. Readers honor the
//!     writer's header: unknown trailing slots are skipped, missing trailing
//!     optionals are None. Fields appended after publication must be `Option`.
//!   - Mandatory pruning: the encoder always drops trailing None fields and
//!     every reader rejects a stored trailing None. Fixed-layout (DWNC)
//!     records have no header, are never pruned and cannot evolve.
//!   - `Option<T>` carries a 1-byte selector iff `T` can encode to zero bytes
//!     (spec 5.5). Otherwise None is empty and Some is the bare payload.
//!   - A buffer holds exactly one value: exact spans, no trailing data.
//!
//! Strings: the type carries the restriction, the wire bytes are the same.
//!   - `String` ⇔ C++ unbounded `psio::utf8_string`: UTF-8 enforced by
//!     decode and validate (a Rust `String` cannot hold anything else).
//!   - [`BoundedString<N>`] ⇔ C++ `psio::utf8_string<N>`.
//!   - [`RawString`] ⇔ C++ `std::string`: raw bytes, no UTF-8 check.
//!   - [`BoundedRawString<N>`] ⇔ C++ `std::string` with `length_bound{.max=N}`.

pub use crate::pssz_dynamic as dynamic;
pub use crate::pssz_evolves::{pssz_evolves_from, shape_evolves, shape_same, Shape, ShapeKind};
pub use crate::pssz_schema_ir as schema_ir;
pub use crate::pssz_upgrade as upgrade;
pub use psio_macros::{PsszPack, PsszUnpack, PsszView, ToPsszSchema};

use crate::pssz_checked as checked;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PsszError(pub &'static str);

impl std::fmt::Display for PsszError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(self.0) }
}
impl std::error::Error for PsszError {}

pub type PsszResult<T> = Result<T, PsszError>;

/// A pSSZ format tag.
pub trait PsszFormat {
    const OFFSET_BYTES: usize;
    const HEADER_BYTES: usize;
    const MAX_TOTAL: u64;
    /// Automatic profile: every type selects its own offset width.
    const AUTO: bool = false;
}

/// Automatic, type-derived widths. Equivalent to C++ `psio::pssz`.
pub struct Pssz;
impl PsszFormat for Pssz {
    // Widest width; used only where a type has no finite bound.
    const OFFSET_BYTES: usize = 4;
    const HEADER_BYTES: usize = 4;
    const MAX_TOTAL: u64 = 0xffff_ffff;
    const AUTO: bool = true;
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

/// Width class of a budget or bound: <=255 → 1, <=65535 → 2, else 4.
/// C++ `auto_pssz_width`.
pub const fn width_class(bound: Option<usize>) -> usize {
    match bound {
        Some(m) if m <= 0xff => 1,
        Some(m) if m <= 0xffff => 2,
        _ => 4,
    }
}

/// The width a type with this bound uses for its own header / offset slots
/// under format `F`.
pub const fn offset_width<F: PsszFormat>(bound: Option<usize>) -> usize {
    if F::AUTO { width_class(bound) } else { F::OFFSET_BYTES }
}

/// The automatic offset width of `T` (C++ `auto_pssz_width_v<T>`).
pub const fn auto_pssz_width<T: PsszPack<Pssz> + ?Sized>() -> usize {
    T::OFFSET_WIDTH
}

/// Encoding trait. `F` is the format tag.
pub trait PsszPack<F: PsszFormat> {
    /// A plain `Option`: as a trailing record field it is pruned when None
    /// and may be missing from an older writer's frame.
    const IS_OPTIONAL: bool = false;
    /// For an optional: whether it uses the selector byte (payload min == 0).
    const OPTIONAL_SELECTOR: bool = false;
    fn pssz_is_none(&self) -> bool { false }
    /// The encoding of None, for views over a pruned field.
    fn pssz_none_bytes() -> Option<&'static [u8]> { None }

    /// `min_encoded_size` — used by enclosing `Option<T>` to decide whether
    /// a 1-byte selector is needed (only zero / nonzero matters).
    const MIN_ENCODED_SIZE: usize;

    /// Tight maximum encoded size under `F` when known. Informational; NOT
    /// the bound used for width selection (see `AUTO_BOUND`).
    const MAX_ENCODED_SIZE: Option<usize>;

    /// True iff the type has a compile-time constant wire size.
    const IS_FIXED_SIZE: bool;

    /// Fixed size in bytes (only valid when `IS_FIXED_SIZE`).
    const FIXED_SIZE: usize;

    /// Explicit `maxDynamicData` cap declared on the type, if any. It is the
    /// type's lifetime width promise and caps the complete encoded size.
    const MAX_DYNAMIC_DATA: Option<usize> = None;

    /// What an enclosing type reserves for this one, and what selects this
    /// type's own width: the explicit cap when declared, otherwise the
    /// conservative bound C++ `max_encoded_size<T>()` infers with four-byte
    /// bookkeeping. `None` is unbounded. Must match C++ exactly: it is ABI.
    const AUTO_BOUND: Option<usize> = None;

    /// Width of this type's own header and offset slots under `F`.
    const OFFSET_WIDTH: usize = offset_width::<F>(Self::AUTO_BOUND);

    /// Layout descriptor for the compile-time evolution check
    /// (`pssz_evolves_from`). Hand-written codecs are opaque.
    const SHAPE: Shape = Shape::OPAQUE;

    /// Size of this value's encoding (not counting any enclosing selector).
    fn pssz_size(&self) -> usize;

    /// Append this value's encoded bytes to `out`. Panics when the value is
    /// not representable; untrusted or fallible callers use `try_encode`.
    fn pssz_pack(&self, out: &mut Vec<u8>);

    /// Fallible append. Containers with offsets or bounds override this to
    /// report an unrepresentable value; on error nothing is appended to `out`.
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        out.try_reserve(self.pssz_size()).map_err(|_| PsszError("pssz: output allocation overflow"))?;
        self.pssz_pack(out);
        Ok(())
    }
}

/// Legacy helper kept for callers that pick an explicit tag from a bound.
pub const fn choose_pssz_format_width(max: Option<usize>) -> PsszWidth {
    match width_class(max) {
        1 => PsszWidth::W8,
        2 => PsszWidth::W16,
        _ => PsszWidth::W32,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PsszWidth { W8, W16, W32 }

/// Decoding trait. `bytes` is the span belonging to exactly this value.
/// Implementations return errors on malformed input; they never panic.
pub trait PsszUnpack<F: PsszFormat>: Sized {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self>;

    fn pssz_unpack_boxed(bytes: &[u8]) -> PsszResult<Box<Self>> {
        Ok(Box::new(Self::pssz_unpack(bytes)?))
    }
    /// Value of a trailing record field the writer did not emit.
    fn pssz_missing_optional() -> PsszResult<Self> {
        Err(PsszError("pssz: required record field missing"))
    }
}

/// Structural validator — walks the tree checking everything decode checks,
/// without materializing.
pub trait PsszValidate<F: PsszFormat> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()>;
}

// ── Primitives ──────────────────────────────────────────────────────────────

macro_rules! pssz_impl_primitive_le {
    ($($ty:ty),* $(,)?) => {$(
        impl<F: PsszFormat> PsszPack<F> for $ty {
            const MIN_ENCODED_SIZE: usize         = std::mem::size_of::<$ty>();
            const MAX_ENCODED_SIZE: Option<usize> = Some(std::mem::size_of::<$ty>());
            const IS_FIXED_SIZE:    bool  = true;
            const FIXED_SIZE:       usize = std::mem::size_of::<$ty>();
            const AUTO_BOUND: Option<usize> = Some(std::mem::size_of::<$ty>());
            const SHAPE: Shape = Shape::leaf(stringify!($ty), std::mem::size_of::<$ty>(), true);
            fn pssz_size(&self) -> usize { std::mem::size_of::<$ty>() }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                out.extend_from_slice(&self.to_le_bytes());
            }
        }
        impl<F: PsszFormat> PsszUnpack<F> for $ty {
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                let _depth = checked::DecodeDepth::enter()?;
                const S: usize = std::mem::size_of::<$ty>();
                if bytes.len() != S { return Err(PsszError("pssz: primitive span mismatch")); }
                let mut buf = [0u8; S];
                buf.copy_from_slice(bytes);
                Ok(<$ty>::from_le_bytes(buf))
            }
        }
        impl<F: PsszFormat> PsszValidate<F> for $ty {
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                let _depth = checked::DecodeDepth::enter()?;
                if bytes.len() != std::mem::size_of::<$ty>() {
                    return Err(PsszError("pssz validate: primitive span mismatch"));
                }
                Ok(())
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
    const AUTO_BOUND: Option<usize> = Some(32);
    const SHAPE: Shape = Shape::leaf("u256", 32, true);
    fn pssz_size(&self) -> usize { 32 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        for l in &self.limb { out.extend_from_slice(&l.to_le_bytes()); }
    }
}
impl<F: PsszFormat> PsszUnpack<F> for Uint256 {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
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
impl<F: PsszFormat> PsszValidate<F> for Uint256 {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() != 32 { return Err(PsszError("pssz validate: uint256 span mismatch")); }
        Ok(())
    }
}

impl<F: PsszFormat> PsszPack<F> for bool {
    const MIN_ENCODED_SIZE: usize         = 1;
    const MAX_ENCODED_SIZE: Option<usize> = Some(1);
    const IS_FIXED_SIZE:    bool  = true;
    const FIXED_SIZE:       usize = 1;
    const AUTO_BOUND: Option<usize> = Some(1);
    const SHAPE: Shape = Shape::leaf("bool", 1, true);
    fn pssz_size(&self) -> usize { 1 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.push(if *self { 1 } else { 0 });
    }
}
impl<F: PsszFormat> PsszUnpack<F> for bool {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() != 1 { return Err(PsszError("pssz: bool span mismatch")); }
        match bytes[0] {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(PsszError("pssz: bool is not 0/1")),
        }
    }
}
impl<F: PsszFormat> PsszValidate<F> for bool {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() != 1 { return Err(PsszError("pssz validate: bool span mismatch")); }
        if bytes[0] > 1 { return Err(PsszError("pssz: bool is not 0/1")); }
        Ok(())
    }
}

// ── Strings ─────────────────────────────────────────────────────────────────
//
// One wire shape (the raw bytes of the span), four types. See the module
// documentation for the pairing with C++.

fn utf8(bytes: &[u8]) -> PsszResult<&str> {
    std::str::from_utf8(bytes).map_err(|_| PsszError("pssz: invalid UTF-8 in string"))
}

macro_rules! pssz_string_consts {
    ($bound:expr, $name:literal, $param:expr) => {
        const SHAPE: Shape = Shape::leaf($name, $param, false);
        const MIN_ENCODED_SIZE: usize         = 0;
        const MAX_ENCODED_SIZE: Option<usize> = $bound;
        const IS_FIXED_SIZE:    bool  = false;
        const FIXED_SIZE:       usize = 0;
    };
}

impl<F: PsszFormat> PsszPack<F> for String {
    pssz_string_consts!(None, "string", 0);
    fn pssz_size(&self) -> usize { self.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.as_bytes()); }
}
impl<F: PsszFormat> PsszPack<F> for str {
    pssz_string_consts!(None, "string", 0);
    fn pssz_size(&self) -> usize { self.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.as_bytes()); }
}
impl<F: PsszFormat> PsszUnpack<F> for String {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        utf8(bytes).map(str::to_owned)
    }
}
impl<F: PsszFormat> PsszValidate<F> for String {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        utf8(bytes).map(|_| ())
    }
}

pub use crate::ssz::{Bitlist, Bitvector, BoundedBytes, BoundedList, BoundedString};

/// C++ `psio::utf8_string<N>`: UTF-8, at most N bytes.
impl<F: PsszFormat, const N: usize> PsszPack<F> for BoundedString<N> {
    pssz_string_consts!(Some(N), "string<=", N);
    const AUTO_BOUND: Option<usize> = bounded_utf8_auto_bound(N);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: BoundedString overflow; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        if self.0.len() > N { return Err(PsszError("pssz: BoundedString overflow")); }
        out.try_reserve(self.0.len()).map_err(|_| PsszError("pssz: output allocation overflow"))?;
        out.extend_from_slice(self.0.as_bytes());
        Ok(())
    }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for BoundedString<N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() > N { return Err(PsszError("pssz: BoundedString overflow")); }
        Ok(BoundedString(utf8(bytes)?.to_owned()))
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for BoundedString<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() > N { return Err(PsszError("pssz: BoundedString overflow")); }
        utf8(bytes).map(|_| ())
    }
}

/// Raw byte string: C++ `std::string`. Same wire shape as `String` with no
/// UTF-8 requirement. The exact (strict) readers do check UTF-8, mirroring
/// C++ `validate_strict`; every other entry point passes the bytes through.
#[derive(Debug, Clone, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RawString(pub Vec<u8>);
impl RawString {
    pub fn new() -> Self { Self(Vec::new()) }
    pub fn as_bytes(&self) -> &[u8] { &self.0 }
    pub fn as_raw_str(&self) -> RawStr<'_> { RawStr(&self.0) }
    /// The text, when the bytes happen to be UTF-8.
    pub fn to_str(&self) -> PsszResult<&str> { utf8(&self.0) }
}
impl From<Vec<u8>> for RawString { fn from(v: Vec<u8>) -> Self { Self(v) } }
impl From<&[u8]> for RawString { fn from(v: &[u8]) -> Self { Self(v.to_vec()) } }
impl From<&str> for RawString { fn from(v: &str) -> Self { Self(v.as_bytes().to_vec()) } }
impl From<String> for RawString { fn from(v: String) -> Self { Self(v.into_bytes()) } }

/// Borrowed counterpart of [`RawString`]; encodes identically.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RawStr<'a>(pub &'a [u8]);
impl<'a> RawStr<'a> {
    pub fn as_bytes(&self) -> &'a [u8] { self.0 }
    pub fn to_str(&self) -> PsszResult<&'a str> { utf8(self.0) }
    pub fn to_owned(&self) -> RawString { RawString(self.0.to_vec()) }
}

fn raw_string(bytes: &[u8]) -> PsszResult<()> {
    if checked::strict_strings() { utf8(bytes)?; }
    Ok(())
}

impl<F: PsszFormat> PsszPack<F> for RawString {
    pssz_string_consts!(None, "raw_string", 0);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(&self.0); }
}
impl<F: PsszFormat> PsszPack<F> for RawStr<'_> {
    pssz_string_consts!(None, "raw_string", 0);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.0); }
}
impl<F: PsszFormat> PsszUnpack<F> for RawString {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        raw_string(bytes)?;
        let mut out = Vec::new();
        out.try_reserve(bytes.len()).map_err(|_| PsszError("pssz: string allocation overflow"))?;
        out.extend_from_slice(bytes);
        Ok(RawString(out))
    }
}
impl<F: PsszFormat> PsszValidate<F> for RawString {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        raw_string(bytes)
    }
}

/// Raw byte string of at most N bytes: C++ `std::string` member annotated
/// `length_bound{.max = N}`. The bound takes part in width inference exactly
/// as in C++. Rust also enforces it on encode, decode and validate.
#[derive(Debug, Clone, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BoundedRawString<const N: usize>(pub Vec<u8>);
impl<const N: usize> BoundedRawString<N> {
    pub fn new() -> Self { Self(Vec::new()) }
    pub fn try_from_bytes(v: Vec<u8>) -> PsszResult<Self> {
        if v.len() > N { return Err(PsszError("pssz: BoundedRawString overflow")); }
        Ok(Self(v))
    }
    pub fn as_bytes(&self) -> &[u8] { &self.0 }
}
impl<F: PsszFormat, const N: usize> PsszPack<F> for BoundedRawString<N> {
    pssz_string_consts!(Some(N), "raw_string<=", N);
    const AUTO_BOUND: Option<usize> = Some(N);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: BoundedRawString overflow; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        if self.0.len() > N { return Err(PsszError("pssz: BoundedRawString overflow")); }
        out.try_reserve(self.0.len()).map_err(|_| PsszError("pssz: output allocation overflow"))?;
        out.extend_from_slice(&self.0);
        Ok(())
    }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for BoundedRawString<N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() > N { return Err(PsszError("pssz: BoundedRawString overflow")); }
        raw_string(bytes)?;
        Ok(BoundedRawString(bytes.to_vec()))
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for BoundedRawString<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        if bytes.len() > N { return Err(PsszError("pssz: BoundedRawString overflow")); }
        raw_string(bytes)
    }
}

/// Bound C++ `max_encoded_size` assigns to `psio::utf8_string<N>`.
const fn bounded_utf8_auto_bound(n: usize) -> Option<usize> { Some(n) }

// ── References and Box: wire-transparent ───────────────────────────────────

macro_rules! pssz_forward_consts {
    ($T:ident) => {
        const IS_OPTIONAL: bool = <$T as PsszPack<F>>::IS_OPTIONAL;
        const OPTIONAL_SELECTOR: bool = <$T as PsszPack<F>>::OPTIONAL_SELECTOR;
        const MIN_ENCODED_SIZE: usize = <$T as PsszPack<F>>::MIN_ENCODED_SIZE;
        const MAX_ENCODED_SIZE: Option<usize> = <$T as PsszPack<F>>::MAX_ENCODED_SIZE;
        const IS_FIXED_SIZE: bool = <$T as PsszPack<F>>::IS_FIXED_SIZE;
        const FIXED_SIZE: usize = <$T as PsszPack<F>>::FIXED_SIZE;
        const MAX_DYNAMIC_DATA: Option<usize> = <$T as PsszPack<F>>::MAX_DYNAMIC_DATA;
        const AUTO_BOUND: Option<usize> = <$T as PsszPack<F>>::AUTO_BOUND;
        const OFFSET_WIDTH: usize = <$T as PsszPack<F>>::OFFSET_WIDTH;
        const SHAPE: Shape = <$T as PsszPack<F>>::SHAPE;
        fn pssz_none_bytes() -> Option<&'static [u8]> { <$T as PsszPack<F>>::pssz_none_bytes() }
    };
}

impl<F: PsszFormat, T: PsszPack<F> + ?Sized> PsszPack<F> for &T {
    pssz_forward_consts!(T);
    fn pssz_is_none(&self) -> bool { <T as PsszPack<F>>::pssz_is_none(self) }
    fn pssz_size(&self) -> usize { <T as PsszPack<F>>::pssz_size(self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) { <T as PsszPack<F>>::pssz_pack(self, out) }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_checked(self, out) }
}

// Box<T> pairs with C++ `psio::schema_types::Box<T>`: wire-transparent (same
// bytes, same fixedness, same minimum, no extra depth node), but, exactly as
// in C++, it contributes no inferred bound (which is also what lets a
// recursive type have a finite description) and a `Box<Option<T>>` field is
// not an omittable optional.
impl<F: PsszFormat, T: PsszPack<F> + ?Sized> PsszPack<F> for Box<T> {
    const MIN_ENCODED_SIZE: usize = <T as PsszPack<F>>::MIN_ENCODED_SIZE;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = <T as PsszPack<F>>::IS_FIXED_SIZE;
    const FIXED_SIZE: usize = <T as PsszPack<F>>::FIXED_SIZE;
    const MAX_DYNAMIC_DATA: Option<usize> = <T as PsszPack<F>>::MAX_DYNAMIC_DATA;
    const AUTO_BOUND: Option<usize> = None;
    // A box has no header or table of its own (the boxed value is encoded
    // at its own width); the width it reports is the unbounded one, as the
    // C++ trait does for `Box<T>`.
    const SHAPE: Shape = Shape { optional: false, ..<T as PsszPack<F>>::SHAPE };
    fn pssz_size(&self) -> usize { <T as PsszPack<F>>::pssz_size(self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) { <T as PsszPack<F>>::pssz_pack(self, out) }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_checked(self, out) }
}
impl<F: PsszFormat, T: PsszUnpack<F>> PsszUnpack<F> for Box<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> { T::pssz_unpack_boxed(bytes) }
}
impl<F: PsszFormat, T: PsszValidate<F> + ?Sized> PsszValidate<F> for Box<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> { T::pssz_validate(bytes) }
}

// ── Sequences: [T; N], [T], Vec<T>, BoundedList<T, N> ───────────────────────

fn sequence_size<F: PsszFormat, T: PsszPack<F>>(w: usize, values: &[T]) -> usize {
    if T::IS_FIXED_SIZE {
        values.len().checked_mul(T::FIXED_SIZE).expect("pssz: sequence size overflow")
    } else {
        let mut total = values.len().checked_mul(w).expect("pssz: sequence table overflow");
        for e in values { total = total.checked_add(e.pssz_size()).expect("pssz: sequence size overflow"); }
        total
    }
}

fn validate_sequence<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>>(
    w: usize, bytes: &[u8], count: Option<usize>,
) -> PsszResult<usize> {
    let n = checked::sequence_len::<F, T>(w, bytes, count)?;
    for i in 0..n {
        T::pssz_validate(&bytes[checked::sequence_span::<F, T>(w, bytes, n, i)?])?;
    }
    Ok(n)
}

impl<F: PsszFormat, T: PsszPack<F>, const N: usize> PsszPack<F> for [T; N] {
    const MIN_ENCODED_SIZE: usize = if T::IS_FIXED_SIZE { N.saturating_mul(T::FIXED_SIZE) } else { N };
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => N.checked_mul(if T::IS_FIXED_SIZE { m } else { m.saturating_add(<Self as PsszPack<F>>::OFFSET_WIDTH) }),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = T::IS_FIXED_SIZE;
    const FIXED_SIZE:       usize = if T::IS_FIXED_SIZE { N.saturating_mul(T::FIXED_SIZE) } else { 0 };
    const AUTO_BOUND: Option<usize> = checked::sequence_bound(N, T::AUTO_BOUND, T::IS_FIXED_SIZE);
    const SHAPE: Shape = Shape::node(ShapeKind::Array, N, <Self as PsszPack<F>>::OFFSET_WIDTH, T::IS_FIXED_SIZE, &[T::SHAPE]);
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: array encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        checked::encode_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, out, true)
    }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F> for [T; N] {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        checked::decode_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, Some(N))?
            .try_into().map_err(|_| PsszError("pssz: array length conversion failed"))
    }
    // A Vec owns each initialized T during errors and unwinding; converting
    // its boxed slice avoids raw allocation and any large inline array.
    fn pssz_unpack_boxed(bytes: &[u8]) -> PsszResult<Box<Self>> {
        let _depth = checked::DecodeDepth::enter()?;
        checked::decode_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, Some(N))?
            .into_boxed_slice().try_into().map_err(|_| PsszError("pssz: boxed array length conversion failed"))
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for [T; N] {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        validate_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, Some(N)).map(|_| ())
    }
}

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for [T] {
    const SHAPE: Shape = Shape::node(ShapeKind::List, usize::MAX, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[T::SHAPE]);
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: slice encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        checked::encode_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, out, false)
    }
}

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for Vec<T> {
    const SHAPE: Shape = Shape::node(ShapeKind::List, usize::MAX, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[T::SHAPE]);
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: vector encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        checked::encode_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, out, false)
    }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Vec<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        checked::decode_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, None)
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Vec<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        validate_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, None).map(|_| ())
    }
}

/// C++ `std::vector<T>` member annotated `length_bound{.max = N}`. The bound
/// feeds the enclosing type's inferred width; the list's own offset table is
/// that of an unbounded vector, as in C++. Rust also enforces the count.
impl<F: PsszFormat, T: PsszPack<F>, const N: usize> PsszPack<F> for BoundedList<T, N> {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => N.checked_mul(if T::IS_FIXED_SIZE { m } else { m.saturating_add(<Self as PsszPack<F>>::OFFSET_WIDTH) }),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    const AUTO_BOUND: Option<usize> = checked::sequence_bound(N, T::AUTO_BOUND, T::IS_FIXED_SIZE);
    const OFFSET_WIDTH: usize = <Vec<T> as PsszPack<F>>::OFFSET_WIDTH;
    const SHAPE: Shape = Shape::node(ShapeKind::List, N, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[T::SHAPE]);
    fn pssz_size(&self) -> usize { self.0.pssz_size() }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: BoundedList overflow; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        if self.0.len() > N { return Err(PsszError("pssz: BoundedList overflow")); }
        self.0.pssz_pack_checked(out)
    }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F> for BoundedList<T, N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
        // The count is checked before any element is allocated.
        if checked::sequence_len::<F, T>(w, bytes, None)? > N {
            return Err(PsszError("pssz: BoundedList overflow"));
        }
        Ok(BoundedList(checked::decode_sequence::<F, T>(w, bytes, None)?))
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for BoundedList<T, N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
        if checked::sequence_len::<F, T>(w, bytes, None)? > N {
            return Err(PsszError("pssz: BoundedList overflow"));
        }
        validate_sequence::<F, T>(w, bytes, None).map(|_| ())
    }
}

// ── Bit types ───────────────────────────────────────────────────────────────

impl<F: PsszFormat, const N: usize> PsszPack<F> for Bitvector<N> {
    const MIN_ENCODED_SIZE: usize         = crate::pssz_bits::byte_count(N);
    const MAX_ENCODED_SIZE: Option<usize> = Some(crate::pssz_bits::byte_count(N));
    const IS_FIXED_SIZE:    bool  = true;
    const FIXED_SIZE:       usize = crate::pssz_bits::byte_count(N);
    const SHAPE: Shape = Shape::leaf("bitvector", N, true);
    fn pssz_size(&self) -> usize { crate::pssz_bits::byte_count(N) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: invalid bitvector; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        crate::pssz_bits::validate_data(&self.bytes, N)?;
        out.try_reserve(self.bytes.len()).map_err(|_| PsszError("pssz: output allocation overflow"))?;
        out.extend_from_slice(&self.bytes);
        Ok(())
    }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for Bitvector<N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        crate::pssz_bits::validate_data(bytes, N)?;
        Ok(Bitvector { bytes: bytes.to_vec() })
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for Bitvector<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        crate::pssz_bits::validate_data(bytes, N)
    }
}

impl<F: PsszFormat, const MAX_N: usize> PsszPack<F> for Bitlist<MAX_N> {
    const MIN_ENCODED_SIZE: usize         = 1;  // always ≥ delimiter byte
    const MAX_ENCODED_SIZE: Option<usize> = Some(1 + (MAX_N + 7) / 8);
    const SHAPE: Shape = Shape::leaf("bitlist", MAX_N, false);
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { (self.bits.len() / 8) + 1 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: bitlist exceeds bound; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        if self.bits.len() > MAX_N { return Err(PsszError("pssz: bitlist count exceeds bound")); }
        out.try_reserve(self.bits.len() / 8 + 1).map_err(|_| PsszError("pssz: output allocation overflow"))?;
        use crate::ssz::SszPack;
        <Self as SszPack>::ssz_pack(self, out);  // wire-identical
        Ok(())
    }
}
impl<F: PsszFormat, const MAX_N: usize> PsszUnpack<F> for Bitlist<MAX_N> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        crate::pssz_bits::list_count(bytes, MAX_N)?;
        use crate::ssz::SszUnpack;
        <Self as SszUnpack>::ssz_unpack(bytes).map_err(|e| PsszError(e.0))
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for Bitlist<N> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        crate::pssz_bits::list_count(bytes, N).map(|_| ())
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
    const IS_OPTIONAL: bool = true;
    const OPTIONAL_SELECTOR: bool = T::MIN_ENCODED_SIZE == 0;
    fn pssz_is_none(&self) -> bool { self.is_none() }
    fn pssz_none_bytes() -> Option<&'static [u8]> { Some(if T::MIN_ENCODED_SIZE == 0 { &[0] } else { &[] }) }
    const MIN_ENCODED_SIZE: usize = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => Some((if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 }) + m),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    // C++ reserves one selector byte for every optional, whether or not
    // this payload uses one.
    const AUTO_BOUND: Option<usize> = checked::bound_add(T::AUTO_BOUND, 1);
    const SHAPE: Shape = Shape::optional(T::MIN_ENCODED_SIZE == 0, &[T::SHAPE]);
    fn pssz_size(&self) -> usize {
        let selector = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
        match self { Some(v) => selector + v.pssz_size(), None => selector }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: optional encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _depth = checked::EncodeDepth::enter()?;
        let mut result = Vec::new();
        if T::MIN_ENCODED_SIZE == 0 { result.push(if self.is_some() { 1 } else { 0 }); }
        if let Some(v) = self {
            v.pssz_pack_checked(&mut result)?;
            if T::MIN_ENCODED_SIZE != 0 && result.is_empty() {
                return Err(PsszError("pssz: optional payload encoded empty without a selector"));
            }
        }
        out.try_reserve(result.len()).map_err(|_| PsszError("pssz: output allocation overflow"))?;
        out.extend_from_slice(&result);
        Ok(())
    }
}

fn optional_payload<F: PsszFormat, T: PsszPack<F>>(bytes: &[u8]) -> PsszResult<Option<&[u8]>> {
    let payload = if T::MIN_ENCODED_SIZE == 0 {
        match bytes.split_first() {
            None => return Err(PsszError("pssz: optional selector missing")),
            Some((0, rest)) => {
                if !rest.is_empty() { return Err(PsszError("pssz: optional None has trailing bytes")); }
                return Ok(None);
            }
            Some((1, rest)) => rest,
            Some(_) => return Err(PsszError("pssz: optional selector not 0/1")),
        }
    } else if bytes.is_empty() {
        return Ok(None);
    } else {
        bytes
    };
    if T::IS_FIXED_SIZE && payload.len() != T::FIXED_SIZE {
        return Err(PsszError("pssz: optional<fixed> span mismatch"));
    }
    Ok(Some(payload))
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Option<T> {
    fn pssz_missing_optional() -> PsszResult<Self> { Ok(None) }
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let _depth = checked::DecodeDepth::enter()?;
        match optional_payload::<F, T>(bytes)? {
            Some(payload) => Ok(Some(T::pssz_unpack(payload)?)),
            None => Ok(None),
        }
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Option<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        let _depth = checked::DecodeDepth::enter()?;
        match optional_payload::<F, T>(bytes)? {
            Some(payload) => T::pssz_validate(payload),
            None => Ok(()),
        }
    }
}

// ── Tuples ──────────────────────────────────────────────────────────────────
// Positional products use the same extensible record layout as C++
// std::tuple: header, slots, optional-suffix pruning. () is a zero-field
// record (a bare header), not an empty span.

/// C++ `max_encoded_size<std::tuple<...>>()`.
#[doc(hidden)]
pub const fn tuple_bound(_fields: &[(Option<usize>, bool)]) -> Option<usize> { None }

macro_rules! pssz_tuple_fields {
    (@each $all:tt;) => {};
    (@each $all:tt; ($selected:ident, $index:tt) $(, ($rest:ident, $other:tt))*) => {
        pssz_tuple_fields!(@one $all; $selected, $index);
        pssz_tuple_fields!(@each $all; $(($rest, $other)),*);
    };
    (@one [$(($ty:ident, $position:tt)),*]; $selected:ident, $index:tt) => {
        impl<F: PsszFormat, $($ty: PsszPack<F>),*> crate::pssz_view::PsszField<F, $index> for ($($ty,)*) {
            type Field = $selected;
            const LOCATION: checked::FieldLocation = checked::record_location(
                <Self as PsszPack<F>>::OFFSET_WIDTH,
                &[$(checked::Layout::of::<F, $ty>()),*], $index);
        }
    };
}
macro_rules! pssz_tuple {
    ($(($ty:ident, $index:tt)),* $(,)?) => {
        impl<F: PsszFormat, $($ty: PsszPack<F>),*> PsszPack<F> for ($($ty,)*) {
            const MIN_ENCODED_SIZE: usize = 1;  // the header
            const MAX_ENCODED_SIZE: Option<usize> = None;
            const IS_FIXED_SIZE: bool = false;
            const FIXED_SIZE: usize = 0;
            const AUTO_BOUND: Option<usize> = tuple_bound(&[$((<$ty as PsszPack<F>>::AUTO_BOUND, <$ty as PsszPack<F>>::IS_FIXED_SIZE)),*]);
            const SHAPE: Shape = Shape::record(true, false, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[$(<$ty as PsszPack<F>>::SHAPE),*]);
            fn pssz_size(&self) -> usize {
                let mut out = Vec::new();
                <Self as PsszPack<F>>::pssz_pack_checked(self, &mut out).expect("pssz: tuple encode failed; use try_to_pssz");
                out.len()
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) { <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: tuple encode failed; use try_to_pssz"); }
            #[allow(unused_variables)]
            fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
                let _depth = checked::EncodeDepth::enter()?;
                let keep = checked::record_kept_fields(&[
                    $(<$ty as PsszPack<F>>::IS_OPTIONAL && <$ty as PsszPack<F>>::pssz_is_none(&self.$index)),*
                ], false);
                checked::finish_record::<F>(<Self as PsszPack<F>>::OFFSET_WIDTH, None, &[
                    $(checked::field_if_kept::<F, $ty>(&self.$index, $index < keep)?),*
                ], false, out)
            }
        }
        impl<F: PsszFormat, $($ty: PsszPack<F> + PsszUnpack<F>),*> PsszUnpack<F> for ($($ty,)*) {
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                let _depth = checked::DecodeDepth::enter()?;
                let spans = checked::record_spans_n(<Self as PsszPack<F>>::OFFSET_WIDTH, [
                    $(checked::Layout::of::<F, $ty>()),*
                ], bytes, false)?;
                let _ = &spans;
                Ok(($(checked::decode_field::<F, $ty>(bytes, spans[$index].clone())?,)*))
            }
        }
        impl<F: PsszFormat, $($ty: PsszPack<F> + PsszValidate<F>),*> PsszValidate<F> for ($($ty,)*) {
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                let _depth = checked::DecodeDepth::enter()?;
                let spans = checked::record_spans_n(<Self as PsszPack<F>>::OFFSET_WIDTH, [
                    $(checked::Layout::of::<F, $ty>()),*
                ], bytes, false)?;
                let _ = &spans;
                $(if let Some(span) = &spans[$index] { <$ty as PsszValidate<F>>::pssz_validate(&bytes[span.clone()])?; })*
                Ok(())
            }
        }
        pssz_tuple_fields!(@each [$(($ty, $index)),*]; $(($ty, $index)),*);
    };
}
pssz_tuple!();
pssz_tuple!((A, 0));
pssz_tuple!((A, 0), (B, 1));
pssz_tuple!((A, 0), (B, 1), (C, 2));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5), (H, 6));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5), (H, 6), (I, 7));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5), (H, 6), (I, 7), (J, 8));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5), (H, 6), (I, 7), (J, 8), (K, 9));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5), (H, 6), (I, 7), (J, 8), (K, 9), (L, 10));
pssz_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (G, 5), (H, 6), (I, 7), (J, 8), (K, 9), (L, 10), (M, 11));

// ── Nested values, maps and sets ────────────────────────────────────────────

#[path = "pssz_nested.rs"]
mod nested;
pub use nested::{Checked, Deferred, Nested, NestedValidation};

#[path = "pssz_assoc.rs"]
mod assoc;
pub use assoc::{BoundedMap, BoundedSet};
pub use crate::key::ByKey;

// ── Top-level API ───────────────────────────────────────────────────────────
//
// Two layers, as in C++ (docs/helpers.md):
//
//   - The helper names `to_pssz` / `try_to_pssz` / `to_pssz_into` /
//     `from_pssz` / `from_pssz_trusted` / `validate_pssz` / `pssz_size` use
//     the automatic profile [`Pssz`] (C++ `psio::to_pssz` ... over the tag
//     `psio::pssz`). They are what most code calls.
//   - The tag-generic functions `encode` / `try_encode` / `encode_into` /
//     `decode` / `decode_strict` / `decode_checked` / `validate` /
//     `validate_strict` / `validate_compatible` / `size_of` take the format
//     tag as their first type argument (C++ `psio::encode(fmt, v)` ...), for
//     the explicit-width tags and for code generic over the tag.

/// `psio::encode(fmt, v)`: the encoding of `value` under `F`. Panics when the
/// value is not representable (an offset, bound or cap overflow); use
/// [`try_encode`] where that can depend on data.
pub fn encode<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> Vec<u8> {
    try_encode::<F, T>(value).expect("pssz: encode failed; use try_encode / try_to_pssz")
}

/// Checked encode. Offset-width, bound, cap and depth failures are errors.
pub fn try_encode<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> PsszResult<Vec<u8>> {
    let mut out = Vec::new();
    encode_into::<F, T>(value, &mut out)?;
    Ok(out)
}

/// `psio::encode(fmt, v, sink)`: appends the encoding of `value` to `out`.
/// On error `out` is left as it was.
pub fn encode_into<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T, out: &mut Vec<u8>) -> PsszResult<()> {
    checked::format::<F>()?;
    let start = out.len();
    let result = value
        .pssz_pack_checked(out)
        .and_then(|()| checked::check_cap(T::MAX_DYNAMIC_DATA, out.len() - start));
    if result.is_err() {
        out.truncate(start);
    }
    result
}

/// `psio::size_of(fmt, v)`: the size of the encoding of `value`.
pub fn size_of<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> usize {
    value.pssz_size()
}

fn enter<F: PsszFormat>(cap: Option<usize>, bytes: &[u8]) -> PsszResult<()> {
    checked::format::<F>()?;
    checked::check_cap(cap, bytes.len())
}

/// `psio::validate` + `psio::decode`: decode exactly one value from `bytes`.
/// Unknown optional suffix fields written by a newer schema are skipped. Use
/// [`decode_checked`] to learn whether any were present and
/// [`decode_strict`] to reject them. The decoder checks as it reads, so the
/// input is read once.
pub fn decode<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<T> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let _session = checked::DecodeSession::enter(true);
    T::pssz_unpack(bytes)
}

/// Exact-schema decode: additionally rejects unknown fields and, like C++
/// `validate_strict`, non-UTF-8 bytes in raw strings.
pub fn decode_strict<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<T> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let session = checked::DecodeSession::enter(false);
    let value = T::pssz_unpack(bytes)?;
    if session.has_unknown() { return Err(PsszError("pssz: unknown fields rejected by exact reader")); }
    Ok(value)
}

#[derive(Debug, Clone, PartialEq)]
pub struct Decoded<T> { pub value: T, pub has_unknown: bool }
impl<T> Decoded<T> {
    /// Refuse a lossy typed rewrite when an older reader skipped new fields.
    pub fn into_known(self) -> PsszResult<T> {
        if self.has_unknown { Err(PsszError("pssz: unknown fields would be lost by typed rewrite")) }
        else { Ok(self.value) }
    }
}

/// Compatible decode that reports whether unknown optional suffix fields were
/// present anywhere in the tree (C++ `decode_pssz_checked`).
pub fn decode_checked<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<Decoded<T>> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let session = checked::DecodeSession::enter(true);
    let value = T::pssz_unpack(bytes)?;
    Ok(Decoded { value, has_unknown: session.has_unknown() })
}

/// Exact schema and bytes identical to the canonical re-encoding.
pub fn validate_canonical_for_profile<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<()> {
    let value = decode_strict::<F, T>(bytes)?;
    if try_encode::<F, T>(&value)? != bytes { return Err(PsszError("pssz: noncanonical encoding")); }
    Ok(())
}

/// Structural validation (C++ `psio::validate`): accepts unknown optional
/// suffix fields; rejects everything decode rejects, including unpruned
/// records.
pub fn validate<F: PsszFormat, T: PsszPack<F> + PsszValidate<F> + ?Sized>(bytes: &[u8]) -> PsszResult<()> {
    validate_compatible::<F, T>(bytes).map(|_| ())
}
/// Structural validation reporting whether unknown fields were present
/// (C++ `validate_pssz_compatible`).
pub fn validate_compatible<F: PsszFormat, T: PsszPack<F> + PsszValidate<F> + ?Sized>(bytes: &[u8]) -> PsszResult<bool> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let session = checked::DecodeSession::enter(true);
    T::pssz_validate(bytes).map(|_| session.has_unknown())
}
/// Exact-schema validation (C++ `psio::validate_strict`): rejects unknown
/// fields and non-UTF-8 raw strings.
pub fn validate_strict<F: PsszFormat, T: PsszPack<F> + PsszValidate<F> + ?Sized>(bytes: &[u8]) -> PsszResult<()> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let session = checked::DecodeSession::enter(false);
    T::pssz_validate(bytes)?;
    if session.has_unknown() { return Err(PsszError("pssz: unknown fields rejected by exact reader")); }
    Ok(())
}

// ── The helper names (automatic profile) ────────────────────────────────────

/// Encodes `value` into a fresh buffer (C++ `psio::to_pssz(v)`). Panics when
/// the value is not representable; [`try_to_pssz`] returns the error.
pub fn to_pssz<T: PsszPack<Pssz> + ?Sized>(value: &T) -> Vec<u8> {
    try_encode::<Pssz, T>(value).expect("pssz: encode failed; use try_to_pssz")
}

/// [`to_pssz`] for values that may not be representable.
pub fn try_to_pssz<T: PsszPack<Pssz> + ?Sized>(value: &T) -> PsszResult<Vec<u8>> {
    try_encode::<Pssz, T>(value)
}

/// Appends the encoding of `value` to `out` (C++ `psio::to_pssz(v, sink)`).
/// On error `out` is left as it was.
pub fn to_pssz_into<T: PsszPack<Pssz> + ?Sized>(value: &T, out: &mut Vec<u8>) -> PsszResult<()> {
    encode_into::<Pssz, T>(value, out)
}

/// The checked decode (C++ `psio::from_pssz<T>(bytes)`): accepts exactly what
/// [`validate_pssz`] accepts. A frame with unknown appended optional members
/// is accepted and the unknown members are dropped.
pub fn from_pssz<T: PsszPack<Pssz> + PsszUnpack<Pssz>>(bytes: &[u8]) -> PsszResult<T> {
    decode::<Pssz, T>(bytes)
}

/// Decodes bytes the caller vouches for (C++
/// `psio::from_pssz<T>(psio::trusted{bytes})`): they were validated earlier,
/// or were produced by [`to_pssz`] and kept.
///
/// This is a safe function. The Rust decoder checks as it reads, so nothing
/// is skipped and bytes that are not a valid `T` cause a panic, never
/// undefined behaviour. A decoder that skipped the checks would have to be
/// an `unsafe fn`; none is provided.
#[track_caller]
pub fn from_pssz_trusted<T: PsszPack<Pssz> + PsszUnpack<Pssz>>(bytes: &[u8]) -> T {
    match decode::<Pssz, T>(bytes) {
        Ok(value) => value,
        Err(e) => panic!("from_pssz_trusted: the bytes are not valid: {e}"),
    }
}

/// Whether `bytes` is one `T` (C++ `psio::validate_pssz<T>(bytes)`). Never
/// panics.
pub fn validate_pssz<T: PsszPack<Pssz> + PsszValidate<Pssz> + ?Sized>(bytes: &[u8]) -> PsszResult<()> {
    validate::<Pssz, T>(bytes)
}

/// Size of the encoding of `value` (C++ `psio::pssz_size(v)`).
pub fn pssz_size<T: PsszPack<Pssz> + ?Sized>(value: &T) -> usize {
    value.pssz_size()
}

// ── The generic layer and the buffer names ──────────────────────────────────

macro_rules! format_impls {
    ($($tag:ty),*) => {$(
        impl crate::Format for $tag {
            type Error = PsszError;
        }
        impl<T: PsszPack<$tag> + ?Sized> crate::Encode<$tag> for T {
            fn encode_into(&self, out: &mut Vec<u8>) -> PsszResult<usize> {
                let start = out.len();
                encode_into::<$tag, T>(self, out)?;
                Ok(out.len() - start)
            }
            fn encoded_size(&self) -> PsszResult<usize> {
                Ok(self.pssz_size())
            }
        }
        impl<T: PsszPack<$tag> + PsszUnpack<$tag>> crate::Decode<$tag> for T {
            fn decode(bytes: &[u8]) -> PsszResult<Self> {
                decode::<$tag, T>(bytes)
            }
        }
        /// A policy that verifies canonical form selects the exact reader
        /// (`validate_strict`); any other the compatible one (`validate`).
        impl<T: PsszPack<$tag> + PsszValidate<$tag>> crate::Validate<$tag> for T {
            fn validate<P: crate::ValidationPolicy>(bytes: &[u8], policy: &P) -> PsszResult<()> {
                if policy.verify_canonical() {
                    validate_strict::<$tag, T>(bytes)
                } else {
                    validate::<$tag, T>(bytes)
                }
            }
        }
        impl<T: ?Sized> crate::ViewFormat<T> for $tag {
            type View<'a> = crate::pssz_view::PsszView<'a, T, $tag>;
            fn view(bytes: &[u8]) -> Self::View<'_> {
                crate::pssz_view::PsszView::trusted(bytes)
            }
        }
        impl<T: ?Sized> crate::MutViewFormat<T> for $tag {
            type MutView<'a> = crate::pssz_view::PsszMutView<'a, T, $tag>;
            fn view_mut(bytes: &mut [u8]) -> Self::MutView<'_> {
                crate::pssz_view::PsszMutView::trusted(bytes)
            }
        }
    )*};
}
format_impls!(Pssz, Pssz8, Pssz16, Pssz32);

/// One `T` encoded as pSSZ, owned (C++ `psio::pssz_buffer<T>`).
pub type PsszBuffer<T, F = Pssz> = crate::Buffer<T, F>;
/// One `T` encoded as pSSZ, immutable and shared through an `Arc<[u8]>`
/// (C++ `psio::shared_pssz<T>`).
pub type SharedPssz<T, F = Pssz> = crate::Shared<T, F>;
pub use crate::pssz_view::{PsszMutView, PsszView};

// ── Tests ──────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn primitives_round_trip() {
        let b = encode::<Pssz32, _>(&0xDEADBEEFu32);
        assert_eq!(b.len(), 4);
        assert_eq!(decode::<Pssz32, u32>(&b).unwrap(), 0xDEADBEEF);

        let b = encode::<Pssz32, _>(&-1_234_567_890i64);
        assert_eq!(decode::<Pssz32, i64>(&b).unwrap(), -1_234_567_890);

        let b = encode::<Pssz32, _>(&3.14159f64);
        assert_eq!(decode::<Pssz32, f64>(&b).unwrap(), 3.14159);

        let b = encode::<Pssz32, _>(&true);
        assert_eq!(b, vec![1]);
        assert_eq!(decode::<Pssz32, bool>(&b).unwrap(), true);
    }

    #[test]
    fn string_no_length_prefix() {
        let s = "hello".to_string();
        let b = encode::<Pssz32, _>(&s);
        assert_eq!(b, b"hello");
        assert_eq!(decode::<Pssz32, String>(&b).unwrap(), s);
    }

    #[test]
    fn vec_fixed_elements_flat() {
        let v: Vec<u32> = vec![1, 2, 3];
        let b = encode::<Pssz32, _>(&v);
        assert_eq!(b.len(), 12);  // 3 × 4 bytes, no offsets
        assert_eq!(decode::<Pssz32, Vec<u32>>(&b).unwrap(), v);
    }

    #[test]
    fn vec_variable_elements_backpatched_offsets() {
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        let b = encode::<Pssz32, _>(&v);
        // 3 × 4-byte offsets = 12, then "a"+"bc"+"def" = 6 bytes → total 18.
        assert_eq!(b.len(), 18);
        assert_eq!(decode::<Pssz32, Vec<String>>(&b).unwrap(), v);
    }

    #[test]
    fn option_fixed_inner_no_selector() {
        let some: Option<u32> = Some(42);
        let b = encode::<Pssz32, _>(&some);
        assert_eq!(b.len(), 4);
        assert_eq!(decode::<Pssz32, Option<u32>>(&b).unwrap(), some);

        let none: Option<u32> = None;
        let b = encode::<Pssz32, _>(&none);
        assert_eq!(b.len(), 0);
        assert_eq!(decode::<Pssz32, Option<u32>>(&b).unwrap(), None);
    }

    #[test]
    fn option_variable_inner_with_selector() {
        let some: Option<String> = Some("x".into());
        let b = encode::<Pssz32, _>(&some);
        assert_eq!(b, vec![1, b'x']);
        assert_eq!(decode::<Pssz32, Option<String>>(&b).unwrap(), some);

        let none: Option<String> = None;
        let b = encode::<Pssz32, _>(&none);
        assert_eq!(b, vec![0]);
        assert_eq!(decode::<Pssz32, Option<String>>(&b).unwrap(), None);
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
        let b = encode::<Pssz16, _>(&v);
        // 2 × 2-byte offsets = 4, then "a"+"bc" = 3 → total 7.
        assert_eq!(b.len(), 7);
        assert_eq!(decode::<Pssz16, Vec<String>>(&b).unwrap(), v);
    }

    // ── Cross-validation with C++ pSSZ (Pssz32) ─────────────────────────────
    //
    // Fixtures below were generated by the C++ encoder
    // (libraries/psio1/cpp/include/psio/encode.hpp) on 2026-04-23. If either
    // side's wire format changes, these assertions will fail and the mismatch
    // must be resolved.

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i+2], 16).unwrap()).collect()
    }

    #[test]
    fn cpp_pssz32_u32_cross() {
        let b = hex("efbeadde");  // C++ emitted for u32{0xDEADBEEF}
        assert_eq!(decode::<Pssz32, u32>(&b).unwrap(), 0xDEADBEEF);
        assert_eq!(encode::<Pssz32, _>(&0xDEADBEEFu32), b);
    }

    #[test]
    fn cpp_pssz32_string_cross() {
        let b = hex("68656c6c6f");  // "hello"
        assert_eq!(decode::<Pssz32, String>(&b).unwrap(), "hello");
        assert_eq!(encode::<Pssz32, _>(&"hello".to_string()), b);
    }

    #[test]
    fn cpp_pssz32_vec_u32_cross() {
        let b = hex("010000000200000003000000");  // vec![1u32, 2, 3]
        let v = decode::<Pssz32, Vec<u32>>(&b).unwrap();
        assert_eq!(v, vec![1u32, 2, 3]);
        assert_eq!(encode::<Pssz32, _>(&v), b);
    }

    #[test]
    fn cpp_pssz32_vec_string_cross() {
        // vec!["a","bc","def"] → 3 × u32 offset + "abcdef"
        let b = hex("0c0000000d0000000f000000616263646566");
        let v = decode::<Pssz32, Vec<String>>(&b).unwrap();
        assert_eq!(v, vec!["a", "bc", "def"].into_iter().map(String::from).collect::<Vec<_>>());
        assert_eq!(encode::<Pssz32, _>(&v), b);
    }

    #[test]
    fn cpp_pssz32_option_u32_cross() {
        // Some(42): 4 bytes, no selector
        let b = hex("2a000000");
        assert_eq!(decode::<Pssz32, Option<u32>>(&b).unwrap(), Some(42u32));

        // None: 0 bytes
        let b: Vec<u8> = vec![];
        assert_eq!(decode::<Pssz32, Option<u32>>(&b).unwrap(), None);
    }

    #[test]
    fn validate_primitives_and_containers() {
        use super::validate;
        assert!(validate::<Pssz32, u32>(&hex("efbeadde")).is_ok());
        assert!(validate::<Pssz32, String>(&hex("68656c6c6f")).is_ok());
        assert!(validate::<Pssz32, Vec<u32>>(
            &hex("010000000200000003000000")).is_ok());
        assert!(validate::<Pssz32, Vec<String>>(
            &hex("0c0000000d0000000f000000616263646566")).is_ok());
        // None pSSZ<fixed inner>: empty span → ok.
        assert!(validate::<Pssz32, Option<u32>>(&[]).is_ok());
        // Some pSSZ<fixed inner>: 4 bytes → ok.
        assert!(validate::<Pssz32, Option<u32>>(&hex("2a000000")).is_ok());
        // Malformed selector on Option<String> (min==0 → needs selector)
        assert!(validate::<Pssz32, Option<String>>(&hex("02")).is_err());
    }

    #[test]
    fn cpp_pssz32_option_string_cross() {
        // Some("x"): 01 + 'x'
        let b = hex("0178");
        assert_eq!(decode::<Pssz32, Option<String>>(&b).unwrap(), Some("x".into()));

        // None: just selector 00
        let b = hex("00");
        assert_eq!(decode::<Pssz32, Option<String>>(&b).unwrap(), None);
    }
}
