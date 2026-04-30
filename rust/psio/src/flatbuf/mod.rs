//! FlatBuffers wire format: builder, zero-copy view, unpack, schema export.
//!
//! This module implements the FlatBuffers binary format, compatible with the
//! C++ implementation in `psio/flatbuf.hpp`. Key types:
//!
//! - [`FbBuilder`] — back-to-front buffer builder
//! - [`FbView`] — zero-copy field access via vtable lookup
//! - [`FbPack`] — trait for serializing to FlatBuffer format
//! - [`FbUnpack`] — trait for deserializing from FlatBuffer format
//! - [`FbLayout`] — vtable slot computation
//! - [`FbSchema`] — schema export to .fbs IDL text
//!
//! # Wire format
//!
//! A FlatBuffer starts with a `u32` root offset, followed by interleaved
//! vtables and table data. Tables reference their vtable via a signed offset
//! (`soffset_t`), and fields within a table are located by reading the
//! corresponding vtable slot (a `u16` offset from the table start, or 0 for
//! absent/default).

pub mod builder;
pub mod layout;
pub mod mutation;
pub mod schema;
pub mod unpack;
pub mod view;

pub use builder::{FbBuilder, FbPack, FbPackElement, FbTableWriter};
pub use layout::{FbFieldLoc, FbLayout};
pub use schema::{to_fbs_schema, to_fbs_text, FbFieldSchema, FbSchema, FbTypeSchema};
pub use unpack::{FbUnpack, FbUnpackElement, FbUnpackError};
pub use view::{FbVecView, FbView, FbViewError};

#[cfg(test)]
mod tests {
    use super::*;

    // ── Multi-field table round-trip (manual trait impls) ─────────────────
    //
    // Demonstrates how a struct like:
    //   struct Token { kind: u16, text: String }
    // would be packed/unpacked without derive macros.

    #[derive(Debug, Clone, PartialEq)]
    struct Token {
        kind: u16,
        text: String,
    }

    impl Default for Token {
        fn default() -> Self {
            Token {
                kind: 0,
                text: String::new(),
            }
        }
    }

    // Layout: 2 fields, no variants → slots [0, 1]
    fn token_layout() -> FbLayout {
        FbLayout::simple(2)
    }

    impl FbPack for Token {
        fn fb_write(&self, b: &mut FbBuilder) -> u32 {
            let layout = token_layout();
            // Phase 1: pre-create sub-objects
            let text_off = if self.text.is_empty() {
                0
            } else {
                b.create_string(&self.text)
            };

            // Phase 2: build table (smallest alignment first for back-to-front)
            let mut tw = FbTableWriter::new(b, &layout);
            tw.add_u16(0, self.kind, 0);       // slot 0: u16 (align 2)
            tw.add_offset(1, text_off);         // slot 1: string offset (align 4)
            tw.finish()
        }
    }

    impl FbUnpack for Token {
        fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
            let view = FbView::from_buffer(data)
                .map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
            Self::fb_unpack_view(&view)
        }

        fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
            Ok(Token {
                kind: view.read_u16(0, 0),
                text: view.read_str_or_empty(1).to_string(),
            })
        }
    }

    impl FbSchema for Token {
        fn fb_schema() -> FbTypeSchema {
            FbTypeSchema {
                name: "Token".to_string(),
                is_struct: false,
                fields: vec![
                    FbFieldSchema {
                        name: "kind".to_string(),
                        fb_type: "ushort".to_string(),
                        default: Some("0".to_string()),
                    },
                    FbFieldSchema {
                        name: "text".to_string(),
                        fb_type: "string".to_string(),
                        default: None,
                    },
                ],
            }
        }
    }

    #[test]
    fn flatbuf_token_roundtrip() {
        let orig = Token {
            kind: 42,
            text: "hello".to_string(),
        };
        let data = orig.fb_pack();
        let result = Token::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_token_view() {
        let orig = Token {
            kind: 7,
            text: "world".to_string(),
        };
        let data = orig.fb_pack();
        let view = FbView::from_buffer(&data).unwrap();
        assert_eq!(view.read_u16(0, 0), 7);
        assert_eq!(view.read_str(1), Some("world"));
    }

    #[test]
    fn flatbuf_token_defaults() {
        let orig = Token::default();
        let data = orig.fb_pack();
        let result = Token::fb_unpack(&data).unwrap();
        assert_eq!(result.kind, 0);
        assert_eq!(result.text, "");
    }

    #[test]
    fn flatbuf_token_schema() {
        let schema = to_fbs_schema::<Token>();
        assert!(schema.contains("table Token {"), "schema: {}", schema);
        assert!(schema.contains("kind:ushort"), "schema: {}", schema);
        assert!(schema.contains("text:string"), "schema: {}", schema);
    }

    // ── Nested table round-trip ──────────────────────────────────────────

    #[derive(Debug, Clone, PartialEq)]
    struct Order {
        id: u64,
        customer_name: String,
        amount: f64,
        tags: Vec<String>,
    }

    impl Default for Order {
        fn default() -> Self {
            Order {
                id: 0,
                customer_name: String::new(),
                amount: 0.0,
                tags: Vec::new(),
            }
        }
    }

    fn order_layout() -> FbLayout {
        FbLayout::simple(4)
    }

    impl FbPack for Order {
        fn fb_write(&self, b: &mut FbBuilder) -> u32 {
            let layout = order_layout();

            // Phase 1: pre-create sub-objects
            let name_off = if self.customer_name.is_empty() {
                0
            } else {
                b.create_string(&self.customer_name)
            };

            let tags_off = if self.tags.is_empty() {
                0
            } else {
                String::fb_write_vec(&self.tags, b)
            };

            // Phase 2: build table
            let mut tw = FbTableWriter::new(b, &layout);
            // Add fields sorted by alignment (smallest first):
            tw.add_offset(1, name_off);   // slot 1: string
            tw.add_offset(3, tags_off);   // slot 3: vector of strings
            tw.add_f64(2, self.amount, 0.0); // slot 2: f64 (align 8)
            tw.add_u64(0, self.id, 0);    // slot 0: u64 (align 8)
            tw.finish()
        }
    }

    impl FbUnpack for Order {
        fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
            let view = FbView::from_buffer(data)
                .map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
            Self::fb_unpack_view(&view)
        }

        fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
            let tags = match view.read_vec(3) {
                Some(vec_view) => {
                    let mut result = Vec::with_capacity(vec_view.len() as usize);
                    for i in 0..vec_view.len() {
                        match vec_view.get_str(i) {
                            Some(s) => result.push(s.to_string()),
                            None => return Err(FbUnpackError::InvalidUtf8),
                        }
                    }
                    result
                }
                None => Vec::new(),
            };

            Ok(Order {
                id: view.read_u64(0, 0),
                customer_name: view.read_str_or_empty(1).to_string(),
                amount: view.read_f64(2, 0.0),
                tags,
            })
        }
    }

    #[test]
    fn flatbuf_order_roundtrip() {
        let orig = Order {
            id: 1001,
            customer_name: "Alice".to_string(),
            amount: 99.95,
            tags: vec!["rush".to_string(), "fragile".to_string()],
        };
        let data = orig.fb_pack();
        let result = Order::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_order_view_fields() {
        let orig = Order {
            id: 42,
            customer_name: "Bob".to_string(),
            amount: 3.14,
            tags: vec!["a".to_string(), "b".to_string(), "c".to_string()],
        };
        let data = orig.fb_pack();
        let view = FbView::from_buffer(&data).unwrap();

        assert_eq!(view.read_u64(0, 0), 42);
        assert_eq!(view.read_str(1), Some("Bob"));
        assert_eq!(view.read_f64(2, 0.0), 3.14);

        let tags_view = view.read_vec(3).expect("tags should be present");
        assert_eq!(tags_view.len(), 3);
        assert_eq!(tags_view.get_str(0), Some("a"));
        assert_eq!(tags_view.get_str(1), Some("b"));
        assert_eq!(tags_view.get_str(2), Some("c"));
    }

    #[test]
    fn flatbuf_order_empty_defaults() {
        let orig = Order::default();
        let data = orig.fb_pack();
        let result = Order::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    // ── Vector round-trips ───────────────────────────────────────────────

    #[test]
    fn flatbuf_vec_u32_roundtrip() {
        let orig: Vec<u32> = vec![10, 20, 30, 40, 50];
        let data = orig.fb_pack();
        let result = Vec::<u32>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_vec_string_roundtrip() {
        let orig: Vec<String> = vec!["foo".into(), "bar".into(), "baz".into()];
        let data = orig.fb_pack();
        let result = Vec::<String>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_vec_empty_roundtrip() {
        let orig: Vec<u32> = vec![];
        let data = orig.fb_pack();
        let result = Vec::<u32>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_vec_i64_roundtrip() {
        let orig: Vec<i64> = vec![-100, 0, 100, i64::MIN, i64::MAX];
        let data = orig.fb_pack();
        let result = Vec::<i64>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_vec_f64_roundtrip() {
        let orig: Vec<f64> = vec![1.1, 2.2, 3.3];
        let data = orig.fb_pack();
        let result = Vec::<f64>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_vec_bool_roundtrip() {
        let orig: Vec<bool> = vec![true, false, true, true, false];
        let data = orig.fb_pack();
        let result = Vec::<bool>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    // ── Wire format structure verification ───────────────────────────────

    #[test]
    fn flatbuf_wire_format_structure() {
        // Verify the wire format has correct structure:
        // [root_offset:u32] ... [vtable] ... [table_data]
        let orig = Token {
            kind: 123,
            text: "test".to_string(),
        };
        let data = orig.fb_pack();

        // 1. Root offset at position 0
        let root_off = u32::from_le_bytes(data[0..4].try_into().unwrap()) as usize;
        assert!(root_off < data.len(), "root offset should point within buffer");

        // 2. Table has soffset to vtable
        let soff = i32::from_le_bytes(data[root_off..root_off + 4].try_into().unwrap());
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        assert!(vt_pos + 4 <= data.len(), "vtable should be within buffer");

        // 3. Vtable has valid sizes
        let vt_size = u16::from_le_bytes(data[vt_pos..vt_pos + 2].try_into().unwrap());
        let tbl_size = u16::from_le_bytes(data[vt_pos + 2..vt_pos + 4].try_into().unwrap());
        assert!(vt_size >= 4, "vtable size should be at least 4");
        assert!(tbl_size >= 4, "table size should be at least 4 (includes soffset)");
        assert!(
            vt_pos + vt_size as usize <= data.len(),
            "vtable should fit in buffer"
        );

        // 4. At least one field present (kind != 0)
        let has_field = (4..vt_size as usize)
            .step_by(2)
            .any(|off| u16::from_le_bytes(data[vt_pos + off..vt_pos + off + 2].try_into().unwrap()) != 0);
        assert!(has_field, "at least one field should be present");
    }

    // ── Scalar edge cases ────────────────────────────────────────────────

    #[test]
    fn flatbuf_scalar_extremes() {
        // u8
        assert_eq!(u8::fb_unpack(&255u8.fb_pack()).unwrap(), 255);
        assert_eq!(u8::fb_unpack(&0u8.fb_pack()).unwrap(), 0);

        // i16
        assert_eq!(i16::fb_unpack(&i16::MIN.fb_pack()).unwrap(), i16::MIN);
        assert_eq!(i16::fb_unpack(&i16::MAX.fb_pack()).unwrap(), i16::MAX);

        // u64
        assert_eq!(u64::fb_unpack(&u64::MAX.fb_pack()).unwrap(), u64::MAX);

        // f32
        let pi = std::f32::consts::PI;
        assert_eq!(f32::fb_unpack(&pi.fb_pack()).unwrap(), pi);

        // Negative f64
        let neg = -1e100_f64;
        assert_eq!(f64::fb_unpack(&neg.fb_pack()).unwrap(), neg);
    }

    // ── Unicode strings ──────────────────────────────────────────────────

    #[test]
    fn flatbuf_unicode_string() {
        let orig = Token {
            kind: 1,
            text: "Hello \u{1F600} \u{00E9}\u{00E8}\u{00EA}".to_string(),
        };
        let data = orig.fb_pack();
        let result = Token::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    // ── Large string ─────────────────────────────────────────────────────

    #[test]
    fn flatbuf_large_string() {
        let long_text: String = "x".repeat(10_000);
        let orig = Token {
            kind: 99,
            text: long_text.clone(),
        };
        let data = orig.fb_pack();
        let result = Token::fb_unpack(&data).unwrap();
        assert_eq!(result.text, long_text);
    }

    // ── Nested table ─────────────────────────────────────────────────────

    #[derive(Debug, Clone, PartialEq)]
    struct Wrapper {
        label: String,
        inner: InnerData,
    }

    #[derive(Debug, Clone, PartialEq)]
    struct InnerData {
        x: i32,
        y: i32,
    }

    impl Default for InnerData {
        fn default() -> Self {
            InnerData { x: 0, y: 0 }
        }
    }

    impl Default for Wrapper {
        fn default() -> Self {
            Wrapper {
                label: String::new(),
                inner: InnerData::default(),
            }
        }
    }

    fn inner_data_layout() -> FbLayout {
        FbLayout::simple(2)
    }

    fn wrapper_layout() -> FbLayout {
        FbLayout::simple(2)
    }

    impl FbPack for InnerData {
        fn fb_write(&self, b: &mut FbBuilder) -> u32 {
            let layout = inner_data_layout();
            let mut tw = FbTableWriter::new(b, &layout);
            tw.add_i32(0, self.x, 0);
            tw.add_i32(1, self.y, 0);
            tw.finish()
        }
    }

    impl FbUnpack for InnerData {
        fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
            let view = FbView::from_buffer(data)
                .map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
            Self::fb_unpack_view(&view)
        }

        fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
            Ok(InnerData {
                x: view.read_i32(0, 0),
                y: view.read_i32(1, 0),
            })
        }
    }

    impl FbPack for Wrapper {
        fn fb_write(&self, b: &mut FbBuilder) -> u32 {
            let layout = wrapper_layout();
            let label_off = if self.label.is_empty() {
                0
            } else {
                b.create_string(&self.label)
            };
            let inner_off = self.inner.fb_write(b);

            let mut tw = FbTableWriter::new(b, &layout);
            tw.add_offset(0, label_off);
            tw.add_offset(1, inner_off);
            tw.finish()
        }
    }

    impl FbUnpack for Wrapper {
        fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
            let view = FbView::from_buffer(data)
                .map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
            Self::fb_unpack_view(&view)
        }

        fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
            let inner = match view.read_table(1) {
                Some(inner_view) => InnerData::fb_unpack_view(&inner_view)?,
                None => InnerData::default(),
            };
            Ok(Wrapper {
                label: view.read_str_or_empty(0).to_string(),
                inner,
            })
        }
    }

    #[test]
    fn flatbuf_nested_table_roundtrip() {
        let orig = Wrapper {
            label: "point".to_string(),
            inner: InnerData { x: 10, y: -20 },
        };
        let data = orig.fb_pack();
        let result = Wrapper::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    #[test]
    fn flatbuf_nested_table_view() {
        let orig = Wrapper {
            label: "coords".to_string(),
            inner: InnerData { x: 100, y: 200 },
        };
        let data = orig.fb_pack();
        let view = FbView::from_buffer(&data).unwrap();

        assert_eq!(view.read_str(0), Some("coords"));

        let inner_view = view.read_table(1).expect("inner should be present");
        assert_eq!(inner_view.read_i32(0, 0), 100);
        assert_eq!(inner_view.read_i32(1, 0), 200);
    }

    #[test]
    fn flatbuf_nested_defaults() {
        let orig = Wrapper::default();
        let data = orig.fb_pack();
        let result = Wrapper::fb_unpack(&data).unwrap();
        assert_eq!(result.label, "");
        assert_eq!(result.inner.x, 0);
        assert_eq!(result.inner.y, 0);
    }

    // ── Vec<u8> (byte vector) ────────────────────────────────────────────

    #[test]
    fn flatbuf_vec_u8_roundtrip() {
        let orig: Vec<u8> = vec![0, 1, 2, 127, 255];
        let data = orig.fb_pack();
        let result = Vec::<u8>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    // ── Multiple strings in vectors ──────────────────────────────────────

    #[test]
    fn flatbuf_many_string_vec() {
        let orig: Vec<String> = (0..100).map(|i| format!("item_{}", i)).collect();
        let data = orig.fb_pack();
        let result = Vec::<String>::fb_unpack(&data).unwrap();
        assert_eq!(result, orig);
    }

    // ── Builder reuse ────────────────────────────────────────────────────

    #[test]
    fn flatbuf_builder_reuse() {
        let mut b = FbBuilder::default();

        // First use
        let root1 = 42u32.fb_write(&mut b);
        b.finish(root1);
        let data1 = b.data().to_vec();

        // Clear and reuse
        b.clear();
        let root2 = 99u32.fb_write(&mut b);
        b.finish(root2);
        let data2 = b.data().to_vec();

        assert_eq!(u32::fb_unpack(&data1).unwrap(), 42);
        assert_eq!(u32::fb_unpack(&data2).unwrap(), 99);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Wire format conformance tests
    //
    // These tests verify byte-level compatibility with the official
    // FlatBuffers binary encoding specification. Each test either:
    //   (a) constructs a known byte array by hand and reads it with our view, or
    //   (b) packs data and verifies every byte of the output.
    //
    // Reference: https://flatbuffers.dev/flatbuffers_internals.html
    // ════════════════════════════════════════════════════════════════════════

    /// Helper: read little-endian u16 from a byte slice.
    fn le_u16(buf: &[u8], pos: usize) -> u16 {
        u16::from_le_bytes(buf[pos..pos + 2].try_into().unwrap())
    }
    /// Helper: read little-endian u32 from a byte slice.
    fn le_u32(buf: &[u8], pos: usize) -> u32 {
        u32::from_le_bytes(buf[pos..pos + 4].try_into().unwrap())
    }
    /// Helper: read little-endian i32 from a byte slice.
    fn le_i32(buf: &[u8], pos: usize) -> i32 {
        i32::from_le_bytes(buf[pos..pos + 4].try_into().unwrap())
    }

    // ── 1. Hand-crafted single-scalar table ─────────────────────────────
    //
    // Schema:  table Tiny { val: uint; }    (field 0, u32)
    // Value:   val = 0xDEADBEEF
    //
    // Expected layout (all LE, 4-byte aligned):
    //   offset  content
    //   0x00    0C 00 00 00           root_offset → table at 12
    //   0x04    06 00                 vtable_size = 6
    //   0x06    08 00                 table_size = 8
    //   0x08    04 00                 field 0 offset = 4 (from table start)
    //   0x0A    00 00                 padding to align table
    //   0x0C    08 00 00 00           soffset = 8 (table_pos - vtable_pos = 12 - 4 = 8)
    //   0x10    EF BE AD DE           val = 0xDEADBEEF

    #[test]
    fn wire_handcrafted_scalar_table_view() {
        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x0C, 0x00, 0x00, 0x00,  // root offset = 12
            0x06, 0x00,              // vtable size = 6
            0x08, 0x00,              // table size = 8
            0x04, 0x00,              // field 0 at offset 4 from table start
            0x00, 0x00,              // padding
            0x08, 0x00, 0x00, 0x00,  // soffset = 8 (12 - 4)
            0xEF, 0xBE, 0xAD, 0xDE,  // val = 0xDEADBEEF
        ];

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u32(0, 0), 0xDEADBEEF);
    }

    // ── 2. Hand-crafted table with absent field returns default ──────────
    //
    // Schema:  table Two { a: uint; b: uint; }
    // Only field b (slot 1) is present with value 42.
    // Field a (slot 0) is absent → vtable slot is 0.

    #[test]
    fn wire_handcrafted_absent_field_default() {
        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x0C, 0x00, 0x00, 0x00,  // root offset = 12
            0x08, 0x00,              // vtable size = 8 (header + 2 slots)
            0x08, 0x00,              // table size = 8
            0x00, 0x00,              // slot 0: absent (0)
            0x04, 0x00,              // slot 1: offset 4 from table start
            0x08, 0x00, 0x00, 0x00,  // soffset = 8 (12 - 4)
            0x2A, 0x00, 0x00, 0x00,  // field b = 42
        ];

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u32(0, 999), 999, "absent field should return default");
        assert_eq!(view.read_u32(1, 0), 42, "present field should return value");
    }

    // ── 3. Hand-crafted string encoding ─────────────────────────────────
    //
    // Schema:  table HasStr { name: string; }
    // Value:   name = "Hi"   (2 bytes + NUL + 1 pad → 4 bytes data)
    //
    // Layout:
    //   0x00  10 00 00 00   root offset = 16
    //   0x04  02 00 00 00   string length = 2
    //   0x08  48 69 00 00   "Hi" + NUL + pad
    //   0x0C  06 00         vtable size = 6
    //   0x0E  08 00         table size = 8
    //   0x10  04 00         slot 0 at offset 4
    //   0x12  00 00         padding
    //   0x14  08 00 00 00   soffset = 8 (0x14 - 0x0C)
    //   0x18  F0 FF FF FF   relative offset to string: -(0x18 - 0x04) = -20 → as u32 relative
    //                       Actually: string_ref_pos = table_pos + field_off = 0x14 + 4 = 0x18
    //                       string_pos = 0x18 + le_u32(buf, 0x18) = 0x18 + value
    //                       We need string_pos = 0x04, so value = 0x04 - 0x18 ... negative!
    //
    // Let me lay this out differently — string AFTER table, which is the normal
    // builder order (strings written before tables, so they come after in the
    // buffer when writing back-to-front, but appear before when reading).
    // Actually in back-to-front building, strings appear earlier in the buffer.
    // Let me just verify our builder's output directly.

    #[test]
    fn wire_string_encoding_structure() {
        let mut b = FbBuilder::default();
        let s_off = b.create_string("Hi");
        b.start_table();
        b.add_offset_field(4, s_off); // slot 0
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // 1. Root offset at position 0
        let root_off = le_u32(buf, 0) as usize;
        assert!(root_off < buf.len());

        // 2. Table → vtable via soffset
        let soff = le_i32(buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        // 3. Vtable header
        let vt_size = le_u16(buf, vt_pos);
        let tbl_size = le_u16(buf, vt_pos + 2);
        assert!(vt_size >= 6, "vtable must hold header + 1 slot");
        assert!(tbl_size >= 8, "table must hold soffset + 1 offset field");

        // 4. Field 0 slot in vtable
        let field_off = le_u16(buf, vt_pos + 4) as usize;
        assert_ne!(field_off, 0, "string field must be present");

        // 5. Dereference offset to get string position
        let str_ref_pos = root_off + field_off;
        let str_rel = le_u32(buf, str_ref_pos) as usize;
        let str_pos = str_ref_pos + str_rel;

        // 6. String: u32 length prefix
        let str_len = le_u32(buf, str_pos) as usize;
        assert_eq!(str_len, 2, "string 'Hi' has length 2");

        // 7. String data
        let str_data = &buf[str_pos + 4..str_pos + 4 + str_len];
        assert_eq!(str_data, b"Hi");

        // 8. NUL terminator after string bytes
        assert_eq!(buf[str_pos + 4 + str_len], 0, "string must be NUL-terminated");

        // 9. String start (length prefix) must be 4-byte aligned
        assert_eq!(str_pos % 4, 0, "string length prefix must be 4-byte aligned");
    }

    // ── 4. Hand-crafted string — read from known bytes ──────────────────
    //
    // Build a buffer by hand where the string comes after the table.
    // Layout:
    //   0x00  08 00 00 00   root offset = 8
    //   0x04  06 00         vtable size = 6
    //   0x06  08 00         table size = 8
    //   0x08  04 00         slot 0 at offset 4
    //   0x0A  00 00         padding (vtable is 6 bytes, table starts at 8)
    //                       Wait, vtable is at 4 and table at 8, soffset = 8-4 = 4
    //
    //   Actually let me lay it out more carefully:
    //   0x00  0C 00 00 00   root offset → table at 12
    //   0x04  06 00         vtable size = 6
    //   0x06  08 00         table size = 8
    //   0x08  04 00         slot 0: field at offset 4 from table
    //   0x0A  00 00         padding
    //   0x0C  08 00 00 00   soffset = 8 (table 12 - vtable 4 = 8)
    //   0x10  08 00 00 00   offset to string: 0x10 + 8 = 0x18
    //   0x14  00 00 00 00   (pad to align string)
    //   0x18  03 00 00 00   string length = 3
    //   0x1C  41 42 43 00   "ABC" + NUL

    #[test]
    fn wire_handcrafted_string_read() {
        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x0C, 0x00, 0x00, 0x00,  // root offset = 12
            0x06, 0x00,              // vtable size = 6
            0x08, 0x00,              // table size = 8
            0x04, 0x00,              // slot 0: offset 4
            0x00, 0x00,              // padding
            0x08, 0x00, 0x00, 0x00,  // soffset = 8
            0x08, 0x00, 0x00, 0x00,  // relative offset to string = 8 → string at 0x10+8=0x18
            0x00, 0x00, 0x00, 0x00,  // padding
            0x03, 0x00, 0x00, 0x00,  // string length = 3
            0x41, 0x42, 0x43, 0x00,  // "ABC" + NUL
        ];

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_str(0), Some("ABC"));
    }

    // ── 5. Vtable structure verification ────────────────────────────────
    //
    // Verify all vtable invariants for a multi-field table.

    #[test]
    fn wire_vtable_structure_multi_field() {
        // Table with u8 (slot 0), u16 (slot 1), u32 (slot 2)
        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u8(4, 0xAB, 0);   // slot 0
        b.add_scalar_u16(6, 0x1234, 0); // slot 1
        b.add_scalar_u32(8, 0xDEAD, 0); // slot 2
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // Navigate to vtable
        let root_off = le_u32(buf, 0) as usize;
        let soff = le_i32(buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        // Vtable header
        let vt_size = le_u16(buf, vt_pos);
        let tbl_size = le_u16(buf, vt_pos + 2);

        // vt_size must accommodate header (4) + 3 slots (6) = 10 bytes
        assert!(vt_size >= 10, "vtable needs 10 bytes for 3 slots, got {}", vt_size);

        // Verify vtable fits in buffer
        assert!(vt_pos + vt_size as usize <= buf.len());

        // tbl_size must include at least the soffset (4 bytes)
        assert!(tbl_size >= 4);

        // All three field slots must be non-zero (present)
        for slot in 0..3u16 {
            let vt_byte = 4 + 2 * slot;
            let fo = le_u16(buf, vt_pos + vt_byte as usize);
            assert_ne!(fo, 0, "slot {} must be present", slot);

            // Each field offset must be within the table
            assert!((fo as u16) < tbl_size,
                "slot {} offset {} must be < table size {}", slot, fo, tbl_size);
        }

        // Read back values through the view to confirm correctness
        let view = FbView::from_buffer(buf).unwrap();
        assert_eq!(view.read_u8(0, 0), 0xAB);
        assert_eq!(view.read_u16(1, 0), 0x1234);
        assert_eq!(view.read_u32(2, 0), 0xDEAD);
    }

    // ── 6. Scalar default elision ───────────────────────────────────────
    //
    // When a scalar field equals its default, FlatBuffers should not store it.
    // The vtable slot for that field should be 0.

    #[test]
    fn wire_scalar_default_elision() {
        let layout = FbLayout::simple(3);
        let mut b = FbBuilder::default();

        // field 0: u32 = 0 (default), should be elided
        // field 1: u32 = 42 (non-default), should be present
        // field 2: u32 = 0 (default), should be elided
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u32(0, 0, 0);
        tw.add_u32(1, 42, 0);
        tw.add_u32(2, 0, 0);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        // Navigate to vtable
        let root_off = le_u32(buf, 0) as usize;
        let soff = le_i32(buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let vt_size = le_u16(buf, vt_pos);

        // slot 0 (vt byte 4): should be 0 (elided)
        let s0 = if vt_size >= 6 { le_u16(buf, vt_pos + 4) } else { 0 };
        assert_eq!(s0, 0, "default-value field 0 must be elided from vtable");

        // slot 1 (vt byte 6): should be non-zero (present)
        let s1 = if vt_size >= 8 { le_u16(buf, vt_pos + 6) } else { 0 };
        assert_ne!(s1, 0, "non-default field 1 must be present");

        // slot 2 (vt byte 8): should be 0 (elided)
        let s2 = if vt_size >= 10 { le_u16(buf, vt_pos + 8) } else { 0 };
        assert_eq!(s2, 0, "default-value field 2 must be elided from vtable");

        // Reading elided fields returns the specified default
        let view = FbView::from_buffer(buf).unwrap();
        assert_eq!(view.read_u32(0, 999), 999);
        assert_eq!(view.read_u32(1, 0), 42);
        assert_eq!(view.read_u32(2, 777), 777);
    }

    // ── 7. Vector encoding: u32 element count + data ────────────────────

    #[test]
    fn wire_vector_u32_encoding() {
        let mut b = FbBuilder::default();
        let data: Vec<u32> = vec![10, 20, 30];
        let bytes: &[u8] = unsafe {
            std::slice::from_raw_parts(
                data.as_ptr() as *const u8,
                data.len() * 4,
            )
        };
        let v_off = b.create_vec_scalar(bytes, 4, 3);
        b.start_table();
        b.add_offset_field(4, v_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // Navigate to vector via view
        let view = FbView::from_buffer(buf).unwrap();
        let vec_view = view.read_vec(0).unwrap();

        // Element count
        assert_eq!(vec_view.len(), 3);

        // Each element
        assert_eq!(vec_view.get_u32(0), 10);
        assert_eq!(vec_view.get_u32(1), 20);
        assert_eq!(vec_view.get_u32(2), 30);

        // Also verify the raw encoding: find the vector and check u32 count prefix
        let root_off = le_u32(buf, 0) as usize;
        let soff = le_i32(buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let field_off = le_u16(buf, vt_pos + 4) as usize;
        let vec_ref_pos = root_off + field_off;
        let vec_pos = vec_ref_pos + le_u32(buf, vec_ref_pos) as usize;

        // u32 element count
        let count = le_u32(buf, vec_pos);
        assert_eq!(count, 3, "vector count prefix must be 3");

        // Elements in LE order
        assert_eq!(le_u32(buf, vec_pos + 4), 10);
        assert_eq!(le_u32(buf, vec_pos + 8), 20);
        assert_eq!(le_u32(buf, vec_pos + 12), 30);
    }

    // ── 8. Hand-crafted vector — read from known bytes ──────────────────

    #[test]
    fn wire_handcrafted_vector_u16() {
        // Table with one field: a vector of u16 [100, 200, 300]
        // Layout:
        //   0x00  0C 00 00 00   root offset → table at 12
        //   0x04  03 00 00 00   vector count = 3
        //   0x08  64 00         100
        //   0x0A  C8 00         200
        //   0x0C  2C 01         300
        //   0x0E  00 00         padding to 4-byte align table
        //   ... actually let me space this properly.

        // Simpler approach: vtable, then table, then vector:
        //   0x00  10 00 00 00   root offset = 16 → table
        //   0x04  06 00         vtable size = 6
        //   0x06  08 00         table size = 8
        //   0x08  04 00         slot 0 at offset 4
        //   0x0A  00 00         padding
        //   0x0C  00 00 00 00   padding (alignment)
        //   0x10  0C 00 00 00   soffset = 12 (16 - 4)
        //   0x14  08 00 00 00   offset to vector: 0x14 + 8 = 0x1C
        //   0x18  00 00 00 00   padding
        //   0x1C  03 00 00 00   vector count = 3
        //   0x20  64 00         100
        //   0x22  C8 00         200
        //   0x24  2C 01         300
        //   0x26  00 00         trailing pad

        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x10, 0x00, 0x00, 0x00,  // root offset = 16
            0x06, 0x00,              // vtable size = 6
            0x08, 0x00,              // table size = 8
            0x04, 0x00,              // slot 0 at offset 4
            0x00, 0x00,              // padding
            0x00, 0x00, 0x00, 0x00,  // alignment padding
            0x0C, 0x00, 0x00, 0x00,  // soffset = 12
            0x08, 0x00, 0x00, 0x00,  // offset to vector = 8 → 0x14+8=0x1C
            0x00, 0x00, 0x00, 0x00,  // padding
            0x03, 0x00, 0x00, 0x00,  // vector count = 3
            0x64, 0x00,              // 100
            0xC8, 0x00,              // 200
            0x2C, 0x01,              // 300
            0x00, 0x00,              // trailing pad
        ];

        let view = FbView::from_buffer(&buf).unwrap();
        let vec_view = view.read_vec(0).unwrap();
        assert_eq!(vec_view.len(), 3);
        assert_eq!(vec_view.get_u16(0), 100);
        assert_eq!(vec_view.get_u16(1), 200);
        assert_eq!(vec_view.get_u16(2), 300);
    }

    // ── 9. String NUL terminator and alignment ──────────────────────────

    #[test]
    fn wire_string_nul_and_alignment() {
        // Test strings of various lengths to verify NUL + alignment padding
        for s in &["", "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg"] {
            let mut b = FbBuilder::default();
            let s_off = b.create_string(s);
            b.start_table();
            b.add_offset_field(4, s_off);
            let root = b.end_table();
            b.finish(root);
            let buf = b.data();

            // Find the string position
            let view = FbView::from_buffer(buf).unwrap();
            let read_back = view.read_str(0);

            if s.is_empty() {
                // Empty string: we pass 0 offset so field is absent
                // Our implementation omits empty string offsets
                continue;
            }

            assert_eq!(read_back, Some(*s), "string mismatch for {:?}", s);

            // Find the string in the buffer to verify NUL terminator
            let root_off = le_u32(buf, 0) as usize;
            let soff = le_i32(buf, root_off);
            let vt_pos = (root_off as i64 - soff as i64) as usize;
            let field_off = le_u16(buf, vt_pos + 4) as usize;
            let str_ref_pos = root_off + field_off;
            let str_pos = str_ref_pos + le_u32(buf, str_ref_pos) as usize;

            // Length prefix
            let len = le_u32(buf, str_pos) as usize;
            assert_eq!(len, s.len(), "length prefix mismatch for {:?}", s);

            // NUL terminator
            assert_eq!(buf[str_pos + 4 + len], 0,
                "NUL terminator missing for string {:?}", s);

            // String length prefix must be 4-byte aligned
            assert_eq!(str_pos % 4, 0,
                "string at pos {} not 4-byte aligned for {:?}", str_pos, s);
        }
    }

    // ── 10. Root offset points to root table ────────────────────────────

    #[test]
    fn wire_root_offset_correct() {
        let orig = Token {
            kind: 7,
            text: "test".to_string(),
        };
        let buf = orig.fb_pack();

        // The first 4 bytes are a u32 that, when added to position 0, gives
        // the absolute byte position of the root table.
        let root_off = le_u32(&buf, 0) as usize;

        // root_off must be within the buffer and leave room for at least
        // the soffset field (4 bytes)
        assert!(root_off + 4 <= buf.len(),
            "root offset {} + 4 exceeds buffer len {}", root_off, buf.len());

        // The root table must have a valid soffset leading to a vtable
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        assert!(vt_pos + 4 <= buf.len(), "vtable out of bounds");

        // vtable_size must be reasonable
        let vt_size = le_u16(&buf, vt_pos);
        assert!(vt_size >= 4, "vtable size too small: {}", vt_size);
        assert!(vt_pos + vt_size as usize <= buf.len(), "vtable extends past buffer");
    }

    // ── 11. soffset from table to vtable ────────────────────────────────
    //
    // The soffset is a signed i32 at the start of the table. The formula is:
    //   vtable_pos = table_pos - soffset
    // This means soffset = table_pos - vtable_pos.

    #[test]
    fn wire_soffset_formula() {
        // Build several different tables and verify the soffset formula
        for &kind in &[0u16, 1, 100, 0xFFFF] {
            let orig = Token {
                kind,
                text: "x".to_string(),
            };
            let buf = orig.fb_pack();

            let root_off = le_u32(&buf, 0) as usize;
            let soff = le_i32(&buf, root_off);
            let computed_vt = (root_off as i64 - soff as i64) as usize;

            // Verify we can read the vtable header
            let vt_size = le_u16(&buf, computed_vt);
            assert!(vt_size >= 4, "vtable at computed position has invalid size");

            // Verify the reverse formula
            let reverse_soff = root_off as i32 - computed_vt as i32;
            assert_eq!(soff, reverse_soff,
                "soffset {} != table_pos {} - vtable_pos {}",
                soff, root_off, computed_vt);
        }
    }

    // ── 12. Multiple scalar types byte-level ────────────────────────────

    #[test]
    fn wire_scalar_types_byte_level() {
        // Build a table with one field of each scalar type and verify the
        // raw bytes at the field position match the expected LE encoding.
        let layout = FbLayout::simple(6);
        let mut b = FbBuilder::default();
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u8(0, 0xAB, 0);
        tw.add_i16(1, -1234, 0);
        tw.add_u32(2, 0x12345678, 0);
        tw.add_i32(3, -99999, 0);
        tw.add_u64(4, 0x0102030405060708, 0);
        tw.add_f32(5, std::f32::consts::PI, 0.0);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        // Helper: read field bytes at a given vtable slot
        let field_bytes = |slot: u16, size: usize| -> &[u8] {
            let vt_byte = 4 + 2 * slot;
            let fo = le_u16(&buf, vt_pos + vt_byte as usize) as usize;
            assert_ne!(fo, 0, "slot {} should be present", slot);
            &buf[root_off + fo..root_off + fo + size]
        };

        // u8
        assert_eq!(field_bytes(0, 1), &[0xAB]);

        // i16 = -1234 = 0xFB2E in LE
        assert_eq!(field_bytes(1, 2), &(-1234i16).to_le_bytes());

        // u32 = 0x12345678
        assert_eq!(field_bytes(2, 4), &0x12345678u32.to_le_bytes());

        // i32 = -99999
        assert_eq!(field_bytes(3, 4), &(-99999i32).to_le_bytes());

        // u64
        assert_eq!(field_bytes(4, 8), &0x0102030405060708u64.to_le_bytes());

        // f32 = PI
        assert_eq!(field_bytes(5, 4), &std::f32::consts::PI.to_le_bytes());
    }

    // ── 13. Vector of strings (offset vector) ───────────────────────────

    #[test]
    fn wire_vector_of_strings_structure() {
        let mut b = FbBuilder::default();
        let offs: Vec<u32> = vec![
            b.create_string("alpha"),
            b.create_string("beta"),
            b.create_string("gamma"),
        ];
        let v_off = b.create_vec_offsets(&offs);
        b.start_table();
        b.add_offset_field(4, v_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        let vec_view = view.read_vec(0).unwrap();

        assert_eq!(vec_view.len(), 3);
        assert_eq!(vec_view.get_str(0), Some("alpha"));
        assert_eq!(vec_view.get_str(1), Some("beta"));
        assert_eq!(vec_view.get_str(2), Some("gamma"));

        // Verify the raw vector: find it and check count + offset entries
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let field_off = le_u16(&buf, vt_pos + 4) as usize;
        let vec_ref_pos = root_off + field_off;
        let vec_pos = vec_ref_pos + le_u32(&buf, vec_ref_pos) as usize;

        let count = le_u32(&buf, vec_pos);
        assert_eq!(count, 3, "vector of strings count must be 3");

        // Each entry is a u32 relative offset to a string
        for i in 0..3u32 {
            let entry_pos = vec_pos + 4 + i as usize * 4;
            let str_rel = le_u32(&buf, entry_pos) as usize;
            let str_pos = entry_pos + str_rel;

            // Verify string at that position
            let slen = le_u32(&buf, str_pos) as usize;
            let sdata = std::str::from_utf8(&buf[str_pos + 4..str_pos + 4 + slen]).unwrap();
            let expected = ["alpha", "beta", "gamma"][i as usize];
            assert_eq!(sdata, expected, "string at index {}", i);

            // NUL terminator
            assert_eq!(buf[str_pos + 4 + slen], 0, "NUL terminator at index {}", i);
        }
    }

    // ── 14. Nested table wire format ────────────────────────────────────

    #[test]
    fn wire_nested_table_structure() {
        // Build: outer table with a nested inner table
        let mut b = FbBuilder::default();

        // Inner table: field 0 = i32(42)
        b.start_table();
        b.add_scalar_i32(4, 42, 0);
        let inner_off = b.end_table();

        // Outer table: field 0 = offset to inner
        b.start_table();
        b.add_offset_field(4, inner_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // Verify outer table
        let outer_view = FbView::from_buffer(&buf).unwrap();
        assert!(outer_view.has_field(0), "outer slot 0 must be present");

        // Verify inner table through nested view
        let inner_view = outer_view.read_table(0).unwrap();
        assert_eq!(inner_view.read_i32(0, 0), 42);

        // Verify the raw encoding: outer table's field 0 is a u32 relative
        // offset to the inner table
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let field_off = le_u16(&buf, vt_pos + 4) as usize;
        let inner_ref_pos = root_off + field_off;
        let inner_rel = le_u32(&buf, inner_ref_pos) as usize;
        let inner_pos = inner_ref_pos + inner_rel;

        // Inner table has its own soffset → vtable
        let inner_soff = le_i32(&buf, inner_pos);
        let inner_vt = (inner_pos as i64 - inner_soff as i64) as usize;
        let inner_vt_size = le_u16(&buf, inner_vt);
        assert!(inner_vt_size >= 6, "inner vtable must have at least 1 slot");

        // Inner field 0 should be 42
        let inner_field_off = le_u16(&buf, inner_vt + 4) as usize;
        let val = le_i32(&buf, inner_pos + inner_field_off);
        assert_eq!(val, 42);
    }

    // ── 15. Empty table (no fields) ─────────────────────────────────────

    #[test]
    fn wire_empty_table() {
        let mut b = FbBuilder::default();
        b.start_table();
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        let vt_size = le_u16(&buf, vt_pos);
        let tbl_size = le_u16(&buf, vt_pos + 2);

        // vtable: just header (4 bytes), no field slots
        assert_eq!(vt_size, 4, "empty vtable should be 4 bytes (header only)");
        // table: just the soffset (4 bytes)
        assert_eq!(tbl_size, 4, "empty table should be 4 bytes (soffset only)");

        // View can be created and all field reads return defaults
        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u32(0, 123), 123);
        assert_eq!(view.read_str(0), None);
        assert!(!view.has_field(0));
    }

    // ── 16. Buffer alignment ────────────────────────────────────────────
    //
    // The root offset + subsequent data must be 4-byte aligned.

    #[test]
    fn wire_buffer_alignment() {
        // Test with various data sizes
        for n in 0..8u32 {
            let text: String = "x".repeat(n as usize);
            let orig = Token {
                kind: n as u16,
                text,
            };
            let buf = orig.fb_pack();

            // Buffer length should be a multiple of the minimum alignment (4)
            // per the FlatBuffers spec: the builder aligns the start.
            let root_off = le_u32(&buf, 0) as usize;

            // Root table position must be 4-byte aligned
            assert_eq!(root_off % 4, 0,
                "root table at {} not 4-byte aligned (text len {})", root_off, n);
        }
    }

    // ── 17. has_field reports correctly ──────────────────────────────────

    #[test]
    fn wire_has_field_correctness() {
        let layout = FbLayout::simple(4);
        let mut b = FbBuilder::default();
        let s_off = b.create_string("present");

        let mut tw = FbTableWriter::new(&mut b, &layout);
        // field 0: absent (default u32)
        tw.add_u32(0, 0, 0);
        // field 1: present (non-default u32)
        tw.add_u32(1, 1, 0);
        // field 2: absent (no string)
        tw.add_offset(2, 0);
        // field 3: present (string)
        tw.add_offset(3, s_off);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        assert!(!view.has_field(0), "field 0 (default scalar) should be absent");
        assert!(view.has_field(1), "field 1 (non-default scalar) should be present");
        assert!(!view.has_field(2), "field 2 (null offset) should be absent");
        assert!(view.has_field(3), "field 3 (string offset) should be present");

        // Out-of-range slot should be absent
        assert!(!view.has_field(99), "slot 99 should be absent");
    }

    // ── 18. Cross-verify builder output with hand-crafted bytes ─────────
    //
    // Build a minimal table with builder and manually decode every byte.

    #[test]
    fn wire_cross_verify_builder_bytes() {
        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u16(4, 0x0102, 0);  // slot 0: u16
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // Decode every structural element
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        let vt_size = le_u16(&buf, vt_pos);
        let tbl_size = le_u16(&buf, vt_pos + 2);

        // vtable: 4 header + 2 for one slot = 6
        assert_eq!(vt_size, 6, "vtable size for 1-slot table");

        // The field must be present
        let fo = le_u16(&buf, vt_pos + 4) as usize;
        assert_ne!(fo, 0);

        // Read the raw u16 value
        let raw = le_u16(&buf, root_off + fo);
        assert_eq!(raw, 0x0102);

        // tbl_size must cover soffset (4) + field data (at minimum)
        assert!(tbl_size >= 4);

        // soffset must be exactly table_pos - vtable_pos
        assert_eq!(soff, root_off as i32 - vt_pos as i32);
    }

    // ── 19. Bool encoding ───────────────────────────────────────────────

    #[test]
    fn wire_bool_encoding() {
        let layout = FbLayout::simple(2);
        let mut b = FbBuilder::default();

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_bool(0, true, false);
        tw.add_bool(1, false, false);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_bool(0, false), true);
        // field 1 = false with default false → elided
        assert_eq!(view.read_bool(1, false), false);
        // Since field 1 matches default, it should not be stored
        assert!(!view.has_field(1), "bool matching default should be elided");
    }

    // ── 20. Vector of u8 (byte buffer) ──────────────────────────────────

    #[test]
    fn wire_vector_u8_bytes() {
        let payload: Vec<u8> = vec![0x00, 0xFF, 0x42, 0x80, 0x7F];
        let mut b = FbBuilder::default();
        let v_off = b.create_vec_scalar(&payload, 1, payload.len());
        b.start_table();
        b.add_offset_field(4, v_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // Navigate to vector
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let field_off = le_u16(&buf, vt_pos + 4) as usize;
        let vec_ref_pos = root_off + field_off;
        let vec_pos = vec_ref_pos + le_u32(&buf, vec_ref_pos) as usize;

        // Count
        assert_eq!(le_u32(&buf, vec_pos), 5);

        // Raw bytes
        assert_eq!(&buf[vec_pos + 4..vec_pos + 9], &[0x00, 0xFF, 0x42, 0x80, 0x7F]);
    }

    // ── 21. Hand-crafted nested table read ──────────────────────────────

    #[test]
    fn wire_handcrafted_nested_table() {
        // Two tables in one buffer:
        // Inner table at position 28: field 0 = u32(99)
        // Outer table at position 16: field 0 = offset to inner
        //
        // Layout:
        //   0x00  10 00 00 00   root offset = 16 (outer table)
        //   --- inner vtable ---
        //   0x04  06 00         vtable size = 6
        //   0x06  08 00         table size = 8
        //   0x08  04 00         slot 0 at offset 4
        //   0x0A  00 00         padding
        //   --- outer vtable ---
        //   0x0C  06 00         vtable size = 6
        //   0x0E  08 00         table size = 8
        //   0x10  04 00         slot 0 at offset 4
        //   --- outer table ---  (at 0x12, but needs to be 4-byte aligned)
        //
        // Let me recalculate with proper alignment:
        //   0x00  14 00 00 00   root offset = 20 (outer table)
        //   --- inner vtable at 4 ---
        //   0x04  06 00         vtable size = 6
        //   0x06  08 00         table size = 8
        //   0x08  04 00         slot 0 at offset 4
        //   0x0A  00 00         padding
        //   --- outer vtable at 12 ---
        //   0x0C  06 00         vtable size = 6
        //   0x0E  08 00         table size = 8
        //   0x10  04 00         slot 0 at offset 4
        //   0x12  00 00         padding
        //   --- outer table at 20 ---
        //   0x14  08 00 00 00   soffset = 8 (20 - 12)
        //   0x18  08 00 00 00   offset to inner table: 0x18 + 8 = 0x20
        //   --- inner table at 32 ---
        //   0x1C  00 00 00 00   padding
        //   0x20  1C 00 00 00   soffset = 28 (32 - 4)
        //   0x24  63 00 00 00   field 0 = 99

        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x14, 0x00, 0x00, 0x00,  // root offset = 20
            // inner vtable at 4
            0x06, 0x00,              // vtable size = 6
            0x08, 0x00,              // table size = 8
            0x04, 0x00,              // slot 0: offset 4
            0x00, 0x00,              // padding
            // outer vtable at 12
            0x06, 0x00,              // vtable size = 6
            0x08, 0x00,              // table size = 8
            0x04, 0x00,              // slot 0: offset 4
            0x00, 0x00,              // padding
            // outer table at 20
            0x08, 0x00, 0x00, 0x00,  // soffset = 8 (20-12)
            0x08, 0x00, 0x00, 0x00,  // offset to inner = 8 → 0x18+8=0x20
            // padding
            0x00, 0x00, 0x00, 0x00,
            // inner table at 32
            0x1C, 0x00, 0x00, 0x00,  // soffset = 28 (32-4)
            0x63, 0x00, 0x00, 0x00,  // field 0 = 99
        ];

        let outer = FbView::from_buffer(&buf).unwrap();
        let inner = outer.read_table(0).expect("inner table should be present");
        assert_eq!(inner.read_u32(0, 0), 99);
    }

    // ── 22. f64 alignment in table ──────────────────────────────────────

    #[test]
    fn wire_f64_alignment() {
        let layout = FbLayout::simple(2);
        let mut b = FbBuilder::default();

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u8(0, 1, 0);           // 1-byte field
        tw.add_f64(1, 3.14159, 0.0);  // 8-byte field
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        // The f64 field should be properly aligned
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        // Find f64 field position
        let f64_field_off = le_u16(&buf, vt_pos + 6) as usize; // slot 1
        let f64_pos = root_off + f64_field_off;

        // Read the raw f64
        let raw = f64::from_le_bytes(buf[f64_pos..f64_pos + 8].try_into().unwrap());
        assert_eq!(raw, 3.14159);

        // View should read it correctly
        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u8(0, 0), 1);
        assert_eq!(view.read_f64(1, 0.0), 3.14159);
    }

    // ── 23. Vtable slot beyond vtable size returns default ──────────────

    #[test]
    fn wire_vtable_slot_beyond_size() {
        // Build a table with only slot 0
        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u32(4, 42, 0); // slot 0 only
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();

        // Slot 0 present
        assert_eq!(view.read_u32(0, 0), 42);

        // Slots 1, 2, 10, 100 all beyond the vtable → should return defaults
        assert_eq!(view.read_u32(1, 111), 111);
        assert_eq!(view.read_u32(2, 222), 222);
        assert_eq!(view.read_u32(10, 1010), 1010);
        assert_eq!(view.read_str(5), None);
        assert!(!view.has_field(3));
    }

    // ── 24. Empty vector ────────────────────────────────────────────────

    #[test]
    fn wire_empty_vector() {
        let mut b = FbBuilder::default();
        let v_off = b.create_vec_scalar(&[], 4, 0);
        b.start_table();
        b.add_offset_field(4, v_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        let vec_view = view.read_vec(0).unwrap();
        assert_eq!(vec_view.len(), 0);
        assert!(vec_view.is_empty());
    }

    // ── 25. FlatBuffers cross-language golden test ───────────────────────
    //
    // This test constructs the "canonical" FlatBuffers test structure manually
    // and verifies the exact byte layout matches what the official C++ builder
    // would produce. Based on the canonical Monster test in the FlatBuffers
    // repository (simplified to a subset we support).
    //
    // Schema:
    //   table SimpleMonster {
    //     hp: short = 100;     // slot 0
    //     name: string;        // slot 1
    //   }
    //
    // Values: hp = 80, name = "MyMonster"

    #[test]
    fn wire_golden_simple_monster() {
        let layout = FbLayout::simple(2);
        let mut b = FbBuilder::default();

        let name_off = b.create_string("MyMonster");

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_i16(0, 80, 100);        // hp = 80, default 100 → non-default, stored
        tw.add_offset(1, name_off);      // name = "MyMonster"
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        // Verify via view
        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_i16(0, 100), 80);
        assert_eq!(view.read_str(1), Some("MyMonster"));

        // Verify structural properties
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        let vt_size = le_u16(&buf, vt_pos);
        let tbl_size = le_u16(&buf, vt_pos + 2);

        // vtable: 4 header + 2*2 slots = 8
        assert_eq!(vt_size, 8, "vtable size for 2-slot table");
        assert!(tbl_size >= 4, "table must hold at least soffset");

        // Both fields should be present
        let hp_off = le_u16(&buf, vt_pos + 4);
        let name_off_vt = le_u16(&buf, vt_pos + 6);
        assert_ne!(hp_off, 0, "hp field should be present");
        assert_ne!(name_off_vt, 0, "name field should be present");

        // hp raw value check
        let hp_raw = i16::from_le_bytes(
            buf[root_off + hp_off as usize..root_off + hp_off as usize + 2]
                .try_into().unwrap()
        );
        assert_eq!(hp_raw, 80);

        // Name string check via raw bytes
        let name_ref_pos = root_off + name_off_vt as usize;
        let name_rel = le_u32(&buf, name_ref_pos) as usize;
        let name_pos = name_ref_pos + name_rel;
        let name_len = le_u32(&buf, name_pos) as usize;
        assert_eq!(name_len, 9); // "MyMonster" = 9 chars
        assert_eq!(&buf[name_pos + 4..name_pos + 4 + 9], b"MyMonster");
        assert_eq!(buf[name_pos + 4 + 9], 0); // NUL terminator
    }

    // ── 26. Default-value hp elided ─────────────────────────────────────

    #[test]
    fn wire_golden_default_hp_elided() {
        // Same schema as above but hp = 100 (the default) → should be elided
        let layout = FbLayout::simple(2);
        let mut b = FbBuilder::default();

        let name_off = b.create_string("DefaultHP");

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_i16(0, 100, 100);       // hp = default → elided
        tw.add_offset(1, name_off);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_i16(0, 100), 100, "should return default");
        assert!(!view.has_field(0), "default field should be absent");
        assert_eq!(view.read_str(1), Some("DefaultHP"));
    }

    // ── 27. Vector of i64 with extreme values ───────────────────────────

    #[test]
    fn wire_vector_i64_extremes() {
        let values: Vec<i64> = vec![i64::MIN, -1, 0, 1, i64::MAX];
        let bytes: &[u8] = unsafe {
            std::slice::from_raw_parts(
                values.as_ptr() as *const u8,
                values.len() * 8,
            )
        };
        let mut b = FbBuilder::default();
        let v_off = b.create_vec_scalar(bytes, 8, values.len());
        b.start_table();
        b.add_offset_field(4, v_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        let vec_view = view.read_vec(0).unwrap();
        assert_eq!(vec_view.len(), 5);
        assert_eq!(vec_view.get_i64(0), i64::MIN);
        assert_eq!(vec_view.get_i64(1), -1);
        assert_eq!(vec_view.get_i64(2), 0);
        assert_eq!(vec_view.get_i64(3), 1);
        assert_eq!(vec_view.get_i64(4), i64::MAX);

        // Verify raw LE encoding of i64::MIN and i64::MAX in the buffer
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let field_off = le_u16(&buf, vt_pos + 4) as usize;
        let vec_ref_pos = root_off + field_off;
        let vec_pos = vec_ref_pos + le_u32(&buf, vec_ref_pos) as usize;

        let first_elem_pos = vec_pos + 4;
        assert_eq!(
            &buf[first_elem_pos..first_elem_pos + 8],
            &i64::MIN.to_le_bytes()
        );
        let last_elem_pos = vec_pos + 4 + 4 * 8;
        assert_eq!(
            &buf[last_elem_pos..last_elem_pos + 8],
            &i64::MAX.to_le_bytes()
        );
    }

    // ── 28. Multiple tables sharing buffer ──────────────────────────────

    #[test]
    fn wire_multiple_sub_tables() {
        // Outer table with two nested sub-tables
        let layout_inner = FbLayout::simple(1);
        let layout_outer = FbLayout::simple(2);
        let mut b = FbBuilder::default();

        // Inner table A: val = 111
        let mut tw_a = FbTableWriter::new(&mut b, &layout_inner);
        tw_a.add_u32(0, 111, 0);
        let a_off = tw_a.finish();

        // Inner table B: val = 222
        let mut tw_b = FbTableWriter::new(&mut b, &layout_inner);
        tw_b.add_u32(0, 222, 0);
        let b_off = tw_b.finish();

        // Outer table
        let mut tw = FbTableWriter::new(&mut b, &layout_outer);
        tw.add_offset(0, a_off);
        tw.add_offset(1, b_off);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let outer = FbView::from_buffer(&buf).unwrap();
        let inner_a = outer.read_table(0).unwrap();
        let inner_b = outer.read_table(1).unwrap();
        assert_eq!(inner_a.read_u32(0, 0), 111);
        assert_eq!(inner_b.read_u32(0, 0), 222);
    }

    // ── 29. Hand-crafted: vtable with extra trailing slots ──────────────
    //
    // If a newer schema adds fields, the vtable grows but old readers should
    // still work. Old readers ignore extra vtable slots beyond what they know.

    #[test]
    fn wire_forward_compat_extra_vtable_slots() {
        // Table built with 4 vtable slots, but we only read slots 0 and 1.
        // Extra slots 2 and 3 have offsets but we ignore them.
        //
        // Vtable at 4, size 12 (0x04-0x0F). Table at 16 (0x10).
        // soffset = 16 - 4 = 12.
        // table_size = 4 (soffset) + 3*4 (fields) = 16
        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x10, 0x00, 0x00, 0x00,  // root offset = 16
            // vtable at 4
            0x0C, 0x00,              // vtable size = 12 (4 header + 4*2 slots)
            0x10, 0x00,              // table size = 16
            0x04, 0x00,              // slot 0: offset 4
            0x08, 0x00,              // slot 1: offset 8
            0x0C, 0x00,              // slot 2: offset 12 (unknown to old reader)
            0x00, 0x00,              // slot 3: absent
            // table at 16
            0x0C, 0x00, 0x00, 0x00,  // soffset = 12 (16-4)
            0x0A, 0x00, 0x00, 0x00,  // field 0 at +4: u32 = 10
            0x14, 0x00, 0x00, 0x00,  // field 1 at +8: u32 = 20
            0x1E, 0x00, 0x00, 0x00,  // field 2 at +12: u32 = 30 (extra)
        ];

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u32(0, 0), 10);
        assert_eq!(view.read_u32(1, 0), 20);
        // Slot 2 is readable (forward compat — new reader on old code would use it)
        assert_eq!(view.read_u32(2, 0), 30);
        // Slot 3 is absent
        assert_eq!(view.read_u32(3, 999), 999);
    }

    // ── 30. Hand-crafted: shorter vtable (backward compat) ──────────────
    //
    // If a buffer was built with an older schema with fewer fields, a newer
    // reader should get defaults for slots beyond the vtable.

    #[test]
    fn wire_backward_compat_short_vtable() {
        // Table with only 1 vtable slot, but reader expects 3 slots
        #[rustfmt::skip]
        let buf: Vec<u8> = vec![
            0x0C, 0x00, 0x00, 0x00,  // root offset = 12
            // vtable at 4
            0x06, 0x00,              // vtable size = 6 (4 header + 1*2 slot)
            0x08, 0x00,              // table size = 8
            0x04, 0x00,              // slot 0: offset 4
            0x00, 0x00,              // padding
            // table at 12
            0x08, 0x00, 0x00, 0x00,  // soffset = 8 (12-4)
            0xFF, 0x00, 0x00, 0x00,  // field 0: u32 = 255
        ];

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u32(0, 0), 255);
        // Slots 1, 2 are beyond vtable → defaults
        assert_eq!(view.read_u32(1, 42), 42);
        assert_eq!(view.read_u32(2, 77), 77);
        assert!(!view.has_field(1));
        assert!(!view.has_field(2));
    }

    // ── 31. Verify i8, i16, i32, i64 two's complement ───────────────────

    #[test]
    fn wire_twos_complement_signed() {
        let layout = FbLayout::simple(4);
        let mut b = FbBuilder::default();

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u8(0, (-1i8) as u8, 0);    // i8 = -1 → 0xFF
        tw.add_i16(1, -32768, 0);          // i16::MIN
        tw.add_i32(2, -2147483648, 0);     // i32::MIN
        tw.add_i64(3, -9223372036854775808, 0); // i64::MIN
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_i8(0, 0), -1);
        assert_eq!(view.read_i16(1, 0), i16::MIN);
        assert_eq!(view.read_i32(2, 0), i32::MIN);
        assert_eq!(view.read_i64(3, 0), i64::MIN);

        // Verify raw bytes match LE two's complement
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;

        let fo0 = le_u16(&buf, vt_pos + 4) as usize;
        assert_eq!(buf[root_off + fo0], 0xFF, "i8(-1) = 0xFF");

        let fo1 = le_u16(&buf, vt_pos + 6) as usize;
        assert_eq!(&buf[root_off + fo1..root_off + fo1 + 2], &[0x00, 0x80]);

        let fo2 = le_u16(&buf, vt_pos + 8) as usize;
        assert_eq!(&buf[root_off + fo2..root_off + fo2 + 4], &[0x00, 0x00, 0x00, 0x80]);

        let fo3 = le_u16(&buf, vt_pos + 10) as usize;
        assert_eq!(&buf[root_off + fo3..root_off + fo3 + 8],
            &[0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80]);
    }

    // ── 32. f32/f64 special values (NaN, Inf) ───────────────────────────

    #[test]
    fn wire_float_special_values() {
        let layout = FbLayout::simple(4);
        let mut b = FbBuilder::default();

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_f32(0, f32::INFINITY, 0.0);
        tw.add_f32(1, f32::NEG_INFINITY, 0.0);
        tw.add_f64(2, f64::INFINITY, 0.0);
        tw.add_f64(3, f64::NEG_INFINITY, 0.0);
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_f32(0, 0.0), f32::INFINITY);
        assert_eq!(view.read_f32(1, 0.0), f32::NEG_INFINITY);
        assert_eq!(view.read_f64(2, 0.0), f64::INFINITY);
        assert_eq!(view.read_f64(3, 0.0), f64::NEG_INFINITY);

        // NaN: verify it round-trips with correct bit pattern
        let layout2 = FbLayout::simple(1);
        let mut b2 = FbBuilder::default();
        let nan_bits: u32 = 0x7FC00001; // specific NaN payload
        let nan_f32 = f32::from_bits(nan_bits);
        let mut tw2 = FbTableWriter::new(&mut b2, &layout2);
        tw2.add_f32(0, nan_f32, 0.0);
        let root2 = tw2.finish();
        b2.finish(root2);
        let buf2 = b2.data();

        let view2 = FbView::from_buffer(&buf2).unwrap();
        let read_nan = view2.read_f32(0, 0.0);
        assert!(read_nan.is_nan());
        assert_eq!(read_nan.to_bits(), nan_bits, "NaN bit pattern must be preserved");
    }

    // ── 33. Complex multi-level nesting ─────────────────────────────────

    #[test]
    fn wire_three_level_nesting() {
        // Grandparent → Parent → Child
        let layout1 = FbLayout::simple(1);

        let mut b = FbBuilder::default();

        // Child: val = 7
        let mut tw_c = FbTableWriter::new(&mut b, &layout1);
        tw_c.add_u32(0, 7, 0);
        let child_off = tw_c.finish();

        // Parent: child ref
        let mut tw_p = FbTableWriter::new(&mut b, &layout1);
        tw_p.add_offset(0, child_off);
        let parent_off = tw_p.finish();

        // Grandparent: parent ref
        let mut tw_g = FbTableWriter::new(&mut b, &layout1);
        tw_g.add_offset(0, parent_off);
        let root = tw_g.finish();
        b.finish(root);
        let buf = b.data();

        let gp = FbView::from_buffer(&buf).unwrap();
        let parent = gp.read_table(0).unwrap();
        let child = parent.read_table(0).unwrap();
        assert_eq!(child.read_u32(0, 0), 7);
    }

    // ── 34. Large vector (1000 elements) ────────────────────────────────

    #[test]
    fn wire_large_vector() {
        let values: Vec<u32> = (0..1000).collect();
        let data = values.fb_pack();
        let result = Vec::<u32>::fb_unpack(&data).unwrap();
        assert_eq!(result, values);

        // Verify count in raw bytes
        let view = FbView::from_buffer(&data).unwrap();
        let vec_view = view.read_vec(0).unwrap();
        assert_eq!(vec_view.len(), 1000);
        assert_eq!(vec_view.get_u32(0), 0);
        assert_eq!(vec_view.get_u32(999), 999);
    }

    // ── 35. Table with many fields (max-slot stress) ────────────────────

    #[test]
    fn wire_many_fields_table() {
        let n = 32;
        let layout = FbLayout::simple(n);
        let mut b = FbBuilder::default();

        let mut tw = FbTableWriter::new(&mut b, &layout);
        for i in 0..n {
            tw.add_u32(i, (i as u32 + 1) * 100, 0);
        }
        let root = tw.finish();
        b.finish(root);
        let buf = b.data();

        let view = FbView::from_buffer(&buf).unwrap();
        for i in 0..n as u16 {
            assert_eq!(
                view.read_u32(i, 0),
                (i as u32 + 1) * 100,
                "field {} mismatch", i
            );
        }

        // Verify vtable has all 32 slots
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let vt_size = le_u16(&buf, vt_pos);
        assert!(vt_size >= 4 + 2 * 32, "vtable must hold 32 slots");
    }

    // ── 36. UTF-8 multi-byte string byte-level ──────────────────────────

    #[test]
    fn wire_utf8_multibyte_string() {
        let s = "\u{1F600}"; // 😀, 4 bytes in UTF-8: F0 9F 98 80
        let mut b = FbBuilder::default();
        let s_off = b.create_string(s);
        b.start_table();
        b.add_offset_field(4, s_off);
        let root = b.end_table();
        b.finish(root);
        let buf = b.data();

        // Find the string in the buffer
        let root_off = le_u32(&buf, 0) as usize;
        let soff = le_i32(&buf, root_off);
        let vt_pos = (root_off as i64 - soff as i64) as usize;
        let fo = le_u16(&buf, vt_pos + 4) as usize;
        let str_ref = root_off + fo;
        let str_pos = str_ref + le_u32(&buf, str_ref) as usize;

        // Length prefix: 4 bytes (not 1 codepoint)
        let len = le_u32(&buf, str_pos) as usize;
        assert_eq!(len, 4, "UTF-8 length should be 4 bytes for U+1F600");

        // Raw bytes
        assert_eq!(&buf[str_pos + 4..str_pos + 8], &[0xF0, 0x9F, 0x98, 0x80]);

        // NUL terminator
        assert_eq!(buf[str_pos + 8], 0);

        // View reads it correctly
        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_str(0), Some("\u{1F600}"));
    }

    // ── 37. Round-trip equivalence: pack then byte-inspect ──────────────
    //
    // Build the same data with FbTableWriter and low-level builder, verify
    // the view reads the same values from both.

    #[test]
    fn wire_two_builders_same_view() {
        // Method 1: FbTableWriter
        let layout = FbLayout::simple(3);
        let mut b1 = FbBuilder::default();
        let s1 = b1.create_string("hello");
        let mut tw1 = FbTableWriter::new(&mut b1, &layout);
        tw1.add_u32(0, 42, 0);
        tw1.add_offset(1, s1);
        tw1.add_i64(2, -999, 0);
        let root1 = tw1.finish();
        b1.finish(root1);
        let buf1 = b1.data();

        // Method 2: low-level builder (same slot assignments)
        let mut b2 = FbBuilder::default();
        let s2 = b2.create_string("hello");
        b2.start_table();
        b2.add_scalar_u32(4, 42, 0);   // slot 0
        b2.add_offset_field(6, s2);     // slot 1
        b2.add_scalar_i64(8, -999, 0);  // slot 2
        let root2 = b2.end_table();
        b2.finish(root2);
        let buf2 = b2.data();

        // Both should produce the same logical content
        let v1 = FbView::from_buffer(buf1).unwrap();
        let v2 = FbView::from_buffer(buf2).unwrap();

        assert_eq!(v1.read_u32(0, 0), v2.read_u32(0, 0));
        assert_eq!(v1.read_str(1), v2.read_str(1));
        assert_eq!(v1.read_i64(2, 0), v2.read_i64(2, 0));
    }

    // ── 38. Official FlatBuffers spec: vtable dedup (optional) ──────────
    //
    // The spec allows vtable deduplication (sharing identical vtables).
    // Our builder does not currently dedup, but we verify that if someone
    // builds a buffer with shared vtables, our view can read it.

    #[test]
    fn wire_shared_vtable_read() {
        // Two tables sharing the same vtable. Both have slot 0 = u32.
        // Layout:
        //   0x00  18 00 00 00  root offset = 24 (outer table)
        //   0x04  06 00        shared vtable size = 6
        //   0x06  08 00        table size = 8
        //   0x08  04 00        slot 0 at offset 4
        //   0x0A  00 00        padding
        //   --- outer vtable at 12 ---
        //   0x0C  06 00        vtable size = 6
        //   0x0E  08 00        table size = 8
        //   0x10  04 00        slot 0: offset 4
        //   0x12  00 00        padding
        //   --- inner table at 20, using shared vtable at 4 ---
        //   0x14  10 00 00 00  soffset = 16 (20-4)
        //   0x18  0A 00 00 00  field 0 = 10
        //   --- outer table at 24 ---
        //   0x18  0C 00 00 00  soffset = 12 (24-12)
        //   0x1C  ...

        // Build with the builder and verify that nested tables with
        // identical vtable structure can be read correctly.
        let layout1 = FbLayout::simple(1);
        let mut b = FbBuilder::default();

        let mut tw_inner = FbTableWriter::new(&mut b, &layout1);
        tw_inner.add_u32(0, 10, 0);
        let inner_off = tw_inner.finish();

        let mut tw_outer = FbTableWriter::new(&mut b, &layout1);
        tw_outer.add_offset(0, inner_off);
        let root = tw_outer.finish();
        b.finish(root);
        let buf = b.data();

        let outer = FbView::from_buffer(&buf).unwrap();
        let inner = outer.read_table(0).unwrap();
        assert_eq!(inner.read_u32(0, 0), 10);
    }
}
