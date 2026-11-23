//! pSSZ on the shapes of the C++ benchmark (`cpp/benchmarks/shapes.hpp`,
//! `bench_psio_vs_externals.cpp`), with the same sample values, so the Rust
//! and the C++ numbers can be read side by side (`docs/format-comparison.md`
//! section 2, "pssz and fracpack, shape by shape").
//!
//!     cargo run --release -p psio --example pssz_shapes_bench [filter ...]
//!
//! Per shape and operation: nanoseconds per call as the minimum, the median
//! and the maximum of the timed batches (the minimum is the number to
//! compare; the spread says how quiet the machine was), and the heap
//! allocations one call makes.
//!
//! Operations: `size` is `pssz_size`, `encode new` is `to_pssz` (a fresh
//! `Vec` per call, like C++ "encode (new)"), `encode reused` is
//! `to_pssz_into` a cleared buffer, `decode` is `from_pssz`, `trusted` is
//! `from_pssz_trusted`, `validate` is `validate_pssz`. `alloc` is not pSSZ:
//! one `Vec<u8>` of the encoding's size made and dropped, the allocator's
//! share of `encode new`.
//!
//! The shapes with a view also have: `view all`, every field of the value
//! read through `PsszView::trusted` (a service's own table row); `view one`,
//! one field of it; `view checked`, `PsszView::new` (which validates) and
//! then every field.
//!
//! `PSSZ_BENCH_BATCHES` (default 15) and `PSSZ_BENCH_BATCH_MS` (default 20)
//! set the number and the length of the timed batches; `PSSZ_BENCH_OP`
//! keeps the operations whose name contains it (for a profiler).

use psio::pssz::{Pssz, PsszPack, PsszUnpack, PsszValidate, PsszView};
use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

// ── Counting allocator ──────────────────────────────────────────────────────

struct Counting;
static ALLOCATIONS: AtomicU64 = AtomicU64::new(0);
// SAFETY: forwards every call unchanged to the system allocator; the counter
// is the only addition.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        unsafe { System.realloc(pointer, layout, size) }
    }
}
#[global_allocator]
static GLOBAL: Counting = Counting;

// ── Shapes (cpp/benchmarks/shapes.hpp) ──────────────────────────────────────

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct Point {
    x: i32,
    y: i32,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct NameRecord {
    account: u64,
    limit: u64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct FlatRecord {
    id: u32,
    label: String,
    values: Vec<u16>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct Record {
    id: u32,
    label: String,
    values: Vec<u16>,
    score: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView)]
#[psio(definition_will_not_change)]
struct Validator {
    pubkey_lo: u64,
    pubkey_hi: u64,
    withdrawal_lo: u64,
    withdrawal_hi: u64,
    effective_balance: u64,
    slashed: bool,
    activation_epoch: u64,
    exit_epoch: u64,
    withdrawable_epoch: u64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct LineItem {
    product: String,
    qty: u32,
    unit_price: f64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct UserProfile {
    id: u64,
    name: String,
    email: String,
    age: u32,
    verified: bool,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Order {
    id: u64,
    customer: UserProfile,
    items: Vec<LineItem>,
    total: f64,
    note: Option<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct ValidatorList {
    epoch: u64,
    validators: Vec<Validator>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct FlatRecordBounded {
    id: u32,
    #[psio(max = 63)]
    label: String,
    #[psio(max = 255)]
    values: Vec<u32>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct RecordBounded {
    id: u32,
    #[psio(max = 63)]
    label: String,
    #[psio(max = 255)]
    values: Vec<u32>,
    score: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct LineItemBounded {
    #[psio(max = 63)]
    product: String,
    qty: u32,
    unit_price: f64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct UserProfileBounded {
    id: u64,
    #[psio(max = 63)]
    name: String,
    #[psio(max = 255)]
    email: String,
    age: u32,
    verified: bool,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct OrderBounded {
    id: u64,
    customer: UserProfileBounded,
    #[psio(max = 255)]
    items: Vec<LineItemBounded>,
    total: f64,
    #[psio(max = 255)]
    note: Option<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct ValidatorListBounded {
    epoch: u64,
    #[psio(max = 1024)]
    validators: Vec<Validator>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct LineItemDwnc {
    product: String,
    qty: u32,
    unit_price: f64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct UserProfileDwnc {
    id: u64,
    name: String,
    email: String,
    age: u32,
    verified: bool,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct OrderDwnc {
    id: u64,
    customer: UserProfileDwnc,
    items: Vec<LineItemDwnc>,
    total: f64,
    note: Option<String>,
}

macro_rules! deep {
    ($attr:meta; $inner4:ident, $inner3:ident, $inner2:ident, $inner1:ident, $deep:ident) => {
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
        #[$attr]
        struct $inner4 {
            value: u64,
        }
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
        #[$attr]
        struct $inner3 {
            child: $inner4,
            pad3: u64,
        }
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
        #[$attr]
        struct $inner2 {
            child: $inner3,
            pad2: u64,
        }
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
        #[$attr]
        struct $inner1 {
            child: $inner2,
            pad1: u64,
        }
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
        #[$attr]
        struct $deep {
            root: $inner1,
        }
        impl $deep {
            fn sample() -> Self {
                $deep {
                    root: $inner1 {
                        child: $inner2 {
                            child: $inner3 { child: $inner4 { value: 0xDEAD_BEEF_CAFE_F00D }, pad3: 3 },
                            pad2: 2,
                        },
                        pad1: 1,
                    },
                }
            }
        }
    };
}
deep!(doc = "extensible at every level"; Inner4Ext, Inner3Ext, Inner2Ext, Inner1Ext, Deep4Ext);
deep!(psio(definition_will_not_change); Inner4Dwnc, Inner3Dwnc, Inner2Dwnc, Inner1Dwnc, Deep4Dwnc);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct WideRecord {
    f00: u32, f01: u32, f02: u32, f03: u32, f04: u32, f05: u32, f06: u32, f07: u32,
    f08: u32, f09: u32, f10: u32, f11: u32, f12: u32, f13: u32, f14: u32, f15: u32,
    f16: u32, f17: u32, f18: u32, f19: u32, f20: u32, f21: u32, f22: u32, f23: u32,
    f24: u32, f25: u32, f26: u32, f27: u32, f28: u32, f29: u32, f30: u32, f31: u32,
}

/// Not in the C++ benchmark: a typical small table row of a service (a
/// key, two numbers, one short string), extensible as rows are.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct TableRow {
    key: u64,
    balance: u64,
    updated: u64,
    name: String,
}

// ── Reading through a view ──────────────────────────────────────────────────

fn text(view: PsszView<'_, String>) -> u64 {
    view.view().expect("the benchmark's strings are UTF-8").len() as u64
}
fn order_fields(order: PsszView<'_, Order>) -> u64 {
    let customer = order.customer();
    let mut sum = order.id().get()
        ^ customer.id().get()
        ^ text(customer.name())
        ^ text(customer.email())
        ^ customer.age().get() as u64
        ^ customer.verified().get() as u64
        ^ order.total().get().to_bits();
    for item in order.items().iter() {
        sum ^= text(item.product()) ^ item.qty().get() as u64 ^ item.unit_price().get().to_bits();
    }
    if let Some(note) = order.note().get() {
        sum ^= text(note);
    }
    sum
}
fn validator_fields(v: PsszView<'_, Validator>) -> u64 {
    v.pubkey_lo().get()
        ^ v.pubkey_hi().get()
        ^ v.withdrawal_lo().get()
        ^ v.withdrawal_hi().get()
        ^ v.effective_balance().get()
        ^ v.slashed().get() as u64
        ^ v.activation_epoch().get()
        ^ v.exit_epoch().get()
        ^ v.withdrawable_epoch().get()
}
fn row_fields(row: PsszView<'_, TableRow>) -> u64 {
    row.key().get() ^ row.balance().get() ^ row.updated().get() ^ text(row.name())
}

/// What a shape with a view adds: every field, one field, and every field
/// of a view that was validated first.
struct Views {
    all: fn(&[u8]) -> u64,
    one: fn(&[u8]) -> u64,
    checked: fn(&[u8]) -> u64,
}
const ORDER_VIEWS: Views = Views {
    all: |bytes| order_fields(PsszView::trusted(bytes)),
    one: |bytes| PsszView::<Order>::trusted(bytes).total().get().to_bits(),
    checked: |bytes| order_fields(PsszView::new(bytes).unwrap()),
};
const VALIDATOR_VIEWS: Views = Views {
    all: |bytes| validator_fields(PsszView::trusted(bytes)),
    one: |bytes| PsszView::<Validator>::trusted(bytes).effective_balance().get(),
    checked: |bytes| validator_fields(PsszView::new(bytes).unwrap()),
};
const ROW_VIEWS: Views = Views {
    all: |bytes| row_fields(PsszView::trusted(bytes)),
    one: |bytes| PsszView::<TableRow>::trusted(bytes).balance().get(),
    checked: |bytes| row_fields(PsszView::new(bytes).unwrap()),
};
const ORDERS_VIEWS: Views = Views {
    all: |bytes| PsszView::<Vec<Order>>::trusted(bytes).iter().fold(0, |sum, order| sum ^ order_fields(order)),
    one: |bytes| {
        let orders = PsszView::<Vec<Order>>::trusted(bytes);
        orders.get(orders.len() / 2).total().get().to_bits()
    },
    checked: |bytes| {
        PsszView::<Vec<Order>>::new(bytes).unwrap().iter().fold(0, |sum, order| sum ^ order_fields(order))
    },
};

// ── Sample values (namespace psio_bench in shapes.hpp) ──────────────────────

fn validator(i: u64) -> Validator {
    Validator {
        pubkey_lo: i * 7,
        pubkey_hi: i * 11,
        withdrawal_lo: i * 13,
        withdrawal_hi: i * 17,
        effective_balance: 32_000_000_000,
        slashed: i % 50 == 0,
        activation_epoch: 100,
        exit_epoch: 0xFFFF_FFFF,
        withdrawable_epoch: 0xFFFF_FFFF,
    }
}
fn validators(n: u64) -> Vec<Validator> {
    (0..n).map(validator).collect()
}
const ITEMS: [(&str, u32, f64); 3] = [("Widget", 2, 9.99), ("Sprocket", 1, 14.50), ("Gizmo-Mk.II", 5, 3.25)];
const TOTAL: f64 = 2.0 * 9.99 + 14.50 + 5.0 * 3.25;

fn order() -> Order {
    Order {
        id: 10042,
        customer: UserProfile { id: 77, name: "Alice Stone".into(), email: "alice@example.com".into(), age: 34, verified: true },
        items: ITEMS.iter().map(|&(p, qty, unit_price)| LineItem { product: p.into(), qty, unit_price }).collect(),
        total: TOTAL,
        note: Some("gift-wrap please".into()),
    }
}
fn order_bounded() -> OrderBounded {
    OrderBounded {
        id: 10042,
        customer: UserProfileBounded { id: 77, name: "Alice Stone".into(), email: "alice@example.com".into(), age: 34, verified: true },
        items: ITEMS.iter().map(|&(p, qty, unit_price)| LineItemBounded { product: p.into(), qty, unit_price }).collect(),
        total: TOTAL,
        note: Some("gift-wrap please".into()),
    }
}
fn order_dwnc() -> OrderDwnc {
    OrderDwnc {
        id: 10042,
        customer: UserProfileDwnc { id: 77, name: "Alice Stone".into(), email: "alice@example.com".into(), age: 34, verified: true },
        items: ITEMS.iter().map(|&(p, qty, unit_price)| LineItemDwnc { product: p.into(), qty, unit_price }).collect(),
        total: TOTAL,
        note: Some("gift-wrap please".into()),
    }
}
fn wide_record() -> WideRecord {
    let f = |i: u32| 0x1000_0000 + i * 0x0101_0101;
    WideRecord {
        f00: f(0), f01: f(1), f02: f(2), f03: f(3), f04: f(4), f05: f(5), f06: f(6), f07: f(7),
        f08: f(8), f09: f(9), f10: f(10), f11: f(11), f12: f(12), f13: f(13), f14: f(14), f15: f(15),
        f16: f(16), f17: f(17), f18: f(18), f19: f(19), f20: f(20), f21: f(21), f22: f(22), f23: f(23),
        f24: f(24), f25: f(25), f26: f(26), f27: f(27), f28: f(28), f29: f(29), f30: f(30), f31: f(31),
    }
}

// ── Harness ─────────────────────────────────────────────────────────────────

struct Settings {
    batches: usize,
    batch: Duration,
    operation: Option<String>,
}

/// (minimum, median, maximum) ns per call over the timed batches, and the
/// allocations of one call.
fn measure<R>(settings: &Settings, mut f: impl FnMut() -> R) -> (f64, f64, f64, u64) {
    // Calibrate the batch size on a warm run.
    let mut rounds = 16u64;
    loop {
        let start = Instant::now();
        for _ in 0..rounds {
            black_box(f());
        }
        let elapsed = start.elapsed();
        if elapsed >= settings.batch / 4 || rounds >= 1 << 30 {
            let per_call = elapsed.as_nanos().max(1) as f64 / rounds as f64;
            rounds = ((settings.batch.as_nanos() as f64 / per_call) as u64).max(16);
            break;
        }
        rounds *= 4;
    }
    let mut samples: Vec<f64> = (0..settings.batches)
        .map(|_| {
            let start = Instant::now();
            for _ in 0..rounds {
                black_box(f());
            }
            start.elapsed().as_nanos() as f64 / rounds as f64
        })
        .collect();
    samples.sort_by(|a, b| a.total_cmp(b));
    const COUNTED: u64 = 64;
    let before = ALLOCATIONS.load(Ordering::Relaxed);
    for _ in 0..COUNTED {
        black_box(f());
    }
    let allocations = (ALLOCATIONS.load(Ordering::Relaxed) - before) / COUNTED;
    (samples[0], samples[samples.len() / 2], samples[samples.len() - 1], allocations)
}

fn shape<T>(settings: &Settings, filters: &[String], name: &str, value: T)
where
    T: PsszPack<Pssz> + PsszUnpack<Pssz> + PsszValidate<Pssz> + PartialEq + std::fmt::Debug,
{
    shape_with_views(settings, filters, name, value, None)
}

fn shape_with_views<T>(settings: &Settings, filters: &[String], name: &str, value: T, views: Option<&Views>)
where
    T: PsszPack<Pssz> + PsszUnpack<Pssz> + PsszValidate<Pssz> + PartialEq + std::fmt::Debug,
{
    if !filters.is_empty() && !filters.iter().any(|f| name.contains(f.as_str())) {
        return;
    }
    let value = &value;
    let bytes = psio::to_pssz(value);
    let bytes = &bytes;
    assert_eq!(&psio::from_pssz::<T>(bytes).unwrap(), value);
    assert_eq!(&psio::from_pssz_trusted::<T>(bytes), value);
    psio::validate_pssz::<T>(bytes).unwrap();
    assert_eq!(psio::pssz_size(value), bytes.len());
    let mut reused = Vec::with_capacity(bytes.len());

    let wanted = |operation: &str| settings.operation.as_deref().map_or(true, |o| operation.contains(o));
    let line = |operation: &str, (min, median, max, allocations): (f64, f64, f64, u64)| {
        println!("{name:<26} {:>6} {operation:<14} {min:>9.1} {median:>9.1} {max:>9.1} {allocations:>7}", bytes.len());
    };
    if wanted("size") {
        line("size", measure(settings, || psio::pssz_size(black_box(value))));
    }
    if wanted("encode new") {
        line("encode new", measure(settings, || psio::to_pssz(black_box(value))));
    }
    if wanted("encode reused") {
        line(
            "encode reused",
            measure(settings, || {
                reused.clear();
                psio::to_pssz_into(black_box(value), &mut reused).unwrap();
                reused.len()
            }),
        );
    }
    if wanted("decode") {
        line("decode", measure(settings, || psio::from_pssz::<T>(black_box(bytes)).unwrap()));
    }
    if wanted("trusted") {
        line("trusted", measure(settings, || psio::from_pssz_trusted::<T>(black_box(bytes))));
    }
    if wanted("validate") {
        line("validate", measure(settings, || psio::validate_pssz::<T>(black_box(bytes)).unwrap()));
    }
    if wanted("alloc") {
        line("alloc", measure(settings, || Vec::<u8>::with_capacity(black_box(bytes.len()))));
    }
    if let Some(views) = views {
        assert_eq!((views.all)(bytes), (views.checked)(bytes));
        if wanted("view all") {
            line("view all", measure(settings, || (views.all)(black_box(bytes))));
        }
        if wanted("view one") {
            line("view one", measure(settings, || (views.one)(black_box(bytes))));
        }
        if wanted("view checked") {
            line("view checked", measure(settings, || (views.checked)(black_box(bytes))));
        }
    }
}

fn setting(name: &str, default: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn main() {
    let filters: Vec<String> = std::env::args().skip(1).collect();
    let settings = Settings {
        batches: setting("PSSZ_BENCH_BATCHES", 15) as usize,
        batch: Duration::from_millis(setting("PSSZ_BENCH_BATCH_MS", 20)),
        operation: std::env::var("PSSZ_BENCH_OP").ok(),
    };
    println!(
        "{:<26} {:>6} {:<14} {:>9} {:>9} {:>9} {:>7}",
        "shape", "bytes", "operation", "min ns", "median", "max", "allocs"
    );
    let s = &settings;
    let f = &filters[..];
    shape(s, f, "Point", Point { x: -42, y: 77 });
    shape(s, f, "NameRecord", NameRecord { account: 0x0123_4567_89AB_CDEF, limit: 1_000_000 });
    shape(s, f, "FlatRecord", FlatRecord { id: 9, label: "flat-cap".into(), values: vec![3, 5, 8, 13, 21, 34] });
    shape(s, f, "Record", Record { id: 7, label: "oracle".into(), values: vec![1, 2, 65535, 4096, 32768, 0], score: Some(99) });
    shape_with_views(s, f, "Validator", validator(1), Some(&VALIDATOR_VIEWS));
    shape_with_views(s, f, "Order", order(), Some(&ORDER_VIEWS));
    shape(s, f, "ValidatorList(100)", ValidatorList { epoch: 42, validators: validators(100) });
    shape(
        s,
        f,
        "FlatRecordBounded",
        FlatRecordBounded { id: 9, label: "flat-cap".into(), values: vec![3, 5, 8, 13, 21, 34] },
    );
    shape(
        s,
        f,
        "RecordBounded",
        RecordBounded { id: 7, label: "oracle".into(), values: vec![1, 2, 65535, 4096, 32768, 0], score: Some(99) },
    );
    shape(s, f, "OrderBounded", order_bounded());
    shape(s, f, "ValidatorListBounded(100)", ValidatorListBounded { epoch: 42, validators: validators(100) });
    shape(s, f, "OrderDwnc", order_dwnc());
    shape(s, f, "Deep4Ext", Deep4Ext::sample());
    shape(s, f, "Deep4Dwnc", Deep4Dwnc::sample());
    shape(s, f, "WideRecord", wide_record());
    // A list of records, as a root.
    shape(s, f, "Vec<LineItem> x3", order().items);
    shape(s, f, "Vec<Order> x16", (0..16).map(|_| order()).collect::<Vec<_>>());
    // What a service reads: a small table row, and a long list of records.
    shape_with_views(
        s,
        f,
        "TableRow",
        TableRow { key: 0x0123_4567_89AB_CDEF, balance: 1_000_000, updated: 1_795_342_530, name: "alice.stone".into() },
        Some(&ROW_VIEWS),
    );
    shape_with_views(s, f, "Vec<Order> x1000", (0..1000).map(|_| order()).collect::<Vec<_>>(), Some(&ORDERS_VIEWS));
}
