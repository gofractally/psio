//! Zero-copy pSSZ views: the counterpart of C++ `pssz_view<T, F>`. Every
//! accessor is arithmetic over the borrowed buffer; nothing is allocated or
//! materialized.
//!
//! Two ways to obtain a view:
//!   - `PsszView::try_new` / `try_pssz_view_of` / `validated_view_*` run the
//!     full validator first (including the mandatory-pruning rule), so every
//!     accessor on the result succeeds. Use these for untrusted input.
//!   - `PsszView::new` / `pssz_view_of` do not validate. Accessors on
//!     malformed bytes may panic with a defined message or, for the `try_*`
//!     and string accessors, return an error. They never fabricate an invalid
//!     Rust value and never touch memory outside the buffer.
//!
//! Offset widths come from the viewed type (`PsszPack::OFFSET_WIDTH`), so a
//! view under the automatic `Pssz` profile reads each nested type at its own
//! width. Record views honor the writer's header: a pruned trailing optional
//! reads as None and unknown appended slots delimit the last known field.

use crate::pssz::{Bitvector, Bitlist, BoundedList, BoundedRawString, BoundedString, Decoded, PsszError, PsszFormat, PsszPack, PsszResult, PsszValidate, RawStr, RawString, Uint256};
use crate::pssz_checked as checked;

/// Field `INDEX` of a record type, for `PsszView::field`.
pub trait PsszField<F: PsszFormat, const INDEX: usize> {
    type Field: PsszPack<F>;
    const LOCATION: checked::FieldLocation;
    const DWNC: bool = false;
    const TRANSPARENT: bool = false;
}

/// Zero-copy pSSZ view parameterized by target type `T` and format tag `F`.
#[derive(Debug, Clone, Copy)]
pub struct PsszView<'a, T: ?Sized, F: PsszFormat> {
    pub data: &'a [u8],
    _phantom: std::marker::PhantomData<(*const T, *const F)>,
}

impl<'a, T: ?Sized, F: PsszFormat> PsszView<'a, T, F> {
    /// An unchecked borrowed byte view. Access to malformed bytes may panic.
    /// Prefer `try_new` for untrusted input where T implements PsszValidate.
    pub fn new(data: &'a [u8]) -> Self {
        PsszView { data, _phantom: std::marker::PhantomData }
    }
    pub fn raw(&self) -> &'a [u8] { self.data }

    /// Positional field access; panics on a malformed unvalidated record.
    pub fn field<const INDEX: usize>(&self) -> PsszView<'a, <T as PsszField<F, INDEX>>::Field, F>
    where T: PsszField<F, INDEX> {
        self.try_field::<INDEX>().expect("pssz view: malformed record; validate first")
    }
    pub fn try_field<const INDEX: usize>(&self) -> PsszResult<PsszView<'a, <T as PsszField<F, INDEX>>::Field, F>>
    where T: PsszField<F, INDEX> {
        if <T as PsszField<F, INDEX>>::TRANSPARENT { return Ok(PsszView::new(self.data)); }
        Ok(PsszView::new(checked::field_bytes::<F, <T as PsszField<F, INDEX>>::Field>(
            self.data, <T as PsszField<F, INDEX>>::LOCATION, <T as PsszField<F, INDEX>>::DWNC,
        )?))
    }
}

impl<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat> PsszView<'a, T, F> {
    /// Validate, then borrow. Unknown optional suffix fields are accepted.
    pub fn try_new(data: &'a [u8]) -> PsszResult<Self> {
        validated_view_compatible::<T, F>(data).map(|v| v.value)
    }
}

/// Validated view that rejects unknown fields (exact reader).
pub fn validated_view_exact<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat>(bytes: &'a [u8]) -> PsszResult<PsszView<'a, T, F>> {
    checked::format::<F>()?;
    checked::check_cap(T::MAX_DYNAMIC_DATA, bytes.len())?;
    let session = checked::DecodeSession::enter(false);
    T::pssz_validate(bytes)?;
    if session.has_unknown() { return Err(PsszError("pssz view: unknown fields in exact validation")); }
    Ok(PsszView::new(bytes))
}
/// Validated view that reports unknown fields (C++ `validated_pssz_view`).
pub fn validated_view_compatible<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat>(bytes: &'a [u8]) -> PsszResult<Decoded<PsszView<'a, T, F>>> {
    checked::format::<F>()?;
    checked::check_cap(T::MAX_DYNAMIC_DATA, bytes.len())?;
    let session = checked::DecodeSession::enter(true);
    T::pssz_validate(bytes)?;
    Ok(Decoded { value: PsszView::new(bytes), has_unknown: session.has_unknown() })
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
    pub fn bytes(&self) -> &'a [u8] { self.data }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, BoundedString<N>, F> {
    pub fn view(&self) -> PsszResult<&'a str> {
        if self.data.len() > N { return Err(PsszError("pssz view: BoundedString overflow")); }
        utf8(self.data)
    }
    pub fn bytes(&self) -> &'a [u8] { self.data }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat> PsszView<'a, RawString, F> {
    pub fn view(&self) -> RawStr<'a> { RawStr(self.data) }
    pub fn bytes(&self) -> &'a [u8] { self.data }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, BoundedRawString<N>, F> {
    pub fn view(&self) -> PsszResult<RawStr<'a>> {
        if self.data.len() > N { return Err(PsszError("pssz view: BoundedRawString overflow")); }
        Ok(RawStr(self.data))
    }
    pub fn bytes(&self) -> &'a [u8] { self.data }
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
        Ok(PsszView::new(&self.data[checked::sequence_span::<F, T>(w, self.data, n, i)?]))
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
        let v: PsszView<'a, Vec<T>, F> = PsszView::new(self.data);
        let n = v.len();
        assert!(n <= N, "pssz view: BoundedList overflow");
        n
    }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        assert!(i < self.len(), "pssz view: BoundedList index out of range");
        let v: PsszView<'a, Vec<T>, F> = PsszView::new(self.data);
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
        Ok(PsszView::new(&self.data[checked::sequence_span::<F, T>(w, self.data, N, i)?]))
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
            PsszView::new(&self.data[1..])
        } else {
            PsszView::new(self.data)
        }
    }
    pub fn get(&self) -> Option<PsszView<'a, T, F>> {
        if self.has_value() { Some(self.unwrap()) } else { None }
    }
}

impl<'a, T: ?Sized, F: PsszFormat> PsszView<'a, Box<T>, F> {
    pub fn deref(&self) -> PsszView<'a, T, F> { PsszView::new(self.data) }
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

/// Entry point matching C++ `psio::pssz_view_of<T, F>(buf)`.
/// This constructor does not validate; malformed access can panic.
pub fn pssz_view_of<'a, T: ?Sized, F: PsszFormat>(buf: &'a [u8]) -> PsszView<'a, T, F> {
    PsszView::new(buf)
}

/// Validate before borrowing untrusted bytes.
pub fn try_pssz_view_of<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat>(
    buf: &'a [u8],
) -> PsszResult<PsszView<'a, T, F>> {
    PsszView::try_new(buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pssz::{Pssz32, to_pssz};

    #[test]
    fn primitive_view() {
        let b = to_pssz::<Pssz32, _>(&0xDEADBEEFu32);
        let v: PsszView<u32, Pssz32> = pssz_view_of(&b);
        assert_eq!(v.get(), 0xDEADBEEF);
    }

    #[test]
    fn string_view() {
        let b = to_pssz::<Pssz32, _>(&"hello".to_string());
        let v: PsszView<String, Pssz32> = pssz_view_of(&b);
        assert_eq!(v.view().unwrap(), "hello");
    }

    #[test]
    fn vec_fixed_view() {
        let v = vec![10u32, 20, 30];
        let b = to_pssz::<Pssz32, _>(&v);
        let view: PsszView<Vec<u32>, Pssz32> = pssz_view_of(&b);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0).get(), 10);
        assert_eq!(view.get(2).get(), 30);
    }

    #[test]
    fn vec_variable_view() {
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        let b = to_pssz::<Pssz32, _>(&v);
        let view: PsszView<Vec<String>, Pssz32> = pssz_view_of(&b);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0).view().unwrap(), "a");
        assert_eq!(view.get(2).view().unwrap(), "def");
    }

    #[test]
    fn option_fixed_no_selector_view() {
        // Option<u32> has no selector in pSSZ.
        let some: Option<u32> = Some(42);
        let b = to_pssz::<Pssz32, _>(&some);
        let v: PsszView<Option<u32>, Pssz32> = pssz_view_of(&b);
        assert!(v.has_value());
        assert_eq!(v.unwrap().get(), 42);

        let none: Option<u32> = None;
        let b = to_pssz::<Pssz32, _>(&none);
        let v: PsszView<Option<u32>, Pssz32> = pssz_view_of(&b);
        assert!(!v.has_value());
    }

    #[test]
    fn option_variable_with_selector_view() {
        // Option<String> carries a selector.
        let some = Some("x".to_string());
        let b = to_pssz::<Pssz32, _>(&some);
        let v: PsszView<Option<String>, Pssz32> = pssz_view_of(&b);
        assert!(v.has_value());
        assert_eq!(v.unwrap().view().unwrap(), "x");

        let none: Option<String> = None;
        let b = to_pssz::<Pssz32, _>(&none);
        let v: PsszView<Option<String>, Pssz32> = pssz_view_of(&b);
        assert!(!v.has_value());
    }
}
