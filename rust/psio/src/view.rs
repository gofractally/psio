//! Zero-copy view types for fracpack data.
//!
//! View types read directly from the underlying bytes without deserialization
//! or allocation. Like C++ `frac_ref`, [`FracViewType::view`] is zero-cost
//! and does **not** validate. Call [`Unpack::verify_no_extra`] separately
//! if you need to validate untrusted data.
//!
//! # Example
//!
//! ```
//! use psio1::{Pack, Unpack, FracViewType};
//!
//! // Zero-cost view (caller must ensure data is valid)
//! let data = "hello".to_string().packed();
//! let view: &str = String::view(&data);
//! assert_eq!(view, "hello");
//!
//! // For untrusted data, validate first:
//! String::verify_no_extra(&data).unwrap();
//! let view: &str = String::view(&data);
//! assert_eq!(view, "hello");
//!
//! // Or use the combined validate-and-view:
//! let view: &str = String::view_validated(&data).unwrap();
//! assert_eq!(view, "hello");
//! ```

use crate::Unpack;
use std::marker::PhantomData;

#[inline(always)]
fn read_u32_at(data: &[u8], pos: usize) -> u32 {
    u32::from_le_bytes(data[pos..pos + 4].try_into().unwrap())
}

/// Maps a fracpack type to its zero-copy view representation.
///
/// View types read directly from bytes without copying or allocating.
/// Like C++ `frac_ref`, [`FracViewType::view`] is zero-cost and does **not**
/// validate. For untrusted data, call [`Unpack::verify_no_extra`] first, or
/// use [`FracViewType::view_validated`] for the combined validate-and-view path.
pub trait FracViewType<'a>: Unpack<'a> {
    /// The zero-copy view type (e.g., `u32` for `u32`, `&'a str` for `String`)
    type View: Copy + 'a;

    /// Read a view from packed data starting at absolute position `pos`.
    ///
    /// # Safety contract
    /// Caller must ensure data is valid fracpack (e.g., via prior `verify_no_extra`).
    /// Panics on out-of-bounds access if data is malformed.
    fn view_at(data: &'a [u8], pos: u32) -> Self::View;

    /// Read a view of an embedded field within a struct or container.
    /// `data` is the full message buffer; `fixed_pos` is the absolute position
    /// of this field's fixed-size slot (advanced past it on return).
    ///
    /// # Safety contract
    /// Caller must ensure data is valid fracpack. Panics if data is malformed.
    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> Self::View;

    /// View of an empty container (e.g., `""` for `String`, empty for `Vec<T>`).
    /// Called when `Option` wraps an empty container (offset == 0).
    ///
    /// # Panics
    /// Panics for types that don't support empty containers.
    fn view_empty() -> Self::View {
        panic!("type does not support empty container view")
    }

    /// Zero-cost view of top-level packed data. Does **not** validate.
    ///
    /// This matches C++ `frac_ref` semantics: validation is the caller's
    /// responsibility. Use [`view_validated`](Self::view_validated) if you
    /// need a one-step validate-and-view.
    ///
    /// # Safety contract
    /// Caller must ensure `data` is valid fracpack. Panics on malformed data.
    #[inline(always)]
    fn view(data: &'a [u8]) -> Self::View {
        Self::view_at(data, 0)
    }

    /// Validate, then create a zero-copy view of top-level packed data.
    ///
    /// Equivalent to calling [`Unpack::verify_no_extra`] followed by [`view`](Self::view).
    fn view_validated(data: &'a [u8]) -> crate::Result<Self::View> {
        Self::verify_no_extra(data)?;
        Ok(Self::view_at(data, 0))
    }

    /// Convert a zero-copy view back to the owned type.
    /// Used by `compact()` to reconstruct owned values from non-canonical data.
    #[doc(hidden)]
    fn view_to_owned(view: Self::View) -> Self
    where
        Self: Sized;
}

// ── bool ──

impl<'a> FracViewType<'a> for bool {
    type View = bool;

    fn view_at(data: &'a [u8], pos: u32) -> bool {
        data[pos as usize] != 0
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> bool {
        let v = data[*fixed_pos as usize] != 0;
        *fixed_pos += 1;
        v
    }

    fn view_to_owned(view: bool) -> bool {
        view
    }
}

// ── scalar types ──

macro_rules! scalar_view_impl {
    ($t:ty) => {
        impl<'a> FracViewType<'a> for $t {
            type View = $t;

            fn view_at(data: &'a [u8], pos: u32) -> $t {
                let p = pos as usize;
                let sz = std::mem::size_of::<$t>();
                <$t>::from_le_bytes(data[p..p + sz].try_into().unwrap())
            }

            fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> $t {
                let p = *fixed_pos as usize;
                let sz = std::mem::size_of::<$t>();
                *fixed_pos += sz as u32;
                <$t>::from_le_bytes(data[p..p + sz].try_into().unwrap())
            }

            fn view_to_owned(view: $t) -> $t {
                view
            }
        }
    };
}

scalar_view_impl!(u8);
scalar_view_impl!(u16);
scalar_view_impl!(u32);
scalar_view_impl!(u64);
scalar_view_impl!(i8);
scalar_view_impl!(i16);
scalar_view_impl!(i32);
scalar_view_impl!(i64);
scalar_view_impl!(f32);
scalar_view_impl!(f64);

// ── String → &'a str ──

impl<'a> FracViewType<'a> for String {
    type View = &'a str;

    fn view_at(data: &'a [u8], pos: u32) -> &'a str {
        let p = pos as usize;
        let len = read_u32_at(data, p) as usize;
        // Safety: UTF-8 was checked during verify_no_extra / verify
        unsafe { std::str::from_utf8_unchecked(&data[p + 4..p + 4 + len]) }
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> &'a str {
        let p = *fixed_pos as usize;
        let offset = read_u32_at(data, p);
        *fixed_pos += 4;
        if offset == 0 {
            return "";
        }
        let start = p + offset as usize;
        let len = read_u32_at(data, start) as usize;
        // Safety: UTF-8 was checked during verify_no_extra / verify
        unsafe { std::str::from_utf8_unchecked(&data[start + 4..start + 4 + len]) }
    }

    fn view_empty() -> &'a str {
        ""
    }

    fn view_to_owned(view: &str) -> String {
        view.to_string()
    }
}

// ── &str → &'a str ──

impl<'a> FracViewType<'a> for &'a str {
    type View = &'a str;

    fn view_at(data: &'a [u8], pos: u32) -> &'a str {
        <String as FracViewType<'a>>::view_at(data, pos)
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> &'a str {
        <String as FracViewType<'a>>::view_embedded(data, fixed_pos)
    }

    fn view_empty() -> &'a str {
        ""
    }

    fn view_to_owned(view: &'a str) -> &'a str {
        view
    }
}

// ── &[u8] → &'a [u8] ──

impl<'a> FracViewType<'a> for &'a [u8] {
    type View = &'a [u8];

    fn view_at(data: &'a [u8], pos: u32) -> &'a [u8] {
        let p = pos as usize;
        let len = read_u32_at(data, p) as usize;
        &data[p + 4..p + 4 + len]
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> &'a [u8] {
        let p = *fixed_pos as usize;
        let offset = read_u32_at(data, p);
        *fixed_pos += 4;
        if offset == 0 {
            return &[];
        }
        let start = p + offset as usize;
        let len = read_u32_at(data, start) as usize;
        &data[start + 4..start + 4 + len]
    }

    fn view_empty() -> &'a [u8] {
        &[]
    }

    fn view_to_owned(view: &'a [u8]) -> &'a [u8] {
        view
    }
}

// ── Vec<T> → FracVecView<'a, T> ──

/// Zero-copy view of a fracpack `Vec<T>`.
///
/// Provides `len()`, `get(index)`, and `iter()` without deserializing elements.
pub struct FracVecView<'a, T: FracViewType<'a>> {
    data: &'a [u8],
    start: u32,
    num_bytes: u32,
    _phantom: PhantomData<fn() -> T>,
}

impl<'a, T: FracViewType<'a>> Clone for FracVecView<'a, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<'a, T: FracViewType<'a>> Copy for FracVecView<'a, T> {}

impl<'a, T: FracViewType<'a>> FracVecView<'a, T> {
    pub fn len(&self) -> usize {
        let fs = <T as Unpack>::FIXED_SIZE;
        if fs == 0 {
            0
        } else {
            (self.num_bytes / fs) as usize
        }
    }

    pub fn is_empty(&self) -> bool {
        self.num_bytes == 0
    }

    pub fn get(&self, index: usize) -> T::View {
        let mut fixed_pos = self.start + (index as u32) * <T as Unpack>::FIXED_SIZE;
        T::view_embedded(self.data, &mut fixed_pos)
    }

    pub fn iter(&self) -> FracVecViewIter<'a, T> {
        FracVecViewIter {
            data: self.data,
            fixed_pos: self.start,
            remaining: self.len(),
            _phantom: PhantomData,
        }
    }
}

impl<'a, T: FracViewType<'a>> std::fmt::Debug for FracVecView<'a, T>
where
    T::View: std::fmt::Debug,
{
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

impl<'a, T: FracViewType<'a>> IntoIterator for FracVecView<'a, T> {
    type Item = T::View;
    type IntoIter = FracVecViewIter<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a, T: FracViewType<'a>> PartialEq for FracVecView<'a, T>
where
    T::View: PartialEq,
{
    fn eq(&self, other: &Self) -> bool {
        self.len() == other.len() && self.iter().zip(other.iter()).all(|(a, b)| a == b)
    }
}

/// Iterator over elements of a [`FracVecView`].
pub struct FracVecViewIter<'a, T: FracViewType<'a>> {
    data: &'a [u8],
    fixed_pos: u32,
    remaining: usize,
    _phantom: PhantomData<fn() -> T>,
}

impl<'a, T: FracViewType<'a>> Iterator for FracVecViewIter<'a, T> {
    type Item = T::View;

    fn next(&mut self) -> Option<T::View> {
        if self.remaining == 0 {
            return None;
        }
        self.remaining -= 1;
        Some(T::view_embedded(self.data, &mut self.fixed_pos))
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.remaining, Some(self.remaining))
    }
}

impl<'a, T: FracViewType<'a>> ExactSizeIterator for FracVecViewIter<'a, T> {}

impl<'a, T: FracViewType<'a> + 'a> FracViewType<'a> for Vec<T> {
    type View = FracVecView<'a, T>;

    fn view_at(data: &'a [u8], pos: u32) -> FracVecView<'a, T> {
        let p = pos as usize;
        let num_bytes = read_u32_at(data, p);
        FracVecView {
            data,
            start: pos + 4,
            num_bytes,
            _phantom: PhantomData,
        }
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> FracVecView<'a, T> {
        let p = *fixed_pos as usize;
        let offset = read_u32_at(data, p);
        *fixed_pos += 4;
        if offset == 0 {
            return Self::view_empty();
        }
        let data_pos = (p as u32) + offset;
        let num_bytes = read_u32_at(data, data_pos as usize);
        FracVecView {
            data,
            start: data_pos + 4,
            num_bytes,
            _phantom: PhantomData,
        }
    }

    fn view_empty() -> FracVecView<'a, T> {
        FracVecView {
            data: &[],
            start: 0,
            num_bytes: 0,
            _phantom: PhantomData,
        }
    }

    fn view_to_owned(view: FracVecView<'a, T>) -> Vec<T> {
        view.iter().map(|v| T::view_to_owned(v)).collect()
    }
}

// ── Option<T> ──

impl<'a, T: FracViewType<'a>> FracViewType<'a> for Option<T> {
    type View = Option<T::View>;

    fn view_at(data: &'a [u8], pos: u32) -> Option<T::View> {
        let offset = read_u32_at(data, pos as usize);
        if offset == 1 {
            return None;
        }
        if offset == 0 {
            return Some(T::view_empty());
        }
        Some(T::view_at(data, pos + offset))
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> Option<T::View> {
        let p = *fixed_pos;
        let offset = read_u32_at(data, p as usize);
        *fixed_pos += 4;
        if offset == 1 {
            return None;
        }
        if offset == 0 {
            return Some(T::view_empty());
        }
        Some(T::view_at(data, p + offset))
    }

    fn view_empty() -> Option<T::View> {
        None
    }

    fn view_to_owned(view: Option<T::View>) -> Option<T> {
        view.map(|v| T::view_to_owned(v))
    }
}

// ── [T; N] ──

impl<'a, T: FracViewType<'a>, const N: usize> FracViewType<'a> for [T; N] {
    type View = [T::View; N];

    fn view_at(data: &'a [u8], pos: u32) -> [T::View; N] {
        let mut fixed_pos = pos;
        std::array::from_fn(|_| T::view_embedded(data, &mut fixed_pos))
    }

    fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> [T::View; N] {
        if <T as Unpack>::VARIABLE_SIZE {
            // Variable-size elements: embedded as offset to array data
            let p = *fixed_pos as usize;
            let offset = read_u32_at(data, p);
            *fixed_pos += 4;
            let mut arr_pos = p as u32 + offset;
            std::array::from_fn(|_| T::view_embedded(data, &mut arr_pos))
        } else {
            // Fixed-size elements: inline
            std::array::from_fn(|_| T::view_embedded(data, fixed_pos))
        }
    }

    fn view_to_owned(view: [T::View; N]) -> [T; N] {
        view.map(|v| T::view_to_owned(v))
    }
}

// ── Mutation support ──

#[allow(dead_code)]
fn read_u16_at(data: &[u8], pos: usize) -> u16 {
    u16::from_le_bytes(data[pos..pos + 2].try_into().unwrap())
}

/// Write a u32 value in little-endian at the given position.
#[doc(hidden)]
pub fn write_u32_at(data: &mut [u8], pos: usize, val: u32) {
    data[pos..pos + 4].copy_from_slice(&val.to_le_bytes());
}

/// Splice `data`: replace `data[pos..pos+old_len]` with `new_data`.
/// Returns the size delta (new_len − old_len).
#[doc(hidden)]
pub fn splice_buffer(data: &mut Vec<u8>, pos: u32, old_len: u32, new_data: &[u8]) -> i32 {
    let pos = pos as usize;
    let old_len = old_len as usize;
    let new_len = new_data.len();
    let delta = new_len as i64 - old_len as i64;

    if delta > 0 {
        let grow = delta as usize;
        let old_total = data.len();
        data.resize(old_total + grow, 0);
        data.copy_within(pos + old_len..old_total, pos + new_len);
    } else if delta < 0 {
        data.copy_within(pos + old_len.., pos + new_len);
        data.truncate(data.len() - (-delta) as usize);
    }
    data[pos..pos + new_len].copy_from_slice(new_data);
    delta as i32
}

/// Adjust a u32 relative offset at `offset_pos` if its absolute target >= `after_old`.
/// Skips null/None markers (offset <= 1).
#[doc(hidden)]
pub fn patch_offset(data: &mut [u8], offset_pos: u32, after_old: u32, delta: i32) {
    let p = offset_pos as usize;
    let offset_val = read_u32_at(data, p);
    if offset_val <= 1 {
        return;
    }
    let abs_target = offset_pos + offset_val;
    if abs_target >= after_old {
        write_u32_at(data, p, (offset_val as i64 + delta as i64) as u32);
    }
}

/// Trait for types that support in-place mutation of fracpack data.
///
/// Provides [`packed_size_at`](FracMutViewType::packed_size_at) to measure an existing
/// packed value's size and [`replace_at`](FracMutViewType::replace_at) to splice a new
/// value at the same position.
pub trait FracMutViewType: crate::Pack + for<'a> Unpack<'a> {
    /// Total packed byte count of the value at position `pos`.
    fn packed_size_at(data: &[u8], pos: u32) -> u32;

    /// Replace the packed value at `pos` with `value`.
    /// Returns `(after_old, delta)` where `after_old` is the byte position just
    /// past the old data (before the splice) and `delta` is the size change.
    fn replace_at(data: &mut Vec<u8>, pos: u32, value: &Self) -> (u32, i32) {
        let old_size = Self::packed_size_at(data, pos);
        let new_packed = crate::Pack::packed(value);
        let after_old = pos + old_size;
        let delta = splice_buffer(data, pos, old_size, &new_packed);
        (after_old, delta)
    }

    /// Compute the end position of this field's heap data when embedded in a struct.
    /// `fixed_pos` is the field's position in the parent's fixed region.
    /// For most types this follows the offset and adds `packed_size_at`. For
    /// `Option<T>` the heap data is the inner `T`'s packed data.
    #[doc(hidden)]
    fn embedded_data_end(data: &[u8], fixed_pos: u32) -> u32 {
        if !<Self as crate::Pack>::VARIABLE_SIZE {
            fixed_pos + <Self as crate::Pack>::FIXED_SIZE
        } else {
            let offset = read_u32_at(data, fixed_pos as usize);
            if offset <= 1 {
                fixed_pos + 4
            } else {
                let target = fixed_pos + offset;
                target + Self::packed_size_at(data, target)
            }
        }
    }

    /// Replace a value embedded in a struct's fixed region at `fixed_pos`.
    /// Canonical mode: splices the buffer so the result is immediately canonical.
    /// Returns `(after_old, delta)` for the caller to patch sibling offsets.
    #[doc(hidden)]
    fn embedded_replace(data: &mut Vec<u8>, fixed_pos: u32, value: &Self) -> (u32, i32) {
        if !<Self as crate::Pack>::VARIABLE_SIZE {
            let packed = crate::Pack::packed(value);
            let p = fixed_pos as usize;
            data[p..p + packed.len()].copy_from_slice(&packed);
            (fixed_pos + <Self as crate::Pack>::FIXED_SIZE, 0)
        } else {
            let p = fixed_pos as usize;
            let offset = read_u32_at(data, p);
            assert!(
                offset > 1,
                "FracMutView: cannot mutate field with no heap allocation (offset={})",
                offset
            );
            let target = fixed_pos + offset;
            Self::replace_at(data, target, value)
        }
    }

    /// Replace a value embedded in a struct's fixed region at `fixed_pos`.
    /// Fast mode: overwrites in place if the new value fits, otherwise appends
    /// to the end of the buffer. Produces non-canonical but valid data;
    /// call `compact()` to restore canonical form after all edits.
    #[doc(hidden)]
    fn embedded_replace_fast(data: &mut Vec<u8>, fixed_pos: u32, value: &Self) {
        if !<Self as crate::Pack>::VARIABLE_SIZE {
            let packed = crate::Pack::packed(value);
            let p = fixed_pos as usize;
            data[p..p + packed.len()].copy_from_slice(&packed);
            return;
        }

        let p = fixed_pos as usize;
        let offset = read_u32_at(data, p);
        let new_packed = crate::Pack::packed(value);

        if offset > 1 {
            let target = fixed_pos + offset;
            let old_size = Self::packed_size_at(data, target);

            if new_packed.len() as u32 <= old_size {
                // Fits in existing space: overwrite in place (dead bytes may remain)
                let t = target as usize;
                data[t..t + new_packed.len()].copy_from_slice(&new_packed);
            } else {
                // Grow: append to end of buffer, update offset
                let new_pos = data.len() as u32;
                data.extend_from_slice(&new_packed);
                write_u32_at(data, p, new_pos - fixed_pos);
            }
        } else {
            // No existing heap data (offset 0 or 1): append to end
            let new_pos = data.len() as u32;
            data.extend_from_slice(&new_packed);
            write_u32_at(data, p, new_pos - fixed_pos);
        }
    }
}

// ── Built-in FracMutViewType impls ──

impl FracMutViewType for bool {
    fn packed_size_at(_: &[u8], _: u32) -> u32 {
        1
    }
    fn replace_at(data: &mut Vec<u8>, pos: u32, value: &Self) -> (u32, i32) {
        data[pos as usize] = *value as u8;
        (pos + 1, 0)
    }
}

macro_rules! scalar_mutview_impl {
    ($t:ty) => {
        impl FracMutViewType for $t {
            fn packed_size_at(_: &[u8], _: u32) -> u32 {
                std::mem::size_of::<$t>() as u32
            }
            fn replace_at(data: &mut Vec<u8>, pos: u32, value: &Self) -> (u32, i32) {
                let p = pos as usize;
                let sz = std::mem::size_of::<$t>();
                data[p..p + sz].copy_from_slice(&value.to_le_bytes());
                (pos + sz as u32, 0)
            }
        }
    };
}

scalar_mutview_impl!(u8);
scalar_mutview_impl!(u16);
scalar_mutview_impl!(u32);
scalar_mutview_impl!(u64);
scalar_mutview_impl!(i8);
scalar_mutview_impl!(i16);
scalar_mutview_impl!(i32);
scalar_mutview_impl!(i64);
scalar_mutview_impl!(f32);
scalar_mutview_impl!(f64);

impl FracMutViewType for String {
    fn packed_size_at(data: &[u8], pos: u32) -> u32 {
        4 + read_u32_at(data, pos as usize)
    }
}

impl<T: FracMutViewType> FracMutViewType for Vec<T> {
    fn packed_size_at(data: &[u8], pos: u32) -> u32 {
        4 + read_u32_at(data, pos as usize)
    }
}

impl<T: FracMutViewType> FracMutViewType for Option<T> {
    fn packed_size_at(data: &[u8], pos: u32) -> u32 {
        let offset = read_u32_at(data, pos as usize);
        if offset <= 1 {
            4
        } else {
            4 + T::packed_size_at(data, pos + offset)
        }
    }

    fn embedded_data_end(data: &[u8], fixed_pos: u32) -> u32 {
        let offset = read_u32_at(data, fixed_pos as usize);
        if offset <= 1 {
            fixed_pos + 4
        } else {
            let target = fixed_pos + offset;
            // Heap data is T's packed data, not Option<T>'s
            target + T::packed_size_at(data, target)
        }
    }

    fn embedded_replace(data: &mut Vec<u8>, fixed_pos: u32, value: &Self) -> (u32, i32) {
        let p = fixed_pos as usize;
        let old_offset = read_u32_at(data, p);

        match (old_offset > 1, value) {
            (true, Some(v)) => {
                // Some → Some: splice-replace inner data
                let target = fixed_pos + old_offset;
                T::replace_at(data, target, v)
            }
            (true, None) => {
                // Some → None: splice out old data, set offset to 1
                let target = fixed_pos + old_offset;
                let old_size = T::packed_size_at(data, target);
                let after_old = target + old_size;
                let delta = splice_buffer(data, target, old_size, &[]);
                write_u32_at(data, p, 1);
                (after_old, delta)
            }
            (false, None) => {
                // None/empty → None: noop
                if old_offset != 1 {
                    write_u32_at(data, p, 1);
                }
                (fixed_pos + 4, 0)
            }
            (false, Some(_)) => {
                panic!("FracMutView canonical: cannot set None to Some (no heap allocation)");
            }
        }
    }

    fn embedded_replace_fast(data: &mut Vec<u8>, fixed_pos: u32, value: &Self) {
        let p = fixed_pos as usize;
        let old_offset = read_u32_at(data, p);

        match (old_offset > 1, value) {
            (true, Some(v)) => {
                // Some → Some: overwrite or append
                let target = fixed_pos + old_offset;
                let old_size = T::packed_size_at(data, target);
                let new_packed = crate::Pack::packed(v);

                if new_packed.len() as u32 <= old_size {
                    let t = target as usize;
                    data[t..t + new_packed.len()].copy_from_slice(&new_packed);
                } else {
                    let new_pos = data.len() as u32;
                    data.extend_from_slice(&new_packed);
                    write_u32_at(data, p, new_pos - fixed_pos);
                }
            }
            (true, None) => {
                // Some → None: mark as None (old data becomes dead bytes)
                write_u32_at(data, p, 1);
            }
            (false, None) => {
                // None/empty → None: ensure offset is 1
                if old_offset != 1 {
                    write_u32_at(data, p, 1);
                }
            }
            (false, Some(v)) => {
                // None → Some: append T's packed data to end
                let new_packed = crate::Pack::packed(v);
                let new_pos = data.len() as u32;
                data.extend_from_slice(&new_packed);
                write_u32_at(data, p, new_pos - fixed_pos);
            }
        }
    }
}

impl<T: FracMutViewType, const N: usize> FracMutViewType for [T; N] {
    fn packed_size_at(data: &[u8], pos: u32) -> u32 {
        if !<T as crate::Pack>::VARIABLE_SIZE {
            <T as crate::Pack>::FIXED_SIZE * N as u32
        } else {
            let mut end = pos + <T as crate::Pack>::FIXED_SIZE * N as u32;
            for i in 0..N {
                let elem_fixed = pos + (i as u32) * <T as crate::Pack>::FIXED_SIZE;
                let offset = read_u32_at(data, elem_fixed as usize);
                if offset > 1 {
                    let target = elem_fixed + offset;
                    let elem_end = target + T::packed_size_at(data, target);
                    if elem_end > end {
                        end = elem_end;
                    }
                }
            }
            end - pos
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Pack;

    #[test]
    fn scalar_views() {
        assert_eq!(u32::view(&42u32.packed()), 42);
        assert_eq!(i16::view(&(-7i16).packed()), -7);
        assert_eq!(f64::view(&3.14f64.packed()), 3.14);
        assert_eq!(bool::view(&true.packed()), true);
        assert_eq!(bool::view(&false.packed()), false);
        assert_eq!(u8::view(&255u8.packed()), 255);
        assert_eq!(i64::view(&(-1i64).packed()), -1);
    }

    #[test]
    fn string_view() {
        assert_eq!(String::view(&"hello".to_string().packed()), "hello");
        assert_eq!(String::view(&"".to_string().packed()), "");
        assert_eq!(
            String::view(&"unicode: \u{1F600}".to_string().packed()),
            "unicode: \u{1F600}"
        );
    }

    #[test]
    fn vec_u32_view() {
        let data = vec![10u32, 20, 30].packed();
        let view = <Vec<u32>>::view(&data);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0), 10);
        assert_eq!(view.get(1), 20);
        assert_eq!(view.get(2), 30);
        assert!(!view.is_empty());

        let collected: Vec<u32> = view.iter().collect();
        assert_eq!(collected, vec![10, 20, 30]);
    }

    #[test]
    fn vec_empty_view() {
        let data = Vec::<u32>::new().packed();
        let view = <Vec<u32>>::view(&data);
        assert_eq!(view.len(), 0);
        assert!(view.is_empty());
    }

    #[test]
    fn vec_string_view() {
        let data = vec!["abc".to_string(), "def".to_string(), "ghi".to_string()].packed();
        let view = <Vec<String>>::view(&data);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0), "abc");
        assert_eq!(view.get(1), "def");
        assert_eq!(view.get(2), "ghi");
    }

    #[test]
    fn vec_for_loop() {
        let data = vec![5u32, 10, 15, 20].packed();
        let view = <Vec<u32>>::view(&data);
        let mut sum = 0u32;
        for val in view {
            sum += val;
        }
        assert_eq!(sum, 50);
    }

    #[test]
    fn option_some_none() {
        assert_eq!(<Option<u32>>::view(&Some(42u32).packed()), Some(42));
        assert_eq!(<Option<u32>>::view(&None::<u32>.packed()), None);
    }

    #[test]
    fn option_string() {
        assert_eq!(
            <Option<String>>::view(&Some("hello".to_string()).packed()),
            Some("hello")
        );
        assert_eq!(
            <Option<String>>::view(&None::<String>.packed()),
            None
        );
        assert_eq!(
            <Option<String>>::view(&Some("".to_string()).packed()),
            Some("")
        );
    }

    #[test]
    fn nested_option() {
        assert_eq!(
            <Option<Option<u32>>>::view(&Some(Some(42u32)).packed()),
            Some(Some(42))
        );
        assert_eq!(
            <Option<Option<u32>>>::view(&Some(None::<u32>).packed()),
            Some(None)
        );
        assert_eq!(
            <Option<Option<u32>>>::view(&None::<Option<u32>>.packed()),
            None
        );
    }

    #[test]
    fn fixed_array_view() {
        assert_eq!(<[i16; 3]>::view(&[1i16, 2, 3].packed()), [1, 2, 3]);
    }

    #[test]
    fn variable_array_view() {
        let data = ["ab".to_string(), "cd".to_string()].packed();
        let view = <[String; 2]>::view(&data);
        assert_eq!(view, ["ab", "cd"]);
    }

    #[test]
    fn vec_view_debug() {
        let data = vec![1u32, 2, 3].packed();
        let view = <Vec<u32>>::view(&data);
        let debug = format!("{:?}", view);
        assert_eq!(debug, "[1, 2, 3]");
    }

    #[test]
    fn vec_view_exact_size_iter() {
        let data = vec![1u32, 2, 3, 4].packed();
        let view = <Vec<u32>>::view(&data);
        let iter = view.iter();
        assert_eq!(iter.len(), 4);
    }

    #[test]
    fn option_vec_view() {
        let data = Some(vec![1u16, 2, 3]).packed();
        let view = <Option<Vec<u16>>>::view(&data);
        let vec_view = view.unwrap();
        assert_eq!(vec_view.len(), 3);
        assert_eq!(vec_view.get(0), 1);
        assert_eq!(vec_view.get(1), 2);
        assert_eq!(vec_view.get(2), 3);
    }

    #[test]
    fn validation_rejects_bad_data() {
        let result = u32::view_validated(&[1, 2, 3]); // too short
        assert!(result.is_err());
    }

    // ── Mutation tests ──

    #[test]
    fn splice_buffer_grow() {
        let mut buf = vec![1, 2, 3, 4, 5];
        let delta = splice_buffer(&mut buf, 2, 1, &[10, 11, 12]);
        assert_eq!(delta, 2);
        assert_eq!(buf, vec![1, 2, 10, 11, 12, 4, 5]);
    }

    #[test]
    fn splice_buffer_shrink() {
        let mut buf = vec![1, 2, 3, 4, 5, 6, 7];
        let delta = splice_buffer(&mut buf, 2, 3, &[10]);
        assert_eq!(delta, -2);
        assert_eq!(buf, vec![1, 2, 10, 6, 7]);
    }

    #[test]
    fn splice_buffer_same_size() {
        let mut buf = vec![1, 2, 3, 4, 5];
        let delta = splice_buffer(&mut buf, 1, 2, &[10, 11]);
        assert_eq!(delta, 0);
        assert_eq!(buf, vec![1, 10, 11, 4, 5]);
    }

    #[test]
    fn patch_offset_affected() {
        let mut data = vec![0; 20];
        write_u32_at(&mut data, 4, 10); // offset at 4, target = 14
        patch_offset(&mut data, 4, 12, 3); // after_old=12, delta=3
        assert_eq!(read_u32_at(&data, 4), 13); // 14 >= 12 → adjust
    }

    #[test]
    fn patch_offset_unaffected() {
        let mut data = vec![0; 20];
        write_u32_at(&mut data, 4, 5); // offset at 4, target = 9
        patch_offset(&mut data, 4, 12, 3); // 9 < 12 → no change
        assert_eq!(read_u32_at(&data, 4), 5);
    }

    #[test]
    fn patch_offset_skips_null() {
        let mut data = vec![0; 20];
        write_u32_at(&mut data, 4, 0);
        patch_offset(&mut data, 4, 0, 5);
        assert_eq!(read_u32_at(&data, 4), 0);

        write_u32_at(&mut data, 4, 1);
        patch_offset(&mut data, 4, 0, 5);
        assert_eq!(read_u32_at(&data, 4), 1);
    }

    #[test]
    fn scalar_replace_at() {
        let mut data = 42u32.packed();
        let (after, delta) = u32::replace_at(&mut data, 0, &99);
        assert_eq!(delta, 0);
        assert_eq!(after, 4);
        assert_eq!(u32::unpacked(&data).unwrap(), 99);
    }

    #[test]
    fn string_replace_at_shrink() {
        let mut data = "hello".to_string().packed();
        let old_len = data.len();
        let (after, delta) = String::replace_at(&mut data, 0, &"hi".to_string());
        assert_eq!(after, old_len as u32); // after_old = 4 + 5 = 9
        assert_eq!(delta, -3);
        assert_eq!(String::unpacked(&data).unwrap(), "hi");
    }

    #[test]
    fn string_replace_at_grow() {
        let mut data = "hi".to_string().packed();
        let old_len = data.len();
        let (after, delta) = String::replace_at(&mut data, 0, &"hello world".to_string());
        assert_eq!(after, old_len as u32); // after_old = 4 + 2 = 6
        assert_eq!(delta, 9);
        assert_eq!(String::unpacked(&data).unwrap(), "hello world");
    }

    #[test]
    fn vec_replace_at() {
        let mut data = vec![1u32, 2, 3].packed();
        let (_, delta) = <Vec<u32>>::replace_at(&mut data, 0, &vec![10, 20]);
        assert_eq!(delta, -4); // 3 elements → 2 elements, each 4 bytes
        assert_eq!(<Vec<u32>>::unpacked(&data).unwrap(), vec![10, 20]);
    }
}
