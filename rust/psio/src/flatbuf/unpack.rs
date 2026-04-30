//! FlatBuffer deserialization — reads FlatBuffer bytes into owned Rust values.
//!
//! The `FbUnpack` trait provides `fb_unpack` which reads from a complete
//! FlatBuffer byte buffer and returns an owned Rust value. For composite types,
//! fields are read from the vtable-driven table structure.

use super::view::{FbVecView, FbView};

/// Error type for FlatBuffer unpacking.
#[derive(Debug, Clone)]
pub enum FbUnpackError {
    /// Buffer is too small or malformed.
    InvalidBuffer(String),
    /// A string field contains invalid UTF-8.
    InvalidUtf8,
    /// A required field is missing.
    MissingField(&'static str),
}

impl std::fmt::Display for FbUnpackError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FbUnpackError::InvalidBuffer(msg) => write!(f, "invalid FlatBuffer: {}", msg),
            FbUnpackError::InvalidUtf8 => write!(f, "invalid UTF-8 in FlatBuffer string"),
            FbUnpackError::MissingField(name) => {
                write!(f, "missing required field: {}", name)
            }
        }
    }
}

impl std::error::Error for FbUnpackError {}

/// Trait for types that can be deserialized from FlatBuffer format.
///
/// For scalar types wrapped in a single-field table, this reads slot 0.
/// For composite types, a manual or derive-generated impl reads each field.
pub trait FbUnpack: Sized {
    /// Deserialize from a complete FlatBuffer byte buffer.
    fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError>;

    /// Deserialize from a view pointing at a specific table.
    fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError>;
}

// ── Scalar impls ─────────────────────────────────────────────────────────

macro_rules! impl_fb_unpack_scalar {
    ($ty:ty, $read_method:ident, $default:expr) => {
        impl FbUnpack for $ty {
            fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
                let view = FbView::from_buffer(data)
                    .map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
                Ok(view.$read_method(0, $default))
            }

            fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
                Ok(view.$read_method(0, $default))
            }
        }
    };
}

impl_fb_unpack_scalar!(u8, read_u8, 0);
impl_fb_unpack_scalar!(i8, read_i8, 0);
impl_fb_unpack_scalar!(u16, read_u16, 0);
impl_fb_unpack_scalar!(i16, read_i16, 0);
impl_fb_unpack_scalar!(u32, read_u32, 0);
impl_fb_unpack_scalar!(i32, read_i32, 0);
impl_fb_unpack_scalar!(u64, read_u64, 0);
impl_fb_unpack_scalar!(i64, read_i64, 0);
impl_fb_unpack_scalar!(f32, read_f32, 0.0);
impl_fb_unpack_scalar!(f64, read_f64, 0.0);
impl_fb_unpack_scalar!(bool, read_bool, false);

impl FbUnpack for String {
    fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
        let view =
            FbView::from_buffer(data).map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
        Self::fb_unpack_view(&view)
    }

    fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
        Ok(view.read_str_or_empty(0).to_string())
    }
}

impl<T: FbUnpackElement> FbUnpack for Vec<T> {
    fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
        let view =
            FbView::from_buffer(data).map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
        Self::fb_unpack_view(&view)
    }

    fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
        match view.read_vec(0) {
            Some(vec_view) => T::fb_unpack_vec(&vec_view),
            None => Ok(Vec::new()),
        }
    }
}

impl<T: FbUnpack> FbUnpack for Option<T> {
    fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
        let view =
            FbView::from_buffer(data).map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
        Self::fb_unpack_view(&view)
    }

    fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
        if view.has_field(0) {
            Ok(Some(T::fb_unpack_view(view)?))
        } else {
            Ok(None)
        }
    }
}

// ── FbUnpackElement — helper for vector element deserialization ──────────

/// Trait for types that can be deserialized from FlatBuffer vector elements.
pub trait FbUnpackElement: Sized {
    /// Deserialize a vector of elements from a FbVecView.
    fn fb_unpack_vec(vec_view: &FbVecView<'_>) -> Result<Vec<Self>, FbUnpackError>;
}

macro_rules! impl_fb_unpack_element_scalar {
    ($ty:ty, $get_method:ident) => {
        impl FbUnpackElement for $ty {
            fn fb_unpack_vec(vec_view: &FbVecView<'_>) -> Result<Vec<Self>, FbUnpackError> {
                let mut result = Vec::with_capacity(vec_view.len() as usize);
                for i in 0..vec_view.len() {
                    result.push(vec_view.$get_method(i));
                }
                Ok(result)
            }
        }
    };
}

impl_fb_unpack_element_scalar!(u8, get_u8);
impl_fb_unpack_element_scalar!(i8, get_i8);
impl_fb_unpack_element_scalar!(u16, get_u16);
impl_fb_unpack_element_scalar!(i16, get_i16);
impl_fb_unpack_element_scalar!(u32, get_u32);
impl_fb_unpack_element_scalar!(i32, get_i32);
impl_fb_unpack_element_scalar!(u64, get_u64);
impl_fb_unpack_element_scalar!(i64, get_i64);
impl_fb_unpack_element_scalar!(f32, get_f32);
impl_fb_unpack_element_scalar!(f64, get_f64);

impl FbUnpackElement for bool {
    fn fb_unpack_vec(vec_view: &FbVecView<'_>) -> Result<Vec<Self>, FbUnpackError> {
        let mut result = Vec::with_capacity(vec_view.len() as usize);
        for i in 0..vec_view.len() {
            result.push(vec_view.get_bool(i));
        }
        Ok(result)
    }
}

impl FbUnpackElement for String {
    fn fb_unpack_vec(vec_view: &FbVecView<'_>) -> Result<Vec<Self>, FbUnpackError> {
        let mut result = Vec::with_capacity(vec_view.len() as usize);
        for i in 0..vec_view.len() {
            match vec_view.get_str(i) {
                Some(s) => result.push(s.to_string()),
                None => return Err(FbUnpackError::InvalidUtf8),
            }
        }
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flatbuf::builder::FbPack;

    #[test]
    fn flatbuf_roundtrip_u32() {
        let val: u32 = 12345;
        let data = val.fb_pack();
        let result = u32::fb_unpack(&data).unwrap();
        assert_eq!(result, val);
    }

    #[test]
    fn flatbuf_roundtrip_zero_default() {
        // Zero matches the default, so the field will be absent
        let val: u32 = 0;
        let data = val.fb_pack();
        let result = u32::fb_unpack(&data).unwrap();
        assert_eq!(result, val);
    }

    #[test]
    fn flatbuf_roundtrip_string() {
        let val = "hello flatbuf".to_string();
        let data = val.fb_pack();
        let result = String::fb_unpack(&data).unwrap();
        assert_eq!(result, val);
    }

    #[test]
    fn flatbuf_roundtrip_empty_string() {
        let val = "".to_string();
        let data = val.fb_pack();
        let result = String::fb_unpack(&data).unwrap();
        assert_eq!(result, val);
    }

    #[test]
    fn flatbuf_roundtrip_bool() {
        for &v in &[true, false] {
            let data = v.fb_pack();
            let result = bool::fb_unpack(&data).unwrap();
            assert_eq!(result, v);
        }
    }

    #[test]
    fn flatbuf_roundtrip_i64() {
        let val: i64 = -9876543210;
        let data = val.fb_pack();
        let result = i64::fb_unpack(&data).unwrap();
        assert_eq!(result, val);
    }

    #[test]
    fn flatbuf_roundtrip_f64() {
        let val: f64 = 3.14159265358979;
        let data = val.fb_pack();
        let result = f64::fb_unpack(&data).unwrap();
        assert_eq!(result, val);
    }
}
