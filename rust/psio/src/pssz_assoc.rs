//! Maps and sets (docs/pssz-spec.md 5.9): `BTreeMap`, `HashMap`, `BTreeSet`,
//! `HashSet`, and the bounded forms [`BoundedMap`] / [`BoundedSet`] (C++
//! `length_bound{.max = N}` on a map / set member; the bound counts entries).
//!
//! They add no wire shape: a map is `Vec<(K, V)>` and a set is `Vec<K>` with
//! the entries in **strictly ascending order of the `key` encoding of K**
//! (`psio::key`, docs/key-spec.md). So `K: KeyPack`, and:
//!
//! * encoders write that order whatever order the container iterates in, and
//!   refuse two entries with equal key bytes;
//! * every reader validates the list and then requires the order;
//! * the order is computed without running invariants: each codec checks the
//!   keys where it writes or reads them as values.
//!
//! The container is not part of the type: `BTreeMap<K, V>` and
//! `HashMap<K, V>` are one value kind with one encoding. A key type with no
//! `Ord` / `Hash` of its own (a float), or whose `Ord` is not the key order,
//! goes in a container as [`ByKey<K>`](crate::key::ByKey).
use super::*;
use crate::key::{key_bytes, KeyPack};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::hash::Hash;

/// A map of at most `N` entries.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BoundedMap<K, V, const N: usize>(pub BTreeMap<K, V>);
/// A set of at most `N` elements.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BoundedSet<K, const N: usize>(pub BTreeSet<K>);
impl<K, V, const N: usize> Default for BoundedMap<K, V, N> {
    fn default() -> Self {
        Self(BTreeMap::new())
    }
}
impl<K, const N: usize> Default for BoundedSet<K, N> {
    fn default() -> Self {
        Self(BTreeSet::new())
    }
}

pub(crate) const MAP_ORDER: PsszError =
    PsszError::new("pssz: map entries are not in strictly ascending key order");
pub(crate) const SET_ORDER: PsszError =
    PsszError::new("pssz: set elements are not in strictly ascending order");
pub(crate) const MAP_DUPLICATE: PsszError = PsszError::new("pssz: map has duplicate keys");
pub(crate) const SET_DUPLICATE: PsszError = PsszError::new("pssz: set has duplicate elements");
const MAP_BOUND: PsszError = PsszError::new("pssz: map exceeds its bound");
const SET_BOUND: PsszError = PsszError::new("pssz: set exceeds its bound");

/// The items in canonical order: ascending key bytes of `key_of(item)`;
/// `duplicate` when two keys have equal bytes.
pub(crate) fn canonical_order<I, K: KeyPack>(
    items: impl Iterator<Item = I>,
    key_of: impl Fn(&I) -> &K,
    duplicate: PsszError,
) -> PsszResult<Vec<I>> {
    let mut keyed: Vec<(Vec<u8>, I)> = items.map(|item| (key_bytes(key_of(&item)), item)).collect();
    keyed.sort_by(|a, b| a.0.cmp(&b.0));
    if keyed.windows(2).any(|pair| pair[0].0 == pair[1].0) {
        return Err(duplicate);
    }
    Ok(keyed.into_iter().map(|(_, item)| item).collect())
}
fn ascending<K: KeyPack>(keys: impl Iterator<Item = K>, error: PsszError) -> PsszResult<()> {
    let mut last: Option<Vec<u8>> = None;
    for key in keys {
        let bytes = key_bytes(&key);
        if last.as_ref().is_some_and(|last| *last >= bytes) {
            return Err(error);
        }
        last = Some(bytes);
    }
    Ok(())
}

fn encode_map<'a, F: PsszFormat, K, V>(
    entries: impl Iterator<Item = (&'a K, &'a V)>,
    bound: usize,
    out: &mut Vec<u8>,
) -> PsszResult<()>
where
    K: PsszPack<F> + KeyPack + 'a,
    V: PsszPack<F> + 'a,
{
    let _depth = checked::EncodeDepth::enter()?;
    let sorted = canonical_order(entries, |entry| entry.0, MAP_DUPLICATE)?;
    if sorted.len() > bound {
        return Err(MAP_BOUND);
    }
    checked::encode_sequence::<F, (&K, &V)>(<Vec<(K, V)> as PsszPack<F>>::OFFSET_WIDTH, &sorted, out, false)
}
fn encode_set<'a, F: PsszFormat, K>(
    elements: impl Iterator<Item = &'a K>,
    bound: usize,
    out: &mut Vec<u8>,
) -> PsszResult<()>
where
    K: PsszPack<F> + KeyPack + 'a,
{
    let _depth = checked::EncodeDepth::enter()?;
    let sorted = canonical_order(elements, |element| *element, SET_DUPLICATE)?;
    if sorted.len() > bound {
        return Err(SET_BOUND);
    }
    checked::encode_sequence::<F, &K>(<Vec<K> as PsszPack<F>>::OFFSET_WIDTH, &sorted, out, false)
}
/// The entries of a map frame: the list is decoded (which runs the
/// invariants of the keys and values, in wire order), then the order is
/// required.
fn decode_map<F: PsszFormat, K, V>(bytes: &[u8], bound: usize) -> PsszResult<Vec<(K, V)>>
where
    K: PsszPack<F> + PsszUnpack<F> + KeyPack,
    V: PsszPack<F> + PsszUnpack<F>,
{
    let _depth = checked::DecodeDepth::enter()?;
    let w = <Vec<(K, V)> as PsszPack<F>>::OFFSET_WIDTH;
    if checked::sequence_len::<F, (K, V)>(w, bytes, None)? > bound {
        return Err(MAP_BOUND);
    }
    let entries = checked::decode_sequence::<F, (K, V)>(w, bytes, None)?;
    ascending(entries.iter().map(|entry| &entry.0), MAP_ORDER)?;
    Ok(entries)
}
fn decode_set<F: PsszFormat, K>(bytes: &[u8], bound: usize) -> PsszResult<Vec<K>>
where
    K: PsszPack<F> + PsszUnpack<F> + KeyPack,
{
    let _depth = checked::DecodeDepth::enter()?;
    let w = <Vec<K> as PsszPack<F>>::OFFSET_WIDTH;
    if checked::sequence_len::<F, K>(w, bytes, None)? > bound {
        return Err(SET_BOUND);
    }
    let elements = checked::decode_sequence::<F, K>(w, bytes, None)?;
    ascending(elements.iter(), SET_ORDER)?;
    Ok(elements)
}
/// Validation without keeping the value: the list, then the order (which
/// needs the keys, and only the keys, as values).
fn validate_map<F: PsszFormat, K, V>(bytes: &[u8], bound: usize) -> PsszResult<()>
where
    K: PsszPack<F> + PsszUnpack<F> + KeyPack,
    V: PsszPack<F> + PsszValidate<F>,
    (K, V): PsszPack<F> + PsszValidate<F>,
{
    let _depth = checked::DecodeDepth::enter()?;
    let w = <Vec<(K, V)> as PsszPack<F>>::OFFSET_WIDTH;
    let n = checked::sequence_len::<F, (K, V)>(w, bytes, None)?;
    if n > bound {
        return Err(MAP_BOUND);
    }
    for i in 0..n {
        <(K, V)>::pssz_validate(&bytes[checked::sequence_span::<F, (K, V)>(w, bytes, n, i)?])?;
    }
    let mut keys = Vec::new();
    for i in 0..n {
        let entry = &bytes[checked::sequence_span::<F, (K, V)>(w, bytes, n, i)?];
        let [key, _] = checked::record_spans_n(
            <(K, V) as PsszPack<F>>::OFFSET_WIDTH,
            [checked::Layout::of::<F, K>(), checked::Layout::of::<F, V>()],
            entry,
            false,
        )?;
        keys.push(checked::decode_field::<F, K>(entry, key)?);
    }
    ascending(keys.iter(), MAP_ORDER)
}
fn validate_set<F: PsszFormat, K>(bytes: &[u8], bound: usize) -> PsszResult<()>
where
    K: PsszPack<F> + PsszUnpack<F> + KeyPack,
{
    // An element is its own key: decoding it is its validation.
    decode_set::<F, K>(bytes, bound).map(drop)
}

macro_rules! assoc_consts {
    ($kind:ident, $list:ty, $bound:expr) => {
        const SHAPE: crate::pssz::Shape = crate::pssz::Shape::node(
            crate::pssz::ShapeKind::$kind,
            match $bound {
                Some(n) => n,
                None => usize::MAX,
            },
            <Self as PsszPack<F>>::OFFSET_WIDTH,
            false,
            &[<$list as PsszPack<F>>::SHAPE],
        );
        const MIN_ENCODED_SIZE: usize = 0;
        const MAX_ENCODED_SIZE: Option<usize> = <Self as PsszPack<F>>::AUTO_BOUND;
        const IS_FIXED_SIZE: bool = false;
        const FIXED_SIZE: usize = 0;
        const AUTO_BOUND: Option<usize> = match $bound {
            Some(n) => checked::sequence_bound(n, <$list as PsszPack<F>>::AUTO_BOUND, <$list as PsszPack<F>>::IS_FIXED_SIZE),
            None => None,
        };
        // A list's offset table: the Vec's width, bounded or not.
        const OFFSET_WIDTH: usize = <Vec<$list> as PsszPack<F>>::OFFSET_WIDTH;
        fn pssz_size(&self) -> usize {
            let mut out = Vec::new();
            <Self as PsszPack<F>>::pssz_pack_checked(self, &mut out)
                .expect("pssz: map / set encode failed; use try_to_pssz");
            out.len()
        }
        fn pssz_pack(&self, out: &mut Vec<u8>) {
            <Self as PsszPack<F>>::pssz_pack_checked(self, out)
                .expect("pssz: map / set encode failed; use try_to_pssz");
        }
    };
}

macro_rules! pssz_map {
    ([$($kb:tt)*], [$($cg:tt)*], $ty:ty, $bound:expr, |$s:ident| $inner:expr, |$items:ident| $build:expr) => {
        impl<F: PsszFormat, K: PsszPack<F> + KeyPack + $($kb)*, V: PsszPack<F>, $($cg)*> PsszPack<F> for $ty {
            assoc_consts!(Map, (K, V), $bound);
            fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
                let $s = self;
                let bound: Option<usize> = $bound;
                encode_map::<F, K, V>($inner.iter(), bound.unwrap_or(usize::MAX), out)
            }
        }
        impl<F: PsszFormat, K, V, $($cg)*> PsszUnpack<F> for $ty
        where
            K: PsszPack<F> + PsszUnpack<F> + KeyPack + $($kb)*,
            V: PsszPack<F> + PsszUnpack<F>,
        {
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                let bound: Option<usize> = $bound;
                let $items = decode_map::<F, K, V>(bytes, bound.unwrap_or(usize::MAX))?;
                let count = $items.len();
                let out: Self = $build;
                let $s = &out;
                if $inner.len() != count {
                    return Err(PsszError::new("pssz: map container merged distinct keys"));
                }
                Ok(out)
            }
        }
        impl<F: PsszFormat, K, V, $($cg)*> PsszValidate<F> for $ty
        where
            K: PsszPack<F> + PsszUnpack<F> + PsszValidate<F> + KeyPack + $($kb)*,
            V: PsszPack<F> + PsszValidate<F>,
        {
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                let bound: Option<usize> = $bound;
                validate_map::<F, K, V>(bytes, bound.unwrap_or(usize::MAX))
            }
        }
        impl<K, V, $($cg)*> crate::pssz_schema_ir::ToPsszSchema for $ty
        where
            K: crate::pssz_schema_ir::ToPsszSchema + $($kb)*,
            V: crate::pssz_schema_ir::ToPsszSchema,
        {
            fn build_schema(
                builder: &mut crate::pssz_schema_ir::SchemaBuilder,
            ) -> PsszResult<crate::pssz_schema_ir::AnyType> {
                let entry = <(K, V) as crate::pssz_schema_ir::ToPsszSchema>::schema(builder)?;
                Ok(assoc_schema("map", entry, $bound))
            }
        }
    };
}
macro_rules! pssz_set {
    ([$($kb:tt)*], [$($cg:tt)*], $ty:ty, $bound:expr, |$s:ident| $inner:expr, |$items:ident| $build:expr) => {
        impl<F: PsszFormat, K: PsszPack<F> + KeyPack + $($kb)*, $($cg)*> PsszPack<F> for $ty {
            assoc_consts!(Set, K, $bound);
            fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
                let $s = self;
                let bound: Option<usize> = $bound;
                encode_set::<F, K>($inner.iter(), bound.unwrap_or(usize::MAX), out)
            }
        }
        impl<F: PsszFormat, K, $($cg)*> PsszUnpack<F> for $ty
        where
            K: PsszPack<F> + PsszUnpack<F> + KeyPack + $($kb)*,
        {
            fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
                let bound: Option<usize> = $bound;
                let $items = decode_set::<F, K>(bytes, bound.unwrap_or(usize::MAX))?;
                let count = $items.len();
                let out: Self = $build;
                let $s = &out;
                if $inner.len() != count {
                    return Err(PsszError::new("pssz: set container merged distinct elements"));
                }
                Ok(out)
            }
        }
        impl<F: PsszFormat, K, $($cg)*> PsszValidate<F> for $ty
        where
            K: PsszPack<F> + PsszUnpack<F> + KeyPack + $($kb)*,
        {
            fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
                let bound: Option<usize> = $bound;
                validate_set::<F, K>(bytes, bound.unwrap_or(usize::MAX))
            }
        }
        impl<K, $($cg)*> crate::pssz_schema_ir::ToPsszSchema for $ty
        where
            K: crate::pssz_schema_ir::ToPsszSchema + $($kb)*,
        {
            fn build_schema(
                builder: &mut crate::pssz_schema_ir::SchemaBuilder,
            ) -> PsszResult<crate::pssz_schema_ir::AnyType> {
                let element = <K as crate::pssz_schema_ir::ToPsszSchema>::schema(builder)?;
                Ok(assoc_schema("set", element, $bound))
            }
        }
    };
}

/// `Custom{ List{element}, id }`, or `BoundedList` under a maximum count.
fn assoc_schema(
    id: &str,
    element: crate::pssz_schema_ir::AnyType,
    bound: Option<usize>,
) -> crate::pssz_schema_ir::AnyType {
    use crate::pssz_schema_ir as ir;
    let list = match bound {
        Some(max) => ir::AnyValue::BoundedList(ir::BoundedList {
            r#type: Box::new(element),
            maxCount: max as u64,
        }),
        None => ir::AnyValue::List(ir::List { r#type: Box::new(element) }),
    };
    ir::AnyType::new(ir::AnyValue::Custom(ir::Custom {
        r#type: Box::new(ir::AnyType::new(list)),
        id: id.into(),
    }))
}

pssz_map!([Ord], [], BTreeMap<K, V>, None, |s| s, |items| items.into_iter().collect());
pssz_map!([Eq + Hash], [], HashMap<K, V>, None, |s| s, |items| items.into_iter().collect());
pssz_map!([Ord], [const N: usize], BoundedMap<K, V, N>, Some(N), |s| s.0,
          |items| BoundedMap(items.into_iter().collect()));
pssz_set!([Ord], [], BTreeSet<K>, None, |s| s, |items| items.into_iter().collect());
pssz_set!([Eq + Hash], [], HashSet<K>, None, |s| s, |items| items.into_iter().collect());
pssz_set!([Ord], [const N: usize], BoundedSet<K, N>, Some(N), |s| s.0,
          |items| BoundedSet(items.into_iter().collect()));

// ── ByKey<T>: T in every format ─────────────────────────────────────────────

use crate::key::ByKey;
impl<F: PsszFormat, T: PsszPack<F>> PsszPack<F> for ByKey<T> {
    pssz_forward_consts!(T);
    fn pssz_is_none(&self) -> bool { <T as PsszPack<F>>::pssz_is_none(&self.0) }
    fn pssz_size(&self) -> usize { <T as PsszPack<F>>::pssz_size(&self.0) }
    fn pssz_pack(&self, out: &mut Vec<u8>) { <T as PsszPack<F>>::pssz_pack(&self.0, out) }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        <T as PsszPack<F>>::pssz_pack_checked(&self.0, out)
    }
}
impl<F: PsszFormat, T: PsszUnpack<F>> PsszUnpack<F> for ByKey<T> {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> { T::pssz_unpack(bytes).map(ByKey) }
    fn pssz_missing_optional() -> PsszResult<Self> { T::pssz_missing_optional().map(ByKey) }
}
impl<F: PsszFormat, T: PsszValidate<F>> PsszValidate<F> for ByKey<T> {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> { T::pssz_validate(bytes) }
}
impl<T: crate::pssz_schema_ir::ToPsszSchema> crate::pssz_schema_ir::ToPsszSchema for ByKey<T> {
    fn schema_name() -> Option<String> { T::schema_name() }
    fn custom_id() -> Option<String> { T::custom_id() }
    fn member_attributes() -> Vec<crate::pssz_schema_ir::Attribute> { T::member_attributes() }
    fn build_schema(
        builder: &mut crate::pssz_schema_ir::SchemaBuilder,
    ) -> PsszResult<crate::pssz_schema_ir::AnyType> {
        T::build_schema(builder)
    }
    fn schema(
        builder: &mut crate::pssz_schema_ir::SchemaBuilder,
    ) -> PsszResult<crate::pssz_schema_ir::AnyType> {
        T::schema(builder)
    }
}
