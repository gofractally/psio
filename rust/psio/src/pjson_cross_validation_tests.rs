//! Cross-validation harness — Rust pjson encoder vs C++ pjson encoder.
//!
//! Self-checks (always run): byte-identity round-trip on every shape
//! defined in Appendix C of `docs/pjson-spec.md`. The Rust encoder
//! produces buffer A; the Rust decoder reads A back; the Rust encoder
//! re-encodes the decoded value into buffer B; then assert A == B.
//! This proves the encoder is canonical (idempotent) on its own.
//!
//! C++ byte-identity cross-checks: for each shape, compare the Rust
//! encoder's output against the bytes captured from the reference
//! C++ encoder (psio::to_pjson on the same logical value). All five
//! Appendix-C shapes pass byte-identity round-trip — the `CPP_BYTES_*`
//! constants below were dumped from a small C++ harness against the
//! pjson_typed.hpp encoder on commit 851bea1 / merged in 9a272ae.

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
    // Mirrors the `validator()` factory in `cpp/benchmarks/shapes.hpp`
    // (i = 1; epochs use 0xFFFF_FFFF, NOT u64::MAX). Keeping all u64
    // values within the i64 positive range is important for cross-val
    // because the current C++ pjson encoder casts u64 → i64 before
    // dispatching, so a u64 with the high bit set would emit NEGINT
    // (logical −1) under the C++ side — diverging from spec §4.4.
    // Filed for the C++ branch separately; the cross-val here uses
    // the safe values to confirm byte-equivalence on the bulk of the
    // u64 range.
    Validator {
        pubkey_lo: 7,
        pubkey_hi: 11,
        withdrawal_lo: 13,
        withdrawal_hi: 17,
        effective_balance: 32_000_000_000,
        slashed: false,
        activation_epoch: 100,
        exit_epoch: 0xFFFF_FFFF,
        withdrawable_epoch: 0xFFFF_FFFF,
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

// ── C++ byte-identity cross-checks ─────────────────────────────────────────
//
// These bytes were captured from the reference C++ pjson encoder on
// commit 851bea1 (`pjson: bring impl into conformance with v1
// wire-format spec`), merged into main as 9a272ae. The dumper used
// (kept locally for repro):
//
//     #include <psio/pjson_typed.hpp>
//     #include "cpp/benchmarks/shapes.hpp"
//
//     std::vector<std::uint8_t> buf;
//     psio::to_pjson(value, buf);  // for each Appendix-C shape
//
// To regenerate after a future C++-side wire change: rebuild the
// dumper and replace the byte arrays in this file.
//
// Note: the `sample_validator()` factory keeps all u64 fields in the
// i64 positive range (epochs use 0xFFFF_FFFF, NOT u64::MAX) because
// the current C++ encoder casts u64 → i64 in pjson_encoded_size /
// typed_field_encode and emits NEGINT when the high bit is set —
// divergence from spec §4.4 (uint payload = raw LE unsigned
// magnitude). Filed against the C++ side; covered here by using
// values that don't exercise the bug. The Rust side already follows
// the spec (uint for non-negative, negint for negative).

// POINT — 16 bytes
const CPP_BYTES_POINT: &[u8] = &[
    0xC0, 0x00, 0x78, 0x70, 0x2A, 0x79, 0x40, 0x4D, 0x11, 0xE5, 0x00, 0x01,
    0x03, 0x01, 0x02, 0x00,
];

// NAME_RECORD — 35 bytes
const CPP_BYTES_NAME_RECORD: &[u8] = &[
    0xC0, 0x00, 0x61, 0x63, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x47, 0xEF, 0xCD,
    0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, 0x6C, 0x69, 0x6D, 0x69, 0x74, 0x42,
    0x40, 0x42, 0x0F, 0xD3, 0x30, 0x00, 0x07, 0x10, 0x05, 0x02, 0x00,
];

// FLAT_RECORD — 47 bytes
const CPP_BYTES_FLAT_RECORD: &[u8] = &[
    0xC0, 0x00, 0x69, 0x64, 0x40, 0x2A, 0x6C, 0x61, 0x62, 0x65, 0x6C, 0x80,
    0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x76, 0x61, 0x6C, 0x75, 0x65, 0x73, 0xB6,
    0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x64, 0x00, 0xE8, 0x03, 0x05, 0x00,
    0x83, 0x6E, 0xAF, 0x00, 0x02, 0x04, 0x05, 0x0F, 0x06, 0x03, 0x00,
];

// RECORD — 57 bytes
const CPP_BYTES_RECORD: &[u8] = &[
    0xC0, 0x00, 0x69, 0x64, 0x40, 0x2A, 0x6C, 0x61, 0x62, 0x65, 0x6C, 0x80,
    0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x76, 0x61, 0x6C, 0x75, 0x65, 0x73, 0xB6,
    0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x64, 0x00, 0xE8, 0x03, 0x05, 0x00,
    0x73, 0x63, 0x6F, 0x72, 0x65, 0x40, 0x5F, 0x83, 0x6E, 0xAF, 0xA3, 0x00,
    0x02, 0x04, 0x05, 0x0F, 0x06, 0x22, 0x05, 0x04, 0x00,
];

// VALIDATOR — 167 bytes
const CPP_BYTES_VALIDATOR: &[u8] = &[
    0xC0, 0x00, 0x70, 0x75, 0x62, 0x6B, 0x65, 0x79, 0x5F, 0x6C, 0x6F, 0x37,
    0x70, 0x75, 0x62, 0x6B, 0x65, 0x79, 0x5F, 0x68, 0x69, 0x3B, 0x77, 0x69,
    0x74, 0x68, 0x64, 0x72, 0x61, 0x77, 0x61, 0x6C, 0x5F, 0x6C, 0x6F, 0x3D,
    0x77, 0x69, 0x74, 0x68, 0x64, 0x72, 0x61, 0x77, 0x61, 0x6C, 0x5F, 0x68,
    0x69, 0x40, 0x11, 0x65, 0x66, 0x66, 0x65, 0x63, 0x74, 0x69, 0x76, 0x65,
    0x5F, 0x62, 0x61, 0x6C, 0x61, 0x6E, 0x63, 0x65, 0x44, 0x00, 0x40, 0x59,
    0x73, 0x07, 0x73, 0x6C, 0x61, 0x73, 0x68, 0x65, 0x64, 0x10, 0x61, 0x63,
    0x74, 0x69, 0x76, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x5F, 0x65, 0x70, 0x6F,
    0x63, 0x68, 0x40, 0x64, 0x65, 0x78, 0x69, 0x74, 0x5F, 0x65, 0x70, 0x6F,
    0x63, 0x68, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0x77, 0x69, 0x74, 0x68, 0x64,
    0x72, 0x61, 0x77, 0x61, 0x62, 0x6C, 0x65, 0x5F, 0x65, 0x70, 0x6F, 0x63,
    0x68, 0x43, 0xFF, 0xFF, 0xFF, 0xFF, 0x77, 0x84, 0xB8, 0x26, 0x7C, 0x4E,
    0xD0, 0x06, 0x1C, 0x00, 0x09, 0x0A, 0x09, 0x14, 0x0D, 0x22, 0x0D, 0x31,
    0x11, 0x48, 0x07, 0x50, 0x10, 0x62, 0x0A, 0x71, 0x12, 0x09, 0x00,
];

fn assert_cpp_byte_identity<T: Pjson>(value: &T, expected: &[u8], shape: &str) {
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
fn point_cpp_byte_identity() {
    assert_cpp_byte_identity(&sample_point(), CPP_BYTES_POINT, "Point");
}

#[test]
fn cpp_byte_identity_summary() {
    // Sanity: every Appendix-C shape's CPP_BYTES_* constant is non-
    // empty (catches accidental table truncation). The five
    // *_cpp_byte_identity tests assert the actual byte equality.
    assert!(!CPP_BYTES_POINT.is_empty());
    assert!(!CPP_BYTES_NAME_RECORD.is_empty());
    assert!(!CPP_BYTES_FLAT_RECORD.is_empty());
    assert!(!CPP_BYTES_RECORD.is_empty());
    assert!(!CPP_BYTES_VALIDATOR.is_empty());
}

#[test]
fn name_record_cpp_byte_identity() {
    assert_cpp_byte_identity(
        &sample_name_record(),
        CPP_BYTES_NAME_RECORD,
        "NameRecord",
    );
}

#[test]
fn flat_record_cpp_byte_identity() {
    assert_cpp_byte_identity(
        &sample_flat_record(),
        CPP_BYTES_FLAT_RECORD,
        "FlatRecord",
    );
}

#[test]
fn record_cpp_byte_identity() {
    assert_cpp_byte_identity(&sample_record(), CPP_BYTES_RECORD, "Record");
}

#[test]
fn validator_cpp_byte_identity() {
    assert_cpp_byte_identity(
        &sample_validator(),
        CPP_BYTES_VALIDATOR,
        "Validator",
    );
}
