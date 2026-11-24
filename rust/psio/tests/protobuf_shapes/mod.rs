//! The types of `cpp/tests/protobuf_shapes.hpp`, member for member: every
//! shape `psio::protobuf` maps. (`char` is `u8`; `PbPlain`, an enum without
//! `PSIO_REFLECT_ENUM`, is its integer.)

#![allow(dead_code)]

use std::collections::{BTreeMap, BTreeSet};

use psio::Protobuf;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Protobuf)]
#[repr(u8)]
pub enum PbColor {
    Red,
    Green,
    Blue,
    #[psio(other)]
    Other(u8),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Protobuf)]
#[repr(i32)]
pub enum PbLevel {
    Low = -1,
    Mid = 5,
    High = 100,
    #[psio(other)]
    Other(i32),
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbPoint {
    pub x: i32,
    pub y: i32,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbScalars {
    pub b: bool,
    pub i8: i8,
    pub i16: i16,
    pub i32: i32,
    pub i64: i64,
    pub u8: u8,
    pub u16: u16,
    pub u32: u32,
    pub u64: u64,
    pub f: f32,
    pub d: f64,
    pub c: u8,
    pub color: PbColor,
    pub level: PbLevel,
    pub plain: u32,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbEncodings {
    #[psio(pb_sint)]
    pub s32: i32,
    #[psio(pb_sint)]
    pub s64: i64,
    #[psio(pb_fixed)]
    pub f32: i32,
    #[psio(pb_fixed)]
    pub uf32: u32,
    #[psio(pb_fixed)]
    pub f64: i64,
    #[psio(pb_fixed)]
    pub uf64: u64,
    #[psio(pb_sint)]
    pub s16: i16,
    #[psio(pb_sint)]
    pub zs: Vec<i32>,
    #[psio(pb_fixed)]
    pub fs: Vec<u64>,
    #[psio(pb_sint)]
    pub os: Option<i64>,
    #[psio(pb_fixed)]
    pub sf: Vec<i32>,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbText {
    pub s: String,
    pub bytes: Vec<u8>,
    pub chars: Vec<u8>,
    pub quad: [u8; 4],
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbOptionals {
    pub i: Option<i32>,
    pub b: Option<bool>,
    pub s: Option<String>,
    pub d: Option<f64>,
    pub p: Option<PbPoint>,
    pub c: Option<PbColor>,
    pub y: Option<Vec<u8>>,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbRepeated {
    pub ints: Vec<i32>,
    pub bools: Vec<bool>,
    pub doubles: Vec<f64>,
    pub floats: Vec<f32>,
    pub strings: Vec<String>,
    pub points: Vec<PbPoint>,
    pub colors: Vec<PbColor>,
    pub blobs: Vec<Vec<u8>>,
    pub ids: BTreeSet<u32>,
    pub u64s: Vec<u64>,
    pub i64s: Vec<i64>,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbMaps {
    pub counts: BTreeMap<String, i32>,
    pub names: BTreeMap<i32, String>,
    pub points: BTreeMap<u64, PbPoint>,
    pub flags: BTreeMap<bool, f64>,
    pub colors: BTreeMap<String, PbColor>,
    pub blobs: BTreeMap<i64, Vec<u8>>,
}

/// C++ `std::variant<std::monostate, std::int32_t, std::string, PbPoint>`.
#[derive(Debug, Clone, PartialEq, Protobuf)]
pub enum PbPick {
    None,
    Int32(i32),
    String(String),
    Point(PbPoint),
}

/// C++ `std::variant<bool, double>`.
#[derive(Debug, Clone, PartialEq, Protobuf)]
pub enum PbAlways {
    Bool(bool),
    Double(f64),
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbChoice {
    pub before: i32,
    #[psio(field = 5)]
    pub pick: PbPick,
    #[psio(field = 10)]
    pub always: PbAlways,
    #[psio(field = 8)]
    pub between: u32,
    #[psio(field = 20)]
    pub after: String,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbRenumbered {
    #[psio(field = 7)]
    pub a: i32,
    #[psio(field = 3)]
    pub b: String,
    #[psio(field = 1000)]
    pub c: u64,
    #[psio(field = 16)]
    pub d: bool,
    #[psio(field = 2, deprecated)]
    pub old: i32,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbTree {
    pub label: String,
    pub children: Vec<PbTree>,
    pub weight: Option<i32>,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbDefaults {
    pub n: i32,
    pub on: bool,
    pub ratio: f64,
    pub color: PbColor,
    pub name: String,
}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbEmpty {}

#[derive(Debug, Clone, PartialEq, Protobuf)]
pub struct PbOrder {
    pub id: u64,
    pub origin: PbPoint,
    pub scalars: PbScalars,
    pub text: PbText,
    pub choices: Vec<PbChoice>,
    pub maps: PbMaps,
    pub tree: Option<PbTree>,
    pub nothing: PbEmpty,
    pub lists: BTreeMap<String, PbRepeated>,
}

/// `$body` with `$T` bound to the Rust type named `$name`.
#[macro_export]
macro_rules! with_pb_type {
    ($name:expr, $T:ident => $body:expr) => {{
        use $crate::protobuf_shapes::*;
        macro_rules! arm {
            ($Ty:ty) => {{
                type $T = $Ty;
                $body
            }};
        }
        match $name {
            "PbPoint" => arm!(PbPoint),
            "PbScalars" => arm!(PbScalars),
            "PbEncodings" => arm!(PbEncodings),
            "PbText" => arm!(PbText),
            "PbOptionals" => arm!(PbOptionals),
            "PbRepeated" => arm!(PbRepeated),
            "PbMaps" => arm!(PbMaps),
            "PbChoice" => arm!(PbChoice),
            "PbRenumbered" => arm!(PbRenumbered),
            "PbTree" => arm!(PbTree),
            "PbDefaults" => arm!(PbDefaults),
            "PbEmpty" => arm!(PbEmpty),
            "PbOrder" => arm!(PbOrder),
            other => panic!("no Rust type for {other}"),
        }
    }};
}

/// C++ `pb_rng` (tests/protobuf_random.hpp).
pub struct PbRng(pub u64);
impl PbRng {
    pub fn new(seed: u64) -> Self {
        PbRng(seed.wrapping_mul(0x9e3779b97f4a7c15).wrapping_add(1))
    }
    pub fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    pub fn below(&mut self, n: u64) -> u64 {
        if n == 0 {
            0
        } else {
            self.next() % n
        }
    }
}

/// One corruption of `base`: `corrupt` of the C++ emitter
/// (`cpp/tests/protobuf_conformance_emit.cpp`), draw for draw.
pub fn corrupt(base: &[u8], r: &mut PbRng) -> Vec<u8> {
    let mut c = base.to_vec();
    let edits = 1 + r.below(3);
    for _ in 0..edits {
        let kind = r.below(5);
        if kind == 2 {
            let n = r.below(c.len() as u64 + 1) as usize;
            c.truncate(n);
        } else if kind == 3 {
            let at = r.below(c.len() as u64 + 1) as usize;
            let value = r.below(256) as u8;
            c.insert(at, value);
        } else if c.is_empty() {
            continue;
        } else if kind == 0 {
            let at = r.below(c.len() as u64) as usize;
            let value = r.below(256) as u8;
            c[at] = value;
        } else if kind == 1 {
            let at = r.below(c.len() as u64) as usize;
            let bit = r.below(8);
            c[at] ^= 1 << bit;
        } else {
            let at = r.below(c.len() as u64) as usize;
            c.remove(at);
        }
    }
    c
}
