//! Cap'n Proto deserialization (unpacking).
//!
//! Reads Cap'n Proto wire data into owned Rust values.
//! The `CapnpUnpack` trait provides `capnp_unpack` which takes a
//! flat-array message and returns an owned struct.

use crate::capnp::layout::FieldLoc;
use crate::capnp::view::CapnpView;
use crate::capnp::wire;

/// A type that can be deserialized from Cap'n Proto wire format.
pub trait CapnpUnpack: Sized {
    /// Unpack from a CapnpView (pre-resolved struct within a segment).
    fn unpack_from(view: &CapnpView) -> Self;

    /// Unpack from a complete flat-array message.
    fn capnp_unpack(msg: &[u8]) -> Result<Self, &'static str> {
        let view = CapnpView::from_message(msg)?;
        Ok(Self::unpack_from(&view))
    }
}

// ── Scalar unpack helpers ─────────────────────────────────────────────

/// Read a bool from a view at the given location.
pub fn unpack_bool(view: &CapnpView, loc: &FieldLoc) -> bool {
    view.read_bool(loc)
}

/// Read a u8 from a view at the given location.
pub fn unpack_u8(view: &CapnpView, loc: &FieldLoc) -> u8 {
    view.read_u8(loc)
}

pub fn unpack_i8(view: &CapnpView, loc: &FieldLoc) -> i8 {
    view.read_i8(loc)
}

pub fn unpack_u16(view: &CapnpView, loc: &FieldLoc) -> u16 {
    view.read_u16(loc)
}

pub fn unpack_i16(view: &CapnpView, loc: &FieldLoc) -> i16 {
    view.read_i16(loc)
}

pub fn unpack_u32(view: &CapnpView, loc: &FieldLoc) -> u32 {
    view.read_u32(loc)
}

pub fn unpack_i32(view: &CapnpView, loc: &FieldLoc) -> i32 {
    view.read_i32(loc)
}

pub fn unpack_u64(view: &CapnpView, loc: &FieldLoc) -> u64 {
    view.read_u64(loc)
}

pub fn unpack_i64(view: &CapnpView, loc: &FieldLoc) -> i64 {
    view.read_i64(loc)
}

pub fn unpack_f32(view: &CapnpView, loc: &FieldLoc) -> f32 {
    view.read_f32(loc)
}

pub fn unpack_f64(view: &CapnpView, loc: &FieldLoc) -> f64 {
    view.read_f64(loc)
}

/// Read a String from a text pointer field.
pub fn unpack_string(view: &CapnpView, loc: &FieldLoc) -> String {
    view.read_text(loc).to_owned()
}

/// Unpack a nested struct.
pub fn unpack_struct<T: CapnpUnpack>(view: &CapnpView, loc: &FieldLoc) -> T {
    let child = view.read_struct(loc);
    T::unpack_from(&child)
}

/// Unpack a vector of scalars (u8, u16, u32, u64, i8, i16, i32, i64, f32, f64).
pub fn unpack_scalar_vec<T: Copy>(
    view: &CapnpView,
    loc: &FieldLoc,
    _elem_size: usize,
    read_fn: fn(&[u8], usize) -> T,
) -> Vec<T> {
    match view.read_list(loc) {
        None => Vec::new(),
        Some((segment, info)) => {
            let mut result = Vec::with_capacity(info.count as usize);
            for i in 0..info.count as usize {
                let off = info.data_offset + i * info.elem_stride as usize;
                result.push(read_fn(segment, off));
            }
            result
        }
    }
}

/// Unpack a vector of booleans.
pub fn unpack_bool_vec(view: &CapnpView, loc: &FieldLoc) -> Vec<bool> {
    match view.read_list(loc) {
        None => Vec::new(),
        Some((segment, info)) => {
            let mut result = Vec::with_capacity(info.count as usize);
            for i in 0..info.count as usize {
                let byte_off = info.data_offset + i / 8;
                let bit = i % 8;
                result.push((segment[byte_off] >> bit) & 1 != 0);
            }
            result
        }
    }
}

/// Unpack a vector of strings.
pub fn unpack_string_vec(view: &CapnpView, loc: &FieldLoc) -> Vec<String> {
    match view.read_list(loc) {
        None => Vec::new(),
        Some((segment, info)) => {
            let mut result = Vec::with_capacity(info.count as usize);
            for i in 0..info.count as usize {
                let ptr_offset = info.data_offset + i * 8;
                let s = wire::read_text(segment, ptr_offset);
                result.push(s.to_owned());
            }
            result
        }
    }
}

/// Unpack a vector of structs.
pub fn unpack_struct_vec<T: CapnpUnpack>(view: &CapnpView, loc: &FieldLoc) -> Vec<T> {
    match view.read_list(loc) {
        None => Vec::new(),
        Some((segment, info)) => {
            let mut result = Vec::with_capacity(info.count as usize);
            for i in 0..info.count as usize {
                let elem_offset = info.data_offset + i * info.elem_stride as usize;
                let elem_view = CapnpView::from_parts(
                    segment,
                    wire::CapnpPtr {
                        data_offset: elem_offset,
                        data_words: info.elem_data_words,
                        ptr_count: info.elem_ptr_count,
                    },
                );
                result.push(T::unpack_from(&elem_view));
            }
            result
        }
    }
}

// Helper readers for unpack_scalar_vec
pub fn read_u8_at(data: &[u8], off: usize) -> u8 {
    data[off]
}
pub fn read_i8_at(data: &[u8], off: usize) -> i8 {
    data[off] as i8
}
pub fn read_u16_at(data: &[u8], off: usize) -> u16 {
    wire::read_u16(data, off)
}
pub fn read_i16_at(data: &[u8], off: usize) -> i16 {
    wire::read_u16(data, off) as i16
}
pub fn read_u32_at(data: &[u8], off: usize) -> u32 {
    wire::read_u32(data, off)
}
pub fn read_i32_at(data: &[u8], off: usize) -> i32 {
    wire::read_u32(data, off) as i32
}
pub fn read_u64_at(data: &[u8], off: usize) -> u64 {
    wire::read_u64(data, off)
}
pub fn read_i64_at(data: &[u8], off: usize) -> i64 {
    wire::read_u64(data, off) as i64
}
pub fn read_f32_at(data: &[u8], off: usize) -> f32 {
    f32::from_bits(wire::read_u32(data, off))
}
pub fn read_f64_at(data: &[u8], off: usize) -> f64 {
    f64::from_bits(wire::read_u64(data, off))
}
