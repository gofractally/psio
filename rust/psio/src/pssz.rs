//! pSSZ (PsiSSZ): SSZ's implicit sizing with records that can gain
//! members. The specification is `docs/pssz-spec.md`; this module is the Rust counterpart
//! of C++ `psio/pssz.hpp` and must produce and accept byte-identical
//! encodings (see `conformance/pssz-vectors.json`).
//!
//! Entry points take a format tag:
//!   - [`Pssz`] is the automatic profile, equivalent to C++ `psio::pssz`: the
//!     offset width is a property of each logical TYPE. An explicit
//!     `max_dynamic_data` cap on the type selects the width class of the cap
//!     (<=255 → 1, <=65535 → 2, else 4); without a cap the class of the
//!     inferred bound (`PsszPack::AUTO_BOUND`, C++ `max_encoded_size<T>()`)
//!     applies, and an unbounded type uses 4. Every type that has offsets —
//!     a record, a tuple, an array / list / map / set of variable elements —
//!     owns its header, slots or table at its own width (docs/pssz-spec.md
//!     3.1); children use theirs. Beneath a record with a cap, everything
//!     that is not itself a record is also bounded by that cap
//!     (docs/pssz-spec.md 3.6): see [`PsszFormat::POSITION_CAP`].
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
pub use crate::pssz_bounds as bounds;
pub use crate::pssz_schema_ir as schema_ir;
pub use crate::pssz_upgrade as upgrade;
pub use psio_macros::{PsszPack, PsszUnpack, PsszView, ToPsszSchema};

use crate::pssz_checked as checked;
use std::borrow::Cow;

/// Why bytes are not a value of the requested type, or why a value has no
/// encoding. The message is a constant (`Cow::Borrowed`, no allocation) or
/// text computed from the value (`Cow::Owned`), which is what a type's
/// invariant may return.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PsszError(pub Cow<'static, str>);

impl PsszError {
    /// An error with a constant message.
    pub const fn new(message: &'static str) -> Self {
        Self(Cow::Borrowed(message))
    }
    /// The message.
    pub fn message(&self) -> &str {
        &self.0
    }
}
impl From<&'static str> for PsszError {
    fn from(message: &'static str) -> Self {
        Self(Cow::Borrowed(message))
    }
}
impl From<String> for PsszError {
    fn from(message: String) -> Self {
        Self(Cow::Owned(message))
    }
}
impl From<Cow<'static, str>> for PsszError {
    fn from(message: Cow<'static, str>) -> Self {
        Self(message)
    }
}

impl std::fmt::Display for PsszError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&self.0) }
}
impl std::error::Error for PsszError {}

pub type PsszResult<T> = Result<T, PsszError>;

/// A pSSZ format tag.
///
/// # The position cap
///
/// A record with an explicit cap never encodes to more than the cap, so
/// nothing beneath it does: the cap is an implicit bound of every position
/// beneath the record, for the purpose of the offset widths there
/// (docs/pssz-spec.md 3.6). A width is therefore a property of a type **at a
/// position**, and the position travels where every codec already looks: in
/// the format tag. A record encodes its members under [`PsszFormat::Under`]
/// of its cap, or under [`PsszFormat::Free`] when it has none; a list, an
/// array, a map, a set, an optional, a variant, a tuple, a `Box` and a
/// transparent type hand the tag they were given to what they hold; a
/// [`Nested`] value starts again at [`Pssz`]. Nothing is copied or converted
/// for it: the tag only selects constants.
///
/// For the automatic profile the two are [`Pssz`] and [`PsszUnder<C>`]; an
/// explicit profile ignores caps, and both are the profile itself.
pub trait PsszFormat {
    const OFFSET_BYTES: usize;
    const HEADER_BYTES: usize;
    const MAX_TOTAL: u64;
    /// Automatic profile: every type selects its own offset width.
    const AUTO: bool = false;
    /// The explicit cap of the nearest enclosing record (automatic profile
    /// only): what [`position_width`] bounds a list, array, map, set or
    /// tuple by. `None` at the root, beneath a record without a cap, and
    /// inside a [`Nested`] value.
    const POSITION_CAP: Option<usize> = None;
    /// The format of the members of a record that has no cap.
    ///
    /// (The equalities in the bounds of `Free` and `Under` say that a
    /// position is the nearest record's and nothing else: beneath a record
    /// without a cap beneath any record is beneath a record without a cap.
    /// They let a generic record that holds itself be written once for
    /// every format.)
    type Free: PsszFormat<Free = Self::Free>;
    /// The format of the members of a record whose cap is `C`.
    type Under<const C: usize>: PsszFormat<Free = Self::Free, Under<C> = Self::Under<C>>;
}

/// Automatic, type-derived widths. Equivalent to C++ `psio::pssz`.
pub struct Pssz;
impl PsszFormat for Pssz {
    // Widest width; used only where a type has no finite bound.
    const OFFSET_BYTES: usize = 4;
    const HEADER_BYTES: usize = 4;
    const MAX_TOTAL: u64 = 0xffff_ffff;
    const AUTO: bool = true;
    type Free = Pssz;
    type Under<const C: usize> = PsszUnder<C>;
}

/// The automatic profile at a position beneath a record whose explicit cap
/// is `C` (docs/pssz-spec.md 3.6). The format of a view of such a member:
/// `PsszView<'_, Vec<String>, PsszUnder<255>>` reads a 1-byte offset table.
/// A record has the same bytes under `PsszUnder<C>` as under [`Pssz`].
pub struct PsszUnder<const C: usize>;
impl<const C: usize> PsszFormat for PsszUnder<C> {
    const OFFSET_BYTES: usize = 4;
    const HEADER_BYTES: usize = 4;
    const MAX_TOTAL: u64 = 0xffff_ffff;
    const AUTO: bool = true;
    const POSITION_CAP: Option<usize> = Some(C);
    type Free = Pssz;
    type Under<const D: usize> = PsszUnder<D>;
}

pub struct Pssz8;
impl PsszFormat for Pssz8 {
    const OFFSET_BYTES: usize = 1;
    const HEADER_BYTES: usize = 1;
    const MAX_TOTAL: u64 = 0xff;
    type Free = Pssz8;
    type Under<const C: usize> = Pssz8;
}

pub struct Pssz16;
impl PsszFormat for Pssz16 {
    const OFFSET_BYTES: usize = 2;
    const HEADER_BYTES: usize = 2;
    const MAX_TOTAL: u64 = 0xffff;
    type Free = Pssz16;
    type Under<const C: usize> = Pssz16;
}

pub struct Pssz32;
impl PsszFormat for Pssz32 {
    const OFFSET_BYTES: usize = 4;
    const HEADER_BYTES: usize = 4;
    const MAX_TOTAL: u64 = 0xffff_ffff;
    type Free = Pssz32;
    type Under<const C: usize> = Pssz32;
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

/// The width a record with this bound (its explicit cap, else its inferred
/// bound) uses for its own header / offset slots under format `F`. A
/// record's width is its own wherever it appears: no position cap applies.
pub const fn offset_width<F: PsszFormat>(bound: Option<usize>) -> usize {
    if F::AUTO { width_class(bound) } else { F::OFFSET_BYTES }
}

/// The width of a type with offsets that is not a record — an array, list,
/// map or set of variable elements, a tuple — whose own bound is `bound`,
/// at the position `F` describes: the width class of the smaller of its
/// bound and the position cap (docs/pssz-spec.md 3.6; C++
/// `pssz_impl::width_for<T, A>()`).
pub const fn position_width<F: PsszFormat>(bound: Option<usize>) -> usize {
    if !F::AUTO {
        return F::OFFSET_BYTES;
    }
    width_class(match (bound, F::POSITION_CAP) {
        (Some(bound), Some(cap)) => Some(if bound < cap { bound } else { cap }),
        (Some(bound), None) => Some(bound),
        (None, cap) => cap,
    })
}

/// The bound of a type that has offsets (docs/pssz-spec.md 3.2; C++
/// `max_size.hpp` `fit`): `words` words of the type's own width (header,
/// slots, table entries) and other bytes bounded by `rest`, for the
/// narrowest width whose worst case fits. The width class of the result is
/// that width. `None` when `rest` is, or on overflow.
pub const fn fit(words: usize, rest: Option<usize>) -> Option<usize> {
    let rest = match rest {
        Some(rest) => rest,
        None => return None,
    };
    if let Some(total) = words.checked_add(rest) {
        if total <= 0xff {
            return Some(total);
        }
    }
    if let Some(table) = words.checked_mul(2) {
        if let Some(total) = table.checked_add(rest) {
            if total <= 0xffff {
                return Some(total);
            }
        }
    }
    match words.checked_mul(4) {
        Some(table) => table.checked_add(rest),
        None => None,
    }
}

/// Whether `T` has an explicit cap and a record wider than the cap's width
/// class lies beneath it with no other record in between (C++
/// `psio::pssz_child_wider_than_cap_v<T>`). Such a child keeps its own
/// width (a record's bytes never depend on what holds it), so it spends
/// offset bytes on a capacity it can never use there; the remedy is a
/// `max_dynamic_data` or member bounds on the child. A query, not an error:
///
/// ```ignore
/// const _: () = assert!(!psio::pssz::pssz_child_wider_than_cap::<Transfer>());
/// ```
pub const fn pssz_child_wider_than_cap<T: PsszPack<Pssz> + ?Sized>() -> bool {
    T::CHILD_WIDER_THAN_CAP
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
    /// bound C++ `max_encoded_size<T>()` infers (docs/pssz-spec.md 3.2),
    /// which counts each type's own header / slots / table at the width that
    /// type ends up with ([`fit`]). `None` is unbounded. Must match C++
    /// exactly: it is ABI. It does not depend on the position.
    const AUTO_BOUND: Option<usize> = None;

    /// Width of this type's own header and offset slots under `F`. The
    /// default is a record's: the width class of `AUTO_BOUND`, whatever the
    /// position. A type with offsets that is not a record uses
    /// [`position_width`].
    const OFFSET_WIDTH: usize = offset_width::<F>(Self::AUTO_BOUND);

    /// The widest record (its `OFFSET_WIDTH`) that lies at or beneath this
    /// position with no other record in between; 0 when there is none. A
    /// record reports its own width, a container the widest of what it
    /// holds, a leaf, a `Nested` value and a hand-written codec 0.
    #[doc(hidden)]
    const RECORD_WIDTH_BENEATH: usize = 0;

    /// See [`pssz_child_wider_than_cap`].
    #[doc(hidden)]
    const CHILD_WIDER_THAN_CAP: bool = false;

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
        out.try_reserve(self.pssz_size()).map_err(|_| PsszError::new("pssz: output allocation overflow"))?;
        self.pssz_pack(out);
        Ok(())
    }

    /// [`pssz_pack_checked`](Self::pssz_pack_checked) for a value that is
    /// node `depth` of the tree being written (the root is node 0): what the
    /// writers in this crate and the derives call for what they hold, so the
    /// depth is an argument instead of a thread-local counter.
    ///
    /// The bytes and the errors are those of `pssz_pack_checked`. On error
    /// `out` may be longer than it was (never shorter, and nothing that was
    /// there is changed): the entry point that owns the transaction
    /// truncates. The default is for a hand-written codec: it runs
    /// `pssz_pack_checked` with the thread's counter at `depth`. A codec
    /// that implements this implements `pssz_pack_checked` as
    /// `pssz_checked::pack_checked(self, out)`.
    #[doc(hidden)]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        let _scope = checked::EncodeDepthAt::enter(depth);
        self.pssz_pack_checked(out)
    }

    /// For a fixed-size type (`IS_FIXED_SIZE`): writes the encoding of this
    /// value, node `depth`, into `slot`, which is `FIXED_SIZE` bytes inside
    /// the frame that holds it. The default encodes the value on its own and
    /// copies it.
    #[doc(hidden)]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        checked::pack_fixed_by_append::<F, Self>(self, slot, depth)
    }

    /// The bytes of `values` when they are, as they lie in memory, the
    /// concatenation of the values' encodings and encoding one cannot fail
    /// (the integers and floats of a little-endian target). A list of such
    /// values is written with one copy.
    #[doc(hidden)]
    fn pssz_bulk_bytes(values: &[Self]) -> Option<&[u8]>
    where
        Self: Sized,
    {
        let _ = values;
        None
    }

    /// The size pass: the number of bytes `pssz_pack_at_depth(out, depth)`
    /// appends for this value as node `depth`, or the error it returns. The
    /// same checks in the same order as the writer, without the bytes, so
    /// the two cannot disagree (for a format the entry points accept; an
    /// allocation failure is the writer's alone). The default is for a
    /// hand-written codec: it encodes and measures.
    #[doc(hidden)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::size_by_encoding::<F, Self>(self, depth)
    }

    /// The size pass of [`pssz_pack_fixed`](Self::pssz_pack_fixed): the
    /// checks it makes for this value in its slot, or its error.
    #[doc(hidden)]
    fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> {
        checked::size_fixed_by_size::<F, Self>(self, depth)
    }

    /// About how many bytes the encoding takes, to reserve the output once.
    /// Never trusted: any number is harmless. 0 when unknown.
    #[doc(hidden)]
    fn pssz_size_hint(&self) -> usize {
        if Self::IS_FIXED_SIZE { Self::FIXED_SIZE } else { 0 }
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

    /// [`pssz_unpack`](Self::pssz_unpack) for a value that is node `depth`
    /// of the tree being read (the root is node 0): what the readers in
    /// this crate and the derives call for what they hold, so the depth is
    /// an argument instead of a thread-local counter. The default is for a
    /// hand-written codec: it runs `pssz_unpack` with the thread's counter
    /// at `depth` (where `pssz_checked::DecodeDepth::enter` finds it). A
    /// codec that implements this implements `pssz_unpack` as
    /// `Self::pssz_unpack_at_depth(bytes, pssz_checked::decode_depth())`.
    #[doc(hidden)]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        let _scope = checked::DecodeDepthAt::enter(depth);
        Self::pssz_unpack(bytes)
    }

    fn pssz_unpack_boxed(bytes: &[u8]) -> PsszResult<Box<Self>> {
        Ok(Box::new(Self::pssz_unpack(bytes)?))
    }
    /// [`pssz_unpack_boxed`](Self::pssz_unpack_boxed) for node `depth`.
    #[doc(hidden)]
    fn pssz_unpack_boxed_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Box<Self>> {
        let _scope = checked::DecodeDepthAt::enter(depth);
        Self::pssz_unpack_boxed(bytes)
    }
    /// Whether [`pssz_unpack_bulk`](Self::pssz_unpack_bulk) and
    /// [`pssz_unpack_bulk_array`](Self::pssz_unpack_bulk_array) read this
    /// type: a fixed-size leaf of which every bit pattern is a value and
    /// whose reader checks its depth and its length and nothing else (the
    /// integers and floats).
    #[doc(hidden)]
    const BULK: bool = false;
    /// Appends the values whose encodings, one after the other, are `bytes`.
    #[doc(hidden)]
    fn pssz_unpack_bulk(bytes: &[u8], out: &mut Vec<Self>) {
        let _ = (bytes, out);
    }
    /// The `N` values whose encodings, one after the other, are `bytes`.
    #[doc(hidden)]
    fn pssz_unpack_bulk_array<const N: usize>(bytes: &[u8]) -> Option<[Self; N]> {
        let _ = bytes;
        None
    }
    /// Value of a trailing record field the writer did not emit.
    fn pssz_missing_optional() -> PsszResult<Self> {
        Err(PsszError::new("pssz: required record field missing"))
    }
    /// Reads bytes the caller vouches for: a valid encoding of `Self`
    /// (see [`from_pssz_trusted`]). An implementation may skip every check
    /// that is not needed for memory safety. On bytes that are not valid it
    /// panics or returns some value of the type; it never has undefined
    /// behaviour. The default reads with every check.
    #[track_caller]
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        checked::unpack_trusted_by_checking::<F, Self>(bytes)
    }
}

/// A trusted reader met bytes that are not what the caller vouched for.
#[doc(hidden)]
#[cold]
#[track_caller]
pub fn trusted_bytes_invalid(error: PsszError) -> ! {
    panic!("from_pssz_trusted: the bytes are not valid: {error}")
}

/// Structural validator — walks the tree checking everything decode checks,
/// without materializing.
pub trait PsszValidate<F: PsszFormat> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()>;

    /// [`pssz_validate`](Self::pssz_validate) for node `depth`: see
    /// [`PsszUnpack::pssz_unpack_at_depth`].
    #[doc(hidden)]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        let _scope = checked::DecodeDepthAt::enter(depth);
        Self::pssz_validate(bytes)
    }
}

// ── Primitives ──────────────────────────────────────────────────────────────

/// `&[$ty]` as its bytes, where those are the little-endian encodings.
#[cfg(target_endian = "little")]
macro_rules! bulk_le_bytes {
    ($ty:ty, $values:ident) => {{
        // SAFETY: `$ty` is one of the primitive integer and floating-point
        // types this macro is instantiated with below (never a type
        // parameter). Such a type has no padding and no uninitialized
        // bytes, so the `size_of_val` bytes of the slice are initialized
        // `u8`s; `u8` has alignment 1, so the pointer is aligned for it; the
        // returned slice borrows `$values`, so the memory stays valid and
        // unaliased by a writer for as long as it lives. On a little-endian
        // target those bytes are each value's `to_le_bytes`, in order.
        Some(unsafe {
            std::slice::from_raw_parts($values.as_ptr().cast::<u8>(), std::mem::size_of_val($values))
        })
    }};
}
#[cfg(not(target_endian = "little"))]
macro_rules! bulk_le_bytes {
    ($ty:ty, $values:ident) => {{
        let _ = $values;
        None
    }};
}

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
            #[inline]
            fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
                checked::encode_node(depth)?;
                checked::append(out, &self.to_le_bytes())
            }
            #[inline(always)]
            fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
                checked::encode_node(depth)?;
                slot.copy_from_slice(&self.to_le_bytes());
                Ok(())
            }
            #[inline]
            fn pssz_bulk_bytes(values: &[Self]) -> Option<&[u8]> {
                bulk_le_bytes!($ty, values)
            }
            #[inline(always)]
            fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
                checked::encode_node(depth)?;
                Ok(std::mem::size_of::<$ty>())
            }
            #[inline(always)]
            fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> {
                checked::encode_node(depth)
            }
        }
        impl<F: PsszFormat> PsszUnpack<F> for $ty {
            #[inline]
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
            }
            #[inline]
            fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
                checked::decode_node(depth)?;
                const S: usize = std::mem::size_of::<$ty>();
                if bytes.len() != S { return Err(PsszError::new("pssz: primitive span mismatch")); }
                let mut buf = [0u8; S];
                buf.copy_from_slice(bytes);
                Ok(<$ty>::from_le_bytes(buf))
            }
            const BULK: bool = true;
            #[inline]
            fn pssz_unpack_bulk(bytes: &[u8], out: &mut Vec<Self>) {
                const S: usize = std::mem::size_of::<$ty>();
                out.extend(bytes.chunks_exact(S).map(|chunk| {
                    let mut buf = [0u8; S];
                    buf.copy_from_slice(chunk);
                    <$ty>::from_le_bytes(buf)
                }));
            }
            #[inline]
            fn pssz_unpack_bulk_array<const N: usize>(bytes: &[u8]) -> Option<[Self; N]> {
                const S: usize = std::mem::size_of::<$ty>();
                if N.checked_mul(S) != Some(bytes.len()) {
                    return None;
                }
                Some(std::array::from_fn(|index| {
                    let mut buf = [0u8; S];
                    buf.copy_from_slice(&bytes[index * S..index * S + S]);
                    <$ty>::from_le_bytes(buf)
                }))
            }
            #[inline]
            fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
                const S: usize = std::mem::size_of::<$ty>();
                let mut buf = [0u8; S];
                buf.copy_from_slice(&bytes[..S]);
                <$ty>::from_le_bytes(buf)
            }
        }
        impl<F: PsszFormat> PsszValidate<F> for $ty {
            #[inline]
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
            }
            #[inline]
            fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
                checked::decode_node(depth)?;
                if bytes.len() != std::mem::size_of::<$ty>() {
                    return Err(PsszError::new("pssz validate: primitive span mismatch"));
                }
                Ok(())
            }
        }
    )*};
}

pssz_impl_primitive_le!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

// Uint256 — re-exported from ssz module since it's the same on-wire shape.
pub use crate::value_types::Uint256;

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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::reserve(out, 32)?;
        <Self as PsszPack<F>>::pssz_pack(self, out);
        Ok(())
    }
    #[inline]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        for (limb, bytes) in self.limb.iter().zip(slot.chunks_exact_mut(8)) {
            bytes.copy_from_slice(&limb.to_le_bytes());
        }
        Ok(())
    }
    #[inline(always)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        Ok(32)
    }
    #[inline(always)]
    fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)
    }
}
impl<F: PsszFormat> PsszUnpack<F> for Uint256 {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        if bytes.len() != 32 { return Err(PsszError::new("pssz: uint256 span mismatch")); }
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
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        if bytes.len() != 32 { return Err(PsszError::new("pssz validate: uint256 span mismatch")); }
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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::append(out, &[*self as u8])
    }
    #[inline(always)]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        slot.copy_from_slice(&[*self as u8]);
        Ok(())
    }
    #[inline(always)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        Ok(1)
    }
    #[inline(always)]
    fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)
    }
}
impl<F: PsszFormat> PsszUnpack<F> for bool {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        if bytes.len() != 1 { return Err(PsszError::new("pssz: bool span mismatch")); }
        match bytes[0] {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(PsszError::new("pssz: bool is not 0/1")),
        }
    }
    #[inline]
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        bytes[0] != 0
    }
}
impl<F: PsszFormat> PsszValidate<F> for bool {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        if bytes.len() != 1 { return Err(PsszError::new("pssz validate: bool span mismatch")); }
        if bytes[0] > 1 { return Err(PsszError::new("pssz: bool is not 0/1")); }
        Ok(())
    }
}

// ── Strings ─────────────────────────────────────────────────────────────────
//
// One wire shape (the raw bytes of the span), four types. See the module
// documentation for the pairing with C++.

/// `bytes` as text, when they are UTF-8: `std::str::from_utf8`, with the
/// common case (short ASCII: a name, a symbol, an address) decided by
/// `is_ascii`, which looks at a word at a time where the general validator
/// walks a short string byte by byte (1 ns against 2 to 4 for 6 to 17
/// bytes).
#[doc(hidden)]
#[inline]
pub fn utf8_str(bytes: &[u8]) -> Option<&str> {
    if bytes.is_ascii() {
        // SAFETY: `is_ascii` returned true, so every byte is below 0x80,
        // and a sequence of such bytes is valid UTF-8 (each is a one-byte
        // code point): the one requirement of `from_utf8_unchecked`.
        Some(unsafe { std::str::from_utf8_unchecked(bytes) })
    } else {
        std::str::from_utf8(bytes).ok()
    }
}

#[inline]
fn utf8(bytes: &[u8]) -> PsszResult<&str> {
    utf8_str(bytes).ok_or(PsszError::new("pssz: invalid UTF-8 in string"))
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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::append(out, self.as_bytes())
    }
    #[inline(always)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        Ok(self.len())
    }
    fn pssz_size_hint(&self) -> usize { <Self as PsszPack<F>>::pssz_size(self) }
}
impl<F: PsszFormat> PsszPack<F> for str {
    pssz_string_consts!(None, "string", 0);
    fn pssz_size(&self) -> usize { self.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.as_bytes()); }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::append(out, self.as_bytes())
    }
    #[inline(always)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        Ok(self.len())
    }
    fn pssz_size_hint(&self) -> usize { <Self as PsszPack<F>>::pssz_size(self) }
}
impl<F: PsszFormat> PsszUnpack<F> for String {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        utf8(bytes).map(str::to_owned)
    }
    /// UTF-8 is still verified: a `String` that is not UTF-8 is undefined
    /// behaviour, which no safe function may produce.
    #[inline]
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        match utf8_str(bytes) {
            Some(text) => text.to_owned(),
            None => trusted_bytes_invalid(PsszError::new("pssz: string is not UTF-8")),
        }
    }
}
impl<F: PsszFormat> PsszValidate<F> for String {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        utf8(bytes).map(|_| ())
    }
}

pub use crate::value_types::{Bitlist, Bitvector, BoundError, BoundedBytes, BoundedList, BoundedString};

/// C++ `psio::utf8_string<N>`: UTF-8, at most N bytes.
impl<F: PsszFormat, const N: usize> PsszPack<F> for BoundedString<N> {
    pssz_string_consts!(Some(N), "string<=", N);
    const AUTO_BOUND: Option<usize> = bounded_utf8_auto_bound(N);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: BoundedString overflow; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        if self.0.len() > N { return Err(PsszError::new("pssz: BoundedString overflow")); }
        checked::append(out, self.0.as_bytes())
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        if self.0.len() > N { return Err(PsszError::new("pssz: BoundedString overflow")); }
        Ok(self.0.len())
    }
    fn pssz_size_hint(&self) -> usize { self.0.len() }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for BoundedString<N> {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        if bytes.len() > N { return Err(PsszError::new("pssz: BoundedString overflow")); }
        Ok(BoundedString(utf8(bytes)?.to_owned()))
    }
    #[inline]
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        BoundedString(<String as PsszUnpack<F>>::pssz_unpack_trusted(bytes))
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for BoundedString<N> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        if bytes.len() > N { return Err(PsszError::new("pssz: BoundedString overflow")); }
        utf8(bytes).map(|_| ())
    }
}

/// Raw byte string: C++ `std::string`. Same wire shape as `String` with no
/// UTF-8 requirement: any bytes, for every reader, the exact (strict) ones
/// included.
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

/// A raw string is any bytes, for every reader: strict means exact-schema
/// and canonical, not "strings are text" (docs/pssz-spec.md 8.3).
fn raw_string(_bytes: &[u8]) -> PsszResult<()> {
    Ok(())
}

impl<F: PsszFormat> PsszPack<F> for RawString {
    pssz_string_consts!(None, "raw_string", 0);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(&self.0); }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::append(out, &self.0)
    }
    #[inline(always)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        Ok(self.0.len())
    }
    fn pssz_size_hint(&self) -> usize { <Self as PsszPack<F>>::pssz_size(self) }
}
impl<F: PsszFormat> PsszPack<F> for RawStr<'_> {
    pssz_string_consts!(None, "raw_string", 0);
    fn pssz_size(&self) -> usize { self.0.len() }
    fn pssz_pack(&self, out: &mut Vec<u8>) { out.extend_from_slice(self.0); }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::append(out, self.0)
    }
    #[inline(always)]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        Ok(self.0.len())
    }
    fn pssz_size_hint(&self) -> usize { <Self as PsszPack<F>>::pssz_size(self) }
}
impl<F: PsszFormat> PsszUnpack<F> for RawString {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        raw_string(bytes)?;
        let mut out = Vec::new();
        out.try_reserve(bytes.len()).map_err(|_| PsszError::new("pssz: string allocation overflow"))?;
        out.extend_from_slice(bytes);
        Ok(RawString(out))
    }
    #[inline]
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        RawString(bytes.to_vec())
    }
}
impl<F: PsszFormat> PsszValidate<F> for RawString {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
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
        if v.len() > N { return Err(PsszError::new("pssz: BoundedRawString overflow")); }
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
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        if self.0.len() > N { return Err(PsszError::new("pssz: BoundedRawString overflow")); }
        checked::append(out, &self.0)
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        if self.0.len() > N { return Err(PsszError::new("pssz: BoundedRawString overflow")); }
        Ok(self.0.len())
    }
    fn pssz_size_hint(&self) -> usize { self.0.len() }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for BoundedRawString<N> {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        if bytes.len() > N { return Err(PsszError::new("pssz: BoundedRawString overflow")); }
        raw_string(bytes)?;
        Ok(BoundedRawString(bytes.to_vec()))
    }
    #[inline]
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        BoundedRawString(bytes.to_vec())
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for BoundedRawString<N> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        if bytes.len() > N { return Err(PsszError::new("pssz: BoundedRawString overflow")); }
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
        const RECORD_WIDTH_BENEATH: usize = <$T as PsszPack<F>>::RECORD_WIDTH_BENEATH;
        const CHILD_WIDER_THAN_CAP: bool = <$T as PsszPack<F>>::CHILD_WIDER_THAN_CAP;
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
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_at_depth(self, out, depth) }
    #[inline]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_fixed(self, slot, depth) }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> { <T as PsszPack<F>>::pssz_size_at_depth(self, depth) }
    #[inline]
    fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> { <T as PsszPack<F>>::pssz_size_fixed(self, depth) }
    fn pssz_size_hint(&self) -> usize { <T as PsszPack<F>>::pssz_size_hint(self) }
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
    // at its own width, at this position: a box passes the position cap
    // on); the width it reports is the unbounded one, as the C++ trait does
    // for `Box<T>`.
    const RECORD_WIDTH_BENEATH: usize = <T as PsszPack<F>>::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = Shape { optional: false, ..<T as PsszPack<F>>::SHAPE };
    fn pssz_size(&self) -> usize { <T as PsszPack<F>>::pssz_size(self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) { <T as PsszPack<F>>::pssz_pack(self, out) }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_checked(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_at_depth(self, out, depth) }
    #[inline]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> { <T as PsszPack<F>>::pssz_pack_fixed(self, slot, depth) }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> { <T as PsszPack<F>>::pssz_size_at_depth(self, depth) }
    #[inline]
    fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> { <T as PsszPack<F>>::pssz_size_fixed(self, depth) }
    fn pssz_size_hint(&self) -> usize { <T as PsszPack<F>>::pssz_size_hint(self) }
}
impl<F: PsszFormat, T: PsszUnpack<F>> PsszUnpack<F> for Box<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> { T::pssz_unpack_boxed(bytes) }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> { T::pssz_unpack_boxed_at_depth(bytes, depth) }
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self { Box::new(T::pssz_unpack_trusted(bytes)) }
}
impl<F: PsszFormat, T: PsszValidate<F> + ?Sized> PsszValidate<F> for Box<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> { T::pssz_validate(bytes) }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> { T::pssz_validate_at_depth(bytes, depth) }
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

/// `pssz_size_hint` of a sequence: saturating, never a panic.
fn sequence_size_hint<F: PsszFormat, T: PsszPack<F>>(w: usize, values: &[T]) -> usize {
    if T::IS_FIXED_SIZE {
        values.len().saturating_mul(T::FIXED_SIZE)
    } else {
        values.iter().fold(values.len().saturating_mul(w), |total, e| total.saturating_add(e.pssz_size_hint()))
    }
}

/// The `n` elements of a sequence frame whose table `sequence_len`
/// accepted; they are nodes `depth`.
#[inline]
fn validate_elements<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>>(
    w: usize, bytes: &[u8], n: usize, depth: usize,
) -> PsszResult<()> {
    checked::for_each_element::<F, T>(w, bytes, n, |_, element| T::pssz_validate_at_depth(element, depth))
}
#[inline]
fn validate_sequence<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>>(
    w: usize, bytes: &[u8], count: Option<usize>, depth: usize,
) -> PsszResult<()> {
    let n = checked::sequence_len::<F, T>(w, bytes, count)?;
    validate_elements::<F, T>(w, bytes, n, depth)
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
    const OFFSET_WIDTH: usize = position_width::<F>(<Self as PsszPack<F>>::AUTO_BOUND);
    const RECORD_WIDTH_BENEATH: usize = T::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = Shape::node(ShapeKind::Array, N, <Self as PsszPack<F>>::OFFSET_WIDTH, T::IS_FIXED_SIZE, &[T::SHAPE]);
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: array encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::encode_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, out, true, depth)
    }
    #[inline]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        if !T::IS_FIXED_SIZE || T::FIXED_SIZE == 0 || slot.len() != N.saturating_mul(T::FIXED_SIZE) {
            return checked::pack_fixed_by_append::<F, Self>(self, slot, depth);
        }
        checked::encode_node(depth)?;
        if let Some(bytes) = T::pssz_bulk_bytes(self) {
            if N != 0 {
                checked::encode_node(depth + 1)?;
            }
            slot.copy_from_slice(bytes);
            return Ok(());
        }
        for (value, element) in self.iter().zip(slot.chunks_exact_mut(T::FIXED_SIZE)) {
            value.pssz_pack_fixed(element, depth + 1)?;
        }
        Ok(())
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        checked::size_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, true, depth)
    }
    #[inline]
    fn pssz_size_fixed(&self, depth: usize) -> PsszResult<()> {
        if !T::IS_FIXED_SIZE || T::FIXED_SIZE == 0 {
            return checked::size_fixed_by_size::<F, Self>(self, depth);
        }
        checked::encode_node(depth)?;
        if T::pssz_bulk_bytes(self).is_some() {
            if N != 0 {
                checked::encode_node(depth + 1)?;
            }
            return Ok(());
        }
        for value in self.iter() {
            value.pssz_size_fixed(depth + 1)?;
        }
        Ok(())
    }
    fn pssz_size_hint(&self) -> usize { sequence_size_hint::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F> for [T; N] {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        checked::decode_array_at_depth::<F, T, N>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, depth + 1)
    }
    fn pssz_unpack_boxed(bytes: &[u8]) -> PsszResult<Box<Self>> {
        <Self as PsszUnpack<F>>::pssz_unpack_boxed_at_depth(bytes, checked::decode_depth())
    }
    // A Vec owns each initialized T during errors and unwinding; converting
    // its boxed slice avoids raw allocation and any large inline array.
    fn pssz_unpack_boxed_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Box<Self>> {
        checked::decode_node(depth)?;
        checked::decode_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, Some(N), depth + 1)?
            .into_boxed_slice().try_into().map_err(|_| PsszError::new("pssz: boxed array length conversion failed"))
    }
    /// Fixed-size elements are read where they lie (a short frame is a
    /// panic); an array of anything else by the checked reader.
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        if !T::IS_FIXED_SIZE || T::FIXED_SIZE == 0 {
            return checked::unpack_trusted_by_checking::<F, Self>(bytes);
        }
        let size = T::FIXED_SIZE;
        if T::BULK {
            if let Some(array) = T::pssz_unpack_bulk_array::<N>(&bytes[..N * size]) {
                return array;
            }
        }
        std::array::from_fn(|index| T::pssz_unpack_trusted(&bytes[index * size..index * size + size]))
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for [T; N] {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        validate_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, Some(N), depth + 1)
    }
}

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for [T] {
    const OFFSET_WIDTH: usize = position_width::<F>(None);
    const RECORD_WIDTH_BENEATH: usize = T::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = Shape::node(ShapeKind::List, usize::MAX, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[T::SHAPE]);
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: slice encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::encode_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, out, false, depth)
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        checked::size_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, false, depth)
    }
    fn pssz_size_hint(&self) -> usize { sequence_size_hint::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
}

impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for Vec<T> {
    const OFFSET_WIDTH: usize = position_width::<F>(None);
    const RECORD_WIDTH_BENEATH: usize = T::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = Shape::node(ShapeKind::List, usize::MAX, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[T::SHAPE]);
    const MIN_ENCODED_SIZE: usize         = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: vector encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        checked::encode_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, out, false, depth)
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        checked::size_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self, false, depth)
    }
    fn pssz_size_hint(&self) -> usize { sequence_size_hint::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, self) }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Vec<T> {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        checked::decode_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, None, depth + 1)
    }
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        trusted_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes)
    }
}

/// The elements of a list frame the caller vouches for: no table checks
/// beyond what slicing does (a bad offset panics).
fn trusted_sequence<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>>(w: usize, bytes: &[u8]) -> Vec<T> {
    if T::IS_FIXED_SIZE {
        let size = T::FIXED_SIZE;
        if size == 0 {
            // Zero-size elements: the checked reader knows the rule.
            return match checked::decode_sequence::<F, T>(w, bytes, None) {
                Ok(items) => items,
                Err(error) => trusted_bytes_invalid(error),
            };
        }
        return bytes.chunks_exact(size).map(T::pssz_unpack_trusted).collect();
    }
    if bytes.is_empty() {
        return Vec::new();
    }
    let offset = |index: usize| -> usize {
        let mut raw = [0u8; 8];
        raw[..w].copy_from_slice(&bytes[index * w..index * w + w]);
        u64::from_le_bytes(raw) as usize
    };
    // The first offset is the size of the table. The count is clamped to
    // what the frame can hold, so garbage cannot ask for a huge allocation.
    let count = (offset(0) / w).min(bytes.len() / w);
    let mut items = Vec::with_capacity(count);
    for index in 0..count {
        let start = offset(index);
        let end = if index + 1 < count { offset(index + 1) } else { bytes.len() };
        items.push(T::pssz_unpack_trusted(&bytes[start..end]));
    }
    items
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Vec<T> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        validate_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes, None, depth + 1)
    }
}

/// C++ `psio::bounded<T, N>`, and a `std::vector<T>` member annotated
/// `max<N>`. The list's own offset table takes its width from the list's own
/// bound (docs/pssz-spec.md 3.1): 1 or 2 bytes when the count and the
/// elements are bounded and the bound is small enough, 4 otherwise.
impl<F: PsszFormat, T: PsszPack<F>, const N: usize> PsszPack<F> for BoundedList<T, N> {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = match T::MAX_ENCODED_SIZE {
        Some(m) => N.checked_mul(if T::IS_FIXED_SIZE { m } else { m.saturating_add(<Self as PsszPack<F>>::OFFSET_WIDTH) }),
        None    => None,
    };
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    const AUTO_BOUND: Option<usize> = checked::sequence_bound(N, T::AUTO_BOUND, T::IS_FIXED_SIZE);
    const OFFSET_WIDTH: usize = position_width::<F>(<Self as PsszPack<F>>::AUTO_BOUND);
    const RECORD_WIDTH_BENEATH: usize = T::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = Shape::node(ShapeKind::List, N, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[T::SHAPE]);
    fn pssz_size(&self) -> usize { sequence_size::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, &self.0) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: BoundedList overflow; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        if self.0.len() > N { return Err(PsszError::new("pssz: BoundedList overflow")); }
        checked::encode_node(depth)?;
        checked::encode_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, &self.0, out, false, depth)
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        if self.0.len() > N { return Err(PsszError::new("pssz: BoundedList overflow")); }
        checked::encode_node(depth)?;
        checked::size_sequence_at_depth::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, &self.0, false, depth)
    }
    fn pssz_size_hint(&self) -> usize { sequence_size_hint::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, &self.0) }
}
impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>, const N: usize> PsszUnpack<F> for BoundedList<T, N> {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
        // The count is checked before any element is allocated.
        let n = checked::sequence_len::<F, T>(w, bytes, None)?;
        if n > N {
            return Err(PsszError::new("pssz: BoundedList overflow"));
        }
        Ok(BoundedList(checked::decode_elements_at_depth::<F, T>(w, bytes, n, depth + 1)?))
    }
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        BoundedList(trusted_sequence::<F, T>(<Self as PsszPack<F>>::OFFSET_WIDTH, bytes))
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>, const N: usize> PsszValidate<F> for BoundedList<T, N> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
        let n = checked::sequence_len::<F, T>(w, bytes, None)?;
        if n > N {
            return Err(PsszError::new("pssz: BoundedList overflow"));
        }
        validate_elements::<F, T>(w, bytes, n, depth + 1)
    }
}

// ── Bit types ───────────────────────────────────────────────────────────────

impl<F: PsszFormat, const N: usize> PsszPack<F> for Bitvector<N> {
    const MIN_ENCODED_SIZE: usize         = crate::pssz_bits::byte_count(N);
    const MAX_ENCODED_SIZE: Option<usize> = Some(crate::pssz_bits::byte_count(N));
    const IS_FIXED_SIZE:    bool  = true;
    const FIXED_SIZE:       usize = crate::pssz_bits::byte_count(N);
    // ceil(N / 8) bytes, inline (docs/pssz-spec.md 3.2).
    const AUTO_BOUND: Option<usize> = Some(crate::pssz_bits::byte_count(N));
    const SHAPE: Shape = Shape::leaf("bitvector", N, true);
    fn pssz_size(&self) -> usize { crate::pssz_bits::byte_count(N) }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: invalid bitvector; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        crate::pssz_bits::validate_data(&self.bytes, N)?;
        checked::append(out, &self.bytes)
    }
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        crate::pssz_bits::validate_data(&self.bytes, N)?;
        if slot.len() != self.bytes.len() {
            return Err(PsszError::new("pssz: fixed encoder size mismatch"));
        }
        slot.copy_from_slice(&self.bytes);
        Ok(())
    }
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        crate::pssz_bits::validate_data(&self.bytes, N)?;
        Ok(self.bytes.len())
    }
}
impl<F: PsszFormat, const N: usize> PsszUnpack<F> for Bitvector<N> {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        crate::pssz_bits::validate_data(bytes, N)?;
        Ok(Bitvector { bytes: bytes.to_vec() })
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for Bitvector<N> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        crate::pssz_bits::validate_data(bytes, N)
    }
}

impl<F: PsszFormat, const MAX_N: usize> PsszPack<F> for Bitlist<MAX_N> {
    const MIN_ENCODED_SIZE: usize         = 1;  // always ≥ delimiter byte
    const MAX_ENCODED_SIZE: Option<usize> = Some(1 + (MAX_N + 7) / 8);
    // ceil(N / 8) + 1: the bits and the delimiter bit (docs/pssz-spec.md
    // 3.2). The bound every implementation uses, not the exact maximum.
    const AUTO_BOUND: Option<usize> = crate::pssz_bits::byte_count(MAX_N).checked_add(1);
    const SHAPE: Shape = Shape::leaf("bitlist", MAX_N, false);
    const IS_FIXED_SIZE:    bool  = false;
    const FIXED_SIZE:       usize = 0;
    fn pssz_size(&self) -> usize { (self.bits.len() / 8) + 1 }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: bitlist exceeds bound; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        if self.bits.len() > MAX_N { return Err(PsszError::new("pssz: bitlist count exceeds bound")); }
        checked::reserve(out, self.bits.len() / 8 + 1)?;
        self.pack_bits(out); // wire-identical with SSZ
        Ok(())
    }
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        if self.bits.len() > MAX_N { return Err(PsszError::new("pssz: bitlist count exceeds bound")); }
        Ok(self.bits.len() / 8 + 1)
    }
    fn pssz_size_hint(&self) -> usize { self.bits.len() / 8 + 1 }
}
impl<F: PsszFormat, const MAX_N: usize> PsszUnpack<F> for Bitlist<MAX_N> {
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        crate::pssz_bits::list_count(bytes, MAX_N)?;
        Self::unpack_bits(bytes).map_err(PsszError::new)
    }
}
impl<F: PsszFormat, const N: usize> PsszValidate<F> for Bitlist<N> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
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
    const RECORD_WIDTH_BENEATH: usize = T::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = Shape::optional(T::MIN_ENCODED_SIZE == 0, &[T::SHAPE]);
    fn pssz_size(&self) -> usize {
        let selector = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
        match self { Some(v) => selector + v.pssz_size(), None => selector }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: optional encode failed; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        if T::MIN_ENCODED_SIZE == 0 { checked::append(out, &[self.is_some() as u8])?; }
        if let Some(v) = self {
            let start = out.len();
            v.pssz_pack_at_depth(out, depth + 1)?;
            if T::MIN_ENCODED_SIZE != 0 && out.len() <= start {
                return Err(PsszError::new("pssz: optional payload encoded empty without a selector"));
            }
        }
        Ok(())
    }
    #[inline]
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        let selector = if T::MIN_ENCODED_SIZE == 0 { 1usize } else { 0 };
        match self {
            Some(v) => {
                let payload = v.pssz_size_at_depth(depth + 1)?;
                if T::MIN_ENCODED_SIZE != 0 && payload == 0 {
                    return Err(PsszError::new("pssz: optional payload encoded empty without a selector"));
                }
                Ok(payload.saturating_add(selector))
            }
            None => Ok(selector),
        }
    }
    fn pssz_size_hint(&self) -> usize {
        let selector = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
        match self { Some(v) => v.pssz_size_hint().saturating_add(selector), None => selector }
    }
}

fn optional_payload<F: PsszFormat, T: PsszPack<F>>(bytes: &[u8]) -> PsszResult<Option<&[u8]>> {
    let payload = if T::MIN_ENCODED_SIZE == 0 {
        match bytes.split_first() {
            None => return Err(PsszError::new("pssz: optional selector missing")),
            Some((0, rest)) => {
                if !rest.is_empty() { return Err(PsszError::new("pssz: optional None has trailing bytes")); }
                return Ok(None);
            }
            Some((1, rest)) => rest,
            Some(_) => return Err(PsszError::new("pssz: optional selector not 0/1")),
        }
    } else if bytes.is_empty() {
        return Ok(None);
    } else {
        bytes
    };
    if T::IS_FIXED_SIZE && payload.len() != T::FIXED_SIZE {
        return Err(PsszError::new("pssz: optional<fixed> span mismatch"));
    }
    Ok(Some(payload))
}

impl<F: PsszFormat, T: PsszUnpack<F> + PsszPack<F>> PsszUnpack<F> for Option<T> {
    fn pssz_missing_optional() -> PsszResult<Self> { Ok(None) }
    #[inline]
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        match optional_payload::<F, T>(bytes)? {
            Some(payload) => Ok(Some(T::pssz_unpack_at_depth(payload, depth + 1)?)),
            None => Ok(None),
        }
    }
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        match optional_payload::<F, T>(bytes) {
            Ok(payload) => payload.map(T::pssz_unpack_trusted),
            Err(error) => trusted_bytes_invalid(error),
        }
    }
}
impl<F: PsszFormat, T: PsszPack<F> + PsszValidate<F>> PsszValidate<F> for Option<T> {
    #[inline]
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    #[inline]
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        match optional_payload::<F, T>(bytes)? {
            Some(payload) => T::pssz_validate_at_depth(payload, depth + 1),
            None => Ok(()),
        }
    }
}

// ── Tuples ──────────────────────────────────────────────────────────────────
// Positional products use the same extensible record layout as C++
// std::tuple: header, slots, optional-suffix pruning. () is a zero-field
// record (a bare header), not an empty span.

/// C++ `max_encoded_size<std::tuple<...>>()` (docs/pssz-spec.md 3.2): a
/// tuple is the extensible positional record of its elements: the header
/// and one slot per element that is not an inline fixed payload, at the
/// tuple's own width ([`fit`]), and per element its bound (its explicit cap
/// in full when it has one). Unbounded when an element is.
#[doc(hidden)]
pub const fn tuple_bound(fields: &[(Option<usize>, bool)]) -> Option<usize> {
    checked::record_bound(fields, false)
}

/// C++ `max_encoded_size<std::variant<...>>()` (docs/pssz-spec.md 3.2): one
/// selector byte, then the widest alternative. Unbounded when an
/// alternative is.
#[doc(hidden)]
pub const fn variant_bound(alternatives: &[Option<usize>]) -> Option<usize> {
    let mut widest = 0usize;
    let mut at = 0;
    while at < alternatives.len() {
        match alternatives[at] {
            Some(bound) => {
                if bound > widest {
                    widest = bound;
                }
            }
            None => return None,
        }
        at += 1;
    }
    widest.checked_add(1)
}

macro_rules! pssz_tuple_fields {
    (@each $all:tt;) => {};
    (@each $all:tt; ($selected:ident, $index:tt) $(, ($rest:ident, $other:tt))*) => {
        pssz_tuple_fields!(@one $all; $selected, $index);
        pssz_tuple_fields!(@each $all; $(($rest, $other)),*);
    };
    (@one [$(($ty:ident, $position:tt)),*]; $selected:ident, $index:tt) => {
        impl<F: PsszFormat, $($ty: PsszPack<F>),*> crate::pssz_view::PsszField<F, $index> for ($($ty,)*) {
            type Field = $selected;
            type Beneath = F;
            const LOCATION: checked::FieldLocation = checked::record_location(
                <Self as PsszPack<F>>::OFFSET_WIDTH,
                &[$(checked::Layout::of::<F, $ty>()),*], $index);
        }
    };
}
/// The checked frame of a tuple (`pssz_checked::RecordFrame`): where each
/// element starts, which are variable, and where the last variable one ends.
macro_rules! pssz_tuple_frame {
    ($frame:ident, $starts:ident, $variable:ident, $tail:ident; $bytes:ident; $($ty:ident),*) => {
        let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
        let known = Some(0usize);
        $(let known = checked::add_slot::<F, $ty>(known, w);)*
        let mut $frame = checked::RecordFrame::begin(
            w,
            known,
            false $(|| (<$ty as PsszPack<F>>::IS_OPTIONAL && <$ty as PsszPack<F>>::IS_FIXED_SIZE))*,
            true $(&& <$ty as PsszPack<F>>::IS_FIXED_SIZE)*,
            false,
            $bytes,
        )?;
        let $starts: &[usize] = &[$($frame.field::<F, $ty>($bytes)?),*];
        let $variable: &[bool] = &[$(!<$ty as PsszPack<F>>::IS_FIXED_SIZE),*];
        let $tail = $frame.finish($bytes)?;
        let _ = (&$starts, &$variable, &$tail);
    };
}
macro_rules! pssz_tuple {
    ($(($ty:ident, $index:tt)),* $(,)?) => {
        impl<F: PsszFormat, $($ty: PsszPack<F>),*> PsszPack<F> for ($($ty,)*) {
            const MIN_ENCODED_SIZE: usize = 1;  // the header
            const MAX_ENCODED_SIZE: Option<usize> = <Self as PsszPack<F>>::AUTO_BOUND;
            const IS_FIXED_SIZE: bool = false;
            const FIXED_SIZE: usize = 0;
            const AUTO_BOUND: Option<usize> = tuple_bound(&[$((<$ty as PsszPack<F>>::AUTO_BOUND, <$ty as PsszPack<F>>::IS_FIXED_SIZE)),*]);
            // A tuple is not a record: the position cap bounds it, and it
            // hands the position to its elements.
            const OFFSET_WIDTH: usize = position_width::<F>(<Self as PsszPack<F>>::AUTO_BOUND);
            const RECORD_WIDTH_BENEATH: usize = checked::widest(&[$(<$ty as PsszPack<F>>::RECORD_WIDTH_BENEATH),*]);
            const SHAPE: Shape = Shape::record(true, false, <Self as PsszPack<F>>::OFFSET_WIDTH, false, &[$(<$ty as PsszPack<F>>::SHAPE),*]);
            fn pssz_size(&self) -> usize {
                <Self as PsszPack<F>>::pssz_size_at_depth(self, checked::encode_depth())
                    .expect("pssz: tuple encode failed; use try_to_pssz")
            }
            #[allow(unused_variables, unused_mut)]
            #[inline]
            fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
                checked::encode_node(depth)?;
                let keep = checked::record_kept_fields(&[
                    $(<$ty as PsszPack<F>>::IS_OPTIONAL && <$ty as PsszPack<F>>::pssz_is_none(&self.$index)),*
                ], false);
                let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
                let fixed = 0usize $(.saturating_add(if $index < keep { checked::slot::<F, $ty>(w) } else { 0 }))*;
                let mut record = checked::RecordSize::begin(w, false, fixed, depth);
                $(if $index < keep { record.field::<F, $ty>(&self.$index)?; })*
                record.finish::<F>(None)
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) { <Self as PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: tuple encode failed; use try_to_pssz"); }
            fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> { checked::pack_checked::<F, Self>(self, out) }
            #[allow(unused_variables, unused_mut)]
            #[inline]
            fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
                checked::encode_node(depth)?;
                let keep = checked::record_kept_fields(&[
                    $(<$ty as PsszPack<F>>::IS_OPTIONAL && <$ty as PsszPack<F>>::pssz_is_none(&self.$index)),*
                ], false);
                let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
                let fixed = 0usize $(.saturating_add(if $index < keep { checked::slot::<F, $ty>(w) } else { 0 }))*;
                let mut record = checked::RecordEncoder::begin(out, w, false, fixed, depth)?;
                $(if $index < keep { record.field::<F, $ty>(out, &self.$index)?; })*
                record.finish::<F>(out, None)
            }
            #[allow(unused_mut)]
            fn pssz_size_hint(&self) -> usize {
                let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
                let mut total = w;
                $(total = total.saturating_add(checked::field_size_hint::<F, $ty>(w, &self.$index));)*
                total
            }
        }
        impl<F: PsszFormat, $($ty: PsszPack<F> + PsszUnpack<F>),*> PsszUnpack<F> for ($($ty,)*) {
            #[inline]
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
            }
            #[inline]
            fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
                checked::decode_node(depth)?;
                pssz_tuple_frame!(frame, starts, variable, tail; bytes; $($ty),*);
                Ok(($(checked::decode_frame_field::<F, $ty>(
                    bytes, starts[$index], checked::field_end(starts, variable, $index, tail), depth + 1)?,)*))
            }
            #[allow(unused_variables, unused_mut)]
            fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
                let w = <Self as PsszPack<F>>::OFFSET_WIDTH;
                let known = 0usize $(.saturating_add(checked::slot::<F, $ty>(w)))*;
                let mut frame = checked::TrustedFrame::begin(w, known, false, bytes);
                let starts: &[usize] = &[$(frame.field::<F, $ty>(bytes)),*];
                let variable: &[bool] = &[$(!<$ty as PsszPack<F>>::IS_FIXED_SIZE),*];
                let tail = frame.finish(known, bytes);
                let _ = (&starts, &variable, &tail);
                ($(checked::trusted_frame_field::<F, $ty>(
                    bytes, starts[$index], checked::field_end(starts, variable, $index, tail)),)*)
            }
        }
        impl<F: PsszFormat, $($ty: PsszPack<F> + PsszValidate<F>),*> PsszValidate<F> for ($($ty,)*) {
            #[inline]
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
            }
            #[inline]
            fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
                checked::decode_node(depth)?;
                pssz_tuple_frame!(frame, starts, variable, tail; bytes; $($ty),*);
                $(checked::validate_frame_field::<F, $ty>(
                    bytes, starts[$index], checked::field_end(starts, variable, $index, tail), depth + 1)?;)*
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
pub(crate) mod assoc;
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
    checked::reserve_hint::<F, T>(value, out);
    let result = value
        .pssz_pack_at_depth(out, checked::encode_depth())
        .and_then(|()| checked::check_cap(T::MAX_DYNAMIC_DATA, out.len().saturating_sub(start)));
    if result.is_err() {
        out.truncate(start);
    }
    result
}

/// `psio::size_of(fmt, v)`: the size of the encoding of `value`.
pub fn size_of<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> usize {
    value.pssz_size()
}

/// The size [`try_encode`] would produce, or the error it would return: the
/// same checks, and nothing is written or allocated (a hand-written codec
/// without a size pass of its own is encoded and measured).
pub fn try_size_of<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> PsszResult<usize> {
    checked::format::<F>()?;
    let size = value.pssz_size_at_depth(checked::encode_depth())?;
    checked::check_cap(T::MAX_DYNAMIC_DATA, size)?;
    Ok(size)
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
    let (_session, depth) = checked::DecodeSession::enter_at(true);
    T::pssz_unpack_at_depth(bytes, depth)
}

/// Exact-schema decode: additionally rejects unknown fields.
pub fn decode_strict<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<T> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let (session, depth) = checked::DecodeSession::enter_at(false);
    let value = T::pssz_unpack_at_depth(bytes, depth)?;
    if session.has_unknown() { return Err(PsszError::new("pssz: unknown fields rejected by exact reader")); }
    Ok(value)
}

#[derive(Debug, Clone, PartialEq)]
pub struct Decoded<T> { pub value: T, pub has_unknown: bool }
impl<T> Decoded<T> {
    /// Refuse a lossy typed rewrite when an older reader skipped new fields.
    pub fn into_known(self) -> PsszResult<T> {
        if self.has_unknown { Err(PsszError::new("pssz: unknown fields would be lost by typed rewrite")) }
        else { Ok(self.value) }
    }
}

/// Compatible decode that reports whether unknown optional suffix fields were
/// present anywhere in the tree (C++ `decode_pssz_checked`).
pub fn decode_checked<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<Decoded<T>> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let (session, depth) = checked::DecodeSession::enter_at(true);
    let value = T::pssz_unpack_at_depth(bytes, depth)?;
    Ok(Decoded { value, has_unknown: session.has_unknown() })
}

/// Exact schema and bytes identical to the canonical re-encoding.
pub fn validate_canonical_for_profile<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> PsszResult<()> {
    let value = decode_strict::<F, T>(bytes)?;
    if try_encode::<F, T>(&value)? != bytes { return Err(PsszError::new("pssz: noncanonical encoding")); }
    Ok(())
}

/// Structural validation (C++ `psio::validate`): accepts unknown optional
/// suffix fields; rejects everything decode rejects, including unpruned
/// records.
pub fn validate<F: PsszFormat, T: PsszPack<F> + PsszValidate<F> + ?Sized>(bytes: &[u8]) -> PsszResult<()> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let (_session, depth) = checked::DecodeSession::enter_at(true);
    T::pssz_validate_at_depth(bytes, depth)
}
/// Structural validation reporting whether unknown fields were present
/// (C++ `validate_pssz_compatible`).
pub fn validate_compatible<F: PsszFormat, T: PsszPack<F> + PsszValidate<F> + ?Sized>(bytes: &[u8]) -> PsszResult<bool> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let (session, depth) = checked::DecodeSession::enter_at(true);
    T::pssz_validate_at_depth(bytes, depth).map(|_| session.has_unknown())
}
/// Exact-schema validation (C++ `psio::validate_strict`): rejects unknown
/// fields. A raw string is any bytes here too.
pub fn validate_strict<F: PsszFormat, T: PsszPack<F> + PsszValidate<F> + ?Sized>(bytes: &[u8]) -> PsszResult<()> {
    enter::<F>(T::MAX_DYNAMIC_DATA, bytes)?;
    let (session, depth) = checked::DecodeSession::enter_at(false);
    T::pssz_validate_at_depth(bytes, depth)?;
    if session.has_unknown() { return Err(PsszError::new("pssz: unknown fields rejected by exact reader")); }
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
/// `psio::from_pssz<T>(psio::trusted{bytes})`): a valid encoding of `T`,
/// because they were validated earlier or were produced by [`to_pssz`] and
/// kept — a service's own table rows.
///
/// **What trusted skips.** The checks that only matter for bytes nobody
/// vouched for: the depth limit, `max_dynamic_data` caps, length bounds
/// (the bounded types and `#[psio(max = N)]`), the consistency of a frame
/// (offsets in order and inside the frame, a header that cuts no field),
/// the canonical-form rules, `#[psio(invariant)]`, and the per-value
/// bookkeeping of the checked reader: a read that is trusted all the way
/// down touches no thread-local. A frame is read as its header and its
/// slots say (so a row an older or a newer version of the type wrote reads
/// as the checked reader reads it). Records, enums, tuples, lists, arrays of
/// fixed-size elements, strings, optionals, boxes and scalars have such a
/// reader; any other type (a map, a set, a nested value, a hand-written
/// codec without one) is read by the checked one inside the trusted read.
///
/// **What it never skips.** Whatever memory safety needs, so this is a safe
/// function: every slice access is bounds-checked and a `String`'s bytes
/// are verified to be UTF-8.
///
/// **On bytes that are not valid** it panics, or returns some value of `T`
/// (which then may be outside a bound or an invariant the checked reader
/// would have enforced); it never has undefined behaviour. Deeply nested
/// hostile input can exhaust the stack. Use [`from_pssz`] for anything a
/// caller, a peer or another service supplied.
///
/// A reader that also skipped the bounds checks and the UTF-8 check would
/// have to be an `unsafe fn` (`from_pssz_unchecked`); none is provided.
/// `cargo run --release -p psio --example trusted_decode_bench` measures
/// the two readers.
#[track_caller]
pub fn from_pssz_trusted<T: PsszPack<Pssz> + PsszUnpack<Pssz>>(bytes: &[u8]) -> T {
    decode_trusted::<Pssz, T>(bytes)
}

/// [`from_pssz_trusted`] for any profile.
#[track_caller]
pub fn decode_trusted<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8]) -> T {
    // A type without a trusted reader is read by the checked one, in a
    // scope of its own (`pssz_checked::unpack_trusted_by_checking`): a read
    // that is trusted all the way down touches no thread-local.
    T::pssz_unpack_trusted(bytes)
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
pub use crate::pssz_view::{PsszMapEntry, PsszMutMapEntry, PsszMutView, PsszView};

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
