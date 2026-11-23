//! Bounds by annotation (docs/pssz-spec.md 3.5): a member keeps its plain
//! type and its bounds are written on it.
//!
//! ```
//! use psio::pssz::{BoundedList, BoundedString};
//! use psio::{PsszPack, PsszUnpack, ToPsszSchema};
//!
//! #[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
//! struct Post {
//!     /// At most 8 tags of at most 16 bytes each.
//!     #[psio(max = 8, element(max = 16))]
//!     tags: Vec<String>,
//! }
//!
//! // The same type, with the bounds in the member's type.
//! #[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
//! #[psio(name = "Post")]
//! struct Wrapped {
//!     tags: BoundedList<BoundedString<16>, 8>,
//! }
//!
//! let post = Post { tags: vec!["rust".into(), "pssz".into()] };
//! let bytes = psio::to_pssz(&post);
//! // The bounds select the record's offset width, exactly as the types do.
//! assert_eq!(psio::pssz::auto_pssz_width::<Post>(), 1);
//! assert_eq!(psio::pssz::auto_pssz_width::<Post>(), psio::pssz::auto_pssz_width::<Wrapped>());
//! assert_eq!(bytes, psio::to_pssz(&psio::from_pssz::<Wrapped>(&bytes)?));
//! // And they are enforced: by the encoder, and by every reader.
//! let long = Post { tags: vec!["x".repeat(17)] };
//! assert!(psio::try_to_pssz(&long).is_err());
//! let nine = psio::to_pssz(&vec![String::new(); 9]);
//! assert!(psio::from_pssz::<Vec<String>>(&nine).is_ok());
//! // The schema is the bounded types'.
//! assert_eq!(
//!     psio::pssz::schema_ir::schema_for::<Post>()?.0,
//!     psio::pssz::schema_ir::schema_for::<Wrapped>()?.0
//! );
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! # The annotation
//!
//! `#[psio(...)]` on a field of a struct, on a field of an enum variant, or
//! on a parameter of a `#[psio::interface]` function:
//!
//! | key | bounds | C++ |
//! |-----|--------|-----|
//! | `max = N` | the member: bytes of a `String` / `RawString`, elements of a `Vec`, `BTreeSet` or `BTreeMap` | `max<N>` |
//! | `element(...)` | every element of a `Vec`, an array or a `BTreeSet` | `element(...)` |
//! | `keys(...)`, `values(...)` | every key, every mapped value of a `BTreeMap` | `keys(...)`, `values(...)` |
//! | `at(I, ...)` | element `I` of a tuple | `at<I>(...)` |
//!
//! | `sorted` | the elements of a `Vec` or an array are in non-descending order | `sorted` |
//! | `unique` (`sorted_unique`) | ... in strictly ascending order | `unique`, `sorted_unique` |
//! | `unique_keys` | no two elements have the same key: the element, or element 0 of a tuple element | `unique_keys` |
//!
//! The last three constrain the *content* of a list without changing any
//! byte, width or bound (docs/pssz-spec.md 3.5, "Order constraints"). Order
//! and equality are those of maps and sets: bytewise comparison of the
//! elements' `key` encodings, the same in every language. A writer refuses
//! a value that violates one and every checking reader rejects a frame that
//! holds one, in pSSZ, Json / IJson text and keys alike. They are not in
//! the Schema IR: a schema-driven reader does not know them.
//!
//! ```
//! use psio::{PsszPack, PsszUnpack};
//!
//! #[derive(Debug, PartialEq, PsszPack, PsszUnpack)]
//! struct Ranked {
//!     #[psio(sorted, max = 4)]
//!     scores: Vec<u32>,
//!     #[psio(unique_keys)]
//!     entries: Vec<(String, u32)>,
//! }
//! let ok = Ranked { scores: vec![1, 1, 2], entries: vec![("b".into(), 1), ("a".into(), 1)] };
//! assert_eq!(psio::from_pssz::<Ranked>(&psio::to_pssz(&ok))?, ok);
//! assert!(psio::try_to_pssz(&Ranked { scores: vec![2, 1], entries: vec![] }).is_err());
//! let twice = vec![("a".to_string(), 1), ("a".to_string(), 2)];
//! assert!(psio::try_to_pssz(&Ranked { scores: vec![], entries: twice }).is_err());
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! A record's field count has a cap of its own, `#[psio(max_fields = N)]`
//! on the struct (docs/pssz-spec.md 16): the record has, and will ever
//! have, at most N fields (N ≤ 255, the limit every record has). It is
//! refused where the struct is declared, written as the `maxFields`
//! attribute of the record's schema, and `pssz_evolves_from` holds every
//! later revision to it.
//!
//! ```compile_fail
//! #[derive(psio::PsszPack)]
//! #[psio(max_fields = 1)] // two fields
//! struct Pair { a: u8, b: u8 }
//! ```
//!
//! The keys nest to any depth (`element(element(max = 4))`), and an `Option`
//! is looked through: what is written for an optional member or position
//! applies to its payload. Whether a string is UTF-8 is its type in Rust
//! (`String` is, `RawString` is not), so there is no `utf8` key. The
//! alternatives of a variant are annotated where they are declared, on the
//! fields of the enum's variants.
//!
//! An annotated member **is** the type the annotation describes:
//! `Vec<String>` under `max = 8, element(max = 16)` is
//! `BoundedList<BoundedString<16>, 8>`. It has that type's bound and
//! widths at every depth, the same bytes, the same schema and the same
//! checks in pSSZ, in Json / IJson text and in keys.
//!
//! # How it works
//!
//! The derives turn the annotation into a marker type `S` ([`Max`],
//! [`Each`], [`Entry`], [`At`], [`Plain`]) and use [`Bounds<S>`] of the
//! field's type: `As` is the described type, which supplies every constant
//! (bound, width, layout), the validator, the reader and the schema. A
//! value is written from the field as it stands, with the described type's
//! widths ([`PackAs`]): a bound can narrow the offset table of a list, a
//! map or a set, the header of a tuple or a map entry, at any depth, and
//! none of that converts or copies the value.
//!
//! The bounds are the member's own. The cap of the record that holds the
//! member is not an annotation: it reaches the member through the format
//! tag ([`PsszFormat::Under`](crate::pssz::PsszFormat::Under)), and the
//! described type, like every type, takes it into its widths
//! (docs/pssz-spec.md 3.6).

use crate::key::KeyPack;
use crate::pssz::assoc::{canonical_order, encode_map, MAP_DUPLICATE, SET_DUPLICATE};
use crate::pssz::{
    BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, PsszError, PsszFormat, PsszPack,
    PsszResult, RawString, Shape,
};
use crate::pssz_checked as checked;
use std::collections::{BTreeMap, BTreeSet};
use std::marker::PhantomData;

/// No annotation: the value as its own type.
pub struct Plain;
/// `max = N`, with the annotation `E` of what the value holds (`element`,
/// or [`Entry`] for a map).
pub struct Max<const N: usize, E = Plain>(PhantomData<E>);
/// `element(...)` without a bound on the container itself (or [`Entry`]
/// for a map).
pub struct Each<E>(PhantomData<E>);
/// `keys(K)` and `values(V)` of a map.
pub struct Entry<K, V>(PhantomData<(K, V)>);
/// `at(I, ...)`: one annotation per tuple element, as a tuple of markers.
pub struct At<T>(PhantomData<T>);

/// An order constraint `R` ([`InOrder`], [`Ascending`], [`DistinctKeys`],
/// [`DistinctFirst`]) on a list or an array whose own annotation is `S`.
pub struct Order<R, S = Plain>(PhantomData<(R, S)>);
/// `sorted`: non-descending key order.
pub struct InOrder;
/// `unique` / `sorted_unique`: strictly ascending key order.
pub struct Ascending;
/// `unique_keys`: no two elements have the same key.
pub struct DistinctKeys;
/// `unique_keys` on a list of pairs or tuples: no two have the same
/// element 0.
pub struct DistinctFirst;

const VIOLATED: PsszError = PsszError::new("pssz: member length_bound violated");
const NOT_IN_ORDER: PsszError = PsszError::new("sorted: elements are not in order");
const NOT_ASCENDING: PsszError = PsszError::new("sorted: elements are not in strictly ascending order");
const SAME_KEY: PsszError = PsszError::new("unique_keys: two elements have the same key");

/// What an order constraint requires of the elements of a list.
pub trait Rule<I>: 'static {
    /// The constraint as bits of `Shape::order`.
    const ORDER: u8;
    fn check(items: &[I]) -> PsszResult<()>;
}
fn in_order<I: KeyPack>(items: &[I], strict: bool) -> PsszResult<()> {
    let mut last: Option<Vec<u8>> = None;
    for item in items {
        let key = crate::key::key_bytes(item);
        if let Some(last) = &last {
            if *last > key || (strict && *last == key) {
                return Err(if strict { NOT_ASCENDING } else { NOT_IN_ORDER });
            }
        }
        last = Some(key);
    }
    Ok(())
}
fn distinct(mut keys: Vec<Vec<u8>>) -> PsszResult<()> {
    keys.sort();
    if keys.windows(2).any(|pair| pair[0] == pair[1]) {
        return Err(SAME_KEY);
    }
    Ok(())
}
impl<I: KeyPack> Rule<I> for InOrder {
    const ORDER: u8 = Shape::SORTED;
    fn check(items: &[I]) -> PsszResult<()> {
        in_order(items, false)
    }
}
impl<I: KeyPack> Rule<I> for Ascending {
    const ORDER: u8 = Shape::SORTED | Shape::STRICT;
    fn check(items: &[I]) -> PsszResult<()> {
        in_order(items, true)
    }
}
impl<I: KeyPack> Rule<I> for DistinctKeys {
    const ORDER: u8 = Shape::UNIQUE_KEYS;
    fn check(items: &[I]) -> PsszResult<()> {
        distinct(items.iter().map(crate::key::key_bytes).collect())
    }
}
/// A pair or tuple, for `unique_keys`: its key is its first element.
pub trait First {
    type Head: KeyPack;
    fn head(&self) -> &Self::Head;
}
macro_rules! first {
    ($($rest:ident),*) => {
        impl<A: KeyPack, $($rest),*> First for (A, $($rest,)*) {
            type Head = A;
            fn head(&self) -> &A {
                &self.0
            }
        }
    };
}
first!();
first!(B);
first!(B, C);
first!(B, C, D);
first!(B, C, D, E);
first!(B, C, D, E, G);
first!(B, C, D, E, G, H);
first!(B, C, D, E, G, H, I);
impl<I: First> Rule<I> for DistinctFirst {
    const ORDER: u8 = Shape::UNIQUE_KEYS;
    fn check(items: &[I]) -> PsszResult<()> {
        distinct(items.iter().map(|item| crate::key::key_bytes(item.head())).collect())
    }
}

/// The elements of a list or an array.
pub trait Elements {
    type Item;
    fn elements(&self) -> &[Self::Item];
}
impl<T> Elements for Vec<T> {
    type Item = T;
    fn elements(&self) -> &[T] {
        self
    }
}
impl<T, const N: usize> Elements for BoundedList<T, N> {
    type Item = T;
    fn elements(&self) -> &[T] {
        &self.0
    }
}
impl<T, const M: usize> Elements for [T; M] {
    type Item = T;
    fn elements(&self) -> &[T] {
        self
    }
}
impl<L: Elements, R> Elements for Ordered<L, R> {
    type Item = L::Item;
    fn elements(&self) -> &[L::Item] {
        self.0.elements()
    }
}

/// The type an order constraint describes: the list `L`, whose elements
/// satisfy `R`. The bytes, the widths, the bound and the schema are `L`'s;
/// the writer and every checking reader also check the order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Ordered<L, R>(pub L, PhantomData<R>);
impl<L, R> Ordered<L, R> {
    pub fn new(list: L) -> Self {
        Self(list, PhantomData)
    }
}

impl<F: PsszFormat, L: PsszPack<F> + Elements, R: Rule<L::Item>> PsszPack<F> for Ordered<L, R> {
    const IS_OPTIONAL: bool = L::IS_OPTIONAL;
    const OPTIONAL_SELECTOR: bool = L::OPTIONAL_SELECTOR;
    const MIN_ENCODED_SIZE: usize = L::MIN_ENCODED_SIZE;
    const MAX_ENCODED_SIZE: Option<usize> = L::MAX_ENCODED_SIZE;
    const IS_FIXED_SIZE: bool = L::IS_FIXED_SIZE;
    const FIXED_SIZE: usize = L::FIXED_SIZE;
    const MAX_DYNAMIC_DATA: Option<usize> = L::MAX_DYNAMIC_DATA;
    const AUTO_BOUND: Option<usize> = L::AUTO_BOUND;
    const OFFSET_WIDTH: usize = L::OFFSET_WIDTH;
    const RECORD_WIDTH_BENEATH: usize = L::RECORD_WIDTH_BENEATH;
    // The constraint is the member's type: both sides of an evolution have
    // it (docs/pssz-spec.md 7.5).
    const SHAPE: Shape = L::SHAPE.ordered(L::SHAPE.order | R::ORDER);
    fn pssz_is_none(&self) -> bool {
        self.0.pssz_is_none()
    }
    fn pssz_none_bytes() -> Option<&'static [u8]> {
        L::pssz_none_bytes()
    }
    fn pssz_size(&self) -> usize {
        self.0.pssz_size()
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        self.pssz_pack_checked(out).expect("pssz: the elements violate an order constraint; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        checked::pack_checked::<F, Self>(self, out)
    }
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        R::check(self.0.elements())?;
        self.0.pssz_pack_at_depth(out, depth)
    }
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        R::check(self.0.elements())?;
        self.0.pssz_pack_fixed(slot, depth)
    }
    fn pssz_size_hint(&self) -> usize {
        self.0.pssz_size_hint()
    }
}
impl<F: PsszFormat, L: crate::pssz::PsszUnpack<F> + Elements, R: Rule<L::Item>> crate::pssz::PsszUnpack<F>
    for Ordered<L, R>
{
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let list = L::pssz_unpack(bytes)?;
        R::check(list.elements())?;
        Ok(Self::new(list))
    }
    fn pssz_missing_optional() -> PsszResult<Self> {
        L::pssz_missing_optional().map(Self::new)
    }
    // The caller vouches for the bytes: the order is not checked.
    fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
        Self::new(L::pssz_unpack_trusted(bytes))
    }
}
// The validator does not materialise; an order needs the elements' keys, so
// such a member is decoded from its span, as a type with an invariant is.
impl<F: PsszFormat, L: crate::pssz::PsszUnpack<F> + Elements, R: Rule<L::Item>> crate::pssz::PsszValidate<F>
    for Ordered<L, R>
{
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as crate::pssz::PsszUnpack<F>>::pssz_unpack(bytes).map(drop)
    }
}
// Not in the Schema IR (docs/pssz-spec.md 3.5): the schema is the list's.
impl<L: crate::pssz::schema_ir::ToPsszSchema, R: 'static> crate::pssz::schema_ir::ToPsszSchema for Ordered<L, R> {
    fn member_attributes() -> Vec<crate::pssz::schema_ir::Attribute> {
        L::member_attributes()
    }
    fn build_schema(
        builder: &mut crate::pssz::schema_ir::SchemaBuilder,
    ) -> PsszResult<crate::pssz::schema_ir::AnyType> {
        L::build_schema(builder)
    }
    fn schema(
        builder: &mut crate::pssz::schema_ir::SchemaBuilder,
    ) -> PsszResult<crate::pssz::schema_ir::AnyType> {
        L::schema(builder)
    }
}

macro_rules! ordered_bounds {
    ($([$($generics:tt)*] $list:ty, $item:ty;)*) => {$(
        impl<T, R, S, $($generics)*> Bounds<Order<R, S>> for $list
        where
            $list: Bounds<S>,
            R: Rule<$item>,
        {
            type As = Ordered<<$list as Bounds<S>>::As, R>;
            fn check(&self) -> PsszResult<()> {
                <$list as Bounds<S>>::check(self)?;
                R::check(self)
            }
            fn to_as(&self) -> Self::As {
                Ordered::new(<$list as Bounds<S>>::to_as(self))
            }
            fn from_as(described: Self::As) -> Self {
                <$list as Bounds<S>>::from_as(described.0)
            }
        }
    )*};
}
ordered_bounds! {
    [] Vec<T>, T;
    [const M: usize] [T; M], T;
}

/// A value of this type under the annotation `S`.
pub trait Bounds<S>: Sized {
    /// The type the annotation describes.
    type As;
    /// Every bound of the annotation holds.
    fn check(&self) -> PsszResult<()>;
    /// The value as the described type. Copies; not checked (the described
    /// type's encoder checks).
    fn to_as(&self) -> Self::As;
    /// A described value back as the field's type. Moves.
    fn from_as(described: Self::As) -> Self;
}

impl<T: Clone> Bounds<Plain> for T {
    type As = T;
    fn check(&self) -> PsszResult<()> {
        Ok(())
    }
    fn to_as(&self) -> T {
        self.clone()
    }
    fn from_as(described: T) -> T {
        described
    }
}

impl<const N: usize> Bounds<Max<N>> for String {
    type As = BoundedString<N>;
    fn check(&self) -> PsszResult<()> {
        if self.len() > N {
            return Err(VIOLATED);
        }
        Ok(())
    }
    fn to_as(&self) -> Self::As {
        BoundedString(self.clone())
    }
    fn from_as(described: Self::As) -> Self {
        described.0
    }
}

impl<const N: usize> Bounds<Max<N>> for RawString {
    type As = BoundedRawString<N>;
    fn check(&self) -> PsszResult<()> {
        if self.0.len() > N {
            return Err(VIOLATED);
        }
        Ok(())
    }
    fn to_as(&self) -> Self::As {
        BoundedRawString(self.0.clone())
    }
    fn from_as(described: Self::As) -> Self {
        RawString(described.0)
    }
}

impl<T: Bounds<E>, E, const N: usize> Bounds<Max<N, E>> for Vec<T> {
    type As = BoundedList<T::As, N>;
    fn check(&self) -> PsszResult<()> {
        if self.len() > N {
            return Err(VIOLATED);
        }
        self.iter().try_for_each(T::check)
    }
    fn to_as(&self) -> Self::As {
        BoundedList(self.iter().map(T::to_as).collect())
    }
    fn from_as(described: Self::As) -> Self {
        described.0.into_iter().map(T::from_as).collect()
    }
}

impl<T: Bounds<E>, E> Bounds<Each<E>> for Vec<T> {
    type As = Vec<T::As>;
    fn check(&self) -> PsszResult<()> {
        self.iter().try_for_each(T::check)
    }
    fn to_as(&self) -> Self::As {
        self.iter().map(T::to_as).collect()
    }
    fn from_as(described: Self::As) -> Self {
        described.into_iter().map(T::from_as).collect()
    }
}

impl<T: Bounds<E>, E, const M: usize> Bounds<Each<E>> for [T; M] {
    type As = [T::As; M];
    fn check(&self) -> PsszResult<()> {
        self.iter().try_for_each(T::check)
    }
    fn to_as(&self) -> Self::As {
        std::array::from_fn(|i| self[i].to_as())
    }
    fn from_as(described: Self::As) -> Self {
        described.map(T::from_as)
    }
}

impl<T: Bounds<E> + Ord, E, const N: usize> Bounds<Max<N, E>> for BTreeSet<T>
where
    T::As: Ord,
{
    type As = BoundedSet<T::As, N>;
    fn check(&self) -> PsszResult<()> {
        if self.len() > N {
            return Err(VIOLATED);
        }
        self.iter().try_for_each(T::check)
    }
    fn to_as(&self) -> Self::As {
        BoundedSet(self.iter().map(T::to_as).collect())
    }
    fn from_as(described: Self::As) -> Self {
        described.0.into_iter().map(T::from_as).collect()
    }
}

impl<T: Bounds<E> + Ord, E> Bounds<Each<E>> for BTreeSet<T>
where
    T::As: Ord,
{
    type As = BTreeSet<T::As>;
    fn check(&self) -> PsszResult<()> {
        self.iter().try_for_each(T::check)
    }
    fn to_as(&self) -> Self::As {
        self.iter().map(T::to_as).collect()
    }
    fn from_as(described: Self::As) -> Self {
        described.into_iter().map(T::from_as).collect()
    }
}

/// A map with a bound on its entry count alone: the entries are unchanged.
impl<K: Ord + Clone, V: Clone, const N: usize> Bounds<Max<N>> for BTreeMap<K, V> {
    type As = BoundedMap<K, V, N>;
    fn check(&self) -> PsszResult<()> {
        if self.len() > N {
            return Err(VIOLATED);
        }
        Ok(())
    }
    fn to_as(&self) -> Self::As {
        BoundedMap(self.clone())
    }
    fn from_as(described: Self::As) -> Self {
        described.0
    }
}

/// A map entry is a pair, whose header width follows the bound of its key
/// and value.
impl<K: Bounds<KS> + Ord, V: Bounds<VS>, KS, VS, const N: usize> Bounds<Max<N, Entry<KS, VS>>> for BTreeMap<K, V>
where
    K::As: Ord,
{
    type As = BoundedMap<K::As, V::As, N>;
    fn check(&self) -> PsszResult<()> {
        if self.len() > N {
            return Err(VIOLATED);
        }
        self.iter().try_for_each(|(k, v)| k.check().and_then(|()| v.check()))
    }
    fn to_as(&self) -> Self::As {
        BoundedMap(self.iter().map(|(k, v)| (k.to_as(), v.to_as())).collect())
    }
    fn from_as(described: Self::As) -> Self {
        described.0.into_iter().map(|(k, v)| (K::from_as(k), V::from_as(v))).collect()
    }
}

impl<K: Bounds<KS> + Ord, V: Bounds<VS>, KS, VS> Bounds<Each<Entry<KS, VS>>> for BTreeMap<K, V>
where
    K::As: Ord,
{
    type As = BTreeMap<K::As, V::As>;
    fn check(&self) -> PsszResult<()> {
        self.iter().try_for_each(|(k, v)| k.check().and_then(|()| v.check()))
    }
    fn to_as(&self) -> Self::As {
        self.iter().map(|(k, v)| (k.to_as(), v.to_as())).collect()
    }
    fn from_as(described: Self::As) -> Self {
        described.into_iter().map(|(k, v)| (K::from_as(k), V::from_as(v))).collect()
    }
}

// An optional is looked through: the annotation is its payload's.
macro_rules! through_option {
    ($([$($generics:tt)*] $spec:ty;)*) => {$(
        impl<T: Bounds<$spec>, $($generics)*> Bounds<$spec> for Option<T> {
            type As = Option<T::As>;
            fn check(&self) -> PsszResult<()> {
                self.as_ref().map_or(Ok(()), T::check)
            }
            fn to_as(&self) -> Self::As {
                self.as_ref().map(T::to_as)
            }
            fn from_as(described: Self::As) -> Self {
                described.map(T::from_as)
            }
        }
    )*};
}
through_option! {
    [E, const N: usize] Max<N, E>;
    [E] Each<E>;
    [X] At<X>;
    [R, X] Order<R, X>;
}

// A tuple takes its offset width from its bound.
macro_rules! tuple_bounds {
    ($(($ty:ident, $spec:ident, $index:tt)),+) => {
        impl<$($ty: Bounds<$spec>, $spec),+> Bounds<At<($($spec,)+)>> for ($($ty,)+) {
            type As = ($($ty::As,)+);
            fn check(&self) -> PsszResult<()> {
                $(self.$index.check()?;)+
                Ok(())
            }
            fn to_as(&self) -> Self::As {
                ($(self.$index.to_as(),)+)
            }
            fn from_as(described: Self::As) -> Self {
                ($($ty::from_as(described.$index),)+)
            }
        }
    };
}
tuple_bounds!((A, SA, 0));
tuple_bounds!((A, SA, 0), (B, SB, 1));
tuple_bounds!((A, SA, 0), (B, SB, 1), (C, SC, 2));
tuple_bounds!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3));
tuple_bounds!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4));
tuple_bounds!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4), (G, SG, 5));
tuple_bounds!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4), (G, SG, 5), (H, SH, 6));
tuple_bounds!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4), (G, SG, 5), (H, SH, 6), (I, SI, 7));

// ── Writing a value as the described type ───────────────────────────────────

/// Writes a value of this type under the annotation `S` with the layout of
/// the described type `As`, from the value as it stands: nothing is
/// converted. The bounds are not checked here ([`Bounds::check`] is, once,
/// by [`Described`]).
pub trait PackAs<F: PsszFormat, S>: Bounds<S> {
    /// Appends the encoding of the value as an `As`, node `depth` of the
    /// tree being written. One depth node per node of `As`, exactly as
    /// `As`'s own encoder (`PsszPack::pssz_pack_at_depth`).
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()>;
    /// When `As` is fixed-size: its encoding, into its slot
    /// (`PsszPack::pssz_pack_fixed`).
    fn pack_fixed_as(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        let mut bytes = Vec::new();
        self.pack_as(&mut bytes, depth)?;
        if bytes.len() != slot.len() {
            return Err(PsszError::new("pssz: fixed encoder size mismatch"));
        }
        slot.copy_from_slice(&bytes);
        Ok(())
    }
    /// `PsszPack::pssz_size_hint` of the value as an `As`.
    fn size_hint_as(&self) -> usize {
        0
    }
    /// The value is an absent optional.
    fn is_none_as(&self) -> bool {
        false
    }
}

impl<F: PsszFormat, T: PsszPack<F> + Clone> PackAs<F, Plain> for T {
    #[inline]
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        self.pssz_pack_at_depth(out, depth)
    }
    #[inline]
    fn pack_fixed_as(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        self.pssz_pack_fixed(slot, depth)
    }
    fn size_hint_as(&self) -> usize {
        self.pssz_size_hint()
    }
    fn is_none_as(&self) -> bool {
        self.pssz_is_none()
    }
}

#[inline]
fn raw_bytes(bytes: &[u8], out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
    checked::encode_node(depth)?;
    checked::append(out, bytes)
}
impl<F: PsszFormat, const N: usize> PackAs<F, Max<N>> for String {
    #[inline]
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        raw_bytes(self.as_bytes(), out, depth)
    }
    fn size_hint_as(&self) -> usize {
        self.len()
    }
}
impl<F: PsszFormat, const N: usize> PackAs<F, Max<N>> for RawString {
    #[inline]
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        raw_bytes(&self.0, out, depth)
    }
    fn size_hint_as(&self) -> usize {
        self.0.len()
    }
}

/// The elements of a list, array or set, at the offset width and with the
/// element layout of the described sequence `L`.
fn pack_elements<'a, F: PsszFormat, L: PsszPack<F>, T: PackAs<F, E> + 'a, E>(
    values: impl ExactSizeIterator<Item = &'a T>,
    known_count: bool,
    out: &mut Vec<u8>,
    depth: usize,
) -> PsszResult<()>
where
    T::As: PsszPack<F>,
{
    checked::encode_sequence_with_at_depth(
        <L as PsszPack<F>>::OFFSET_WIDTH,
        <T::As as PsszPack<F>>::IS_FIXED_SIZE.then_some(<T::As as PsszPack<F>>::FIXED_SIZE),
        values,
        |value: &T, out: &mut Vec<u8>, depth| value.pack_as(out, depth),
        |value: &T, slot: &mut [u8], depth| value.pack_fixed_as(slot, depth),
        out,
        known_count,
        depth,
    )
}

/// `size_hint_as` of the elements of a sequence whose described type is `L`.
fn elements_size_hint<'a, F: PsszFormat, L: PsszPack<F>, T: PackAs<F, E> + 'a, E>(
    values: impl ExactSizeIterator<Item = &'a T>,
) -> usize
where
    T::As: PsszPack<F>,
{
    let count = values.len();
    if <T::As as PsszPack<F>>::IS_FIXED_SIZE {
        count.saturating_mul(<T::As as PsszPack<F>>::FIXED_SIZE)
    } else {
        values.fold(count.saturating_mul(<L as PsszPack<F>>::OFFSET_WIDTH), |total, value| {
            total.saturating_add(value.size_hint_as())
        })
    }
}

impl<F: PsszFormat, T: PackAs<F, E>, E, const N: usize> PackAs<F, Max<N, E>> for Vec<T>
where
    T::As: PsszPack<F>,
{
    #[inline]
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        pack_elements::<F, Self::As, T, E>(self.iter(), false, out, depth)
    }
    fn size_hint_as(&self) -> usize {
        elements_size_hint::<F, Self::As, T, E>(self.iter())
    }
}
impl<F: PsszFormat, T: PackAs<F, E>, E> PackAs<F, Each<E>> for Vec<T>
where
    T::As: PsszPack<F>,
{
    #[inline]
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        pack_elements::<F, Self::As, T, E>(self.iter(), false, out, depth)
    }
    fn size_hint_as(&self) -> usize {
        elements_size_hint::<F, Self::As, T, E>(self.iter())
    }
}
impl<F: PsszFormat, T: PackAs<F, E>, E, const M: usize> PackAs<F, Each<E>> for [T; M]
where
    T::As: PsszPack<F>,
{
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        pack_elements::<F, Self::As, T, E>(self.iter(), true, out, depth)
    }
    fn size_hint_as(&self) -> usize {
        elements_size_hint::<F, Self::As, T, E>(self.iter())
    }
}

/// The elements in canonical order. A bound does not change a key encoding:
/// the order of the values is the order of the described values.
fn pack_set<F: PsszFormat, L: PsszPack<F>, T: PackAs<F, E> + KeyPack, E>(
    set: &BTreeSet<T>,
    out: &mut Vec<u8>,
    depth: usize,
) -> PsszResult<()>
where
    T::As: PsszPack<F>,
{
    checked::encode_node(depth)?;
    let sorted = canonical_order(set.iter(), |element| *element, SET_DUPLICATE)?;
    pack_elements::<F, L, T, E>(sorted.into_iter(), false, out, depth)
}
impl<F: PsszFormat, T: PackAs<F, E> + KeyPack + Ord, E, const N: usize> PackAs<F, Max<N, E>> for BTreeSet<T>
where
    T::As: PsszPack<F> + KeyPack + Ord,
{
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        pack_set::<F, Self::As, T, E>(self, out, depth)
    }
}
impl<F: PsszFormat, T: PackAs<F, E> + KeyPack + Ord, E> PackAs<F, Each<E>> for BTreeSet<T>
where
    T::As: PsszPack<F> + KeyPack + Ord,
{
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        pack_set::<F, Self::As, T, E>(self, out, depth)
    }
}

impl<F: PsszFormat, K: PsszPack<F> + KeyPack + Ord + Clone, V: PsszPack<F> + Clone, const N: usize> PackAs<F, Max<N>>
    for BTreeMap<K, V>
{
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        encode_map::<F, K, V>(<Self::As as PsszPack<F>>::OFFSET_WIDTH, self.iter(), usize::MAX, out, depth)
    }
}
/// The entries in canonical order, each the pair of the described key and
/// the described value, in the offset table of the described map `L`.
fn pack_map<F: PsszFormat, L: PsszPack<F>, K, V, KS, VS>(map: &BTreeMap<K, V>, out: &mut Vec<u8>, depth: usize) -> PsszResult<()>
where
    K: PackAs<F, KS> + KeyPack,
    V: PackAs<F, VS>,
    K::As: PsszPack<F>,
    V::As: PsszPack<F>,
{
    checked::encode_node(depth)?;
    let sorted = canonical_order(map.iter(), |entry| entry.0, MAP_DUPLICATE)?;
    checked::encode_sequence_with_at_depth(
        <L as PsszPack<F>>::OFFSET_WIDTH,
        None,
        sorted.into_iter(),
        |(key, value): (&K, &V), out: &mut Vec<u8>, depth| {
            <(Packed<K, KS>, Packed<V, VS>) as PsszPack<F>>::pssz_pack_at_depth(&(Packed::new(key), Packed::new(value)), out, depth)
        },
        // An entry is a tuple: never fixed-size.
        |_: (&K, &V), _: &mut [u8], _| Err(PsszError::new("pssz: fixed encoder size mismatch")),
        out,
        false,
        depth,
    )
}
impl<F: PsszFormat, K, V, KS, VS, const N: usize> PackAs<F, Max<N, Entry<KS, VS>>> for BTreeMap<K, V>
where
    K: PackAs<F, KS> + KeyPack + Ord,
    V: PackAs<F, VS>,
    K::As: PsszPack<F> + KeyPack + Ord,
    V::As: PsszPack<F>,
{
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        pack_map::<F, Self::As, K, V, KS, VS>(self, out, depth)
    }
}
impl<F: PsszFormat, K, V, KS, VS> PackAs<F, Each<Entry<KS, VS>>> for BTreeMap<K, V>
where
    K: PackAs<F, KS> + KeyPack + Ord,
    V: PackAs<F, VS>,
    K::As: PsszPack<F> + KeyPack + Ord,
    V::As: PsszPack<F>,
{
    fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        pack_map::<F, Self::As, K, V, KS, VS>(self, out, depth)
    }
}

macro_rules! pack_through_option {
    ($([$($generics:tt)*] $spec:ty;)*) => {$(
        impl<F: PsszFormat, T: PackAs<F, $spec>, $($generics)*> PackAs<F, $spec> for Option<T>
        where
            T::As: PsszPack<F>,
        {
            #[inline]
            fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
                <Option<Packed<T, $spec>> as PsszPack<F>>::pssz_pack_at_depth(&self.as_ref().map(Packed::new), out, depth)
            }
            fn size_hint_as(&self) -> usize {
                <Option<Packed<T, $spec>> as PsszPack<F>>::pssz_size_hint(&self.as_ref().map(Packed::new))
            }
            fn is_none_as(&self) -> bool {
                self.is_none()
            }
        }
    )*};
}
pack_through_option! {
    [E, const N: usize] Max<N, E>;
    [E] Each<E>;
    [X] At<X>;
    [R, X] Order<R, X>;
}

// An ordered list is written as the list its own annotation describes: the
// order was checked with the bounds, once ([`Described`]).
macro_rules! ordered_pack {
    ($([$($generics:tt)*] $list:ty, $item:ty;)*) => {$(
        impl<F: PsszFormat, T, R, S, $($generics)*> PackAs<F, Order<R, S>> for $list
        where
            $list: PackAs<F, S>,
            R: Rule<$item>,
        {
            #[inline]
            fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
                <$list as PackAs<F, S>>::pack_as(self, out, depth)
            }
            fn pack_fixed_as(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
                <$list as PackAs<F, S>>::pack_fixed_as(self, slot, depth)
            }
            fn size_hint_as(&self) -> usize {
                <$list as PackAs<F, S>>::size_hint_as(self)
            }
        }
    )*};
}
ordered_pack! {
    [] Vec<T>, T;
    [const M: usize] [T; M], T;
}

macro_rules! tuple_pack {
    ($(($ty:ident, $spec:ident, $index:tt)),+) => {
        impl<F: PsszFormat, $($ty: PackAs<F, $spec>, $spec),+> PackAs<F, At<($($spec,)+)>> for ($($ty,)+)
        where
            $($ty::As: PsszPack<F>,)+
        {
            fn pack_as(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
                <($(Packed<$ty, $spec>,)+) as PsszPack<F>>::pssz_pack_at_depth(&($(Packed::new(&self.$index),)+), out, depth)
            }
            fn size_hint_as(&self) -> usize {
                <($(Packed<$ty, $spec>,)+) as PsszPack<F>>::pssz_size_hint(&($(Packed::new(&self.$index),)+))
            }
        }
    };
}
tuple_pack!((A, SA, 0));
tuple_pack!((A, SA, 0), (B, SB, 1));
tuple_pack!((A, SA, 0), (B, SB, 1), (C, SC, 2));
tuple_pack!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3));
tuple_pack!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4));
tuple_pack!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4), (G, SG, 5));
tuple_pack!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4), (G, SG, 5), (H, SH, 6));
tuple_pack!((A, SA, 0), (B, SB, 1), (C, SC, 2), (D, SD, 3), (E, SE, 4), (G, SG, 5), (H, SH, 6), (I, SI, 7));

/// A borrowed value that encodes as the described type: its constants, its
/// layout. `CHECK` says whether the encoder checks the annotation's bounds
/// first: [`Described`] does, once for the whole value; the values beneath
/// it are [`Packed`] and are not checked again.
#[doc(hidden)]
pub struct Carried<'a, T, S, const CHECK: bool>(&'a T, PhantomData<S>);
/// A field under its annotation, as the derives write it.
#[doc(hidden)]
pub type Described<'a, T, S> = Carried<'a, T, S, true>;
type Packed<'a, T, S> = Carried<'a, T, S, false>;

impl<'a, T, S, const CHECK: bool> Carried<'a, T, S, CHECK> {
    pub fn new(value: &'a T) -> Self {
        Self(value, PhantomData)
    }
}

impl<F: PsszFormat, T, S, const CHECK: bool> PsszPack<F> for Carried<'_, T, S, CHECK>
where
    T: PackAs<F, S>,
    T::As: PsszPack<F>,
{
    const IS_OPTIONAL: bool = <T::As as PsszPack<F>>::IS_OPTIONAL;
    const OPTIONAL_SELECTOR: bool = <T::As as PsszPack<F>>::OPTIONAL_SELECTOR;
    const MIN_ENCODED_SIZE: usize = <T::As as PsszPack<F>>::MIN_ENCODED_SIZE;
    const MAX_ENCODED_SIZE: Option<usize> = <T::As as PsszPack<F>>::MAX_ENCODED_SIZE;
    const IS_FIXED_SIZE: bool = <T::As as PsszPack<F>>::IS_FIXED_SIZE;
    const FIXED_SIZE: usize = <T::As as PsszPack<F>>::FIXED_SIZE;
    const MAX_DYNAMIC_DATA: Option<usize> = <T::As as PsszPack<F>>::MAX_DYNAMIC_DATA;
    const AUTO_BOUND: Option<usize> = <T::As as PsszPack<F>>::AUTO_BOUND;
    const OFFSET_WIDTH: usize = <T::As as PsszPack<F>>::OFFSET_WIDTH;
    const RECORD_WIDTH_BENEATH: usize = <T::As as PsszPack<F>>::RECORD_WIDTH_BENEATH;
    const SHAPE: Shape = <T::As as PsszPack<F>>::SHAPE;

    fn pssz_is_none(&self) -> bool {
        self.0.is_none_as()
    }
    fn pssz_none_bytes() -> Option<&'static [u8]> {
        <T::As as PsszPack<F>>::pssz_none_bytes()
    }
    fn pssz_size(&self) -> usize {
        let mut out = Vec::new();
        self.0.pack_as(&mut out, checked::encode_depth()).expect("pssz: a member has no encoding; use try_to_pssz");
        out.len()
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        self.pssz_pack_checked(out).expect("pssz: a member is outside its annotated bound; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        checked::pack_checked::<F, Self>(self, out)
    }
    #[inline]
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        if CHECK {
            self.0.check()?;
        }
        self.0.pack_as(out, depth)
    }
    #[inline]
    fn pssz_pack_fixed(&self, slot: &mut [u8], depth: usize) -> PsszResult<()> {
        if CHECK {
            self.0.check()?;
        }
        self.0.pack_fixed_as(slot, depth)
    }
    fn pssz_size_hint(&self) -> usize {
        self.0.size_hint_as()
    }
}

/// The message of a violated annotation, for the text and key codecs.
#[doc(hidden)]
pub fn check<T: Bounds<S>, S>(value: &T) -> Result<(), &'static str> {
    value.check().map_err(|_| "member length_bound violated")
}
