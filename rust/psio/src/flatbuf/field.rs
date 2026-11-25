//! How Rust types map to FlatBuffers: the traits the derives implement and
//! their implementations for the standard types.
//!
//! | Rust | FlatBuffers |
//! |---|---|
//! | `bool`, `i8` … `u64`, `f32`, `f64` | `bool`, `byte` … `ulong`, `float`, `double` |
//! | field-less `#[repr(int)]` enum | an `enum` of its repr type |
//! | `String` | `string`, checked to be UTF-8 |
//! | [`RawString`] | `string`, any bytes (what C++ `std::string` holds) |
//! | `Vec<T>` | `[T]`: scalars, enums, structs, strings, tables |
//! | `[T; N]` in a struct | a fixed array `[T:N]` |
//! | `[T; N]` in a table | a vector that must hold exactly N elements |
//! | struct | `table` |
//! | struct that is `#[psio(definition_will_not_change)]` and holds only scalars, enums, fixed arrays and such structs | `struct`: fixed size, stored inline, no vtable |
//! | `Option<S>`, S a scalar or enum | an optional scalar (`= null`) |
//! | `Option<X>`, X anything else | the field; absent is `None` |
//! | enum whose variants hold one table each | `union`; a leading unit variant is `NONE` |
//! | `BTreeSet<K>`, K a scalar or string | `[K]`, sorted |
//! | `BTreeMap<K, V>` | `[Entry]` sorted by key, with `table Entry { key:K (key); value:V; }` |
//!
//! The mapping is decided by constants, as in C++ (`psio/flatbuf.hpp`), so
//! one declaration gives the same bytes in both languages.
//!
//! The root of a buffer is a table. A record that maps to a struct cannot
//! be one — wrap it in a record:
//!
//! ```compile_fail
//! #[derive(psio::FbPack)]
//! #[psio(definition_will_not_change)]
//! struct Vec3 { x: f32, y: f32, z: f32 }
//! psio::to_flatbuf(&Vec3 { x: 1.0, y: 2.0, z: 3.0 });   // a struct: no vtable, no root
//! ```
//!
//! ```
//! #[derive(psio::FbPack)]
//! #[psio(definition_will_not_change)]
//! struct Vec3 { x: f32, y: f32, z: f32 }
//! #[derive(psio::FbPack)]
//! struct Shape { at: Vec3 }
//! psio::to_flatbuf(&Shape { at: Vec3 { x: 1.0, y: 2.0, z: 3.0 } });
//! ```

use std::collections::{BTreeMap, BTreeSet};

use super::builder::FbBuilder;
use super::raw::{self, FbTable};
use super::verify::{FbCheckedTable, FbChecker, FbError, FbLimits, FbOptions};
use crate::pssz::RawString;

// ── Encoding ────────────────────────────────────────────────────────────────

/// A type FlatBuffers can hold: its place in the mapping (the constants)
/// and how it is written. Derived with `#[derive(FbPack)]`.
pub trait FbPack: Sized {
    /// A value stored inline in 1, 2, 4 or 8 bytes.
    const SCALAR: bool = false;
    /// A FlatBuffers struct: fixed size, stored inline, no vtable.
    const STRUCT: bool = false;
    /// A FlatBuffers table: what the root of a buffer is.
    const TABLE: bool = false;
    /// A union: two vtable slots, its type and its value.
    const UNION: bool = false;
    /// A string.
    const STRING: bool = false;
    /// A fixed array `[T; N]`.
    const ARRAY: bool = false;
    /// An `Option`.
    const OPTIONAL: bool = false;
    /// May be a member of a struct: a scalar, a struct, a fixed array of those.
    const FIXED: bool = Self::SCALAR || Self::STRUCT;
    /// Size and alignment of a fixed value.
    const FIXED_SIZE: usize = 0;
    const FIXED_ALIGN: usize = 1;
    /// Stored inline in its table (a scalar or struct, or an option of one)
    /// rather than behind an offset.
    const INLINE: bool = Self::SCALAR || Self::STRUCT;
    /// The bytes a present member occupies in its table, or an element in
    /// its vector.
    const INLINE_SIZE: usize = if Self::INLINE { Self::FIXED_SIZE } else { 4 };
    const INLINE_ALIGN: usize = if Self::INLINE { Self::FIXED_ALIGN } else { 4 };
    /// The size `flatc` sorts a field by when it orders the fields of a
    /// table (a struct counts as 4 whatever its alignment).
    const SORT_SIZE: u8 = if Self::SCALAR { Self::FIXED_SIZE as u8 } else { 4 };
    /// Vtable slots the member takes.
    const SLOTS: u16 = if Self::UNION { 2 } else { 1 };
    /// May be an element of a vector.
    const ELEMENT: bool = Self::SCALAR || Self::STRUCT || Self::STRING || Self::TABLE;
    /// May be the key of a set or map.
    const KEY: bool = Self::SCALAR || Self::STRING;
    /// May be a member of a table. `false` for the shapes FlatBuffers has
    /// no encoding for (a vector of vectors, an option of an option, …).
    const SUPPORTED: bool = true;
    /// A scalar whose zero is a value of the type. `false` for an enum with
    /// no enumerator 0: a member of that type declares its default.
    const HAS_ZERO: bool = true;

    /// The FlatBuffers default of a scalar: zero.
    fn fb_zero() -> Option<Self> {
        None
    }

    /// Writes the out-of-line part of the value — a string, a vector, a
    /// table — and returns where it is (its distance from the end of the
    /// buffer), or 0 for nothing. `keep_empty`: write an empty string or
    /// vector (an engaged option, a vector element, a map key).
    fn fb_write_child(&self, _b: &mut FbBuilder, _keep_empty: bool) -> u32 {
        0
    }

    /// A fixed value: its `FIXED_SIZE` bytes.
    fn fb_write_fixed(&self, _dst: &mut [u8]) {
        unreachable!("psio::flatbuf: not a fixed-size type")
    }

    /// Fixed values, one after another.
    fn fb_write_fixed_slice(items: &[Self], dst: &mut [u8]) {
        if Self::FIXED_SIZE == 0 {
            return;
        }
        for (item, chunk) in items.iter().zip(dst.chunks_exact_mut(Self::FIXED_SIZE)) {
            item.fb_write_fixed(chunk);
        }
    }

    /// Adds the member to the table under construction, at vtable slot
    /// `slot`. `child` is what [`fb_write_child`](Self::fb_write_child)
    /// returned; `default` the member's default (a scalar equal to it is
    /// not written); `part` is 1 for the value slot of a union.
    fn fb_add(&self, b: &mut FbBuilder, _part: u8, child: u32, default: Option<&Self>, slot: u16) {
        let vt = 4 + 2 * slot;
        if Self::SCALAR {
            if default.is_some_and(|d| same_scalar(self, d)) {
                return;
            }
            self.fb_write_fixed(b.add_inline(vt, Self::FIXED_ALIGN, Self::FIXED_SIZE));
        } else if Self::STRUCT {
            self.fb_write_fixed(b.add_inline(vt, Self::FIXED_ALIGN, Self::FIXED_SIZE));
        } else if child != 0 {
            b.add_offset_field(vt, child);
        }
    }

    /// A table: writes it (children first) and returns where it is.
    fn fb_write_table(&self, _b: &mut FbBuilder) -> u32 {
        unreachable!("psio::flatbuf: not a table")
    }

    /// The buffer whose root is this table.
    fn fb_pack(&self) -> Vec<u8> {
        self.fb_pack_with(&FbOptions::default())
    }

    /// The buffer whose root is this table, with a file identifier and / or
    /// a size prefix.
    fn fb_pack_with(&self, opts: &FbOptions) -> Vec<u8> {
        const {
            assert!(
                Self::TABLE,
                "psio::flatbuf: the root of a buffer is a table; a struct (a \
                 definition_will_not_change record of fixed members), a scalar, a \
                 string or a vector is wrapped in a record"
            )
        };
        let mut b = FbBuilder::default();
        let root = self.fb_write_table(&mut b);
        b.finish_with(root, opts);
        b.into_vec()
    }
}

/// Whether a scalar equals its default, i.e. is not written. By bytes: for
/// a float -0.0 and a NaN of another payload are values of their own.
fn same_scalar<T: FbPack>(a: &T, b: &T) -> bool {
    let (mut x, mut y) = ([0u8; 8], [0u8; 8]);
    let n = T::FIXED_SIZE.min(8);
    a.fb_write_fixed(&mut x[..n]);
    b.fb_write_fixed(&mut y[..n]);
    x == y
}

/// Writes a vector of `count` elements.
fn write_elements<'x, T: FbPack + 'x>(
    b: &mut FbBuilder,
    count: usize,
    items: impl Iterator<Item = &'x T>,
) -> u32 {
    if T::INLINE {
        let size = T::FIXED_SIZE;
        let dst = b.start_vector(count, size, T::FIXED_ALIGN);
        if size != 0 {
            for (item, chunk) in items.zip(dst.chunks_exact_mut(size)) {
                item.fb_write_fixed(chunk);
            }
        }
        b.end_vector(count)
    } else {
        let offsets: Vec<u32> = items.map(|item| item.fb_write_child(b, true)).collect();
        b.create_vec_offsets(&offsets)
    }
}

/// One vtable slot of a table, in the order the fields are written.
#[derive(Clone, Copy)]
pub struct FbAddEntry {
    pub slot: u16,
    pub member: u16,
    pub size: u8,
    pub part: u8,
    pub live: bool,
}

impl FbAddEntry {
    /// The two entries of member `member` of type `F` at `slot`; the second
    /// is live for a union only (its value offset).
    pub const fn of<F: FbPack>(member: u16, slot: u16) -> [FbAddEntry; 2] {
        [
            FbAddEntry { slot, member, size: if F::UNION { 1 } else { F::SORT_SIZE }, part: 0, live: true },
            FbAddEntry { slot: slot + 1, member, size: 4, part: 1, live: F::UNION },
        ]
    }
}

/// The order in which the fields of a table are written, which is
/// `flatc`'s: by size, largest first, and within a size by descending field
/// id. Entries that are not live sort last.
pub const fn fb_add_order<const N: usize>(members: [[FbAddEntry; 2]; N]) -> [[FbAddEntry; 2]; N] {
    let mut e = members;
    let total = 2 * N;
    let mut i = 1;
    while i < total {
        let mut j = i;
        while j > 0 {
            let (a, b) = (e[j / 2][j % 2], e[(j - 1) / 2][(j - 1) % 2]);
            let before = (a.live && !b.live)
                || (a.live == b.live && (a.size > b.size || (a.size == b.size && a.slot > b.slot)));
            if !before {
                break;
            }
            e[j / 2][j % 2] = b;
            e[(j - 1) / 2][(j - 1) % 2] = a;
            j -= 1;
        }
        i += 1;
    }
    e
}

/// Offsets, size and alignment of a struct: members in declaration order,
/// each at its natural alignment; the size is rounded up to the largest
/// alignment (what `flatc` computes for a struct without `force_align`).
pub struct FbStructLayout<const N: usize> {
    pub offset: [usize; N],
    pub size: usize,
    pub align: usize,
}

/// The layout of a struct whose members have these `(size, alignment)`.
pub const fn fb_struct_layout<const N: usize>(members: [(usize, usize); N]) -> FbStructLayout<N> {
    let mut offset = [0usize; N];
    let (mut pos, mut align) = (0usize, 1usize);
    let mut i = 0;
    while i < N {
        let (size, a) = members[i];
        let a = if a == 0 { 1 } else { a };
        pos = (pos + a - 1) & !(a - 1);
        offset[i] = pos;
        pos += size;
        if a > align {
            align = a;
        }
        i += 1;
    }
    FbStructLayout { offset, size: (pos + align - 1) & !(align - 1), align }
}

// ── Verifying and decoding ──────────────────────────────────────────────────

/// A type that can be read from FlatBuffers bytes: how it is verified and
/// decoded. Derived with `#[derive(FbUnpack)]`.
///
/// Verification is the gate: [`fb_unpack`](FbUnpack::fb_unpack) verifies
/// and then decodes. The decoding functions themselves are lenient — they
/// never fail, panic or read outside the buffer; what does not fit reads as
/// absent — so that they can also serve bytes the caller vouches for.
pub trait FbUnpack: FbPack {
    /// A fixed value some byte patterns are not values of (an enum without
    /// a catch-all variant, or a struct or array holding one).
    const FIXED_CHECKED: bool = false;

    /// Whether the `FIXED_SIZE` bytes are a value of the type.
    fn fb_fixed_valid(_src: &[u8]) -> bool {
        true
    }

    /// Verifies the value whose inline cell — a table field or a vector
    /// element — is at `pos`: its inline bytes, then what they point to.
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool;

    /// Verifies the table at `pos`.
    fn fb_check_table(c: &mut FbChecker<'_>, pos: u64) -> bool {
        c.fail("flatbuf: not a table", pos)
    }

    /// Verifies the member at vtable slot `slot` of table `t`. `required`:
    /// the member must be present (the string key of a map entry).
    fn fb_check_member(c: &mut FbChecker<'_>, t: &FbCheckedTable, slot: u16, required: bool) -> bool {
        let Ok(cell) = c.checked_field(t, slot, Self::INLINE_SIZE as u32) else {
            return false;
        };
        if cell == 0 {
            return !required || c.fail("flatbuf: map entry has no key", t.position());
        }
        Self::fb_check_cell(c, cell)
    }

    /// Whether the key at cell `a` orders before the key at cell `b`; an
    /// absent key is the default. On verified cells.
    fn fb_key_less(_buf: &[u8], _a: Option<usize>, _b: Option<usize>) -> bool {
        false
    }

    /// The bytes [`fb_key_less`](Self::fb_key_less) may read for the keys
    /// at cells `a` and `b` beyond their inline cells: for strings, the
    /// length of the shorter one. The verifier charges it to its budget.
    fn fb_key_cost(_buf: &[u8], _a: Option<usize>, _b: Option<usize>) -> u64 {
        0
    }

    /// A fixed value from its `FIXED_SIZE` bytes.
    fn fb_read_fixed(_src: &[u8]) -> Self {
        unreachable!("psio::flatbuf: not a fixed-size type")
    }

    /// The value whose inline cell is at `cell`.
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self;

    /// The table `t`.
    fn fb_decode_table(_t: FbTable<'_>) -> Self {
        Self::fb_absent()
    }

    /// The member at vtable slot `slot` of table `t`; `default` when the
    /// table does not store it.
    fn fb_decode_member(t: FbTable<'_>, slot: u16, default: Option<Self>) -> Self {
        match t.cell(slot, Self::INLINE_SIZE) {
            Some(cell) => Self::fb_decode_cell(t.buffer(), cell),
            None => default.unwrap_or_else(Self::fb_absent),
        }
    }

    /// The value of a member that is not stored and declares no default:
    /// zero, empty, `None`, a table of defaults.
    fn fb_absent() -> Self;

    /// Verifies `bytes` as a buffer whose root is this table.
    fn fb_validate(bytes: &[u8]) -> Result<(), FbError> {
        Self::fb_validate_with(bytes, &FbOptions::default(), FbLimits::default())
    }

    /// Verifies under other limits, and / or a buffer with a required file
    /// identifier or a size prefix.
    ///
    /// The structure of the buffer first — what FlatBuffers defines — and
    /// then what the type declares about its values
    /// ([`fb_check_declared`](Self::fb_check_declared)), on the value the
    /// buffer decodes to. A type that declares nothing is not decoded.
    fn fb_validate_with(bytes: &[u8], opts: &FbOptions, limits: FbLimits) -> Result<(), FbError> {
        const { assert!(Self::TABLE, "psio::flatbuf: the root of a buffer is a table") };
        super::verify::check_buffer(bytes, opts, limits, Self::fb_check_table)?;
        if Self::fb_has_declarations() {
            let inner = if opts.size_prefixed { bytes.get(4..).unwrap_or(&[]) } else { bytes };
            Self::fb_unpack_trusted(inner).fb_check_declared().map_err(|why| FbError::new(why, 0))?;
        }
        Ok(())
    }

    /// Verifies, then decodes.
    fn fb_unpack(bytes: &[u8]) -> Result<Self, FbError> {
        const { assert!(Self::TABLE, "psio::flatbuf: the root of a buffer is a table") };
        super::verify::check_buffer(bytes, &FbOptions::default(), FbLimits::default(), Self::fb_check_table)?;
        let value = Self::fb_unpack_trusted(bytes);
        if Self::fb_has_declarations() {
            value.fb_check_declared().map_err(|why| FbError::new(why, 0))?;
        }
        Ok(value)
    }

    /// Whether the type, or anything a value of it holds, declares
    /// something a value can violate: a bound or an order on a member
    /// (`#[psio(max = N)]`, `element(...)`, `sorted`, …) or an invariant.
    /// `seen`: the types this search has entered (a type may hold itself).
    fn fb_declares(_seen: &mut crate::contract::Seen) -> bool {
        false
    }

    /// [`fb_declares`](Self::fb_declares) for the type as a root. The
    /// derive keeps the answer of a type without parameters.
    fn fb_has_declarations() -> bool {
        Self::fb_declares(&mut crate::contract::Seen::new())
    }

    /// What the type declares about its values, on `self`: the first
    /// violation (the text C++ `validate_flatbuf` reports), or `Ok`. The
    /// verifier and the checked decode call it after the structure of the
    /// buffer has passed (C++ `psio::check_value_contract`).
    fn fb_check_declared(&self) -> Result<(), &'static str> {
        Ok(())
    }

    /// Decodes bytes the caller vouches for, without verifying them. Never
    /// reads outside `bytes` and never panics on what it reads; on bytes
    /// that would not verify the value is unspecified, and the work is not
    /// bounded by the verifier's limits (a hostile buffer can nest deeply or
    /// share sub-objects). Verify untrusted bytes: [`fb_unpack`](Self::fb_unpack).
    fn fb_unpack_trusted(bytes: &[u8]) -> Self {
        const { assert!(Self::TABLE, "psio::flatbuf: the root of a buffer is a table") };
        Self::fb_decode_table(FbTable::root(bytes))
    }
}

/// Verifies the fixed value at `pos`: alignment, bounds, and — where the
/// type has byte patterns that are not values — the value.
pub fn fb_check_fixed<T: FbUnpack>(c: &mut FbChecker<'_>, pos: u64) -> bool {
    if !c.check_inline(pos, T::FIXED_SIZE as u64, T::FIXED_ALIGN as u64) {
        return false;
    }
    if T::FIXED_CHECKED {
        let at = pos as usize;
        let valid = c.buffer().get(at..at + T::FIXED_SIZE).is_some_and(T::fb_fixed_valid);
        if !valid {
            return c.fail("flatbuf: unknown enum value", pos);
        }
    }
    true
}

/// The fixed value at `pos`; the absent value when it is not inside the
/// buffer.
pub fn fb_fixed_at<T: FbUnpack>(buf: &[u8], pos: usize) -> T {
    match pos.checked_add(T::FIXED_SIZE).and_then(|end| buf.get(pos..end)) {
        Some(src) => T::fb_read_fixed(src),
        None => T::fb_absent(),
    }
}

fn fixed_opt<T: FbUnpack>(buf: &[u8], cell: Option<usize>) -> T {
    match cell {
        Some(cell) => fb_fixed_at(buf, cell),
        None => T::fb_absent(),
    }
}

/// The elements of the verified vector at `target`.
fn check_elements<T: FbUnpack>(c: &mut FbChecker<'_>, target: u64, count: u64) -> bool {
    if !T::INLINE {
        for i in 0..count {
            if !T::fb_check_cell(c, target + 4 + i * 4) {
                return false;
            }
        }
    } else if T::FIXED_CHECKED {
        let size = T::FIXED_SIZE as u64;
        for i in 0..count {
            let at = (target + 4 + i * size) as usize;
            if !c.buffer().get(at..at + size as usize).is_some_and(T::fb_fixed_valid) {
                return c.fail("flatbuf: unknown enum value", at as u64);
            }
        }
    }
    true
}

/// The vector the offset at `pos` points to, with elements of type `T`:
/// its position and count, or `None` after recording an error.
fn check_vector_of<T: FbUnpack>(c: &mut FbChecker<'_>, pos: u64) -> Option<(u64, u64)> {
    let target = c.check_offset_cell(pos);
    if target == 0 {
        return None;
    }
    let count = c.check_vector(target, T::INLINE_SIZE as u64, T::INLINE_ALIGN as u64, T::INLINE)?;
    Some((target, count))
}

/// The start and element count of the vector the offset at `cell` points
/// to, when its `count * size` bytes are inside the buffer.
pub(crate) fn vector_at(buf: &[u8], cell: usize, size: usize) -> Option<(usize, usize)> {
    let target = raw::follow(buf, cell)?;
    let count = raw::u32_at(buf, target)? as usize;
    let data = target + 4;
    (data.checked_add(count.checked_mul(size)?)? <= buf.len()).then_some((data, count))
}

fn decode_element<T: FbUnpack>(buf: &[u8], pos: usize) -> T {
    if T::INLINE {
        fb_fixed_at(buf, pos)
    } else {
        T::fb_decode_cell(buf, pos)
    }
}

// ── Scalars ─────────────────────────────────────────────────────────────────

macro_rules! scalar {
    ($($ty:ty),*) => {$(
        impl FbPack for $ty {
            const SCALAR: bool = true;
            const FIXED_SIZE: usize = std::mem::size_of::<$ty>();
            const FIXED_ALIGN: usize = std::mem::size_of::<$ty>();
            fn fb_zero() -> Option<Self> {
                Some(<$ty>::default())
            }
            #[inline]
            fn fb_write_fixed(&self, dst: &mut [u8]) {
                dst.copy_from_slice(&self.to_le_bytes());
            }
        }
        impl FbUnpack for $ty {
            fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
                fb_check_fixed::<Self>(c, pos)
            }
            fn fb_key_less(buf: &[u8], a: Option<usize>, b: Option<usize>) -> bool {
                fixed_opt::<Self>(buf, a) < fixed_opt::<Self>(buf, b)
            }
            #[inline]
            fn fb_read_fixed(src: &[u8]) -> Self {
                src.try_into().map_or(<$ty>::default(), <$ty>::from_le_bytes)
            }
            fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
                fb_fixed_at(buf, cell)
            }
            fn fb_absent() -> Self {
                <$ty>::default()
            }
        }
    )*};
}
scalar!(i8, i16, i32, i64, u16, u32, u64, f32, f64);

impl FbPack for u8 {
    const SCALAR: bool = true;
    const FIXED_SIZE: usize = 1;
    const FIXED_ALIGN: usize = 1;
    fn fb_zero() -> Option<Self> {
        Some(0)
    }
    #[inline]
    fn fb_write_fixed(&self, dst: &mut [u8]) {
        dst[0] = *self;
    }
    fn fb_write_fixed_slice(items: &[Self], dst: &mut [u8]) {
        dst[..items.len()].copy_from_slice(items);
    }
}
impl FbUnpack for u8 {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        fb_check_fixed::<Self>(c, pos)
    }
    fn fb_key_less(buf: &[u8], a: Option<usize>, b: Option<usize>) -> bool {
        fixed_opt::<Self>(buf, a) < fixed_opt::<Self>(buf, b)
    }
    #[inline]
    fn fb_read_fixed(src: &[u8]) -> Self {
        src.first().copied().unwrap_or(0)
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        fb_fixed_at(buf, cell)
    }
    fn fb_absent() -> Self {
        0
    }
}

impl FbPack for bool {
    const SCALAR: bool = true;
    const FIXED_SIZE: usize = 1;
    const FIXED_ALIGN: usize = 1;
    fn fb_zero() -> Option<Self> {
        Some(false)
    }
    #[inline]
    fn fb_write_fixed(&self, dst: &mut [u8]) {
        dst[0] = *self as u8;
    }
}
impl FbUnpack for bool {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        fb_check_fixed::<Self>(c, pos)
    }
    fn fb_key_less(buf: &[u8], a: Option<usize>, b: Option<usize>) -> bool {
        !fixed_opt::<Self>(buf, a) & fixed_opt::<Self>(buf, b)
    }
    // Any byte that is not zero is true, as in C++.
    #[inline]
    fn fb_read_fixed(src: &[u8]) -> Self {
        src.first().is_some_and(|b| *b != 0)
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        fb_fixed_at(buf, cell)
    }
    fn fb_absent() -> Self {
        false
    }
}

// ── Strings ─────────────────────────────────────────────────────────────────

/// The bytes of the string the offset at `cell` points to; empty when
/// absent or outside the buffer.
pub(crate) fn string_at(buf: &[u8], cell: Option<usize>) -> &[u8] {
    cell.and_then(|cell| raw::follow(buf, cell))
        .and_then(|target| raw::string_bytes(buf, target))
        .unwrap_or(&[])
}

fn check_string_cell(c: &mut FbChecker<'_>, pos: u64) -> u64 {
    let target = c.check_offset_cell(pos);
    if target == 0 || !c.check_string(target) {
        return 0;
    }
    target
}

impl FbPack for String {
    const STRING: bool = true;
    fn fb_write_child(&self, b: &mut FbBuilder, keep_empty: bool) -> u32 {
        if self.is_empty() && !keep_empty {
            0
        } else {
            b.create_bytes_string(self.as_bytes())
        }
    }
}
impl FbUnpack for String {
    /// A Rust `String` is UTF-8, so the verifier checks it. (C++
    /// `std::string` holds any bytes; that is [`RawString`] here.)
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        let target = check_string_cell(c, pos);
        target != 0 && c.check_utf8(target)
    }
    fn fb_key_less(buf: &[u8], a: Option<usize>, b: Option<usize>) -> bool {
        string_at(buf, a) < string_at(buf, b)
    }
    fn fb_key_cost(buf: &[u8], a: Option<usize>, b: Option<usize>) -> u64 {
        string_at(buf, a).len().min(string_at(buf, b).len()) as u64
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        std::str::from_utf8(string_at(buf, Some(cell))).unwrap_or("").to_string()
    }
    fn fb_absent() -> Self {
        String::new()
    }
}

impl FbPack for RawString {
    const STRING: bool = true;
    fn fb_write_child(&self, b: &mut FbBuilder, keep_empty: bool) -> u32 {
        if self.0.is_empty() && !keep_empty {
            0
        } else {
            b.create_bytes_string(&self.0)
        }
    }
}
impl FbUnpack for RawString {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        check_string_cell(c, pos) != 0
    }
    fn fb_key_less(buf: &[u8], a: Option<usize>, b: Option<usize>) -> bool {
        string_at(buf, a) < string_at(buf, b)
    }
    fn fb_key_cost(buf: &[u8], a: Option<usize>, b: Option<usize>) -> u64 {
        string_at(buf, a).len().min(string_at(buf, b).len()) as u64
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        RawString(string_at(buf, Some(cell)).to_vec())
    }
    fn fb_absent() -> Self {
        RawString::new()
    }
}

// ── Vectors ─────────────────────────────────────────────────────────────────

impl<T: FbPack> FbPack for Vec<T> {
    const SUPPORTED: bool = T::ELEMENT;
    fn fb_write_child(&self, b: &mut FbBuilder, keep_empty: bool) -> u32 {
        if self.is_empty() && !keep_empty {
            return 0;
        }
        if T::INLINE {
            let dst = b.start_vector(self.len(), T::FIXED_SIZE, T::FIXED_ALIGN);
            T::fb_write_fixed_slice(self, dst);
            b.end_vector(self.len())
        } else {
            write_elements(b, self.len(), self.iter())
        }
    }
}
impl<T: FbUnpack> FbUnpack for Vec<T> {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        match check_vector_of::<T>(c, pos) {
            Some((target, count)) => check_elements::<T>(c, target, count),
            None => false,
        }
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        let size = T::INLINE_SIZE;
        match vector_at(buf, cell, size) {
            Some((data, count)) => (0..count).map(|i| decode_element(buf, data + i * size)).collect(),
            None => Vec::new(),
        }
    }
    fn fb_absent() -> Self {
        Vec::new()
    }
    fn fb_declares(seen: &mut crate::contract::Seen) -> bool {
        T::fb_declares(seen)
    }
    fn fb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(T::fb_check_declared)
    }
}

/// `[T; N]`: in a struct a fixed array, in a table a vector that must hold
/// exactly N elements.
impl<T: FbPack, const N: usize> FbPack for [T; N] {
    const ARRAY: bool = true;
    const FIXED: bool = N > 0 && !T::ARRAY && T::FIXED;
    const FIXED_SIZE: usize = N * T::FIXED_SIZE;
    const FIXED_ALIGN: usize = T::FIXED_ALIGN;
    const INLINE: bool = false;
    const ELEMENT: bool = false;
    const KEY: bool = false;
    const SUPPORTED: bool = N > 0 && T::ELEMENT;
    fn fb_write_child(&self, b: &mut FbBuilder, _keep_empty: bool) -> u32 {
        write_elements(b, N, self.iter())
    }
    fn fb_write_fixed(&self, dst: &mut [u8]) {
        T::fb_write_fixed_slice(self, dst);
    }
}
impl<T: FbUnpack, const N: usize> FbUnpack for [T; N] {
    const FIXED_CHECKED: bool = T::FIXED_CHECKED;
    fn fb_fixed_valid(src: &[u8]) -> bool {
        T::FIXED_SIZE == 0 || src.chunks_exact(T::FIXED_SIZE).all(T::fb_fixed_valid)
    }
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        let Some((target, count)) = check_vector_of::<T>(c, pos) else {
            return false;
        };
        if count != N as u64 {
            return c.fail("flatbuf: a fixed-length vector has the wrong length", target);
        }
        check_elements::<T>(c, target, count)
    }
    fn fb_read_fixed(src: &[u8]) -> Self {
        let size = T::FIXED_SIZE;
        std::array::from_fn(|i| match src.get(i * size..(i + 1) * size) {
            Some(bytes) => T::fb_read_fixed(bytes),
            None => T::fb_absent(),
        })
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        let size = T::INLINE_SIZE;
        let (data, count) = vector_at(buf, cell, size).unwrap_or((0, 0));
        std::array::from_fn(|i| if i < count { decode_element(buf, data + i * size) } else { T::fb_absent() })
    }
    fn fb_absent() -> Self {
        std::array::from_fn(|_| T::fb_absent())
    }
    fn fb_declares(seen: &mut crate::contract::Seen) -> bool {
        T::fb_declares(seen)
    }
    fn fb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(T::fb_check_declared)
    }
}

// ── Options ─────────────────────────────────────────────────────────────────

impl<T: FbPack> FbPack for Option<T> {
    const OPTIONAL: bool = true;
    const FIXED: bool = false;
    const INLINE: bool = T::INLINE;
    const INLINE_SIZE: usize = T::INLINE_SIZE;
    const INLINE_ALIGN: usize = T::INLINE_ALIGN;
    const SORT_SIZE: u8 = T::SORT_SIZE;
    const ELEMENT: bool = false;
    const KEY: bool = false;
    const SUPPORTED: bool = !T::OPTIONAL && !T::UNION && T::SUPPORTED;
    fn fb_write_child(&self, b: &mut FbBuilder, _keep_empty: bool) -> u32 {
        match self {
            Some(value) => value.fb_write_child(b, true),
            None => 0,
        }
    }
    fn fb_add(&self, b: &mut FbBuilder, _part: u8, child: u32, _default: Option<&Self>, slot: u16) {
        let vt = 4 + 2 * slot;
        if T::INLINE {
            if let Some(value) = self {
                value.fb_write_fixed(b.add_inline(vt, T::FIXED_ALIGN, T::FIXED_SIZE));
            }
        } else if child != 0 {
            b.add_offset_field(vt, child);
        }
    }
}
impl<T: FbUnpack> FbUnpack for Option<T> {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        T::fb_check_cell(c, pos)
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        Some(T::fb_decode_cell(buf, cell))
    }
    fn fb_absent() -> Self {
        None
    }
    fn fb_declares(seen: &mut crate::contract::Seen) -> bool {
        T::fb_declares(seen)
    }
    fn fb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(T::fb_check_declared)
    }
}

// ── Sorted vectors ──────────────────────────────────────────────────────────

impl<K: FbPack + Ord> FbPack for BTreeSet<K> {
    const SUPPORTED: bool = K::KEY;
    fn fb_write_child(&self, b: &mut FbBuilder, keep_empty: bool) -> u32 {
        if self.is_empty() && !keep_empty {
            return 0;
        }
        write_elements(b, self.len(), self.iter())
    }
}
impl<K: FbUnpack + Ord> FbUnpack for BTreeSet<K> {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        let Some((target, count)) = check_vector_of::<K>(c, pos) else {
            return false;
        };
        if !check_elements::<K>(c, target, count) {
            return false;
        }
        let step = K::INLINE_SIZE as u64;
        let buf = c.buffer();
        for i in 1..count {
            let at = |i: u64| Some((target + 4 + i * step) as usize);
            if !c.charge_key_bytes(K::fb_key_cost(buf, at(i - 1), at(i)), target) {
                return false;
            }
            if !K::fb_key_less(buf, at(i - 1), at(i)) {
                return c.fail("flatbuf: set keys are not in ascending order", target);
            }
        }
        true
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        let size = K::INLINE_SIZE;
        match vector_at(buf, cell, size) {
            Some((data, count)) => (0..count).map(|i| decode_element(buf, data + i * size)).collect(),
            None => BTreeSet::new(),
        }
    }
    fn fb_absent() -> Self {
        BTreeSet::new()
    }
    fn fb_declares(seen: &mut crate::contract::Seen) -> bool {
        K::fb_declares(seen)
    }
    fn fb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(K::fb_check_declared)
    }
}

/// The table of a map entry: `table Entry { key:K (key); value:V; }`.
fn write_entry<K: FbPack, V: FbPack>(b: &mut FbBuilder, key: &K, value: &V) -> u32 {
    // The key is `(key)` in the schema, which makes a string key a required
    // field: it is written even when empty.
    let key_child = key.fb_write_child(b, true);
    let value_child = value.fb_write_child(b, false);
    let (key_zero, value_zero) = (K::fb_zero(), V::fb_zero());
    b.start_table();
    if K::SORT_SIZE > V::SORT_SIZE {
        key.fb_add(b, 0, key_child, key_zero.as_ref(), 0);
        value.fb_add(b, 0, value_child, value_zero.as_ref(), 1);
    } else {
        value.fb_add(b, 0, value_child, value_zero.as_ref(), 1);
        key.fb_add(b, 0, key_child, key_zero.as_ref(), 0);
    }
    b.end_table()
}

/// The cell of the key of the map entry whose offset is at `cell`.
pub(crate) fn entry_key_cell<K: FbPack>(buf: &[u8], cell: usize) -> Option<usize> {
    FbTable::follow(buf, cell).cell(0, K::INLINE_SIZE)
}

impl<K: FbPack + Ord, V: FbPack> FbPack for BTreeMap<K, V> {
    const SUPPORTED: bool = K::KEY && !V::UNION && V::SUPPORTED;
    fn fb_write_child(&self, b: &mut FbBuilder, keep_empty: bool) -> u32 {
        if self.is_empty() && !keep_empty {
            return 0;
        }
        let offsets: Vec<u32> = self.iter().map(|(k, v)| write_entry(b, k, v)).collect();
        b.create_vec_offsets(&offsets)
    }
}
impl<K: FbUnpack + Ord, V: FbUnpack> FbUnpack for BTreeMap<K, V> {
    fn fb_check_cell(c: &mut FbChecker<'_>, pos: u64) -> bool {
        let target = c.check_offset_cell(pos);
        if target == 0 {
            return false;
        }
        let Some(count) = c.check_vector(target, 4, 4, false) else {
            return false;
        };
        for i in 0..count {
            let entry = c.check_offset(target + 4 + i * 4);
            if entry == 0 {
                return false;
            }
            let Some(t) = c.begin_table(entry) else {
                return false;
            };
            let ok = K::fb_check_member(c, &t, 0, K::STRING) && V::fb_check_member(c, &t, 1, false);
            c.end_table();
            if !ok {
                return false;
            }
        }
        let buf = c.buffer();
        for i in 1..count {
            let key = |i: u64| entry_key_cell::<K>(buf, (target + 4 + i * 4) as usize);
            if !c.charge_key_bytes(K::fb_key_cost(buf, key(i - 1), key(i)), target) {
                return false;
            }
            if !K::fb_key_less(buf, key(i - 1), key(i)) {
                return c.fail("flatbuf: map keys are not in ascending order", target);
            }
        }
        true
    }
    fn fb_decode_cell(buf: &[u8], cell: usize) -> Self {
        let Some((data, count)) = vector_at(buf, cell, 4) else {
            return BTreeMap::new();
        };
        (0..count)
            .map(|i| {
                let t = FbTable::follow(buf, data + i * 4);
                (K::fb_decode_member(t, 0, None), V::fb_decode_member(t, 1, None))
            })
            .collect()
    }
    fn fb_absent() -> Self {
        BTreeMap::new()
    }
    fn fb_declares(seen: &mut crate::contract::Seen) -> bool {
        // (Both: `||` would leave the mapped type unentered.)
        let key = K::fb_declares(seen);
        V::fb_declares(seen) || key
    }
    fn fb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(|(key, value)| {
            key.fb_check_declared()?;
            value.fb_check_declared()
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn struct_layout_is_flatc() {
        // struct Mixed { a:ubyte; b:ulong; c:ushort; d:[int:3]; v:Vec3; e:bool; }
        let l = fb_struct_layout([(1, 1), (8, 8), (2, 2), (12, 4), (12, 4), (1, 1)]);
        assert_eq!(l.offset, [0, 8, 16, 20, 32, 44]);
        assert_eq!((l.size, l.align), (48, 8));
    }

    #[test]
    fn add_order_is_by_size_then_descending_slot() {
        let order = fb_add_order([
            FbAddEntry::of::<u8>(0, 0),
            FbAddEntry::of::<u64>(1, 1),
            FbAddEntry::of::<String>(2, 2),
            FbAddEntry::of::<u64>(3, 3),
            FbAddEntry::of::<u32>(4, 4),
        ]);
        let live: Vec<u16> = order.iter().flatten().filter(|e| e.live).map(|e| e.slot).collect();
        assert_eq!(live, [3, 1, 4, 2, 0]);
    }
}
