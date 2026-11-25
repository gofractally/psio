//! Views of one shape for every format (gofractally/psio#13).
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
//! // Written once; `F` is the format, and `Reads<Order>` the only bound.
//! fn total<F: Reads<Order>>(order: View<'_, Order, F>) -> u64 {
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
//! # What a value reads as
//!
//! The same for every format ([`ViewShape`]); [`View<'a, T, F>`](View) is
//! the name of it.
//!
//! | type | reads as |
//! |---|---|
//! | integer, float, `bool`, `Uint256` | the value |
//! | `String`, `BoundedString`, `&str` | `&str` |
//! | `RawString`, `BoundedRawString`, `&[u8]` | `&[u8]` |
//! | record | the struct `<Record>View<'a, F>` its derive writes: one method per member |
//! | `Vec<T>`, `[T; N]`, `BoundedList` | [`ListView<'a, _, F>`](ListView): `len`, `get`, `at_index`, `iter`, `for` |
//! | `BTreeMap<K, V>`, `BoundedMap` | [`MapView<'a, _, F>`](MapView): entries by ascending key, `get(key)` |
//! | `BTreeSet<K>`, `BoundedSet` | [`SetView<'a, _, F>`](SetView): keys ascending, `contains(key)` |
//! | `Option<T>` | `Option` of what `T` reads as |
//! | `Box<T>`, `Rc<T>`, `Arc<T>`, `RefCell<T>`, `&T` | what `T` reads as |
//! | `(A, B, ...)` | the tuple of what the elements read as |
//! | enum without payloads | the enum's value |
//! | enum with payloads | the enum `<Name>View<'a, F>` its derive writes: one alternative per variant, to `match` on |
//!
//! Not every format holds every type (FlatBuffers has no tuple, no list of
//! lists, no `Box`): what one generic function can read in several formats
//! is what all of them hold.
//!
//! # Validated and trusted
//!
//! A view is made in one of two ways, and the call says which:
//!
//! * `View::<T, F>::new(bytes)` and [`read`] run the format's validator
//!   first. Use them for bytes from outside.
//! * `View::<T, F>::trusted(bytes)` and [`read_trusted`] do not: the caller
//!   vouches for the bytes (validated earlier, or produced by the format's
//!   encoder).
//!
//! Nothing converts one into the other, nothing here writes, and every
//! function that makes a view of bytes without validating them has
//! `trusted` in its name. Accessors are safe on any bytes in both cases.
//! On bytes that would not validate, an accessor of a trusted view may
//! panic (PSSZ) or read a default (FlatBuffers; a string that is not UTF-8
//! reads as `""` there and panics in PSSZ); it never reads outside the
//! buffer and never makes an invalid value: a string is checked to be
//! UTF-8 when it is read.
//!
//! # How it is put together
//!
//! * [`ViewTag`] is a format as far as views are concerned.
//! * [`ViewShape`] says what a type reads as, and what its members are
//!   ([`ViewShape::Members`], a list of types: a record's members, a list's
//!   element, a map's key and value). It is implemented once per type and
//!   mentions no format.
//! * [`Reads<T>`](Reads) says that a format reads a `T` all the way down;
//!   it is the one bound of code generic over the format. Its
//!   [`Fields`](Reads::Fields) are one reader per member
//!   ([`FieldList`]), and each says at which format its member is read
//!   ([`FieldList::Beneath`]) and that this format reads the member's type:
//!   that is what makes the bound deep, without a trait per record and
//!   without `#[derive(View)]` having to know what kind of type a member
//!   is. A record may hold itself.
//! * A type says how it is read in the formats of one family in
//!   [`ViewIn`]: all the PSSZ format tags are one family, FlatBuffers is
//!   another. `#[derive(View)]` implements it for the formats named in
//!   `#[view(...)]`, on top of the format's own view (`PsszView`,
//!   `FbView`), which stay as they are.
//!
//! The format a member is read at is the format itself except in PSSZ,
//! where the members of a record with a cap are read at the offset widths
//! the cap selects (`PsszUnder<C>`).

use std::cmp::Ordering;
use std::marker::PhantomData;

/// A format, as far as views are concerned.
pub trait ViewTag: Sized {
    /// Where a value is in the bytes a view borrows. PSSZ: nothing (the
    /// bytes are the value's). FlatBuffers: a position in the buffer.
    type At: Copy;
    /// The formats that read a type the same way: what [`ViewIn`] is
    /// implemented for.
    type Family;
    /// What the format's validator reports.
    type Error: std::error::Error + Send + Sync + 'static;
}

/// What finds one value: the bytes a view borrows, and where the value is
/// in them. Made by a format's own code; it is not a claim that the bytes
/// were validated.
pub struct Cursor<'a, F: ViewTag> {
    pub(crate) bytes: &'a [u8],
    // (PSSZ finds a value by its bytes alone.)
    #[allow(dead_code)]
    pub(crate) at: F::At,
}
impl<F: ViewTag> Clone for Cursor<'_, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<F: ViewTag> Copy for Cursor<'_, F> {}

/// A list of types: the members of a record, the element of a list.
/// `()` is the empty list and `(Head, Tail)` a longer one.
pub trait TList {
    type Head: ViewShape;
    type Tail: TList;
}
/// What is past the end of a [`TList`].
pub struct End;
impl TList for () {
    type Head = End;
    type Tail = ();
}
impl<H: ViewShape, T: TList> TList for (H, T) {
    type Head = H;
    type Tail = T;
}

/// What a type reads as, in any format.
pub trait ViewShape: Sized {
    /// The types that are read through this one: a record's members, a
    /// list's element, a map's key and value, an optional's value, a
    /// tuple's elements, the payloads of an enum. `()` for a value.
    type Members: TList;
    /// What a `Self` reads as in the format `F`, whose readers of the
    /// members are `R`. [`View<'a, T, F>`](View) is the name to use: `R` is
    /// then what `F` reads the members of `T` with.
    ///
    /// A shape is `Copy`: it borrows, or is a value.
    type Shape<'a, F: ViewTag, R: FieldList<F, Self::Members>>: Copy;
}
impl ViewShape for End {
    type Members = ();
    type Shape<'a, F: ViewTag, R: FieldList<F, ()>> = ();
}

/// What a `T` reads as in the format `F`. For a record `Order` this is the
/// struct `OrderView<'a, F>` that `#[derive(View)]` writes, so
/// `View::<Order, Pssz>::new(bytes)` and `fn f<F: Reads<Order>>(order:
/// View<'_, Order, F>)` are how a view is made and named.
pub type View<'a, T, F> = <T as ViewShape>::Shape<'a, F, <F as Reads<T>>::Fields>;

/// `Self` reads a `T` all the way down. The one bound of code that is
/// generic over the format.
pub trait Reads<T: ViewShape>: ViewTag {
    /// One reader per member of `T`.
    type Fields: FieldList<Self, T::Members>;
    /// The number of elements of a list, map or set.
    fn len(list: Cursor<'_, Self>) -> usize;
    /// Whether `bytes` are one valid `T`.
    fn validate(bytes: &[u8]) -> Result<(), Self::Error>;
    /// What the `T` that `bytes` are reads as; does not validate.
    fn trusted_root(bytes: &[u8]) -> View<'_, T, Self>;
}

/// The readers of the members `L` of a value read at the format `F`: the
/// reader of the first, and the readers of the rest.
pub trait FieldList<F: ViewTag, L: TList> {
    /// The format the first member is read at, which reads its type.
    type Beneath: Reads<L::Head>;
    type Tail: FieldList<F, L::Tail>;
    /// The first member of the value at `value`: a record's member; element
    /// `i` of a list, key or value `i` of a map or set; the value of an
    /// optional that has one; the payload of an enum that holds it. Does
    /// not validate. Where there is no such member (an optional without a
    /// value, an index past the end) it panics, or reads as absent in a
    /// format whose views do not panic.
    fn trusted_head<'a>(value: Cursor<'a, F>, i: usize) -> View<'a, L::Head, Self::Beneath>;
}

/// The readers of no members.
pub struct Nil<F, B>(PhantomData<(F, B)>);
impl<F: ViewTag, B: Reads<End>> FieldList<F, ()> for Nil<F, B> {
    type Beneath = B;
    type Tail = Nil<F, B>;
    #[inline]
    fn trusted_head<'a>(_: Cursor<'a, F>, _: usize) {}
}

/// How a type is read in the formats of the family `Fam`: what a type
/// implements, once per family, for [`Reads`] to hold. (The family is a
/// parameter so that the implementations for two families do not overlap.)
pub trait ViewIn<Fam, F: ViewTag>: ViewShape {
    type Fields: FieldList<F, Self::Members>;
    /// The number of elements of a list, map or set; 0 for anything else.
    #[inline]
    fn len(_list: Cursor<'_, F>) -> usize {
        0
    }
    /// Whether `bytes` are one valid `Self`.
    fn validate(bytes: &[u8]) -> Result<(), F::Error>;
    /// What the `Self` that `bytes` are reads as; does not validate.
    fn trusted_root<'a>(bytes: &'a [u8]) -> Self::Shape<'a, F, Self::Fields>;
}
impl<F: ViewTag, T: ViewIn<F::Family, F>> Reads<T> for F {
    type Fields = T::Fields;
    #[inline]
    fn len(list: Cursor<'_, F>) -> usize {
        T::len(list)
    }
    #[inline]
    fn validate(bytes: &[u8]) -> Result<(), F::Error> {
        T::validate(bytes)
    }
    #[inline]
    fn trusted_root(bytes: &[u8]) -> T::Shape<'_, F, T::Fields> {
        T::trusted_root(bytes)
    }
}

/// `Self` decodes the owned `T` a cursor finds: what `unpack()` of a view
/// gives. The decoder is the format's trusted one: it does not validate,
/// and on bytes that would not validate it panics or (FlatBuffers) reads
/// defaults.
pub trait Unpacks<T>: ViewTag {
    fn trusted_unpack(value: Cursor<'_, Self>) -> T;
}

/// Validates `bytes` as one `T` in the format `F`, then gives what a `T`
/// reads as. For a record this is `View::<T, F>::new`; it is also how the
/// root of a buffer that is not a record (a list, an enum, a string) is
/// read.
#[inline]
pub fn read<T: ViewShape, F: Reads<T>>(bytes: &[u8]) -> Result<View<'_, T, F>, F::Error> {
    F::validate(bytes)?;
    Ok(F::trusted_root(bytes))
}

/// What the `T` that `bytes` are reads as, of bytes the caller vouches for
/// (validated earlier, or produced by the format's encoder); does not
/// validate.
#[inline]
pub fn read_trusted<T: ViewShape, F: Reads<T>>(bytes: &[u8]) -> View<'_, T, F> {
    F::trusted_root(bytes)
}

// ── The members of a type, by position ──────────────────────────────────────
#[doc(hidden)]
pub trait Third {
    type Is;
}
impl<T, F, R> Third for (T, F, R) {
    type Is = R;
}
#[doc(hidden)] pub type L0<T> = <T as ViewShape>::Members;
#[doc(hidden)] pub type L1<T> = <L0<T> as TList>::Tail;
#[doc(hidden)] pub type L2<T> = <L1<T> as TList>::Tail;
#[doc(hidden)] pub type L3<T> = <L2<T> as TList>::Tail;
#[doc(hidden)] pub type L4<T> = <L3<T> as TList>::Tail;
#[doc(hidden)] pub type L5<T> = <L4<T> as TList>::Tail;
#[doc(hidden)] pub type L6<T> = <L5<T> as TList>::Tail;
#[doc(hidden)] pub type L7<T> = <L6<T> as TList>::Tail;
#[doc(hidden)] pub type L8<T> = <L7<T> as TList>::Tail;
#[doc(hidden)] pub type L9<T> = <L8<T> as TList>::Tail;
#[doc(hidden)] pub type L10<T> = <L9<T> as TList>::Tail;
#[doc(hidden)] pub type L11<T> = <L10<T> as TList>::Tail;
#[doc(hidden)] pub type X0<T, F, R> = <(T, F, R) as Third>::Is;
#[doc(hidden)] pub type X1<T, F, R> = <X0<T, F, R> as FieldList<F, L0<T>>>::Tail;
#[doc(hidden)] pub type X2<T, F, R> = <X1<T, F, R> as FieldList<F, L1<T>>>::Tail;
#[doc(hidden)] pub type X3<T, F, R> = <X2<T, F, R> as FieldList<F, L2<T>>>::Tail;
#[doc(hidden)] pub type X4<T, F, R> = <X3<T, F, R> as FieldList<F, L3<T>>>::Tail;
#[doc(hidden)] pub type X5<T, F, R> = <X4<T, F, R> as FieldList<F, L4<T>>>::Tail;
#[doc(hidden)] pub type X6<T, F, R> = <X5<T, F, R> as FieldList<F, L5<T>>>::Tail;
#[doc(hidden)] pub type X7<T, F, R> = <X6<T, F, R> as FieldList<F, L6<T>>>::Tail;
#[doc(hidden)] pub type X8<T, F, R> = <X7<T, F, R> as FieldList<F, L7<T>>>::Tail;
#[doc(hidden)] pub type X9<T, F, R> = <X8<T, F, R> as FieldList<F, L8<T>>>::Tail;
#[doc(hidden)] pub type X10<T, F, R> = <X9<T, F, R> as FieldList<F, L9<T>>>::Tail;
#[doc(hidden)] pub type X11<T, F, R> = <X10<T, F, R> as FieldList<F, L10<T>>>::Tail;
#[doc(hidden)] pub type H0<T> = <L0<T> as TList>::Head;
#[doc(hidden)] pub type Y0<T, F, R> = <X0<T, F, R> as FieldList<F, L0<T>>>::Beneath;
/// What member `0` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z0<'a, T, F, R> = View<'a, H0<T>, Y0<T, F, R>>;
/// The format member `0` of a `T` read at `F` is read at.
pub type B0<T, F> = Y0<T, F, <F as Reads<T>>::Fields>;
/// What member `0` of a `T` read at `F` reads as.
pub type S0<'a, T, F> = Z0<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H1<T> = <L1<T> as TList>::Head;
#[doc(hidden)] pub type Y1<T, F, R> = <X1<T, F, R> as FieldList<F, L1<T>>>::Beneath;
/// What member `1` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z1<'a, T, F, R> = View<'a, H1<T>, Y1<T, F, R>>;
/// The format member `1` of a `T` read at `F` is read at.
pub type B1<T, F> = Y1<T, F, <F as Reads<T>>::Fields>;
/// What member `1` of a `T` read at `F` reads as.
pub type S1<'a, T, F> = Z1<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H2<T> = <L2<T> as TList>::Head;
#[doc(hidden)] pub type Y2<T, F, R> = <X2<T, F, R> as FieldList<F, L2<T>>>::Beneath;
/// What member `2` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z2<'a, T, F, R> = View<'a, H2<T>, Y2<T, F, R>>;
/// The format member `2` of a `T` read at `F` is read at.
pub type B2<T, F> = Y2<T, F, <F as Reads<T>>::Fields>;
/// What member `2` of a `T` read at `F` reads as.
pub type S2<'a, T, F> = Z2<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H3<T> = <L3<T> as TList>::Head;
#[doc(hidden)] pub type Y3<T, F, R> = <X3<T, F, R> as FieldList<F, L3<T>>>::Beneath;
/// What member `3` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z3<'a, T, F, R> = View<'a, H3<T>, Y3<T, F, R>>;
/// The format member `3` of a `T` read at `F` is read at.
pub type B3<T, F> = Y3<T, F, <F as Reads<T>>::Fields>;
/// What member `3` of a `T` read at `F` reads as.
pub type S3<'a, T, F> = Z3<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H4<T> = <L4<T> as TList>::Head;
#[doc(hidden)] pub type Y4<T, F, R> = <X4<T, F, R> as FieldList<F, L4<T>>>::Beneath;
/// What member `4` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z4<'a, T, F, R> = View<'a, H4<T>, Y4<T, F, R>>;
/// The format member `4` of a `T` read at `F` is read at.
pub type B4<T, F> = Y4<T, F, <F as Reads<T>>::Fields>;
/// What member `4` of a `T` read at `F` reads as.
pub type S4<'a, T, F> = Z4<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H5<T> = <L5<T> as TList>::Head;
#[doc(hidden)] pub type Y5<T, F, R> = <X5<T, F, R> as FieldList<F, L5<T>>>::Beneath;
/// What member `5` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z5<'a, T, F, R> = View<'a, H5<T>, Y5<T, F, R>>;
/// The format member `5` of a `T` read at `F` is read at.
pub type B5<T, F> = Y5<T, F, <F as Reads<T>>::Fields>;
/// What member `5` of a `T` read at `F` reads as.
pub type S5<'a, T, F> = Z5<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H6<T> = <L6<T> as TList>::Head;
#[doc(hidden)] pub type Y6<T, F, R> = <X6<T, F, R> as FieldList<F, L6<T>>>::Beneath;
/// What member `6` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z6<'a, T, F, R> = View<'a, H6<T>, Y6<T, F, R>>;
/// The format member `6` of a `T` read at `F` is read at.
pub type B6<T, F> = Y6<T, F, <F as Reads<T>>::Fields>;
/// What member `6` of a `T` read at `F` reads as.
pub type S6<'a, T, F> = Z6<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H7<T> = <L7<T> as TList>::Head;
#[doc(hidden)] pub type Y7<T, F, R> = <X7<T, F, R> as FieldList<F, L7<T>>>::Beneath;
/// What member `7` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z7<'a, T, F, R> = View<'a, H7<T>, Y7<T, F, R>>;
/// The format member `7` of a `T` read at `F` is read at.
pub type B7<T, F> = Y7<T, F, <F as Reads<T>>::Fields>;
/// What member `7` of a `T` read at `F` reads as.
pub type S7<'a, T, F> = Z7<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H8<T> = <L8<T> as TList>::Head;
#[doc(hidden)] pub type Y8<T, F, R> = <X8<T, F, R> as FieldList<F, L8<T>>>::Beneath;
/// What member `8` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z8<'a, T, F, R> = View<'a, H8<T>, Y8<T, F, R>>;
/// The format member `8` of a `T` read at `F` is read at.
pub type B8<T, F> = Y8<T, F, <F as Reads<T>>::Fields>;
/// What member `8` of a `T` read at `F` reads as.
pub type S8<'a, T, F> = Z8<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H9<T> = <L9<T> as TList>::Head;
#[doc(hidden)] pub type Y9<T, F, R> = <X9<T, F, R> as FieldList<F, L9<T>>>::Beneath;
/// What member `9` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z9<'a, T, F, R> = View<'a, H9<T>, Y9<T, F, R>>;
/// The format member `9` of a `T` read at `F` is read at.
pub type B9<T, F> = Y9<T, F, <F as Reads<T>>::Fields>;
/// What member `9` of a `T` read at `F` reads as.
pub type S9<'a, T, F> = Z9<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H10<T> = <L10<T> as TList>::Head;
#[doc(hidden)] pub type Y10<T, F, R> = <X10<T, F, R> as FieldList<F, L10<T>>>::Beneath;
/// What member `10` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z10<'a, T, F, R> = View<'a, H10<T>, Y10<T, F, R>>;
/// The format member `10` of a `T` read at `F` is read at.
pub type B10<T, F> = Y10<T, F, <F as Reads<T>>::Fields>;
/// What member `10` of a `T` read at `F` reads as.
pub type S10<'a, T, F> = Z10<'a, T, F, <F as Reads<T>>::Fields>;
#[doc(hidden)] pub type H11<T> = <L11<T> as TList>::Head;
#[doc(hidden)] pub type Y11<T, F, R> = <X11<T, F, R> as FieldList<F, L11<T>>>::Beneath;
/// What member `11` of a `T` reads as, read at `F` by the readers `R`.
#[doc(hidden)] pub type Z11<'a, T, F, R> = View<'a, H11<T>, Y11<T, F, R>>;
/// The format member `11` of a `T` read at `F` is read at.
pub type B11<T, F> = Y11<T, F, <F as Reads<T>>::Fields>;
/// What member `11` of a `T` read at `F` reads as.
pub type S11<'a, T, F> = Z11<'a, T, F, <F as Reads<T>>::Fields>;

// ── Lists ───────────────────────────────────────────────────────────────────

/// A read-only view of a list `C` (`Vec<T>`, `[T; N]`, `BoundedList`) in
/// the format `F`.
pub struct ListView<'a, C, F: ViewTag> {
    cursor: Cursor<'a, F>,
    _type: PhantomData<fn() -> C>,
}

impl<C, F: ViewTag> Clone for ListView<'_, C, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<C, F: ViewTag> Copy for ListView<'_, C, F> {}

/// Prints the elements.
impl<'a, C: ViewShape, F: Reads<C>> std::fmt::Debug for ListView<'a, C, F>
where
    S0<'a, C, F>: std::fmt::Debug,
{
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

impl<'a, C, F: ViewTag> ListView<'a, C, F> {
    /// The view of the list a cursor finds; does not validate. For the
    /// code that implements a format.
    #[doc(hidden)]
    #[inline]
    pub fn trusted_at(cursor: Cursor<'a, F>) -> Self {
        ListView { cursor, _type: PhantomData }
    }
}

impl<C, F: Unpacks<C>> ListView<'_, C, F> {
    /// The owned list.
    #[inline]
    pub fn unpack(&self) -> C {
        F::trusted_unpack(self.cursor)
    }
}

impl<'a, C: ViewShape, F: Reads<C>> ListView<'a, C, F> {
    #[inline]
    fn element(cursor: Cursor<'a, F>, i: usize) -> S0<'a, C, F> {
        <<F as Reads<C>>::Fields as FieldList<F, L0<C>>>::trusted_head(cursor, i)
    }

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
    pub fn get(&self, i: usize) -> Option<S0<'a, C, F>> {
        (i < self.len()).then(|| Self::element(self.cursor, i))
    }

    /// Element `i`; panics past the end (as indexing a slice).
    #[inline]
    #[track_caller]
    pub fn at_index(&self, i: usize) -> S0<'a, C, F> {
        let len = self.len();
        assert!(i < len, "psio view: index {i} out of range for a list of {len}");
        Self::element(self.cursor, i)
    }

    /// The first element.
    #[inline]
    pub fn first(&self) -> Option<S0<'a, C, F>> {
        self.get(0)
    }

    /// The last element.
    #[inline]
    pub fn last(&self) -> Option<S0<'a, C, F>> {
        self.len().checked_sub(1).map(|i| Self::element(self.cursor, i))
    }

    /// The elements, in order.
    #[inline]
    pub fn iter(&self) -> ListIter<'a, C, F> {
        ListIter { cursor: self.cursor, next: 0, end: self.len(), _type: PhantomData }
    }
}

impl<'a, T: ViewShape, F: Reads<[T; N]>, const N: usize> ListView<'a, [T; N], F> {
    /// The elements as an array: an array of views to index and to `match`
    /// on. Panics when the list is shorter than the type says, which only a
    /// view of bytes nobody validated can be.
    #[inline]
    pub fn to_array(&self) -> [S0<'a, [T; N], F>; N] {
        let len = self.len();
        assert!(len >= N, "psio view: an array of {N} holds {len}; validate first");
        std::array::from_fn(|i| Self::element(self.cursor, i))
    }

    /// `f` of every element, as an array.
    #[inline]
    pub fn map<U>(&self, f: impl FnMut(S0<'a, [T; N], F>) -> U) -> [U; N] {
        self.to_array().map(f)
    }
}

/// The elements of a [`ListView`].
pub struct ListIter<'a, C, F: ViewTag> {
    cursor: Cursor<'a, F>,
    next: usize,
    end: usize,
    _type: PhantomData<fn() -> C>,
}

impl<'a, C: ViewShape, F: Reads<C>> Iterator for ListIter<'a, C, F> {
    type Item = S0<'a, C, F>;
    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            let i = self.next;
            self.next += 1;
            Some(ListView::<C, F>::element(self.cursor, i))
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
impl<'a, C: ViewShape, F: Reads<C>> DoubleEndedIterator for ListIter<'a, C, F> {
    #[inline]
    fn next_back(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            self.end -= 1;
            Some(ListView::<C, F>::element(self.cursor, self.end))
        } else {
            None
        }
    }
}
impl<'a, C: ViewShape, F: Reads<C>> ExactSizeIterator for ListIter<'a, C, F> {}

impl<'a, C: ViewShape, F: Reads<C>> IntoIterator for ListView<'a, C, F> {
    type Item = S0<'a, C, F>;
    type IntoIter = ListIter<'a, C, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, C, F> {
        self.iter()
    }
}
impl<'a, C: ViewShape, F: Reads<C>> IntoIterator for &ListView<'a, C, F> {
    type Item = S0<'a, C, F>;
    type IntoIter = ListIter<'a, C, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, C, F> {
        self.iter()
    }
}

// ── Maps ────────────────────────────────────────────────────────────────────

/// The first index in `0..count` at which `cmp` is not `Less`. Terminates
/// and stays in range whatever the order of what is stored.
#[inline]
fn lower_bound(count: usize, mut cmp: impl FnMut(usize) -> Option<Ordering>) -> usize {
    let (mut lo, mut hi) = (0, count);
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        if cmp(mid) == Some(Ordering::Less) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    lo
}

/// A read-only view of a map `M` (`BTreeMap<K, V>`, `BoundedMap`) in the
/// format `F`: its entries by ascending key.
pub struct MapView<'a, M, F: ViewTag> {
    cursor: Cursor<'a, F>,
    _type: PhantomData<fn() -> M>,
}

impl<M, F: ViewTag> Clone for MapView<'_, M, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<M, F: ViewTag> Copy for MapView<'_, M, F> {}

/// Prints the entries.
impl<'a, M: ViewShape, F: Reads<M>> std::fmt::Debug for MapView<'a, M, F>
where
    S0<'a, M, F>: std::fmt::Debug,
    S1<'a, M, F>: std::fmt::Debug,
{
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_map().entries(self.iter()).finish()
    }
}

impl<'a, M, F: ViewTag> MapView<'a, M, F> {
    /// The view of the map a cursor finds; does not validate. For the code
    /// that implements a format.
    #[doc(hidden)]
    #[inline]
    pub fn trusted_at(cursor: Cursor<'a, F>) -> Self {
        MapView { cursor, _type: PhantomData }
    }
}

impl<M, F: Unpacks<M>> MapView<'_, M, F> {
    /// The owned map.
    #[inline]
    pub fn unpack(&self) -> M {
        F::trusted_unpack(self.cursor)
    }
}

impl<'a, M: ViewShape, F: Reads<M>> MapView<'a, M, F> {
    #[inline]
    fn key(cursor: Cursor<'a, F>, i: usize) -> S0<'a, M, F> {
        <<F as Reads<M>>::Fields as FieldList<F, L0<M>>>::trusted_head(cursor, i)
    }
    #[inline]
    fn value(cursor: Cursor<'a, F>, i: usize) -> S1<'a, M, F> {
        <X1<M, F, <F as Reads<M>>::Fields> as FieldList<F, L1<M>>>::trusted_head(cursor, i)
    }

    /// Number of entries.
    #[inline]
    pub fn len(&self) -> usize {
        F::len(self.cursor)
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Entry `i` in ascending key order, or `None` past the end.
    #[inline]
    pub fn entry(&self, i: usize) -> Option<(S0<'a, M, F>, S1<'a, M, F>)> {
        (i < self.len()).then(|| (Self::key(self.cursor, i), Self::value(self.cursor, i)))
    }

    /// The entries as `(key, value)`, by ascending key.
    #[inline]
    pub fn iter(&self) -> MapIter<'a, M, F> {
        MapIter { cursor: self.cursor, next: 0, end: self.len(), _type: PhantomData }
    }

    /// The keys, ascending.
    #[inline]
    pub fn keys(&self) -> MapKeys<'a, M, F> {
        MapKeys(self.iter())
    }

    /// The mapped values, by ascending key.
    #[inline]
    pub fn values(&self) -> MapValues<'a, M, F> {
        MapValues(self.iter())
    }

    /// The index of the entry of `key`. Binary search: the entries are in
    /// ascending key order in every format (the validator checks it; over
    /// bytes nobody validated the answer may be wrong, never out of range).
    #[inline]
    pub fn find<Q>(&self, key: Q) -> Option<usize>
    where
        S0<'a, M, F>: PartialOrd<Q>,
    {
        let len = self.len();
        let i = lower_bound(len, |i| Self::key(self.cursor, i).partial_cmp(&key));
        (i < len && Self::key(self.cursor, i).partial_cmp(&key) == Some(Ordering::Equal)).then_some(i)
    }

    /// The value of `key`: `map.get("name")`, `map.get(7)`.
    #[inline]
    pub fn get<Q>(&self, key: Q) -> Option<S1<'a, M, F>>
    where
        S0<'a, M, F>: PartialOrd<Q>,
    {
        self.find(key).map(|i| Self::value(self.cursor, i))
    }

    #[inline]
    pub fn contains_key<Q>(&self, key: Q) -> bool
    where
        S0<'a, M, F>: PartialOrd<Q>,
    {
        self.find(key).is_some()
    }
}

/// The entries of a [`MapView`].
pub struct MapIter<'a, M, F: ViewTag> {
    cursor: Cursor<'a, F>,
    next: usize,
    end: usize,
    _type: PhantomData<fn() -> M>,
}
/// The keys of a [`MapView`].
pub struct MapKeys<'a, M, F: ViewTag>(MapIter<'a, M, F>);
/// The mapped values of a [`MapView`].
pub struct MapValues<'a, M, F: ViewTag>(MapIter<'a, M, F>);

macro_rules! map_iterator {
    ($name:ident, $item:ty, |$cursor:ident, $i:ident| $read:expr, |$this:ident| $entries:expr, $shared:expr) => {
        impl<'a, M: ViewShape, F: Reads<M>> Iterator for $name<'a, M, F> {
            type Item = $item;
            #[inline]
            fn next(&mut self) -> Option<Self::Item> {
                let $this = self;
                let entries: &mut MapIter<'a, M, F> = $entries;
                if entries.next < entries.end {
                    let ($cursor, $i) = (entries.cursor, entries.next);
                    entries.next += 1;
                    Some($read)
                } else {
                    None
                }
            }
            #[inline]
            fn size_hint(&self) -> (usize, Option<usize>) {
                let $this = self;
                let entries: &MapIter<'a, M, F> = $shared;
                let n = entries.end - entries.next;
                (n, Some(n))
            }
        }
        impl<'a, M: ViewShape, F: Reads<M>> DoubleEndedIterator for $name<'a, M, F> {
            #[inline]
            fn next_back(&mut self) -> Option<Self::Item> {
                let $this = self;
                let entries: &mut MapIter<'a, M, F> = $entries;
                if entries.next < entries.end {
                    entries.end -= 1;
                    let ($cursor, $i) = (entries.cursor, entries.end);
                    Some($read)
                } else {
                    None
                }
            }
        }
        impl<'a, M: ViewShape, F: Reads<M>> ExactSizeIterator for $name<'a, M, F> {}
    };
}
map_iterator!(MapIter, (S0<'a, M, F>, S1<'a, M, F>),
    |cursor, i| (MapView::<M, F>::key(cursor, i), MapView::<M, F>::value(cursor, i)), |this| this, this);
map_iterator!(MapKeys, S0<'a, M, F>, |cursor, i| MapView::<M, F>::key(cursor, i), |this| &mut this.0, &this.0);
map_iterator!(MapValues, S1<'a, M, F>, |cursor, i| MapView::<M, F>::value(cursor, i), |this| &mut this.0, &this.0);

impl<'a, M: ViewShape, F: Reads<M>> IntoIterator for MapView<'a, M, F> {
    type Item = (S0<'a, M, F>, S1<'a, M, F>);
    type IntoIter = MapIter<'a, M, F>;
    #[inline]
    fn into_iter(self) -> MapIter<'a, M, F> {
        self.iter()
    }
}
impl<'a, M: ViewShape, F: Reads<M>> IntoIterator for &MapView<'a, M, F> {
    type Item = (S0<'a, M, F>, S1<'a, M, F>);
    type IntoIter = MapIter<'a, M, F>;
    #[inline]
    fn into_iter(self) -> MapIter<'a, M, F> {
        self.iter()
    }
}

// ── Sets ────────────────────────────────────────────────────────────────────

/// A read-only view of a set `S` (`BTreeSet<K>`, `BoundedSet`) in the
/// format `F`: its keys, ascending.
pub struct SetView<'a, S, F: ViewTag> {
    cursor: Cursor<'a, F>,
    _type: PhantomData<fn() -> S>,
}

impl<S, F: ViewTag> Clone for SetView<'_, S, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<S, F: ViewTag> Copy for SetView<'_, S, F> {}

/// Prints the keys.
impl<'a, S: ViewShape, F: Reads<S>> std::fmt::Debug for SetView<'a, S, F>
where
    S0<'a, S, F>: std::fmt::Debug,
{
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_set().entries(self.iter()).finish()
    }
}

impl<'a, S, F: ViewTag> SetView<'a, S, F> {
    /// The view of the set a cursor finds; does not validate. For the code
    /// that implements a format.
    #[doc(hidden)]
    #[inline]
    pub fn trusted_at(cursor: Cursor<'a, F>) -> Self {
        SetView { cursor, _type: PhantomData }
    }
}

impl<S, F: Unpacks<S>> SetView<'_, S, F> {
    /// The owned set.
    #[inline]
    pub fn unpack(&self) -> S {
        F::trusted_unpack(self.cursor)
    }
}

impl<'a, S: ViewShape, F: Reads<S>> SetView<'a, S, F> {
    /// As a list of the keys, ascending: `len`, `get(i)`, `iter`, `for`.
    #[inline]
    fn keys(&self) -> ListView<'a, S, F> {
        ListView::trusted_at(self.cursor)
    }

    /// Number of keys.
    #[inline]
    pub fn len(&self) -> usize {
        F::len(self.cursor)
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Key `i` in ascending order, or `None` past the end.
    #[inline]
    pub fn get(&self, i: usize) -> Option<S0<'a, S, F>> {
        self.keys().get(i)
    }

    /// The keys, ascending.
    #[inline]
    pub fn iter(&self) -> ListIter<'a, S, F> {
        self.keys().iter()
    }

    /// The index of `key`. Binary search, as [`MapView::find`].
    #[inline]
    pub fn find<Q>(&self, key: Q) -> Option<usize>
    where
        S0<'a, S, F>: PartialOrd<Q>,
    {
        let len = self.len();
        let read = |i| ListView::<S, F>::element(self.cursor, i);
        let i = lower_bound(len, |i| read(i).partial_cmp(&key));
        (i < len && read(i).partial_cmp(&key) == Some(Ordering::Equal)).then_some(i)
    }

    /// Whether `key` is in the set: `set.contains("name")`.
    #[inline]
    pub fn contains<Q>(&self, key: Q) -> bool
    where
        S0<'a, S, F>: PartialOrd<Q>,
    {
        self.find(key).is_some()
    }
}

impl<'a, S: ViewShape, F: Reads<S>> IntoIterator for SetView<'a, S, F> {
    type Item = S0<'a, S, F>;
    type IntoIter = ListIter<'a, S, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, S, F> {
        self.iter()
    }
}
impl<'a, S: ViewShape, F: Reads<S>> IntoIterator for &SetView<'a, S, F> {
    type Item = S0<'a, S, F>;
    type IntoIter = ListIter<'a, S, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, S, F> {
        self.iter()
    }
}

// ── Shapes of the library's types ───────────────────────────────────────────

use crate::pssz::{BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, RawString, Uint256};
use std::collections::{BTreeMap, BTreeSet};

macro_rules! value_shape {
    ($($ty:ty),*) => {$(
        impl ViewShape for $ty {
            type Members = ();
            type Shape<'a, F: ViewTag, R: FieldList<F, ()>> = $ty;
        }
    )*};
}
value_shape!(bool, i8, i16, i32, i64, i128, u8, u16, u32, u64, u128, f32, f64, Uint256);

macro_rules! text_shape {
    ($([$($generics:tt)*] $ty:ty => $shape:ty),*) => {$(
        impl<$($generics)*> ViewShape for $ty {
            type Members = ();
            type Shape<'a, F: ViewTag, R: FieldList<F, ()>> = $shape;
        }
    )*};
}
text_shape!([] String => &'a str, [const N: usize] BoundedString<N> => &'a str, ['x] &'x str => &'a str,
            [] RawString => &'a [u8], [const N: usize] BoundedRawString<N> => &'a [u8], ['x] &'x [u8] => &'a [u8]);

impl<T: ViewShape> ViewShape for Option<T> {
    type Members = (T, ());
    type Shape<'a, F: ViewTag, R: FieldList<F, (T, ())>> = Option<Z0<'a, Self, F, R>>;
}
// What holds one value reads as that value does: a reference (the member
// of a record that borrows), and the owning pointers.
macro_rules! wrapper_shape {
    ($([$($generics:tt)*] $ty:ty),*) => {$(
        impl<$($generics)* T: ViewShape> ViewShape for $ty {
            type Members = (T, ());
            type Shape<'a, F: ViewTag, R: FieldList<F, (T, ())>> = Z0<'a, Self, F, R>;
        }
    )*};
}
wrapper_shape!(['x,] &'x T, [] Box<T>, [] std::rc::Rc<T>, [] std::sync::Arc<T>, [] std::cell::RefCell<T>);

macro_rules! list_shape {
    ($([$($generics:tt)*] $ty:ty),*) => {$(
        impl<T: ViewShape, $($generics)*> ViewShape for $ty {
            type Members = (T, ());
            type Shape<'a, F: ViewTag, R: FieldList<F, (T, ())>> = ListView<'a, Self, F>;
        }
    )*};
}
list_shape!([] Vec<T>, [const N: usize] [T; N], [const N: usize] BoundedList<T, N>);

impl<K: ViewShape, V: ViewShape> ViewShape for BTreeMap<K, V> {
    type Members = (K, (V, ()));
    type Shape<'a, F: ViewTag, R: FieldList<F, (K, (V, ()))>> = MapView<'a, Self, F>;
}
impl<K: ViewShape, V: ViewShape, const N: usize> ViewShape for BoundedMap<K, V, N> {
    type Members = (K, (V, ()));
    type Shape<'a, F: ViewTag, R: FieldList<F, (K, (V, ()))>> = MapView<'a, Self, F>;
}
impl<K: ViewShape> ViewShape for BTreeSet<K> {
    type Members = (K, ());
    type Shape<'a, F: ViewTag, R: FieldList<F, (K, ())>> = SetView<'a, Self, F>;
}
impl<K: ViewShape, const N: usize> ViewShape for BoundedSet<K, N> {
    type Members = (K, ());
    type Shape<'a, F: ViewTag, R: FieldList<F, (K, ())>> = SetView<'a, Self, F>;
}

/// `(A, (B, ()))` from `A, B`.
#[doc(hidden)]
#[macro_export]
macro_rules! __view_tlist {
    () => { () };
    ($head:ty $(, $tail:ty)*) => { ($head, $crate::__view_tlist!($($tail),*)) };
}

macro_rules! tuple_shape {
    ($(($ty:ident, $shape:ident)),*) => {
        impl<$($ty: ViewShape),*> ViewShape for ($($ty,)*) {
            type Members = __view_tlist!($($ty),*);
            type Shape<'a, F: ViewTag, R: FieldList<F, __view_tlist!($($ty),*)>> = ($($shape<'a, Self, F, R>,)*);
        }
    };
}
tuple_shape!((A, Z0));
tuple_shape!((A, Z0), (B, Z1));
tuple_shape!((A, Z0), (B, Z1), (C, Z2));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5), (H, Z6));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5), (H, Z6), (I, Z7));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5), (H, Z6), (I, Z7), (J, Z8));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5), (H, Z6), (I, Z7), (J, Z8), (K, Z9));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5), (H, Z6), (I, Z7), (J, Z8), (K, Z9), (L, Z10));
tuple_shape!((A, Z0), (B, Z1), (C, Z2), (D, Z3), (E, Z4), (G, Z5), (H, Z6), (I, Z7), (J, Z8), (K, Z9), (L, Z10), (M, Z11));

// ── PSSZ ────────────────────────────────────────────────────────────────────

/// Reading through the existing [`PsszView`](crate::PsszView).
pub mod pssz {
    use super::{Cursor, End, FieldList, ListView, MapView, Nil, SetView, UnpackIn, ViewIn, ViewTag};
    use crate::pssz::{
        BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, PsszError, PsszFormat, PsszPack,
        PsszResult, PsszUnpack, PsszValidate, RawString, Uint256,
    };
    use crate::pssz_view::PsszView;
    use std::collections::{BTreeMap, BTreeSet};
    use std::marker::PhantomData;

    /// The family of the PSSZ format tags: `Pssz`, `PsszUnder<C>`, and the
    /// fixed-width ones. A type says once how all of them read it.
    pub struct PsszFamily;

    /// Every PSSZ format tag is a view format. A value is found by its
    /// bytes alone.
    impl<F: PsszFormat> ViewTag for F {
        type At = ();
        type Family = PsszFamily;
        type Error = PsszError;
    }

    /// The cursor of the value that `bytes` are.
    #[inline]
    pub(crate) fn cursor<F: PsszFormat>(bytes: &[u8]) -> Cursor<'_, F> {
        Cursor { bytes, at: () }
    }

    impl<F: PsszFormat> ViewIn<PsszFamily, F> for End {
        type Fields = Nil<F, F>;
        #[inline]
        fn validate(_: &[u8]) -> PsszResult<()> {
            Ok(())
        }
        #[inline]
        fn trusted_root<'a>(_: &'a [u8]) {}
    }

    /// The owned value: the format's trusted decoder.
    impl<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>> UnpackIn<PsszFamily, F> for T {
        #[inline]
        fn trusted_unpack(value: Cursor<'_, F>) -> T {
            T::pssz_unpack_trusted(value.bytes)
        }
    }

    macro_rules! value {
        ($($ty:ty),*) => {$(
            impl<F: PsszFormat> ViewIn<PsszFamily, F> for $ty {
                type Fields = Nil<F, F>;
                #[inline]
                fn validate(bytes: &[u8]) -> PsszResult<()> {
                    PsszView::<$ty, F>::new(bytes).map(|_| ())
                }
                #[inline]
                fn trusted_root<'a>(bytes: &'a [u8]) -> $ty {
                    PsszView::<$ty, F>::trusted(bytes).get()
                }
            }
        )*};
    }
    value!(bool, i8, i16, i32, i64, i128, u8, u16, u32, u64, u128, f32, f64, Uint256);

    /// Panics on bytes that are not UTF-8, which only a view of bytes
    /// nobody validated can hold.
    #[inline]
    fn utf8(bytes: &[u8]) -> &str {
        crate::pssz::utf8_str(bytes).expect("psio view: a string is not UTF-8; validate first")
    }
    #[inline]
    fn within(bytes: &[u8], max: usize) -> &[u8] {
        assert!(bytes.len() <= max, "psio view: a bounded string is longer than its bound; validate first");
        bytes
    }

    // Strings: `$as` is the type whose validator is used (a borrowed
    // string has the bytes of an owned one).
    macro_rules! text {
        ($([$($generics:tt)*] $ty:ty, $as:ty, $shape:ty, |$bytes:ident| $read:expr);* $(;)?) => {$(
            impl<$($generics)* F: PsszFormat> ViewIn<PsszFamily, F> for $ty {
                type Fields = Nil<F, F>;
                #[inline]
                fn validate(bytes: &[u8]) -> PsszResult<()> {
                    PsszView::<$as, F>::new(bytes).map(|_| ())
                }
                #[inline]
                fn trusted_root<'a>($bytes: &'a [u8]) -> $shape {
                    $read
                }
            }
        )*};
    }
    text! {
        [] String, String, &'a str, |bytes| utf8(bytes);
        ['x,] &'x str, String, &'a str, |bytes| utf8(bytes);
        [const N: usize,] BoundedString<N>, BoundedString<N>, &'a str, |bytes| utf8(within(bytes, N));
        [] RawString, RawString, &'a [u8], |bytes| bytes;
        ['x,] &'x [u8], RawString, &'a [u8], |bytes| bytes;
        [const N: usize,] BoundedRawString<N>, BoundedRawString<N>, &'a [u8], |bytes| within(bytes, N);
    }

    /// The reader of the value an optional has.
    pub struct OptionField<T, F>(PhantomData<(T, F)>);
    impl<F: PsszFormat, T: ViewIn<PsszFamily, F> + PsszPack<F> + 'static> FieldList<F, (T, ())> for OptionField<T, F> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        /// Panics when the optional has no value.
        #[inline]
        fn trusted_head<'a>(optional: Cursor<'a, F>, _: usize) -> super::View<'a, T, F> {
            T::trusted_root(PsszView::<Option<T>, F>::trusted(optional.bytes).unwrap().bytes())
        }
    }
    impl<F: PsszFormat, T: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F> + 'static> ViewIn<PsszFamily, F> for Option<T> {
        type Fields = OptionField<T, F>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<Option<T>, F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> Option<super::View<'a, T, F>> {
            PsszView::<Option<T>, F>::trusted(bytes).get().map(|value| T::trusted_root(value.bytes()))
        }
    }

    /// The reader of what a pointer holds: the same bytes.
    pub struct SameField<T, F>(PhantomData<(T, F)>);
    impl<F: PsszFormat, T: ViewIn<PsszFamily, F>> FieldList<F, (T, ())> for SameField<T, F> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(value: Cursor<'a, F>, _: usize) -> super::View<'a, T, F> {
            T::trusted_root(value.bytes)
        }
    }
    macro_rules! wrapper {
        ($([$($generics:tt)*] $ty:ty),*) => {$(
            impl<$($generics)* F: PsszFormat, T: ViewIn<PsszFamily, F>> ViewIn<PsszFamily, F> for $ty {
                type Fields = SameField<T, F>;
                #[inline]
                fn validate(bytes: &[u8]) -> PsszResult<()> {
                    T::validate(bytes)
                }
                #[inline]
                fn trusted_root<'a>(bytes: &'a [u8]) -> super::View<'a, T, F> {
                    T::trusted_root(bytes)
                }
            }
        )*};
    }
    wrapper!(['x,] &'x T, [] Box<T>, [] std::rc::Rc<T>, [] std::sync::Arc<T>, [] std::cell::RefCell<T>);

    /// The reader of the elements of a list `C`, and of the keys of a set.
    pub struct ElementField<C, F>(PhantomData<(C, F)>);
    macro_rules! list {
        ([$($generics:tt)*], $ty:ty, $view:ident, $len:expr) => {
            impl<F: PsszFormat, T: ViewIn<PsszFamily, F> + PsszPack<F> + 'static, $($generics)*> FieldList<F, (T, ())> for ElementField<$ty, F> {
                type Beneath = F;
                type Tail = Nil<F, F>;
                /// Panics past the end.
                #[inline]
                fn trusted_head<'a>(list: Cursor<'a, F>, i: usize) -> super::View<'a, T, F> {
                    T::trusted_root(PsszView::<$ty, F>::trusted(list.bytes).get(i).bytes())
                }
            }
            impl<F: PsszFormat, T: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F> + 'static, $($generics)*> ViewIn<PsszFamily, F> for $ty
            where
                $ty: PsszPack<F> + PsszValidate<F>,
            {
                type Fields = ElementField<$ty, F>;
                #[inline]
                fn len(list: Cursor<'_, F>) -> usize {
                    let len: fn(&[u8]) -> usize = $len;
                    len(list.bytes)
                }
                #[inline]
                fn validate(bytes: &[u8]) -> PsszResult<()> {
                    PsszView::<$ty, F>::new(bytes).map(|_| ())
                }
                #[inline]
                fn trusted_root<'a>(bytes: &'a [u8]) -> $view<'a, $ty, F> {
                    $view::trusted_at(cursor(bytes))
                }
            }
        };
    }
    list!([], Vec<T>, ListView, |list| PsszView::<Vec<T>, F>::trusted(list).len());
    list!([const N: usize], [T; N], ListView, |_| N);
    list!([const N: usize], BoundedList<T, N>, ListView, |list| PsszView::<BoundedList<T, N>, F>::trusted(list).len());
    list!([], BTreeSet<T>, SetView, |set| PsszView::<BTreeSet<T>, F>::trusted(set).len());
    list!([const N: usize], BoundedSet<T, N>, SetView, |set| PsszView::<BoundedSet<T, N>, F>::trusted(set).len());

    /// The reader of the keys of a map `M`.
    pub struct KeyField<M, F>(PhantomData<(M, F)>);
    /// The reader of the mapped values of a map `M`.
    pub struct ValueField<M, F>(PhantomData<(M, F)>);
    macro_rules! map {
        ([$($generics:tt)*], $ty:ty) => {
            impl<F: PsszFormat, K: ViewIn<PsszFamily, F> + PsszPack<F>, V: ViewIn<PsszFamily, F> + PsszPack<F>, $($generics)*> FieldList<F, (K, (V, ()))> for KeyField<$ty, F> {
                type Beneath = F;
                type Tail = ValueField<$ty, F>;
                /// Panics past the end.
                #[inline]
                fn trusted_head<'a>(map: Cursor<'a, F>, i: usize) -> super::View<'a, K, F> {
                    K::trusted_root(PsszView::<$ty, F>::trusted(map.bytes).get(i).key().bytes())
                }
            }
            impl<F: PsszFormat, K: ViewIn<PsszFamily, F> + PsszPack<F>, V: ViewIn<PsszFamily, F> + PsszPack<F>, $($generics)*> FieldList<F, (V, ())> for ValueField<$ty, F> {
                type Beneath = F;
                type Tail = Nil<F, F>;
                /// Panics past the end.
                #[inline]
                fn trusted_head<'a>(map: Cursor<'a, F>, i: usize) -> super::View<'a, V, F> {
                    V::trusted_root(PsszView::<$ty, F>::trusted(map.bytes).get(i).value().bytes())
                }
            }
            impl<F: PsszFormat, K: ViewIn<PsszFamily, F> + PsszPack<F>, V: ViewIn<PsszFamily, F> + PsszPack<F>, $($generics)*> ViewIn<PsszFamily, F> for $ty
            where
                $ty: PsszPack<F> + PsszValidate<F>,
            {
                type Fields = KeyField<$ty, F>;
                #[inline]
                fn len(map: Cursor<'_, F>) -> usize {
                    PsszView::<$ty, F>::trusted(map.bytes).len()
                }
                #[inline]
                fn validate(bytes: &[u8]) -> PsszResult<()> {
                    PsszView::<$ty, F>::new(bytes).map(|_| ())
                }
                #[inline]
                fn trusted_root<'a>(bytes: &'a [u8]) -> MapView<'a, $ty, F> {
                    MapView::trusted_at(cursor(bytes))
                }
            }
        };
    }
    map!([], BTreeMap<K, V>);
    map!([const N: usize], BoundedMap<K, V, N>);

    /// The reader of element `I` of a tuple, which is at the tuple's own
    /// position.
    pub struct TupleField<T, F, const I: usize>(PhantomData<(T, F)>);
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A)> for TupleField<(A,), F, 0> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,) {
        type Fields = TupleField<(A,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,), F, 0> as FieldList<F, __view_tlist!(A)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B)> for TupleField<(A,B,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B)> for TupleField<(A,B,), F, 1> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,) {
        type Fields = TupleField<(A,B,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,), F, 0> as FieldList<F, __view_tlist!(A, B)>>::trusted_head(tuple, 0), <TupleField<(A,B,), F, 1> as FieldList<F, __view_tlist!(B)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C)> for TupleField<(A,B,C,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C)> for TupleField<(A,B,C,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C)> for TupleField<(A,B,C,), F, 2> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,) {
        type Fields = TupleField<(A,B,C,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,), F, 0> as FieldList<F, __view_tlist!(A, B, C)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,), F, 1> as FieldList<F, __view_tlist!(B, C)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,), F, 2> as FieldList<F, __view_tlist!(C)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D)> for TupleField<(A,B,C,D,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D)> for TupleField<(A,B,C,D,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D)> for TupleField<(A,B,C,D,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D)> for TupleField<(A,B,C,D,), F, 3> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,) {
        type Fields = TupleField<(A,B,C,D,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,), F, 1> as FieldList<F, __view_tlist!(B, C, D)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,), F, 2> as FieldList<F, __view_tlist!(C, D)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,), F, 3> as FieldList<F, __view_tlist!(D)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E)> for TupleField<(A,B,C,D,E,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E)> for TupleField<(A,B,C,D,E,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E)> for TupleField<(A,B,C,D,E,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E)> for TupleField<(A,B,C,D,E,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E)> for TupleField<(A,B,C,D,E,), F, 4> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,) {
        type Fields = TupleField<(A,B,C,D,E,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,), F, 2> as FieldList<F, __view_tlist!(C, D, E)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,), F, 3> as FieldList<F, __view_tlist!(D, E)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,), F, 4> as FieldList<F, __view_tlist!(E)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G)> for TupleField<(A,B,C,D,E,G,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G)> for TupleField<(A,B,C,D,E,G,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G)> for TupleField<(A,B,C,D,E,G,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G)> for TupleField<(A,B,C,D,E,G,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G)> for TupleField<(A,B,C,D,E,G,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G)> for TupleField<(A,B,C,D,E,G,), F, 5> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,) {
        type Fields = TupleField<(A,B,C,D,E,G,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,), F, 3> as FieldList<F, __view_tlist!(D, E, G)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,), F, 4> as FieldList<F, __view_tlist!(E, G)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,), F, 5> as FieldList<F, __view_tlist!(G)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G, H)> for TupleField<(A,B,C,D,E,G,H,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G, H)> for TupleField<(A,B,C,D,E,G,H,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G, H)> for TupleField<(A,B,C,D,E,G,H,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G, H)> for TupleField<(A,B,C,D,E,G,H,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G, H)> for TupleField<(A,B,C,D,E,G,H,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G, H)> for TupleField<(A,B,C,D,E,G,H,), F, 5> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,), F, 6>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(H)> for TupleField<(A,B,C,D,E,G,H,), F, 6> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, H, F> {
            <H as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,), F>::trusted(tuple.bytes).field::<6>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,H,) {
        type Fields = TupleField<(A,B,C,D,E,G,H,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,H,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,super::View<'a, H, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,H,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G, H)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G, H)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G, H)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,), F, 3> as FieldList<F, __view_tlist!(D, E, G, H)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,), F, 4> as FieldList<F, __view_tlist!(E, G, H)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,), F, 5> as FieldList<F, __view_tlist!(G, H)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,), F, 6> as FieldList<F, __view_tlist!(H)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G, H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G, H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G, H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G, H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G, H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 5> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 6>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(H, I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 6> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,), F, 7>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, H, F> {
            <H as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<6>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(I)> for TupleField<(A,B,C,D,E,G,H,I,), F, 7> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, I, F> {
            <I as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,), F>::trusted(tuple.bytes).field::<7>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,H,I,) {
        type Fields = TupleField<(A,B,C,D,E,G,H,I,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,H,I,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,super::View<'a, H, F>,super::View<'a, I, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,H,I,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G, H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G, H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 3> as FieldList<F, __view_tlist!(D, E, G, H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 4> as FieldList<F, __view_tlist!(E, G, H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 5> as FieldList<F, __view_tlist!(G, H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 6> as FieldList<F, __view_tlist!(H, I)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,), F, 7> as FieldList<F, __view_tlist!(I)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G, H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G, H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G, H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G, H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 5> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 6>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(H, I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 6> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 7>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, H, F> {
            <H as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<6>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(I, J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 7> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,), F, 8>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, I, F> {
            <I as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<7>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(J)> for TupleField<(A,B,C,D,E,G,H,I,J,), F, 8> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, J, F> {
            <J as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,), F>::trusted(tuple.bytes).field::<8>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,H,I,J,) {
        type Fields = TupleField<(A,B,C,D,E,G,H,I,J,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,H,I,J,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,super::View<'a, H, F>,super::View<'a, I, F>,super::View<'a, J, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,H,I,J,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G, H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 3> as FieldList<F, __view_tlist!(D, E, G, H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 4> as FieldList<F, __view_tlist!(E, G, H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 5> as FieldList<F, __view_tlist!(G, H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 6> as FieldList<F, __view_tlist!(H, I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 7> as FieldList<F, __view_tlist!(I, J)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,), F, 8> as FieldList<F, __view_tlist!(J)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G, H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G, H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G, H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G, H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 5> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 6>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(H, I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 6> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 7>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, H, F> {
            <H as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<6>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(I, J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 7> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 8>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, I, F> {
            <I as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<7>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(J, K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 8> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 9>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, J, F> {
            <J as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<8>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(K)> for TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 9> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, K, F> {
            <K as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::trusted(tuple.bytes).field::<9>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,H,I,J,K,) {
        type Fields = TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,H,I,J,K,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,super::View<'a, H, F>,super::View<'a, I, F>,super::View<'a, J, F>,super::View<'a, K, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G, H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 3> as FieldList<F, __view_tlist!(D, E, G, H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 4> as FieldList<F, __view_tlist!(E, G, H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 5> as FieldList<F, __view_tlist!(G, H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 6> as FieldList<F, __view_tlist!(H, I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 7> as FieldList<F, __view_tlist!(I, J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 8> as FieldList<F, __view_tlist!(J, K)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,), F, 9> as FieldList<F, __view_tlist!(K)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G, H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G, H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G, H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G, H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 5> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 6>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(H, I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 6> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 7>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, H, F> {
            <H as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<6>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(I, J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 7> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 8>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, I, F> {
            <I as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<7>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(J, K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 8> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 9>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, J, F> {
            <J as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<8>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(K, L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 9> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 10>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, K, F> {
            <K as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<9>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(L)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 10> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, L, F> {
            <L as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::trusted(tuple.bytes).field::<10>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,H,I,J,K,L,) {
        type Fields = TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,H,I,J,K,L,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,super::View<'a, H, F>,super::View<'a, I, F>,super::View<'a, J, F>,super::View<'a, K, F>,super::View<'a, L, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G, H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 3> as FieldList<F, __view_tlist!(D, E, G, H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 4> as FieldList<F, __view_tlist!(E, G, H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 5> as FieldList<F, __view_tlist!(G, H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 6> as FieldList<F, __view_tlist!(H, I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 7> as FieldList<F, __view_tlist!(I, J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 8> as FieldList<F, __view_tlist!(J, K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 9> as FieldList<F, __view_tlist!(K, L)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,), F, 10> as FieldList<F, __view_tlist!(L)>>::trusted_head(tuple, 0),)
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 0> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 1>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, A, F> {
            <A as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<0>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 1> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 2>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, B, F> {
            <B as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<1>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(C, D, E, G, H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 2> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 3>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, C, F> {
            <C as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<2>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(D, E, G, H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 3> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 4>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, D, F> {
            <D as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<3>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(E, G, H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 4> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 5>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, E, F> {
            <E as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<4>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(G, H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 5> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 6>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, G, F> {
            <G as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<5>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(H, I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 6> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 7>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, H, F> {
            <H as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<6>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(I, J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 7> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 8>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, I, F> {
            <I as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<7>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(J, K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 8> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 9>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, J, F> {
            <J as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<8>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(K, L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 9> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 10>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, K, F> {
            <K as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<9>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(L, M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 10> {
        type Beneath = F;
        type Tail = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 11>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, L, F> {
            <L as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<10>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> FieldList<F, __view_tlist!(M)> for TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 11> {
        type Beneath = F;
        type Tail = Nil<F, F>;
        #[inline]
        fn trusted_head<'a>(tuple: Cursor<'a, F>, _: usize) -> super::View<'a, M, F> {
            <M as ViewIn<PsszFamily, F>>::trusted_root(PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::trusted(tuple.bytes).field::<11>().bytes())
        }
    }
    impl<F: PsszFormat, A: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, B: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, C: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, D: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, E: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, G: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, H: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, I: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, J: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, K: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, L: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>, M: ViewIn<PsszFamily, F> + PsszPack<F> + PsszValidate<F>> ViewIn<PsszFamily, F> for (A,B,C,D,E,G,H,I,J,K,L,M,) {
        type Fields = TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 0>;
        #[inline]
        fn validate(bytes: &[u8]) -> PsszResult<()> {
            PsszView::<(A,B,C,D,E,G,H,I,J,K,L,M,), F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn trusted_root<'a>(bytes: &'a [u8]) -> (super::View<'a, A, F>,super::View<'a, B, F>,super::View<'a, C, F>,super::View<'a, D, F>,super::View<'a, E, F>,super::View<'a, G, F>,super::View<'a, H, F>,super::View<'a, I, F>,super::View<'a, J, F>,super::View<'a, K, F>,super::View<'a, L, F>,super::View<'a, M, F>,) {
            let tuple = cursor::<F>(bytes);
            (<TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 0> as FieldList<F, __view_tlist!(A, B, C, D, E, G, H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 1> as FieldList<F, __view_tlist!(B, C, D, E, G, H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 2> as FieldList<F, __view_tlist!(C, D, E, G, H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 3> as FieldList<F, __view_tlist!(D, E, G, H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 4> as FieldList<F, __view_tlist!(E, G, H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 5> as FieldList<F, __view_tlist!(G, H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 6> as FieldList<F, __view_tlist!(H, I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 7> as FieldList<F, __view_tlist!(I, J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 8> as FieldList<F, __view_tlist!(J, K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 9> as FieldList<F, __view_tlist!(K, L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 10> as FieldList<F, __view_tlist!(L, M)>>::trusted_head(tuple, 0), <TupleField<(A,B,C,D,E,G,H,I,J,K,L,M,), F, 11> as FieldList<F, __view_tlist!(M)>>::trusted_head(tuple, 0),)
        }
    }
}

/// How a type is decoded whole in the formats of the family `Fam`: what
/// [`Unpacks`] is made of, as [`ViewIn`] is what [`Reads`] is made of.
pub trait UnpackIn<Fam, F: ViewTag>: Sized {
    fn trusted_unpack(value: Cursor<'_, F>) -> Self;
}
impl<F: ViewTag, T: UnpackIn<F::Family, F>> Unpacks<T> for F {
    #[inline]
    fn trusted_unpack(value: Cursor<'_, F>) -> T {
        T::trusted_unpack(value)
    }
}

// ── FlatBuffers ─────────────────────────────────────────────────────────────

/// Reading through the existing [`FbView`](crate::flatbuf::FbView).
#[cfg(feature = "flatbuf")]
pub mod flatbuf {
    use super::{Cursor, End, FieldList, ListView, MapView, Nil, SetView, UnpackIn, ViewIn, ViewTag};
    use crate::flatbuf::{FbError, FbMap, FbSet, FbVec, FbViewable, Flatbuf};
    use crate::pssz::{BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, RawString};
    use std::collections::{BTreeMap, BTreeSet};
    use std::marker::PhantomData;

    /// The family of FlatBuffers: one format.
    pub struct FbFamily;

    /// Where a value is in a buffer.
    #[derive(Clone, Copy, Debug)]
    pub struct FbAt {
        /// The position of a table, of a struct's first byte, or of the
        /// first element of a vector.
        pub(crate) position: usize,
        /// A vector's number of elements; the type of a union.
        pub(crate) count: usize,
    }

    impl ViewTag for Flatbuf {
        type At = FbAt;
        type Family = FbFamily;
        type Error = FbError;
    }

    #[inline]
    pub(crate) fn cursor(bytes: &[u8], position: usize, count: usize) -> Cursor<'_, Flatbuf> {
        Cursor { bytes, at: FbAt { position, count } }
    }

    /// A type that reads as its shape from a FlatBuffers view.
    ///
    /// The view is that of [`Native`](FbShape::Native), the type
    /// FlatBuffers stores: the type itself, except for the types that only
    /// add a bound (`BoundedList<T, N>` is stored as a `Vec<T>`).
    pub trait FbShape: ViewIn<FbFamily, Flatbuf> {
        type Native: FbViewable;
        /// Does not validate: the view may be of bytes nobody verified.
        fn trusted_from_fb<'a>(view: <Self::Native as FbViewable>::View<'a>) -> Self::Shape<'a, Flatbuf, Self::Fields>;
    }

    /// What a `T` that is not in the buffer reads as.
    #[inline]
    pub(crate) fn absent<T: FbShape>(bytes: &[u8]) -> super::View<'_, T, Flatbuf> {
        T::trusted_from_fb(<T::Native as FbViewable>::fb_view_absent(bytes, None))
    }

    /// Only a table is the root of a buffer.
    pub(crate) fn not_a_root() -> FbError {
        FbError::new("flatbuf: the root of a buffer is a table", 0)
    }

    impl ViewIn<FbFamily, Flatbuf> for End {
        type Fields = Nil<Flatbuf, Flatbuf>;
        fn validate(_: &[u8]) -> Result<(), FbError> {
            Ok(())
        }
        #[inline]
        fn trusted_root<'a>(_: &'a [u8]) {}
    }

    /// What is not the root of a buffer: `new` refuses it, and a trusted
    /// read of a buffer as one gives what an absent one reads as.
    macro_rules! never_root {
        () => {
            fn validate(_: &[u8]) -> Result<(), FbError> {
                Err(not_a_root())
            }
            #[inline]
            fn trusted_root<'a>(bytes: &'a [u8]) -> Self::Shape<'a, Flatbuf, Self::Fields> {
                absent::<Self>(bytes)
            }
        };
    }

    macro_rules! same {
        ($([$($generics:tt)*] $ty:ty, $native:ty, $view:ty);* $(;)?) => {$(
            impl<$($generics)*> ViewIn<FbFamily, Flatbuf> for $ty {
                type Fields = Nil<Flatbuf, Flatbuf>;
                never_root!();
            }
            impl<$($generics)*> FbShape for $ty {
                type Native = $native;
                #[inline]
                fn trusted_from_fb<'a>(view: $view) -> $view
                where
                    'a: 'a,
                {
                    view
                }
            }
        )*};
    }
    same! {
        [] bool, bool, bool; [] i8, i8, i8; [] i16, i16, i16; [] i32, i32, i32; [] i64, i64, i64;
        [] u8, u8, u8; [] u16, u16, u16; [] u32, u32, u32; [] u64, u64, u64; [] f32, f32, f32; [] f64, f64, f64;
        [] String, String, &'a str; [] RawString, RawString, &'a [u8];
        [const N: usize] BoundedString<N>, String, &'a str;
        [const N: usize] BoundedRawString<N>, RawString, &'a [u8];
    }

    /// The reader of the value an optional has. FlatBuffers finds a member
    /// through the table that holds it, so there is nothing to read from
    /// the optional alone: it reads as absent.
    pub struct OptionField<T>(PhantomData<T>);
    impl<T: FbShape> FieldList<Flatbuf, (T, ())> for OptionField<T> {
        type Beneath = Flatbuf;
        type Tail = Nil<Flatbuf, Flatbuf>;
        #[inline]
        fn trusted_head<'a>(optional: Cursor<'a, Flatbuf>, _: usize) -> super::View<'a, T, Flatbuf> {
            absent::<T>(optional.bytes)
        }
    }
    impl<T: FbShape> ViewIn<FbFamily, Flatbuf> for Option<T> {
        type Fields = OptionField<T>;
        never_root!();
    }
    impl<T: FbShape> FbShape for Option<T> {
        type Native = Option<T::Native>;
        #[inline]
        fn trusted_from_fb<'a>(view: Option<<T::Native as FbViewable>::View<'a>>) -> Option<super::View<'a, T, Flatbuf>>
        where
            'a: 'a,
        {
            view.map(T::trusted_from_fb)
        }
    }

    /// The reader of the elements of a vector, and of the keys of a set.
    pub struct ElementField<T>(PhantomData<T>);
    impl<T: FbShape> FieldList<Flatbuf, (T, ())> for ElementField<T> {
        type Beneath = Flatbuf;
        type Tail = Nil<Flatbuf, Flatbuf>;
        /// Past the end: what an absent element reads as.
        #[inline]
        fn trusted_head<'a>(list: Cursor<'a, Flatbuf>, i: usize) -> super::View<'a, T, Flatbuf> {
            match FbVec::<T::Native>::from_parts(list.bytes, list.at.position, list.at.count).get(i) {
                Some(view) => T::trusted_from_fb(view),
                None => absent::<T>(list.bytes),
            }
        }
    }
    macro_rules! list {
        ([$($generics:tt)*], $ty:ty, $native:ty, $view:ident, $native_view:ident, $bound:ident) => {
            impl<T: FbShape, $($generics)*> ViewIn<FbFamily, Flatbuf> for $ty
            where
                T::Native: $bound,
            {
                type Fields = ElementField<T>;
                #[inline]
                fn len(list: Cursor<'_, Flatbuf>) -> usize {
                    list.at.count
                }
                never_root!();
            }
            impl<T: FbShape, $($generics)*> FbShape for $ty
            where
                T::Native: $bound,
            {
                type Native = $native;
                #[inline]
                fn trusted_from_fb<'a>(view: $native_view<'a, T::Native>) -> $view<'a, $ty, Flatbuf>
                where
                    'a: 'a,
                {
                    let (bytes, position, count) = view.into_parts();
                    $view::trusted_at(cursor(bytes, position, count))
                }
            }
        };
    }
    /// Every type: what a vector's elements have to be.
    pub trait Any {}
    impl<T> Any for T {}
    list!([], Vec<T>, Vec<T::Native>, ListView, FbVec, Any);
    list!([const N: usize], [T; N], [T::Native; N], ListView, FbVec, Any);
    list!([const N: usize], BoundedList<T, N>, Vec<T::Native>, ListView, FbVec, Any);
    list!([], BTreeSet<T>, BTreeSet<T::Native>, SetView, FbSet, Ord);
    list!([const N: usize], BoundedSet<T, N>, BTreeSet<T::Native>, SetView, FbSet, Ord);

    /// The owned vector: what `FbVec::unpack` gives.
    impl<T: FbViewable> UnpackIn<FbFamily, Flatbuf> for Vec<T> {
        #[inline]
        fn trusted_unpack(list: Cursor<'_, Flatbuf>) -> Vec<T> {
            FbVec::<T>::from_parts(list.bytes, list.at.position, list.at.count).unpack()
        }
    }

    /// The reader of the keys of a map.
    pub struct KeyField<K, V>(PhantomData<(K, V)>);
    /// The reader of the mapped values of a map.
    pub struct ValueField<K, V>(PhantomData<(K, V)>);
    impl<K: FbShape, V: FbShape> FieldList<Flatbuf, (K, (V, ()))> for KeyField<K, V> {
        type Beneath = Flatbuf;
        type Tail = ValueField<K, V>;
        /// Past the end: what an absent key reads as.
        #[inline]
        fn trusted_head<'a>(map: Cursor<'a, Flatbuf>, i: usize) -> super::View<'a, K, Flatbuf> {
            match FbMap::<K::Native, V::Native>::from_parts(map.bytes, map.at.position, map.at.count).key(i) {
                Some(view) => K::trusted_from_fb(view),
                None => absent::<K>(map.bytes),
            }
        }
    }
    impl<K: FbShape, V: FbShape> FieldList<Flatbuf, (V, ())> for ValueField<K, V> {
        type Beneath = Flatbuf;
        type Tail = Nil<Flatbuf, Flatbuf>;
        /// Past the end: what an absent value reads as.
        #[inline]
        fn trusted_head<'a>(map: Cursor<'a, Flatbuf>, i: usize) -> super::View<'a, V, Flatbuf> {
            match FbMap::<K::Native, V::Native>::from_parts(map.bytes, map.at.position, map.at.count).value(i) {
                Some(view) => V::trusted_from_fb(view),
                None => absent::<V>(map.bytes),
            }
        }
    }
    macro_rules! map {
        ([$($generics:tt)*], $ty:ty) => {
            impl<K: FbShape, V: FbShape, $($generics)*> ViewIn<FbFamily, Flatbuf> for $ty
            where
                K::Native: Ord,
            {
                type Fields = KeyField<K, V>;
                #[inline]
                fn len(map: Cursor<'_, Flatbuf>) -> usize {
                    map.at.count
                }
                never_root!();
            }
            impl<K: FbShape, V: FbShape, $($generics)*> FbShape for $ty
            where
                K::Native: Ord,
            {
                type Native = BTreeMap<K::Native, V::Native>;
                #[inline]
                fn trusted_from_fb<'a>(view: FbMap<'a, K::Native, V::Native>) -> MapView<'a, $ty, Flatbuf>
                where
                    'a: 'a,
                {
                    let (bytes, position, count) = view.into_parts();
                    MapView::trusted_at(cursor(bytes, position, count))
                }
            }
        };
    }
    map!([], BTreeMap<K, V>);
    map!([const N: usize], BoundedMap<K, V, N>);
}

// ── What `#[derive(View)]` calls ────────────────────────────────────────────

/// The plumbing of `#[derive(View)]`. Not for use by hand: these functions
/// make views of bytes without validating them.
#[doc(hidden)]
pub mod __derive {
    use super::pssz::PsszFamily;
    use super::{Cursor, ViewIn};
    use crate::pssz::{PsszFormat, PsszPack, PsszResult, PsszValidate};
    use crate::pssz_view::{PsszField, PsszView};
    pub use std::marker::PhantomData;

    /// The members of a type from one of them on: implemented by a marker
    /// type per member, so that the list is named in constant space.
    pub trait ListAt {
        type L: super::TList;
    }
    /// The reader of the members from one of them on, when the readers of
    /// all of them are `R`.
    pub trait ReaderAt<F: super::ViewTag, R>: ListAt {
        type Reader: super::FieldList<F, Self::L>;
    }

    /// The cursor of the PSSZ value that `bytes` are.
    #[inline]
    pub fn pssz_trusted_cursor<F: PsszFormat>(bytes: &[u8]) -> Cursor<'_, F> {
        super::pssz::cursor(bytes)
    }

    /// Whether `bytes` are one valid `R`: the validator of `PsszView::new`.
    #[inline]
    pub fn pssz_validate<R: PsszPack<F> + PsszValidate<F>, F: PsszFormat>(bytes: &[u8]) -> PsszResult<()> {
        PsszView::<R, F>::new(bytes).map(|_| ())
    }

    /// Member `I` of the record (or tuple) `R` at `record`, as what its
    /// type `T` reads as. `T` is the type PSSZ has for the member
    /// (`PsszField::Field`), so it is read at that type's widths.
    #[inline]
    pub fn pssz_trusted_field<'a, R, F, T, const I: usize>(
        record: Cursor<'a, F>,
    ) -> super::View<'a, T, <R as PsszField<F, I>>::Beneath>
    where
        F: PsszFormat,
        R: PsszField<F, I, Field = T>,
        T: ViewIn<PsszFamily, <R as PsszField<F, I>>::Beneath>,
    {
        T::trusted_root(PsszView::<R, F>::trusted(record.bytes).field::<I>().bytes())
    }

    /// The payload of alternative `tag` of the enum at `value`; panics
    /// when the enum holds another.
    #[inline]
    #[track_caller]
    pub fn pssz_trusted_payload<'a, F: PsszFormat>(value: Cursor<'a, F>, tag: u8, name: &str) -> Cursor<'a, F> {
        match value.bytes.split_first() {
            Some((selector, payload)) if *selector == tag => super::pssz::cursor(payload),
            _ => panic!("psio view: the {name} does not hold this alternative; validate first"),
        }
    }

    /// The selector of the enum at `value` and its payload.
    #[inline]
    pub fn pssz_selector<F: PsszFormat>(value: Cursor<'_, F>) -> Option<u8> {
        value.bytes.first().copied()
    }

    /// The bytes a PSSZ cursor finds.
    #[inline]
    pub fn pssz_bytes<'a, F: PsszFormat>(value: Cursor<'a, F>) -> &'a [u8] {
        value.bytes
    }

    #[cfg(feature = "flatbuf")]
    pub use fb::*;
    #[cfg(feature = "flatbuf")]
    mod fb {
        use super::super::flatbuf::{cursor, not_a_root};
        use super::super::Cursor;
        use crate::flatbuf::verify::check_buffer;
        use crate::flatbuf::{FbError, FbLimits, FbOptions, FbTable, FbUnion, FbUnpack, FbView, Flatbuf};

        /// The `FbView` of the record a cursor finds.
        #[inline]
        pub fn fb_trusted_native<R: FbUnpack>(record: Cursor<'_, Flatbuf>) -> FbView<'_, R> {
            FbView::at(record.bytes, record.at.position)
        }

        /// The cursor of the record an `FbView` is of.
        #[inline]
        pub fn fb_trusted_record<R: FbUnpack>(view: FbView<'_, R>) -> Cursor<'_, Flatbuf> {
            cursor(view.fb_buffer(), view.fb_position(), 0)
        }

        /// The cursor of the root of `bytes` read as an `R`; does not
        /// verify. A record that is a struct is never the root of a buffer:
        /// it then reads as an absent one (every member at its default).
        #[inline]
        pub fn fb_trusted_root<R: FbUnpack>(bytes: &[u8]) -> Cursor<'_, Flatbuf> {
            if R::TABLE {
                cursor(bytes, FbTable::root(bytes).position(), 0)
            } else {
                fb_trusted_record(FbView::<R>::absent(bytes))
            }
        }

        /// Whether `bytes` are a buffer whose root is a valid `R`: the
        /// verifier of `FbView::new`. (For a record that is a struct this
        /// is an error and not, as there, a failure to compile.)
        #[inline]
        pub fn fb_validate<R: FbUnpack>(bytes: &[u8]) -> Result<(), FbError> {
            if !R::TABLE {
                return Err(not_a_root());
            }
            check_buffer(bytes, &FbOptions::default(), FbLimits::default(), R::fb_check_table)
        }

        /// The owned record.
        #[inline]
        pub fn fb_trusted_unpack<R: FbUnpack>(record: Cursor<'_, Flatbuf>) -> R {
            fb_trusted_native::<R>(record).unpack()
        }

        /// The cursor of a union: the table it holds, and its type.
        #[inline]
        pub fn fb_trusted_union<U>(view: FbUnion<'_, U>) -> Cursor<'_, Flatbuf> {
            let table = view.fb_table();
            cursor(table.buffer(), table.position(), view.tag() as usize)
        }

        /// The type of the union a cursor finds (0: it holds nothing).
        #[inline]
        pub fn fb_union_tag(value: Cursor<'_, Flatbuf>) -> usize {
            value.at.count
        }

        /// The alternative of wire type `tag` of the union at `value`: the
        /// table it holds, or an absent one when it holds another.
        #[inline]
        pub fn fb_trusted_alternative<T: FbUnpack>(value: Cursor<'_, Flatbuf>, tag: usize) -> FbView<'_, T> {
            if value.at.count == tag && tag != 0 {
                FbView::<T>::at(value.bytes, value.at.position)
            } else {
                FbView::<T>::absent(value.bytes)
            }
        }

        /// Only a table is the root of a buffer.
        pub fn fb_not_a_root() -> FbError {
            not_a_root()
        }

        /// What a union that holds nothing is found by.
        #[inline]
        pub fn fb_trusted_no_union(bytes: &[u8]) -> Cursor<'_, Flatbuf> {
            cursor(bytes, usize::MAX, 0)
        }
    }
}
