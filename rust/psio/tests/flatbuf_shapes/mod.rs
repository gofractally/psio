//! The record types the FlatBuffers tests share: the declarations of
//! `cpp/tests/flatbuf_shapes.hpp`, member for member.
//!
//! They exist twice. `natural` is how a Rust program declares them: `String`
//! members and a plain `enum`. `raw` holds exactly what the C++ types hold:
//! [`RawString`] (a FlatBuffers string is bytes; C++ `std::string` does not
//! require UTF-8) and an enum with a `#[psio(other)]` catch-all (a C++ enum
//! holds any value of its underlying type). The verifier gives the C++
//! verdict for `raw` on every buffer; for `natural` it additionally refuses
//! a string that is not UTF-8 and an enum value that is not an enumerator.
#![allow(dead_code)]

use psio::flatbuf::{FbError, FbLimits, FbOptions, FbPack, FbUnpack, FbView};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Debug;

/// Decodes `bytes` to `value` (through the checked decode, the trusted
/// decode and the view) and encodes `value` to `bytes`.
pub fn check_value<T: FbPack + FbUnpack + PartialEq + Debug>(id: &str, bytes: &[u8], opts: &FbOptions, value: &T) {
    T::fb_validate_with(bytes, opts, FbLimits::default()).unwrap_or_else(|e| panic!("{id}: {e}"));
    let inner = if opts.size_prefixed { psio::flatbuf_size_prefixed(bytes).expect(id) } else { bytes };
    // (Alignment is checked from the start of a size prefix, so the buffer
    // inside one is not always valid on its own.)
    if !opts.size_prefixed {
        assert_eq!(&psio::from_flatbuf::<T>(inner).unwrap(), value, "{id}: decode");
    }
    assert_eq!(&psio::from_flatbuf_trusted::<T>(inner), value, "{id}: trusted decode");
    assert_eq!(&FbView::<T>::new_with(bytes, opts, FbLimits::default()).unwrap().unpack(), value, "{id}: view");
    assert_eq!(psio::to_flatbuf_with(value, opts), bytes, "{id}: encode");
}

/// A buffer that validates decodes to a value whose encoding validates and
/// decodes to itself.
pub fn check_stable<T: FbPack + FbUnpack>(id: &str, bytes: &[u8]) {
    let again = psio::to_flatbuf(&psio::from_flatbuf::<T>(bytes).unwrap_or_else(|e| panic!("{id}: {e}")));
    T::fb_validate(&again).unwrap_or_else(|e| panic!("{id}: re-encoded: {e}"));
    // Compared as encodings: a corrupted float may be a NaN.
    assert_eq!(psio::to_flatbuf(&psio::from_flatbuf_trusted::<T>(&again)), again, "{id}: stable");
    assert_eq!(psio::to_flatbuf(&FbView::<T>::trusted(bytes).unpack()), again, "{id}: view");
}

/// Checks that what a view hands out lies inside the buffer it was made
/// from, and bounds the walk of a buffer that reaches one sub-object along
/// many paths (a view is lazy: nothing else stops such a walk).
pub struct Walk {
    range: std::ops::Range<usize>,
    pub inside: bool,
    pub budget: usize,
}

impl Walk {
    pub fn new(buf: &[u8]) -> Self {
        let start = buf.as_ptr() as usize;
        Walk { range: start..start + buf.len(), inside: true, budget: 4000 }
    }
    /// Notes the bytes of a borrowed string or byte vector.
    pub fn borrowed<'a, B: AsRef<[u8]> + ?Sized>(&mut self, bytes: &'a B) -> &'a B {
        let b = bytes.as_ref();
        let start = b.as_ptr() as usize;
        if !b.is_empty() && (start < self.range.start || start + b.len() > self.range.end) {
            self.inside = false;
        }
        bytes
    }
    pub fn spend(&mut self) -> bool {
        if self.budget == 0 {
            return false;
        }
        self.budget -= 1;
        true
    }
}

macro_rules! shapes {
    ($Str:ty) => {
        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        #[psio(definition_will_not_change)]
        pub struct Vec3 {
            pub x: f32,
            pub y: f32,
            pub z: f32,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        #[psio(definition_will_not_change)]
        pub struct Mixed {
            pub a: u8,
            pub b: u64,
            pub c: u16,
            pub d: [i32; 3],
            pub v: Vec3,
            pub e: bool,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Point {
            pub x: i32,
            pub y: i32,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Profile {
            pub id: u64,
            pub name: $Str,
            pub email: $Str,
            pub age: u32,
            pub verified: bool,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Item {
            pub product: $Str,
            #[psio(default = 1)]
            pub qty: u32,
            pub unit_price: f64,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Card {
            pub number: $Str,
            pub cvv: u16,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Wire {
            pub iban: $Str,
        }

        /// C++ `std::variant<std::monostate, Card, Wire>`.
        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub enum Payment {
            None,
            Card(Card),
            Wire(Wire),
        }

        /// C++ `std::variant<Card, Wire>`.
        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub enum Refund {
            Card(Card),
            Wire(Wire),
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Order {
            pub id: u64,
            pub customer: Profile,
            pub items: Vec<Item>,
            pub total: f64,
            pub note: Option<$Str>,
            pub payment: Payment,
            pub position: Vec3,
            pub origin: Option<Vec3>,
            pub path: Vec<Vec3>,
            pub tags: Vec<$Str>,
            #[psio(default = "Color::Green")]
            pub color: Color,
            pub discount: Option<i32>,
            pub labels: BTreeSet<$Str>,
            pub stock: BTreeMap<$Str, u32>,
            pub by_id: BTreeMap<u32, Item>,
            pub blob: Vec<u8>,
            pub flags: Vec<bool>,
            pub quad: [u16; 4],
            pub colors: Vec<Color>,
            pub ids: BTreeSet<u32>,
            pub refund: Refund,
            pub mixed: Mixed,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Tree {
            pub value: i32,
            pub name: $Str,
            pub children: Vec<Tree>,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Scalars {
            pub b: bool,
            pub i8: i8,
            pub u8: u8,
            pub i16: i16,
            pub u16: u16,
            pub i32: i32,
            pub u32: u32,
            pub i64: i64,
            pub u64: u64,
            pub f: f32,
            pub d: f64,
            pub c: Color,
            /// C++ `char`.
            pub ch: i8,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Defaults {
            #[psio(default = 7)]
            pub level: i16,
            #[psio(default = true)]
            pub on: bool,
            #[psio(default = "Color::Green")]
            pub c: Color,
            #[psio(default = 0.5)]
            pub ratio: f64,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Renumbered {
            #[psio(field = 3)]
            pub c: i32,
            #[psio(field = 1)]
            pub a: $Str,
            #[psio(field = 2)]
            pub b: i64,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct WithDeprecated {
            pub a: i32,
            #[psio(deprecated)]
            pub old: i32,
            pub c: i32,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct SparseRow {
            pub a: i32,
            pub b: i32,
            pub c: i32,
            pub d: i32,
            pub e: i32,
            pub f: i32,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Sparse {
            pub rows: Vec<SparseRow>,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Numbers {
            pub b: bool,
            pub i8: i8,
            pub u8: u8,
            pub i16: i16,
            pub u16: u16,
            pub i32: i32,
            pub u32: u32,
            pub i64: i64,
            pub u64: u64,
            pub f32: f32,
            pub f64: f64,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Entry {
            pub name: $Str,
            pub count: u32,
            pub price: f64,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Basket {
            pub id: u64,
            pub owner: Profile,
            pub entries: Vec<Entry>,
            pub tags: Vec<$Str>,
            pub codes: Vec<u16>,
            pub bytes: Vec<u8>,
            pub total: f64,
            pub paid: bool,
        }

        #[derive(Debug, Clone, PartialEq, FbPack, FbUnpack, FbView)]
        pub struct Longs {
            pub values: Vec<u64>,
            pub reals: Vec<f64>,
        }

        /// C++ `T{}`: every member at its default.
        pub fn empty<T: FbUnpack>() -> T {
            T::fb_absent()
        }

        fn s(text: &str) -> $Str {
            text.into()
        }
        fn vec3(x: f32, y: f32, z: f32) -> Vec3 {
            Vec3 { x, y, z }
        }
        fn item(product: &str, qty: u32, unit_price: f64) -> Item {
            Item { product: s(product), qty, unit_price }
        }
        fn card(number: &str, cvv: u16) -> Card {
            Card { number: s(number), cvv }
        }
        fn wire(iban: &str) -> Wire {
            Wire { iban: s(iban) }
        }

        pub fn sample_profile() -> Profile {
            Profile { id: 42, name: s("Alice Example"), email: s("alice@example.com"), age: 37, verified: true }
        }

        pub fn sample_order() -> Order {
            Order {
                id: 1001,
                customer: sample_profile(),
                items: vec![item("widget", 3, 9.99), item("gadget", 1, 24.5), item("", 2, 0.0)],
                total: 54.47,
                note: Some(s("leave at the door")),
                payment: Payment::Card(card("4111", 123)),
                position: vec3(1.0, 2.0, 3.0),
                origin: Some(vec3(-1.0, -2.0, -3.0)),
                path: vec![vec3(0.0, 0.0, 0.0), vec3(1.0, 1.5, 2.0), vec3(4.0, 5.0, 6.0)],
                tags: vec![s("a"), s(""), s("longer tag")],
                color: Color::Blue,
                discount: Some(-15),
                labels: [s("gift"), s("rush")].into(),
                stock: [(s("bolt"), 17), (s("nut"), 0), (s("washer"), 250)].into(),
                by_id: [(0, item("zero", 1, 0.0)), (7, item("seven", 7, 7.5)), (4_000_000_000, item("big", 0, 1.0))]
                    .into(),
                blob: vec![0, 1, 2, 253, 254, 255, 7],
                flags: vec![true, false, true],
                quad: [1, 2, 3, 65535],
                colors: vec![Color::Red, Color::Blue],
                ids: [3, 40, 500].into(),
                refund: Refund::Card(card("refund", 1)),
                mixed: Mixed { a: 200, b: 0x0102030405060708, c: 65000, d: [-1, 2, -3], v: vec3(0.5, 1.5, 2.5), e: true },
            }
        }

        pub fn compact_order() -> Order {
            Order {
                id: 7,
                customer: Profile { id: 1, name: s("Al"), email: s(""), age: 30, verified: true },
                items: vec![item("w", 3, 1.5)],
                total: 0.0,
                note: Some(s("n")),
                payment: Payment::Card(card("41", 9)),
                position: vec3(0.0, 0.0, 0.0),
                origin: Some(vec3(1.0, 0.0, -1.0)),
                path: vec![vec3(1.0, 2.0, 3.0)],
                tags: vec![s("t"), s("")],
                color: Color::Blue,
                discount: Some(5),
                labels: [s("a"), s("b")].into(),
                stock: [(s("k"), 1), (s("m"), 2)].into(),
                by_id: [(1, item("one", 1, 0.0)), (9, item("nine", 2, 0.5))].into(),
                blob: vec![1, 2, 3],
                flags: vec![true, false],
                quad: [1, 2, 3, 4],
                colors: vec![Color::Green],
                ids: [1, 2].into(),
                refund: Refund::Wire(wire("FR")),
                mixed: Mixed { a: 1, b: 2, c: 3, d: [4, 5, 6], v: vec3(7.0, 8.0, 9.0), e: true },
            }
        }

        pub fn conformance_tree() -> Tree {
            let leaf = |value: i32| Tree { value, name: s(""), children: vec![] };
            Tree {
                value: 1,
                name: s("root"),
                children: vec![
                    leaf(2),
                    Tree { value: 0, name: s("middle"), children: vec![leaf(0), leaf(-7)] },
                    leaf(2),
                ],
            }
        }

        /// The tree of `cpp/tests/flatbuf_fuzz_tests.cpp`.
        pub fn sample_tree() -> Tree {
            let node = |value: i32, name: &str, children: Vec<Tree>| Tree { value, name: s(name), children };
            node(
                7,
                "root",
                vec![
                    node(0, "a", vec![]),
                    node(0, "", vec![node(0, "", vec![]), node(0, "", vec![]), node(9, "", vec![node(0, "", vec![])])]),
                    node(0, "", vec![]),
                    node(-1, "", vec![]),
                ],
            )
        }

        pub fn conformance_basket() -> Basket {
            Basket {
                id: 9_000_000_000,
                owner: sample_profile(),
                entries: vec![
                    Entry { name: s("bolt"), count: 12, price: 0.25 },
                    Entry { name: s(""), count: 0, price: 0.0 },
                    Entry { name: s("washer"), count: 1, price: 1.5 },
                ],
                tags: vec![s("a"), s(""), s("third")],
                codes: vec![1, 256, 65535],
                bytes: vec![0, 1, 254, 255, 7],
                total: 4.5,
                paid: true,
            }
        }

        pub fn sample_scalars() -> Scalars {
            Scalars {
                b: true,
                i8: -1,
                u8: 2,
                i16: -3,
                u16: 4,
                i32: -5,
                u32: 6,
                i64: -7,
                u64: 8,
                f: 1.5,
                d: -2.25,
                c: Color::Blue,
                ch: b'x' as i8,
            }
        }

        // ── Values rebuilt through the views' accessors ──────────────────

        pub fn vec3_from_view(v: FbView<'_, Vec3>) -> Vec3 {
            Vec3 { x: v.x(), y: v.y(), z: v.z() }
        }
        pub fn profile_from_view(v: FbView<'_, Profile>, w: &mut Walk) -> Profile {
            Profile {
                id: v.id(),
                name: w.borrowed(v.name()).into(),
                email: w.borrowed(v.email()).into(),
                age: v.age(),
                verified: v.verified(),
            }
        }
        pub fn item_from_view(v: FbView<'_, Item>, w: &mut Walk) -> Item {
            Item { product: w.borrowed(v.product()).into(), qty: v.qty(), unit_price: v.unit_price() }
        }
        pub fn card_from_view(v: FbView<'_, Card>, w: &mut Walk) -> Card {
            Card { number: w.borrowed(v.number()).into(), cvv: v.cvv() }
        }
        pub fn wire_from_view(v: FbView<'_, Wire>, w: &mut Walk) -> Wire {
            Wire { iban: w.borrowed(v.iban()).into() }
        }
        pub fn order_from_view(v: FbView<'_, Order>, w: &mut Walk) -> Order {
            let (payment, refund, mixed) = (v.payment(), v.refund(), v.mixed());
            // Lookups terminate and stay inside whatever the order of the keys is.
            let (labels, stock, by_id, ids) = (v.labels(), v.stock(), v.by_id(), v.ids());
            if let Some(first) = labels.get(0) {
                assert!(labels.lower_bound(first) <= labels.len());
            }
            if let Some(first) = stock.key(0) {
                let _ = (stock.find(first), stock.get(first), stock.contains_key(first));
            }
            let _ = (by_id.get(&7).map(|item| item.qty()), by_id.lower_bound(&1), ids.contains(&2), ids.find(&500));
            assert!(v.items().get(v.items().len()).is_none() && v.path().get(usize::MAX).is_none());
            Order {
                id: v.id(),
                customer: profile_from_view(v.customer(), w),
                items: v.items().iter().map(|item| item_from_view(item, w)).collect(),
                total: v.total(),
                note: v.note().map(|note| w.borrowed(note).into()),
                payment: if let Some(card) = payment.card() {
                    Payment::Card(card_from_view(card, w))
                } else if let Some(wire) = payment.wire() {
                    Payment::Wire(wire_from_view(wire, w))
                } else {
                    Payment::None
                },
                position: vec3_from_view(v.position()),
                origin: v.origin().map(vec3_from_view),
                path: v.path().iter().map(vec3_from_view).collect(),
                tags: v.tags().iter().map(|tag| w.borrowed(tag).into()).collect(),
                color: v.color(),
                discount: v.discount(),
                labels: labels.iter().map(|label| w.borrowed(label).into()).collect(),
                stock: stock.iter().map(|(key, count)| (w.borrowed(key).into(), count)).collect(),
                by_id: by_id.iter().map(|(key, item)| (key, item_from_view(item, w))).collect(),
                blob: w.borrowed(v.blob().as_bytes()).to_vec(),
                flags: v.flags().iter().collect(),
                quad: {
                    let quad = v.quad();
                    std::array::from_fn(|i| quad.get(i).unwrap_or(0))
                },
                colors: v.colors().iter().collect(),
                ids: ids.iter().collect(),
                refund: match refund.wire() {
                    Some(wire) => Refund::Wire(wire_from_view(wire, w)),
                    // NONE or absent: the first alternative, of defaults.
                    None => Refund::Card(card_from_view(refund.card().unwrap_or(FbView::absent(v.fb_buffer())), w)),
                },
                mixed: Mixed {
                    a: mixed.a(),
                    b: mixed.b(),
                    c: mixed.c(),
                    d: {
                        let d = mixed.d();
                        std::array::from_fn(|i| d.get(i).unwrap_or(0))
                    },
                    v: vec3_from_view(mixed.v()),
                    e: mixed.e(),
                },
            }
        }
        pub fn tree_from_view(v: FbView<'_, Tree>, w: &mut Walk) -> Tree {
            let mut children = Vec::new();
            for child in v.children().iter() {
                if !w.spend() {
                    break;
                }
                children.push(tree_from_view(child, w));
            }
            Tree { value: v.value(), name: w.borrowed(v.name()).into(), children }
        }
        pub fn scalars_from_view(v: FbView<'_, Scalars>) -> Scalars {
            Scalars {
                b: v.b(),
                i8: v.i8(),
                u8: v.u8(),
                i16: v.i16(),
                u16: v.u16(),
                i32: v.i32(),
                u32: v.u32(),
                i64: v.i64(),
                u64: v.u64(),
                f: v.f(),
                d: v.d(),
                c: v.c(),
                ch: v.ch(),
            }
        }

        /// Checks the accepted row `id` of `conformance/flatbuf-vectors.json`
        /// against the value it names. `false`: the row is not known here.
        pub fn check_accepted(id: &str, bytes: &[u8], opts: &FbOptions) -> bool {
            use super::check_value as check;
            match id {
                "Point/zero" => check(id, bytes, opts, &empty::<Point>()),
                "Point/values" => check(id, bytes, opts, &Point { x: 1, y: -2 }),
                "Profile/empty" => check(id, bytes, opts, &empty::<Profile>()),
                "Profile/sample" | "Profile/identifier" | "Profile/size_prefixed" => {
                    check(id, bytes, opts, &sample_profile())
                }
                "Numbers/zero" => check(id, bytes, opts, &empty::<Numbers>()),
                "Numbers/extremes" => check(
                    id,
                    bytes,
                    opts,
                    &Numbers {
                        b: true,
                        i8: i8::MIN,
                        u8: u8::MAX,
                        i16: i16::MIN,
                        u16: u16::MAX,
                        i32: i32::MIN,
                        u32: u32::MAX,
                        i64: i64::MIN,
                        u64: u64::MAX,
                        f32: -1.5,
                        f64: 1e300,
                    },
                ),
                "Tree/empty" => check(id, bytes, opts, &empty::<Tree>()),
                "Tree/nested" => check(id, bytes, opts, &conformance_tree()),
                "Basket/empty" => check(id, bytes, opts, &empty::<Basket>()),
                "Basket/full" => check(id, bytes, opts, &conformance_basket()),
                "Order/empty" => check(id, bytes, opts, &empty::<Order>()),
                "Order/sample" | "Order/identifier_size_prefixed" => check(id, bytes, opts, &sample_order()),
                "Order/wire" | "Order/none" => {
                    let mut o = sample_order();
                    o.payment = Payment::Wire(wire("DE00 1234 5678"));
                    o.refund = Refund::Wire(wire("FR11"));
                    (o.origin, o.discount, o.note) = (None, None, None);
                    if id == "Order/none" {
                        o.payment = Payment::None;
                        o.note = Some(s(""));
                        o.discount = Some(0);
                    }
                    check(id, bytes, opts, &o)
                }
                "Order/compact" => check(id, bytes, opts, &compact_order()),
                "Defaults/defaults" => check(id, bytes, opts, &empty::<Defaults>()),
                "Defaults/zeros" => {
                    check(id, bytes, opts, &Defaults { level: 0, on: false, c: Color::Red, ratio: 0.0 })
                }
                "Renumbered/sample" => check(id, bytes, opts, &Renumbered { c: 5, a: s("five"), b: -5 }),
                "WithDeprecated/sample" => check(id, bytes, opts, &WithDeprecated { a: 1, old: 0, c: 3 }),
                "Scalars/zero" => check(id, bytes, opts, &empty::<Scalars>()),
                "Scalars/sample" => check(id, bytes, opts, &sample_scalars()),
                "Sparse/rows" => {
                    let row = |a, b, f| SparseRow { a, b, c: 0, d: 0, e: 0, f };
                    let rows = vec![row(1, 0, 0), row(0, 0, 6), row(0, 2, 0), row(1, 0, 0), row(0, 0, 0)];
                    check(id, bytes, opts, &Sparse { rows })
                }
                "Longs/empty" => check(id, bytes, opts, &empty::<Longs>()),
                "Longs/sample" => {
                    check(id, bytes, opts, &Longs { values: vec![1, u64::MAX, 3], reals: vec![0.5, -0.0] })
                }
                _ => return false,
            }
            true
        }

        /// The verifier's verdict on `bytes` as the type named `ty`.
        pub fn verdict(ty: &str, bytes: &[u8], opts: &FbOptions) -> Result<(), FbError> {
            let limits = FbLimits::default();
            match ty {
                "Point" => Point::fb_validate_with(bytes, opts, limits),
                "Profile" => Profile::fb_validate_with(bytes, opts, limits),
                "Numbers" => Numbers::fb_validate_with(bytes, opts, limits),
                "Tree" => Tree::fb_validate_with(bytes, opts, limits),
                "Basket" => Basket::fb_validate_with(bytes, opts, limits),
                "Order" => Order::fb_validate_with(bytes, opts, limits),
                "Defaults" => Defaults::fb_validate_with(bytes, opts, limits),
                "Renumbered" => Renumbered::fb_validate_with(bytes, opts, limits),
                "WithDeprecated" => WithDeprecated::fb_validate_with(bytes, opts, limits),
                "Scalars" => Scalars::fb_validate_with(bytes, opts, limits),
                "Sparse" => Sparse::fb_validate_with(bytes, opts, limits),
                "Longs" => Longs::fb_validate_with(bytes, opts, limits),
                other => panic!("unknown type {other}"),
            }
        }

        /// `check_stable` for the type named `ty`.
        pub fn stable(id: &str, ty: &str, bytes: &[u8]) {
            use super::check_stable;
            match ty {
                "Profile" => check_stable::<Profile>(id, bytes),
                "Tree" => check_stable::<Tree>(id, bytes),
                "Basket" => check_stable::<Basket>(id, bytes),
                "Order" => check_stable::<Order>(id, bytes),
                "Longs" => check_stable::<Longs>(id, bytes),
                other => panic!("unknown type {other}"),
            }
        }
    };
}

/// The types as a Rust program declares them.
pub mod natural {
    use super::*;

    #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, FbPack, FbUnpack, FbView)]
    #[repr(u8)]
    pub enum Color {
        Red,
        Green,
        Blue,
    }

    shapes!(String);
}

/// The types holding exactly what the C++ types hold.
pub mod raw {
    use super::*;
    use psio::pssz::RawString;

    #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, FbPack, FbUnpack, FbView)]
    #[repr(u8)]
    pub enum Color {
        Red,
        Green,
        Blue,
        #[psio(other)]
        Other(u8),
    }

    shapes!(RawString);
}
