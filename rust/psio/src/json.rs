// Canonical JSON conversion for fracpack types.
//
// Rust's serde_json serializes u64/i64 as JSON numbers, which silently overflow
// JavaScript's Number.MAX_SAFE_INTEGER (2^53-1). Bytes become [1,2,3] arrays
// instead of hex strings. Enum tagging depends on #[serde(tag)] attributes which
// may not match the canonical {"CaseName": value} format. This module provides
// schema-aware canonical JSON matching all fracpack implementations.
//
// Canonical format rules:
// - u64/i64       -> JSON string (e.g., "12345678901234")
// - u8-u32, i8-i32 -> JSON number
// - f32/f64       -> JSON number; NaN/Inf -> null
// - bool          -> true/false
// - String        -> JSON string
// - Vec<u8>       -> lowercase hex string
// - Option<T>     -> null for None, recurse for Some
// - Variant/enum  -> single-key object {"CaseName": value}
// - Vec<T>        -> JSON array
// - Tuple         -> JSON array
// - Struct        -> JSON object with field names as keys

use serde_json::Value;

/// Error type for canonical JSON conversion.
#[derive(Debug)]
pub enum JsonError {
    /// Expected a certain JSON type but got something else.
    TypeMismatch {
        expected: &'static str,
        got: String,
    },
    /// Missing required field in a JSON object.
    MissingField(String),
    /// Invalid hex string.
    InvalidHex(String),
    /// Variant object must have exactly one key.
    BadVariantObject(usize),
    /// Unknown variant case name.
    UnknownVariant(String),
    /// Wrapped serde_json error.
    SerdeJson(serde_json::Error),
    /// Other error.
    Other(String),
}

impl std::fmt::Display for JsonError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            JsonError::TypeMismatch { expected, got } => {
                write!(f, "expected {}, got {}", expected, got)
            }
            JsonError::MissingField(name) => write!(f, "missing field: {}", name),
            JsonError::InvalidHex(msg) => write!(f, "invalid hex: {}", msg),
            JsonError::BadVariantObject(n) => {
                write!(f, "variant JSON must have exactly one key, got {}", n)
            }
            JsonError::UnknownVariant(name) => write!(f, "unknown variant case: {}", name),
            JsonError::SerdeJson(e) => write!(f, "serde_json error: {}", e),
            JsonError::Other(msg) => write!(f, "{}", msg),
        }
    }
}

impl std::error::Error for JsonError {}

impl From<serde_json::Error> for JsonError {
    fn from(e: serde_json::Error) -> Self {
        JsonError::SerdeJson(e)
    }
}

/// Convert a fracpack-serializable value to a canonical JSON `serde_json::Value`.
pub trait ToCanonicalJson {
    fn to_json_value(&self) -> Value;
}

/// Parse a canonical JSON `serde_json::Value` into a fracpack-serializable value.
pub trait FromCanonicalJson: Sized {
    fn from_json_value(v: &Value) -> Result<Self, JsonError>;
}

/// Convert a fracpack-serializable value to a canonical JSON string.
pub fn to_json<T: ToCanonicalJson>(value: &T) -> String {
    serde_json::to_string(&value.to_json_value()).expect("serde_json::to_string cannot fail on Value")
}

/// Parse a canonical JSON string into a fracpack-serializable value.
pub fn from_json<T: FromCanonicalJson>(json: &str) -> Result<T, JsonError> {
    let v: Value = serde_json::from_str(json)?;
    T::from_json_value(&v)
}

// ============================================================
// Hex encoding helpers
// ============================================================

fn to_hex(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for byte in data {
        s.push(HEX_CHARS[(byte >> 4) as usize]);
        s.push(HEX_CHARS[(byte & 0x0f) as usize]);
    }
    s
}

const HEX_CHARS: [char; 16] = [
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
];

fn from_hex_str(hex: &str) -> Result<Vec<u8>, JsonError> {
    if hex.len() % 2 != 0 {
        return Err(JsonError::InvalidHex("odd length".into()));
    }
    let mut result = Vec::with_capacity(hex.len() / 2);
    for i in (0..hex.len()).step_by(2) {
        let hi = u8::from_str_radix(&hex[i..i + 1], 16)
            .map_err(|_| JsonError::InvalidHex(format!("invalid char at {}", i)))?;
        let lo = u8::from_str_radix(&hex[i + 1..i + 2], 16)
            .map_err(|_| JsonError::InvalidHex(format!("invalid char at {}", i + 1)))?;
        result.push((hi << 4) | lo);
    }
    Ok(result)
}

// ============================================================
// Helper to describe a JSON value type
// ============================================================

fn json_type_name(v: &Value) -> String {
    match v {
        Value::Null => "null".into(),
        Value::Bool(_) => "bool".into(),
        Value::Number(_) => "number".into(),
        Value::String(_) => "string".into(),
        Value::Array(_) => "array".into(),
        Value::Object(_) => "object".into(),
    }
}

// ============================================================
// Primitive implementations
// ============================================================

impl ToCanonicalJson for bool {
    fn to_json_value(&self) -> Value {
        Value::Bool(*self)
    }
}

impl FromCanonicalJson for bool {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::Bool(b) => Ok(*b),
            _ => Err(JsonError::TypeMismatch {
                expected: "bool",
                got: json_type_name(v),
            }),
        }
    }
}

macro_rules! impl_small_int {
    ($t:ty, $expected:expr) => {
        impl ToCanonicalJson for $t {
            fn to_json_value(&self) -> Value {
                Value::Number(serde_json::Number::from(*self as i64))
            }
        }

        impl FromCanonicalJson for $t {
            fn from_json_value(v: &Value) -> Result<Self, JsonError> {
                match v {
                    Value::Number(n) => {
                        if let Some(i) = n.as_i64() {
                            Ok(i as $t)
                        } else if let Some(u) = n.as_u64() {
                            Ok(u as $t)
                        } else {
                            Err(JsonError::TypeMismatch {
                                expected: $expected,
                                got: format!("number {}", n),
                            })
                        }
                    }
                    Value::String(s) => {
                        s.parse::<$t>().map_err(|_| JsonError::TypeMismatch {
                            expected: $expected,
                            got: format!("string {:?}", s),
                        })
                    }
                    _ => Err(JsonError::TypeMismatch {
                        expected: $expected,
                        got: json_type_name(v),
                    }),
                }
            }
        }
    };
}

macro_rules! impl_small_uint {
    ($t:ty, $expected:expr) => {
        impl ToCanonicalJson for $t {
            fn to_json_value(&self) -> Value {
                Value::Number(serde_json::Number::from(*self as u64))
            }
        }

        impl FromCanonicalJson for $t {
            fn from_json_value(v: &Value) -> Result<Self, JsonError> {
                match v {
                    Value::Number(n) => {
                        if let Some(u) = n.as_u64() {
                            Ok(u as $t)
                        } else if let Some(i) = n.as_i64() {
                            Ok(i as $t)
                        } else {
                            Err(JsonError::TypeMismatch {
                                expected: $expected,
                                got: format!("number {}", n),
                            })
                        }
                    }
                    Value::String(s) => {
                        s.parse::<$t>().map_err(|_| JsonError::TypeMismatch {
                            expected: $expected,
                            got: format!("string {:?}", s),
                        })
                    }
                    _ => Err(JsonError::TypeMismatch {
                        expected: $expected,
                        got: json_type_name(v),
                    }),
                }
            }
        }
    };
}

impl_small_int!(i8, "i8");
impl_small_int!(i16, "i16");
impl_small_int!(i32, "i32");
impl_small_uint!(u8, "u8");
impl_small_uint!(u16, "u16");
impl_small_uint!(u32, "u32");

// u64/i64: serialize as JSON string to avoid JS 53-bit overflow
impl ToCanonicalJson for u64 {
    fn to_json_value(&self) -> Value {
        Value::String(self.to_string())
    }
}

impl FromCanonicalJson for u64 {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::String(s) => s.parse::<u64>().map_err(|_| JsonError::TypeMismatch {
                expected: "u64 string",
                got: format!("string {:?}", s),
            }),
            Value::Number(n) => {
                if let Some(u) = n.as_u64() {
                    Ok(u)
                } else if let Some(i) = n.as_i64() {
                    Ok(i as u64)
                } else {
                    Err(JsonError::TypeMismatch {
                        expected: "u64",
                        got: format!("number {}", n),
                    })
                }
            }
            _ => Err(JsonError::TypeMismatch {
                expected: "u64 string",
                got: json_type_name(v),
            }),
        }
    }
}

impl ToCanonicalJson for i64 {
    fn to_json_value(&self) -> Value {
        Value::String(self.to_string())
    }
}

impl FromCanonicalJson for i64 {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::String(s) => s.parse::<i64>().map_err(|_| JsonError::TypeMismatch {
                expected: "i64 string",
                got: format!("string {:?}", s),
            }),
            Value::Number(n) => {
                if let Some(i) = n.as_i64() {
                    Ok(i)
                } else if let Some(u) = n.as_u64() {
                    Ok(u as i64)
                } else {
                    Err(JsonError::TypeMismatch {
                        expected: "i64",
                        got: format!("number {}", n),
                    })
                }
            }
            _ => Err(JsonError::TypeMismatch {
                expected: "i64 string",
                got: json_type_name(v),
            }),
        }
    }
}

// f32/f64: NaN/Inf -> null
impl ToCanonicalJson for f32 {
    fn to_json_value(&self) -> Value {
        if self.is_nan() || self.is_infinite() {
            Value::Null
        } else {
            serde_json::Number::from_f64(*self as f64)
                .map(Value::Number)
                .unwrap_or(Value::Null)
        }
    }
}

impl FromCanonicalJson for f32 {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::Null => Ok(f32::NAN),
            Value::Number(n) => Ok(n.as_f64().unwrap_or(0.0) as f32),
            _ => Err(JsonError::TypeMismatch {
                expected: "f32 number",
                got: json_type_name(v),
            }),
        }
    }
}

impl ToCanonicalJson for f64 {
    fn to_json_value(&self) -> Value {
        if self.is_nan() || self.is_infinite() {
            Value::Null
        } else {
            serde_json::Number::from_f64(*self)
                .map(Value::Number)
                .unwrap_or(Value::Null)
        }
    }
}

impl FromCanonicalJson for f64 {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::Null => Ok(f64::NAN),
            Value::Number(n) => Ok(n.as_f64().unwrap_or(0.0)),
            _ => Err(JsonError::TypeMismatch {
                expected: "f64 number",
                got: json_type_name(v),
            }),
        }
    }
}

// String
impl ToCanonicalJson for String {
    fn to_json_value(&self) -> Value {
        Value::String(self.clone())
    }
}

impl FromCanonicalJson for String {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::String(s) => Ok(s.clone()),
            Value::Null => Ok(String::new()),
            _ => Err(JsonError::TypeMismatch {
                expected: "string",
                got: json_type_name(v),
            }),
        }
    }
}

// Vec<T> -> JSON array
impl<T: ToCanonicalJson> ToCanonicalJson for Vec<T> {
    fn to_json_value(&self) -> Value {
        Value::Array(self.iter().map(|item| item.to_json_value()).collect())
    }
}

impl<T: FromCanonicalJson> FromCanonicalJson for Vec<T> {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::Array(arr) => arr.iter().map(|item| T::from_json_value(item)).collect(),
            _ => Err(JsonError::TypeMismatch {
                expected: "array",
                got: json_type_name(v),
            }),
        }
    }
}

// Fixed-size arrays
impl<T: ToCanonicalJson, const N: usize> ToCanonicalJson for [T; N] {
    fn to_json_value(&self) -> Value {
        Value::Array(self.iter().map(|item| item.to_json_value()).collect())
    }
}

impl<T: FromCanonicalJson, const N: usize> FromCanonicalJson for [T; N] {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::Array(arr) => {
                if arr.len() != N {
                    return Err(JsonError::Other(format!(
                        "expected array of length {}, got {}",
                        N,
                        arr.len()
                    )));
                }
                let items: Vec<T> = arr
                    .iter()
                    .map(|item| T::from_json_value(item))
                    .collect::<Result<_, _>>()?;
                items.try_into().map_err(|_| {
                    JsonError::Other(format!(
                        "failed to convert to fixed array of length {}",
                        N
                    ))
                })
            }
            _ => Err(JsonError::TypeMismatch {
                expected: "array",
                got: json_type_name(v),
            }),
        }
    }
}

// Option<T>
impl<T: ToCanonicalJson> ToCanonicalJson for Option<T> {
    fn to_json_value(&self) -> Value {
        match self {
            None => Value::Null,
            Some(v) => v.to_json_value(),
        }
    }
}

impl<T: FromCanonicalJson> FromCanonicalJson for Option<T> {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        match v {
            Value::Null => Ok(None),
            _ => Ok(Some(T::from_json_value(v)?)),
        }
    }
}

// ============================================================
// Bytes (Vec<u8>) hex helpers — used by derive macros
// ============================================================

/// Convert bytes to lowercase hex string. Used by derive macros for Vec<u8> fields.
pub fn bytes_to_hex(data: &[u8]) -> String {
    to_hex(data)
}

/// Parse a hex string or byte array JSON value into Vec<u8>. Used by derive macros.
pub fn bytes_from_json_value(v: &Value) -> Result<Vec<u8>, JsonError> {
    match v {
        Value::String(s) => from_hex_str(s),
        Value::Array(arr) => arr.iter().map(|item| u8::from_json_value(item)).collect(),
        _ => Err(JsonError::TypeMismatch {
            expected: "hex string or byte array",
            got: json_type_name(v),
        }),
    }
}

// Tuples
macro_rules! impl_tuple {
    ($($T:ident $i:tt),+) => {
        impl<$($T: ToCanonicalJson),+> ToCanonicalJson for ($($T,)+) {
            fn to_json_value(&self) -> Value {
                Value::Array(vec![$(self.$i.to_json_value()),+])
            }
        }

        impl<$($T: FromCanonicalJson),+> FromCanonicalJson for ($($T,)+) {
            fn from_json_value(v: &Value) -> Result<Self, JsonError> {
                match v {
                    Value::Array(arr) => {
                        Ok(($($T::from_json_value(arr.get($i).unwrap_or(&Value::Null))?,)+))
                    }
                    _ => Err(JsonError::TypeMismatch {
                        expected: "array (tuple)",
                        got: json_type_name(v),
                    }),
                }
            }
        }
    };
}

impl_tuple!(T0 0);
impl_tuple!(T0 0, T1 1);
impl_tuple!(T0 0, T1 1, T2 2);
impl_tuple!(T0 0, T1 1, T2 2, T3 3);
impl_tuple!(T0 0, T1 1, T2 2, T3 3, T4 4);
impl_tuple!(T0 0, T1 1, T2 2, T3 3, T4 4, T5 5);
impl_tuple!(T0 0, T1 1, T2 2, T3 3, T4 4, T5 5, T6 6);

// Box<T> / Rc<T> / Arc<T>
impl<T: ToCanonicalJson> ToCanonicalJson for Box<T> {
    fn to_json_value(&self) -> Value {
        (**self).to_json_value()
    }
}
impl<T: FromCanonicalJson> FromCanonicalJson for Box<T> {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        Ok(Box::new(T::from_json_value(v)?))
    }
}

impl<T: ToCanonicalJson> ToCanonicalJson for std::rc::Rc<T> {
    fn to_json_value(&self) -> Value {
        (**self).to_json_value()
    }
}
impl<T: FromCanonicalJson> FromCanonicalJson for std::rc::Rc<T> {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        Ok(std::rc::Rc::new(T::from_json_value(v)?))
    }
}

impl<T: ToCanonicalJson> ToCanonicalJson for std::sync::Arc<T> {
    fn to_json_value(&self) -> Value {
        (**self).to_json_value()
    }
}
impl<T: FromCanonicalJson> FromCanonicalJson for std::sync::Arc<T> {
    fn from_json_value(v: &Value) -> Result<Self, JsonError> {
        Ok(std::sync::Arc::new(T::from_json_value(v)?))
    }
}

// ============================================================
// Struct / Enum helper types for derive macros
// ============================================================

/// Helper for building a JSON object from struct fields.
/// Used by derive macros.
pub struct JsonObjectBuilder {
    map: serde_json::Map<String, Value>,
}

impl JsonObjectBuilder {
    pub fn new() -> Self {
        Self {
            map: serde_json::Map::new(),
        }
    }

    pub fn field<T: ToCanonicalJson>(&mut self, name: &str, value: &T) {
        self.map.insert(name.to_string(), value.to_json_value());
    }

    /// Insert a pre-computed Value directly.
    pub fn field_value(&mut self, name: &str, value: Value) {
        self.map.insert(name.to_string(), value);
    }

    pub fn build(self) -> Value {
        Value::Object(self.map)
    }
}

/// Helper for reading fields from a JSON object.
/// Used by derive macros.
pub struct JsonObjectReader<'a> {
    map: &'a serde_json::Map<String, Value>,
}

impl<'a> JsonObjectReader<'a> {
    pub fn new(v: &'a Value) -> Result<Self, JsonError> {
        match v {
            Value::Object(map) => Ok(Self { map }),
            _ => Err(JsonError::TypeMismatch {
                expected: "object",
                got: json_type_name(v),
            }),
        }
    }

    pub fn field<T: FromCanonicalJson>(&self, name: &str) -> Result<T, JsonError> {
        let val = self.map.get(name).unwrap_or(&Value::Null);
        T::from_json_value(val)
    }

    /// Get the raw Value for a field.
    pub fn field_value(&self, name: &str) -> &Value {
        self.map.get(name).unwrap_or(&Value::Null)
    }
}

/// Helper for building a variant JSON value: {"CaseName": value}
pub fn variant_to_json<T: ToCanonicalJson>(case_name: &str, value: &T) -> Value {
    let mut map = serde_json::Map::new();
    map.insert(case_name.to_string(), value.to_json_value());
    Value::Object(map)
}

/// Helper for building a variant JSON value from a raw Value.
pub fn variant_to_json_value(case_name: &str, value: Value) -> Value {
    let mut map = serde_json::Map::new();
    map.insert(case_name.to_string(), value);
    Value::Object(map)
}

/// Helper to parse a variant JSON value: {"CaseName": value}
/// Returns the case name and the inner value.
pub fn variant_from_json(v: &Value) -> Result<(&str, &Value), JsonError> {
    match v {
        Value::Object(map) => {
            if map.len() != 1 {
                return Err(JsonError::BadVariantObject(map.len()));
            }
            let (key, val) = map.iter().next().unwrap();
            Ok((key.as_str(), val))
        }
        _ => Err(JsonError::TypeMismatch {
            expected: "variant object",
            got: json_type_name(v),
        }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_u64_as_string() {
        let v: u64 = 12345678901234;
        let json = v.to_json_value();
        assert_eq!(json, Value::String("12345678901234".into()));

        let back = u64::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_i64_as_string() {
        let v: i64 = -9223372036854775808;
        let json = v.to_json_value();
        assert_eq!(json, Value::String("-9223372036854775808".into()));

        let back = i64::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_u32_as_number() {
        let v: u32 = 4294967295;
        let json = v.to_json_value();
        assert!(json.is_number());
        assert_eq!(json, Value::Number(4294967295u64.into()));

        let back = u32::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_i32_as_number() {
        let v: i32 = -2147483648;
        let json = v.to_json_value();
        assert!(json.is_number());

        let back = i32::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_bytes_as_hex() {
        let v: Vec<u8> = vec![0xde, 0xad, 0xbe, 0xef];
        let hex = bytes_to_hex(&v);
        assert_eq!(hex, "deadbeef");

        let json = Value::String(hex);
        let back = bytes_from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_empty_bytes() {
        let v: Vec<u8> = vec![];
        let hex = bytes_to_hex(&v);
        assert_eq!(hex, "");

        let json = Value::String(hex);
        let back = bytes_from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_vec_u8_as_array() {
        // Vec<u8> through the generic Vec<T> trait produces an array of numbers
        let v: Vec<u8> = vec![1, 2, 3];
        let json = v.to_json_value();
        assert_eq!(
            json,
            Value::Array(vec![
                Value::Number(1.into()),
                Value::Number(2.into()),
                Value::Number(3.into()),
            ])
        );
    }

    #[test]
    fn test_option_none() {
        let v: Option<u32> = None;
        let json = v.to_json_value();
        assert_eq!(json, Value::Null);

        let back = Option::<u32>::from_json_value(&json).unwrap();
        assert_eq!(back, None);
    }

    #[test]
    fn test_option_some() {
        let v: Option<u32> = Some(42);
        let json = v.to_json_value();
        assert_eq!(json, Value::Number(42.into()));

        let back = Option::<u32>::from_json_value(&json).unwrap();
        assert_eq!(back, Some(42));
    }

    #[test]
    fn test_bool() {
        assert_eq!(true.to_json_value(), Value::Bool(true));
        assert_eq!(false.to_json_value(), Value::Bool(false));

        assert_eq!(bool::from_json_value(&Value::Bool(true)).unwrap(), true);
        assert_eq!(bool::from_json_value(&Value::Bool(false)).unwrap(), false);
    }

    #[test]
    fn test_string_roundtrip() {
        let v = String::from("hello world");
        let json = v.to_json_value();
        assert_eq!(json, Value::String("hello world".into()));

        let back = String::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_f64_normal() {
        let v: f64 = 3.14;
        let json = v.to_json_value();
        assert!(json.is_number());
        let back = f64::from_json_value(&json).unwrap();
        assert!((back - v).abs() < 1e-10);
    }

    #[test]
    fn test_f64_nan() {
        let v: f64 = f64::NAN;
        let json = v.to_json_value();
        assert_eq!(json, Value::Null);

        let back = f64::from_json_value(&json).unwrap();
        assert!(back.is_nan());
    }

    #[test]
    fn test_f64_inf() {
        let v: f64 = f64::INFINITY;
        let json = v.to_json_value();
        assert_eq!(json, Value::Null);
    }

    #[test]
    fn test_tuple_roundtrip() {
        let v: (u32, String) = (42, "hello".into());
        let json = v.to_json_value();
        assert_eq!(
            json,
            Value::Array(vec![
                Value::Number(42.into()),
                Value::String("hello".into()),
            ])
        );

        let back = <(u32, String)>::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_vec_roundtrip() {
        let v: Vec<u32> = vec![1, 2, 3];
        let json = v.to_json_value();
        assert_eq!(
            json,
            Value::Array(vec![
                Value::Number(1.into()),
                Value::Number(2.into()),
                Value::Number(3.into()),
            ])
        );

        let back = Vec::<u32>::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_fixed_array_roundtrip() {
        let v: [u32; 3] = [10, 20, 30];
        let json = v.to_json_value();
        assert_eq!(
            json,
            Value::Array(vec![
                Value::Number(10.into()),
                Value::Number(20.into()),
                Value::Number(30.into()),
            ])
        );

        let back = <[u32; 3]>::from_json_value(&json).unwrap();
        assert_eq!(back, v);
    }

    #[test]
    fn test_variant_helpers() {
        let json = variant_to_json("Uint32", &42u32);
        assert_eq!(
            json,
            Value::Object(serde_json::Map::from_iter([(
                "Uint32".to_string(),
                Value::Number(42.into()),
            )]))
        );

        let (name, val) = variant_from_json(&json).unwrap();
        assert_eq!(name, "Uint32");
        assert_eq!(u32::from_json_value(val).unwrap(), 42);
    }

    #[test]
    fn test_to_json_string() {
        let v: u64 = 18446744073709551615;
        let json_str = to_json(&v);
        assert_eq!(json_str, "\"18446744073709551615\"");
    }

    #[test]
    fn test_from_json_string() {
        let v: u64 = from_json("\"18446744073709551615\"").unwrap();
        assert_eq!(v, u64::MAX);
    }

    #[test]
    fn test_hex_roundtrip() {
        let data = vec![0x00, 0x01, 0x0a, 0xff];
        let hex = to_hex(&data);
        assert_eq!(hex, "00010aff");
        let back = from_hex_str(&hex).unwrap();
        assert_eq!(back, data);
    }

    #[test]
    fn test_nested_option() {
        let v: Option<Option<u32>> = Some(None);
        let json = v.to_json_value();
        assert_eq!(json, Value::Null);

        // Roundtrip: Some(None) becomes None since null maps to None
        let back = Option::<Option<u32>>::from_json_value(&json).unwrap();
        assert_eq!(back, None);
    }

    #[test]
    fn test_u64_from_number() {
        // Accept number as well as string for u64
        let json = Value::Number(42u64.into());
        let v = u64::from_json_value(&json).unwrap();
        assert_eq!(v, 42);
    }

    #[test]
    fn test_json_object_builder() {
        let mut b = JsonObjectBuilder::new();
        b.field("x", &42u32);
        b.field("name", &String::from("hello"));
        let json = b.build();

        let reader = JsonObjectReader::new(&json).unwrap();
        assert_eq!(reader.field::<u32>("x").unwrap(), 42);
        assert_eq!(reader.field::<String>("name").unwrap(), "hello");
    }
}
