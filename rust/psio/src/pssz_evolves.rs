//! Compile-time PSSZ schema-evolution check: the Rust equivalent of C++
//! `psio::pssz_evolves_from<Old, New>`.
//!
//! ```ignore
//! const _: () = assert!(psio::pssz::pssz_evolves_from::<AccountV1, AccountV2>());
//! ```
//!
//! `New` evolves from `Old` iff data written for either is readable as the
//! other under the automatic (type-derived width) profile:
//!   - New's fields are Old's fields, in order, followed only by `Option`
//!     fields;
//!   - the two have the same offset width. An optional field may not be added
//!     if it would change the type's width: an explicit `max_dynamic_data`
//!     always rules, and without one the width falls back to the bound
//!     inferred from the fields, which an appended field can move. Reserve
//!     room with an explicit cap to evolve;
//!   - fixed-layout (DWNC) status is unchanged and a DWNC record gains no
//!     fields.
//! A prefix field may itself have evolved (a nested record, or a vector /
//! array / optional / variant / tuple of one) under the same rules.
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
//!     `Nested<T, V, N>` are part of the type, like the bound of a
//!     `BoundedList`: a value one side accepts and the other refuses is not
//!     readable as both.
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
        children: &[],
    };
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
    if old.kind as u8 != new.kind as u8 || old.width != new.width || old.fixed != new.fixed {
        return false;
    }
    match old.kind {
        ShapeKind::Opaque => false,
        // A leaf with children is a marked scalar (a time value): the same
        // marker over the same parts.
        ShapeKind::Leaf => {
            str_eq(old.name, new.name)
                && old.param == new.param
                && old.children.len() == new.children.len()
                && all_evolve(old.children, new.children, old.children.len())
        }
        ShapeKind::Optional => {
            old.selector == new.selector && all_evolve(old.children, new.children, 1)
        }
        ShapeKind::List => old.param == new.param && all_evolve(old.children, new.children, 1),
        ShapeKind::Array => old.param == new.param && all_evolve(old.children, new.children, 1),
        ShapeKind::Map => {
            old.param == new.param && entry_evolves(&old.children[0], &new.children[0])
        }
        ShapeKind::Set => old.param == new.param && shape_same(&old.children[0], &new.children[0]),
        // The inner value is a frame of its own: it evolves as a top-level
        // type does.
        ShapeKind::Nested => old.param == new.param && all_evolve(old.children, new.children, 1),
        ShapeKind::Variant => {
            old.children.len() == new.children.len()
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
            true
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
