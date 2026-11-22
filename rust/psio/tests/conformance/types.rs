// Rust mapping of cpp/tests/pssz_conformance_types.hpp. Included by
// ../pssz_conformance.rs, which defines `rec!` / `dwnc!`: a record
// declaration with the PSSZ derives. Each also implements `Conf`
// (id = type name, shape = the fixture's definition string), so a wrong
// mapping fails the manifest check before any vector is read.
//
// C++ std::string is RawString, psio::utf8_string<> is String,
// utf8_string<N> is BoundedString<N>; a member with length_bound{.max = N}
// is BoundedRawString<N> / BoundedList<T, N>. A field named `in_` is `in`.

use super::harness::*;
// The harness `encode` takes the format at run time; it
// shadows the tag-generic `psio::pssz::encode` of the glob import below.
use super::harness::encode;
pub use crate::custom_types::*;
use psio::pssz::*;
use std::collections::{BTreeMap, BTreeSet};

type Raw = RawString;

// A C++ enum holds every value of its underlying type, so each mirror has
// the catch-all: an undeclared wire value decodes into `Unknown`.
macro_rules! c_enum {
    ($name:ident : $repr:ident { $($v:ident = $value:literal),* $(,)? }) => {
        #[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
        #[repr($repr)]
        pub enum $name {
            $($v = $value,)*
            #[pssz(other)]
            Unknown($repr),
        }
        impl Conf for $name {
            fn id() -> String { stringify!($name).into() }
            fn shape() -> String {
                let items: Vec<String> = vec![$(format!("{}={}", stringify!($v), stringify!($value))),*];
                format!("enum<{}>{{{}}}", stringify!($repr), items.join(","))
            }
        }
    };
}
c_enum!(Color: u8 { Red = 0, Green = 1, Blue = 2 });
c_enum!(E16: u16 { Lo = 1, Hi = 258 });
c_enum!(E32: i32 { Neg = -2, Zero = 0, Pos = 16909060 });
c_enum!(E64: u64 { Zero = 0, Big = 72623859790382856 });

// std::variant<A, B, ...>: structural, alternatives in order.
macro_rules! variant {
    ($name:ident < $($ty:ident),* >) => {
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
        pub enum $name<$($ty),*> { $($ty($ty)),* }
        impl<$($ty: Conf),*> Conf for $name<$($ty),*> {
            fn id() -> String {
                let parts: Vec<String> = vec![$($ty::id()),*];
                format!("variant<{}>", parts.join(","))
            }
        }
    };
}
variant!(V2<A, B>);
variant!(V5<A, B, C, D, E>);

rec!(Scalars { t: bool, a: u8, b: u16, c: u32, d: u64, e: i8, f: i16, g: i32, h: i64, x: f32, y: f64 });

// Evolution lineages.
rec!(R0 { a: u32, s: Raw });
rec!(R1 { a: u32, s: Raw, x: Option<u32> });
rec!(R2 { a: u32, s: Raw, x: Option<u32>, y: Option<Raw> });
rec!(R3 { a: u32, s: Raw, x: Option<u32>, y: Option<Raw>, z: Option<Vec<u16>> });
rec!(Inner { k: u8, t: Raw });
rec!(N1 { a: u32, s: Raw, in_: Option<Inner> });
rec!(N2 { a: u32, s: Raw, in_: Option<Inner>, oo: Option<Option<u32>> });
rec!(P1 { a: u32, s: Raw, oo: Option<Option<u32>> });
pub type VarUS = V2<u32, Raw>;
pub type TupBS = (u8, Raw);
rec!(E1 { a: u32, s: Raw, e: Option<Color> });
rec!(E2 { a: u32, s: Raw, e: Option<Color>, v: Option<VarUS> });
rec!(E3 { a: u32, s: Raw, e: Option<Color>, v: Option<VarUS>, t: Option<TupBS> });
rec!(Bad1 { a: u32, s: Raw, b: u32 });
rec!(Parent0 { child: R0, tail: Raw });
rec!(Parent1 { child: R1, tail: Raw });
rec!(Parent2 { child: R2, tail: Raw });
rec!(Parent3 { child: R3, tail: Raw });
rec!(#[cap(200)] C1a { a: u32, s: Raw });
rec!(#[cap(200)] C1b { a: u32, s: Raw, x: Option<u32> });
rec!(#[cap(200)] C1c { a: u32, s: Raw, x: Option<u32>, y: Option<Raw> });
rec!(#[cap(1000)] C2a { a: u32, s: Raw });
rec!(#[cap(1000)] C2b { a: u32, s: Raw, x: Option<u32> });
rec!(#[cap(1000)] C2c { a: u32, s: Raw, x: Option<u32>, y: Option<Raw> });
// Boxed values. A Box<T> is T on the wire, and the typed codecs infer no
// size bound through it: a record holding a boxed bounded value uses 4-byte
// offsets unless it carries an explicit cap.
rec!(BLeaf { n: u16, t: BoundedString<4> });
rec!(BDirect { id: u32, leaf: BLeaf });
rec!(BoxRec { id: u32, leaf: Box<BLeaf> });
rec!(#[cap(200)] BoxCapRec { id: u32, leaf: Box<BLeaf> });
rec!(BoxOptRec { id: u32, leaf: Option<Box<BLeaf>>, tail: u8 });
rec!(BoxVecRec { id: u8, leaves: BoundedList<Box<BLeaf>, 3> });
rec!(#[cap(60000)] BoxVecCapRec { id: u8, leaves: Vec<Box<BLeaf>> });
rec!(BoxFixedRec { id: u8, word: Box<u32>, pair: Box<[u8; 2]> });
rec!(BoxOuter { k: u8, capped: BoxCapRec, open: BoxRec, direct: Option<BDirect> });
rec!(I0 { a: u32, b: u8 });
rec!(I1 { a: u32, b: u8, x: Option<u16> });

// Optional fixtures.
rec!(Base { name: Raw });
rec!(Extended { name: Raw, extra: Option<Raw> });
rec!(Twice { name: Raw, middle: Option<u32>, last: Option<Raw> });
rec!(OddBase { flag: bool, name: Raw });
rec!(OddExtended { flag: bool, name: Raw, extra: Option<Raw> });
rec!(OldOuter { inner: Base, values: Vec<Base> });
rec!(NewOuter { inner: Extended, values: Vec<Extended> });
// A directly recursive record: its shape names the type instead of
// expanding it, so `Conf` is written out.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub struct DepthNode { pub value: u8, pub next: Option<Box<DepthNode>> }
impl Conf for DepthNode {
    fn id() -> String { "DepthNode".into() }
    fn shape() -> String { "record{value:u8,next:optional<box<DepthNode>>}".into() }
}

// Zero-size and fixed records.
dwnc!(Unit {});
rec!(Empty {});
dwnc!(Pair { x: u8, y: u8 });
// A fixed-layout record with a non-Copy field (`dwnc!` declares `Copy`
// records).
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
#[pssz(definition_will_not_change)]
pub struct DwncVar { pub k: u8, pub s: Raw }
impl Conf for DwncVar {
    fn id() -> String { "DwncVar".into() }
    fn shape() -> String { "dwnc{k:u8,s:raw_string}".into() }
}

// Optional presence classes.
pub type VarBS = V2<u8, Raw>;
pub type Bits12 = Bitlist<12>;
rec!(PresMid {
    head: u8, o_u32: Option<u32>, o_str: Option<Raw>, o_vec: Option<Vec<u16>>,
    o_unit: Option<Unit>, o_empty: Option<Empty>, o_pair: Option<Pair>, o_rec: Option<Inner>,
    o_var: Option<VarBS>, o_bits: Option<Bits12>, o_arr: Option<[Raw; 2]>,
    o_arr0: Option<[u32; 0]>, o_oo: Option<Option<u32>>, o_oos: Option<Option<Raw>>,
    o_enum: Option<Color>, tail: u8,
});
rec!(PresTail {
    head: u8, o_u32: Option<u32>, o_str: Option<Raw>, o_vec: Option<Vec<u16>>,
    o_unit: Option<Unit>, o_empty: Option<Empty>, o_pair: Option<Pair>, o_rec: Option<Inner>,
    o_var: Option<VarBS>, o_bits: Option<Bits12>, o_arr: Option<[Raw; 2]>,
    o_arr0: Option<[u32; 0]>, o_oo: Option<Option<u32>>, o_oos: Option<Option<Raw>>,
    o_enum: Option<Color>,
});

// Type-derived widths.
rec!(#[cap(64)] WidthCheckpoint { height: u32, label: BoundedRawString<16> });
rec!(WidthRelayBatch {
    checkpoint: WidthCheckpoint, entries: Vec<WidthCheckpoint>, latest: Option<WidthCheckpoint>,
    pair: [WidthCheckpoint; 2], proof: V2<WidthCheckpoint, u32>, context: Raw,
});
rec!(#[cap(1000000)] WidthLargeBudget { value: u8 });
rec!(#[cap(64)] WidthNarrowEnvelope { child: WidthLargeBudget, sequence: u32 });
rec!(WidthReserveParent { child: WidthLargeBudget });
rec!(#[cap(512)] WidthGrowV1 { id: u32 });
rec!(#[cap(512)] WidthGrowV2 { id: u32, time: Option<u32> });
rec!(WidthGrowParentV1 { child: WidthGrowV1 });
rec!(WidthGrowParentV2 { child: WidthGrowV2 });
rec!(WidthThreshold251 { value: [u8; 251] });
rec!(WidthThreshold252 { value: [u8; 252] });
rec!(WidthBoundedBatch { entries: BoundedList<WidthCheckpoint, 4> });
rec!(WidthStringPair { first: Raw, second: Raw });
rec!(#[cap(255)] WidthCappedPair { first: Raw, second: Raw });
rec!(S247 { s: BoundedRawString<247> });
rec!(S248 { s: BoundedRawString<248> });
rec!(S65527 { s: BoundedRawString<65527> });
rec!(S65528 { s: BoundedRawString<65528> });
rec!(#[cap(16)] Tiny { v: u8 });
rec!(NarrowBatch { entries: BoundedList<Tiny, 2> });

macro_rules! mix {
    ($(#[cap($cap:literal)])? $name:ident, $child:ty) => {
        rec!($(#[cap($cap)])? $name {
            child: $child, kids: Vec<$child>, pair: [$child; 2], maybe: Option<$child>,
            alt: V2<u32, $child>,
        });
    };
}
mix!(#[cap(250)] Mix1x1, C1a);
mix!(#[cap(250)] Mix1x2, C2a);
mix!(#[cap(250)] Mix1x4, R0);
mix!(#[cap(60000)] Mix2x1, C1a);
mix!(#[cap(60000)] Mix2x2, C2a);
mix!(#[cap(60000)] Mix2x4, R0);
mix!(Mix4x1, C1a);
mix!(Mix4x2, C2a);
mix!(Mix4x4, R0);

// Strings, bounds, bytes, enums, tuples, variants.
rec!(StrRec { r: Raw, t: String, b: BoundedString<4> });
rec!(Bounded { s: BoundedRawString<4>, v: BoundedList<u16, 2> });
rec!(BytesRec { b: Vec<u8>, a: [u8; 4], bv: Bitvector<12>, bl: Bits12, u: Uint256, x: u128, y: i128 });
rec!(EnumRec { c: Color, w: E16, i: E32, q: E64, list: Vec<Color>, opt: Option<E32> });
rec!(TupRec { t: TupBS, n: u8 });
pub type Var5 = V5<u32, Raw, Inner, TupBS, Unit>;
rec!(T16 { a: u32, s: BoundedString<16> });
rec!(T300 { a: u32, s: BoundedString<300> });

// Nested values (docs/pssz-spec.md 5.8).
rec!(NArgs { to: Raw, amount: u64, memo: Option<Raw> });
rec!(NArgs2 { to: Raw, amount: u64, memo: Option<Raw>, fee: Option<u32> });
rec!(NAction { method: u32, args: Nested<NArgs> });
rec!(NAction2 { method: u32, args: Nested<NArgs2> });
rec!(NActionRaw { method: u32, args: Vec<u8> });
rec!(NActionLater { method: u32, args: Nested<NArgs, Deferred> });
rec!(NShapes {
    scalar: Nested<u32>, twice: Nested<Nested<NArgs>>, maybe: Option<Nested<NArgs>>,
    many: Vec<Nested<NArgs>>,
});
rec!(NCapped { id: u8, note: Nested<Raw, Checked, 8> });
// The bound counts encoded bytes, whatever the inner value is: a record, a
// list of u16 (half as many elements as bytes).
rec!(NCapRec { id: u8, args: Nested<NArgs, Checked, 28> });
rec!(NCapWords { id: u8, words: Nested<Vec<u16>, Checked, 5> });
// A self-nesting chain: recursive, declared like DepthNode.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub struct NChain { pub next: Option<Nested<NChain>> }
impl Conf for NChain {
    fn id() -> String { "NChain".into() }
    fn shape() -> String { "record{next:optional<nested<NChain>>}".into() }
}

// Pairs, maps and sets (docs/pssz-spec.md 5.9), and the same record with
// the lists they are on the wire.
rec!(AssocRec {
    counts: BTreeMap<Raw, u32>, ids: BTreeSet<u16>, head: (u16, Raw),
    names: Option<BTreeMap<u32, Raw>>, limited: BoundedMap<u8, u8, 2>, groups: Vec<BTreeSet<i32>>,
});
rec!(AssocLists {
    counts: Vec<(Raw, u32)>, ids: Vec<u16>, head: (u16, Raw),
    names: Option<Vec<(u32, Raw)>>, limited: BoundedList<(u8, u8), 2>, groups: Vec<Vec<i32>>,
});

