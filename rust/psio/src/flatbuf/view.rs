//! Typed views over FlatBuffers bytes: the counterpart of C++
//! `psio::flatbuf_view<T>` / `flatbuf_trusted_view<T>`.
//!
//! [`FbView<'a, T>`](FbView) reads a `T` in place. There are two ways to
//! obtain one, as for [`PsszView`](crate::PsszView):
//!
//! * [`FbView::new`] runs the verifier first: use it for untrusted bytes.
//! * [`FbView::trusted`] does not: the caller vouches for the bytes
//!   (verified earlier, or written by `to_flatbuf`).
//!
//! Both are safe on any bytes. Every offset, vtable entry and length is
//! compared with the buffer before it is used, and what does not fit reads
//! as absent — a default, an empty string, an empty list. A view never
//! panics and never reads outside the bytes it was given, whether they
//! were verified or not; verification is what makes the values it reads
//! the values that were written.
//!
//! A member reads as what is natural for its type ([`FbViewable::View`]):
//!
//! | member | reads as |
//! |---|---|
//! | scalar, enum | the value (its default when absent) |
//! | `String` | `&str` |
//! | [`RawString`] | `&[u8]` |
//! | table, struct | [`FbView`] (an absent one reads as defaults) |
//! | `Vec<T>`, `[T; N]` | [`FbVec`] |
//! | `Option<X>` | `Option` of X's view |
//! | `BTreeSet<K>` | [`FbSet`], with key lookup |
//! | `BTreeMap<K, V>` | [`FbMap`], with key lookup |
//! | union | [`FbUnion`], with one accessor per alternative |
//!
//! `#[derive(FbView)]` gives a record's view one method per member
//! (`view.name()`), through a trait named `<Record>FbAccessors` it defines
//! beside the record.

use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet};
use std::marker::PhantomData;

use super::field::{entry_key_cell, fb_fixed_at, string_at, vector_at, FbUnpack};
use super::raw::{self, FbTable, NOWHERE};
use super::verify::{FbError, FbLimits, FbOptions};
use crate::pssz::RawString;

/// A type with a view: what reading it in place gives.
pub trait FbViewable: FbUnpack {
    /// What a member of this type reads as.
    type View<'a>;

    /// The view of the value whose inline cell — a table field or a vector
    /// element — is at `cell`.
    fn fb_view_cell(buf: &[u8], cell: usize) -> Self::View<'_>;

    /// The view of the fixed value at `pos`: a member of a struct.
    fn fb_view_fixed(buf: &[u8], pos: usize) -> Self::View<'_> {
        Self::fb_view_cell(buf, pos)
    }

    /// The view of a member the table does not store.
    fn fb_view_absent(buf: &[u8], default: Option<Self>) -> Self::View<'_>;

    /// The view of the member at vtable slot `slot` of table `t`.
    fn fb_view_member(t: FbTable<'_>, slot: u16, default: Option<Self>) -> Self::View<'_> {
        match t.cell(slot, Self::INLINE_SIZE) {
            Some(cell) => Self::fb_view_cell(t.buffer(), cell),
            None => Self::fb_view_absent(t.buffer(), default),
        }
    }
}

fn view_element<T: FbViewable>(buf: &[u8], pos: usize) -> T::View<'_> {
    if T::INLINE {
        T::fb_view_fixed(buf, pos)
    } else {
        T::fb_view_cell(buf, pos)
    }
}

// ── Records ─────────────────────────────────────────────────────────────────

/// A read-only view of a table or struct `T` inside a buffer.
pub struct FbView<'a, T> {
    buf: &'a [u8],
    pos: usize,
    _type: PhantomData<fn() -> T>,
}

impl<T> Clone for FbView<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for FbView<'_, T> {}

impl<T> std::fmt::Debug for FbView<'_, T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FbView").field("type", &std::any::type_name::<T>()).field("pos", &self.pos).finish()
    }
}

impl<'a, T: FbUnpack> FbView<'a, T> {
    /// Verifies `bytes` as a buffer whose root is a `T`, then borrows.
    pub fn new(bytes: &'a [u8]) -> Result<Self, FbError> {
        T::fb_validate(bytes)?;
        Ok(Self::trusted(bytes))
    }

    /// Verifies under other limits, and / or a buffer with a required file
    /// identifier or a size prefix; the view is of the buffer inside the
    /// prefix.
    pub fn new_with(bytes: &'a [u8], opts: &FbOptions, limits: FbLimits) -> Result<Self, FbError> {
        T::fb_validate_with(bytes, opts, limits)?;
        Ok(Self::trusted(if opts.size_prefixed { bytes.get(4..).unwrap_or(&[]) } else { bytes }))
    }

    /// A view of bytes the caller vouches for; does not verify. Reads of
    /// bytes that would not verify give defaults or unspecified values —
    /// never a panic, never a read outside `bytes`.
    pub fn trusted(bytes: &'a [u8]) -> Self {
        const { assert!(T::TABLE, "psio::flatbuf: the root of a buffer is a table") };
        FbView { buf: bytes, pos: raw::follow(bytes, 0).unwrap_or(NOWHERE), _type: PhantomData }
    }

    /// The view of the `T` at `pos` in `buf` (a table's position, or a
    /// struct's first byte).
    pub fn at(buf: &'a [u8], pos: usize) -> Self {
        FbView { buf, pos, _type: PhantomData }
    }

    /// The view of a `T` that is not stored: every member reads as its
    /// default.
    pub fn absent(buf: &'a [u8]) -> Self {
        FbView { buf, pos: NOWHERE, _type: PhantomData }
    }

    // The methods below are named so that they cannot hide the accessor of
    // a member (`view.position()` is the member `position`).

    /// The whole buffer.
    pub fn fb_buffer(&self) -> &'a [u8] {
        self.buf
    }

    /// The position of the table, or of the struct's first byte.
    pub fn fb_position(&self) -> usize {
        self.pos
    }

    /// The table, for reading a member by vtable slot.
    pub fn fb_table(&self) -> FbTable<'a> {
        FbTable::at(self.buf, self.pos)
    }

    /// The value. Does not verify: on a view made by [`trusted`](Self::trusted)
    /// this is the trusted decode. (A member named `unpack` is read with
    /// `<Record>FbAccessors::unpack(&view)`.)
    pub fn unpack(&self) -> T {
        if T::STRUCT {
            fb_fixed_at(self.buf, self.pos)
        } else {
            T::fb_decode_table(self.fb_table())
        }
    }
}

// ── Scalars and strings ─────────────────────────────────────────────────────

macro_rules! scalar_view {
    ($($ty:ty),*) => {$(
        impl FbViewable for $ty {
            type View<'a> = $ty;
            #[inline]
            fn fb_view_cell(buf: &[u8], cell: usize) -> $ty {
                fb_fixed_at(buf, cell)
            }
            #[inline]
            fn fb_view_absent(_buf: &[u8], default: Option<Self>) -> $ty {
                default.unwrap_or_default()
            }
        }
    )*};
}
scalar_view!(bool, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64);

impl FbViewable for String {
    type View<'a> = &'a str;
    fn fb_view_cell(buf: &[u8], cell: usize) -> &str {
        std::str::from_utf8(string_at(buf, Some(cell))).unwrap_or("")
    }
    fn fb_view_absent(_buf: &[u8], _default: Option<Self>) -> &str {
        ""
    }
}

impl FbViewable for RawString {
    type View<'a> = &'a [u8];
    fn fb_view_cell(buf: &[u8], cell: usize) -> &[u8] {
        string_at(buf, Some(cell))
    }
    fn fb_view_absent(_buf: &[u8], _default: Option<Self>) -> &[u8] {
        &[]
    }
}

impl<T: FbViewable> FbViewable for Option<T> {
    type View<'a> = Option<T::View<'a>>;
    fn fb_view_cell(buf: &[u8], cell: usize) -> Option<T::View<'_>> {
        Some(T::fb_view_cell(buf, cell))
    }
    fn fb_view_absent(_buf: &[u8], _default: Option<Self>) -> Option<T::View<'_>> {
        None
    }
}

// ── Vectors ─────────────────────────────────────────────────────────────────

/// A view of a vector (or of a fixed array inside a struct).
pub struct FbVec<'a, T> {
    buf: &'a [u8],
    data: usize,
    count: usize,
    _type: PhantomData<fn() -> T>,
}

impl<T> Clone for FbVec<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for FbVec<'_, T> {}

impl<T> std::fmt::Debug for FbVec<'_, T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FbVec").field("len", &self.count).finish()
    }
}

impl<'a, T: FbViewable> FbVec<'a, T> {
    fn empty(buf: &'a [u8]) -> Self {
        FbVec { buf, data: 0, count: 0, _type: PhantomData }
    }

    /// The vector the offset at `cell` points to; empty when it is not
    /// inside the buffer.
    fn follow(buf: &'a [u8], cell: usize) -> Self {
        match vector_at(buf, cell, T::INLINE_SIZE) {
            Some((data, count)) => FbVec { buf, data, count, _type: PhantomData },
            None => Self::empty(buf),
        }
    }

    /// Number of elements.
    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Element `i`, or `None` past the end.
    pub fn get(&self, i: usize) -> Option<T::View<'a>> {
        (i < self.count).then(|| view_element::<T>(self.buf, self.data + i * T::INLINE_SIZE))
    }

    /// The elements, in order.
    pub fn iter(&self) -> impl ExactSizeIterator<Item = T::View<'a>> + DoubleEndedIterator + 'a {
        let (buf, data) = (self.buf, self.data);
        (0..self.count).map(move |i| view_element::<T>(buf, data + i * T::INLINE_SIZE))
    }

    /// The elements as values.
    pub fn unpack(&self) -> Vec<T> {
        let size = T::INLINE_SIZE;
        (0..self.count)
            .map(|i| {
                let pos = self.data + i * size;
                if T::INLINE {
                    fb_fixed_at(self.buf, pos)
                } else {
                    T::fb_decode_cell(self.buf, pos)
                }
            })
            .collect()
    }
}

impl<'a> FbVec<'a, u8> {
    /// The bytes, borrowed from the buffer.
    pub fn as_bytes(&self) -> &'a [u8] {
        self.buf.get(self.data..self.data + self.count).unwrap_or(&[])
    }
}

impl<T: FbViewable> FbViewable for Vec<T> {
    type View<'a> = FbVec<'a, T>;
    fn fb_view_cell(buf: &[u8], cell: usize) -> FbVec<'_, T> {
        FbVec::follow(buf, cell)
    }
    fn fb_view_absent(buf: &[u8], _default: Option<Self>) -> FbVec<'_, T> {
        FbVec::empty(buf)
    }
}

impl<T: FbViewable, const N: usize> FbViewable for [T; N] {
    type View<'a> = FbVec<'a, T>;
    fn fb_view_cell(buf: &[u8], cell: usize) -> FbVec<'_, T> {
        FbVec::follow(buf, cell)
    }
    fn fb_view_fixed(buf: &[u8], pos: usize) -> FbVec<'_, T> {
        match pos.checked_add(N * T::FIXED_SIZE) {
            Some(end) if end <= buf.len() => FbVec { buf, data: pos, count: N, _type: PhantomData },
            _ => FbVec::empty(buf),
        }
    }
    fn fb_view_absent(buf: &[u8], _default: Option<Self>) -> FbVec<'_, T> {
        FbVec::empty(buf)
    }
}

// ── Sorted vectors ──────────────────────────────────────────────────────────

/// A key of a set or map: how a stored key compares with one to look up.
pub trait FbKey: FbViewable {
    /// What a lookup takes: `str` for a string key, the scalar itself
    /// otherwise.
    type Lookup: ?Sized;

    /// The stored key at `cell` (the default when absent) against `key`.
    fn fb_key_cmp(buf: &[u8], cell: Option<usize>, key: &Self::Lookup) -> Ordering;
}

macro_rules! scalar_key {
    ($($ty:ty),*) => {$(
        impl FbKey for $ty {
            type Lookup = $ty;
            fn fb_key_cmp(buf: &[u8], cell: Option<usize>, key: &$ty) -> Ordering {
                let stored: $ty = cell.map_or_else(<$ty>::default, |cell| fb_fixed_at(buf, cell));
                stored.cmp(key)
            }
        }
    )*};
}
scalar_key!(bool, i8, i16, i32, i64, u8, u16, u32, u64);

impl FbKey for String {
    type Lookup = str;
    fn fb_key_cmp(buf: &[u8], cell: Option<usize>, key: &str) -> Ordering {
        string_at(buf, cell).cmp(key.as_bytes())
    }
}

impl FbKey for RawString {
    type Lookup = [u8];
    fn fb_key_cmp(buf: &[u8], cell: Option<usize>, key: &[u8]) -> Ordering {
        string_at(buf, cell).cmp(key)
    }
}

/// The first index in `0..count` whose key is not less than the one
/// `cmp` compares with. Terminates and stays in range whatever the order
/// of the stored keys is.
fn lower_bound(count: usize, cmp: impl Fn(usize) -> Ordering) -> usize {
    let (mut lo, mut hi) = (0, count);
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        if cmp(mid) == Ordering::Less {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    lo
}

/// A view of a set: a vector sorted by key.
pub struct FbSet<'a, K> {
    items: FbVec<'a, K>,
}

impl<K> Clone for FbSet<'_, K> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<K> Copy for FbSet<'_, K> {}

impl<K> std::fmt::Debug for FbSet<'_, K> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FbSet").field("len", &self.items.count).finish()
    }
}

impl<'a, K: FbViewable> FbSet<'a, K> {
    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// Key `i` in ascending order, or `None` past the end.
    pub fn get(&self, i: usize) -> Option<K::View<'a>> {
        self.items.get(i)
    }

    /// The keys, ascending.
    pub fn iter(&self) -> impl ExactSizeIterator<Item = K::View<'a>> + DoubleEndedIterator + 'a {
        self.items.iter()
    }
}

impl<'a, K: FbKey> FbSet<'a, K> {
    fn cmp_at(&self, i: usize, key: &K::Lookup) -> Ordering {
        K::fb_key_cmp(self.items.buf, Some(self.items.data + i * K::INLINE_SIZE), key)
    }

    /// The index of the first key that is not less than `key` (`len()` when
    /// there is none). Binary search.
    pub fn lower_bound(&self, key: &K::Lookup) -> usize {
        lower_bound(self.len(), |i| self.cmp_at(i, key))
    }

    /// The index of `key`.
    pub fn find(&self, key: &K::Lookup) -> Option<usize> {
        let i = self.lower_bound(key);
        (i < self.len() && self.cmp_at(i, key) == Ordering::Equal).then_some(i)
    }

    pub fn contains(&self, key: &K::Lookup) -> bool {
        self.find(key).is_some()
    }
}

impl<K: FbViewable + Ord> FbViewable for BTreeSet<K> {
    type View<'a> = FbSet<'a, K>;
    fn fb_view_cell(buf: &[u8], cell: usize) -> FbSet<'_, K> {
        FbSet { items: FbVec::follow(buf, cell) }
    }
    fn fb_view_absent(buf: &[u8], _default: Option<Self>) -> FbSet<'_, K> {
        FbSet { items: FbVec::empty(buf) }
    }
}

/// A view of a map: a vector of `{key, value}` tables sorted by key.
pub struct FbMap<'a, K, V> {
    buf: &'a [u8],
    data: usize,
    count: usize,
    _type: PhantomData<fn() -> (K, V)>,
}

impl<K, V> Clone for FbMap<'_, K, V> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<K, V> Copy for FbMap<'_, K, V> {}

impl<K, V> std::fmt::Debug for FbMap<'_, K, V> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FbMap").field("len", &self.count).finish()
    }
}

impl<'a, K: FbViewable, V: FbViewable> FbMap<'a, K, V> {
    fn entry(&self, i: usize) -> FbTable<'a> {
        FbTable::follow(self.buf, self.data + i * 4)
    }

    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Key `i` in ascending order, or `None` past the end.
    pub fn key(&self, i: usize) -> Option<K::View<'a>> {
        (i < self.count).then(|| K::fb_view_member(self.entry(i), 0, None))
    }

    /// The value of entry `i`, or `None` past the end.
    pub fn value(&self, i: usize) -> Option<V::View<'a>> {
        (i < self.count).then(|| V::fb_view_member(self.entry(i), 1, None))
    }

    /// The entries as `(key, value)`, by ascending key.
    pub fn iter(&self) -> impl ExactSizeIterator<Item = (K::View<'a>, V::View<'a>)> + DoubleEndedIterator + 'a {
        let (buf, data) = (self.buf, self.data);
        (0..self.count).map(move |i| {
            let entry = FbTable::follow(buf, data + i * 4);
            (K::fb_view_member(entry, 0, None), V::fb_view_member(entry, 1, None))
        })
    }
}

impl<'a, K: FbKey, V: FbViewable> FbMap<'a, K, V> {
    fn cmp_at(&self, i: usize, key: &K::Lookup) -> Ordering {
        K::fb_key_cmp(self.buf, entry_key_cell::<K>(self.buf, self.data + i * 4), key)
    }

    /// The index of the first entry whose key is not less than `key`
    /// (`len()` when there is none). Binary search.
    pub fn lower_bound(&self, key: &K::Lookup) -> usize {
        lower_bound(self.count, |i| self.cmp_at(i, key))
    }

    /// The index of the entry of `key`.
    pub fn find(&self, key: &K::Lookup) -> Option<usize> {
        let i = self.lower_bound(key);
        (i < self.count && self.cmp_at(i, key) == Ordering::Equal).then_some(i)
    }

    pub fn contains_key(&self, key: &K::Lookup) -> bool {
        self.find(key).is_some()
    }

    /// The value of `key`.
    pub fn get(&self, key: &K::Lookup) -> Option<V::View<'a>> {
        self.find(key).and_then(|i| self.value(i))
    }
}

impl<K: FbViewable + Ord, V: FbViewable> FbViewable for BTreeMap<K, V> {
    type View<'a> = FbMap<'a, K, V>;
    fn fb_view_cell(buf: &[u8], cell: usize) -> FbMap<'_, K, V> {
        match vector_at(buf, cell, 4) {
            Some((data, count)) => FbMap { buf, data, count, _type: PhantomData },
            None => Self::fb_view_absent(buf, None),
        }
    }
    fn fb_view_absent(buf: &[u8], _default: Option<Self>) -> FbMap<'_, K, V> {
        FbMap { buf, data: 0, count: 0, _type: PhantomData }
    }
}

// ── Unions ──────────────────────────────────────────────────────────────────

/// A view of a union member: its type and, through the accessors
/// `#[derive(FbView)]` adds for each alternative, its value.
pub struct FbUnion<'a, U> {
    buf: &'a [u8],
    tag: u8,
    pos: usize,
    _type: PhantomData<fn() -> U>,
}

impl<U> Clone for FbUnion<'_, U> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<U> Copy for FbUnion<'_, U> {}

impl<U> std::fmt::Debug for FbUnion<'_, U> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FbUnion").field("tag", &self.tag).finish()
    }
}

impl<'a, U> FbUnion<'a, U> {
    /// The union stored at slots `slot` (type) and `slot + 1` (value) of
    /// `t`. A type without a value, or a value without a type, is `NONE`.
    pub fn member(t: FbTable<'a>, slot: u16) -> Self {
        let buf = t.buffer();
        let tag = t.cell(slot, 1).and_then(|cell| buf.get(cell).copied()).unwrap_or(0);
        let pos = t.cell(slot + 1, 4).and_then(|cell| raw::follow(buf, cell));
        match pos {
            Some(pos) if tag != 0 => FbUnion { buf, tag, pos, _type: PhantomData },
            _ => Self::none(buf),
        }
    }

    /// A union that holds nothing.
    pub fn none(buf: &'a [u8]) -> Self {
        FbUnion { buf, tag: 0, pos: NOWHERE, _type: PhantomData }
    }

    /// The union type on the wire: 0 is `NONE`, 1 the first alternative
    /// that holds a table.
    pub fn tag(&self) -> u8 {
        self.tag
    }

    /// Whether the union holds nothing.
    pub fn is_none(&self) -> bool {
        self.tag == 0
    }

    /// The view of the alternative whose wire type is `tag`, when the
    /// union holds it.
    pub fn alternative<T: FbUnpack>(&self, tag: u8) -> Option<FbView<'a, T>> {
        (self.tag == tag && tag != 0).then(|| FbView::at(self.buf, self.pos))
    }

    /// The table the union holds (no table when it is `NONE`).
    pub fn fb_table(&self) -> FbTable<'a> {
        FbTable::at(self.buf, self.pos)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lower_bound_terminates_on_any_order() {
        for keys in [vec![], vec![1], vec![3, 1, 2], vec![5, 5, 5], vec![9, 0, 9, 0, 9]] {
            for probe in 0..10 {
                let i = lower_bound(keys.len(), |i| keys[i].cmp(&probe));
                assert!(i <= keys.len());
            }
        }
    }
}
