//! Encode / decode cost of the width rules (docs/pssz-spec.md 3.1, 3.6):
//! an Order-like record and lists of strings, without bounds, bounded by a
//! member annotation, and beneath a capped record.
//!
//!     cargo run --release -p psio --example pssz_width_bench

use psio::{PsszPack, PsszUnpack};
use std::hint::black_box;
use std::time::Instant;

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct Line {
    sku: u64,
    quantity: u32,
    price: u64,
    note: Option<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct Order {
    id: u64,
    customer: String,
    lines: Vec<Line>,
    tags: Vec<String>,
    memo: Option<String>,
}

/// The same record under a cap: its lists lie beneath the cap.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[pssz(max_dynamic_data = 60000)]
struct CappedOrder {
    id: u64,
    customer: String,
    lines: Vec<Line>,
    tags: Vec<String>,
    memo: Option<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct Tags {
    tags: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct BoundedTags {
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[pssz(max_dynamic_data = 255)]
struct CappedTags {
    tags: Vec<String>,
}

fn lines(i: u64) -> Vec<Line> {
    (0..4)
        .map(|l| Line { sku: i * 10 + l, quantity: l as u32 + 1, price: 1_000 + l, note: (l == 0).then(|| "gift".into()) })
        .collect()
}
fn tags() -> Vec<String> {
    (0..8).map(|i| format!("tag-number-{i}")).collect()
}

fn time<R>(label: &str, rounds: u32, mut f: impl FnMut() -> R) {
    for _ in 0..rounds / 10 + 1 {
        black_box(f());
    }
    let mut best = f64::MAX;
    for _ in 0..7 {
        let start = Instant::now();
        for _ in 0..rounds {
            black_box(f());
        }
        best = best.min(start.elapsed().as_nanos() as f64 / rounds as f64);
    }
    println!("{label:<40} {best:>10.1} ns");
}

fn row<T>(name: &str, value: &T)
where
    T: psio::pssz::PsszPack<psio::pssz::Pssz> + psio::pssz::PsszUnpack<psio::pssz::Pssz> + PartialEq + std::fmt::Debug,
{
    const ROUNDS: u32 = 200_000;
    let bytes = psio::to_pssz(value);
    assert_eq!(&psio::from_pssz::<T>(&bytes).unwrap(), value);
    println!("{name}: {} bytes", bytes.len());
    time("  to_pssz", ROUNDS, || psio::to_pssz(black_box(value)));
    time("  from_pssz", ROUNDS, || psio::from_pssz::<T>(black_box(&bytes)).unwrap());
    time("  from_pssz_trusted", ROUNDS, || psio::from_pssz_trusted::<T>(black_box(&bytes)));
}

fn main() {
    let order = Order {
        id: 7,
        customer: "customer-7".into(),
        lines: lines(7),
        tags: vec!["new".into(), "priority".into()],
        memo: Some("leave at the door".into()),
    };
    let capped = CappedOrder {
        id: order.id,
        customer: order.customer.clone(),
        lines: order.lines.clone(),
        tags: order.tags.clone(),
        memo: order.memo.clone(),
    };
    row("Order", &order);
    row("Order beneath a cap of 60000", &capped);
    row("Vec<String> x8", &tags());
    row("record { Vec<String> x8 }", &Tags { tags: tags() });
    row("record { Vec<String> x8, max 8 x max 16 }", &BoundedTags { tags: tags() });
    row("record @cap=255 { Vec<String> x8 }", &CappedTags { tags: tags() });
}
