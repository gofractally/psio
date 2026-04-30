//! Cross-validation harness — Rust pjson encoder vs C++ pjson encoder.
//!
//! Self-checks (always run): byte-identity round-trip on every shape
//! defined in Appendix C of `docs/pjson-spec.md`. The Rust encoder
//! produces buffer A; the Rust decoder reads A back; the Rust encoder
//! re-encodes the decoded value into buffer B; then assert A == B.
//! This proves the encoder is canonical (idempotent) on its own.
//!
//! C++ cross-checks (`#[ignore]`-marked): for each shape, compare the
//! Rust encoder's output against precomputed bytes captured from the
//! reference C++ encoder. The byte arrays are placeholders
//! (`TODO_BYTES_FROM_CPP`) until the sibling C++ branch lands; once
//! it does, `cpp/tests/pjson_cross_validation.cpp` will dump the
//! exact bytes for each Appendix-C shape and someone copies them in.

use crate::pjson::{from_pjson, to_pjson, Pjson};

// ── Appendix C shapes ──────────────────────────────────────────────────────

#[derive(Debug, PartialEq, Clone)]
pub struct Point {
    pub x: i32,
    pub y: i32,
}
pjson_struct!(Point { x: i32, y: i32 });

#[derive(Debug, PartialEq, Clone)]
pub struct NameRecord {
    pub account: u64,
    pub limit: u64,
}
pjson_struct!(NameRecord {
    account: u64,
    limit: u64,
});

#[derive(Debug, PartialEq, Clone)]
pub struct FlatRecord {
    pub id: u32,
    pub label: String,
    pub values: Vec<u16>,
}
pjson_struct!(FlatRecord {
    id: u32,
    label: String,
    values: Vec<u16>,
});

#[derive(Debug, PartialEq, Clone)]
pub struct Record {
    pub id: u32,
    pub label: String,
    pub values: Vec<u16>,
    pub score: Option<u32>,
}
pjson_struct!(Record {
    id: u32,
    label: String,
    values: Vec<u16>,
    score: Option<u32>,
});

#[derive(Debug, PartialEq, Clone)]
pub struct Validator {
    pub pubkey_lo: u64,
    pub pubkey_hi: u64,
    pub withdrawal_lo: u64,
    pub withdrawal_hi: u64,
    pub effective_balance: u64,
    pub slashed: bool,
    pub activation_epoch: u64,
    pub exit_epoch: u64,
    pub withdrawable_epoch: u64,
}
pjson_struct!(Validator {
    pubkey_lo: u64,
    pubkey_hi: u64,
    withdrawal_lo: u64,
    withdrawal_hi: u64,
    effective_balance: u64,
    slashed: bool,
    activation_epoch: u64,
    exit_epoch: u64,
    withdrawable_epoch: u64,
});

// ── Sample-data factories ──────────────────────────────────────────────────

pub fn sample_point() -> Point {
    Point { x: -42, y: 77 }
}

pub fn sample_name_record() -> NameRecord {
    NameRecord {
        account: 0x0123_4567_89AB_CDEF,
        limit: 1_000_000,
    }
}

pub fn sample_flat_record() -> FlatRecord {
    FlatRecord {
        id: 42,
        label: "hello".to_string(),
        values: vec![1, 2, 3, 100, 1000],
    }
}

pub fn sample_record() -> Record {
    Record {
        id: 42,
        label: "hello".to_string(),
        values: vec![1, 2, 3, 100, 1000],
        score: Some(95),
    }
}

pub fn sample_validator() -> Validator {
    Validator {
        pubkey_lo: 0x0123_4567_89AB_CDEF,
        pubkey_hi: 0xFEDC_BA98_7654_3210,
        withdrawal_lo: 0x1111_2222_3333_4444,
        withdrawal_hi: 0x5555_6666_7777_8888,
        effective_balance: 32_000_000_000,
        slashed: false,
        activation_epoch: 100,
        exit_epoch: u64::MAX,
        withdrawable_epoch: u64::MAX,
    }
}

// ── Round-trip self checks (always run) ────────────────────────────────────

fn roundtrip_canonical<T: Pjson + PartialEq + Clone + std::fmt::Debug>(value: &T) {
    let buf_a = to_pjson(value);
    let decoded: T = from_pjson(&buf_a).expect("roundtrip decode");
    assert_eq!(*value, decoded, "decode != original");
    let buf_b = to_pjson(&decoded);
    assert_eq!(
        buf_a, buf_b,
        "encoder is not canonical (idempotent) for shape"
    );
}

#[test]
fn point_roundtrip_canonical() {
    roundtrip_canonical(&sample_point());
}

#[test]
fn name_record_roundtrip_canonical() {
    roundtrip_canonical(&sample_name_record());
}

#[test]
fn flat_record_roundtrip_canonical() {
    roundtrip_canonical(&sample_flat_record());
}

#[test]
fn record_roundtrip_canonical() {
    roundtrip_canonical(&sample_record());
}

#[test]
fn validator_roundtrip_canonical() {
    roundtrip_canonical(&sample_validator());
}

// ── C++ byte-identity cross-checks (placeholder bytes; #[ignore]) ──────────
//
// Each TODO_BYTES_FROM_CPP_* constant must be filled with the exact
// byte string the C++ pjson encoder produces for the corresponding
// `sample_*()` value, once the sibling branch
// `pjson-impl-conformance` lands and a `psio_cross_dump_pjson_bytes`
// CLI / test is added on the C++ side.
//
// Until then these tests are `#[ignore]`-marked so they don't fail
// the suite. To run them locally after populating the bytes:
//     cargo test -p psio --lib pjson_cross_validation_tests -- --ignored

const TODO_BYTES_FROM_CPP_POINT: &[u8] = &[];
const TODO_BYTES_FROM_CPP_NAME_RECORD: &[u8] = &[];
const TODO_BYTES_FROM_CPP_FLAT_RECORD: &[u8] = &[];
const TODO_BYTES_FROM_CPP_RECORD: &[u8] = &[];
const TODO_BYTES_FROM_CPP_VALIDATOR: &[u8] = &[];

fn assert_cpp_byte_identity<T: Pjson>(value: &T, expected: &[u8], shape: &str) {
    if expected.is_empty() {
        panic!(
            "TODO: populate cross-validation bytes for shape `{}` from \
             the C++ pjson encoder once the sibling branch lands",
            shape
        );
    }
    let buf = to_pjson(value);
    if buf != expected {
        panic!(
            "Rust pjson bytes diverge from C++ for shape `{}`:\n\
             Rust: {:02x?}\n\
             C++:  {:02x?}",
            shape, buf, expected
        );
    }
}

#[test]
#[ignore]
fn point_cpp_byte_identity() {
    assert_cpp_byte_identity(&sample_point(), TODO_BYTES_FROM_CPP_POINT, "Point");
}

#[test]
#[ignore]
fn name_record_cpp_byte_identity() {
    assert_cpp_byte_identity(
        &sample_name_record(),
        TODO_BYTES_FROM_CPP_NAME_RECORD,
        "NameRecord",
    );
}

#[test]
#[ignore]
fn flat_record_cpp_byte_identity() {
    assert_cpp_byte_identity(
        &sample_flat_record(),
        TODO_BYTES_FROM_CPP_FLAT_RECORD,
        "FlatRecord",
    );
}

#[test]
#[ignore]
fn record_cpp_byte_identity() {
    assert_cpp_byte_identity(&sample_record(), TODO_BYTES_FROM_CPP_RECORD, "Record");
}

#[test]
#[ignore]
fn validator_cpp_byte_identity() {
    assert_cpp_byte_identity(
        &sample_validator(),
        TODO_BYTES_FROM_CPP_VALIDATOR,
        "Validator",
    );
}
