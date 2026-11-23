//! Zero-copy pSSZ views: the counterpart of C++ `psio::pssz_view<T>`
//! (docs/helpers.md). Every accessor is arithmetic over the borrowed buffer;
//! nothing is allocated or materialized, and every accessor is O(1): a
//! field is found from the header and its own slot, an element from the
//! first offset of the table and its own (the whole table is checked by
//! the validator, [`PsszView::new`], not on each access).
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
//! at its own width. The format tag is also the position: the view of a
//! member of a record whose cap is `C` is a `PsszView<'a, Member,
//! PsszUnder<C>>`, which reads the lists, maps and tuples beneath the cap at
//! the widths the cap selects (docs/pssz-spec.md 3.6); the view of a member
//! of a record without a cap is a `PsszView<'a, Member>`. A record has the
//! same bytes at every position: `PsszView::<Child>::trusted(view.bytes())`
//! is the view of a child record without its position.
//! Record views honor the writer's header: a pruned
//! trailing optional reads as None and unknown appended slots delimit the
//! last known field.
//!
//! By type: a record has named accessors (`#[derive(PsszView)]`) and
//! `field::<N>()`, which a tuple has too; `Vec` / `BoundedList` / arrays
//! have `len()` / `get(i)`; a map has `len()` / `get(i)` giving a
//! [`PsszMapEntry`] (`key()`, `value()`) and a set `len()` / `get(i)`, both
//! in ascending key order; `Option` has `get()`; `Nested<T>` has `get()`,
//! the view of its inner value.

use crate::pssz::{Bitvector, Bitlist, BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, Decoded, Nested, Pssz, PsszError, PsszFormat, PsszPack, PsszResult, PsszValidate, RawStr, RawString, Uint256};
use crate::pssz_checked as checked;

#[path = "pssz_mut_view.rs"]
mod mut_view;
pub use mut_view::{PsszMutMapEntry, PsszMutView};

/// Field `INDEX` of a record type, for `PsszView::field`.
pub trait PsszField<F: PsszFormat, const INDEX: usize> {
    /// The format of the field's position: `F` for a tuple and a
    /// transparent type, `F::Free` or `F::Under<cap>` for a record.
    type Beneath: PsszFormat;
    type Field: PsszPack<Self::Beneath>;
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
    #[inline]
    pub fn trusted(data: &'a [u8]) -> Self {
        PsszView { data, _phantom: std::marker::PhantomData }
    }
    /// The bytes of the viewed value.
    #[inline]
    pub fn bytes(&self) -> &'a [u8] { self.data }

    /// Positional field access; panics on a malformed unvalidated record.
    #[inline]
    pub fn field<const INDEX: usize>(
        &self,
    ) -> PsszView<'a, <T as PsszField<F, INDEX>>::Field, <T as PsszField<F, INDEX>>::Beneath>
    where T: PsszField<F, INDEX> {
        self.try_field::<INDEX>().expect("pssz view: malformed record; validate first")
    }
    #[inline]
    pub fn try_field<const INDEX: usize>(
        &self,
    ) -> PsszResult<PsszView<'a, <T as PsszField<F, INDEX>>::Field, <T as PsszField<F, INDEX>>::Beneath>>
    where T: PsszField<F, INDEX> {
        if <T as PsszField<F, INDEX>>::TRANSPARENT { return Ok(PsszView::trusted(self.data)); }
        Ok(PsszView::trusted(checked::field_bytes::<<T as PsszField<F, INDEX>>::Beneath, <T as PsszField<F, INDEX>>::Field>(
            self.data, <T as PsszField<F, INDEX>>::LOCATION, <T as PsszField<F, INDEX>>::DWNC,
        )?))
    }
}

impl<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat> PsszView<'a, T, F> {
    /// Validate, then borrow. Accepts what `validate_pssz` accepts: unknown
    /// appended optional fields are allowed.
    #[inline]
    pub fn new(data: &'a [u8]) -> PsszResult<Self> {
        Self::new_checked(data).map(|v| v.value)
    }
    /// Validated view that rejects unknown fields (the exact reader, C++
    /// `validate_strict`).
    #[inline]
    pub fn new_strict(bytes: &'a [u8]) -> PsszResult<Self> {
        crate::pssz::validate_strict::<F, T>(bytes)?;
        Ok(PsszView::trusted(bytes))
    }
    /// Validated view that reports whether unknown fields were present
    /// (C++ `validated_pssz_view`).
    #[inline]
    pub fn new_checked(bytes: &'a [u8]) -> PsszResult<Decoded<Self>> {
        let has_unknown = crate::pssz::validate_compatible::<F, T>(bytes)?;
        Ok(Decoded { value: PsszView::trusted(bytes), has_unknown })
    }
}

// ── Primitives ──────────────────────────────────────────────────────────────

macro_rules! impl_pssz_view_primitive {
    ($($ty:ty),* $(,)?) => {$(
        impl<'a, F: PsszFormat> PsszView<'a, $ty, F> {
            #[inline]
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
    #[inline]
    pub fn get(&self) -> bool {
        match self.data.first() {
            Some(0) => false,
            Some(1) => true,
            _ => panic!("pssz view: invalid bool"),
        }
    }
}

impl<'a, F: PsszFormat> PsszView<'a, Uint256, F> {
    #[inline]
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
    crate::pssz::utf8_str(data).ok_or(PsszError::new("pssz view: invalid UTF-8 in string"))
}

impl<'a, F: PsszFormat> PsszView<'a, String, F> {
    #[inline]
    pub fn view(&self) -> PsszResult<&'a str> { utf8(self.data) }
    #[inline]
    pub fn len(&self) -> usize { self.data.len() }
    #[inline]
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, BoundedString<N>, F> {
    #[inline]
    pub fn view(&self) -> PsszResult<&'a str> {
        if self.data.len() > N { return Err(PsszError::new("pssz view: BoundedString overflow")); }
        utf8(self.data)
    }
    #[inline]
    pub fn len(&self) -> usize { self.data.len() }
    #[inline]
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat> PsszView<'a, RawString, F> {
    #[inline]
    pub fn view(&self) -> RawStr<'a> { RawStr(self.data) }
    #[inline]
    pub fn len(&self) -> usize { self.data.len() }
    #[inline]
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, BoundedRawString<N>, F> {
    #[inline]
    pub fn view(&self) -> PsszResult<RawStr<'a>> {
        if self.data.len() > N { return Err(PsszError::new("pssz view: BoundedRawString overflow")); }
        Ok(RawStr(self.data))
    }
    #[inline]
    pub fn len(&self) -> usize { self.data.len() }
    #[inline]
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

// ── Vec / BoundedList ───────────────────────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat> PsszView<'a, Vec<T>, F> {
    #[inline]
    pub fn try_len(&self) -> PsszResult<usize> {
        checked::sequence_count::<F, T>(<Vec<T> as PsszPack<F>>::OFFSET_WIDTH, self.data, None)
    }
    #[inline]
    pub fn try_get(&self, i: usize) -> PsszResult<PsszView<'a, T, F>> {
        let w = <Vec<T> as PsszPack<F>>::OFFSET_WIDTH;
        let n = checked::sequence_count::<F, T>(w, self.data, None)?;
        Ok(PsszView::trusted(element_bytes(self.data, checked::sequence_span::<F, T>(w, self.data, n, i)?)?))
    }
    #[inline]
    pub fn len(&self) -> usize { self.try_len().expect("pssz view: malformed vector") }
    #[inline]
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    #[inline]
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        self.try_get(i).expect("pssz view: invalid vector element")
    }
    /// The elements in order. Panics, like [`get`](Self::get), on a
    /// malformed unvalidated list.
    #[inline]
    pub fn iter(&self) -> impl ExactSizeIterator<Item = PsszView<'a, T, F>> + 'a
    where
        F: 'a,
    {
        let view = *self;
        (0..view.len()).map(move |i| view.get(i))
    }
}

/// The bytes of an element whose span an accessor computed.
#[inline]
fn element_bytes(data: &[u8], span: std::ops::Range<usize>) -> PsszResult<&[u8]> {
    data.get(span).ok_or(PsszError::new("pssz view: element outside the buffer"))
}

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat, const N: usize>
    PsszView<'a, BoundedList<T, N>, F>
{
    // The list's own offset width: that of its bound, not of a `Vec<T>`.
    const W: usize = list_width::<F, T>(N);
    #[inline]
    pub fn try_len(&self) -> PsszResult<usize> {
        bounded_len::<F, T>(Self::W, self.data, N)
    }
    #[inline]
    pub fn try_get(&self, i: usize) -> PsszResult<PsszView<'a, T, F>> {
        Ok(PsszView::trusted(element_bytes(self.data, bounded_element::<F, T>(Self::W, self.data, N, i)?)?))
    }
    #[inline]
    pub fn len(&self) -> usize { self.try_len().expect("pssz view: BoundedList overflow or malformed list") }
    #[inline]
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    #[inline]
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        self.try_get(i).expect("pssz view: BoundedList index out of range")
    }
    /// The elements in order. Panics, like [`get`](Self::get), on a
    /// malformed unvalidated list.
    #[inline]
    pub fn iter(&self) -> impl ExactSizeIterator<Item = PsszView<'a, T, F>> + 'a
    where
        F: 'a,
    {
        let view = *self;
        (0..view.len()).map(move |i| view.get(i))
    }
}

// ── [T; N] ──────────────────────────────────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat, const N: usize>
    PsszView<'a, [T; N], F>
{
    #[inline]
    pub const fn size() -> usize { N }
    #[inline]
    pub fn try_get(&self, i: usize) -> PsszResult<PsszView<'a, T, F>> {
        let w = <[T; N] as PsszPack<F>>::OFFSET_WIDTH;
        checked::sequence_count::<F, T>(w, self.data, Some(N))?;
        Ok(PsszView::trusted(element_bytes(self.data, checked::sequence_span::<F, T>(w, self.data, N, i)?)?))
    }
    #[inline]
    pub fn get(&self, i: usize) -> PsszView<'a, T, F> {
        self.try_get(i).expect("pssz view: invalid array element")
    }
}

// ── Option<T> — min-size-gated selector ────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat> PsszView<'a, Option<T>, F> {
    #[inline]
    pub fn try_has_value(&self) -> PsszResult<bool> {
        if T::MIN_ENCODED_SIZE == 0 {
            match self.data.first() {
                Some(0) if self.data.len() == 1 => Ok(false),
                Some(1) => Ok(true),
                _ => Err(PsszError::new("pssz view: invalid optional selector or None payload")),
            }
        } else {
            if T::IS_FIXED_SIZE && !self.data.is_empty() && self.data.len() != T::FIXED_SIZE {
                return Err(PsszError::new("pssz view: optional fixed span mismatch"));
            }
            Ok(!self.data.is_empty())
        }
    }
    #[inline]
    pub fn has_value(&self) -> bool {
        self.try_has_value().expect("pssz view: invalid optional")
    }
    #[inline]
    pub fn unwrap(&self) -> PsszView<'a, T, F> {
        assert!(self.has_value(), "pssz view: unwrap of None");
        if T::MIN_ENCODED_SIZE == 0 {
            PsszView::trusted(&self.data[1..])
        } else {
            PsszView::trusted(self.data)
        }
    }
    #[inline]
    pub fn get(&self) -> Option<PsszView<'a, T, F>> {
        if self.has_value() { Some(self.unwrap()) } else { None }
    }
}

impl<'a, T: ?Sized, F: PsszFormat> PsszView<'a, Box<T>, F> {
    #[inline]
    pub fn deref(&self) -> PsszView<'a, T, F> { PsszView::trusted(self.data) }
}

// ── Maps and sets ───────────────────────────────────────────────────────────
//
// A map is the list of its entries `(K, V)` in ascending key order, a set
// the list of its elements (docs/pssz-spec.md 5.9): the views index that
// list. Position `i` is the i-th entry in key order. The order is checked by
// the validator, not here.

/// One entry of a map view: the key and the mapped value (C++: the view of
/// the entry tuple).
#[derive(Debug)]
pub struct PsszMapEntry<'a, K, V, F: PsszFormat = Pssz> {
    entry: PsszView<'a, (K, V), F>,
}
impl<'a, K, V, F: PsszFormat> Clone for PsszMapEntry<'a, K, V, F> {
    fn clone(&self) -> Self { *self }
}
impl<'a, K, V, F: PsszFormat> Copy for PsszMapEntry<'a, K, V, F> {}

impl<'a, K: PsszPack<F>, V: PsszPack<F>, F: PsszFormat> PsszMapEntry<'a, K, V, F> {
    #[inline]
    pub fn try_key(&self) -> PsszResult<PsszView<'a, K, F>> { self.entry.try_field::<0>() }
    #[inline]
    pub fn try_value(&self) -> PsszResult<PsszView<'a, V, F>> { self.entry.try_field::<1>() }
    #[inline]
    pub fn key(&self) -> PsszView<'a, K, F> {
        self.try_key().expect("pssz view: malformed map entry; validate first")
    }
    #[inline]
    pub fn value(&self) -> PsszView<'a, V, F> {
        self.try_value().expect("pssz view: malformed map entry; validate first")
    }
    /// The bytes of the entry: the encoding of the tuple `(K, V)`.
    #[inline]
    pub fn bytes(&self) -> &'a [u8] { self.entry.bytes() }
}

/// The offset-table width of a list (or the list of a map / set) of at most
/// `max` elements `T` at the position `F`; `usize::MAX` is no bound.
pub(crate) const fn list_width<F: PsszFormat, T: PsszPack<F>>(max: usize) -> usize {
    crate::pssz::position_width::<F>(if max == usize::MAX {
        None
    } else {
        checked::sequence_bound(max, T::AUTO_BOUND, T::IS_FIXED_SIZE)
    })
}
/// `try_len` of a list of `T` holding at most `max` elements, whose offset
/// table is `w` wide.
fn bounded_len<F: PsszFormat, T: PsszPack<F>>(w: usize, data: &[u8], max: usize) -> PsszResult<usize> {
    let n = checked::sequence_count::<F, T>(w, data, None)?;
    if n > max {
        return Err(PsszError::new("pssz view: list / map / set exceeds its bound"));
    }
    Ok(n)
}
/// The bytes of element `i` of a list of `T` holding at most `max` elements.
pub(crate) fn bounded_element<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    data: &[u8],
    max: usize,
    i: usize,
) -> PsszResult<std::ops::Range<usize>> {
    let n = bounded_len::<F, T>(w, data, max)?;
    checked::sequence_span::<F, T>(w, data, n, i)
}

macro_rules! pssz_map_view {
    ([$($cg:tt)*], $ty:ty, $max:expr) => {
        impl<'a, K: PsszPack<F>, V: PsszPack<F>, F: PsszFormat, $($cg)*> PsszView<'a, $ty, F> {
            /// The number of entries.
            #[inline]
            pub fn try_len(&self) -> PsszResult<usize> {
                bounded_len::<F, (K, V)>(list_width::<F, (K, V)>($max), self.data, $max)
            }
            /// Entry `i` in ascending key order.
            #[inline]
            pub fn try_get(&self, i: usize) -> PsszResult<PsszMapEntry<'a, K, V, F>> {
                let span = bounded_element::<F, (K, V)>(list_width::<F, (K, V)>($max), self.data, $max, i)?;
                Ok(PsszMapEntry { entry: PsszView::trusted(element_bytes(self.data, span)?) })
            }
            #[inline]
            pub fn len(&self) -> usize { self.try_len().expect("pssz view: malformed map") }
            #[inline]
            pub fn is_empty(&self) -> bool { self.len() == 0 }
            #[inline]
            pub fn get(&self, i: usize) -> PsszMapEntry<'a, K, V, F> {
                self.try_get(i).expect("pssz view: invalid map entry")
            }
        }
    };
}
macro_rules! pssz_set_view {
    ([$($cg:tt)*], $ty:ty, $max:expr) => {
        impl<'a, K: PsszPack<F>, F: PsszFormat, $($cg)*> PsszView<'a, $ty, F> {
            /// The number of elements.
            #[inline]
            pub fn try_len(&self) -> PsszResult<usize> {
                bounded_len::<F, K>(list_width::<F, K>($max), self.data, $max)
            }
            /// Element `i` in ascending key order.
            #[inline]
            pub fn try_get(&self, i: usize) -> PsszResult<PsszView<'a, K, F>> {
                let span = bounded_element::<F, K>(list_width::<F, K>($max), self.data, $max, i)?;
                Ok(PsszView::trusted(element_bytes(self.data, span)?))
            }
            #[inline]
            pub fn len(&self) -> usize { self.try_len().expect("pssz view: malformed set") }
            #[inline]
            pub fn is_empty(&self) -> bool { self.len() == 0 }
            #[inline]
            pub fn get(&self, i: usize) -> PsszView<'a, K, F> {
                self.try_get(i).expect("pssz view: invalid set element")
            }
        }
    };
}
pssz_map_view!([], std::collections::BTreeMap<K, V>, usize::MAX);
pssz_map_view!([S], std::collections::HashMap<K, V, S>, usize::MAX);
pssz_map_view!([const N: usize], BoundedMap<K, V, N>, N);
pssz_set_view!([], std::collections::BTreeSet<K>, usize::MAX);
pssz_set_view!([S], std::collections::HashSet<K, S>, usize::MAX);
pssz_set_view!([const N: usize], BoundedSet<K, N>, N);

// ── Nested values ───────────────────────────────────────────────────────────
//
// The bytes of a nested value are the encoding of its inner value, always
// under the automatic profile, whatever profile the enclosing view reads
// (docs/pssz-spec.md 5.8).

impl<'a, T, V, F: PsszFormat, const N: usize> PsszView<'a, Nested<T, V, N>, F> {
    /// The view of the inner value. The bytes were validated as a `T` with
    /// the enclosing value when the nested value is `Checked`; a `Deferred`
    /// one was not looked at, so this is then a view of unexamined bytes
    /// (as from [`PsszView::trusted`]): use [`Self::try_get`].
    #[inline]
    pub fn get(&self) -> PsszView<'a, T, Pssz> { PsszView::trusted(self.data) }
    /// The number of bytes of the inner encoding.
    #[inline]
    pub fn len(&self) -> usize { self.data.len() }
    #[inline]
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}
impl<'a, T: PsszPack<Pssz> + PsszValidate<Pssz>, V, F: PsszFormat, const N: usize>
    PsszView<'a, Nested<T, V, N>, F>
{
    /// Validates the bytes as a `T`, then gives the view of the inner value.
    #[inline]
    pub fn try_get(&self) -> PsszResult<PsszView<'a, T, Pssz>> { PsszView::new(self.data) }
}

// ── Bit types ───────────────────────────────────────────────────────────────

impl<'a, F: PsszFormat, const N: usize> PsszView<'a, Bitvector<N>, F> {
    #[inline]
    pub fn test(&self, i: usize) -> bool {
        assert!(i < N, "pssz view: bitvector index out of range");
        (self.data[i >> 3] >> (i & 7)) & 1 != 0
    }
    #[inline]
    pub const fn size(&self) -> usize { N }
}

impl<'a, F: PsszFormat, const MAX_N: usize> PsszView<'a, Bitlist<MAX_N>, F> {
    #[inline]
    pub fn len(&self) -> usize {
        crate::pssz_bits::list_count(self.data, MAX_N).expect("pssz view: malformed bitlist")
    }
    #[inline]
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
