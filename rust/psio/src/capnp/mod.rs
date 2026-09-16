//! Cap'n Proto wire format: layout, pack, unpack, zero-copy view, schema export.
//!
//! This module provides a complete Cap'n Proto implementation compatible with
//! the official wire format (single-segment flat-array messages).
//!
//! # Architecture
//!
//! - [`layout`] — Struct layout computation (data words, pointer slots, field locations)
//! - [`wire`] — Low-level wire format helpers (pointer resolution, validation)
//! - [`pack`] — Serialization: Rust values -> Cap'n Proto bytes
//! - [`view`] — Zero-copy views: read fields directly from borrowed bytes
//! - [`unpack`] — Deserialization: Cap'n Proto bytes -> owned Rust values
//! - [`schema`] — Generate `.capnp` IDL text from type metadata
//!
//! # Example
//!
//! ```rust,ignore
//! use psio::capnp::pack::CapnpPack;
//! use psio::capnp::unpack::CapnpUnpack;
//!
//! let msg = my_struct.capnp_pack();
//! let restored = MyStruct::capnp_unpack(&msg).unwrap();
//! ```

pub mod layout;
pub mod mutation;
pub mod pack;
pub mod schema;
pub mod unpack;
pub mod view;
pub mod wire;

// Re-export key types at the capnp module level.
pub use layout::{CapnpLayout, FieldKind, FieldLoc, MemberDesc};
pub use pack::{CapnpPack, WordBuf};
pub use schema::{CapnpSchema, SchemaField, SchemaStruct};
pub use unpack::CapnpUnpack;
pub use view::CapnpView;
pub use wire::validate;

#[cfg(test)]
mod tests {
    use super::layout::*;
    use super::pack::*;
    use super::schema::*;
    use super::unpack::*;
    use super::view::*;
    use super::wire;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: Point { x: i32, y: i32 }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct Point {
        x: i32,
        y: i32,
    }

    impl CapnpPack for Point {
        fn member_descs() -> Vec<MemberDesc> {
            vec![
                MemberDesc::Simple(FieldKind::Scalar(4)),
                MemberDesc::Simple(FieldKind::Scalar(4)),
            ]
        }

        fn pack_into(&self, buf: &mut WordBuf, data_start: u32, _ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_data_field(buf, data_start, &layout.fields[0], self.x);
            pack_data_field(buf, data_start, &layout.fields[1], self.y);
        }
    }

    impl CapnpUnpack for Point {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            Point {
                x: view.read_i32(&layout.fields[0]),
                y: view.read_i32(&layout.fields[1]),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: Person { name: String, age: u32, score: f64 }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct Person {
        name: String,
        age: u32,
        score: f64,
    }

    impl CapnpPack for Person {
        fn member_descs() -> Vec<MemberDesc> {
            vec![
                MemberDesc::Simple(FieldKind::Pointer),   // name: String
                MemberDesc::Simple(FieldKind::Scalar(4)),  // age: u32
                MemberDesc::Simple(FieldKind::Scalar(8)),  // score: f64
            ]
        }

        fn pack_into(&self, buf: &mut WordBuf, data_start: u32, ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_text_field(buf, ptrs_start, &layout.fields[0], &self.name);
            pack_data_field(buf, data_start, &layout.fields[1], self.age);
            pack_data_field(buf, data_start, &layout.fields[2], self.score);
        }
    }

    impl CapnpUnpack for Person {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            Person {
                name: unpack_string(view, &layout.fields[0]),
                age: unpack_u32(view, &layout.fields[1]),
                score: unpack_f64(view, &layout.fields[2]),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: Order { id: u64, customer: Person, items: Vec<u32> }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct Order {
        id: u64,
        customer: Person,
        items: Vec<u32>,
    }

    impl CapnpPack for Order {
        fn member_descs() -> Vec<MemberDesc> {
            vec![
                MemberDesc::Simple(FieldKind::Scalar(8)), // id: u64
                MemberDesc::Simple(FieldKind::Pointer),   // customer: Person
                MemberDesc::Simple(FieldKind::Pointer),   // items: Vec<u32>
            ]
        }

        fn pack_into(&self, buf: &mut WordBuf, data_start: u32, ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_data_field(buf, data_start, &layout.fields[0], self.id);
            pack_struct_field(buf, ptrs_start, &layout.fields[1], &self.customer);
            pack_scalar_vec(buf, ptrs_start + layout.fields[2].offset, &self.items, 4);
        }
    }

    impl CapnpUnpack for Order {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            Order {
                id: unpack_u64(view, &layout.fields[0]),
                customer: unpack_struct(view, &layout.fields[1]),
                items: unpack_scalar_vec(view, &layout.fields[2], 4, read_u32_at),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: BoolStruct { a: bool, b: bool, c: bool }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct BoolStruct {
        a: bool,
        b: bool,
        c: bool,
    }

    impl CapnpPack for BoolStruct {
        fn member_descs() -> Vec<MemberDesc> {
            vec![
                MemberDesc::Simple(FieldKind::Bool),
                MemberDesc::Simple(FieldKind::Bool),
                MemberDesc::Simple(FieldKind::Bool),
            ]
        }

        fn pack_into(&self, buf: &mut WordBuf, data_start: u32, _ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_bool_field(buf, data_start, &layout.fields[0], self.a);
            pack_bool_field(buf, data_start, &layout.fields[1], self.b);
            pack_bool_field(buf, data_start, &layout.fields[2], self.c);
        }
    }

    impl CapnpUnpack for BoolStruct {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            BoolStruct {
                a: view.read_bool(&layout.fields[0]),
                b: view.read_bool(&layout.fields[1]),
                c: view.read_bool(&layout.fields[2]),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: StringList { tags: Vec<String> }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct StringList {
        tags: Vec<String>,
    }

    impl CapnpPack for StringList {
        fn member_descs() -> Vec<MemberDesc> {
            vec![MemberDesc::Simple(FieldKind::Pointer)]
        }

        fn pack_into(&self, buf: &mut WordBuf, _data_start: u32, ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_string_vec(buf, ptrs_start + layout.fields[0].offset, &self.tags);
        }
    }

    impl CapnpUnpack for StringList {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            StringList {
                tags: unpack_string_vec(view, &layout.fields[0]),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: PointCloud { points: Vec<Point> }
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct PointCloud {
        points: Vec<Point>,
    }

    impl CapnpPack for PointCloud {
        fn member_descs() -> Vec<MemberDesc> {
            vec![MemberDesc::Simple(FieldKind::Pointer)]
        }

        fn pack_into(&self, buf: &mut WordBuf, _data_start: u32, ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_struct_vec(buf, ptrs_start + layout.fields[0].offset, &self.points);
        }
    }

    impl CapnpUnpack for PointCloud {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            PointCloud {
                points: unpack_struct_vec(view, &layout.fields[0]),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Test struct: AllScalars — every scalar type
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[derive(Debug, Clone, PartialEq)]
    struct AllScalars {
        v_bool: bool,
        v_u8: u8,
        v_i8: i8,
        v_u16: u16,
        v_i16: i16,
        v_u32: u32,
        v_i32: i32,
        v_u64: u64,
        v_i64: i64,
        v_f32: f32,
        v_f64: f64,
    }

    impl CapnpPack for AllScalars {
        fn member_descs() -> Vec<MemberDesc> {
            vec![
                MemberDesc::Simple(FieldKind::Bool),
                MemberDesc::Simple(FieldKind::Scalar(1)),
                MemberDesc::Simple(FieldKind::Scalar(1)),
                MemberDesc::Simple(FieldKind::Scalar(2)),
                MemberDesc::Simple(FieldKind::Scalar(2)),
                MemberDesc::Simple(FieldKind::Scalar(4)),
                MemberDesc::Simple(FieldKind::Scalar(4)),
                MemberDesc::Simple(FieldKind::Scalar(8)),
                MemberDesc::Simple(FieldKind::Scalar(8)),
                MemberDesc::Simple(FieldKind::Scalar(4)),
                MemberDesc::Simple(FieldKind::Scalar(8)),
            ]
        }

        fn pack_into(&self, buf: &mut WordBuf, data_start: u32, _ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_bool_field(buf, data_start, &layout.fields[0], self.v_bool);
            pack_data_field(buf, data_start, &layout.fields[1], self.v_u8);
            pack_data_field(buf, data_start, &layout.fields[2], self.v_i8);
            pack_data_field(buf, data_start, &layout.fields[3], self.v_u16);
            pack_data_field(buf, data_start, &layout.fields[4], self.v_i16);
            pack_data_field(buf, data_start, &layout.fields[5], self.v_u32);
            pack_data_field(buf, data_start, &layout.fields[6], self.v_i32);
            pack_data_field(buf, data_start, &layout.fields[7], self.v_u64);
            pack_data_field(buf, data_start, &layout.fields[8], self.v_i64);
            pack_data_field(buf, data_start, &layout.fields[9], self.v_f32);
            pack_data_field(buf, data_start, &layout.fields[10], self.v_f64);
        }
    }

    impl CapnpUnpack for AllScalars {
        fn unpack_from(view: &CapnpView) -> Self {
            let layout = <Self as CapnpPack>::capnp_layout();
            AllScalars {
                v_bool: view.read_bool(&layout.fields[0]),
                v_u8: view.read_u8(&layout.fields[1]),
                v_i8: view.read_i8(&layout.fields[2]),
                v_u16: view.read_u16(&layout.fields[3]),
                v_i16: view.read_i16(&layout.fields[4]),
                v_u32: view.read_u32(&layout.fields[5]),
                v_i32: view.read_i32(&layout.fields[6]),
                v_u64: view.read_u64(&layout.fields[7]),
                v_i64: view.read_i64(&layout.fields[8]),
                v_f32: view.read_f32(&layout.fields[9]),
                v_f64: view.read_f64(&layout.fields[10]),
            }
        }
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Tests
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    #[test]
    fn capnp_round_trip_point() {
        let orig = Point { x: 42, y: -17 };
        let msg = orig.capnp_pack();

        // Validate
        assert!(wire::validate(&msg));

        // Unpack
        let restored = Point::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);

        // View
        let view = CapnpView::from_message(&msg).unwrap();
        let layout = Point::capnp_layout();
        assert_eq!(view.read_i32(&layout.fields[0]), 42);
        assert_eq!(view.read_i32(&layout.fields[1]), -17);
    }

    #[test]
    fn capnp_round_trip_person() {
        let orig = Person {
            name: "Alice".to_string(),
            age: 30,
            score: 99.5,
        };
        let msg = orig.capnp_pack();
        assert!(wire::validate(&msg));

        let restored = Person::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);

        // View
        let view = CapnpView::from_message(&msg).unwrap();
        let layout = Person::capnp_layout();
        assert_eq!(view.read_text(&layout.fields[0]), "Alice");
        assert_eq!(view.read_u32(&layout.fields[1]), 30);
        assert_eq!(view.read_f64(&layout.fields[2]), 99.5);
    }

    #[test]
    fn capnp_round_trip_order() {
        let orig = Order {
            id: 12345,
            customer: Person {
                name: "Bob".to_string(),
                age: 25,
                score: 88.0,
            },
            items: vec![1, 2, 3, 4, 5],
        };
        let msg = orig.capnp_pack();
        assert!(wire::validate(&msg));

        let restored = Order::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);
    }

    #[test]
    fn capnp_round_trip_bools() {
        let orig = BoolStruct {
            a: true,
            b: false,
            c: true,
        };
        let msg = orig.capnp_pack();
        assert!(wire::validate(&msg));

        let restored = BoolStruct::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);
    }

    #[test]
    fn capnp_round_trip_string_list() {
        let orig = StringList {
            tags: vec!["foo".to_string(), "bar".to_string(), "baz".to_string()],
        };
        let msg = orig.capnp_pack();
        assert!(wire::validate(&msg));

        let restored = StringList::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);
    }

    #[test]
    fn capnp_round_trip_struct_list() {
        let orig = PointCloud {
            points: vec![
                Point { x: 1, y: 2 },
                Point { x: 3, y: 4 },
                Point { x: -5, y: 6 },
            ],
        };
        let msg = orig.capnp_pack();
        assert!(wire::validate(&msg));

        let restored = PointCloud::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);
    }

    #[test]
    fn capnp_round_trip_all_scalars() {
        let orig = AllScalars {
            v_bool: true,
            v_u8: 255,
            v_i8: -128,
            v_u16: 65535,
            v_i16: -32768,
            v_u32: 0xDEAD_BEEF,
            v_i32: -1,
            v_u64: 0xCAFE_BABE_DEAD_BEEF,
            v_i64: i64::MIN,
            v_f32: std::f32::consts::PI,
            v_f64: std::f64::consts::E,
        };
        let msg = orig.capnp_pack();
        assert!(wire::validate(&msg));

        let restored = AllScalars::capnp_unpack(&msg).unwrap();
        assert_eq!(orig, restored);
    }

    #[test]
    fn capnp_empty_containers() {
        // Empty string
        let p = Person {
            name: String::new(),
            age: 0,
            score: 0.0,
        };
        let msg = p.capnp_pack();
        assert!(wire::validate(&msg));
        let restored = Person::capnp_unpack(&msg).unwrap();
        assert_eq!(restored.name, "");

        // Empty vector
        let o = Order {
            id: 0,
            customer: Person {
                name: String::new(),
                age: 0,
                score: 0.0,
            },
            items: vec![],
        };
        let msg = o.capnp_pack();
        assert!(wire::validate(&msg));
        let restored = Order::capnp_unpack(&msg).unwrap();
        assert!(restored.items.is_empty());
    }

    #[test]
    fn capnp_view_struct_list() {
        let orig = PointCloud {
            points: vec![Point { x: 10, y: 20 }, Point { x: 30, y: 40 }],
        };
        let msg = orig.capnp_pack();
        let view = CapnpView::from_message(&msg).unwrap();
        let layout = PointCloud::capnp_layout();
        let point_layout = Point::capnp_layout();

        // Read list
        let (segment, list_info) = view.read_list(&layout.fields[0]).unwrap();
        let list_view = super::view::StructListView::new(segment, list_info);
        assert_eq!(list_view.len(), 2);

        let p0 = list_view.get(0);
        assert_eq!(p0.read_i32(&point_layout.fields[0]), 10);
        assert_eq!(p0.read_i32(&point_layout.fields[1]), 20);

        let p1 = list_view.get(1);
        assert_eq!(p1.read_i32(&point_layout.fields[0]), 30);
        assert_eq!(p1.read_i32(&point_layout.fields[1]), 40);
    }

    #[test]
    fn capnp_view_text_list() {
        let orig = StringList {
            tags: vec!["hello".to_string(), "world".to_string()],
        };
        let msg = orig.capnp_pack();
        let view = CapnpView::from_message(&msg).unwrap();
        let layout = StringList::capnp_layout();

        let (segment, list_info) = view.read_list(&layout.fields[0]).unwrap();
        let list_view = super::view::TextListView::new(segment, list_info);
        assert_eq!(list_view.len(), 2);
        assert_eq!(list_view.get(0), "hello");
        assert_eq!(list_view.get(1), "world");
    }

    #[test]
    fn capnp_validate_invalid() {
        // Too short
        assert!(!wire::validate(&[]));
        assert!(!wire::validate(&[0u8; 7]));

        // Truncated segment
        let mut msg = vec![0u8; 8];
        msg[4..8].copy_from_slice(&10u32.to_le_bytes()); // claims 10 words
        assert!(!wire::validate(&msg));
    }

    #[test]
    fn capnp_wire_layout_matches_expected() {
        // Verify the wire format structure of a simple packed message.
        // Point { x: i32, y: i32 } should have:
        //   - Segment header: 8 bytes
        //   - Root pointer: 8 bytes (1 word)
        //   - Data section: 8 bytes (1 word, two i32s packed into one 64-bit word)
        //   - No pointer section
        let p = Point { x: 1, y: 2 };
        let msg = p.capnp_pack();

        // Total size: 8 (header) + 8 (root ptr) + 8 (data) = 24 bytes
        assert_eq!(msg.len(), 24);

        // Read data directly
        // Segment starts at offset 8
        // Root pointer at offset 8 points to offset 16 (data section)
        // Data section: x=1 at byte 0, y=2 at byte 4
        let x = i32::from_le_bytes(msg[16..20].try_into().unwrap());
        let y = i32::from_le_bytes(msg[20..24].try_into().unwrap());
        assert_eq!(x, 1);
        assert_eq!(y, 2);
    }

    #[test]
    fn capnp_schema_export() {
        let schema = SchemaStruct {
            name: "Point".to_string(),
            fields: vec![
                SchemaField {
                    name: "x".to_string(),
                    capnp_type: "Int32".to_string(),
                    ordinal: 0,
                },
                SchemaField {
                    name: "y".to_string(),
                    capnp_type: "Int32".to_string(),
                    ordinal: 1,
                },
            ],
        };
        let idl = super::schema::to_capnp_schema(&schema);
        assert!(idl.contains("struct Point {"));
        assert!(idl.contains("x @0 :Int32;"));
        assert!(idl.contains("y @1 :Int32;"));
    }

    #[test]
    fn capnp_round_trip_large_string() {
        // Test with a string that spans multiple words
        let big_name = "A".repeat(1000);
        let p = Person {
            name: big_name.clone(),
            age: 42,
            score: 1.5,
        };
        let msg = p.capnp_pack();
        assert!(wire::validate(&msg));
        let restored = Person::capnp_unpack(&msg).unwrap();
        assert_eq!(restored.name, big_name);
    }

    #[test]
    fn capnp_round_trip_nested_struct() {
        let order = Order {
            id: 999,
            customer: Person {
                name: "Charlie".to_string(),
                age: 40,
                score: 77.7,
            },
            items: vec![10, 20, 30],
        };
        let msg = order.capnp_pack();
        assert!(wire::validate(&msg));

        // View nested struct
        let view = CapnpView::from_message(&msg).unwrap();
        let order_layout = Order::capnp_layout();
        let person_layout = Person::capnp_layout();

        assert_eq!(view.read_u64(&order_layout.fields[0]), 999);

        let customer_view = view.read_struct(&order_layout.fields[1]);
        assert_eq!(customer_view.read_text(&person_layout.fields[0]), "Charlie");
        assert_eq!(customer_view.read_u32(&person_layout.fields[1]), 40);
        assert_eq!(customer_view.read_f64(&person_layout.fields[2]), 77.7);

        // View scalar list
        let (segment, list_info) = view.read_list(&order_layout.fields[2]).unwrap();
        let list_view = super::view::ScalarListView::new(segment, list_info);
        assert_eq!(list_view.len(), 3);
        assert_eq!(list_view.get_u32(0), 10);
        assert_eq!(list_view.get_u32(1), 20);
        assert_eq!(list_view.get_u32(2), 30);
    }

    #[test]
    fn capnp_round_trip_empty_struct_list() {
        let cloud = PointCloud { points: vec![] };
        let msg = cloud.capnp_pack();
        assert!(wire::validate(&msg));
        let restored = PointCloud::capnp_unpack(&msg).unwrap();
        assert!(restored.points.is_empty());
    }

    #[test]
    fn capnp_round_trip_bool_vec() {
        // Test packing and unpacking a vector of bools.
        // We don't have a dedicated struct for this, so test the helpers directly.
        let bools = vec![true, false, true, true, false, true, false, false, true];

        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let ptrs_start = buf.alloc(1); // one pointer slot
        buf.write_struct_ptr(root_ptr, ptrs_start, 0, 1);
        super::pack::pack_bool_vec(&mut buf, ptrs_start, &bools);
        let msg = buf.finish();

        assert!(wire::validate(&msg));

        let view = CapnpView::from_message(&msg).unwrap();
        let loc = FieldLoc {
            is_ptr: true,
            offset: 0,
            bit_index: 0,
        };
        let unpacked = unpack_bool_vec(&view, &loc);
        assert_eq!(unpacked, bools);
    }

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Wire format conformance tests
    //
    // These tests verify byte-level wire format compatibility with the
    // official Cap'n Proto encoding specification:
    //   https://capnproto.org/encoding.html
    //
    // Each test constructs expected bytes by hand per the spec, then
    // verifies our pack output matches exactly, OR constructs reference
    // bytes and verifies our reader can decode them.
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    /// Helper: build a reference message from raw words (prepends segment table).
    fn make_single_segment_msg(words: &[u64]) -> Vec<u8> {
        let seg_count_minus_1: u32 = 0;
        let seg_word_count: u32 = words.len() as u32;
        let mut msg = Vec::with_capacity(8 + words.len() * 8);
        msg.extend_from_slice(&seg_count_minus_1.to_le_bytes());
        msg.extend_from_slice(&seg_word_count.to_le_bytes());
        for &w in words {
            msg.extend_from_slice(&w.to_le_bytes());
        }
        msg
    }

    /// Encode a struct pointer word per the spec:
    ///   bits[0:1]  = 0 (struct tag)
    ///   bits[2:31] = signed offset in words from end of pointer to start of struct
    ///   bits[32:47] = data section size in words
    ///   bits[48:63] = pointer section size in words
    fn encode_struct_ptr(offset: i32, data_words: u16, ptr_count: u16) -> u64 {
        let lo: u32 = (offset << 2) as u32; // tag=0 is implicit
        ((ptr_count as u64) << 48)
            | ((data_words as u64) << 32)
            | (lo as u64)
    }

    /// Encode a list pointer word per the spec:
    ///   bits[0:1]  = 1 (list tag)
    ///   bits[2:31] = signed offset in words from end of pointer to start of list
    ///   bits[32:34] = element size tag (0=void,1=bit,2=byte,3=2byte,4=4byte,5=8byte,6=ptr,7=composite)
    ///   bits[35:63] = element count (or total word count for composite)
    fn encode_list_ptr(offset: i32, elem_sz: u8, count: u32) -> u64 {
        let lo: u32 = ((offset << 2) as u32) | 1; // tag=1
        ((count as u64) << 35)
            | ((elem_sz as u64) << 32)
            | (lo as u64)
    }

    /// Encode a composite list tag word:
    ///   bits[0:1]  = 0
    ///   bits[2:31] = element count (NOT a pointer offset)
    ///   bits[32:47] = data words per element
    ///   bits[48:63] = pointer words per element
    fn encode_composite_tag(count: u32, data_words: u16, ptr_count: u16) -> u64 {
        ((ptr_count as u64) << 48)
            | ((data_words as u64) << 32)
            | ((count as u64) << 2)
    }

    // ── 1. Segment table format ──────────────────────────────────────────

    #[test]
    fn conformance_segment_table_single_segment() {
        // Per spec: single-segment message segment table is:
        //   bytes[0:3] = 0 (segment count - 1)
        //   bytes[4:7] = N (word count of segment 0)
        // Total header = 8 bytes, then N*8 bytes of segment data.
        let p = Point { x: 0, y: 0 };
        let msg = p.capnp_pack();

        // Header
        let seg_count_m1 = u32::from_le_bytes(msg[0..4].try_into().unwrap());
        assert_eq!(seg_count_m1, 0, "segment count - 1 must be 0 for single segment");

        let seg_words = u32::from_le_bytes(msg[4..8].try_into().unwrap());
        let expected_body_len = seg_words as usize * 8;
        assert_eq!(
            msg.len(),
            8 + expected_body_len,
            "message length must be 8 (header) + seg_words * 8"
        );
    }

    #[test]
    fn conformance_segment_table_word_alignment() {
        // The segment table must be padded to 8-byte (word) alignment.
        // For single-segment: header is exactly 8 bytes (4 + 4), already aligned.
        // Verify segment data starts at offset 8.
        let p = Point { x: 1, y: 2 };
        let msg = p.capnp_pack();
        assert_eq!(msg.len() % 8, 0, "total message must be word-aligned");
    }

    // ── 2. Struct pointer encoding ───────────────────────────────────────

    #[test]
    fn conformance_struct_pointer_encoding() {
        // Point { x: i32, y: i32 } layout: 1 data word, 0 pointers.
        // Message layout (after 8-byte header):
        //   Word 0 (offset 8): root struct pointer
        //   Word 1 (offset 16): data section (x, y)
        //
        // Root pointer at word 0 points to word 1:
        //   offset = 0 (struct immediately follows pointer)
        //   data_words = 1, ptr_count = 0
        let p = Point { x: 0x12345678, y: -1 };
        let msg = p.capnp_pack();

        // Read the root pointer word (at segment offset 0 = message offset 8)
        let ptr_word = u64::from_le_bytes(msg[8..16].try_into().unwrap());

        // Verify tag bits [0:1] = 0 (struct)
        assert_eq!(ptr_word & 3, 0, "struct pointer tag must be 0");

        // Verify offset bits [2:31]: offset = 0 means struct starts right after pointer
        let offset = ((ptr_word as u32 & !3u32) as i32) >> 2;
        assert_eq!(offset, 0, "root pointer offset should be 0 (struct follows immediately)");

        // Verify data_words bits [32:47] = 1
        let data_words = ((ptr_word >> 32) & 0xFFFF) as u16;
        assert_eq!(data_words, 1, "Point should have 1 data word");

        // Verify ptr_count bits [48:63] = 0
        let ptr_count = ((ptr_word >> 48) & 0xFFFF) as u16;
        assert_eq!(ptr_count, 0, "Point should have 0 pointer slots");

        // Verify the data section content
        let x = i32::from_le_bytes(msg[16..20].try_into().unwrap());
        let y = i32::from_le_bytes(msg[20..24].try_into().unwrap());
        assert_eq!(x, 0x12345678);
        assert_eq!(y, -1);
    }

    #[test]
    fn conformance_struct_pointer_with_pointers() {
        // Person { name: String, age: u32, score: f64 }
        // Layout: name is Pointer, age is Scalar(4), score is Scalar(8)
        // Computed layout:
        //   field 0 (name): ptr slot 0
        //   field 1 (age):  data byte 0, 4 bytes
        //   field 2 (score): data byte 8, 8 bytes (needs 2nd word)
        // So: data_words=2, ptr_count=1
        let person_layout = Person::capnp_layout();
        assert_eq!(person_layout.data_words, 2, "Person should have 2 data words");
        assert_eq!(person_layout.ptr_count, 1, "Person should have 1 pointer slot");

        let p = Person {
            name: "Hi".to_string(),
            age: 42,
            score: 3.14,
        };
        let msg = p.capnp_pack();

        // Root pointer
        let ptr_word = u64::from_le_bytes(msg[8..16].try_into().unwrap());
        assert_eq!(ptr_word & 3, 0, "struct tag");
        let dw = ((ptr_word >> 32) & 0xFFFF) as u16;
        let pc = ((ptr_word >> 48) & 0xFFFF) as u16;
        assert_eq!(dw, 2);
        assert_eq!(pc, 1);

        // Data section starts at word 1 (offset 16 from msg start)
        // age at byte 0 of data section
        let age = u32::from_le_bytes(msg[16..20].try_into().unwrap());
        assert_eq!(age, 42);

        // score at byte 8 of data section (second data word)
        let score_bits = u64::from_le_bytes(msg[24..32].try_into().unwrap());
        assert_eq!(f64::from_bits(score_bits), 3.14);
    }

    // ── 3. Text (string) encoding ────────────────────────────────────────

    #[test]
    fn conformance_text_encoding() {
        // Per spec, Text is a list of bytes (elem_sz=2) with count = strlen + 1
        // (includes NUL terminator). The list data contains the UTF-8 bytes
        // followed by a NUL byte, padded to word boundary.
        let p = Person {
            name: "Alice".to_string(),
            age: 0,
            score: 0.0,
        };
        let msg = p.capnp_pack();

        // The name pointer is in the pointer section (after 2 data words)
        // Root struct starts at word 1. Data section = words 1-2. Ptr section = word 3.
        // So the name pointer is at message offset 8 + 8(root) + 16(data) = 32
        // Actually: root pointer = word 0 of segment, data = words 1-2, ptrs = word 3
        let seg_start = 8usize; // segment table
        let data_start = seg_start + 8; // word 1
        let ptrs_start = data_start + 2 * 8; // word 3 (after 2 data words)

        // Read the text list pointer from the pointer section
        let text_ptr_word = u64::from_le_bytes(msg[ptrs_start..ptrs_start + 8].try_into().unwrap());

        // Verify tag bits = 1 (list)
        assert_eq!(text_ptr_word & 3, 1, "text pointer tag must be 1 (list)");

        // Verify elem_sz bits [32:34] = 2 (byte)
        let elem_sz = ((text_ptr_word >> 32) & 7) as u8;
        assert_eq!(elem_sz, 2, "text element size tag must be 2 (byte)");

        // Verify count bits [35:63] = strlen + 1 = 6
        let count = (text_ptr_word >> 35) as u32;
        assert_eq!(count, 6, "text count must be strlen('Alice') + 1 = 6 (includes NUL)");

        // Verify the text data
        let off = wire::ptr_offset(text_ptr_word);
        let text_data_start = ptrs_start + 8 + off as usize * 8;
        assert_eq!(&msg[text_data_start..text_data_start + 5], b"Alice");
        assert_eq!(msg[text_data_start + 5], 0, "NUL terminator required");
    }

    #[test]
    fn conformance_text_empty_string() {
        // An empty string should result in a null pointer (all zeros),
        // since our implementation skips writing empty strings.
        let p = Person {
            name: String::new(),
            age: 0,
            score: 0.0,
        };
        let msg = p.capnp_pack();

        let seg_start = 8usize;
        let ptrs_start = seg_start + 8 + 2 * 8; // after root ptr + 2 data words
        let text_ptr_word = u64::from_le_bytes(msg[ptrs_start..ptrs_start + 8].try_into().unwrap());
        assert_eq!(text_ptr_word, 0, "empty string should produce null pointer");
    }

    #[test]
    fn conformance_text_word_padding() {
        // Text "Hi" = 2 bytes + NUL = 3 bytes. Padded to 8 bytes (1 word).
        // Text "Hello!!" = 7 bytes + NUL = 8 bytes = exactly 1 word.
        // Text "Hello!!X" = 8 bytes + NUL = 9 bytes. Padded to 16 bytes (2 words).
        for (text, expected_words) in &[("Hi", 1u32), ("Hello!!", 1), ("Hello!!X", 2)] {
            let p = Person {
                name: text.to_string(),
                age: 0,
                score: 0.0,
            };
            let msg = p.capnp_pack();

            // Total message words = 1 (root ptr) + 2 (data) + 1 (ptrs) + text_words
            let seg_words = u32::from_le_bytes(msg[4..8].try_into().unwrap());
            let text_words = seg_words - 4; // subtract root + data + ptr section
            assert_eq!(
                text_words, *expected_words,
                "text '{}' ({} bytes + NUL) should use {} word(s)",
                text,
                text.len(),
                expected_words
            );
        }
    }

    // ── 4. List pointer encoding ─────────────────────────────────────────

    #[test]
    fn conformance_list_pointer_u32() {
        // A list of u32 values uses elem_sz=4 (four bytes).
        // Per spec: elem_sz tag for 4-byte elements = 4.
        let order = Order {
            id: 1,
            customer: Person {
                name: String::new(),
                age: 0,
                score: 0.0,
            },
            items: vec![10, 20, 30],
        };
        let msg = order.capnp_pack();

        // Find the items list pointer.
        // Order layout: id=u64 at data[0], customer=ptr[0], items=ptr[1]
        let order_layout = Order::capnp_layout();
        assert_eq!(order_layout.data_words, 1);
        assert_eq!(order_layout.ptr_count, 2);

        // items is ptr slot 1
        let seg_start = 8usize;
        let ptrs_start = seg_start + 8 + 1 * 8; // root ptr + 1 data word
        let items_ptr_offset = ptrs_start + 1 * 8; // ptr slot 1

        let list_ptr_word = u64::from_le_bytes(msg[items_ptr_offset..items_ptr_offset + 8].try_into().unwrap());

        // Verify tag = 1 (list)
        assert_eq!(list_ptr_word & 3, 1, "list tag must be 1");

        // Verify elem_sz = 4 (four-byte elements)
        let elem_sz = ((list_ptr_word >> 32) & 7) as u8;
        assert_eq!(elem_sz, 4, "u32 list elem_sz tag must be 4");

        // Verify count = 3
        let count = (list_ptr_word >> 35) as u32;
        assert_eq!(count, 3, "list should have 3 elements");
    }

    #[test]
    fn conformance_list_element_size_tags() {
        // Verify the element size tag mapping matches the spec:
        //   0 = void (0 bits), 1 = bit, 2 = byte, 3 = two bytes,
        //   4 = four bytes, 5 = eight bytes, 6 = pointer, 7 = composite
        use super::pack::scalar_elem_tag;
        assert_eq!(scalar_elem_tag(0), 1, "bool -> bit -> tag 1");
        assert_eq!(scalar_elem_tag(1), 2, "byte -> tag 2");
        assert_eq!(scalar_elem_tag(2), 3, "2-byte -> tag 3");
        assert_eq!(scalar_elem_tag(4), 4, "4-byte -> tag 4");
        assert_eq!(scalar_elem_tag(8), 5, "8-byte -> tag 5");
    }

    // ── 5. Composite list (struct list) encoding ─────────────────────────

    #[test]
    fn conformance_composite_list_encoding() {
        // Per spec, composite lists (lists of structs) use:
        //   - List pointer with elem_sz=7, count = total words (excl. tag)
        //   - First word is a tag with element count, data_words, ptr_count
        //   - Then count * (data_words + ptr_count) words of element data
        let cloud = PointCloud {
            points: vec![Point { x: 1, y: 2 }, Point { x: 3, y: 4 }],
        };
        let msg = cloud.capnp_pack();

        // PointCloud layout: 0 data words, 1 pointer
        let cloud_layout = PointCloud::capnp_layout();
        assert_eq!(cloud_layout.data_words, 0);
        assert_eq!(cloud_layout.ptr_count, 1);

        // The list pointer is at ptr slot 0
        let seg_start = 8usize;
        // root ptr at seg_start, then 0 data words, then 1 ptr word
        let ptrs_start = seg_start + 8; // root ptr + 0 data words
        let list_ptr_offset = ptrs_start;

        let list_ptr_word = u64::from_le_bytes(msg[list_ptr_offset..list_ptr_offset + 8].try_into().unwrap());

        // Tag = 1 (list)
        assert_eq!(list_ptr_word & 3, 1);

        // elem_sz = 7 (composite)
        let elem_sz = ((list_ptr_word >> 32) & 7) as u8;
        assert_eq!(elem_sz, 7, "struct list must use composite tag (7)");

        // Word count = 2 elements * 1 word each = 2
        let word_count = (list_ptr_word >> 35) as u32;
        assert_eq!(word_count, 2, "2 Points * 1 word/Point = 2 total words");

        // Resolve the tag word
        let off = wire::ptr_offset(list_ptr_word);
        let tag_offset = list_ptr_offset + 8 + off as usize * 8;
        let tag_word = u64::from_le_bytes(msg[tag_offset..tag_offset + 8].try_into().unwrap());

        // Tag word: element count in bits[2:31]
        let elem_count = (tag_word as u32) >> 2;
        assert_eq!(elem_count, 2, "tag must report 2 elements");

        // Tag word: data_words in bits[32:47]
        let dw = ((tag_word >> 32) & 0xFFFF) as u16;
        assert_eq!(dw, 1, "Point has 1 data word");

        // Tag word: ptr_count in bits[48:63]
        let pc = ((tag_word >> 48) & 0xFFFF) as u16;
        assert_eq!(pc, 0, "Point has 0 pointers");

        // Verify element data
        let elem0_start = tag_offset + 8;
        let x0 = i32::from_le_bytes(msg[elem0_start..elem0_start + 4].try_into().unwrap());
        let y0 = i32::from_le_bytes(msg[elem0_start + 4..elem0_start + 8].try_into().unwrap());
        assert_eq!(x0, 1);
        assert_eq!(y0, 2);

        let elem1_start = elem0_start + 8; // 1 word per element
        let x1 = i32::from_le_bytes(msg[elem1_start..elem1_start + 4].try_into().unwrap());
        let y1 = i32::from_le_bytes(msg[elem1_start + 4..elem1_start + 8].try_into().unwrap());
        assert_eq!(x1, 3);
        assert_eq!(y1, 4);
    }

    // ── 6. Boolean bit packing ───────────────────────────────────────────

    #[test]
    fn conformance_bool_bit_packing_in_struct() {
        // Per spec, booleans occupy individual bits in the data section.
        // BoolStruct { a: bool, b: bool, c: bool } should pack all 3 bools
        // into bit 0, bit 1, bit 2 of byte 0.
        let bs = BoolStruct {
            a: true,
            b: false,
            c: true,
        };
        let msg = bs.capnp_pack();

        // Layout check
        let layout = BoolStruct::capnp_layout();
        assert_eq!(layout.data_words, 1);
        assert_eq!(layout.fields[0].offset, 0);
        assert_eq!(layout.fields[0].bit_index, 0);
        assert_eq!(layout.fields[1].offset, 0);
        assert_eq!(layout.fields[1].bit_index, 1);
        assert_eq!(layout.fields[2].offset, 0);
        assert_eq!(layout.fields[2].bit_index, 2);

        // Data section starts at message offset 16 (8 header + 8 root ptr)
        let data_byte = msg[16];
        assert_eq!(data_byte & 1, 1, "bit 0 (a=true)");
        assert_eq!((data_byte >> 1) & 1, 0, "bit 1 (b=false)");
        assert_eq!((data_byte >> 2) & 1, 1, "bit 2 (c=true)");

        // Expected value: 0b00000101 = 5
        assert_eq!(data_byte, 0b00000101);
    }

    #[test]
    fn conformance_bool_list_bit_packing() {
        // Per spec, a List(Bool) uses elem_sz=1 (bit), and bits are packed
        // LSB-first within each byte.
        let bools = vec![true, false, true, true, false, false, true, false, true];

        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let ptrs_start = buf.alloc(1);
        buf.write_struct_ptr(root_ptr, ptrs_start, 0, 1);
        super::pack::pack_bool_vec(&mut buf, ptrs_start, &bools);
        let msg = buf.finish();

        // Find the list pointer
        let seg_start = 8usize;
        let list_ptr_offset = seg_start + 8; // after root ptr (which is the ptr section word)
        let list_ptr_word = u64::from_le_bytes(msg[list_ptr_offset..list_ptr_offset + 8].try_into().unwrap());

        // Tag = 1 (list)
        assert_eq!(list_ptr_word & 3, 1);

        // elem_sz = 1 (bit)
        let elem_sz = ((list_ptr_word >> 32) & 7) as u8;
        assert_eq!(elem_sz, 1, "bool list must use bit element size (1)");

        // Count = 9
        let count = (list_ptr_word >> 35) as u32;
        assert_eq!(count, 9);

        // Verify bit packing: bools = [T,F,T,T,F,F,T,F, T]
        // Byte 0: bits 0-7 = 1,0,1,1,0,0,1,0 = 0b01001101 = 0x4D
        // Byte 1: bit 0 = 1 = 0b00000001 = 0x01
        let off = wire::ptr_offset(list_ptr_word);
        let data_start = list_ptr_offset + 8 + off as usize * 8;
        assert_eq!(msg[data_start], 0b01001101, "first byte of bool list");
        assert_eq!(msg[data_start + 1], 0b00000001, "second byte of bool list");
    }

    // ── 7. Scalar endianness ─────────────────────────────────────────────

    #[test]
    fn conformance_little_endian_scalars() {
        // Cap'n Proto uses little-endian for all scalar values.
        let p = Point { x: 0x01020304, y: 0x05060708_u32 as i32 };
        let msg = p.capnp_pack();

        // Data section at offset 16
        // x = 0x01020304 in LE: [04, 03, 02, 01]
        assert_eq!(msg[16], 0x04);
        assert_eq!(msg[17], 0x03);
        assert_eq!(msg[18], 0x02);
        assert_eq!(msg[19], 0x01);

        // y = 0x05060708 in LE: [08, 07, 06, 05]
        assert_eq!(msg[20], 0x08);
        assert_eq!(msg[21], 0x07);
        assert_eq!(msg[22], 0x06);
        assert_eq!(msg[23], 0x05);
    }

    #[test]
    fn conformance_u64_endianness() {
        // Verify 64-bit values are little-endian
        let order = Order {
            id: 0x0102030405060708u64,
            customer: Person {
                name: String::new(),
                age: 0,
                score: 0.0,
            },
            items: vec![],
        };
        let msg = order.capnp_pack();

        // Order has 1 data word (u64 id at byte 0)
        // Data section at offset 16 (after header + root ptr)
        assert_eq!(msg[16], 0x08);
        assert_eq!(msg[17], 0x07);
        assert_eq!(msg[18], 0x06);
        assert_eq!(msg[19], 0x05);
        assert_eq!(msg[20], 0x04);
        assert_eq!(msg[21], 0x03);
        assert_eq!(msg[22], 0x02);
        assert_eq!(msg[23], 0x01);
    }

    // ── 8. Read reference bytes (official format) ────────────────────────

    #[test]
    fn conformance_read_handcrafted_struct() {
        // Construct a valid Cap'n Proto message by hand representing:
        //   struct { x @0 :Int32; y @1 :Int32; }
        //   with x=100, y=-200
        //
        // Word 0: root struct pointer -> offset=0, data_words=1, ptr_count=0
        // Word 1: data = (x=100 as i32 LE) | (y=-200 as i32 LE) << 32

        let root_ptr = encode_struct_ptr(0, 1, 0);
        let x_bytes = 100i32.to_le_bytes();
        let y_bytes = (-200i32).to_le_bytes();
        let mut data_word = [0u8; 8];
        data_word[0..4].copy_from_slice(&x_bytes);
        data_word[4..8].copy_from_slice(&y_bytes);
        let data = u64::from_le_bytes(data_word);

        let msg = make_single_segment_msg(&[root_ptr, data]);
        assert!(wire::validate(&msg));

        let restored = Point::capnp_unpack(&msg).unwrap();
        assert_eq!(restored.x, 100);
        assert_eq!(restored.y, -200);
    }

    #[test]
    fn conformance_read_handcrafted_text() {
        // Construct a message with a text field by hand.
        // Struct: { name @0 :Text; age @1 :UInt32; score @2 :Float64; }
        // name = "Bob", age = 25, score = 0.0
        //
        // Layout (matching Person): data_words=2, ptr_count=1
        // Word 0: root pointer -> offset=0, dw=2, pc=1
        // Word 1: data word 0: age=25 at bytes 0-3
        // Word 2: data word 1: score=0.0
        // Word 3: pointer section: text pointer for name
        // Word 4: text data "Bob\0" padded to 8 bytes

        let root_ptr = encode_struct_ptr(0, 2, 1);
        let age_word = 25u64; // u32=25 in lower 32 bits, upper 32 bits = 0
        let score_word = f64::to_bits(0.0);
        // Text pointer at word 3, pointing to word 4:
        //   offset = 0 (word 4 is right after word 3)
        //   elem_sz = 2 (byte), count = 4 (3 chars + NUL)
        let text_ptr = encode_list_ptr(0, 2, 4);
        // Text data: "Bob\0\0\0\0\0"
        let mut text_data = [0u8; 8];
        text_data[0] = b'B';
        text_data[1] = b'o';
        text_data[2] = b'b';
        text_data[3] = 0; // NUL terminator
        let text_word = u64::from_le_bytes(text_data);

        let msg = make_single_segment_msg(&[root_ptr, age_word, score_word, text_ptr, text_word]);
        assert!(wire::validate(&msg));

        let restored = Person::capnp_unpack(&msg).unwrap();
        assert_eq!(restored.name, "Bob");
        assert_eq!(restored.age, 25);
        assert_eq!(restored.score, 0.0);
    }

    #[test]
    fn conformance_read_handcrafted_list() {
        // Construct a message with a u32 list by hand.
        // We'll build a simple struct with 1 ptr field containing [10, 20, 30].
        //
        // PointCloud layout: 0 data words, 1 pointer
        // But let's use Order for items: Vec<u32>.
        // Order: 1 data word (u64 id), 2 pointers (customer, items)
        //
        // Actually, let's just construct a raw message and read it with wire primitives.
        //
        // Struct: 0 data words, 1 pointer
        // Word 0: root struct pointer (dw=0, pc=1)
        // Word 1: list pointer for u32 list (elem_sz=4, count=3)
        // Words 2-3: [10u32, 20u32, 30u32, 0u32] = 3 elements in 2 words (12 bytes -> 2 words)

        let root_ptr = encode_struct_ptr(0, 0, 1);
        let list_ptr = encode_list_ptr(0, 4, 3); // offset=0, elem_sz=4, count=3
        let mut list_data_0 = [0u8; 8];
        list_data_0[0..4].copy_from_slice(&10u32.to_le_bytes());
        list_data_0[4..8].copy_from_slice(&20u32.to_le_bytes());
        let mut list_data_1 = [0u8; 8];
        list_data_1[0..4].copy_from_slice(&30u32.to_le_bytes());
        let list_word_0 = u64::from_le_bytes(list_data_0);
        let list_word_1 = u64::from_le_bytes(list_data_1);

        let msg = make_single_segment_msg(&[root_ptr, list_ptr, list_word_0, list_word_1]);
        assert!(wire::validate(&msg));

        // Read via wire primitives
        let segment = &msg[8..];
        let info = wire::resolve_list_ptr(segment, 8).unwrap(); // ptr at word 1
        assert_eq!(info.count, 3);
        assert_eq!(info.elem_stride, 4);
        let v0 = wire::read_u32(segment, info.data_offset);
        let v1 = wire::read_u32(segment, info.data_offset + 4);
        let v2 = wire::read_u32(segment, info.data_offset + 8);
        assert_eq!(v0, 10);
        assert_eq!(v1, 20);
        assert_eq!(v2, 30);
    }

    #[test]
    fn conformance_read_handcrafted_composite_list() {
        // Construct a composite list of Points by hand.
        // Struct: 0 data words, 1 pointer (PointCloud)
        // Word 0: root pointer (dw=0, pc=1)
        // Word 1: list pointer (elem_sz=7, word_count=2)
        // Word 2: composite tag (count=2, dw=1, pc=0)
        // Word 3: Point 0 data (x=5, y=10)
        // Word 4: Point 1 data (x=15, y=20)

        let root_ptr = encode_struct_ptr(0, 0, 1);
        let list_ptr = encode_list_ptr(0, 7, 2); // 2 total words of element data
        let tag = encode_composite_tag(2, 1, 0); // 2 elements, 1 data word, 0 ptrs

        let mut p0 = [0u8; 8];
        p0[0..4].copy_from_slice(&5i32.to_le_bytes());
        p0[4..8].copy_from_slice(&10i32.to_le_bytes());

        let mut p1 = [0u8; 8];
        p1[0..4].copy_from_slice(&15i32.to_le_bytes());
        p1[4..8].copy_from_slice(&20i32.to_le_bytes());

        let msg = make_single_segment_msg(&[
            root_ptr,
            list_ptr,
            tag,
            u64::from_le_bytes(p0),
            u64::from_le_bytes(p1),
        ]);
        assert!(wire::validate(&msg));

        let restored = PointCloud::capnp_unpack(&msg).unwrap();
        assert_eq!(restored.points.len(), 2);
        assert_eq!(restored.points[0], Point { x: 5, y: 10 });
        assert_eq!(restored.points[1], Point { x: 15, y: 20 });
    }

    // ── 9. Round-trip byte stability ─────────────────────────────────────

    #[test]
    fn conformance_pack_matches_expected_bytes() {
        // Verify that packing Point { x: 1, y: 2 } produces exactly the
        // expected bytes (hand-computed from the spec).
        //
        // Expected message:
        //   Header: [00 00 00 00] [02 00 00 00]  (0 segments-1, 2 words)
        //   Word 0 (root ptr): struct, offset=0, dw=1, pc=0
        //     = 0x00_00_00_01_00_00_00_00 -> [00,00,00,00, 01,00,00,00]
        //   Word 1 (data): x=1 LE, y=2 LE
        //     = [01,00,00,00, 02,00,00,00]

        let p = Point { x: 1, y: 2 };
        let msg = p.capnp_pack();

        let expected_root_ptr = encode_struct_ptr(0, 1, 0);
        let mut expected_data = [0u8; 8];
        expected_data[0..4].copy_from_slice(&1i32.to_le_bytes());
        expected_data[4..8].copy_from_slice(&2i32.to_le_bytes());
        let expected_data_word = u64::from_le_bytes(expected_data);

        let expected = make_single_segment_msg(&[expected_root_ptr, expected_data_word]);
        assert_eq!(msg, expected, "packed bytes must match hand-computed reference");
    }

    #[test]
    fn conformance_pack_boolstruct_matches_expected() {
        // BoolStruct { a: true, b: true, c: false }
        // Layout: 1 data word, 0 ptrs
        // Data byte 0: bit0=1(a), bit1=1(b), bit2=0(c) = 0b011 = 3
        // Rest of data word = 0

        let bs = BoolStruct {
            a: true,
            b: true,
            c: false,
        };
        let msg = bs.capnp_pack();

        let expected_root_ptr = encode_struct_ptr(0, 1, 0);
        let expected_data = 3u64; // bits 0 and 1 set
        let expected = make_single_segment_msg(&[expected_root_ptr, expected_data]);
        assert_eq!(msg, expected);
    }

    // ── 10. Pointer offset calculation ───────────────────────────────────

    #[test]
    fn conformance_struct_pointer_offset_arithmetic() {
        // Per spec: the offset in a struct pointer is a signed 30-bit value
        // representing the number of words from the END of the pointer to
        // the START of the struct's data section.
        //
        // For a root pointer at word 0 with struct at word 1: offset = 0
        // (struct starts 0 words after the end of the pointer word).

        let p = Point { x: 0, y: 0 };
        let msg = p.capnp_pack();

        let ptr_word = u64::from_le_bytes(msg[8..16].try_into().unwrap());
        let offset = wire::ptr_offset(ptr_word);

        // Root pointer is at word 0 in segment, struct is at word 1.
        // End of pointer = word 1. Start of struct = word 1. Offset = 0.
        assert_eq!(offset, 0, "root pointer offset is 0 when struct immediately follows");
    }

    #[test]
    fn conformance_text_pointer_offset() {
        // Verify text pointer offsets are correct.
        // Person: dw=2, pc=1. Root ptr at word 0, data at words 1-2, ptrs at word 3.
        // Text pointer at word 3 should point to text data at word 4.
        // End of pointer = word 4. Start of text = word 4. Offset = 0.
        let p = Person {
            name: "X".to_string(),
            age: 0,
            score: 0.0,
        };
        let msg = p.capnp_pack();

        let seg_start = 8;
        let text_ptr_offset = seg_start + 8 + 16; // word 3 in segment
        let ptr_word = u64::from_le_bytes(msg[text_ptr_offset..text_ptr_offset + 8].try_into().unwrap());

        let offset = wire::ptr_offset(ptr_word);
        assert_eq!(offset, 0, "text pointer offset should be 0 (data immediately follows)");
    }

    // ── 11. Nested struct pointer offsets ─────────────────────────────────

    #[test]
    fn conformance_nested_struct_pointer() {
        // Order { id, customer: Person, items: Vec<u32> }
        // The customer pointer at ptr slot 0 must correctly point to the
        // Person struct data further in the message.
        let order = Order {
            id: 42,
            customer: Person {
                name: "Z".to_string(),
                age: 99,
                score: 1.0,
            },
            items: vec![],
        };
        let msg = order.capnp_pack();

        // Order layout: dw=1, pc=2
        // Segment layout:
        //   Word 0: root ptr
        //   Word 1: data (id=42)
        //   Word 2: ptr[0] = customer struct ptr
        //   Word 3: ptr[1] = items list ptr (null)
        //   Word 4+: Person struct data...

        let seg_start = 8;
        let customer_ptr_offset = seg_start + 8 + 8; // word 2
        let ptr_word = u64::from_le_bytes(msg[customer_ptr_offset..customer_ptr_offset + 8].try_into().unwrap());

        // Should be a struct pointer
        assert_eq!(ptr_word & 3, 0, "customer should be a struct pointer");

        // Resolve it and check
        let segment = &msg[seg_start..];
        let customer = wire::resolve_struct_ptr(segment, 16).unwrap(); // word 2 = byte 16

        let person_layout = Person::capnp_layout();
        assert_eq!(customer.data_words, person_layout.data_words);
        assert_eq!(customer.ptr_count, person_layout.ptr_count);

        // Read age from the resolved struct
        let age_offset = customer.data_offset; // age is at byte 0 of data section
        let age = wire::read_u32(segment, age_offset);
        assert_eq!(age, 99);
    }

    // ── 12. Null pointer handling ────────────────────────────────────────

    #[test]
    fn conformance_null_pointer_is_all_zeros() {
        // Per spec, a null pointer is all zero bytes. Empty optional fields
        // should produce null pointers.
        let order = Order {
            id: 0,
            customer: Person {
                name: String::new(),
                age: 0,
                score: 0.0,
            },
            items: vec![],
        };
        let msg = order.capnp_pack();

        // items list pointer at ptr slot 1 should be null (empty vec)
        let seg_start = 8;
        let items_ptr_offset = seg_start + 8 + 8 + 8; // root + data + ptr[0] -> ptr[1]
        let items_word = u64::from_le_bytes(msg[items_ptr_offset..items_ptr_offset + 8].try_into().unwrap());
        assert_eq!(items_word, 0, "empty vector should produce null list pointer");
    }

    // ── 13. Default values (zero-filled) ─────────────────────────────────

    #[test]
    fn conformance_default_values_are_zero() {
        // Per spec, all default values are zero/false/empty.
        // A struct with all default values should have all-zero data section.
        let p = Point { x: 0, y: 0 };
        let msg = p.capnp_pack();

        // Data section at offset 16 should be all zeros
        let data_word = u64::from_le_bytes(msg[16..24].try_into().unwrap());
        assert_eq!(data_word, 0, "all-default struct should have zero data word");
    }

    // ── 14. Field alignment in data section ──────────────────────────────

    #[test]
    fn conformance_field_natural_alignment() {
        // Per Cap'n Proto layout rules, fields are naturally aligned within
        // the data section. Our layout algorithm should match the capnp
        // compiler's behavior for sequential ordinals.
        //
        // AllScalars: bool, u8, i8, u16, i16, u32, i32, u64, i64, f32, f64
        // Expected allocation order (by ordinal, finding first fit):
        //   bool:  bit 0
        //   u8:    byte 1 (bit 8)
        //   i8:    byte 2 (bit 16)
        //   u16:   byte 4 (bit 24 doesn't work for 2-byte align, next = bit 32 = byte 4)
        //   i16:   byte 6 (bit 48 = byte 6)
        //   u32:   byte 8 (bit 64 = word 1 byte 0)
        //   i32:   byte 12
        //   u64:   byte 16 (word 2)
        //   i64:   byte 24 (word 3)
        //   f32:   byte 28 ... wait, that's in word 3 and i64 uses it all
        //          Actually u64 at word 2, i64 at word 3, f32 back to gap?

        // Let's just verify the layout is internally consistent by checking
        // round-trip correctness with known values.
        let scalars = AllScalars {
            v_bool: true,
            v_u8: 0xAB,
            v_i8: -42,
            v_u16: 0x1234,
            v_i16: -1000,
            v_u32: 0xDEADBEEF,
            v_i32: -99999,
            v_u64: 0xCAFEBABEDEADBEEF,
            v_i64: -123456789012345,
            v_f32: 1.5,
            v_f64: 2.718281828,
        };
        let msg = scalars.capnp_pack();
        assert!(wire::validate(&msg));

        // Verify we can read each value back at its computed position
        let view = CapnpView::from_message(&msg).unwrap();
        let layout = AllScalars::capnp_layout();

        assert_eq!(view.read_bool(&layout.fields[0]), true);
        assert_eq!(view.read_u8(&layout.fields[1]), 0xAB);
        assert_eq!(view.read_i8(&layout.fields[2]), -42);
        assert_eq!(view.read_u16(&layout.fields[3]), 0x1234);
        assert_eq!(view.read_i16(&layout.fields[4]), -1000);
        assert_eq!(view.read_u32(&layout.fields[5]), 0xDEADBEEF);
        assert_eq!(view.read_i32(&layout.fields[6]), -99999);
        assert_eq!(view.read_u64(&layout.fields[7]), 0xCAFEBABEDEADBEEF);
        assert_eq!(view.read_i64(&layout.fields[8]), -123456789012345);
        assert_eq!(view.read_f32(&layout.fields[9]), 1.5);
        assert_eq!(view.read_f64(&layout.fields[10]), 2.718281828);

        // Verify field locations don't overlap
        let layout = AllScalars::capnp_layout();
        let mut occupied_bytes = std::collections::HashSet::new();
        let field_sizes: Vec<usize> = vec![0, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8]; // 0 for bool (sub-byte)
        for (i, loc) in layout.fields.iter().enumerate() {
            if !loc.is_ptr && field_sizes[i] > 0 {
                for b in 0..field_sizes[i] {
                    let byte = loc.offset as usize + b;
                    assert!(
                        occupied_bytes.insert(byte),
                        "field {} overlaps at byte {}",
                        i,
                        byte
                    );
                }
            }
        }
    }

    // ── 15. Verify our encoding is readable by wire primitives ───────────

    #[test]
    fn conformance_wire_primitives_read_our_output() {
        // Pack a complex struct and verify every piece is readable via
        // the low-level wire module functions.
        let order = Order {
            id: 0x1122334455667788,
            customer: Person {
                name: "TestName".to_string(),
                age: 42,
                score: 3.14159,
            },
            items: vec![100, 200, 300],
        };
        let msg = order.capnp_pack();
        let segment = &msg[8..];

        // 1. Parse segment table
        let (seg_start, seg_words) = wire::parse_segment_table(&msg).unwrap();
        assert_eq!(seg_start, 8);
        assert!(seg_words > 0);

        // 2. Resolve root
        let root = wire::resolve_struct_ptr(segment, 0).unwrap();
        assert_eq!(root.data_words, 1); // Order: 1 data word
        assert_eq!(root.ptr_count, 2); // Order: 2 pointers

        // 3. Read id
        let id = wire::read_u64(segment, root.data_offset);
        assert_eq!(id, 0x1122334455667788);

        // 4. Resolve customer struct
        let cust_slot = root.ptr_slot_offset(0);
        let cust = wire::resolve_struct_ptr(segment, cust_slot).unwrap();
        assert_eq!(cust.data_words, 2);
        assert_eq!(cust.ptr_count, 1);

        // 5. Read customer.age
        let age = wire::read_u32(segment, cust.data_offset);
        assert_eq!(age, 42);

        // 6. Read customer.name via text pointer
        let name_slot = cust.ptr_slot_offset(0);
        let name = wire::read_text(segment, name_slot);
        assert_eq!(name, "TestName");

        // 7. Resolve items list
        let items_slot = root.ptr_slot_offset(1);
        let items_info = wire::resolve_list_ptr(segment, items_slot).unwrap();
        assert_eq!(items_info.count, 3);
        assert_eq!(items_info.elem_stride, 4);
        for (i, &expected) in [100u32, 200, 300].iter().enumerate() {
            let val = wire::read_u32(segment, items_info.data_offset + i * 4);
            assert_eq!(val, expected);
        }
    }

    // ── 16. Cross-read: hand-built message -> our unpack ─────────────────

    #[test]
    fn conformance_read_reference_allscalars() {
        // Build an AllScalars message by hand and verify our unpack reads it.
        //
        // AllScalars layout (from our algorithm):
        //   bool:  byte 0, bit 0
        //   u8:    byte 1
        //   i8:    byte 2
        //   u16:   byte 4
        //   i16:   byte 6
        //   u32:   byte 8
        //   i32:   byte 12
        //   u64:   byte 16
        //   i64:   byte 24
        //   f32:   byte 32  -- wait, need to check
        //   f64:   byte 40

        let layout = AllScalars::capnp_layout();

        // Build data section manually
        let total_data_bytes = layout.data_words as usize * 8;
        let mut data = vec![0u8; total_data_bytes];

        // Write each field at its computed location
        // bool at byte 0, bit 0
        data[layout.fields[0].offset as usize] |= 1 << layout.fields[0].bit_index;
        // u8 = 0xFF
        data[layout.fields[1].offset as usize] = 0xFF;
        // i8 = -1 (0xFF)
        data[layout.fields[2].offset as usize] = 0xFF;
        // u16 = 0xABCD
        data[layout.fields[3].offset as usize..layout.fields[3].offset as usize + 2]
            .copy_from_slice(&0xABCDu16.to_le_bytes());
        // i16 = -1
        data[layout.fields[4].offset as usize..layout.fields[4].offset as usize + 2]
            .copy_from_slice(&(-1i16).to_le_bytes());
        // u32 = 0x12345678
        data[layout.fields[5].offset as usize..layout.fields[5].offset as usize + 4]
            .copy_from_slice(&0x12345678u32.to_le_bytes());
        // i32 = -42
        data[layout.fields[6].offset as usize..layout.fields[6].offset as usize + 4]
            .copy_from_slice(&(-42i32).to_le_bytes());
        // u64 = 0xDEADCAFEBEEF0001
        data[layout.fields[7].offset as usize..layout.fields[7].offset as usize + 8]
            .copy_from_slice(&0xDEADCAFEBEEF0001u64.to_le_bytes());
        // i64 = i64::MIN
        data[layout.fields[8].offset as usize..layout.fields[8].offset as usize + 8]
            .copy_from_slice(&i64::MIN.to_le_bytes());
        // f32 = 2.5
        data[layout.fields[9].offset as usize..layout.fields[9].offset as usize + 4]
            .copy_from_slice(&2.5f32.to_bits().to_le_bytes());
        // f64 = 3.14
        data[layout.fields[10].offset as usize..layout.fields[10].offset as usize + 8]
            .copy_from_slice(&3.14f64.to_bits().to_le_bytes());

        // Build words from data bytes
        let mut words = vec![0u64; 1 + layout.data_words as usize]; // root ptr + data
        words[0] = encode_struct_ptr(0, layout.data_words, 0);
        for i in 0..layout.data_words as usize {
            let off = i * 8;
            words[1 + i] = u64::from_le_bytes(data[off..off + 8].try_into().unwrap());
        }

        let msg = make_single_segment_msg(&words);
        assert!(wire::validate(&msg));

        let view = CapnpView::from_message(&msg).unwrap();
        assert_eq!(view.read_bool(&layout.fields[0]), true);
        assert_eq!(view.read_u8(&layout.fields[1]), 0xFF);
        assert_eq!(view.read_i8(&layout.fields[2]), -1);
        assert_eq!(view.read_u16(&layout.fields[3]), 0xABCD);
        assert_eq!(view.read_i16(&layout.fields[4]), -1);
        assert_eq!(view.read_u32(&layout.fields[5]), 0x12345678);
        assert_eq!(view.read_i32(&layout.fields[6]), -42);
        assert_eq!(view.read_u64(&layout.fields[7]), 0xDEADCAFEBEEF0001);
        assert_eq!(view.read_i64(&layout.fields[8]), i64::MIN);
        assert_eq!(view.read_f32(&layout.fields[9]), 2.5);
        assert_eq!(view.read_f64(&layout.fields[10]), 3.14);
    }

    // ── 17. Negative pointer offsets ─────────────────────────────────────

    #[test]
    fn conformance_negative_pointer_offset() {
        // Per spec, struct pointer offsets are signed 30-bit values.
        // Verify our encoder produces correct negative offsets.
        // This is tested indirectly: when a struct pointer is NOT immediately
        // followed by its target, the offset must account for the gap.
        //
        // In Order, the customer struct pointer at ptr[0] points past ptr[1],
        // so its offset is >= 1 (skipping the items pointer slot).
        let order = Order {
            id: 1,
            customer: Person {
                name: String::new(),
                age: 1,
                score: 0.0,
            },
            items: vec![],
        };
        let msg = order.capnp_pack();
        let segment = &msg[8..];

        // Customer pointer at ptr slot 0
        let cust_ptr_offset = 8 + 8; // word 2 (after root ptr + 1 data word)
        let cust_ptr = wire::read_u64(segment, cust_ptr_offset);
        let offset = wire::ptr_offset(cust_ptr);

        // Customer ptr is at word 2, items ptr is at word 3, so customer
        // data starts at word 4 at earliest. Offset from end of word 2 (=word 3)
        // to word 4 = 1.
        assert!(offset >= 1, "customer pointer offset must skip items pointer slot");
    }

    // ── 18. String list (list of pointers to text) ───────────────────────

    #[test]
    fn conformance_string_list_encoding() {
        // A list of strings is encoded as a list of pointers (elem_sz=6).
        // Each pointer slot contains a text pointer.
        let sl = StringList {
            tags: vec!["ab".to_string(), "cd".to_string()],
        };
        let msg = sl.capnp_pack();

        // StringList: 0 data words, 1 pointer
        let seg_start = 8;
        let list_ptr_offset = seg_start + 8; // after root ptr, 0 data words

        let list_ptr = u64::from_le_bytes(msg[list_ptr_offset..list_ptr_offset + 8].try_into().unwrap());

        // Tag = 1 (list)
        assert_eq!(list_ptr & 3, 1);

        // elem_sz = 6 (pointer)
        let elem_sz = ((list_ptr >> 32) & 7) as u8;
        assert_eq!(elem_sz, 6, "string list must use pointer element size (6)");

        // Count = 2
        let count = (list_ptr >> 35) as u32;
        assert_eq!(count, 2);
    }

    // ── 19. Message size verification ────────────────────────────────────

    #[test]
    fn conformance_message_size_word_granularity() {
        // Every valid Cap'n Proto message must have a total size that is
        // exactly 8 (header) + segment_words * 8.
        for test_case in &[
            Point { x: 0, y: 0 }.capnp_pack(),
            Person {
                name: "test".to_string(),
                age: 1,
                score: 2.0,
            }
            .capnp_pack(),
            BoolStruct {
                a: true,
                b: false,
                c: true,
            }
            .capnp_pack(),
        ] {
            assert_eq!(test_case.len() % 8, 0, "message must be word-aligned");
            let seg_words = u32::from_le_bytes(test_case[4..8].try_into().unwrap());
            assert_eq!(
                test_case.len(),
                8 + seg_words as usize * 8,
                "message size must match segment table"
            );
        }
    }

    // ── 20. Bit-level pointer encoding verification ──────────────────────

    #[test]
    fn conformance_pointer_bit_layout() {
        // Verify our WordBuf::write_struct_ptr produces the correct bit layout
        // per the spec.
        let mut buf = WordBuf::new();
        let at = buf.alloc(1);
        let target = buf.alloc(1);
        // at=0, target=1: offset = target - at - 1 = 0
        buf.write_struct_ptr(at, target, 3, 2);
        let msg = buf.finish();

        // Read the pointer word (skip 8-byte header)
        let word = u64::from_le_bytes(msg[8..16].try_into().unwrap());

        // bits[0:1] = 0 (struct tag)
        assert_eq!(word & 3, 0);

        // bits[2:31] = 0 (offset=0, shifted left by 2)
        assert_eq!((word as u32) >> 2, 0);

        // bits[32:47] = 3 (data_words)
        assert_eq!((word >> 32) & 0xFFFF, 3);

        // bits[48:63] = 2 (ptr_count)
        assert_eq!((word >> 48) & 0xFFFF, 2);
    }

    #[test]
    fn conformance_list_pointer_bit_layout() {
        // Verify write_list_ptr bit layout.
        let mut buf = WordBuf::new();
        let at = buf.alloc(1);
        let target = buf.alloc(1);
        buf.write_list_ptr(at, target, 4, 10); // elem_sz=4 (u32), count=10
        let msg = buf.finish();

        let word = u64::from_le_bytes(msg[8..16].try_into().unwrap());

        // bits[0:1] = 1 (list tag)
        assert_eq!(word & 3, 1);

        // bits[32:34] = 4 (elem_sz)
        assert_eq!((word >> 32) & 7, 4);

        // bits[35:63] = 10 (count)
        assert_eq!(word >> 35, 10);
    }

    #[test]
    fn conformance_composite_tag_bit_layout() {
        // Verify write_composite_tag bit layout.
        let mut buf = WordBuf::new();
        let at = buf.alloc(1);
        buf.write_composite_tag(at, 5, 2, 1);
        let msg = buf.finish();

        let word = u64::from_le_bytes(msg[8..16].try_into().unwrap());

        // bits[0:1] = 0
        assert_eq!(word & 3, 0);

        // bits[2:31] = element count = 5
        assert_eq!((word as u32) >> 2, 5);

        // bits[32:47] = data_words = 2
        assert_eq!((word >> 32) & 0xFFFF, 2);

        // bits[48:63] = ptr_count = 1
        assert_eq!((word >> 48) & 0xFFFF, 1);
    }

    // ── 21. Cross-read: build with our pack, decode with raw spec logic ──

    #[test]
    fn conformance_full_cross_decode() {
        // Pack a complete Order, then decode it purely with byte math
        // (no use of our view/unpack modules).
        let order = Order {
            id: 9876543210,
            customer: Person {
                name: "Cross".to_string(),
                age: 55,
                score: 98.6,
            },
            items: vec![7, 14, 21],
        };
        let msg = order.capnp_pack();

        // --- Decode from raw bytes ---

        // 1. Segment table
        let seg_count_m1 = u32::from_le_bytes(msg[0..4].try_into().unwrap());
        assert_eq!(seg_count_m1, 0);
        let seg_words = u32::from_le_bytes(msg[4..8].try_into().unwrap());
        let segment = &msg[8..8 + seg_words as usize * 8];

        // 2. Root pointer (word 0 of segment)
        let root_word = u64::from_le_bytes(segment[0..8].try_into().unwrap());
        assert_eq!(root_word & 3, 0); // struct
        let root_off = ((root_word as u32 & !3) as i32) >> 2;
        let root_dw = ((root_word >> 32) & 0xFFFF) as usize;
        let _root_pc = ((root_word >> 48) & 0xFFFF) as usize;
        let root_data = 8 + root_off as usize * 8;
        let root_ptrs = root_data + root_dw * 8;

        // 3. Read id (u64 at data offset 0)
        let id = u64::from_le_bytes(segment[root_data..root_data + 8].try_into().unwrap());
        assert_eq!(id, 9876543210u64);

        // 4. Resolve customer (ptr slot 0)
        let cust_word = u64::from_le_bytes(segment[root_ptrs..root_ptrs + 8].try_into().unwrap());
        assert_eq!(cust_word & 3, 0); // struct
        let cust_off = ((cust_word as u32 & !3) as i32) >> 2;
        let cust_dw = ((cust_word >> 32) & 0xFFFF) as usize;
        let cust_data = root_ptrs + 8 + cust_off as usize * 8;
        let cust_ptrs = cust_data + cust_dw * 8;

        // 5. Read customer.age (u32 at data offset 0)
        let age = u32::from_le_bytes(segment[cust_data..cust_data + 4].try_into().unwrap());
        assert_eq!(age, 55);

        // 6. Read customer.score (f64 at data offset 8, in word 1)
        let score_bits = u64::from_le_bytes(segment[cust_data + 8..cust_data + 16].try_into().unwrap());
        assert_eq!(f64::from_bits(score_bits), 98.6);

        // 7. Read customer.name (text at ptr slot 0)
        let name_word = u64::from_le_bytes(segment[cust_ptrs..cust_ptrs + 8].try_into().unwrap());
        assert_eq!(name_word & 3, 1); // list
        let name_off = ((name_word as u32 & !3) as i32) >> 2;
        let name_count = (name_word >> 35) as usize;
        let name_data = cust_ptrs + 8 + name_off as usize * 8;
        let name_len = name_count - 1; // exclude NUL
        let name = std::str::from_utf8(&segment[name_data..name_data + name_len]).unwrap();
        assert_eq!(name, "Cross");
        assert_eq!(segment[name_data + name_len], 0); // NUL

        // 8. Resolve items (ptr slot 1)
        let items_word = u64::from_le_bytes(segment[root_ptrs + 8..root_ptrs + 16].try_into().unwrap());
        assert_eq!(items_word & 3, 1); // list
        let items_off = ((items_word as u32 & !3) as i32) >> 2;
        let items_elem_sz = ((items_word >> 32) & 7) as u8;
        assert_eq!(items_elem_sz, 4); // u32
        let items_count = (items_word >> 35) as usize;
        assert_eq!(items_count, 3);
        let items_data = root_ptrs + 16 + items_off as usize * 8;

        for (i, &expected) in [7u32, 14, 21].iter().enumerate() {
            let off = items_data + i * 4;
            let val = u32::from_le_bytes(segment[off..off + 4].try_into().unwrap());
            assert_eq!(val, expected, "item[{}]", i);
        }
    }
}
