//! Views of one shape for every format (prototype; gofractally/psio#13).
//!
//! The principle is the one of the C++ library, where `psio::view<T, Fmt>`
//! is one template: what reading a `T` in place looks like does not depend
//! on the format, so code written against one format works with another by
//! changing a type parameter.
//!
//! ```ignore
//! #[derive(PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, View)]
//! #[view(pssz, flatbuf)]
//! struct Item { product: String, qty: u32 }
//!
//! #[derive(PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, View)]
//! #[view(pssz, flatbuf)]
//! struct Order { id: u64, items: Vec<Item>, note: Option<String> }
//!
//! // Written once; `F` is the format.
//! fn total<F: OrderFormat>(order: View<'_, Order, F>) -> u64 {
//!     let mut total = 0;
//!     for item in order.items() {
//!         if !item.product().is_empty() {
//!             total += item.qty() as u64;
//!         }
//!     }
//!     total
//! }
//!
//! total(View::<Order, Pssz>::new(&pssz_bytes)?);
//! total(View::<Order, Flatbuf>::new(&flatbuf_bytes)?);
//! ```
//!
//! # What a member reads as
//!
//! The same for every format ([`ViewShape`]):
//!
//! | member | reads as |
//! |---|---|
//! | integer, float, `bool` | the value |
//! | `String` | `&str` |
//! | record | [`View<'a, T, F>`](View), one method per member |
//! | `Vec<T>`, `[T; N]` | [`ListView<'a, _, F>`](ListView): `len`, `get`, `at`, `iter`, `for` |
//! | `Option<T>` | `Option` of what `T` reads as |
//!
//! # Validated and trusted
//!
//! A view is made in one of two ways, and the call says which:
//!
//! * [`View::new`] runs the format's validator first. Use it for bytes from
//!   outside.
//! * [`View::trusted`] does not: the caller vouches for the bytes (validated
//!   earlier, or produced by the format's encoder).
//!
//! Nothing converts one into the other, and nothing here writes: a view only
//! reads. Accessors are safe on any bytes in both cases. On bytes that
//! would not validate an accessor of a trusted view may panic (PSSZ) or read
//! a default (FlatBuffers); it never reads outside the buffer and never
//! makes an invalid value: a string member is checked to be UTF-8 when it
//! is read.
//!
//! # How it is put together
//!
//! * [`ViewTag`] is a format as far as views are concerned: the handle it
//!   needs to find a value ([`ViewTag::Cursor`]).
//! * [`ViewShape`] says what a type reads as. It is implemented once per
//!   type and does not mention any format, which is what lets the compiler
//!   know the shape in code that is generic over the format.
//! * [`RecordIn`], [`FieldIn`] and [`ListIn`] say how a type is read in one
//!   format. `#[derive(View)]` implements the first two for a record, for
//!   the formats named in `#[view(...)]`, on top of the format's own view
//!   (`PsszView`, `FbView`), which stay as they are.
//! * [`RecordFormat`], [`ReadsField`] and [`ReadsList`] are the same facts
//!   stated on the format. They exist because Rust carries a bound from a
//!   function's signature into its body only through the supertraits of the
//!   traits it names: `#[derive(View)]` writes a trait `<Record>Format`
//!   whose supertraits are everything reading the record needs, so
//!   `F: OrderFormat` is the only bound generic code states.
//!
//! The format of a record's members is [`RecordFormat::Beneath`]. It is the
//! format itself except in PSSZ, where the members of a record with a cap
//! are read at the offset widths the cap selects (`PsszUnder<C>`).

use std::marker::PhantomData;

/// A format, as far as views are concerned.
pub trait ViewTag: Sized {
    /// What finds one value in a buffer.
    type Cursor<'a>: Copy;
    /// What the format's validator reports.
    type Error;
}

/// What a type reads as, in any format.
pub trait ViewShape {
    type Shape<'a, F: ViewTag>;
}

/// A type that reads as a [`ListView`]: what its elements are, in any
/// format.
pub trait ListShape: ViewShape {
    type Elem: ViewShape;
}

// ── How a type is read in one format ────────────────────────────────────────

/// A record that can be read in format `F`.
pub trait RecordIn<F: ViewTag>: ViewShape {
    /// The format of the members.
    type Beneath: ViewTag;
    /// Whether `bytes` are one valid `Self`.
    fn validate(bytes: &[u8]) -> Result<(), F::Error>;
    /// The cursor of the record that `bytes` are.
    fn root(bytes: &[u8]) -> F::Cursor<'_>;
}

/// Member `I` of a record, in format `F`.
pub trait FieldIn<F: ViewTag, const I: usize>: RecordIn<F> {
    type Type: ViewShape;
    fn field<'a>(record: F::Cursor<'a>) -> <Self::Type as ViewShape>::Shape<'a, Self::Beneath>;
}

/// A list that can be read in format `F`.
pub trait ListIn<F: ViewTag>: ListShape {
    fn len(list: F::Cursor<'_>) -> usize;
    /// Element `i`; `i` is less than `len`.
    fn element<'a>(list: F::Cursor<'a>, i: usize) -> <Self::Elem as ViewShape>::Shape<'a, F>;
}

// ── The same, stated on the format ──────────────────────────────────────────

/// `Self` reads the record `R`.
pub trait RecordFormat<R: ViewShape>: ViewTag {
    type Beneath: ViewTag;
    fn validate(bytes: &[u8]) -> Result<(), Self::Error>;
    fn root(bytes: &[u8]) -> Self::Cursor<'_>;
}
impl<F: ViewTag, R: RecordIn<F>> RecordFormat<R> for F {
    type Beneath = R::Beneath;
    #[inline]
    fn validate(bytes: &[u8]) -> Result<(), F::Error> {
        R::validate(bytes)
    }
    #[inline]
    fn root(bytes: &[u8]) -> F::Cursor<'_> {
        R::root(bytes)
    }
}

/// `Self` reads member `I` of the record `R`.
pub trait ReadsField<R: ViewShape, const I: usize>: RecordFormat<R> {
    type Type: ViewShape;
    fn field<'a>(
        record: Self::Cursor<'a>,
    ) -> <Self::Type as ViewShape>::Shape<'a, <Self as RecordFormat<R>>::Beneath>;
}
impl<F: ViewTag, R: FieldIn<F, I>, const I: usize> ReadsField<R, I> for F {
    type Type = R::Type;
    #[inline]
    fn field<'a>(record: F::Cursor<'a>) -> <R::Type as ViewShape>::Shape<'a, R::Beneath> {
        R::field(record)
    }
}

/// `Self` reads the list `C`.
pub trait ReadsList<C: ListShape>: ViewTag {
    fn len(list: Self::Cursor<'_>) -> usize;
    fn element<'a>(list: Self::Cursor<'a>, i: usize) -> <C::Elem as ViewShape>::Shape<'a, Self>;
}
impl<F: ViewTag, C: ListIn<F>> ReadsList<C> for F {
    #[inline]
    fn len(list: F::Cursor<'_>) -> usize {
        C::len(list)
    }
    #[inline]
    fn element<'a>(list: F::Cursor<'a>, i: usize) -> <C::Elem as ViewShape>::Shape<'a, F> {
        C::element(list, i)
    }
}

// ── Records ─────────────────────────────────────────────────────────────────

/// A read-only view of a record `T` in format `F`.
///
/// The members are methods of the trait `<Record>View` that
/// `#[derive(View)]` defines beside the record. Everything else is an
/// associated function, so that it cannot hide a member's accessor.
pub struct View<'a, T: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    _type: PhantomData<(fn() -> *const T, fn() -> F)>,
}

impl<T: ?Sized, F: ViewTag> Clone for View<'_, T, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T: ?Sized, F: ViewTag> Copy for View<'_, T, F> {}

impl<T: ?Sized, F: ViewTag> std::fmt::Debug for View<'_, T, F> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("View").field("type", &std::any::type_name::<T>()).finish()
    }
}

impl<'a, T: ViewShape, F: RecordFormat<T>> View<'a, T, F> {
    /// Validates `bytes` as one `T`, then borrows them.
    #[inline]
    pub fn new(bytes: &'a [u8]) -> Result<Self, F::Error> {
        F::validate(bytes)?;
        Ok(Self::trusted(bytes))
    }

    /// A view of bytes the caller vouches for (validated earlier, or
    /// produced by the format's encoder); does not validate.
    #[inline]
    pub fn trusted(bytes: &'a [u8]) -> Self {
        Self::at(F::root(bytes))
    }
}

impl<'a, T: ?Sized, F: ViewTag> View<'a, T, F> {
    /// The view of the `T` a cursor finds. For the code that implements a
    /// format; a cursor is not validated bytes.
    #[inline]
    pub fn at(cursor: F::Cursor<'a>) -> Self {
        View { cursor, _type: PhantomData }
    }

    /// Where the record is. `View::cursor(&view)`: not a method, so that a
    /// member named `cursor` is still `view.cursor()`.
    #[inline]
    pub fn cursor(this: &Self) -> F::Cursor<'a> {
        this.cursor
    }
}

// ── Lists ───────────────────────────────────────────────────────────────────

/// A read-only view of a list `C` (`Vec<T>`, `[T; N]`) in format `F`.
pub struct ListView<'a, C: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    _type: PhantomData<(fn() -> *const C, fn() -> F)>,
}

impl<C: ?Sized, F: ViewTag> Clone for ListView<'_, C, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<C: ?Sized, F: ViewTag> Copy for ListView<'_, C, F> {}

impl<C: ?Sized, F: ViewTag> std::fmt::Debug for ListView<'_, C, F> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ListView").field("type", &std::any::type_name::<C>()).finish()
    }
}

impl<'a, C: ?Sized, F: ViewTag> ListView<'a, C, F> {
    /// The view of the list a cursor finds. For the code that implements a
    /// format.
    #[inline]
    pub fn at(cursor: F::Cursor<'a>) -> Self {
        ListView { cursor, _type: PhantomData }
    }
}

impl<'a, C: ListShape, F: ReadsList<C>> ListView<'a, C, F> {
    /// Number of elements.
    #[inline]
    pub fn len(&self) -> usize {
        F::len(self.cursor)
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Element `i`, or `None` past the end (as a slice's `get`).
    #[inline]
    pub fn get(&self, i: usize) -> Option<<C::Elem as ViewShape>::Shape<'a, F>> {
        (i < self.len()).then(|| F::element(self.cursor, i))
    }

    /// Element `i`; panics past the end (as indexing a slice).
    #[inline]
    #[track_caller]
    pub fn at_index(&self, i: usize) -> <C::Elem as ViewShape>::Shape<'a, F> {
        let len = self.len();
        assert!(i < len, "psio view: index {i} out of range for a list of {len}");
        F::element(self.cursor, i)
    }

    /// The elements, in order.
    #[inline]
    pub fn iter(&self) -> ListIter<'a, C, F> {
        ListIter { cursor: self.cursor, next: 0, end: self.len(), _type: PhantomData }
    }
}

/// The elements of a [`ListView`].
pub struct ListIter<'a, C: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    next: usize,
    end: usize,
    _type: PhantomData<(fn() -> *const C, fn() -> F)>,
}

impl<'a, C: ListShape, F: ReadsList<C>> Iterator for ListIter<'a, C, F> {
    type Item = <C::Elem as ViewShape>::Shape<'a, F>;
    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            let i = self.next;
            self.next += 1;
            Some(F::element(self.cursor, i))
        } else {
            None
        }
    }
    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        let n = self.end - self.next;
        (n, Some(n))
    }
}
impl<'a, C: ListShape, F: ReadsList<C>> DoubleEndedIterator for ListIter<'a, C, F> {
    #[inline]
    fn next_back(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            self.end -= 1;
            Some(F::element(self.cursor, self.end))
        } else {
            None
        }
    }
}
impl<'a, C: ListShape, F: ReadsList<C>> ExactSizeIterator for ListIter<'a, C, F> {}

impl<'a, C: ListShape, F: ReadsList<C>> IntoIterator for ListView<'a, C, F> {
    type Item = <C::Elem as ViewShape>::Shape<'a, F>;
    type IntoIter = ListIter<'a, C, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, C, F> {
        self.iter()
    }
}
impl<'a, C: ListShape, F: ReadsList<C>> IntoIterator for &ListView<'a, C, F> {
    type Item = <C::Elem as ViewShape>::Shape<'a, F>;
    type IntoIter = ListIter<'a, C, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, C, F> {
        self.iter()
    }
}

// ── Shapes of the standard types ────────────────────────────────────────────

macro_rules! value_shape {
    ($($ty:ty),*) => {$(
        impl ViewShape for $ty {
            type Shape<'a, F: ViewTag> = $ty;
        }
    )*};
}
value_shape!(bool, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64);

impl ViewShape for String {
    type Shape<'a, F: ViewTag> = &'a str;
}
impl<T: ViewShape> ViewShape for Option<T> {
    type Shape<'a, F: ViewTag> = Option<T::Shape<'a, F>>;
}
impl<T: ViewShape> ViewShape for Vec<T> {
    type Shape<'a, F: ViewTag> = ListView<'a, Vec<T>, F>;
}
impl<T: ViewShape, const N: usize> ViewShape for [T; N] {
    type Shape<'a, F: ViewTag> = ListView<'a, [T; N], F>;
}
impl<T: ViewShape> ListShape for Vec<T> {
    type Elem = T;
}
impl<T: ViewShape, const N: usize> ListShape for [T; N] {
    type Elem = T;
}

// ── PSSZ ────────────────────────────────────────────────────────────────────

/// Reading through the existing [`PsszView`](crate::PsszView).
pub mod pssz {
    use super::{ListIn, ListView, ViewShape, ViewTag};
    use crate::pssz::{PsszError, PsszFormat, PsszPack, PsszValidate};
    use crate::pssz_view::{PsszField, PsszView};

    /// Every PSSZ format tag (`Pssz`, `PsszUnder<C>`, the fixed-width
    /// ones) is a view format. The cursor of a value is its bytes.
    impl<F: PsszFormat> ViewTag for F {
        type Cursor<'a> = &'a [u8];
        type Error = PsszError;
    }

    /// A type whose PSSZ bytes, at a position of format `F`, read as its
    /// shape.
    pub trait PsszShape<F: PsszFormat>: ViewShape {
        fn from_pssz<'a>(bytes: &'a [u8]) -> Self::Shape<'a, F>;
    }

    macro_rules! value {
        ($($ty:ty),*) => {$(
            impl<F: PsszFormat> PsszShape<F> for $ty {
                #[inline]
                fn from_pssz<'a>(bytes: &'a [u8]) -> $ty {
                    PsszView::<$ty, F>::trusted(bytes).get()
                }
            }
        )*};
    }
    value!(bool, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64);

    impl<F: PsszFormat> PsszShape<F> for String {
        /// Panics on bytes that are not UTF-8, which only a view of bytes
        /// nobody validated can hold.
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> &'a str {
            crate::pssz::utf8_str(bytes).expect("psio view: a string member is not UTF-8; validate first")
        }
    }

    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static> PsszShape<F> for Option<T> {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> Option<T::Shape<'a, F>> {
            PsszView::<Option<T>, F>::trusted(bytes).get().map(|value| T::from_pssz(value.bytes()))
        }
    }

    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static> PsszShape<F> for Vec<T> {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> ListView<'a, Vec<T>, F> {
            ListView::at(bytes)
        }
    }
    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static> ListIn<F> for Vec<T> {
        #[inline]
        fn len(list: &[u8]) -> usize {
            PsszView::<Vec<T>, F>::trusted(list).len()
        }
        #[inline]
        fn element<'a>(list: &'a [u8], i: usize) -> T::Shape<'a, F>
        where
            'a: 'a,
        {
            T::from_pssz(PsszView::<Vec<T>, F>::trusted(list).get(i).bytes())
        }
    }

    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static, const N: usize> PsszShape<F> for [T; N] {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> ListView<'a, [T; N], F> {
            ListView::at(bytes)
        }
    }
    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static, const N: usize> ListIn<F> for [T; N] {
        #[inline]
        fn len(_list: &[u8]) -> usize {
            N
        }
        #[inline]
        fn element<'a>(list: &'a [u8], i: usize) -> T::Shape<'a, F>
        where
            'a: 'a,
        {
            T::from_pssz(PsszView::<[T; N], F>::trusted(list).get(i).bytes())
        }
    }

    /// Member `I` of the record `R` whose bytes are `record`, as what its
    /// type `T` reads as. What `#[derive(View)]` calls.
    #[inline]
    pub fn field<'a, R, F, T, const I: usize>(
        record: &'a [u8],
    ) -> T::Shape<'a, <R as PsszField<F, I>>::Beneath>
    where
        F: PsszFormat,
        R: PsszField<F, I> + ?Sized,
        T: PsszShape<<R as PsszField<F, I>>::Beneath>,
    {
        T::from_pssz(PsszView::<R, F>::trusted(record).field::<I>().bytes())
    }

    /// Whether `bytes` are one valid `R`: the validator of
    /// [`PsszView::new`].
    #[inline]
    pub fn validate<R, F>(bytes: &[u8]) -> Result<(), PsszError>
    where
        F: PsszFormat,
        R: PsszPack<F> + PsszValidate<F> + ?Sized,
    {
        PsszView::<R, F>::new(bytes).map(|_| ())
    }
}

// ── FlatBuffers ─────────────────────────────────────────────────────────────

/// Reading through the existing [`FbView`](crate::flatbuf::FbView).
#[cfg(feature = "flatbuf")]
pub mod flatbuf {
    use super::{ListIn, ListView, View, ViewShape, ViewTag};
    use crate::flatbuf::verify::check_buffer;
    use crate::flatbuf::{FbError, FbLimits, FbOptions, FbTable, FbUnpack, FbVec, FbView, FbViewable, Flatbuf};

    /// Where a table, a struct or a vector is in a buffer.
    #[derive(Clone, Copy, Debug)]
    pub struct FbCursor<'a> {
        pub buffer: &'a [u8],
        /// The position of a table, of a struct's first byte, or of a
        /// vector's first element.
        pub position: usize,
        /// A vector's number of elements.
        pub count: usize,
    }

    impl ViewTag for Flatbuf {
        type Cursor<'a> = FbCursor<'a>;
        type Error = FbError;
    }

    /// A type whose FlatBuffers view reads as its shape.
    pub trait FbShape: FbViewable + ViewShape {
        fn from_fb<'a>(view: <Self as FbViewable>::View<'a>) -> Self::Shape<'a, Flatbuf>;
    }

    macro_rules! value {
        ($($ty:ty),*) => {$(
            impl FbShape for $ty {
                #[inline]
                fn from_fb<'a>(view: $ty) -> $ty
        where
            'a: 'a,
        {
                    view
                }
            }
        )*};
    }
    value!(bool, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64);

    impl FbShape for String {
        #[inline]
        fn from_fb<'a>(view: &'a str) -> &'a str
        where
            'a: 'a,
        {
            view
        }
    }

    impl<T: FbShape> FbShape for Option<T> {
        #[inline]
        fn from_fb<'a>(view: Option<<T as FbViewable>::View<'a>>) -> Option<T::Shape<'a, Flatbuf>>
        where
            'a: 'a,
        {
            view.map(T::from_fb)
        }
    }

    #[inline]
    fn list_cursor<T: FbViewable>(view: FbVec<'_, T>) -> FbCursor<'_> {
        let (buffer, position, count) = view.into_parts();
        FbCursor { buffer, position, count }
    }
    #[inline]
    fn list_element<T: FbShape>(list: FbCursor<'_>, i: usize) -> T::Shape<'_, Flatbuf> {
        let view = FbVec::<T>::from_parts(list.buffer, list.position, list.count);
        T::from_fb(view.get(i).expect("psio view: element index within the list"))
    }

    impl<T: FbShape> FbShape for Vec<T> {
        #[inline]
        fn from_fb<'a>(view: FbVec<'a, T>) -> ListView<'a, Vec<T>, Flatbuf>
        where
            'a: 'a,
        {
            ListView::at(list_cursor(view))
        }
    }
    impl<T: FbShape> ListIn<Flatbuf> for Vec<T> {
        #[inline]
        fn len(list: FbCursor<'_>) -> usize {
            list.count
        }
        #[inline]
        fn element<'a>(list: FbCursor<'a>, i: usize) -> T::Shape<'a, Flatbuf>
        where
            'a: 'a,
        {
            list_element::<T>(list, i)
        }
    }

    impl<T: FbShape, const N: usize> FbShape for [T; N] {
        #[inline]
        fn from_fb<'a>(view: FbVec<'a, T>) -> ListView<'a, [T; N], Flatbuf>
        where
            'a: 'a,
        {
            ListView::at(list_cursor(view))
        }
    }
    impl<T: FbShape, const N: usize> ListIn<Flatbuf> for [T; N] {
        #[inline]
        fn len(list: FbCursor<'_>) -> usize {
            list.count
        }
        #[inline]
        fn element<'a>(list: FbCursor<'a>, i: usize) -> T::Shape<'a, Flatbuf>
        where
            'a: 'a,
        {
            list_element::<T>(list, i)
        }
    }

    /// The view of the record an `FbView` is of. What `#[derive(View)]`
    /// calls.
    #[inline]
    pub fn record<'a, R: FbUnpack>(view: FbView<'a, R>) -> View<'a, R, Flatbuf> {
        View::at(FbCursor { buffer: view.fb_buffer(), position: view.fb_position(), count: 0 })
    }

    /// The `FbView` of the record a cursor finds.
    #[inline]
    pub fn native<R: FbUnpack>(record: FbCursor<'_>) -> FbView<'_, R> {
        FbView::at(record.buffer, record.position)
    }

    /// The cursor of the root table of `bytes`; does not verify.
    #[inline]
    pub fn root<R: FbUnpack>(bytes: &[u8]) -> FbCursor<'_> {
        FbCursor { buffer: bytes, position: FbTable::root(bytes).position(), count: 0 }
    }

    /// Whether `bytes` are a buffer whose root is a valid `R`: the verifier
    /// of [`FbView::new`]. (A record that is a struct is never the root of
    /// a buffer; for it this is an error and not, as in `FbView::new`, a
    /// failure to compile, because every record implements `RecordIn`.)
    #[inline]
    pub fn validate<R: FbUnpack>(bytes: &[u8]) -> Result<(), FbError> {
        if !R::TABLE {
            return Err(FbError::new("flatbuf: the root of a buffer is a table", 0));
        }
        check_buffer(bytes, &FbOptions::default(), FbLimits::default(), R::fb_check_table)
    }
}
