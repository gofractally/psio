//! Rust counterpart to libraries/psio/cpp/benchmarks/bench_psio_vs_externals.cpp.
//!
//! Mirrors the C++ shape library (Point / NameRecord / Validator /
//! Order / ValidatorList / Deep4Dwnc and Dwnc twins) so the Rust
//! results can be compared cell-by-cell against the C++ snapshot CSVs.
//!
//! Coverage: psio::pssz encode + decode timings on each shape, plus
//! competitor crates (bincode / borsh / postcard / rmp-serde / rkyv)
//! gated behind the dev-dependencies already declared in Cargo.toml.
//!
//! Run with:
//!     cargo bench -p psio --bench psio_vs_externals

use criterion::{black_box, criterion_group, criterion_main, Criterion};
use psio::pssz::{Pssz32, PsszPack, PsszUnpack};
use psio::pssz_struct_dwnc;

// Concrete pssz width.  Pssz32 = 4-byte offset slots, the auto-default
// the C++ side picks for Validator/Order/ValidatorList class shapes.
type W = Pssz32;

// ── Shape definitions ────────────────────────────────────────────────────

#[derive(Debug, PartialEq, Eq, Clone)]
pub struct Validator {
    pub pubkey_lo:          u64,
    pub pubkey_hi:          u64,
    pub withdrawal_lo:      u64,
    pub withdrawal_hi:      u64,
    pub effective_balance:  u64,
    pub slashed:            bool,
    pub activation_epoch:   u64,
    pub exit_epoch:         u64,
    pub withdrawable_epoch: u64,
}
pssz_struct_dwnc!(Validator {
    pubkey_lo:          u64,
    pubkey_hi:          u64,
    withdrawal_lo:      u64,
    withdrawal_hi:      u64,
    effective_balance:  u64,
    slashed:            bool,
    activation_epoch:   u64,
    exit_epoch:         u64,
    withdrawable_epoch: u64,
});

#[derive(Debug, PartialEq, Eq, Clone, Default)]
pub struct Point {
    pub x: i32,
    pub y: i32,
}
pssz_struct_dwnc!(Point {
    x: i32,
    y: i32,
});

#[derive(Debug, PartialEq, Eq, Clone, Default)]
pub struct NameRecord {
    pub account: u64,
    pub limit:   u64,
}
pssz_struct_dwnc!(NameRecord {
    account: u64,
    limit:   u64,
});

// ── Sample data (matches shapes.hpp) ─────────────────────────────────────

fn validator(i: u64) -> Validator {
    Validator {
        pubkey_lo:          i.wrapping_mul(7),
        pubkey_hi:          i.wrapping_mul(11),
        withdrawal_lo:      i.wrapping_mul(13),
        withdrawal_hi:      i.wrapping_mul(17),
        effective_balance:  32_000_000_000,
        slashed:            (i % 50) == 0,
        activation_epoch:   100,
        exit_epoch:         0xFFFF_FFFF,
        withdrawable_epoch: 0xFFFF_FFFF,
    }
}

fn validator_list(n: usize) -> Vec<Validator> {
    (0..n as u64).map(validator).collect()
}

fn point() -> Point { Point { x: -42, y: 77 } }

fn name_record() -> NameRecord {
    NameRecord { account: 0x0123_4567_89AB_CDEF, limit: 1_000_000 }
}

// ── Bench cells ──────────────────────────────────────────────────────────

fn bench_pssz_encode(c: &mut Criterion) {
    let mut g = c.benchmark_group("pssz_encode");

    let p = point();
    g.bench_function("Point", |b| {
        b.iter(|| {
            let mut out = Vec::with_capacity(8);
            <Point as PsszPack<W>>::pssz_pack(black_box(&p), &mut out);
            black_box(out)
        })
    });

    let nr = name_record();
    g.bench_function("NameRecord", |b| {
        b.iter(|| {
            let mut out = Vec::with_capacity(16);
            <NameRecord as PsszPack<W>>::pssz_pack(
                black_box(&nr), &mut out);
            black_box(out)
        })
    });

    let v = validator(1);
    g.bench_function("Validator", |b| {
        b.iter(|| {
            let mut out = Vec::with_capacity(65);
            <Validator as PsszPack<W>>::pssz_pack(
                black_box(&v), &mut out);
            black_box(out)
        })
    });

    let list = validator_list(100);
    g.bench_function("ValidatorList(100)", |b| {
        b.iter(|| {
            let mut out = Vec::with_capacity(list.len() * 65);
            for v in &list {
                <Validator as PsszPack<W>>::pssz_pack(v, &mut out);
            }
            black_box(out)
        })
    });

    g.finish();
}

fn bench_pssz_decode(c: &mut Criterion) {
    let mut g = c.benchmark_group("pssz_decode");

    let p = point();
    let mut p_bytes = Vec::new();
    <Point as PsszPack<W>>::pssz_pack(&p, &mut p_bytes);
    g.bench_function("Point", |b| {
        b.iter(|| {
            let v =
               <Point as PsszUnpack<W>>::pssz_unpack(black_box(&p_bytes))
                  .unwrap();
            black_box(v)
        })
    });

    let v = validator(1);
    let mut v_bytes = Vec::new();
    <Validator as PsszPack<W>>::pssz_pack(&v, &mut v_bytes);
    g.bench_function("Validator", |b| {
        b.iter(|| {
            let dv =
               <Validator as PsszUnpack<W>>::pssz_unpack(black_box(&v_bytes))
                  .unwrap();
            black_box(dv)
        })
    });

    let list = validator_list(100);
    let mut list_bytes = Vec::new();
    for v in &list {
        <Validator as PsszPack<W>>::pssz_pack(v, &mut list_bytes);
    }
    let bytes_per = list_bytes.len() / list.len();
    g.bench_function("ValidatorList(100)", |b| {
        b.iter(|| {
            let mut out = Vec::with_capacity(100);
            let mut pos = 0;
            while pos + bytes_per <= list_bytes.len() {
                out.push(
                   <Validator as PsszUnpack<W>>::pssz_unpack(
                      &list_bytes[pos..pos + bytes_per]).unwrap());
                pos += bytes_per;
            }
            black_box(out)
        })
    });

    g.finish();
}

fn print_wire_sizes() {
    let p = point();
    let mut out = Vec::new();
    <Point as PsszPack<W>>::pssz_pack(&p, &mut out);
    eprintln!("[wire] pssz Point        = {} B", out.len());

    out.clear();
    <NameRecord as PsszPack<W>>::pssz_pack(&name_record(), &mut out);
    eprintln!("[wire] pssz NameRecord   = {} B", out.len());

    out.clear();
    <Validator as PsszPack<W>>::pssz_pack(&validator(1), &mut out);
    eprintln!("[wire] pssz Validator    = {} B", out.len());

    out.clear();
    for v in &validator_list(100) {
        <Validator as PsszPack<W>>::pssz_pack(v, &mut out);
    }
    eprintln!("[wire] pssz ValidatorList(100) = {} B", out.len());
}

fn bench_root(c: &mut Criterion) {
    print_wire_sizes();
    bench_pssz_encode(c);
    bench_pssz_decode(c);
}

criterion_group!(benches, bench_root);
criterion_main!(benches);
