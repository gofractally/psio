//! Rust pjson benchmarks.
//!
//! Mirrors the C++ shape library (`cpp/benchmarks/shapes.hpp`,
//! Appendix C of `docs/pjson-spec.md`) so the Rust numbers can be
//! cross-walked against the C++ snapshot CSVs.
//!
//! Coverage: `encode`, `decode`, `validate`, and `view_one` per shape
//! (Point / NameRecord / FlatRecord / Record / Validator). The
//! `view_one` cell is the canonical-typed view's one-field read on a
//! cached buffer — the latency that headlines §1 of the spec.
//!
//! Run with:
//!     cargo bench -p psio --bench pjson_bench

use criterion::{black_box, criterion_group, criterion_main, Criterion};
use psio::pjson::{from_pjson, to_pjson, validate};
use psio::pjson_struct;
use psio::pjson_view::View;

// ── Shape definitions ────────────────────────────────────────────────────

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

// ── Sample data ──────────────────────────────────────────────────────────

fn sample_point() -> Point {
    Point { x: -42, y: 77 }
}
fn sample_name_record() -> NameRecord {
    NameRecord {
        account: 0x0123_4567_89AB_CDEF,
        limit: 1_000_000,
    }
}
fn sample_flat_record() -> FlatRecord {
    FlatRecord {
        id: 42,
        label: "hello".to_string(),
        values: vec![1, 2, 3, 100, 1000],
    }
}
fn sample_record() -> Record {
    Record {
        id: 42,
        label: "hello".to_string(),
        values: vec![1, 2, 3, 100, 1000],
        score: Some(95),
    }
}
fn sample_validator() -> Validator {
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

// ── Bench cells ──────────────────────────────────────────────────────────

fn bench_encode(c: &mut Criterion) {
    let mut g = c.benchmark_group("pjson_encode");

    let p = sample_point();
    g.bench_function("Point", |b| b.iter(|| to_pjson(black_box(&p))));

    let n = sample_name_record();
    g.bench_function("NameRecord", |b| b.iter(|| to_pjson(black_box(&n))));

    let f = sample_flat_record();
    g.bench_function("FlatRecord", |b| b.iter(|| to_pjson(black_box(&f))));

    let r = sample_record();
    g.bench_function("Record", |b| b.iter(|| to_pjson(black_box(&r))));

    let v = sample_validator();
    g.bench_function("Validator", |b| b.iter(|| to_pjson(black_box(&v))));

    g.finish();
}

fn bench_decode(c: &mut Criterion) {
    let mut g = c.benchmark_group("pjson_decode");

    let buf = to_pjson(&sample_point());
    g.bench_function("Point", |b| {
        b.iter(|| from_pjson::<Point>(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_name_record());
    g.bench_function("NameRecord", |b| {
        b.iter(|| from_pjson::<NameRecord>(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_flat_record());
    g.bench_function("FlatRecord", |b| {
        b.iter(|| from_pjson::<FlatRecord>(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_record());
    g.bench_function("Record", |b| {
        b.iter(|| from_pjson::<Record>(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_validator());
    g.bench_function("Validator", |b| {
        b.iter(|| from_pjson::<Validator>(black_box(&buf)).unwrap())
    });

    g.finish();
}

fn bench_validate(c: &mut Criterion) {
    let mut g = c.benchmark_group("pjson_validate");

    let buf = to_pjson(&sample_point());
    g.bench_function("Point", |b| b.iter(|| validate(black_box(&buf)).unwrap()));

    let buf = to_pjson(&sample_name_record());
    g.bench_function("NameRecord", |b| {
        b.iter(|| validate(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_flat_record());
    g.bench_function("FlatRecord", |b| {
        b.iter(|| validate(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_record());
    g.bench_function("Record", |b| {
        b.iter(|| validate(black_box(&buf)).unwrap())
    });

    let buf = to_pjson(&sample_validator());
    g.bench_function("Validator", |b| {
        b.iter(|| validate(black_box(&buf)).unwrap())
    });

    g.finish();
}

fn bench_view_one(c: &mut Criterion) {
    use psio::pjson_typed::{template_for, TypedView};

    let mut g = c.benchmark_group("pjson_view_one");

    // Point — read field `x`.
    let buf = to_pjson(&sample_point());
    let template = template_for(&[b"x", b"y"]);
    g.bench_function("Point", |b| {
        b.iter(|| {
            let tv = TypedView::from_buffer(black_box(&buf)).unwrap();
            tv.verify_template(black_box(&template)).unwrap();
            tv.field(0).as_int().unwrap()
        })
    });

    let buf = to_pjson(&sample_name_record());
    let template = template_for(&[b"account", b"limit"]);
    g.bench_function("NameRecord", |b| {
        b.iter(|| {
            let tv = TypedView::from_buffer(black_box(&buf)).unwrap();
            tv.verify_template(black_box(&template)).unwrap();
            tv.field(0).as_uint().unwrap()
        })
    });

    let buf = to_pjson(&sample_flat_record());
    let template = template_for(&[b"id", b"label", b"values"]);
    g.bench_function("FlatRecord", |b| {
        b.iter(|| {
            let tv = TypedView::from_buffer(black_box(&buf)).unwrap();
            tv.verify_template(black_box(&template)).unwrap();
            tv.field(0).as_uint().unwrap()
        })
    });

    let buf = to_pjson(&sample_record());
    let template = template_for(&[b"id", b"label", b"values", b"score"]);
    g.bench_function("Record", |b| {
        b.iter(|| {
            let tv = TypedView::from_buffer(black_box(&buf)).unwrap();
            tv.verify_template(black_box(&template)).unwrap();
            tv.field(0).as_uint().unwrap()
        })
    });

    let buf = to_pjson(&sample_validator());
    let template = template_for(&[
        b"pubkey_lo",
        b"pubkey_hi",
        b"withdrawal_lo",
        b"withdrawal_hi",
        b"effective_balance",
        b"slashed",
        b"activation_epoch",
        b"exit_epoch",
        b"withdrawable_epoch",
    ]);
    g.bench_function("Validator", |b| {
        b.iter(|| {
            let tv = TypedView::from_buffer(black_box(&buf)).unwrap();
            tv.verify_template(black_box(&template)).unwrap();
            tv.field(0).as_uint().unwrap()
        })
    });

    g.finish();
}

// Schemaless lookup baseline (hash-prefilter scan, no template).
fn bench_view_lookup(c: &mut Criterion) {
    let mut g = c.benchmark_group("pjson_view_find");

    let buf = to_pjson(&sample_point());
    g.bench_function("Point", |b| {
        b.iter(|| {
            let v = View::new(black_box(&buf));
            v.find(b"x").unwrap()
        })
    });

    let buf = to_pjson(&sample_validator());
    g.bench_function("Validator_first", |b| {
        b.iter(|| {
            let v = View::new(black_box(&buf));
            v.find(b"pubkey_lo").unwrap()
        })
    });
    g.bench_function("Validator_middle", |b| {
        b.iter(|| {
            let v = View::new(black_box(&buf));
            v.find(b"effective_balance").unwrap()
        })
    });
    g.bench_function("Validator_last", |b| {
        b.iter(|| {
            let v = View::new(black_box(&buf));
            v.find(b"withdrawable_epoch").unwrap()
        })
    });

    g.finish();
}

criterion_group!(
    benches,
    bench_encode,
    bench_decode,
    bench_validate,
    bench_view_one,
    bench_view_lookup
);
criterion_main!(benches);
