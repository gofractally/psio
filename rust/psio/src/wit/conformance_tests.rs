//! WIT Canonical ABI conformance tests.
//!
//! These tests verify that our WIT implementation matches the official
//! WebAssembly Component Model Canonical ABI specification from:
//! https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md
//!
//! The spec defines layout rules for Memory32 (32-bit pointers):
//! - Scalars: natural size and alignment
//! - Bool: 1 byte (0 or 1), NOT bit-packed
//! - Strings: (ptr: u32, len: u32) = size 8, alignment 4
//! - Lists: (ptr: u32, len: u32) = size 8, alignment 4
//! - Records: fields in declaration order, each naturally aligned,
//!   struct alignment = max field alignment, total size rounded up
//! - Variants: discriminant (u8 for <=256 cases) + padding to max
//!   payload alignment + max payload size, rounded to variant alignment
//! - Option<T> = variant { none, some(T) }
//! - Tuples = records with positional fields
//! - All multi-byte values are little-endian

#[cfg(test)]
mod tests {
    use crate::wit::layout::{align_up, compute_struct_layout, WitLayout};
    use crate::wit::pack::WitPack;
    use crate::wit::view::{WitUnpack, WitView, WitViewError};
    use crate::wit::mutation::WitMut;
    // WitPack/WitUnpack derive macros (re-exported from psio-macros through fracpack.rs)
    use crate::WitPack as WitPack;
    use crate::WitUnpack as WitUnpack;

    // ========================================================================
    // 1. Scalar alignment and size (spec: alignment() and elem_size())
    // ========================================================================

    #[test]
    fn spec_scalar_bool_layout() {
        // Spec: BoolType => alignment 1, size 1
        assert_eq!(bool::wit_alignment(), 1);
        assert_eq!(bool::wit_size(), 1);
    }

    #[test]
    fn spec_scalar_u8_s8_layout() {
        // Spec: U8Type/S8Type => alignment 1, size 1
        assert_eq!(u8::wit_alignment(), 1);
        assert_eq!(u8::wit_size(), 1);
        assert_eq!(i8::wit_alignment(), 1);
        assert_eq!(i8::wit_size(), 1);
    }

    #[test]
    fn spec_scalar_u16_s16_layout() {
        // Spec: U16Type/S16Type => alignment 2, size 2
        assert_eq!(u16::wit_alignment(), 2);
        assert_eq!(u16::wit_size(), 2);
        assert_eq!(i16::wit_alignment(), 2);
        assert_eq!(i16::wit_size(), 2);
    }

    #[test]
    fn spec_scalar_u32_s32_f32_layout() {
        // Spec: U32Type/S32Type/F32Type => alignment 4, size 4
        assert_eq!(u32::wit_alignment(), 4);
        assert_eq!(u32::wit_size(), 4);
        assert_eq!(i32::wit_alignment(), 4);
        assert_eq!(i32::wit_size(), 4);
        assert_eq!(f32::wit_alignment(), 4);
        assert_eq!(f32::wit_size(), 4);
    }

    #[test]
    fn spec_scalar_u64_s64_f64_layout() {
        // Spec: U64Type/S64Type/F64Type => alignment 8, size 8
        assert_eq!(u64::wit_alignment(), 8);
        assert_eq!(u64::wit_size(), 8);
        assert_eq!(i64::wit_alignment(), 8);
        assert_eq!(i64::wit_size(), 8);
        assert_eq!(f64::wit_alignment(), 8);
        assert_eq!(f64::wit_size(), 8);
    }

    // ========================================================================
    // 2. Bool encoding: single byte, not bit-packed
    // ========================================================================

    #[test]
    fn spec_bool_encoding_true() {
        // Spec: store_int(cx, int(bool(v)), ptr, 1) => true = 0x01
        let buf = true.wit_pack();
        assert_eq!(buf, vec![1u8]);
    }

    #[test]
    fn spec_bool_encoding_false() {
        // Spec: store_int(cx, int(bool(v)), ptr, 1) => false = 0x00
        let buf = false.wit_pack();
        assert_eq!(buf, vec![0u8]);
    }

    #[test]
    fn spec_bool_not_bit_packed() {
        // Verify that each bool occupies a full byte, not a single bit.
        // Pack a tuple of 3 bools: should be 3 bytes, not packed into bits.
        assert_eq!(<(bool, bool, bool)>::wit_size(), 3);
        let val = (true, false, true);
        let buf = val.wit_pack();
        assert_eq!(buf.len(), 3);
        assert_eq!(buf[0], 1); // first bool
        assert_eq!(buf[1], 0); // second bool
        assert_eq!(buf[2], 1); // third bool
    }

    // ========================================================================
    // 3. String encoding: (ptr: u32, len: u32) for Memory32
    // ========================================================================

    #[test]
    fn spec_string_layout() {
        // Spec: StringType => alignment = ptr_size(Memory32) = 4
        //       size = 2 * ptr_size = 8
        assert_eq!(String::wit_alignment(), 4);
        assert_eq!(String::wit_size(), 8);
        assert_eq!(str::wit_alignment(), 4);
        assert_eq!(str::wit_size(), 8);
    }

    #[test]
    fn spec_string_encoding_hello() {
        let s = String::from("hello");
        let buf = s.wit_pack();
        // Root record is 8 bytes (ptr + len), then 5 bytes of string data
        assert_eq!(buf.len(), 13);
        // ptr field at offset 0 (little-endian u32)
        let ptr = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        // len field at offset 4 (little-endian u32)
        let len = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(ptr, 8); // string data starts right after the root record
        assert_eq!(len, 5);
        // Verify actual UTF-8 bytes
        assert_eq!(&buf[ptr as usize..(ptr + len) as usize], b"hello");
    }

    #[test]
    fn spec_string_empty() {
        let s = String::new();
        let buf = s.wit_pack();
        // Root record is 8 bytes, no trailing data
        assert_eq!(buf.len(), 8);
        let ptr = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        let len = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(len, 0);
        // ptr can be anything for empty string; just verify roundtrip
        assert_eq!(String::wit_unpack(&buf).unwrap(), "");
    }

    #[test]
    fn spec_string_utf8_multibyte() {
        // Verify UTF-8 encoded strings with multi-byte characters
        let s = String::from("\u{00e9}\u{1f600}"); // e-acute + grinning face emoji
        let buf = s.wit_pack();
        let len = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        // e-acute = 2 bytes in UTF-8, grinning face = 4 bytes
        assert_eq!(len, 6);
        assert_eq!(String::wit_unpack(&buf).unwrap(), "\u{00e9}\u{1f600}");
    }

    // ========================================================================
    // 4. List encoding: (ptr: u32, len: u32) for Memory32
    // ========================================================================

    #[test]
    fn spec_list_layout() {
        // Spec: list<T> => alignment = ptr_size = 4, size = 2 * ptr_size = 8
        assert_eq!(Vec::<u8>::wit_alignment(), 4);
        assert_eq!(Vec::<u8>::wit_size(), 8);
        assert_eq!(Vec::<u32>::wit_alignment(), 4);
        assert_eq!(Vec::<u32>::wit_size(), 8);
        assert_eq!(Vec::<u64>::wit_alignment(), 4);
        assert_eq!(Vec::<u64>::wit_size(), 8);
        assert_eq!(Vec::<String>::wit_alignment(), 4);
        assert_eq!(Vec::<String>::wit_size(), 8);
    }

    #[test]
    fn spec_list_encoding_u32() {
        let v: Vec<u32> = vec![0xAABBCCDD, 0x11223344, 0xDEADBEEF];
        let buf = v.wit_pack();
        // Root: 8 bytes (ptr + count)
        // Array: 3 * 4 = 12 bytes at offset 8 (aligned to 4)
        assert_eq!(buf.len(), 20);
        let ptr = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        let count = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(ptr, 8);
        assert_eq!(count, 3);
        // Elements stored little-endian
        assert_eq!(
            u32::from_le_bytes(buf[8..12].try_into().unwrap()),
            0xAABBCCDD
        );
        assert_eq!(
            u32::from_le_bytes(buf[12..16].try_into().unwrap()),
            0x11223344
        );
        assert_eq!(
            u32::from_le_bytes(buf[16..20].try_into().unwrap()),
            0xDEADBEEF
        );
    }

    #[test]
    fn spec_list_encoding_u8() {
        let v: Vec<u8> = vec![1, 2, 3, 4, 5];
        let buf = v.wit_pack();
        // Root: 8 bytes, array: 5 bytes at offset 8 (u8 alignment=1)
        assert_eq!(buf.len(), 13);
        let ptr = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        let count = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(ptr, 8);
        assert_eq!(count, 5);
        assert_eq!(&buf[8..13], &[1, 2, 3, 4, 5]);
    }

    #[test]
    fn spec_list_empty() {
        let v: Vec<u32> = vec![];
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 8); // just the root record
        let count = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(count, 0);
        assert_eq!(Vec::<u32>::wit_unpack(&buf).unwrap(), Vec::<u32>::new());
    }

    #[test]
    fn spec_list_u64_alignment() {
        // u64 elements should be aligned to 8 bytes in the array
        let v: Vec<u64> = vec![1, 2];
        let buf = v.wit_pack();
        let ptr = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        // Array must start at an 8-byte aligned offset
        assert_eq!(ptr % 8, 0);
        let count = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(count, 2);
    }

    #[test]
    fn spec_list_of_strings() {
        // list<string>: each element is (ptr, len) = 8 bytes inline
        let v: Vec<String> = vec!["ab".into(), "cde".into()];
        let buf = v.wit_pack();
        // Root: 8 bytes
        // Array of 2 string records: 2 * 8 = 16 bytes
        // String data: "ab" (2 bytes) + "cde" (3 bytes) = 5 bytes
        // Total: 8 + 16 + 5 = 29
        assert_eq!(buf.len(), 29);
        let result = Vec::<String>::wit_unpack(&buf).unwrap();
        assert_eq!(result, vec!["ab", "cde"]);
    }

    // ========================================================================
    // 5. Record (struct) layout
    // ========================================================================

    #[test]
    fn spec_record_simple_u8_u32_u16() {
        // record { a: u8, b: u32, c: u16 }
        // Spec layout:
        //   offset 0: u8 (1 byte)
        //   offset 1-3: padding (3 bytes to align u32)
        //   offset 4: u32 (4 bytes)
        //   offset 8: u16 (2 bytes)
        //   offset 10-11: trailing padding (2 bytes to align to max=4)
        // Total: 12 bytes, alignment: 4
        let fields = vec![
            (u8::wit_alignment(), u8::wit_size()),
            (u32::wit_alignment(), u32::wit_size()),
            (u16::wit_alignment(), u16::wit_size()),
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 4);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 4);
        assert_eq!(locs[2].offset, 8);
        assert_eq!(total, 12);
    }

    #[test]
    fn spec_record_u64_u8() {
        // record { a: u64, b: u8 }
        // offset 0: u64 (8 bytes)
        // offset 8: u8 (1 byte)
        // offset 9-15: trailing padding (7 bytes to align to max=8)
        // Total: 16, alignment: 8
        let fields = vec![
            (u64::wit_alignment(), u64::wit_size()),
            (u8::wit_alignment(), u8::wit_size()),
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 8);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 8);
        assert_eq!(total, 16);
    }

    #[test]
    fn spec_record_u8_u64() {
        // record { a: u8, b: u64 }
        // offset 0: u8 (1 byte)
        // offset 1-7: padding (7 bytes to align u64)
        // offset 8: u64 (8 bytes)
        // Total: 16, alignment: 8
        let fields = vec![
            (u8::wit_alignment(), u8::wit_size()),
            (u64::wit_alignment(), u64::wit_size()),
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 8);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 8);
        assert_eq!(total, 16);
    }

    #[test]
    fn spec_record_all_u8() {
        // record { a: u8, b: u8, c: u8 }
        // No padding needed, alignment = 1
        let fields = vec![(1, 1), (1, 1), (1, 1)];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 1);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 1);
        assert_eq!(locs[2].offset, 2);
        assert_eq!(total, 3);
    }

    #[test]
    fn spec_record_empty() {
        // Empty record: alignment 1, size 0
        let fields: Vec<(u32, u32)> = vec![];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(locs.len(), 0);
        assert_eq!(max_align, 1);
        assert_eq!(total, 0);
    }

    #[test]
    fn spec_record_with_string_field() {
        // record { id: u32, name: string }
        // offset 0: u32 (4 bytes)
        // offset 4: string (8 bytes, alignment 4) -- no padding needed
        // Total: 12, alignment: 4
        let fields = vec![
            (u32::wit_alignment(), u32::wit_size()),
            (String::wit_alignment(), String::wit_size()),
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 4);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 4);
        assert_eq!(total, 12);
    }

    #[test]
    fn spec_record_u16_u8_trailing_pad() {
        // record { a: u16, b: u8 }
        // offset 0: u16 (2 bytes)
        // offset 2: u8 (1 byte)
        // offset 3: trailing pad (1 byte to align to max=2)
        // Total: 4, alignment: 2
        let fields = vec![
            (u16::wit_alignment(), u16::wit_size()),
            (u8::wit_alignment(), u8::wit_size()),
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 2);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 2);
        assert_eq!(total, 4);
    }

    #[test]
    fn spec_record_nested_alignment() {
        // Simulating nested record (inner is struct {u8, u32} = size 8, align 4)
        // record { x: u8, inner: {u8, u32}, y: u16 }
        // offset 0: u8 (1 byte)
        // offset 1-3: padding to align inner to 4
        // offset 4: inner (8 bytes)
        // offset 12: u16 (2 bytes)
        // offset 14-15: trailing pad to align to max=4
        // Total: 16, alignment: 4
        let inner_align = 4u32;
        let inner_size = 8u32;
        let fields = vec![
            (u8::wit_alignment(), u8::wit_size()),
            (inner_align, inner_size),
            (u16::wit_alignment(), u16::wit_size()),
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);
        assert_eq!(max_align, 4);
        assert_eq!(locs[0].offset, 0);
        assert_eq!(locs[1].offset, 4);
        assert_eq!(locs[2].offset, 12);
        assert_eq!(total, 16);
    }

    // ========================================================================
    // 6. Variant / Option layout
    // ========================================================================

    #[test]
    fn spec_variant_discriminant_is_u8_for_2_cases() {
        // Spec: discriminant_type(cases) for n=2:
        //   ceil(log2(2)/8) = ceil(0.125) = 1 => U8Type
        // Option is a variant with 2 cases, so discriminant is u8 (1 byte)
        // This is verified by Option's layout using 1-byte discriminant
        let opt: Option<u32> = Some(42);
        let buf = opt.wit_pack();
        // First byte is discriminant
        assert_eq!(buf[0], 1); // Some = case index 1

        let opt: Option<u32> = None;
        let buf = opt.wit_pack();
        assert_eq!(buf[0], 0); // None = case index 0
    }

    #[test]
    fn spec_option_u8_layout() {
        // option<u8> = variant { none, some(u8) }
        // discriminant: u8 (1 byte), alignment 1
        // max_case_alignment = max(void=1, u8=1) = 1
        // variant alignment = max(1, 1) = 1
        // size = 1 (disc) + 0 (no padding, already aligned) + 1 (payload) = 2
        // align_to(2, 1) = 2
        assert_eq!(Option::<u8>::wit_alignment(), 1);
        assert_eq!(Option::<u8>::wit_size(), 2);
    }

    #[test]
    fn spec_option_u16_layout() {
        // option<u16> = variant { none, some(u16) }
        // discriminant: u8, alignment 1
        // max_case_alignment = max(void=1, u16=2) = 2
        // variant alignment = max(1, 2) = 2
        // payload_offset = align_to(1, 2) = 2
        // size = 2 (disc+pad) + 2 (payload) = 4
        // align_to(4, 2) = 4
        assert_eq!(Option::<u16>::wit_alignment(), 2);
        assert_eq!(Option::<u16>::wit_size(), 4);
    }

    #[test]
    fn spec_option_u32_layout() {
        // option<u32> = variant { none, some(u32) }
        // discriminant: u8, alignment 1
        // max_case_alignment = max(void=1, u32=4) = 4
        // variant alignment = max(1, 4) = 4
        // payload_offset = align_to(1, 4) = 4
        // size = 4 (disc+pad) + 4 (payload) = 8
        // align_to(8, 4) = 8
        assert_eq!(Option::<u32>::wit_alignment(), 4);
        assert_eq!(Option::<u32>::wit_size(), 8);
    }

    #[test]
    fn spec_option_u64_layout() {
        // option<u64> = variant { none, some(u64) }
        // discriminant: u8, alignment 1
        // max_case_alignment = max(void=1, u64=8) = 8
        // variant alignment = max(1, 8) = 8
        // payload_offset = align_to(1, 8) = 8
        // size = 8 (disc+pad) + 8 (payload) = 16
        // align_to(16, 8) = 16
        assert_eq!(Option::<u64>::wit_alignment(), 8);
        assert_eq!(Option::<u64>::wit_size(), 16);
    }

    #[test]
    fn spec_option_string_layout() {
        // option<string>
        // discriminant: u8
        // max_case_alignment = max(void=1, string=4) = 4
        // variant alignment = max(1, 4) = 4
        // payload_offset = align_to(1, 4) = 4
        // size = 4 + 8 = 12
        // align_to(12, 4) = 12
        assert_eq!(Option::<String>::wit_alignment(), 4);
        assert_eq!(Option::<String>::wit_size(), 12);
    }

    #[test]
    fn spec_option_bool_layout() {
        // option<bool>
        // discriminant: u8
        // max_case_alignment = max(void=1, bool=1) = 1
        // variant alignment = max(1, 1) = 1
        // payload_offset = align_to(1, 1) = 1
        // size = 1 + 1 = 2
        assert_eq!(Option::<bool>::wit_alignment(), 1);
        assert_eq!(Option::<bool>::wit_size(), 2);
    }

    #[test]
    fn spec_option_encoding_none() {
        let v: Option<u32> = None;
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 0); // discriminant = 0 (none)
        // Payload area should be zeroed (padding bytes)
        assert_eq!(&buf[1..8], &[0, 0, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn spec_option_encoding_some_u32() {
        let v: Option<u32> = Some(0xDEADBEEF);
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 1); // discriminant = 1 (some)
        // Padding bytes 1-3
        // Payload at offset 4 (little-endian)
        assert_eq!(
            u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            0xDEADBEEF
        );
    }

    #[test]
    fn spec_option_encoding_some_u8() {
        let v: Option<u8> = Some(0xFF);
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 2);
        assert_eq!(buf[0], 1); // discriminant
        assert_eq!(buf[1], 0xFF); // payload immediately follows (no padding)
    }

    #[test]
    fn spec_option_nested() {
        // option<option<u32>>
        // Inner option<u32>: size=8, alignment=4
        // Outer option<inner>: disc u8, max_case_align=4, variant_align=4
        // payload_offset = align_to(1, 4) = 4
        // size = 4 + 8 = 12, align_to(12, 4) = 12
        assert_eq!(Option::<Option<u32>>::wit_alignment(), 4);
        assert_eq!(Option::<Option<u32>>::wit_size(), 12);

        // Roundtrip
        let v: Option<Option<u32>> = Some(Some(42));
        let buf = v.wit_pack();
        assert_eq!(Option::<Option<u32>>::wit_unpack(&buf).unwrap(), Some(Some(42)));

        let v: Option<Option<u32>> = Some(None);
        let buf = v.wit_pack();
        assert_eq!(Option::<Option<u32>>::wit_unpack(&buf).unwrap(), Some(None));

        let v: Option<Option<u32>> = None;
        let buf = v.wit_pack();
        assert_eq!(Option::<Option<u32>>::wit_unpack(&buf).unwrap(), None);
    }

    // ========================================================================
    // 7. Tuple layout (= record with positional fields)
    // ========================================================================

    #[test]
    fn spec_tuple_unit() {
        // tuple<> = record {} => alignment 1, size 0
        assert_eq!(<()>::wit_alignment(), 1);
        assert_eq!(<()>::wit_size(), 0);
    }

    #[test]
    fn spec_tuple_single() {
        // tuple<u32> = record { 0: u32 } => alignment 4, size 4
        assert_eq!(<(u32,)>::wit_alignment(), 4);
        assert_eq!(<(u32,)>::wit_size(), 4);
    }

    #[test]
    fn spec_tuple_u8_u32() {
        // tuple<u8, u32> = record { 0: u8, 1: u32 }
        // offset 0: u8, offset 4: u32 (3 bytes padding)
        // alignment = 4, size = 8
        assert_eq!(<(u8, u32)>::wit_alignment(), 4);
        assert_eq!(<(u8, u32)>::wit_size(), 8);
    }

    #[test]
    fn spec_tuple_u32_u64() {
        // tuple<u32, u64>
        // offset 0: u32 (4 bytes), offset 8: u64 (4 bytes padding)
        // alignment = 8, size = 16
        assert_eq!(<(u32, u64)>::wit_alignment(), 8);
        assert_eq!(<(u32, u64)>::wit_size(), 16);
    }

    #[test]
    fn spec_tuple_u8_u8() {
        // tuple<u8, u8> => alignment 1, size 2, no padding
        assert_eq!(<(u8, u8)>::wit_alignment(), 1);
        assert_eq!(<(u8, u8)>::wit_size(), 2);
    }

    #[test]
    fn spec_tuple_u8_u16_u32_u64() {
        // tuple<u8, u16, u32, u64>
        // offset 0: u8 (1 byte)
        // offset 2: u16 (1 byte padding)
        // offset 4: u32
        // offset 8: u64
        // alignment = 8, size = 16
        assert_eq!(<(u8, u16, u32, u64)>::wit_alignment(), 8);
        assert_eq!(<(u8, u16, u32, u64)>::wit_size(), 16);
    }

    #[test]
    fn spec_tuple_u64_u8_trailing_pad() {
        // tuple<u64, u8>
        // offset 0: u64 (8 bytes)
        // offset 8: u8 (1 byte)
        // trailing padding to 16 (align to 8)
        // alignment = 8, size = 16
        assert_eq!(<(u64, u8)>::wit_alignment(), 8);
        assert_eq!(<(u64, u8)>::wit_size(), 16);
    }

    // ========================================================================
    // 8. Byte-level encoding verification
    // ========================================================================

    #[test]
    fn spec_little_endian_u16() {
        let buf = 0x1234u16.wit_pack();
        assert_eq!(buf, vec![0x34, 0x12]);
    }

    #[test]
    fn spec_little_endian_u32() {
        let buf = 0x12345678u32.wit_pack();
        assert_eq!(buf, vec![0x78, 0x56, 0x34, 0x12]);
    }

    #[test]
    fn spec_little_endian_u64() {
        let buf = 0x0102030405060708u64.wit_pack();
        assert_eq!(buf, vec![0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01]);
    }

    #[test]
    fn spec_signed_encoding_i8() {
        let buf = (-1i8).wit_pack();
        assert_eq!(buf, vec![0xFF]); // two's complement
    }

    #[test]
    fn spec_signed_encoding_i16() {
        let buf = (-1i16).wit_pack();
        assert_eq!(buf, vec![0xFF, 0xFF]);
    }

    #[test]
    fn spec_signed_encoding_i32() {
        let buf = (-1i32).wit_pack();
        assert_eq!(buf, vec![0xFF, 0xFF, 0xFF, 0xFF]);
    }

    #[test]
    fn spec_signed_encoding_i64() {
        let buf = (-1i64).wit_pack();
        assert_eq!(buf, vec![0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
    }

    #[test]
    fn spec_f32_encoding() {
        // IEEE 754 single precision, little-endian
        let buf = 1.0f32.wit_pack();
        assert_eq!(buf, 1.0f32.to_le_bytes().to_vec());
        // 1.0f32 = 0x3F800000
        assert_eq!(buf, vec![0x00, 0x00, 0x80, 0x3F]);
    }

    #[test]
    fn spec_f64_encoding() {
        // IEEE 754 double precision, little-endian
        let buf = 1.0f64.wit_pack();
        assert_eq!(buf, 1.0f64.to_le_bytes().to_vec());
    }

    // ========================================================================
    // 9. Tuple pack/unpack byte-level verification
    // ========================================================================

    #[test]
    fn spec_tuple_u8_u32_byte_layout() {
        let val: (u8, u32) = (0xAA, 0xDEADBEEF);
        let buf = val.wit_pack();
        assert_eq!(buf.len(), 8);
        // u8 at offset 0
        assert_eq!(buf[0], 0xAA);
        // padding bytes 1-3 should be zero
        assert_eq!(&buf[1..4], &[0, 0, 0]);
        // u32 at offset 4, little-endian
        assert_eq!(
            u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            0xDEADBEEF
        );
    }

    #[test]
    fn spec_tuple_u8_u16_u32_byte_layout() {
        let val: (u8, u16, u32) = (0xFF, 0x1234, 0xABCD0000);
        let buf = val.wit_pack();
        assert_eq!(buf.len(), 8);
        // u8 at offset 0
        assert_eq!(buf[0], 0xFF);
        // 1 byte padding
        assert_eq!(buf[1], 0);
        // u16 at offset 2
        assert_eq!(u16::from_le_bytes(buf[2..4].try_into().unwrap()), 0x1234);
        // u32 at offset 4
        assert_eq!(
            u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            0xABCD0000
        );
    }

    // ========================================================================
    // 10. Record with string: out-of-line data
    // ========================================================================

    #[test]
    fn spec_tuple_u32_string_encoding() {
        // Simulate: record { id: u32, name: string }
        // Record layout: u32 at 0, string at 4 (ptr+len = 8 bytes)
        // Total record: 12 bytes
        // String data appended after
        let val: (u32, String) = (42, "hello".into());
        let buf = val.wit_pack();
        // Record: 12 bytes + string data "hello" 5 bytes = 17
        assert_eq!(buf.len(), 17);

        // u32 at offset 0
        assert_eq!(u32::from_le_bytes(buf[0..4].try_into().unwrap()), 42);
        // string ptr at offset 4
        let str_ptr = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        // string len at offset 8
        let str_len = u32::from_le_bytes(buf[8..12].try_into().unwrap());
        assert_eq!(str_len, 5);
        // string data at str_ptr
        assert_eq!(
            &buf[str_ptr as usize..(str_ptr + str_len) as usize],
            b"hello"
        );

        // Roundtrip
        let result = <(u32, String)>::wit_unpack(&buf).unwrap();
        assert_eq!(result, (42, "hello".into()));
    }

    // ========================================================================
    // 11. View operations
    // ========================================================================

    #[test]
    fn spec_view_tuple_fields() {
        let val: (u8, u32, u16) = (0xAA, 0x12345678, 0xBBCC);
        let buf = val.wit_pack();
        let view = WitView::from_buffer(&buf);
        // u8 at offset 0
        assert_eq!(view.read_u8(0), 0xAA);
        // u32 at offset 4
        assert_eq!(view.read_u32(4), 0x12345678);
        // u16 at offset 8
        assert_eq!(view.read_u16(8), 0xBBCC);
    }

    #[test]
    fn spec_view_option_discriminant() {
        let some_val: Option<u32> = Some(99);
        let buf = some_val.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert!(view.read_option_discriminant(0));
        let payload = view.option_payload_view::<u32>(0);
        assert_eq!(payload.read_u32(0), 99);

        let none_val: Option<u32> = None;
        let buf = none_val.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert!(!view.read_option_discriminant(0));
    }

    #[test]
    fn spec_view_list_elements() {
        let v: Vec<u32> = vec![10, 20, 30];
        let buf = v.wit_pack();
        let view = WitView::from_buffer(&buf);
        let list = view.read_list::<u32>(0).unwrap();
        assert_eq!(list.len(), 3);
        assert_eq!(list.element_view(0).read_u32(0), 10);
        assert_eq!(list.element_view(1).read_u32(0), 20);
        assert_eq!(list.element_view(2).read_u32(0), 30);
    }

    #[test]
    fn spec_view_string_field() {
        let s = String::from("canonical abi");
        let buf = s.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert_eq!(view.read_string(0).unwrap(), "canonical abi");
    }

    // ========================================================================
    // 12. Mutation operations
    // ========================================================================

    #[test]
    fn spec_mutation_scalar_in_record() {
        let val: (u8, u32) = (10, 20);
        let mut buf = val.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            m.write_u8(0, 99);
            m.write_u32(4, 0xCAFEBABE);
        }
        let result = <(u8, u32)>::wit_unpack(&buf).unwrap();
        assert_eq!(result, (99, 0xCAFEBABE));
    }

    #[test]
    fn spec_mutation_option_payload() {
        let val: Option<u32> = Some(42);
        let mut buf = val.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            assert!(m.read_option_discriminant(0));
            let payload_off = m.option_payload_offset::<u32>(0);
            assert_eq!(payload_off, 4); // align_up(1, 4) = 4
            m.write_u32(payload_off, 999);
        }
        assert_eq!(Option::<u32>::wit_unpack(&buf).unwrap(), Some(999));
    }

    #[test]
    fn spec_mutation_option_u8_payload() {
        let val: Option<u8> = Some(42);
        let mut buf = val.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            let payload_off = m.option_payload_offset::<u8>(0);
            assert_eq!(payload_off, 1); // align_up(1, 1) = 1
            m.write_u8(payload_off, 0xFF);
        }
        assert_eq!(Option::<u8>::wit_unpack(&buf).unwrap(), Some(0xFF));
    }

    // ========================================================================
    // 13. align_up helper
    // ========================================================================

    #[test]
    fn spec_align_up_function() {
        // Spec: align_to(ptr, alignment) = ceil(ptr / alignment) * alignment
        assert_eq!(align_up(0, 1), 0);
        assert_eq!(align_up(0, 4), 0);
        assert_eq!(align_up(0, 8), 0);
        assert_eq!(align_up(1, 1), 1);
        assert_eq!(align_up(1, 2), 2);
        assert_eq!(align_up(1, 4), 4);
        assert_eq!(align_up(1, 8), 8);
        assert_eq!(align_up(3, 4), 4);
        assert_eq!(align_up(4, 4), 4);
        assert_eq!(align_up(5, 4), 8);
        assert_eq!(align_up(7, 8), 8);
        assert_eq!(align_up(8, 8), 8);
        assert_eq!(align_up(9, 8), 16);
    }

    // ========================================================================
    // 14. Comprehensive roundtrip tests
    // ========================================================================

    #[test]
    fn spec_roundtrip_all_scalars() {
        assert_eq!(bool::wit_unpack(&true.wit_pack()).unwrap(), true);
        assert_eq!(bool::wit_unpack(&false.wit_pack()).unwrap(), false);
        assert_eq!(u8::wit_unpack(&0xFFu8.wit_pack()).unwrap(), 0xFF);
        assert_eq!(i8::wit_unpack(&(-128i8).wit_pack()).unwrap(), -128);
        assert_eq!(u16::wit_unpack(&0xABCDu16.wit_pack()).unwrap(), 0xABCD);
        assert_eq!(i16::wit_unpack(&(-32768i16).wit_pack()).unwrap(), -32768);
        assert_eq!(u32::wit_unpack(&0xDEADBEEFu32.wit_pack()).unwrap(), 0xDEADBEEF);
        assert_eq!(i32::wit_unpack(&(-2_000_000_000i32).wit_pack()).unwrap(), -2_000_000_000);
        assert_eq!(u64::wit_unpack(&u64::MAX.wit_pack()).unwrap(), u64::MAX);
        assert_eq!(i64::wit_unpack(&i64::MIN.wit_pack()).unwrap(), i64::MIN);
        assert_eq!(f32::wit_unpack(&std::f32::consts::PI.wit_pack()).unwrap(), std::f32::consts::PI);
        assert_eq!(f64::wit_unpack(&std::f64::consts::E.wit_pack()).unwrap(), std::f64::consts::E);
    }

    #[test]
    fn spec_roundtrip_complex_nested() {
        // tuple<option<string>, vec<u32>, bool>
        let val: (Option<String>, Vec<u32>, bool) = (
            Some("nested".into()),
            vec![1, 2, 3],
            true,
        );
        let buf = val.wit_pack();
        let result = <(Option<String>, Vec<u32>, bool)>::wit_unpack(&buf).unwrap();
        assert_eq!(result.0, Some("nested".into()));
        assert_eq!(result.1, vec![1, 2, 3]);
        assert_eq!(result.2, true);
    }

    #[test]
    fn spec_roundtrip_vec_of_options() {
        let val: Vec<Option<u32>> = vec![Some(1), None, Some(3), None, Some(5)];
        let buf = val.wit_pack();
        let result = Vec::<Option<u32>>::wit_unpack(&buf).unwrap();
        assert_eq!(result, vec![Some(1), None, Some(3), None, Some(5)]);
    }

    #[test]
    fn spec_roundtrip_option_vec_string() {
        let val: Option<Vec<String>> = Some(vec!["a".into(), "bb".into(), "ccc".into()]);
        let buf = val.wit_pack();
        let result = Option::<Vec<String>>::wit_unpack(&buf).unwrap();
        assert_eq!(
            result,
            Some(vec!["a".into(), "bb".into(), "ccc".into()])
        );

        let val: Option<Vec<String>> = None;
        let buf = val.wit_pack();
        assert_eq!(Option::<Vec<String>>::wit_unpack(&buf).unwrap(), None);
    }

    #[test]
    fn spec_roundtrip_deeply_nested_tuples() {
        let val: ((u8, u16), (u32, u64)) = ((1, 2), (3, 4));
        let buf = val.wit_pack();
        let result = <((u8, u16), (u32, u64))>::wit_unpack(&buf).unwrap();
        assert_eq!(result, ((1, 2), (3, 4)));
    }

    // ========================================================================
    // 15. Error handling
    // ========================================================================

    #[test]
    fn spec_buffer_too_small_error() {
        // u32 needs 4 bytes
        assert!(matches!(
            u32::wit_unpack(&[0, 0]),
            Err(WitViewError::BufferTooSmall { expected: 4, actual: 2 })
        ));
    }

    #[test]
    fn spec_string_out_of_bounds_error() {
        // Craft a buffer where the string ptr/len point past the end
        let mut buf = vec![0u8; 8];
        // ptr = 100, len = 5 -- way past buffer end
        buf[0..4].copy_from_slice(&100u32.to_le_bytes());
        buf[4..8].copy_from_slice(&5u32.to_le_bytes());
        let view = WitView::from_buffer(&buf);
        assert!(matches!(
            view.read_string(0),
            Err(WitViewError::StringOutOfBounds { .. })
        ));
    }

    #[test]
    fn spec_list_out_of_bounds_error() {
        let mut buf = vec![0u8; 8];
        // ptr = 50, count = 10 -- way past buffer
        buf[0..4].copy_from_slice(&50u32.to_le_bytes());
        buf[4..8].copy_from_slice(&10u32.to_le_bytes());
        let view = WitView::from_buffer(&buf);
        assert!(matches!(
            view.read_list::<u32>(0),
            Err(WitViewError::ListOutOfBounds { .. })
        ));
    }

    #[test]
    fn spec_invalid_utf8_error() {
        // Create a buffer with invalid UTF-8 string data
        let mut buf = vec![0u8; 12];
        // ptr = 8, len = 4
        buf[0..4].copy_from_slice(&8u32.to_le_bytes());
        buf[4..8].copy_from_slice(&4u32.to_le_bytes());
        // Invalid UTF-8 sequence
        buf[8] = 0xFF;
        buf[9] = 0xFE;
        buf[10] = 0x80;
        buf[11] = 0x81;
        let view = WitView::from_buffer(&buf);
        assert!(matches!(
            view.read_string(0),
            Err(WitViewError::InvalidUtf8 { .. })
        ));
    }

    // ========================================================================
    // 16. Option<T> with various payload types -- verify padding bytes
    // ========================================================================

    #[test]
    fn spec_option_f64_layout_and_encoding() {
        // option<f64>: disc(1) + 7 pad + f64(8) = 16, alignment 8
        assert_eq!(Option::<f64>::wit_alignment(), 8);
        assert_eq!(Option::<f64>::wit_size(), 16);

        let v: Option<f64> = Some(std::f64::consts::PI);
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 16);
        assert_eq!(buf[0], 1); // discriminant
        // Payload at offset 8
        let payload = f64::from_le_bytes(buf[8..16].try_into().unwrap());
        assert_eq!(payload, std::f64::consts::PI);
    }

    #[test]
    fn spec_option_option_u8_layout() {
        // option<option<u8>>
        // Inner: option<u8> = size 2, alignment 1
        // Outer: disc(1) + pad(0) + inner(2) = 3, alignment 1
        assert_eq!(Option::<Option<u8>>::wit_alignment(), 1);
        assert_eq!(Option::<Option<u8>>::wit_size(), 3);

        let v: Option<Option<u8>> = Some(Some(42));
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 3);
        assert_eq!(buf[0], 1); // outer discriminant (some)
        assert_eq!(buf[1], 1); // inner discriminant (some)
        assert_eq!(buf[2], 42); // payload
    }

    // ========================================================================
    // 17. Vec with aligned element types
    // ========================================================================

    #[test]
    fn spec_vec_u16_alignment() {
        // list<u16>: elements should be contiguous with u16 alignment
        let v: Vec<u16> = vec![0x1111, 0x2222, 0x3333];
        let buf = v.wit_pack();
        let arr_ptr = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        // Array start should be 2-byte aligned (trivially true for any even offset)
        assert_eq!(arr_ptr % 2, 0);
        // Verify elements
        let result = Vec::<u16>::wit_unpack(&buf).unwrap();
        assert_eq!(result, vec![0x1111, 0x2222, 0x3333]);
    }

    #[test]
    fn spec_vec_of_tuples() {
        // list<tuple<u8, u32>>: each element is 8 bytes (1+3pad+4), alignment 4
        let v: Vec<(u8, u32)> = vec![(1, 100), (2, 200), (3, 300)];
        let buf = v.wit_pack();
        let result = Vec::<(u8, u32)>::wit_unpack(&buf).unwrap();
        assert_eq!(result, vec![(1, 100), (2, 200), (3, 300)]);
    }

    // ========================================================================
    // 18. Spec edge cases
    // ========================================================================

    #[test]
    fn spec_zero_values() {
        assert_eq!(0u8.wit_pack(), vec![0]);
        assert_eq!(0u16.wit_pack(), vec![0, 0]);
        assert_eq!(0u32.wit_pack(), vec![0, 0, 0, 0]);
        assert_eq!(0u64.wit_pack(), vec![0, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(0.0f32.wit_pack(), vec![0, 0, 0, 0]);
        assert_eq!(0.0f64.wit_pack(), vec![0, 0, 0, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn spec_max_values() {
        assert_eq!(u8::wit_unpack(&u8::MAX.wit_pack()).unwrap(), u8::MAX);
        assert_eq!(u16::wit_unpack(&u16::MAX.wit_pack()).unwrap(), u16::MAX);
        assert_eq!(u32::wit_unpack(&u32::MAX.wit_pack()).unwrap(), u32::MAX);
        assert_eq!(u64::wit_unpack(&u64::MAX.wit_pack()).unwrap(), u64::MAX);
        assert_eq!(i8::wit_unpack(&i8::MAX.wit_pack()).unwrap(), i8::MAX);
        assert_eq!(i8::wit_unpack(&i8::MIN.wit_pack()).unwrap(), i8::MIN);
        assert_eq!(i16::wit_unpack(&i16::MAX.wit_pack()).unwrap(), i16::MAX);
        assert_eq!(i16::wit_unpack(&i16::MIN.wit_pack()).unwrap(), i16::MIN);
        assert_eq!(i32::wit_unpack(&i32::MAX.wit_pack()).unwrap(), i32::MAX);
        assert_eq!(i32::wit_unpack(&i32::MIN.wit_pack()).unwrap(), i32::MIN);
        assert_eq!(i64::wit_unpack(&i64::MAX.wit_pack()).unwrap(), i64::MAX);
        assert_eq!(i64::wit_unpack(&i64::MIN.wit_pack()).unwrap(), i64::MIN);
    }

    #[test]
    fn spec_f32_special_values() {
        assert_eq!(f32::wit_unpack(&f32::INFINITY.wit_pack()).unwrap(), f32::INFINITY);
        assert_eq!(f32::wit_unpack(&f32::NEG_INFINITY.wit_pack()).unwrap(), f32::NEG_INFINITY);
        assert!(f32::wit_unpack(&f32::NAN.wit_pack()).unwrap().is_nan());
    }

    #[test]
    fn spec_f64_special_values() {
        assert_eq!(f64::wit_unpack(&f64::INFINITY.wit_pack()).unwrap(), f64::INFINITY);
        assert_eq!(f64::wit_unpack(&f64::NEG_INFINITY.wit_pack()).unwrap(), f64::NEG_INFINITY);
        assert!(f64::wit_unpack(&f64::NAN.wit_pack()).unwrap().is_nan());
    }

    #[test]
    fn spec_large_string() {
        let s: String = "x".repeat(10_000);
        let buf = s.wit_pack();
        let len = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(len, 10_000);
        assert_eq!(String::wit_unpack(&buf).unwrap(), s);
    }

    #[test]
    fn spec_large_vec() {
        let v: Vec<u32> = (0..1000).collect();
        let buf = v.wit_pack();
        let count = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        assert_eq!(count, 1000);
        let result = Vec::<u32>::wit_unpack(&buf).unwrap();
        assert_eq!(result, (0..1000).collect::<Vec<u32>>());
    }

    // ========================================================================
    // 19. char type (Unicode scalar value as u32)
    // ========================================================================

    #[test]
    fn spec_char_layout() {
        // Spec: CharType => alignment 4, size 4
        assert_eq!(char::wit_alignment(), 4);
        assert_eq!(char::wit_size(), 4);
    }

    #[test]
    fn spec_char_encoding_ascii() {
        let c = 'A';
        let buf = c.wit_pack();
        assert_eq!(buf.len(), 4);
        assert_eq!(u32::from_le_bytes(buf[0..4].try_into().unwrap()), 0x41);
    }

    #[test]
    fn spec_char_encoding_emoji() {
        // U+1F600 = grinning face
        let c = '\u{1F600}';
        let buf = c.wit_pack();
        assert_eq!(buf.len(), 4);
        assert_eq!(u32::from_le_bytes(buf[0..4].try_into().unwrap()), 0x1F600);
    }

    #[test]
    fn spec_char_encoding_max() {
        // U+10FFFF = maximum valid Unicode scalar value
        let c = '\u{10FFFF}';
        let buf = c.wit_pack();
        assert_eq!(u32::from_le_bytes(buf[0..4].try_into().unwrap()), 0x10FFFF);
    }

    #[test]
    fn spec_char_roundtrip() {
        for c in ['A', '\u{00e9}', '\u{1F600}', '\u{10FFFF}', '\0'] {
            let buf = c.wit_pack();
            assert_eq!(char::wit_unpack(&buf).unwrap(), c);
        }
    }

    #[test]
    fn spec_char_invalid_surrogate() {
        // 0xD800 is a surrogate — not a valid Unicode scalar value
        let mut buf = vec![0u8; 4];
        buf[0..4].copy_from_slice(&0xD800u32.to_le_bytes());
        assert!(matches!(
            char::wit_unpack(&buf),
            Err(WitViewError::InvalidChar { value: 0xD800 })
        ));
    }

    #[test]
    fn spec_char_invalid_too_large() {
        // 0x110000 is past the valid Unicode range
        let mut buf = vec![0u8; 4];
        buf[0..4].copy_from_slice(&0x110000u32.to_le_bytes());
        assert!(matches!(
            char::wit_unpack(&buf),
            Err(WitViewError::InvalidChar { value: 0x110000 })
        ));
    }

    #[test]
    fn spec_char_view_read() {
        let c = '\u{00e9}'; // e-acute
        let buf = c.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert_eq!(view.read_char(0).unwrap(), '\u{00e9}');
    }

    #[test]
    fn spec_char_mutation() {
        let c = 'A';
        let mut buf = c.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            assert_eq!(m.read_char(0), Some('A'));
            m.write_char(0, 'Z');
            assert_eq!(m.read_char(0), Some('Z'));
        }
        assert_eq!(char::wit_unpack(&buf).unwrap(), 'Z');
    }

    #[test]
    fn spec_char_in_tuple() {
        let val: (char, u32) = ('\u{1F600}', 42);
        let buf = val.wit_pack();
        let result = <(char, u32)>::wit_unpack(&buf).unwrap();
        assert_eq!(result, ('\u{1F600}', 42));
    }

    #[test]
    fn spec_char_in_option() {
        let val: Option<char> = Some('\u{00e9}');
        let buf = val.wit_pack();
        assert_eq!(Option::<char>::wit_unpack(&buf).unwrap(), Some('\u{00e9}'));

        let val: Option<char> = None;
        let buf = val.wit_pack();
        assert_eq!(Option::<char>::wit_unpack(&buf).unwrap(), None);
    }

    #[test]
    fn spec_char_in_vec() {
        let val: Vec<char> = vec!['h', 'e', 'l', 'l', 'o'];
        let buf = val.wit_pack();
        assert_eq!(Vec::<char>::wit_unpack(&buf).unwrap(), vec!['h', 'e', 'l', 'l', 'o']);
    }

    // ========================================================================
    // 20. result<T, E> type
    // ========================================================================

    #[test]
    fn spec_result_u32_u8_layout() {
        // result<u32, u8> = variant { ok(u32), err(u8) }
        // discriminant: u8 (1 byte)
        // max_case_align = max(u32=4, u8=1) = 4
        // payload_offset = align_to(1, 4) = 4
        // max_case_size = max(4, 1) = 4
        // size = 4 + 4 = 8, align = 4
        assert_eq!(Result::<u32, u8>::wit_alignment(), 4);
        assert_eq!(Result::<u32, u8>::wit_size(), 8);
    }

    #[test]
    fn spec_result_u8_u8_layout() {
        // result<u8, u8>: disc(1) + max(1,1) = 2, align 1
        assert_eq!(Result::<u8, u8>::wit_alignment(), 1);
        assert_eq!(Result::<u8, u8>::wit_size(), 2);
    }

    #[test]
    fn spec_result_u64_u32_layout() {
        // result<u64, u32>: disc(1) + 7pad + max(8,4)=8 = 16, align 8
        assert_eq!(Result::<u64, u32>::wit_alignment(), 8);
        assert_eq!(Result::<u64, u32>::wit_size(), 16);
    }

    #[test]
    fn spec_result_unit_string_layout() {
        // result<(), string>: disc(1) + 3pad + max(0,8)=8 = 12, align 4
        assert_eq!(Result::<(), String>::wit_alignment(), 4);
        assert_eq!(Result::<(), String>::wit_size(), 12);
    }

    #[test]
    fn spec_result_encoding_ok() {
        let v: Result<u32, u8> = Ok(42);
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 0); // ok = discriminant 0
        assert_eq!(u32::from_le_bytes(buf[4..8].try_into().unwrap()), 42);
    }

    #[test]
    fn spec_result_encoding_err() {
        let v: Result<u32, u8> = Err(7);
        let buf = v.wit_pack();
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 1); // err = discriminant 1
        assert_eq!(buf[4], 7); // err payload at same offset
    }

    #[test]
    fn spec_result_roundtrip_ok_u32() {
        let v: Result<u32, u8> = Ok(0xDEADBEEF);
        let buf = v.wit_pack();
        assert_eq!(Result::<u32, u8>::wit_unpack(&buf).unwrap(), Ok(0xDEADBEEF));
    }

    #[test]
    fn spec_result_roundtrip_err_u8() {
        let v: Result<u32, u8> = Err(255);
        let buf = v.wit_pack();
        assert_eq!(Result::<u32, u8>::wit_unpack(&buf).unwrap(), Err(255));
    }

    #[test]
    fn spec_result_roundtrip_ok_string() {
        let v: Result<String, u32> = Ok("hello".into());
        let buf = v.wit_pack();
        assert_eq!(
            Result::<String, u32>::wit_unpack(&buf).unwrap(),
            Ok("hello".into())
        );
    }

    #[test]
    fn spec_result_roundtrip_err_string() {
        let v: Result<u32, String> = Err("error message".into());
        let buf = v.wit_pack();
        assert_eq!(
            Result::<u32, String>::wit_unpack(&buf).unwrap(),
            Err("error message".into())
        );
    }

    #[test]
    fn spec_result_roundtrip_both_strings() {
        let v: Result<String, String> = Ok("success".into());
        let buf = v.wit_pack();
        assert_eq!(
            Result::<String, String>::wit_unpack(&buf).unwrap(),
            Ok("success".into())
        );

        let v: Result<String, String> = Err("failure".into());
        let buf = v.wit_pack();
        assert_eq!(
            Result::<String, String>::wit_unpack(&buf).unwrap(),
            Err("failure".into())
        );
    }

    #[test]
    fn spec_result_u8_u8_roundtrip() {
        let ok: Result<u8, u8> = Ok(42);
        let buf = ok.wit_pack();
        assert_eq!(buf.len(), 2);
        assert_eq!(buf[0], 0);
        assert_eq!(buf[1], 42);
        assert_eq!(Result::<u8, u8>::wit_unpack(&buf).unwrap(), Ok(42));

        let err: Result<u8, u8> = Err(99);
        let buf = err.wit_pack();
        assert_eq!(buf[0], 1);
        assert_eq!(buf[1], 99);
        assert_eq!(Result::<u8, u8>::wit_unpack(&buf).unwrap(), Err(99));
    }

    #[test]
    fn spec_result_nested() {
        // result<result<u32, u8>, String>
        let v: Result<Result<u32, u8>, String> = Ok(Ok(42));
        let buf = v.wit_pack();
        assert_eq!(
            Result::<Result<u32, u8>, String>::wit_unpack(&buf).unwrap(),
            Ok(Ok(42))
        );

        let v: Result<Result<u32, u8>, String> = Ok(Err(7));
        let buf = v.wit_pack();
        assert_eq!(
            Result::<Result<u32, u8>, String>::wit_unpack(&buf).unwrap(),
            Ok(Err(7))
        );

        let v: Result<Result<u32, u8>, String> = Err("bad".into());
        let buf = v.wit_pack();
        assert_eq!(
            Result::<Result<u32, u8>, String>::wit_unpack(&buf).unwrap(),
            Err("bad".into())
        );
    }

    #[test]
    fn spec_result_in_vec() {
        let v: Vec<Result<u32, u8>> = vec![Ok(1), Err(2), Ok(3)];
        let buf = v.wit_pack();
        assert_eq!(
            Vec::<Result<u32, u8>>::wit_unpack(&buf).unwrap(),
            vec![Ok(1), Err(2), Ok(3)]
        );
    }

    #[test]
    fn spec_result_in_option() {
        let v: Option<Result<u32, u8>> = Some(Ok(42));
        let buf = v.wit_pack();
        assert_eq!(
            Option::<Result<u32, u8>>::wit_unpack(&buf).unwrap(),
            Some(Ok(42))
        );

        let v: Option<Result<u32, u8>> = None;
        let buf = v.wit_pack();
        assert_eq!(
            Option::<Result<u32, u8>>::wit_unpack(&buf).unwrap(),
            None
        );
    }

    #[test]
    fn spec_result_unit_ok() {
        // result<(), string> — Ok variant has no payload
        let v: Result<(), String> = Ok(());
        let buf = v.wit_pack();
        assert_eq!(buf[0], 0); // ok discriminant
        assert_eq!(Result::<(), String>::wit_unpack(&buf).unwrap(), Ok(()));
    }

    #[test]
    fn spec_result_unit_err() {
        let v: Result<(), String> = Err("oops".into());
        let buf = v.wit_pack();
        assert_eq!(buf[0], 1); // err discriminant
        assert_eq!(
            Result::<(), String>::wit_unpack(&buf).unwrap(),
            Err("oops".into())
        );
    }

    #[test]
    fn spec_result_invalid_discriminant() {
        // Craft a buffer with discriminant = 2 (invalid for result)
        let mut buf = vec![0u8; 8];
        buf[0] = 2;
        assert!(matches!(
            Result::<u32, u8>::wit_unpack(&buf),
            Err(WitViewError::InvalidDiscriminant { value: 2, max: 1 })
        ));
    }

    #[test]
    fn spec_result_view_discriminant() {
        let v: Result<u32, u8> = Ok(42);
        let buf = v.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert_eq!(view.read_result_discriminant(0), 0);
        let payload = view.result_payload_view::<u32, u8>(0);
        assert_eq!(payload.read_u32(0), 42);

        let v: Result<u32, u8> = Err(7);
        let buf = v.wit_pack();
        let view = WitView::from_buffer(&buf);
        assert_eq!(view.read_result_discriminant(0), 1);
        let payload = view.result_payload_view::<u32, u8>(0);
        assert_eq!(payload.read_u8(0), 7);
    }

    #[test]
    fn spec_result_mutation() {
        let v: Result<u32, u8> = Ok(42);
        let mut buf = v.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            assert_eq!(m.read_result_discriminant(0), 0);
            let payload_off = m.result_payload_offset::<u32, u8>(0);
            assert_eq!(payload_off, 4);
            m.write_u32(payload_off, 999);
        }
        assert_eq!(Result::<u32, u8>::wit_unpack(&buf).unwrap(), Ok(999));
    }

    // ========================================================================
    // 21. WitFlags type
    // ========================================================================

    #[test]
    fn spec_flags_0_layout() {
        use crate::wit::layout::WitFlags;
        assert_eq!(WitFlags::<0>::wit_size(), 0);
        assert_eq!(WitFlags::<0>::wit_alignment(), 1);
    }

    #[test]
    fn spec_flags_1_layout() {
        use crate::wit::layout::WitFlags;
        assert_eq!(WitFlags::<1>::wit_size(), 1);
        assert_eq!(WitFlags::<1>::wit_alignment(), 1);
    }

    #[test]
    fn spec_flags_8_layout() {
        use crate::wit::layout::WitFlags;
        assert_eq!(WitFlags::<8>::wit_size(), 1);
        assert_eq!(WitFlags::<8>::wit_alignment(), 1);
    }

    #[test]
    fn spec_flags_9_layout() {
        use crate::wit::layout::WitFlags;
        assert_eq!(WitFlags::<9>::wit_size(), 2);
        assert_eq!(WitFlags::<9>::wit_alignment(), 2);
    }

    #[test]
    fn spec_flags_16_layout() {
        use crate::wit::layout::WitFlags;
        assert_eq!(WitFlags::<16>::wit_size(), 2);
        assert_eq!(WitFlags::<16>::wit_alignment(), 2);
    }

    #[test]
    fn spec_flags_17_layout() {
        use crate::wit::layout::WitFlags;
        assert_eq!(WitFlags::<17>::wit_size(), 4);
        assert_eq!(WitFlags::<17>::wit_alignment(), 4);
    }

    #[test]
    fn spec_flags_33_layout() {
        use crate::wit::layout::WitFlags;
        // 33 flags = 2 x u32 = 8 bytes
        assert_eq!(WitFlags::<33>::wit_size(), 8);
        assert_eq!(WitFlags::<33>::wit_alignment(), 4);
    }

    #[test]
    fn spec_flags_roundtrip_u8() {
        use crate::wit::layout::WitFlags;
        let mut flags = WitFlags::<8>::new();
        flags.set(0, true);
        flags.set(3, true);
        flags.set(7, true);

        let buf = flags.wit_pack();
        assert_eq!(buf.len(), 1);
        assert_eq!(buf[0], 0b1000_1001); // bits 0, 3, 7

        let result = WitFlags::<8>::wit_unpack(&buf).unwrap();
        assert!(result.get(0));
        assert!(!result.get(1));
        assert!(!result.get(2));
        assert!(result.get(3));
        assert!(!result.get(4));
        assert!(!result.get(5));
        assert!(!result.get(6));
        assert!(result.get(7));
    }

    #[test]
    fn spec_flags_roundtrip_u16() {
        use crate::wit::layout::WitFlags;
        let mut flags = WitFlags::<16>::new();
        flags.set(0, true);
        flags.set(8, true);
        flags.set(15, true);

        let buf = flags.wit_pack();
        assert_eq!(buf.len(), 2);

        let result = WitFlags::<16>::wit_unpack(&buf).unwrap();
        assert!(result.get(0));
        assert!(result.get(8));
        assert!(result.get(15));
        assert!(!result.get(1));
    }

    #[test]
    fn spec_flags_roundtrip_u32() {
        use crate::wit::layout::WitFlags;
        let mut flags = WitFlags::<32>::new();
        flags.set(0, true);
        flags.set(16, true);
        flags.set(31, true);

        let buf = flags.wit_pack();
        assert_eq!(buf.len(), 4);

        let result = WitFlags::<32>::wit_unpack(&buf).unwrap();
        assert!(result.get(0));
        assert!(result.get(16));
        assert!(result.get(31));
        assert!(!result.get(1));
    }

    #[test]
    fn spec_flags_roundtrip_multi_chunk() {
        use crate::wit::layout::WitFlags;
        let mut flags = WitFlags::<64>::new();
        flags.set(0, true);
        flags.set(31, true);
        flags.set(32, true);
        flags.set(63, true);

        let buf = flags.wit_pack();
        assert_eq!(buf.len(), 8); // 2 x u32

        let result = WitFlags::<64>::wit_unpack(&buf).unwrap();
        assert!(result.get(0));
        assert!(result.get(31));
        assert!(result.get(32));
        assert!(result.get(63));
        assert!(!result.get(1));
        assert!(!result.get(33));
    }

    #[test]
    fn spec_flags_empty() {
        use crate::wit::layout::WitFlags;
        let flags = WitFlags::<0>::new();
        let buf = flags.wit_pack();
        assert_eq!(buf.len(), 0);
        let result = WitFlags::<0>::wit_unpack(&buf).unwrap();
        assert_eq!(result, WitFlags::<0>::new());
    }

    #[test]
    fn spec_flags_all_set() {
        use crate::wit::layout::WitFlags;
        let mut flags = WitFlags::<8>::new();
        for i in 0..8 {
            flags.set(i, true);
        }
        let buf = flags.wit_pack();
        assert_eq!(buf[0], 0xFF);

        let result = WitFlags::<8>::wit_unpack(&buf).unwrap();
        for i in 0..8 {
            assert!(result.get(i));
        }
    }

    #[test]
    fn spec_flags_in_option() {
        use crate::wit::layout::WitFlags;
        let mut flags = WitFlags::<8>::new();
        flags.set(2, true);
        let val: Option<WitFlags<8>> = Some(flags);
        let buf = val.wit_pack();
        let result = Option::<WitFlags<8>>::wit_unpack(&buf).unwrap();
        assert!(result.unwrap().get(2));

        let val: Option<WitFlags<8>> = None;
        let buf = val.wit_pack();
        assert_eq!(Option::<WitFlags<8>>::wit_unpack(&buf).unwrap(), None);
    }

    // ========================================================================
    // 22. Tuple pack/unpack completeness
    // ========================================================================

    #[test]
    fn spec_tuple_1_roundtrip() {
        let val: (u32,) = (42,);
        let buf = val.wit_pack();
        assert_eq!(<(u32,)>::wit_unpack(&buf).unwrap(), (42,));
    }

    #[test]
    fn spec_tuple_2_roundtrip() {
        let val: (u8, u64) = (0xFF, u64::MAX);
        let buf = val.wit_pack();
        assert_eq!(<(u8, u64)>::wit_unpack(&buf).unwrap(), (0xFF, u64::MAX));
    }

    #[test]
    fn spec_tuple_3_roundtrip() {
        let val: (bool, u16, f32) = (true, 1234, 3.14);
        let buf = val.wit_pack();
        let result = <(bool, u16, f32)>::wit_unpack(&buf).unwrap();
        assert_eq!(result.0, true);
        assert_eq!(result.1, 1234);
        assert!((result.2 - 3.14).abs() < 1e-6);
    }

    #[test]
    fn spec_tuple_4_roundtrip() {
        let val: (u8, u16, u32, u64) = (1, 2, 3, 4);
        let buf = val.wit_pack();
        assert_eq!(
            <(u8, u16, u32, u64)>::wit_unpack(&buf).unwrap(),
            (1, 2, 3, 4)
        );
    }

    #[test]
    fn spec_tuple_with_char_and_result() {
        let val: (char, Result<u32, u8>) = ('\u{1F600}', Ok(42));
        let buf = val.wit_pack();
        let result = <(char, Result<u32, u8>)>::wit_unpack(&buf).unwrap();
        assert_eq!(result.0, '\u{1F600}');
        assert_eq!(result.1, Ok(42));
    }

    // ========================================================================
    // 23. Variant (Rust enum) layout, pack, and unpack
    // ========================================================================

    use crate::wit::layout::{discriminant_size, variant_layout, variant_payload_offset};

    #[test]
    fn variant_discriminant_size() {
        assert_eq!(discriminant_size(1), 1);
        assert_eq!(discriminant_size(2), 1);
        assert_eq!(discriminant_size(256), 1);
        assert_eq!(discriminant_size(257), 2);
        assert_eq!(discriminant_size(65536), 2);
        assert_eq!(discriminant_size(65537), 4);
    }

    #[test]
    fn variant_layout_basic() {
        // 3-case variant with payloads: u32(size=4,align=4), f64(size=8,align=8), unit(size=0,align=1)
        let cases = vec![(4, 4), (8, 8), (0, 1)];
        let (size, align) = variant_layout(3, &cases);
        // disc=1(u8), max_payload_align=8, payload_offset=align_up(1,8)=8
        // total = align_up(8+8, 8) = 16
        assert_eq!(align, 8);
        assert_eq!(size, 16);
    }

    #[test]
    fn variant_layout_all_unit() {
        // 3 unit cases: all payloads (0, 1)
        let cases = vec![(0, 1), (0, 1), (0, 1)];
        let (size, align) = variant_layout(3, &cases);
        // disc=1(u8), max_payload_align=1, payload_offset=1
        // total = align_up(1+0, 1) = 1
        assert_eq!(align, 1);
        assert_eq!(size, 1);
    }

    #[test]
    fn variant_payload_offset_basic() {
        // 3 cases, max payload align = 4
        assert_eq!(variant_payload_offset(3, 4), 4);
        // 3 cases, max payload align = 8
        assert_eq!(variant_payload_offset(3, 8), 8);
        // 3 cases, max payload align = 1
        assert_eq!(variant_payload_offset(3, 1), 1);
    }

    // ── Derived enum tests ────────────────────────────────────────────────

    #[derive(WitPack, WitUnpack, PartialEq, Debug)]
    #[fracpack(fracpack_mod = "crate")]
    enum SimpleVariant {
        A(u32),
        B(f64),
        None,
    }

    #[test]
    fn variant_roundtrip_case_a() {
        let val = SimpleVariant::A(42);
        let packed = val.wit_pack();
        let unpacked = SimpleVariant::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }

    #[test]
    fn variant_roundtrip_case_b() {
        let val = SimpleVariant::B(std::f64::consts::PI);
        let packed = val.wit_pack();
        let unpacked = SimpleVariant::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }

    #[test]
    fn variant_roundtrip_unit_case() {
        let val = SimpleVariant::None;
        let packed = val.wit_pack();
        let unpacked = SimpleVariant::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }

    #[test]
    fn variant_wire_layout() {
        // SimpleVariant: 3 cases, disc=u8(1)
        // Case payloads: A(u32) -> size=4,align=4; B(f64) -> size=8,align=8; None -> size=0,align=1
        // max_payload_align = 8, max_payload_size = 8
        // variant_align = max(1, 8) = 8
        // payload_offset = align_up(1, 8) = 8
        // total = align_up(8 + 8, 8) = 16
        assert_eq!(SimpleVariant::wit_size(), 16);
        assert_eq!(SimpleVariant::wit_alignment(), 8);

        // Case A(42): disc=0, payload at offset 8 is u32 LE 42
        let val = SimpleVariant::A(42);
        let buf = val.wit_pack();
        assert_eq!(buf.len(), 16);
        assert_eq!(buf[0], 0); // discriminant = 0
        // Payload at byte 8
        assert_eq!(u32::from_le_bytes(buf[8..12].try_into().unwrap()), 42);

        // Case B(PI): disc=1, payload at offset 8 is f64 LE
        let val = SimpleVariant::B(std::f64::consts::PI);
        let buf = val.wit_pack();
        assert_eq!(buf[0], 1); // discriminant = 1
        let pi_bytes = std::f64::consts::PI.to_le_bytes();
        assert_eq!(&buf[8..16], &pi_bytes);

        // Case None: disc=2, no meaningful payload
        let val = SimpleVariant::None;
        let buf = val.wit_pack();
        assert_eq!(buf[0], 2); // discriminant = 2
    }

    #[test]
    fn variant_invalid_discriminant() {
        // Craft a buffer with invalid discriminant = 3
        let mut buf = vec![0u8; 16];
        buf[0] = 3;
        assert!(matches!(
            SimpleVariant::wit_unpack(&buf),
            Err(WitViewError::InvalidDiscriminant { value: 3, max: 2 })
        ));
    }

    // Multi-field tuple variant
    #[derive(WitPack, WitUnpack, PartialEq, Debug)]
    #[fracpack(fracpack_mod = "crate")]
    enum MultiField {
        Rect(f64, f64),
        Circle(f32),
    }

    #[test]
    fn variant_multi_field_tuple_roundtrip() {
        let val = MultiField::Rect(3.0, 4.0);
        let packed = val.wit_pack();
        let unpacked = MultiField::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);

        let val = MultiField::Circle(2.5);
        let packed = val.wit_pack();
        let unpacked = MultiField::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }

    // Named (struct) variant
    #[derive(WitPack, WitUnpack, PartialEq, Debug)]
    #[fracpack(fracpack_mod = "crate")]
    enum NamedFields {
        Point { x: f32, y: f32 },
        Label { text: String },
        Empty,
    }

    #[test]
    fn variant_named_fields_roundtrip() {
        let val = NamedFields::Point { x: 1.0, y: 2.0 };
        let packed = val.wit_pack();
        let unpacked = NamedFields::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);

        let val = NamedFields::Label { text: "hello".into() };
        let packed = val.wit_pack();
        let unpacked = NamedFields::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);

        let val = NamedFields::Empty;
        let packed = val.wit_pack();
        let unpacked = NamedFields::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }

    // Variant as a struct field
    #[derive(WitPack, WitUnpack, PartialEq, Debug)]
    #[fracpack(fracpack_mod = "crate")]
    enum Color {
        Red,
        Green,
        Blue,
    }

    #[derive(WitPack, WitUnpack, PartialEq, Debug)]
    #[fracpack(fracpack_mod = "crate")]
    struct ShapeWithColor {
        kind: SimpleVariant,
        color: Color,
        scale: f32,
    }

    #[test]
    fn variant_as_struct_field_roundtrip() {
        let val = ShapeWithColor {
            kind: SimpleVariant::A(100),
            color: Color::Green,
            scale: 2.5,
        };
        let packed = val.wit_pack();
        let unpacked = ShapeWithColor::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }

    #[test]
    fn variant_all_unit_layout() {
        // Color: 3 unit cases, all payloads zero-size
        // disc=1(u8), max_payload_align=1, payload_offset=1
        // total = align_up(1+0, 1) = 1
        assert_eq!(Color::wit_size(), 1);
        assert_eq!(Color::wit_alignment(), 1);

        let packed = Color::Red.wit_pack();
        assert_eq!(packed, vec![0]);
        let packed = Color::Green.wit_pack();
        assert_eq!(packed, vec![1]);
        let packed = Color::Blue.wit_pack();
        assert_eq!(packed, vec![2]);

        assert_eq!(Color::wit_unpack(&[0]).unwrap(), Color::Red);
        assert_eq!(Color::wit_unpack(&[1]).unwrap(), Color::Green);
        assert_eq!(Color::wit_unpack(&[2]).unwrap(), Color::Blue);
    }

    // Two-case variant (similar to Option but custom)
    #[derive(WitPack, WitUnpack, PartialEq, Debug)]
    #[fracpack(fracpack_mod = "crate")]
    enum MaybeInt {
        Nothing,
        Just(u32),
    }

    #[test]
    fn variant_two_case_roundtrip() {
        let val = MaybeInt::Nothing;
        let packed = val.wit_pack();
        let unpacked = MaybeInt::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);

        let val = MaybeInt::Just(999);
        let packed = val.wit_pack();
        let unpacked = MaybeInt::wit_unpack(&packed).unwrap();
        assert_eq!(val, unpacked);
    }
}
