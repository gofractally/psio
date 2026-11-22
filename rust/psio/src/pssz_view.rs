//! Zero-copy pSSZ views: the counterpart of C++ `psio::pssz_view<T>`
//! (docs/helpers.md). Every accessor is arithmetic over the borrowed buffer;
//! nothing is allocated or materialized.
//!
//! [`PsszView<'a, T>`] only reads. Writing is a different type,
//! [`PsszMutView`], made from `&mut [u8]`; nothing turns a read view into
//! one. [`PsszBuffer<T>`] owns its bytes and [`SharedPssz<T>`] shares
//! immutable bytes (`Arc<[u8]>`).
//!
//! Two ways to obtain a view:
//!   - [`PsszView::new`] (and `new_strict` / `new_checked`) run the full
//!     validator first (including the mandatory-pruning rule), so every
//!     accessor on the result succeeds. Use these for untrusted input.
//!   - [`PsszView::trusted`] does not validate: the caller vouches for the
//!     bytes (validated earlier, or produced by `to_pssz`). It is a safe
//!     function: accessors on malformed bytes panic with a defined message
//!     or, for the `try_*` and string accessors, return an error. They never
//!     fabricate an invalid Rust value and never touch memory outside the
//!     buffer.
//!
//! The format tag defaults to the automatic profile: `PsszView<'a, T>` is
//! `PsszView<'a, T, Pssz>`. Offset widths come from the viewed type
//! (`PsszPack::OFFSET_WIDTH`), so a view under `Pssz` reads each nested type
//! at its own width. Record views honor the writer's header: a pruned
//! trailing optional reads as None and unknown appended slots delimit the
//! last known field.

use crate::pssz::{Bitvector, Bitlist, BoundedList, BoundedRawString, BoundedString, Decoded, Pssz, PsszError, PsszFormat, PsszPack, PsszResult, PsszValidate, RawStr, RawString, Uint256};
use crate::pssz_checked as checked;

#[path = "pssz_mut_view.rs"]
mod mut_view;
pub use mut_view::PsszMutView;

/// Field `INDEX` of a record type, for `PsszView::field`.
pub trait PsszField<F: PsszFormat, const INDEX: usize> {
    type Field: PsszPack<F>;
    const LOCATION: checked::FieldLocation;
    const DWNC: bool = false;
    const TRANSPARENT: bool = false;
}

/// Read-only zero-copy view of a `T` encoded under format tag `F`
/// (C++ `psio::pssz_view<T>`).
#[derive(Debug)]
pub struct PsszView<'a, T: ?Sized, F: PsszFormat = Pssz> {
    data: &'a [u8],
    _phantom: std::marker::PhantomData<(fn() -> *const T, fn() -> F)>,
}

impl<'a, T: ?Sized, F: PsszFormat> Clone for PsszView<'a, T, F> {
    fn clone(&self) -> Self { *self }
}
impl<'a, T: ?Sized, F: PsszFormat> Copy for PsszView<'a, T, F> {}

impl<'a, T: ?Sized, F: PsszFormat> PsszView<'a, T, F> {
    /// A view of bytes the caller vouches for; does not validate. Access to
    /// malformed bytes may panic (never undefined behaviour). Use
    /// [`PsszView::new`] for untrusted input.
    pub fn trusted(data: &'a [u8]) -> Self {
        PsszView { data, _phantom: std::marker::PhantomData }
    }
    /// The bytes of the viewed value.
    pub fn bytes(&self) -> &'a [u8] { self.data }

    /// Positional field access; panics on a malformed unvalidated record.
    pub fn field<const INDEX: usize>(&self) -> PsszView<'a, <T as PsszField<F, INDEX>>::Field, F>
    where T: PsszField<F, INDEX> {
        self.try_field::<INDEX>().expect("pssz view: malformed record; validate first")
    }
    pub fn try_field<const INDEX: usize>(&self) -> PsszResult<PsszView<'a, <T as PsszField<F, INDEX>>::Field, F>>
    where T: PsszField<F, INDEX> {
        if <T as PsszField<F, INDEX>>::TRANSPARENT { return Ok(PsszView::trusted(self.data)); }
        Ok(PsszView::trusted(checked::field_bytes::<F, <T as PsszField<F, INDEX>>::Field>(
            self.data, <T as PsszField<F, INDEX>>::LOCATION, <T as PsszField<F, INDEX>>::DWNC,
        )?))
    }
}

impl<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat> PsszView<'a, T, F> {
    /// Validate, then borrow. Accepts what `validate_pssz` accepts: unknown
    /// appended optional fields are allowed.
    pub fn new(data: &'a [u8]) -> PsszResult<Self> {
        Self::new_checked(data).map(|v| v.value)
    }
    /// Validated view that rejects unknown fields (the exact reader, C++
    /// `validate_strict`).
    pub fn new_strict(bytes: &'a [u8]) -> PsszResult<Self> {
        crate::pssz::validate_strict::<F, T>(bytes)?;
        Ok(PsszView::trusted(bytes))
    }
    /// Validated view that reports whether unknown fields were present
    /// (C++ `validated_pssz_view`).
    pub fn new_checked(bytes: &'a [u8]) -> PsszResult<Decoded<Self>> {
        let has_unknown = crate::pssz::validate_compatible::<F, T>(bytes)?;
        Ok(Decoded { value: PsszView::trusted(bytes), has_unknown })
    }
}

// ── Primitives ──────────────────────────────────────────────────────────────

macro_rules! impl_pssz_view_primitive {
    ($($ty:ty),* $(,)?) => {$(
        impl<'a, F: PsszFormat> PsszView<'a, $ty, F> {
            pub fn get(&self) -> $ty {
                let mut buf = [0u8; std::mem::size_of::<$ty>()];
                buf.copy_from_slice(&self.data[..std::mem::size_of::<$ty>()]);
                <$ty>::from_le_bytes(buf)
            }
        }
    )*};
}
impl_pssz_view_primitive!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

impl<'a, F: PsszFormat> PsszView<'a, bool, F> {
    pub fn get(&self) -> bool {
        match self.data.first() {
            Some(0) => false,
            Some(1) => true,
            _ => panic!("pssz view: invalid bool"),
        }
    }
}

impl<'a, F: PsszFormat> PsszView<'a, Uint256, F> {
    pub fn get(&self) -> Uint256 {
        let mut limb = [0u64; 4];
        for i in 0..4 {
            let mut b = [0u8; 8];
            b.copy_from_slice(&self.data[i * 8 .. (i + 1) * 8]);
            limb[i] = u64::from_le_bytes(b);
        }
        Uint256 { limb }
    }
}

// ── Strings ─────────────────────────────────────────────────────────────────
//
// UTF-8 string views return an error on bytes that are not UTF-8 (possible
// only over unvalidated data); they never panic. Raw string views hand out
// the bytes.

fn utf8<'a>(data: &'a [u8]) -> PsszResult<&'a str> {
    std::str::from_utf8(data).map_err(|_| PsszError("pssz view: invalid UTF-8 in string"))
}

impl<'a, F: PsszFormat> PsszView<'a, String, F> {
    pub fn view(&self) -> PsszResult<&'a str> { utf8(self.data) }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, BoundedString<N>, F> {
    pub fn view(&self) -> PsszResult<&'a str> {
        if self.data.len() > N { return Err(PsszError("pssz view: BoundedString overflow")); }
        utf8(self.data)
    }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat> PsszView<'a, RawString, F> {
    pub fn view(&self) -> RawStr<'a> { RawStr(self.data) }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, BoundedRawString<N>, F> {
    pub fn view(&self) -> PsszResult<RawStr<'a>> {
        if self.data.len() > N { return Err(PsszError("pssz view: BoundedRawString overflow")); }
        Ok(RawStr(self.data))
    }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

// ── Vec / BoundedList ───────────────────────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat> PsszView<'a, Vec<T>, F> {
    pub fn try_len(&self) -> PsszResult<usize> {
        checked::sequence_len::<F, T>(<Vec<T> as PsszPack<F>>::OFFSET_WIDTH, self.data, None)
    }
    pub fn try_get(&self, i: usize) -> PsszResult<PsszView<'a, T, F>> {
        let w = <Vec<T> as PsszPack<F>>::OFFSET_WIDTH;
        let n = checked::sequence_len::<F, T>(w, self.data, None)?;
        Ok(PsszView::trusted(&self.data[checked::sequence_span::<F, T>(w, self.data, n, i)?]))
    }
    pub fn len(&self) -> usize { self.try_len().expect("pssz view: malformed vector") }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        self.try_get(i).expect("pssz view: invalid vector element")
    }
}

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat, const N: usize>
    PsszView<'a, BoundedList<T, N>, F>
{
    pub fn len(&self) -> usize {
        // Same wire as Vec<T>, so construct a Vec view internally.
        let v: PsszView<'a, Vec<T>, F> = PsszView::trusted(self.data);
        let n = v.len();
        assert!(n <= N, "pssz view: BoundedList overflow");
        n
    }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        assert!(i < self.len(), "pssz view: BoundedList index out of range");
        let v: PsszView<'a, Vec<T>, F> = PsszView::trusted(self.data);
        v.get(i)
    }
}

// ── [T; N] ──────────────────────────────────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat, const N: usize>
    PsszView<'a, [T; N], F>
{
    pub const fn size() -> usize { N }
    pub fn try_get(&self, i: usize) -> PsszResult<PsszView<'a, T, F>> {
        let w = <[T; N] as PsszPack<F>>::OFFSET_WIDTH;
        checked::sequence_len::<F, T>(w, self.data, Some(N))?;
        Ok(PsszView::trusted(&self.data[checked::sequence_span::<F, T>(w, self.data, N, i)?]))
    }
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        self.try_get(i).expect("pssz view: invalid array element")
    }
}

// ── Option<T> — min-size-gated selector ────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat> PsszView<'a, Option<T>, F> {
    pub fn try_has_value(&self) -> PsszResult<bool> {
        if T::MIN_ENCODED_SIZE == 0 {
            match self.data.first() {
                Some(0) if self.data.len() == 1 => Ok(false),
                Some(1) => Ok(true),
                _ => Err(PsszError("pssz view: invalid optional selector or None payload")),
            }
        } else {
            if T::IS_FIXED_SIZE && !self.data.is_empty() && self.data.len() != T::FIXED_SIZE {
                return Err(PsszError("pssz view: optional fixed span mismatch"));
            }
            Ok(!self.data.is_empty())
        }
    }
    pub fn has_value(&self) -> bool {
        self.try_has_value().expect("pssz view: invalid optional")
    }
    pub fn unwrap(&self) -> PsszView<'a, T, F> {
        assert!(self.has_value(), "pssz view: unwrap of None");
        if T::MIN_ENCODED_SIZE == 0 {
            PsszView::trusted(&self.data[1..])
        } else {
            PsszView::trusted(self.data)
        }
    }
    pub fn get(&self) -> Option<PsszView<'a, T, F>> {
        if self.has_value() { Some(self.unwrap()) } else { None }
    }
}

impl<'a, T: ?Sized, F: PsszFormat> PsszView<'a, Box<T>, F> {
    pub fn deref(&self) -> PsszView<'a, T, F> { PsszView::trusted(self.data) }
}

// ── Bit types ───────────────────────────────────────────────────────────────

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, Bitvector<N>, F> {
    pub fn test(&self, i: usize) -> bool {
        assert!(i < N, "pssz view: bitvector index out of range");
        (self.data[i >> 3] >> (i & 7)) & 1 != 0
    }
    pub const fn size(&self) -> usize { N }
}

impl<'a, F: PsszFormat, const MAX_N: usize> PsszView<'a, Bitlist<MAX_N>, F> {
    pub fn len(&self) -> usize {
        crate::pssz_bits::list_count(self.data, MAX_N).expect("pssz view: malformed bitlist")
    }
    pub fn test(&self, i: usize) -> bool {
        assert!(i < self.len(), "pssz view: bitlist index out of range");
        (self.data[i >> 3] >> (i & 7)) & 1 != 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pssz::{Pssz32, encode};

    #[test]
    fn primitive_view() {
        let b = encode::<Pssz32, _>(&0xDEADBEEFu32);
        let v: PsszView<u32, Pssz32> = PsszView::trusted(&b);
        assert_eq!(v.get(), 0xDEADBEEF);
    }

    #[test]
    fn string_view() {
        let b = encode::<Pssz32, _>(&"hello".to_string());
        let v: PsszView<String, Pssz32> = PsszView::trusted(&b);
        assert_eq!(v.view().unwrap(), "hello");
    }

    #[test]
    fn vec_fixed_view() {
        let v = vec![10u32, 20, 30];
        let b = encode::<Pssz32, _>(&v);
        let view: PsszView<Vec<u32>, Pssz32> = PsszView::trusted(&b);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0).get(), 10);
        assert_eq!(view.get(2).get(), 30);
    }

    #[test]
    fn vec_variable_view() {
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        let b = encode::<Pssz32, _>(&v);
        let view: PsszView<Vec<String>, Pssz32> = PsszView::trusted(&b);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0).view().unwrap(), "a");
        assert_eq!(view.get(2).view().unwrap(), "def");
    }

    #[test]
    fn option_fixed_no_selector_view() {
        // Option<u32> has no selector in pSSZ.
        let some: Option<u32> = Some(42);
        let b = encode::<Pssz32, _>(&some);
        let v: PsszView<Option<u32>, Pssz32> = PsszView::trusted(&b);
        assert!(v.has_value());
        assert_eq!(v.unwrap().get(), 42);

        let none: Option<u32> = None;
        let b = encode::<Pssz32, _>(&none);
        let v: PsszView<Option<u32>, Pssz32> = PsszView::trusted(&b);
        assert!(!v.has_value());
    }

    #[test]
    fn option_variable_with_selector_view() {
        // Option<String> carries a selector.
        let some = Some("x".to_string());
        let b = encode::<Pssz32, _>(&some);
        let v: PsszView<Option<String>, Pssz32> = PsszView::trusted(&b);
        assert!(v.has_value());
        assert_eq!(v.unwrap().view().unwrap(), "x");

        let none: Option<String> = None;
        let b = encode::<Pssz32, _>(&none);
        let v: PsszView<Option<String>, Pssz32> = PsszView::trusted(&b);
        assert!(!v.has_value());
    }
}
