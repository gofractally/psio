//! WIT Canonical ABI zero-copy view.
//!
//! `WitView<'a>` provides zero-copy read access to WIT-encoded buffers.
//! Scalars are read directly from the buffer at their natural alignment.
//! Strings are returned as `&str` (UTF-8 validated).
//! Vectors provide an iterator over elements.

use super::layout::{align_up, WitLayout};
use std::marker::PhantomData;

/// Error type for WIT view operations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WitViewError {
    /// Buffer is too small for the expected type.
    BufferTooSmall {
        expected: u32,
        actual: u32,
    },
    /// A string pointer/length pair points outside the buffer.
    StringOutOfBounds {
        offset: u32,
        length: u32,
        buf_size: u32,
    },
    /// String data is not valid UTF-8.
    InvalidUtf8 {
        offset: u32,
        length: u32,
    },
    /// A list pointer/length pair points outside the buffer.
    ListOutOfBounds {
        offset: u32,
        count: u32,
        elem_size: u32,
        buf_size: u32,
    },
    /// A char value is not a valid Unicode scalar value.
    InvalidChar {
        value: u32,
    },
    /// A result/variant discriminant is out of range.
    InvalidDiscriminant {
        value: u32,
        max: u32,
    },
}

impl std::fmt::Display for WitViewError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::BufferTooSmall { expected, actual } => {
                write!(f, "buffer too small: need {} bytes, have {}", expected, actual)
            }
            Self::StringOutOfBounds { offset, length, buf_size } => {
                write!(f, "string at offset {} length {} exceeds buffer size {}", offset, length, buf_size)
            }
            Self::InvalidUtf8 { offset, length } => {
                write!(f, "invalid UTF-8 in string at offset {} length {}", offset, length)
            }
            Self::ListOutOfBounds { offset, count, elem_size, buf_size } => {
                write!(f, "list at offset {} count {} elem_size {} exceeds buffer size {}",
                    offset, count, elem_size, buf_size)
            }
            Self::InvalidChar { value } => {
                write!(f, "invalid Unicode scalar value: 0x{:X}", value)
            }
            Self::InvalidDiscriminant { value, max } => {
                write!(f, "invalid discriminant {} (max {})", value, max)
            }
        }
    }
}

impl std::error::Error for WitViewError {}

/// Zero-copy view over a WIT Canonical ABI encoded buffer.
///
/// The view borrows the buffer and provides typed field access.
/// The `base` pointer is the start of the entire buffer (for resolving
/// string/list offsets), and `rec` is the start of the current record.
#[derive(Clone, Copy)]
pub struct WitView<'a> {
    base: &'a [u8],
    rec_offset: u32,
}

impl<'a> WitView<'a> {
    /// Create a view rooted at the start of the buffer.
    pub fn from_buffer(buf: &'a [u8]) -> Self {
        Self {
            base: buf,
            rec_offset: 0,
        }
    }

    /// Create a view at a specific record offset within the buffer.
    pub fn at_offset(buf: &'a [u8], offset: u32) -> Self {
        Self {
            base: buf,
            rec_offset: offset,
        }
    }

    /// The underlying buffer.
    pub fn buffer(&self) -> &'a [u8] {
        self.base
    }

    /// The offset of this record within the buffer.
    pub fn offset(&self) -> u32 {
        self.rec_offset
    }

    // ── Scalar reads ────────────────────────────────────────────────────────

    fn read_at(&self, off: u32, len: u32) -> &'a [u8] {
        let start = (self.rec_offset + off) as usize;
        &self.base[start..start + len as usize]
    }

    /// Read a bool field at the given offset within this record.
    pub fn read_bool(&self, field_offset: u32) -> bool {
        self.base[(self.rec_offset + field_offset) as usize] != 0
    }

    /// Read a u8 field.
    pub fn read_u8(&self, field_offset: u32) -> u8 {
        self.base[(self.rec_offset + field_offset) as usize]
    }

    /// Read an i8 field.
    pub fn read_i8(&self, field_offset: u32) -> i8 {
        self.base[(self.rec_offset + field_offset) as usize] as i8
    }

    /// Read a u16 field.
    pub fn read_u16(&self, field_offset: u32) -> u16 {
        let bytes = self.read_at(field_offset, 2);
        u16::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an i16 field.
    pub fn read_i16(&self, field_offset: u32) -> i16 {
        self.read_u16(field_offset) as i16
    }

    /// Read a u32 field.
    pub fn read_u32(&self, field_offset: u32) -> u32 {
        let bytes = self.read_at(field_offset, 4);
        u32::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an i32 field.
    pub fn read_i32(&self, field_offset: u32) -> i32 {
        self.read_u32(field_offset) as i32
    }

    /// Read a u64 field.
    pub fn read_u64(&self, field_offset: u32) -> u64 {
        let bytes = self.read_at(field_offset, 8);
        u64::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an i64 field.
    pub fn read_i64(&self, field_offset: u32) -> i64 {
        self.read_u64(field_offset) as i64
    }

    /// Read an f32 field.
    pub fn read_f32(&self, field_offset: u32) -> f32 {
        let bytes = self.read_at(field_offset, 4);
        f32::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an f64 field.
    pub fn read_f64(&self, field_offset: u32) -> f64 {
        let bytes = self.read_at(field_offset, 8);
        f64::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read a char field at the given offset. Validates Unicode scalar value range.
    pub fn read_char(&self, field_offset: u32) -> Result<char, WitViewError> {
        let raw = self.read_u32(field_offset);
        char::from_u32(raw).ok_or(WitViewError::InvalidChar { value: raw })
    }

    // ── String read ─────────────────────────────────────────────────────────

    /// Read a string field at the given offset. Returns `&str`.
    pub fn read_string(&self, field_offset: u32) -> Result<&'a str, WitViewError> {
        let str_off = self.read_u32(field_offset);
        let str_len = self.read_u32(field_offset + 4);

        let end = str_off + str_len;
        if end as usize > self.base.len() {
            return Err(WitViewError::StringOutOfBounds {
                offset: str_off,
                length: str_len,
                buf_size: self.base.len() as u32,
            });
        }

        let bytes = &self.base[str_off as usize..end as usize];
        std::str::from_utf8(bytes).map_err(|_| WitViewError::InvalidUtf8 {
            offset: str_off,
            length: str_len,
        })
    }

    /// Read a string field, returning raw bytes without UTF-8 validation.
    pub fn read_string_bytes(&self, field_offset: u32) -> Result<&'a [u8], WitViewError> {
        let str_off = self.read_u32(field_offset);
        let str_len = self.read_u32(field_offset + 4);

        let end = str_off + str_len;
        if end as usize > self.base.len() {
            return Err(WitViewError::StringOutOfBounds {
                offset: str_off,
                length: str_len,
                buf_size: self.base.len() as u32,
            });
        }

        Ok(&self.base[str_off as usize..end as usize])
    }

    // ── List/Vec read ───────────────────────────────────────────────────────

    /// Read a list field, returning a `WitListView` for iterating elements.
    pub fn read_list<T: WitLayout>(&self, field_offset: u32) -> Result<WitListView<'a, T>, WitViewError> {
        let arr_off = self.read_u32(field_offset);
        let count = self.read_u32(field_offset + 4);
        let es = T::wit_size();

        let end = arr_off as u64 + count as u64 * es as u64;
        if end > self.base.len() as u64 {
            return Err(WitViewError::ListOutOfBounds {
                offset: arr_off,
                count,
                elem_size: es,
                buf_size: self.base.len() as u32,
            });
        }

        Ok(WitListView {
            base: self.base,
            arr_offset: arr_off,
            count,
            _marker: PhantomData,
        })
    }

    // ── Option read ─────────────────────────────────────────────────────────

    /// Read an optional discriminant at the given offset.
    /// Returns `true` if the value is `Some`.
    pub fn read_option_discriminant(&self, field_offset: u32) -> bool {
        self.read_u8(field_offset) != 0
    }

    /// Get a sub-view for an optional's payload. Call only if discriminant is 1.
    pub fn option_payload_view<T: WitLayout>(&self, field_offset: u32) -> WitView<'a> {
        let ea = T::wit_alignment();
        let payload_off = align_up(1, ea);
        WitView {
            base: self.base,
            rec_offset: self.rec_offset + field_offset + payload_off,
        }
    }

    // ── Result read ──────────────────────────────────────────────────────────

    /// Read a result discriminant at the given offset.
    /// Returns 0 for Ok, 1 for Err.
    pub fn read_result_discriminant(&self, field_offset: u32) -> u8 {
        self.read_u8(field_offset)
    }

    /// Get a sub-view for a result's payload.
    /// `T` and `E` are the Ok and Err types respectively.
    pub fn result_payload_view<T: WitLayout, E: WitLayout>(&self, field_offset: u32) -> WitView<'a> {
        let max_case_align = std::cmp::max(T::wit_alignment(), E::wit_alignment());
        let payload_off = align_up(1, max_case_align);
        WitView {
            base: self.base,
            rec_offset: self.rec_offset + field_offset + payload_off,
        }
    }

    // ── Variant read ────────────────────────────────────────────────────────

    /// Read a variant discriminant (u8/u16/u32 depending on case_count).
    pub fn read_variant_discriminant(&self, field_offset: u32, case_count: usize) -> u32 {
        let disc_sz = super::layout::discriminant_size(case_count);
        match disc_sz {
            1 => self.read_u8(field_offset) as u32,
            2 => self.read_u16(field_offset) as u32,
            _ => self.read_u32(field_offset),
        }
    }

    // ── Sub-record view ─────────────────────────────────────────────────────

    /// Get a sub-view for a nested struct at the given field offset.
    pub fn sub_view(&self, field_offset: u32) -> WitView<'a> {
        WitView {
            base: self.base,
            rec_offset: self.rec_offset + field_offset,
        }
    }
}

/// Zero-copy view over a contiguous list of WIT elements.
pub struct WitListView<'a, T: WitLayout> {
    pub(crate) base: &'a [u8],
    pub(crate) arr_offset: u32,
    pub(crate) count: u32,
    pub(crate) _marker: PhantomData<T>,
}

impl<'a, T: WitLayout> WitListView<'a, T> {
    /// Number of elements in the list.
    pub fn len(&self) -> u32 {
        self.count
    }

    /// Whether the list is empty.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Get a view over the element at index `i`.
    pub fn element_view(&self, i: u32) -> WitView<'a> {
        let es = T::wit_size();
        WitView {
            base: self.base,
            rec_offset: self.arr_offset + i * es,
        }
    }

    /// Iterator over element views.
    pub fn iter(&self) -> WitListIter<'a, T> {
        WitListIter {
            base: self.base,
            arr_offset: self.arr_offset,
            count: self.count,
            index: 0,
            _marker: PhantomData,
        }
    }
}

/// Iterator over `WitListView` elements. Holds the data directly
/// (not a reference to `WitListView`) to avoid lifetime issues.
pub struct WitListIter<'a, T: WitLayout> {
    base: &'a [u8],
    arr_offset: u32,
    count: u32,
    index: u32,
    _marker: PhantomData<T>,
}

impl<'a, T: WitLayout> Iterator for WitListIter<'a, T> {
    type Item = WitView<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.index < self.count {
            let es = T::wit_size();
            let view = WitView {
                base: self.base,
                rec_offset: self.arr_offset + self.index * es,
            };
            self.index += 1;
            Some(view)
        } else {
            None
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = (self.count - self.index) as usize;
        (remaining, Some(remaining))
    }
}

impl<'a, T: WitLayout> ExactSizeIterator for WitListIter<'a, T> {}

// ── WitUnpack trait: reconstruct owned values from a view ───────────────────

/// Trait for types that can be unpacked from WIT Canonical ABI bytes.
pub trait WitUnpack: Sized + WitLayout {
    /// Read this value from the view at field offset 0 (the root).
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError>;

    /// Unpack from a raw buffer (convenience).
    fn wit_unpack(buf: &[u8]) -> Result<Self, WitViewError> {
        let needed = Self::wit_size();
        if (buf.len() as u32) < needed {
            return Err(WitViewError::BufferTooSmall {
                expected: needed,
                actual: buf.len() as u32,
            });
        }
        let view = WitView::from_buffer(buf);
        Self::wit_unpack_from(&view)
    }
}

// ── Scalar unpack implementations ───────────────────────────────────────────

impl WitUnpack for bool {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_bool(0))
    }
}

impl WitUnpack for u8 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_u8(0))
    }
}

impl WitUnpack for i8 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_i8(0))
    }
}

impl WitUnpack for u16 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_u16(0))
    }
}

impl WitUnpack for i16 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_i16(0))
    }
}

impl WitUnpack for u32 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_u32(0))
    }
}

impl WitUnpack for i32 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_i32(0))
    }
}

impl WitUnpack for u64 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_u64(0))
    }
}

impl WitUnpack for i64 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_i64(0))
    }
}

impl WitUnpack for f32 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_f32(0))
    }
}

impl WitUnpack for f64 {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(view.read_f64(0))
    }
}

// ── char unpack ─────────────────────────────────────────────────────────────

impl WitUnpack for char {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        view.read_char(0)
    }
}

// ── String unpack ───────────────────────────────────────────────────────────

impl WitUnpack for String {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        view.read_string(0).map(|s| s.to_owned())
    }
}

// ── Vec<T> unpack ───────────────────────────────────────────────────────────

impl<T: WitUnpack> WitUnpack for Vec<T> {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        let list = view.read_list::<T>(0)?;
        let mut result = Vec::with_capacity(list.len() as usize);
        for elem_view in list.iter() {
            result.push(T::wit_unpack_from(&elem_view)?);
        }
        Ok(result)
    }
}

// ── Option<T> unpack ────────────────────────────────────────────────────────

impl<T: WitUnpack> WitUnpack for Option<T> {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        let disc = view.read_u8(0);
        if disc == 0 {
            Ok(None)
        } else {
            let payload_view = view.option_payload_view::<T>(0);
            Ok(Some(T::wit_unpack_from(&payload_view)?))
        }
    }
}

// ── Result<T, E> unpack ─────────────────────────────────────────────────────

impl<T: WitUnpack, E: WitUnpack> WitUnpack for Result<T, E> {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        let disc = view.read_u8(0);
        let payload_view = view.result_payload_view::<T, E>(0);
        match disc {
            0 => Ok(Ok(T::wit_unpack_from(&payload_view)?)),
            1 => Ok(Err(E::wit_unpack_from(&payload_view)?)),
            _ => Err(WitViewError::InvalidDiscriminant { value: disc as u32, max: 1 }),
        }
    }
}

// ── WitFlags unpack ─────────────────────────────────────────────────────────

impl<const N: usize> WitUnpack for super::layout::WitFlags<N> {
    fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
        match N {
            0 => Ok(super::layout::WitFlags::new()),
            1..=8 => {
                let raw = view.read_u8(0) as u32;
                Ok(super::layout::WitFlags::from_bits(vec![raw]))
            }
            9..=16 => {
                let raw = view.read_u16(0) as u32;
                Ok(super::layout::WitFlags::from_bits(vec![raw]))
            }
            _ => {
                let num_chunks = (N + 31) / 32;
                let mut bits = Vec::with_capacity(num_chunks);
                for i in 0..num_chunks {
                    bits.push(view.read_u32(i as u32 * 4));
                }
                Ok(super::layout::WitFlags::from_bits(bits))
            }
        }
    }
}

// ── Tuple unpack ────────────────────────────────────────────────────────────

impl WitUnpack for () {
    fn wit_unpack_from(_view: &WitView<'_>) -> Result<Self, WitViewError> {
        Ok(())
    }
}

macro_rules! impl_wit_unpack_tuple {
    ($(($T:ident, $idx:tt)),+) => {
        impl<$($T: WitUnpack),+> WitUnpack for ($($T,)+) {
            fn wit_unpack_from(view: &WitView<'_>) -> Result<Self, WitViewError> {
                let fields: &[(u32, u32)] = &[$(($T::wit_alignment(), $T::wit_size())),+];
                let (locs, _total, _align) = super::layout::compute_struct_layout(fields);
                let mut _i = 0;
                Ok(($(
                    {
                        let sub = view.sub_view(locs[{ let j = _i; _i += 1; j }].offset);
                        $T::wit_unpack_from(&sub)?
                    },
                )+))
            }
        }
    };
}

impl_wit_unpack_tuple!((A, 0));
impl_wit_unpack_tuple!((A, 0), (B, 1));
impl_wit_unpack_tuple!((A, 0), (B, 1), (C, 2));
impl_wit_unpack_tuple!((A, 0), (B, 1), (C, 2), (D, 3));
impl_wit_unpack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4));
impl_wit_unpack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (F, 5));
impl_wit_unpack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (F, 5), (G, 6));
impl_wit_unpack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (F, 5), (G, 6), (H, 7));

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wit::pack::WitPack;

    #[test]
    fn roundtrip_bool() {
        let buf = true.wit_pack();
        assert_eq!(bool::wit_unpack(&buf).unwrap(), true);

        let buf = false.wit_pack();
        assert_eq!(bool::wit_unpack(&buf).unwrap(), false);
    }

    #[test]
    fn roundtrip_u32() {
        let buf = 42u32.wit_pack();
        assert_eq!(u32::wit_unpack(&buf).unwrap(), 42);
    }

    #[test]
    fn roundtrip_i64() {
        let buf = (-12345i64).wit_pack();
        assert_eq!(i64::wit_unpack(&buf).unwrap(), -12345);
    }

    #[test]
    fn roundtrip_f64() {
        let buf = std::f64::consts::PI.wit_pack();
        assert_eq!(f64::wit_unpack(&buf).unwrap(), std::f64::consts::PI);
    }

    #[test]
    fn roundtrip_string() {
        let s = String::from("hello, world!");
        let buf = s.wit_pack();
        assert_eq!(String::wit_unpack(&buf).unwrap(), "hello, world!");
    }

    #[test]
    fn roundtrip_empty_string() {
        let s = String::new();
        let buf = s.wit_pack();
        assert_eq!(String::wit_unpack(&buf).unwrap(), "");
    }

    #[test]
    fn roundtrip_vec_u32() {
        let v: Vec<u32> = vec![10, 20, 30, 40];
        let buf = v.wit_pack();
        assert_eq!(Vec::<u32>::wit_unpack(&buf).unwrap(), vec![10, 20, 30, 40]);
    }

    #[test]
    fn roundtrip_vec_string() {
        let v: Vec<String> = vec!["alpha".into(), "beta".into(), "gamma".into()];
        let buf = v.wit_pack();
        let result = Vec::<String>::wit_unpack(&buf).unwrap();
        assert_eq!(result, vec!["alpha", "beta", "gamma"]);
    }

    #[test]
    fn roundtrip_option_none() {
        let v: Option<u32> = None;
        let buf = v.wit_pack();
        assert_eq!(Option::<u32>::wit_unpack(&buf).unwrap(), None);
    }

    #[test]
    fn roundtrip_option_some() {
        let v: Option<u32> = Some(99);
        let buf = v.wit_pack();
        assert_eq!(Option::<u32>::wit_unpack(&buf).unwrap(), Some(99));
    }

    #[test]
    fn roundtrip_option_string() {
        let v: Option<String> = Some("test".into());
        let buf = v.wit_pack();
        assert_eq!(Option::<String>::wit_unpack(&buf).unwrap(), Some("test".into()));

        let v: Option<String> = None;
        let buf = v.wit_pack();
        assert_eq!(Option::<String>::wit_unpack(&buf).unwrap(), None);
    }

    #[test]
    fn roundtrip_tuple() {
        let v: (u8, u32, String) = (0xFF, 42, "hello".into());
        let buf = v.wit_pack();
        let result = <(u8, u32, String)>::wit_unpack(&buf).unwrap();
        assert_eq!(result, (0xFF, 42, "hello".into()));
    }

    #[test]
    fn view_string_field() {
        let s = String::from("test");
        let buf = s.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert_eq!(view.read_string(0).unwrap(), "test");
    }

    #[test]
    fn view_list_iteration() {
        let v: Vec<u32> = vec![100, 200, 300];
        let buf = v.wit_pack();
        let view = WitView::from_buffer(&buf);
        let list = view.read_list::<u32>(0).unwrap();
        assert_eq!(list.len(), 3);

        let values: Vec<u32> = list.iter()
            .map(|ev| ev.read_u32(0))
            .collect();
        assert_eq!(values, vec![100, 200, 300]);
    }

    #[test]
    fn buffer_too_small() {
        let result = u32::wit_unpack(&[0, 0]);
        assert!(result.is_err());
        match result {
            Err(WitViewError::BufferTooSmall { expected: 4, actual: 2 }) => {}
            other => panic!("unexpected result: {:?}", other),
        }
    }
}
