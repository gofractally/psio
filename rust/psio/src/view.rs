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
//! | `RawString` | `&[u8]` |
//! | record | [`View<'a, T, F>`](View), one method per member |
//! | `Vec<T>`, `[T; N]`, `BoundedList` | [`ListView<'a, _, F>`](ListView): `len`, `get`, `at_index`, `iter`, `for` |
//! | `BTreeMap<K, V>`, `BoundedMap` | [`MapView<'a, _, F>`](MapView): entries by ascending key, `get(key)` |
//! | `BTreeSet<K>`, `BoundedSet` | [`SetView<'a, _, F>`](SetView): keys ascending, `contains(key)` |
//! | `Option<T>` | `Option` of what `T` reads as |
//! | `Box<T>` | what `T` reads as |
//! | `(A, B)` | the tuple of what `A` and `B` read as |
//! | field-less enum | the enum's value |
//! | enum with payloads | the enum `<Name>View` its derive writes: one alternative per variant, to `match` on |
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
//! * [`RecordFormat`], [`ReadsField`], [`ReadsList`], [`ReadsMap`],
//!   [`ReadsSet`] and [`Reads`] say that a format reads a type. They are
//!   stated on the format because Rust carries a bound from a function's
//!   signature into its body only through the supertraits of the traits it
//!   names: `#[derive(View)]` writes a trait `<Record>Format` whose
//!   supertraits are everything reading the record needs, so
//!   `F: OrderFormat` is the only bound generic code states.
//! * Each format says how it reads in traits of its own ([`pssz`],
//!   [`flatbuf`]), on top of its existing view (`PsszView`, `FbView`), which
//!   stay as they are. The traits above are implemented once here for every
//!   PSSZ format tag and once for FlatBuffers. `#[derive(View)]` implements
//!   the format's traits for a record, for the formats named in
//!   `#[view(...)]`.
//!
//! The format of a record's members is [`RecordFormat::Beneath`]. It is the
//! format itself except in PSSZ, where the members of a record with a cap
//! are read at the offset widths the cap selects (`PsszUnder<C>`).

use std::cmp::Ordering;
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

/// A type that reads as a [`MapView`]: its keys and mapped values.
pub trait MapShape: ViewShape {
    type Key: ViewShape;
    type Value: ViewShape;
}

/// A type that reads as a [`SetView`]: its keys.
pub trait SetShape: ViewShape {
    type Key: ViewShape;
}

/// A type that reads as what the one type inside it reads as (`Option<T>`
/// as an `Option` of it, `Box<T>` as it).
pub trait WrapShape: ViewShape {
    type Inner: ViewShape;
}

/// A type that derives `View`.
///
/// `#[derive(View)]` cannot see what a member's type is, only how it is
/// written: a type it does not know is taken to be one that derives `View`
/// too, with the trait `<Name>Format` beside it. This trait is what turns a
/// wrong guess into a message.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is a member of a view and does not derive `psio::View`",
    label = "taken to be a record or enum that derives `psio::View`",
    note = "a type `#[derive(View)]` does not know is taken to derive `View`, with `<Name>Format` beside it",
    note = "if it is a value read as itself, say so on the member: `#[view(value)]`",
    note = "if it is an alias or a renamed import of a type that derives `View`, name that type's format trait on the member: `#[view(format = \"path::NameFormat\")]`"
)]
pub trait Derived: ViewShape {}

// ── What a format reads ─────────────────────────────────────────────────────

/// `Self` reads a `T` that is the whole of a buffer.
pub trait Reads<T: ViewShape + ?Sized>: ViewTag {
    /// Whether `bytes` are one valid `T`.
    fn validate(bytes: &[u8]) -> Result<(), Self::Error>;
    /// What the `T` that `bytes` are reads as; does not validate.
    fn root(bytes: &[u8]) -> T::Shape<'_, Self>;
}

/// `Self` reads the record `R`.
pub trait RecordFormat<R: ViewShape + ?Sized>: ViewTag {
    /// The format of the members.
    type Beneath: ViewTag;
    /// Whether `bytes` are one valid `R`.
    fn validate(bytes: &[u8]) -> Result<(), Self::Error>;
    /// The cursor of the record that `bytes` are.
    fn root(bytes: &[u8]) -> Self::Cursor<'_>;
}

/// `Self` reads member `I` of the record `R`.
pub trait ReadsField<R: ViewShape + ?Sized, const I: usize>: RecordFormat<R> {
    type Type: ViewShape;
    fn field<'a>(
        record: Self::Cursor<'a>,
    ) -> <Self::Type as ViewShape>::Shape<'a, <Self as RecordFormat<R>>::Beneath>;
}

/// `Self` reads the list `C`.
pub trait ReadsList<C: ListShape + ?Sized>: ViewTag {
    fn len(list: Self::Cursor<'_>) -> usize;
    /// Element `i`; `i` is less than `len`.
    fn element<'a>(list: Self::Cursor<'a>, i: usize) -> <C::Elem as ViewShape>::Shape<'a, Self>;
}
impl<F: ViewTag, C: ListIn<F> + ?Sized> ReadsList<C> for F {
    #[inline]
    fn len(list: F::Cursor<'_>) -> usize {
        C::len(list)
    }
    #[inline]
    fn element<'a>(list: F::Cursor<'a>, i: usize) -> <C::Elem as ViewShape>::Shape<'a, F> {
        C::element(list, i)
    }
}

/// `Self` reads the map `M`.
pub trait ReadsMap<M: MapShape + ?Sized>: ViewTag {
    fn len(map: Self::Cursor<'_>) -> usize;
    /// The key of entry `i`; `i` is less than `len`.
    fn key<'a>(map: Self::Cursor<'a>, i: usize) -> <M::Key as ViewShape>::Shape<'a, Self>;
    /// The mapped value of entry `i`; `i` is less than `len`.
    fn value<'a>(map: Self::Cursor<'a>, i: usize) -> <M::Value as ViewShape>::Shape<'a, Self>;
}
impl<F: ViewTag, M: MapIn<F> + ?Sized> ReadsMap<M> for F {
    #[inline]
    fn len(map: F::Cursor<'_>) -> usize {
        M::len(map)
    }
    #[inline]
    fn key<'a>(map: F::Cursor<'a>, i: usize) -> <M::Key as ViewShape>::Shape<'a, F> {
        M::key(map, i)
    }
    #[inline]
    fn value<'a>(map: F::Cursor<'a>, i: usize) -> <M::Value as ViewShape>::Shape<'a, F> {
        M::value(map, i)
    }
}

/// `Self` reads the set `S`.
pub trait ReadsSet<S: SetShape + ?Sized>: ViewTag {
    fn len(set: Self::Cursor<'_>) -> usize;
    /// Key `i`; `i` is less than `len`.
    fn key<'a>(set: Self::Cursor<'a>, i: usize) -> <S::Key as ViewShape>::Shape<'a, Self>;
}
impl<F: ViewTag, S: SetIn<F> + ?Sized> ReadsSet<S> for F {
    #[inline]
    fn len(set: F::Cursor<'_>) -> usize {
        S::len(set)
    }
    #[inline]
    fn key<'a>(set: F::Cursor<'a>, i: usize) -> <S::Key as ViewShape>::Shape<'a, F> {
        S::key(set, i)
    }
}

// ── How the library's own containers are read in one format ────────────────

/// A list that can be read in format `F`.
pub trait ListIn<F: ViewTag>: ListShape {
    fn len(list: F::Cursor<'_>) -> usize;
    /// Element `i`; `i` is less than `len`.
    fn element<'a>(list: F::Cursor<'a>, i: usize) -> <Self::Elem as ViewShape>::Shape<'a, F>;
}

/// A map that can be read in format `F`.
pub trait MapIn<F: ViewTag>: MapShape {
    fn len(map: F::Cursor<'_>) -> usize;
    fn key<'a>(map: F::Cursor<'a>, i: usize) -> <Self::Key as ViewShape>::Shape<'a, F>;
    fn value<'a>(map: F::Cursor<'a>, i: usize) -> <Self::Value as ViewShape>::Shape<'a, F>;
}

/// A set that can be read in format `F`.
pub trait SetIn<F: ViewTag>: SetShape {
    fn len(set: F::Cursor<'_>) -> usize;
    fn key<'a>(set: F::Cursor<'a>, i: usize) -> <Self::Key as ViewShape>::Shape<'a, F>;
}

// ── Any value ───────────────────────────────────────────────────────────────

/// Validates `bytes` as one `T` in format `F`, then gives what a `T` reads
/// as. For a record this is [`View::new`]; it is also how the root of a
/// buffer that is not a record (a list, an enum) is read.
#[inline]
pub fn read<T: ViewShape + ?Sized, F: Reads<T>>(bytes: &[u8]) -> Result<T::Shape<'_, F>, F::Error> {
    F::validate(bytes)?;
    Ok(F::root(bytes))
}

/// What the `T` that `bytes` are reads as, of bytes the caller vouches for
/// (validated earlier, or produced by the format's encoder); does not
/// validate.
#[inline]
pub fn read_trusted<T: ViewShape + ?Sized, F: Reads<T>>(bytes: &[u8]) -> T::Shape<'_, F> {
    F::root(bytes)
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

impl<'a, T: ViewShape + ?Sized, F: RecordFormat<T>> View<'a, T, F> {
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

/// A read-only view of a list `C` (`Vec<T>`, `[T; N]`, `BoundedList`) in
/// format `F`.
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

impl<'a, C: ListShape + ?Sized, F: ReadsList<C>> ListView<'a, C, F> {
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

    /// The first element.
    #[inline]
    pub fn first(&self) -> Option<<C::Elem as ViewShape>::Shape<'a, F>> {
        self.get(0)
    }

    /// The last element.
    #[inline]
    pub fn last(&self) -> Option<<C::Elem as ViewShape>::Shape<'a, F>> {
        self.len().checked_sub(1).map(|i| F::element(self.cursor, i))
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

impl<'a, C: ListShape + ?Sized, F: ReadsList<C>> Iterator for ListIter<'a, C, F> {
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
impl<'a, C: ListShape + ?Sized, F: ReadsList<C>> DoubleEndedIterator for ListIter<'a, C, F> {
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
impl<'a, C: ListShape + ?Sized, F: ReadsList<C>> ExactSizeIterator for ListIter<'a, C, F> {}

impl<'a, C: ListShape + ?Sized, F: ReadsList<C>> IntoIterator for ListView<'a, C, F> {
    type Item = <C::Elem as ViewShape>::Shape<'a, F>;
    type IntoIter = ListIter<'a, C, F>;
    #[inline]
    fn into_iter(self) -> ListIter<'a, C, F> {
        self.iter()
    }
}
impl<'a, C: ListShape + ?Sized, F: ReadsList<C>> IntoIterator for &ListView<'a, C, F> {
    type Item = <C::Elem as ViewShape>::Shape<'a, F>;
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

/// A read-only view of a map `M` (`BTreeMap<K, V>`, `BoundedMap`) in
/// format `F`: its entries by ascending key.
pub struct MapView<'a, M: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    _type: PhantomData<(fn() -> *const M, fn() -> F)>,
}

impl<M: ?Sized, F: ViewTag> Clone for MapView<'_, M, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<M: ?Sized, F: ViewTag> Copy for MapView<'_, M, F> {}

impl<M: ?Sized, F: ViewTag> std::fmt::Debug for MapView<'_, M, F> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MapView").field("type", &std::any::type_name::<M>()).finish()
    }
}

impl<'a, M: ?Sized, F: ViewTag> MapView<'a, M, F> {
    /// The view of the map a cursor finds. For the code that implements a
    /// format.
    #[inline]
    pub fn at(cursor: F::Cursor<'a>) -> Self {
        MapView { cursor, _type: PhantomData }
    }
}

impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> MapView<'a, M, F> {
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
    #[allow(clippy::type_complexity)]
    pub fn entry(&self, i: usize) -> Option<(<M::Key as ViewShape>::Shape<'a, F>, <M::Value as ViewShape>::Shape<'a, F>)> {
        (i < self.len()).then(|| (F::key(self.cursor, i), F::value(self.cursor, i)))
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
        <M::Key as ViewShape>::Shape<'a, F>: PartialOrd<Q>,
    {
        let len = self.len();
        let i = lower_bound(len, |i| F::key(self.cursor, i).partial_cmp(&key));
        (i < len && F::key(self.cursor, i).partial_cmp(&key) == Some(Ordering::Equal)).then_some(i)
    }

    /// The value of `key`: `map.get("name")`, `map.get(7)`.
    #[inline]
    pub fn get<Q>(&self, key: Q) -> Option<<M::Value as ViewShape>::Shape<'a, F>>
    where
        <M::Key as ViewShape>::Shape<'a, F>: PartialOrd<Q>,
    {
        self.find(key).map(|i| F::value(self.cursor, i))
    }

    #[inline]
    pub fn contains_key<Q>(&self, key: Q) -> bool
    where
        <M::Key as ViewShape>::Shape<'a, F>: PartialOrd<Q>,
    {
        self.find(key).is_some()
    }
}

/// The entries of a [`MapView`].
pub struct MapIter<'a, M: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    next: usize,
    end: usize,
    _type: PhantomData<(fn() -> *const M, fn() -> F)>,
}

impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> Iterator for MapIter<'a, M, F> {
    type Item = (<M::Key as ViewShape>::Shape<'a, F>, <M::Value as ViewShape>::Shape<'a, F>);
    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            let i = self.next;
            self.next += 1;
            Some((F::key(self.cursor, i), F::value(self.cursor, i)))
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
impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> DoubleEndedIterator for MapIter<'a, M, F> {
    #[inline]
    fn next_back(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            self.end -= 1;
            Some((F::key(self.cursor, self.end), F::value(self.cursor, self.end)))
        } else {
            None
        }
    }
}
impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> ExactSizeIterator for MapIter<'a, M, F> {}

/// The keys of a [`MapView`].
pub struct MapKeys<'a, M: ?Sized, F: ViewTag>(MapIter<'a, M, F>);
/// The mapped values of a [`MapView`].
pub struct MapValues<'a, M: ?Sized, F: ViewTag>(MapIter<'a, M, F>);

macro_rules! map_part {
    ($name:ident, $part:ident, $read:ident) => {
        impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> Iterator for $name<'a, M, F> {
            type Item = <M::$part as ViewShape>::Shape<'a, F>;
            #[inline]
            fn next(&mut self) -> Option<Self::Item> {
                let entries = &mut self.0;
                if entries.next < entries.end {
                    let i = entries.next;
                    entries.next += 1;
                    Some(F::$read(entries.cursor, i))
                } else {
                    None
                }
            }
            #[inline]
            fn size_hint(&self) -> (usize, Option<usize>) {
                self.0.size_hint()
            }
        }
        impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> DoubleEndedIterator for $name<'a, M, F> {
            #[inline]
            fn next_back(&mut self) -> Option<Self::Item> {
                let entries = &mut self.0;
                if entries.next < entries.end {
                    entries.end -= 1;
                    Some(F::$read(entries.cursor, entries.end))
                } else {
                    None
                }
            }
        }
        impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> ExactSizeIterator for $name<'a, M, F> {}
    };
}
map_part!(MapKeys, Key, key);
map_part!(MapValues, Value, value);

impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> IntoIterator for MapView<'a, M, F> {
    type Item = (<M::Key as ViewShape>::Shape<'a, F>, <M::Value as ViewShape>::Shape<'a, F>);
    type IntoIter = MapIter<'a, M, F>;
    #[inline]
    fn into_iter(self) -> MapIter<'a, M, F> {
        self.iter()
    }
}
impl<'a, M: MapShape + ?Sized, F: ReadsMap<M>> IntoIterator for &MapView<'a, M, F> {
    type Item = (<M::Key as ViewShape>::Shape<'a, F>, <M::Value as ViewShape>::Shape<'a, F>);
    type IntoIter = MapIter<'a, M, F>;
    #[inline]
    fn into_iter(self) -> MapIter<'a, M, F> {
        self.iter()
    }
}

// ── Sets ────────────────────────────────────────────────────────────────────

/// A read-only view of a set `S` (`BTreeSet<K>`, `BoundedSet`) in format
/// `F`: its keys, ascending.
pub struct SetView<'a, S: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    _type: PhantomData<(fn() -> *const S, fn() -> F)>,
}

impl<S: ?Sized, F: ViewTag> Clone for SetView<'_, S, F> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<S: ?Sized, F: ViewTag> Copy for SetView<'_, S, F> {}

impl<S: ?Sized, F: ViewTag> std::fmt::Debug for SetView<'_, S, F> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SetView").field("type", &std::any::type_name::<S>()).finish()
    }
}

impl<'a, S: ?Sized, F: ViewTag> SetView<'a, S, F> {
    /// The view of the set a cursor finds. For the code that implements a
    /// format.
    #[inline]
    pub fn at(cursor: F::Cursor<'a>) -> Self {
        SetView { cursor, _type: PhantomData }
    }
}

impl<'a, S: SetShape + ?Sized, F: ReadsSet<S>> SetView<'a, S, F> {
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
    pub fn get(&self, i: usize) -> Option<<S::Key as ViewShape>::Shape<'a, F>> {
        (i < self.len()).then(|| F::key(self.cursor, i))
    }

    /// The keys, ascending.
    #[inline]
    pub fn iter(&self) -> SetIter<'a, S, F> {
        SetIter { cursor: self.cursor, next: 0, end: self.len(), _type: PhantomData }
    }

    /// The index of `key`. Binary search, as [`MapView::find`].
    #[inline]
    pub fn find<Q>(&self, key: Q) -> Option<usize>
    where
        <S::Key as ViewShape>::Shape<'a, F>: PartialOrd<Q>,
    {
        let len = self.len();
        let i = lower_bound(len, |i| F::key(self.cursor, i).partial_cmp(&key));
        (i < len && F::key(self.cursor, i).partial_cmp(&key) == Some(Ordering::Equal)).then_some(i)
    }

    /// Whether `key` is in the set: `set.contains("name")`.
    #[inline]
    pub fn contains<Q>(&self, key: Q) -> bool
    where
        <S::Key as ViewShape>::Shape<'a, F>: PartialOrd<Q>,
    {
        self.find(key).is_some()
    }
}

/// The keys of a [`SetView`].
pub struct SetIter<'a, S: ?Sized, F: ViewTag> {
    cursor: F::Cursor<'a>,
    next: usize,
    end: usize,
    _type: PhantomData<(fn() -> *const S, fn() -> F)>,
}

impl<'a, S: SetShape + ?Sized, F: ReadsSet<S>> Iterator for SetIter<'a, S, F> {
    type Item = <S::Key as ViewShape>::Shape<'a, F>;
    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            let i = self.next;
            self.next += 1;
            Some(F::key(self.cursor, i))
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
impl<'a, S: SetShape + ?Sized, F: ReadsSet<S>> DoubleEndedIterator for SetIter<'a, S, F> {
    #[inline]
    fn next_back(&mut self) -> Option<Self::Item> {
        if self.next < self.end {
            self.end -= 1;
            Some(F::key(self.cursor, self.end))
        } else {
            None
        }
    }
}
impl<'a, S: SetShape + ?Sized, F: ReadsSet<S>> ExactSizeIterator for SetIter<'a, S, F> {}

impl<'a, S: SetShape + ?Sized, F: ReadsSet<S>> IntoIterator for SetView<'a, S, F> {
    type Item = <S::Key as ViewShape>::Shape<'a, F>;
    type IntoIter = SetIter<'a, S, F>;
    #[inline]
    fn into_iter(self) -> SetIter<'a, S, F> {
        self.iter()
    }
}
impl<'a, S: SetShape + ?Sized, F: ReadsSet<S>> IntoIterator for &SetView<'a, S, F> {
    type Item = <S::Key as ViewShape>::Shape<'a, F>;
    type IntoIter = SetIter<'a, S, F>;
    #[inline]
    fn into_iter(self) -> SetIter<'a, S, F> {
        self.iter()
    }
}

// ── Shapes of the library's types ───────────────────────────────────────────

use crate::pssz::{BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, RawString, Uint256};
use std::collections::{BTreeMap, BTreeSet};

macro_rules! value_shape {
    ($($ty:ty),*) => {$(
        impl ViewShape for $ty {
            type Shape<'a, F: ViewTag> = $ty;
        }
    )*};
}
value_shape!(bool, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, Uint256);

impl ViewShape for String {
    type Shape<'a, F: ViewTag> = &'a str;
}
impl<const N: usize> ViewShape for BoundedString<N> {
    type Shape<'a, F: ViewTag> = &'a str;
}
impl ViewShape for RawString {
    type Shape<'a, F: ViewTag> = &'a [u8];
}
impl<const N: usize> ViewShape for BoundedRawString<N> {
    type Shape<'a, F: ViewTag> = &'a [u8];
}

impl<T: ViewShape> ViewShape for Option<T> {
    type Shape<'a, F: ViewTag> = Option<T::Shape<'a, F>>;
}
impl<T: ViewShape> WrapShape for Option<T> {
    type Inner = T;
}
impl<T: ViewShape + ?Sized> ViewShape for Box<T> {
    type Shape<'a, F: ViewTag> = T::Shape<'a, F>;
}
impl<T: ViewShape> WrapShape for Box<T> {
    type Inner = T;
}

impl<T: ViewShape> ViewShape for Vec<T> {
    type Shape<'a, F: ViewTag> = ListView<'a, Vec<T>, F>;
}
impl<T: ViewShape> ListShape for Vec<T> {
    type Elem = T;
}
impl<T: ViewShape, const N: usize> ViewShape for [T; N] {
    type Shape<'a, F: ViewTag> = ListView<'a, [T; N], F>;
}
impl<T: ViewShape, const N: usize> ListShape for [T; N] {
    type Elem = T;
}
impl<T: ViewShape, const N: usize> ViewShape for BoundedList<T, N> {
    type Shape<'a, F: ViewTag> = ListView<'a, BoundedList<T, N>, F>;
}
impl<T: ViewShape, const N: usize> ListShape for BoundedList<T, N> {
    type Elem = T;
}

impl<K: ViewShape, V: ViewShape> ViewShape for BTreeMap<K, V> {
    type Shape<'a, F: ViewTag> = MapView<'a, BTreeMap<K, V>, F>;
}
impl<K: ViewShape, V: ViewShape> MapShape for BTreeMap<K, V> {
    type Key = K;
    type Value = V;
}
impl<K: ViewShape, V: ViewShape, const N: usize> ViewShape for BoundedMap<K, V, N> {
    type Shape<'a, F: ViewTag> = MapView<'a, BoundedMap<K, V, N>, F>;
}
impl<K: ViewShape, V: ViewShape, const N: usize> MapShape for BoundedMap<K, V, N> {
    type Key = K;
    type Value = V;
}
impl<K: ViewShape> ViewShape for BTreeSet<K> {
    type Shape<'a, F: ViewTag> = SetView<'a, BTreeSet<K>, F>;
}
impl<K: ViewShape> SetShape for BTreeSet<K> {
    type Key = K;
}
impl<K: ViewShape, const N: usize> ViewShape for BoundedSet<K, N> {
    type Shape<'a, F: ViewTag> = SetView<'a, BoundedSet<K, N>, F>;
}
impl<K: ViewShape, const N: usize> SetShape for BoundedSet<K, N> {
    type Key = K;
}

macro_rules! tuple_shape {
    ($($ty:ident),*) => {
        impl<$($ty: ViewShape),*> ViewShape for ($($ty,)*) {
            type Shape<'a, F: ViewTag> = ($($ty::Shape<'a, F>,)*);
        }
    };
}
tuple_shape!(A, B);
tuple_shape!(A, B, C);
tuple_shape!(A, B, C, D);

// ── PSSZ ────────────────────────────────────────────────────────────────────

/// Reading through the existing [`PsszView`](crate::PsszView).
pub mod pssz {
    use super::{
        ListIn, ListView, MapIn, MapView, Reads, ReadsField, RecordFormat, SetIn, SetView, ViewShape, ViewTag,
    };
    use crate::pssz::{
        BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, PsszError, PsszFormat, PsszPack,
        PsszValidate, RawString, Uint256,
    };
    use crate::pssz_view::{PsszField, PsszView};
    use std::collections::{BTreeMap, BTreeSet};

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

    /// A record whose PSSZ bytes are viewed: what `#[derive(View)]`
    /// implements, once for every format tag.
    pub trait PsszRecord<F: PsszFormat>: ViewShape + PsszPack<F> + PsszValidate<F> {
        /// The format of the members' position.
        type Beneath: PsszFormat;
    }

    impl<F: PsszFormat, R: PsszRecord<F> + ?Sized> RecordFormat<R> for F {
        type Beneath = R::Beneath;
        #[inline]
        fn validate(bytes: &[u8]) -> Result<(), PsszError> {
            PsszView::<R, F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn root(bytes: &[u8]) -> &[u8] {
            bytes
        }
    }

    /// Member `I` of a record: its type is the one PSSZ has for it
    /// (`PsszField::Field`, which for a member with a length bound from
    /// `#[psio(max = ...)]` is the bounded type and not the declared one),
    /// so it is read at that type's widths.
    impl<F, R, const I: usize> ReadsField<R, I> for F
    where
        F: PsszFormat,
        R: PsszRecord<F> + PsszField<F, I, Beneath = <R as PsszRecord<F>>::Beneath> + ?Sized,
        <R as PsszField<F, I>>::Field: PsszShape<<R as PsszRecord<F>>::Beneath>,
    {
        type Type = <R as PsszField<F, I>>::Field;
        #[inline]
        fn field<'a>(
            record: &'a [u8],
        ) -> <<R as PsszField<F, I>>::Field as ViewShape>::Shape<'a, <R as PsszRecord<F>>::Beneath>
        where
            'a: 'a,
        {
            <<R as PsszField<F, I>>::Field as PsszShape<<R as PsszRecord<F>>::Beneath>>::from_pssz(
                PsszView::<R, F>::trusted(record).field::<I>().bytes(),
            )
        }
    }

    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + PsszValidate<F> + ?Sized> Reads<T> for F {
        #[inline]
        fn validate(bytes: &[u8]) -> Result<(), PsszError> {
            PsszView::<T, F>::new(bytes).map(|_| ())
        }
        #[inline]
        fn root(bytes: &[u8]) -> T::Shape<'_, F> {
            T::from_pssz(bytes)
        }
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
    value!(bool, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, Uint256);

    /// Panics on bytes that are not UTF-8, which only a view of bytes
    /// nobody validated can hold.
    #[inline]
    fn utf8(bytes: &[u8]) -> &str {
        crate::pssz::utf8_str(bytes).expect("psio view: a string member is not UTF-8; validate first")
    }
    #[inline]
    fn within(bytes: &[u8], max: usize) -> &[u8] {
        assert!(bytes.len() <= max, "psio view: a bounded string is longer than its bound; validate first");
        bytes
    }

    impl<F: PsszFormat> PsszShape<F> for String {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> &'a str {
            utf8(bytes)
        }
    }
    impl<F: PsszFormat, const N: usize> PsszShape<F> for BoundedString<N> {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> &'a str {
            utf8(within(bytes, N))
        }
    }
    impl<F: PsszFormat> PsszShape<F> for RawString {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> &'a [u8] {
            bytes
        }
    }
    impl<F: PsszFormat, const N: usize> PsszShape<F> for BoundedRawString<N> {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> &'a [u8] {
            within(bytes, N)
        }
    }

    impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static> PsszShape<F> for Option<T> {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> Option<T::Shape<'a, F>> {
            PsszView::<Option<T>, F>::trusted(bytes).get().map(|value| T::from_pssz(value.bytes()))
        }
    }

    impl<F: PsszFormat, T: PsszShape<F> + ?Sized> PsszShape<F> for Box<T> {
        #[inline]
        fn from_pssz<'a>(bytes: &'a [u8]) -> T::Shape<'a, F> {
            T::from_pssz(bytes)
        }
    }

    // Lists.
    macro_rules! list {
        ([$($generics:tt)*], $ty:ty, $len:expr) => {
            impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static, $($generics)*> PsszShape<F> for $ty {
                #[inline]
                fn from_pssz<'a>(bytes: &'a [u8]) -> ListView<'a, $ty, F> {
                    ListView::at(bytes)
                }
            }
            impl<F: PsszFormat, T: PsszShape<F> + PsszPack<F> + 'static, $($generics)*> ListIn<F> for $ty {
                #[inline]
                fn len(list: &[u8]) -> usize {
                    let len: fn(&[u8]) -> usize = $len;
                    len(list)
                }
                #[inline]
                fn element<'a>(list: &'a [u8], i: usize) -> T::Shape<'a, F>
                where
                    'a: 'a,
                {
                    T::from_pssz(PsszView::<$ty, F>::trusted(list).get(i).bytes())
                }
            }
        };
    }
    list!([], Vec<T>, |list| PsszView::<Vec<T>, F>::trusted(list).len());
    list!([const N: usize], [T; N], |_| N);
    list!([const N: usize], BoundedList<T, N>, |list| PsszView::<BoundedList<T, N>, F>::trusted(list).len());

    // Maps and sets: the list of the entries, in ascending key order.
    macro_rules! map {
        ([$($generics:tt)*], $ty:ty) => {
            impl<F: PsszFormat, K: PsszShape<F> + PsszPack<F>, V: PsszShape<F> + PsszPack<F>, $($generics)*> PsszShape<F> for $ty {
                #[inline]
                fn from_pssz<'a>(bytes: &'a [u8]) -> MapView<'a, $ty, F> {
                    MapView::at(bytes)
                }
            }
            impl<F: PsszFormat, K: PsszShape<F> + PsszPack<F>, V: PsszShape<F> + PsszPack<F>, $($generics)*> MapIn<F> for $ty {
                #[inline]
                fn len(map: &[u8]) -> usize {
                    PsszView::<$ty, F>::trusted(map).len()
                }
                #[inline]
                fn key<'a>(map: &'a [u8], i: usize) -> K::Shape<'a, F>
                where
                    'a: 'a,
                {
                    K::from_pssz(PsszView::<$ty, F>::trusted(map).get(i).key().bytes())
                }
                #[inline]
                fn value<'a>(map: &'a [u8], i: usize) -> V::Shape<'a, F>
                where
                    'a: 'a,
                {
                    V::from_pssz(PsszView::<$ty, F>::trusted(map).get(i).value().bytes())
                }
            }
        };
    }
    map!([], BTreeMap<K, V>);
    map!([const N: usize], BoundedMap<K, V, N>);

    macro_rules! set {
        ([$($generics:tt)*], $ty:ty) => {
            impl<F: PsszFormat, K: PsszShape<F> + PsszPack<F>, $($generics)*> PsszShape<F> for $ty {
                #[inline]
                fn from_pssz<'a>(bytes: &'a [u8]) -> SetView<'a, $ty, F> {
                    SetView::at(bytes)
                }
            }
            impl<F: PsszFormat, K: PsszShape<F> + PsszPack<F>, $($generics)*> SetIn<F> for $ty {
                #[inline]
                fn len(set: &[u8]) -> usize {
                    PsszView::<$ty, F>::trusted(set).len()
                }
                #[inline]
                fn key<'a>(set: &'a [u8], i: usize) -> K::Shape<'a, F>
                where
                    'a: 'a,
                {
                    K::from_pssz(PsszView::<$ty, F>::trusted(set).get(i).bytes())
                }
            }
        };
    }
    set!([], BTreeSet<K>);
    set!([const N: usize], BoundedSet<K, N>);

    // Tuples: the elements are at the tuple's own position.
    macro_rules! tuple {
        ($(($ty:ident, $index:tt)),*) => {
            impl<F: PsszFormat, $($ty: PsszShape<F> + PsszPack<F>),*> PsszShape<F> for ($($ty,)*) {
                #[inline]
                fn from_pssz<'a>(bytes: &'a [u8]) -> ($($ty::Shape<'a, F>,)*) {
                    let tuple = PsszView::<($($ty,)*), F>::trusted(bytes);
                    ($($ty::from_pssz(tuple.field::<$index>().bytes()),)*)
                }
            }
        };
    }
    tuple!((A, 0), (B, 1));
    tuple!((A, 0), (B, 1), (C, 2));
    tuple!((A, 0), (B, 1), (C, 2), (D, 3));
}

// ── FlatBuffers ─────────────────────────────────────────────────────────────

/// Reading through the existing [`FbView`](crate::flatbuf::FbView).
#[cfg(feature = "flatbuf")]
pub mod flatbuf {
    use super::{
        ListIn, ListView, MapIn, MapView, Reads, ReadsField, RecordFormat, SetIn, SetView, View, ViewShape, ViewTag,
    };
    use crate::flatbuf::verify::check_buffer;
    use crate::flatbuf::{
        FbError, FbLimits, FbMap, FbOptions, FbSet, FbTable, FbUnpack, FbVec, FbView, FbViewable, Flatbuf,
    };
    use crate::pssz::{BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, RawString};
    use std::collections::{BTreeMap, BTreeSet};

    /// Where a table, a struct, a vector or a map is in a buffer.
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

    /// A type that reads as its shape from a FlatBuffers view.
    ///
    /// The view is that of [`Native`](FbShape::Native), the type
    /// FlatBuffers stores: the type itself, except for the types that only
    /// add a bound (`BoundedList<T, N>` is stored as a `Vec<T>`).
    pub trait FbShape: ViewShape {
        type Native: FbViewable;
        fn from_fb<'a>(view: <Self::Native as FbViewable>::View<'a>) -> Self::Shape<'a, Flatbuf>;
    }

    /// A record whose FlatBuffers bytes are viewed: what `#[derive(View)]`
    /// implements.
    pub trait FbRecord: ViewShape + FbUnpack {}

    /// Member `I` of a record.
    pub trait FbField<const I: usize>: FbRecord {
        type Type: ViewShape;
        fn field<'a>(record: FbCursor<'a>) -> <Self::Type as ViewShape>::Shape<'a, Flatbuf>;
    }

    impl<R: FbRecord> RecordFormat<R> for Flatbuf {
        type Beneath = Flatbuf;
        /// Whether `bytes` are a buffer whose root is a valid `R`: the
        /// verifier of [`FbView::new`]. (A record that is a struct is never
        /// the root of a buffer; for it this is an error and not, as in
        /// `FbView::new`, a failure to compile.)
        #[inline]
        fn validate(bytes: &[u8]) -> Result<(), FbError> {
            if !R::TABLE {
                return Err(FbError::new("flatbuf: the root of a buffer is a table", 0));
            }
            check_buffer(bytes, &FbOptions::default(), FbLimits::default(), R::fb_check_table)
        }
        #[inline]
        fn root(bytes: &[u8]) -> FbCursor<'_> {
            FbCursor { buffer: bytes, position: FbTable::root(bytes).position(), count: 0 }
        }
    }

    impl<R: FbField<I>, const I: usize> ReadsField<R, I> for Flatbuf {
        type Type = R::Type;
        #[inline]
        fn field<'a>(record: FbCursor<'a>) -> <R::Type as ViewShape>::Shape<'a, Flatbuf>
        where
            'a: 'a,
        {
            R::field(record)
        }
    }

    impl<R: FbRecord + ViewShape> Reads<R> for Flatbuf
    where
        for<'a> R: ViewShape<Shape<'a, Flatbuf> = View<'a, R, Flatbuf>>,
    {
        #[inline]
        fn validate(bytes: &[u8]) -> Result<(), FbError> {
            <Flatbuf as RecordFormat<R>>::validate(bytes)
        }
        #[inline]
        fn root(bytes: &[u8]) -> View<'_, R, Flatbuf> {
            View::at(<Flatbuf as RecordFormat<R>>::root(bytes))
        }
    }

    macro_rules! same {
        ($($ty:ty => $view:ty),*) => {$(
            impl FbShape for $ty {
                type Native = $ty;
                #[inline]
                fn from_fb<'a>(view: $view) -> $view
                where
                    'a: 'a,
                {
                    view
                }
            }
        )*};
    }
    same!(bool => bool, i8 => i8, i16 => i16, i32 => i32, i64 => i64, u8 => u8, u16 => u16, u32 => u32,
          u64 => u64, f32 => f32, f64 => f64, String => &'a str, RawString => &'a [u8]);

    impl<const N: usize> FbShape for BoundedString<N> {
        type Native = String;
        #[inline]
        fn from_fb<'a>(view: &'a str) -> &'a str
        where
            'a: 'a,
        {
            view
        }
    }
    impl<const N: usize> FbShape for BoundedRawString<N> {
        type Native = RawString;
        #[inline]
        fn from_fb<'a>(view: &'a [u8]) -> &'a [u8]
        where
            'a: 'a,
        {
            view
        }
    }

    impl<T: FbShape> FbShape for Option<T> {
        type Native = Option<T::Native>;
        #[inline]
        fn from_fb<'a>(view: Option<<T::Native as FbViewable>::View<'a>>) -> Option<T::Shape<'a, Flatbuf>>
        where
            'a: 'a,
        {
            view.map(T::from_fb)
        }
    }

    macro_rules! list {
        ([$($generics:tt)*], $ty:ty, $native:ty) => {
            impl<T: FbShape, $($generics)*> FbShape for $ty {
                type Native = $native;
                #[inline]
                fn from_fb<'a>(view: FbVec<'a, T::Native>) -> ListView<'a, $ty, Flatbuf>
                where
                    'a: 'a,
                {
                    let (buffer, position, count) = view.into_parts();
                    ListView::at(FbCursor { buffer, position, count })
                }
            }
            impl<T: FbShape, $($generics)*> ListIn<Flatbuf> for $ty {
                #[inline]
                fn len(list: FbCursor<'_>) -> usize {
                    list.count
                }
                #[inline]
                fn element<'a>(list: FbCursor<'a>, i: usize) -> T::Shape<'a, Flatbuf>
                where
                    'a: 'a,
                {
                    let view = FbVec::<T::Native>::from_parts(list.buffer, list.position, list.count);
                    T::from_fb(view.get(i).expect("psio view: element index within the list"))
                }
            }
        };
    }
    list!([], Vec<T>, Vec<T::Native>);
    list!([const N: usize], [T; N], [T::Native; N]);
    list!([const N: usize], BoundedList<T, N>, Vec<T::Native>);

    macro_rules! map {
        ([$($generics:tt)*], $ty:ty) => {
            impl<K: FbShape, V: FbShape, $($generics)*> FbShape for $ty
            where
                K::Native: Ord,
            {
                type Native = BTreeMap<K::Native, V::Native>;
                #[inline]
                fn from_fb<'a>(view: FbMap<'a, K::Native, V::Native>) -> MapView<'a, $ty, Flatbuf>
                where
                    'a: 'a,
                {
                    let (buffer, position, count) = view.into_parts();
                    MapView::at(FbCursor { buffer, position, count })
                }
            }
            impl<K: FbShape, V: FbShape, $($generics)*> MapIn<Flatbuf> for $ty
            where
                K::Native: Ord,
            {
                #[inline]
                fn len(map: FbCursor<'_>) -> usize {
                    map.count
                }
                #[inline]
                fn key<'a>(map: FbCursor<'a>, i: usize) -> K::Shape<'a, Flatbuf>
                where
                    'a: 'a,
                {
                    let view = FbMap::<K::Native, V::Native>::from_parts(map.buffer, map.position, map.count);
                    K::from_fb(view.key(i).expect("psio view: entry index within the map"))
                }
                #[inline]
                fn value<'a>(map: FbCursor<'a>, i: usize) -> V::Shape<'a, Flatbuf>
                where
                    'a: 'a,
                {
                    let view = FbMap::<K::Native, V::Native>::from_parts(map.buffer, map.position, map.count);
                    V::from_fb(view.value(i).expect("psio view: entry index within the map"))
                }
            }
        };
    }
    map!([], BTreeMap<K, V>);
    map!([const N: usize], BoundedMap<K, V, N>);

    macro_rules! set {
        ([$($generics:tt)*], $ty:ty) => {
            impl<K: FbShape, $($generics)*> FbShape for $ty
            where
                K::Native: Ord,
            {
                type Native = BTreeSet<K::Native>;
                #[inline]
                fn from_fb<'a>(view: FbSet<'a, K::Native>) -> SetView<'a, $ty, Flatbuf>
                where
                    'a: 'a,
                {
                    let (buffer, position, count) = view.into_parts();
                    SetView::at(FbCursor { buffer, position, count })
                }
            }
            impl<K: FbShape, $($generics)*> SetIn<Flatbuf> for $ty
            where
                K::Native: Ord,
            {
                #[inline]
                fn len(set: FbCursor<'_>) -> usize {
                    set.count
                }
                #[inline]
                fn key<'a>(set: FbCursor<'a>, i: usize) -> K::Shape<'a, Flatbuf>
                where
                    'a: 'a,
                {
                    let view = FbSet::<K::Native>::from_parts(set.buffer, set.position, set.count);
                    K::from_fb(view.get(i).expect("psio view: key index within the set"))
                }
            }
        };
    }
    set!([], BTreeSet<K>);
    set!([const N: usize], BoundedSet<K, N>);

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
}
