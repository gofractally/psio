//! Compile-time PSSZ schema-evolution check: the Rust equivalent of C++
//! `psio::pssz_evolves_from<Old, New>`.
//!
//! ```ignore
//! const _: () = assert!(psio::pssz::pssz_evolves_from::<AccountV1, AccountV2>());
//! ```
//!
//! `New` evolves from `Old` iff the new reader reads every value the old
//! writer can produce, and the old reader reads every new value that is also
//! a value of the old type, under the automatic (type-derived width) profile
//! (docs/pssz-spec.md 7.5, 7.6; docs/pssz-optional-evolution.md):
//!   1. *Fields.* New's fields are Old's fields, in order, followed only by
//!      `Option` fields; fixed-layout (DWNC) status is unchanged and a DWNC
//!      record gains no fields.
//!   2. *Widths.* The record's offset width is unchanged, and so is the
//!      width of every list, map, set, array and tuple at and below every
//!      member. An optional field may not be added if it would change the
//!      type's width: an explicit `max_dynamic_data` always rules, and
//!      without one the width falls back to the bound inferred from the
//!      fields, which an appended field can move. Reserve room with an
//!      explicit cap to evolve.
//!   3. *Length bounds.* A bound may only **widen**: at the member and at
//!      every position below it New's maximum is at least Old's (no bound
//!      is the widest). Tightening one keeps the bytes of old values and
//!      makes the new reader reject the old records outside it.
//!   4. *UTF-8* is the same on both sides: `String` and `RawString` do not
//!      evolve into each other.
//!   5. *`sorted` / `unique` / `unique_keys`* are the same on both sides.
//!   6. *`max_dynamic_data`.* New's cap, if it has one, is at least Old's —
//!      or, when Old has none, at least Old's inferred bound. Dropping a
//!      cap is allowed when rule 2 still holds.
//!   7. *`max_fields`.* If Old has one, New has no more fields than it
//!      allows and declares a `max_fields` that is not larger.
//! The relation is deliberately one-directional for bounds and caps:
//! `pssz_evolves_from::<Old, New>()` does not imply
//! `pssz_evolves_from::<New, Old>()`. A bound is the same whether it is
//! written as an annotation (`#[psio(max = N)]`) or as a wrapper type
//! (`BoundedList<T, N>`): the two are one type in Rust.
//! A prefix field may itself have evolved (a nested record, or a vector /
//! array / optional / variant / tuple of one) under the same rules.
//!
//! A variant (an enum with data) may gain alternatives **at the end**
//! (docs/pssz-spec.md 7.5): New's alternatives are Old's, in order, each
//! evolved by these rules, followed by any number of new ones. A variant
//! writes no offsets, so it has no width to compare; a new alternative that
//! is larger than the others raises the variant's bound, and the change is
//! refused when that moves the width of anything that holds the variant
//! (rule 2), exactly as for an appended field. Under a cap nothing moves.
//!
//! Maps, sets and nested values (docs/pssz-spec.md 5.9.4, 5.8, 7.5):
//!   - a map evolves when its value type does: the entry is a 2-tuple whose
//!     key type is the same and whose value type evolves. The key type
//!     cannot change (it defines the entry order);
//!   - a set evolves only from a set of the same element type;
//!   - `Nested<A>` evolves into `Nested<B>` exactly when `A` evolves into `B`
//!     as a top-level type. The validation mode (`Checked` / `Deferred`) is
//!     not part of the wire and may differ;
//!   - a map is not its list of entries, a set is not its list of elements
//!     and a nested value is not a byte list: none evolves from or into the
//!     other, although the bytes agree;
//!   - the maximum of a `BoundedMap` / `BoundedSet` and the byte bound of a
//!     `Nested<T, V, N>` are bounds like that of a `BoundedList`: they may
//!     widen (rule 3), not tighten.
//! `BTreeMap`, `HashMap` and `BoundedMap` are all maps (likewise the sets):
//! the container is not on the wire.
//!
//! The check is a `const fn` over [`Shape`] descriptors that every
//! `PsszPack` implementation carries (`PsszPack::SHAPE`). Hand-written
//! `PsszPack` impls default to an opaque shape, which evolves from nothing.
//! Directly recursive types cannot have a finite const descriptor; use the
//! schema-based `pssz::upgrade::validate_optional_upgrade` for those.
use crate::pssz::{Pssz, PsszPack};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShapeKind {
    /// Unknown layout (a hand-written codec).
    Opaque,
    /// Scalar, string, bit type or C-like enum, identified by name + param.
    Leaf,
    Optional,
    List,
    Array,
    Variant,
    Record,
    Tuple,
    /// Child: the entry 2-tuple `(K, V)`. `param`: the maximum entry count.
    Map,
    /// Child: the element. `param`: the maximum element count.
    Set,
    /// Child: the inner type under the automatic profile. `param`: the
    /// maximum byte count.
    Nested,
}

#[derive(Clone, Copy, Debug)]
pub struct Shape {
    pub kind: ShapeKind,
    pub name: &'static str,
    /// Array length, bound, or leaf size.
    pub param: usize,
    /// The type's own automatic offset width.
    pub width: usize,
    pub fixed: bool,
    /// A plain `Option` (may be an appended field).
    pub optional: bool,
    /// For an optional: selector form.
    pub selector: bool,
    pub dwnc: bool,
    /// A record's explicit `max_dynamic_data`.
    pub cap: Option<usize>,
    /// A record's inferred bound (`PsszPack::INFERRED_BOUND`): what a cap
    /// declared later must admit.
    pub bound: Option<usize>,
    /// A record's `max_fields`.
    pub max_fields: Option<usize>,
    /// The order constraint of a list or an array (`sorted`, `unique`,
    /// `unique_keys`): the bits of [`Shape::SORTED`] and its neighbours.
    pub order: u8,
    pub children: &'static [Shape],
}

impl Shape {
    pub const OPAQUE: Shape = Shape {
        kind: ShapeKind::Opaque,
        name: "",
        param: 0,
        width: 4,
        fixed: false,
        optional: false,
        selector: false,
        dwnc: false,
        cap: None,
        bound: None,
        max_fields: None,
        order: 0,
        children: &[],
    };
    /// `sorted`: the elements are in non-descending key order.
    pub const SORTED: u8 = 1;
    /// `unique`: strictly ascending (with [`Shape::SORTED`]).
    pub const STRICT: u8 = 2;
    /// `unique_keys`: no two elements have the same key.
    pub const UNIQUE_KEYS: u8 = 4;
    /// A record's cap, inferred bound and field-count cap.
    pub const fn limits(self, cap: Option<usize>, bound: Option<usize>, max_fields: Option<usize>) -> Shape {
        Shape { cap, bound, max_fields, ..self }
    }
    /// A list or an array under an order constraint.
    pub const fn ordered(self, order: u8) -> Shape {
        Shape { order, ..self }
    }
    pub const fn leaf(name: &'static str, param: usize, fixed: bool) -> Shape {
        Shape { kind: ShapeKind::Leaf, name, param, fixed, ..Shape::OPAQUE }
    }
    pub const fn node(
        kind: ShapeKind,
        param: usize,
        width: usize,
        fixed: bool,
        children: &'static [Shape],
    ) -> Shape {
        Shape { kind, param, width, fixed, children, ..Shape::OPAQUE }
    }
    pub const fn optional(selector: bool, inner: &'static [Shape]) -> Shape {
        Shape {
            kind: ShapeKind::Optional,
            optional: true,
            selector,
            children: inner,
            ..Shape::OPAQUE
        }
    }
    pub const fn record(
        tuple: bool,
        dwnc: bool,
        width: usize,
        fixed: bool,
        fields: &'static [Shape],
    ) -> Shape {
        Shape {
            kind: if tuple { ShapeKind::Tuple } else { ShapeKind::Record },
            width,
            fixed,
            dwnc,
            children: fields,
            ..Shape::OPAQUE
        }
    }
}

const fn str_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    let mut i = 0;
    while i < a.len() {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    true
}

const fn option_eq(a: Option<usize>, b: Option<usize>) -> bool {
    match (a, b) {
        (Some(a), Some(b)) => a == b,
        (None, None) => true,
        _ => false,
    }
}

/// A leaf evolves into itself, and a bounded string into the same kind of
/// string with a bound that is not smaller, or with none (`string<=` /
/// `raw_string<=` with their maximum in `param`; `string` / `raw_string`).
const fn leaf_evolves(old: &Shape, new: &Shape) -> bool {
    if str_eq(old.name, new.name) {
        let bounded = str_eq(old.name, "string<=") || str_eq(old.name, "raw_string<=");
        return if bounded { new.param >= old.param } else { old.param == new.param };
    }
    // Dropping the bound: the name without its "<=".
    (str_eq(old.name, "string<=") && str_eq(new.name, "string"))
        || (str_eq(old.name, "raw_string<=") && str_eq(new.name, "raw_string"))
}

/// The two shapes describe one type: nothing differs, at any depth. What a
/// map key or set element must be, since its key encoding orders the entries.
/// An opaque shape is not known to be the same as anything.
pub const fn shape_same(a: &Shape, b: &Shape) -> bool {
    if a.kind as u8 != b.kind as u8
        || matches!(a.kind, ShapeKind::Opaque)
        || !str_eq(a.name, b.name)
        || a.param != b.param
        || a.width != b.width
        || a.fixed != b.fixed
        || a.optional != b.optional
        || a.selector != b.selector
        || a.dwnc != b.dwnc
        || !option_eq(a.cap, b.cap)
        || !option_eq(a.max_fields, b.max_fields)
        || a.order != b.order
        || a.children.len() != b.children.len()
    {
        return false;
    }
    let mut i = 0;
    while i < a.children.len() {
        if !shape_same(&a.children[i], &b.children[i]) {
            return false;
        }
        i += 1;
    }
    true
}

/// A map entry `(K, V)`: the same key, a value that evolves, and like any
/// tuple the same width.
const fn entry_evolves(old: &Shape, new: &Shape) -> bool {
    matches!(old.kind, ShapeKind::Tuple)
        && matches!(new.kind, ShapeKind::Tuple)
        && old.children.len() == 2
        && new.children.len() == 2
        && old.width == new.width
        && old.fixed == new.fixed
        && shape_same(&old.children[0], &new.children[0])
        && shape_evolves(&old.children[1], &new.children[1])
}

const fn all_evolve(old: &[Shape], new: &[Shape], count: usize) -> bool {
    let mut i = 0;
    while i < count {
        if !shape_evolves(&old[i], &new[i]) {
            return false;
        }
        i += 1;
    }
    true
}

/// C++ `detail::pssz_impl::layout_evolves`.
pub const fn shape_evolves(old: &Shape, new: &Shape) -> bool {
    if old.kind as u8 != new.kind as u8 || old.fixed != new.fixed {
        return false;
    }
    // A variant writes no offsets: the width in its shape is only the class
    // of its bound, which an appended alternative may raise. Whatever holds
    // the variant compares its own width.
    if !matches!(old.kind, ShapeKind::Variant) && old.width != new.width {
        return false;
    }
    match old.kind {
        ShapeKind::Opaque => false,
        // A leaf with children is a marked scalar (a time value): the same
        // marker over the same parts.
        ShapeKind::Leaf => {
            leaf_evolves(old, new)
                && old.children.len() == new.children.len()
                && all_evolve(old.children, new.children, old.children.len())
        }
        ShapeKind::Optional => {
            old.selector == new.selector && all_evolve(old.children, new.children, 1)
        }
        // A maximum (`param`; `usize::MAX` for none) may widen. The order
        // constraint is the member's type.
        ShapeKind::List => {
            new.param >= old.param && old.order == new.order && all_evolve(old.children, new.children, 1)
        }
        ShapeKind::Array => {
            old.param == new.param && old.order == new.order && all_evolve(old.children, new.children, 1)
        }
        ShapeKind::Map => {
            new.param >= old.param && entry_evolves(&old.children[0], &new.children[0])
        }
        ShapeKind::Set => new.param >= old.param && shape_same(&old.children[0], &new.children[0]),
        // The inner value is a frame of its own: it evolves as a top-level
        // type does.
        ShapeKind::Nested => new.param >= old.param && all_evolve(old.children, new.children, 1),
        // docs/pssz-spec.md 7.5: New's alternatives are Old's, in order,
        // each evolved by these rules, followed by any number of new ones.
        // A larger new alternative is refused where it moves the width of
        // what holds the variant (a record, tuple, array, list, map).
        ShapeKind::Variant => {
            new.children.len() >= old.children.len()
                && all_evolve(old.children, new.children, old.children.len())
        }
        ShapeKind::Record | ShapeKind::Tuple => {
            let (no, nn) = (old.children.len(), new.children.len());
            if old.dwnc != new.dwnc || nn < no || (old.dwnc && nn != no) {
                return false;
            }
            if !all_evolve(old.children, new.children, no) {
                return false;
            }
            let mut i = no;
            while i < nn {
                if !new.children[i].optional {
                    return false;
                }
                i += 1;
            }
            // The cap: not lowered; one declared later admits everything
            // the old type could encode. (Its width class is the record's
            // width, compared above.)
            let cap = match (old.cap, new.cap) {
                (_, None) => true,
                (Some(old), Some(new)) => new >= old,
                (None, Some(new)) => match old.bound {
                    Some(bound) => new >= bound,
                    None => false,
                },
            };
            // The field-count cap is a promise about every later revision:
            // not exceeded, not raised, not dropped.
            let fields = match (old.max_fields, new.max_fields) {
                (None, _) => true,
                (Some(old), Some(new)) => nn <= old && new <= old,
                (Some(_), None) => false,
            };
            cap && fields
        }
    }
}

/// True iff `New` is a legal PSSZ evolution of `Old` (see module docs).
/// Usable in `const` context, so a violation can fail the build.
pub const fn pssz_evolves_from<Old, New>() -> bool
where
    Old: PsszPack<Pssz> + ?Sized,
    New: PsszPack<Pssz> + ?Sized,
{
    shape_evolves(&Old::SHAPE, &New::SHAPE)
}
