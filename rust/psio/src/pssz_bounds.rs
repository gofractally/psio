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
//! value is written from the field as it stands when the described type
//! has the same bytes (a list, a string, a set: only the checks differ);
//! where a bound narrows an offset width below the member (a tuple, a map
//! entry, an array of variable elements) the value is converted to the
//! described type first, which copies it.

use crate::pssz::{
    BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, Pssz, PsszError, PsszFormat, PsszPack,
    PsszResult, RawString, Shape,
};
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

const VIOLATED: PsszError = PsszError::new("pssz: member length_bound violated");

/// A value of this type under the annotation `S`.
pub trait Bounds<S>: Sized {
    /// The type the annotation describes.
    type As;
    /// Whether every value has the same pSSZ bytes as `As`, in every
    /// profile (only the checks differ).
    const SAME_BYTES: bool;
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
    const SAME_BYTES: bool = true;
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
    const SAME_BYTES: bool = true;
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
    const SAME_BYTES: bool = true;
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
    const SAME_BYTES: bool = T::SAME_BYTES;
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
    const SAME_BYTES: bool = T::SAME_BYTES;
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

/// An array of variable-size elements takes its offset width from its
/// bound, so only an array of fixed-size elements keeps its bytes.
impl<T: Bounds<E> + PsszPack<Pssz>, E, const M: usize> Bounds<Each<E>> for [T; M] {
    type As = [T::As; M];
    const SAME_BYTES: bool = T::SAME_BYTES && <T as PsszPack<Pssz>>::IS_FIXED_SIZE;
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
    const SAME_BYTES: bool = T::SAME_BYTES;
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
    const SAME_BYTES: bool = T::SAME_BYTES;
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
    const SAME_BYTES: bool = true;
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
/// and value: the bytes are the described type's.
impl<K: Bounds<KS> + Ord, V: Bounds<VS>, KS, VS, const N: usize> Bounds<Max<N, Entry<KS, VS>>> for BTreeMap<K, V>
where
    K::As: Ord,
{
    type As = BoundedMap<K::As, V::As, N>;
    const SAME_BYTES: bool = false;
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
    const SAME_BYTES: bool = false;
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
            const SAME_BYTES: bool = T::SAME_BYTES;
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
}

// A tuple takes its offset width from its bound: the bytes are the
// described tuple's.
macro_rules! tuple_bounds {
    ($(($ty:ident, $spec:ident, $index:tt)),+) => {
        impl<$($ty: Bounds<$spec>, $spec),+> Bounds<At<($($spec,)+)>> for ($($ty,)+) {
            type As = ($($ty::As,)+);
            const SAME_BYTES: bool = false;
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

/// A field under its annotation, as the derives write it: the constants of
/// the described type, the field's own bytes where they are the same.
#[doc(hidden)]
pub struct Described<'a, T, S>(&'a T, PhantomData<S>);

impl<'a, T, S> Described<'a, T, S> {
    pub fn new(value: &'a T) -> Self {
        Self(value, PhantomData)
    }
}

impl<F: PsszFormat, T, S> PsszPack<F> for Described<'_, T, S>
where
    T: Bounds<S> + PsszPack<F>,
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
    const SHAPE: Shape = <T::As as PsszPack<F>>::SHAPE;

    fn pssz_is_none(&self) -> bool {
        self.0.pssz_is_none()
    }
    fn pssz_none_bytes() -> Option<&'static [u8]> {
        <T::As as PsszPack<F>>::pssz_none_bytes()
    }
    fn pssz_size(&self) -> usize {
        if T::SAME_BYTES {
            self.0.pssz_size()
        } else {
            self.0.to_as().pssz_size()
        }
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        self.pssz_pack_checked(out).expect("pssz: a member is outside its annotated bound; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        if T::SAME_BYTES {
            self.0.check()?;
            self.0.pssz_pack_checked(out)
        } else {
            // The described type's encoder checks its own bounds; the
            // field's check gives the annotation's message first.
            self.0.check()?;
            self.0.to_as().pssz_pack_checked(out)
        }
    }
}

/// The message of a violated annotation, for the text and key codecs.
#[doc(hidden)]
pub fn check<T: Bounds<S>, S>(value: &T) -> Result<(), &'static str> {
    value.check().map_err(|_| "member length_bound violated")
}
