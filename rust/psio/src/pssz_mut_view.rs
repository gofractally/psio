//! The mutable pSSZ view: the counterpart of C++ `psio::pssz_mutable_view<T>`
//! (docs/helpers.md, docs/pssz-spec.md §17).
//!
//! A [`PsszMutView`] is made from `&mut [u8]` and from nothing else: there is
//! no conversion from the read-only [`PsszView`], from shared bytes or from a
//! shared buffer, so code holding a read view cannot write through it. The
//! other direction is explicit and free: [`PsszMutView::as_view`].
//!
//! It has one write, [`PsszMutView::set`]: a fixed-size value is overwritten
//! by another value of the same type. Everything that would change a size or
//! the presence of an optional is a decode → modify → encode (§17.1); `set`
//! on a variable-size type does not compile.
//!
//! ```
//! use psio::{PsszBuffer, PsszMutView, PsszView};
//!
//! let mut bytes = psio::to_pssz(&vec![1u32, 2, 3]);
//! let mut writer = PsszMutView::<Vec<u32>>::new(&mut bytes)?;
//! writer.get_mut(1).set(&20)?;
//! assert_eq!(writer.as_view().get(1).get(), 20);          // mutable → read: explicit
//! assert_eq!(PsszView::<Vec<u32>>::new(&bytes)?.get(1).get(), 20);
//!
//! let mut owned = PsszBuffer::<u32>::encode(&7)?;
//! owned.as_mut_view().set(&8)?;                           // needs `&mut owned`
//! assert_eq!(owned.decode()?, 8);
//! # Ok::<(), psio::pssz::PsszError>(())
//! ```
//!
//! What does not compile:
//!
//! ```compile_fail
//! let bytes = psio::to_pssz(&7u32);
//! let view = psio::PsszView::<u32>::trusted(&bytes);
//! let writer: psio::PsszMutView<u32> = view.into();        // no conversion exists
//! ```
//! ```compile_fail
//! let bytes = psio::to_pssz(&7u32);
//! let writer = psio::PsszMutView::<u32>::trusted(&bytes);  // needs `&mut [u8]`
//! ```
//! ```compile_fail
//! let shared = psio::SharedPssz::<u32>::trusted(psio::to_pssz(&7u32));
//! let writer = shared.as_mut_view();                       // shared bytes are immutable
//! ```
//! ```compile_fail
//! let owned = psio::PsszBuffer::<u32>::trusted(psio::to_pssz(&7u32));
//! let writer = owned.as_mut_view();                        // `owned` is not `mut`
//! ```
//! ```compile_fail
//! let mut bytes = psio::to_pssz(&String::from("a"));
//! let mut writer = psio::PsszMutView::<String>::trusted(&mut bytes);
//! writer.set(&String::from("b"));                          // not a fixed-size type
//! ```

use super::{PsszField, PsszView};
use crate::pssz::{BoundedList, Pssz, PsszError, PsszFormat, PsszPack, PsszResult, PsszValidate};
use crate::pssz_checked as checked;
use std::marker::PhantomData;
use std::ops::Range;

/// Where `child` lies inside `parent`. `None` when it is not a subslice: a
/// pruned trailing optional reads as its static None encoding.
fn subrange(parent: &[u8], child: &[u8]) -> Option<Range<usize>> {
    if child.is_empty() {
        return Some(0..0);
    }
    let start = (child.as_ptr() as usize).checked_sub(parent.as_ptr() as usize)?;
    let end = start.checked_add(child.len())?;
    (end <= parent.len()).then_some(start..end)
}

/// Mutable zero-copy view of a `T` encoded under format tag `F`.
#[derive(Debug)]
pub struct PsszMutView<'a, T: ?Sized, F: PsszFormat = Pssz> {
    data: &'a mut [u8],
    _phantom: PhantomData<(fn() -> *const T, fn() -> F)>,
}

impl<'a, T: ?Sized, F: PsszFormat> PsszMutView<'a, T, F> {
    /// A mutable view of bytes the caller vouches for; does not validate.
    /// Access to malformed bytes may panic (never undefined behaviour).
    pub fn trusted(data: &'a mut [u8]) -> Self {
        PsszMutView { data, _phantom: PhantomData }
    }

    /// The read-only view of the same bytes, for as long as this view is
    /// not used to write.
    pub fn as_view(&self) -> PsszView<'_, T, F> {
        PsszView::trusted(self.data)
    }
    /// Gives up the ability to write.
    pub fn into_view(self) -> PsszView<'a, T, F> {
        PsszView::trusted(self.data)
    }
    /// A shorter-lived mutable view of the same value.
    pub fn reborrow(&mut self) -> PsszMutView<'_, T, F> {
        PsszMutView::trusted(self.data)
    }
    /// The bytes of the viewed value (read-only).
    pub fn bytes(&self) -> &[u8] {
        self.data
    }

    fn narrow<U: ?Sized>(self, child: PsszResult<Range<usize>>) -> PsszResult<PsszMutView<'a, U, F>> {
        let range = child?;
        self.data
            .get_mut(range)
            .map(PsszMutView::trusted)
            .ok_or(PsszError("pssz mutable view: span outside the buffer"))
    }

    /// Positional field access, consuming the view (keeps its lifetime).
    pub fn try_into_field<const INDEX: usize>(self) -> PsszResult<PsszMutView<'a, <T as PsszField<F, INDEX>>::Field, F>>
    where T: PsszField<F, INDEX> {
        if <T as PsszField<F, INDEX>>::TRANSPARENT {
            return Ok(PsszMutView::trusted(self.data));
        }
        let range = checked::field_bytes::<F, <T as PsszField<F, INDEX>>::Field>(
            self.data, <T as PsszField<F, INDEX>>::LOCATION, <T as PsszField<F, INDEX>>::DWNC,
        ).and_then(|field| subrange(self.data, field).ok_or(PsszError(
            "pssz mutable view: the field is not stored in the frame (a pruned trailing optional)",
        )));
        self.narrow(range)
    }
    /// Positional field access; an error on a malformed record or a field
    /// that is not stored (a pruned trailing optional).
    pub fn try_field_mut<const INDEX: usize>(&mut self) -> PsszResult<PsszMutView<'_, <T as PsszField<F, INDEX>>::Field, F>>
    where T: PsszField<F, INDEX> {
        self.reborrow().try_into_field::<INDEX>()
    }
    /// Positional field access; panics where [`Self::try_field_mut`] fails.
    pub fn field_mut<const INDEX: usize>(&mut self) -> PsszMutView<'_, <T as PsszField<F, INDEX>>::Field, F>
    where T: PsszField<F, INDEX> {
        self.try_field_mut::<INDEX>().expect("pssz mutable view: field not writable in place")
    }
}

impl<'a, T: PsszPack<F> + PsszValidate<F> + ?Sized, F: PsszFormat> PsszMutView<'a, T, F> {
    /// Validate, then borrow mutably. Accepts what `validate_pssz` accepts.
    pub fn new(data: &'a mut [u8]) -> PsszResult<Self> {
        crate::pssz::validate::<F, T>(data)?;
        Ok(Self::trusted(data))
    }
}

impl<'a, T: PsszPack<F>, F: PsszFormat> PsszMutView<'a, T, F> {
    /// Overwrites the fixed-size value this view designates with the
    /// encoding of `value` (§17.2). No other byte of the frame changes.
    ///
    /// Compiles only for a fixed-size `T`. Fails, leaving every byte
    /// unchanged, when `value` has no valid encoding (a type invariant) or
    /// the view does not designate `T::FIXED_SIZE` bytes (malformed input
    /// behind a `trusted` view).
    pub fn set(&mut self, value: &T) -> PsszResult<()> {
        const {
            assert!(
                T::IS_FIXED_SIZE,
                "PsszMutView::set: only a fixed-size value can be overwritten in place \
                 (decode, modify and encode a variable-size value)"
            )
        };
        if self.data.len() != T::FIXED_SIZE {
            return Err(PsszError("pssz mutable view: span is not the fixed size of the type"));
        }
        let mut encoded = Vec::with_capacity(T::FIXED_SIZE);
        value.pssz_pack_checked(&mut encoded)?;
        if encoded.len() != self.data.len() {
            return Err(PsszError("pssz mutable view: encoding is not the fixed size of the type"));
        }
        self.data.copy_from_slice(&encoded);
        Ok(())
    }
}

// ── Sequences ───────────────────────────────────────────────────────────────

fn element<F: PsszFormat, T: PsszPack<F>>(w: usize, data: &[u8], fixed_len: Option<usize>, i: usize) -> PsszResult<Range<usize>> {
    let n = checked::sequence_len::<F, T>(w, data, fixed_len)?;
    checked::sequence_span::<F, T>(w, data, n, i)
}

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat> PsszMutView<'a, Vec<T>, F> {
    pub fn len(&self) -> usize { self.as_view().len() }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    /// Element `i`; an error when out of range or malformed.
    pub fn try_get_mut(&mut self, i: usize) -> PsszResult<PsszMutView<'_, T, F>> {
        let range = element::<F, T>(<Vec<T> as PsszPack<F>>::OFFSET_WIDTH, self.data, None, i);
        self.reborrow().narrow(range)
    }
    pub fn get_mut(&mut self, i: usize) -> PsszMutView<'_, T, F> {
        self.try_get_mut(i).expect("pssz mutable view: invalid vector element")
    }
}

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat, const N: usize> PsszMutView<'a, BoundedList<T, N>, F> {
    pub fn len(&self) -> usize { self.as_view().len() }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn try_get_mut(&mut self, i: usize) -> PsszResult<PsszMutView<'_, T, F>> {
        let range = element::<F, T>(<Vec<T> as PsszPack<F>>::OFFSET_WIDTH, self.data, None, i);
        self.reborrow().narrow(range)
    }
    pub fn get_mut(&mut self, i: usize) -> PsszMutView<'_, T, F> {
        self.try_get_mut(i).expect("pssz mutable view: invalid list element")
    }
}

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat, const N: usize> PsszMutView<'a, [T; N], F> {
    pub fn try_get_mut(&mut self, i: usize) -> PsszResult<PsszMutView<'_, T, F>> {
        let range = element::<F, T>(<[T; N] as PsszPack<F>>::OFFSET_WIDTH, self.data, Some(N), i);
        self.reborrow().narrow(range)
    }
    pub fn get_mut(&mut self, i: usize) -> PsszMutView<'_, T, F> {
        self.try_get_mut(i).expect("pssz mutable view: invalid array element")
    }
}

// ── Option / Box ────────────────────────────────────────────────────────────

impl<'a, T: PsszPack<F> + 'static, F: PsszFormat> PsszMutView<'a, Option<T>, F> {
    pub fn has_value(&self) -> bool { self.as_view().has_value() }
    /// The payload when present. Presence itself cannot change in place.
    pub fn get_mut(&mut self) -> Option<PsszMutView<'_, T, F>> {
        if !self.as_view().try_has_value().ok()? {
            return None;
        }
        let skip = if T::MIN_ENCODED_SIZE == 0 { 1 } else { 0 };
        self.data.get_mut(skip..).map(PsszMutView::trusted)
    }
}

impl<'a, T: ?Sized, F: PsszFormat> PsszMutView<'a, Box<T>, F> {
    pub fn deref_mut(&mut self) -> PsszMutView<'_, T, F> { PsszMutView::trusted(self.data) }
}
