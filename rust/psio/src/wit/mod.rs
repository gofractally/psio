//! WIT Canonical ABI: layout computation, pack, zero-copy view, in-place mutation.
//!
//! The WIT (WebAssembly Interface Types) Canonical ABI is the binary encoding
//! used at WASM component model boundaries. Key properties:
//!
//! - **Natural alignment**: fields aligned to their type's natural alignment
//! - **Scalars**: bool, u8/i8, u16/i16, u32/i32, u64/i64, f32, f64
//! - **char**: Unicode scalar value stored as u32 (4 bytes, align 4)
//! - **Strings**: stored as `(ptr: u32, len: u32)` with UTF-8 data out-of-line
//! - **Lists/vectors**: stored as `(ptr: u32, len: u32)` with elements contiguous
//! - **Options**: 1-byte discriminant + aligned payload
//! - **Results**: 1-byte discriminant (0=ok, 1=err) + aligned payload
//! - **Flags**: bitfield stored as u8/u16/u32 depending on count
//! - **Tuples**: record-like layout with positional fields
//! - **Booleans**: 1 byte (0 or 1), NOT bit-packed
//!
//! # Usage
//!
//! ```
//! use psio1::wit::pack::WitPack;
//! use psio1::wit::view::{WitUnpack, WitView};
//! use psio1::wit::mutation::WitMut;
//!
//! // Pack a value
//! let buf = 42u32.wit_pack();
//!
//! // Unpack (owned copy)
//! let val = u32::wit_unpack(&buf).unwrap();
//! assert_eq!(val, 42);
//!
//! // Zero-copy view
//! let view = WitView::from_buffer(&buf);
//! assert_eq!(view.read_u32(0), 42);
//!
//! // In-place mutation
//! let mut buf = buf;
//! let mut m = WitMut::from_buffer(&mut buf);
//! m.write_u32(0, 99);
//! assert_eq!(u32::wit_unpack(&buf).unwrap(), 99);
//! ```

pub mod layout;
pub mod mutation;
pub mod pack;
pub mod view;

#[cfg(test)]
mod conformance_tests;
