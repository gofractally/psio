//! What a trusted decode saves: `from_pssz` (validates as it reads) against
//! `from_pssz_trusted` on an Order-like record and a list of records.
//!
//!     cargo run --release -p psio --example trusted_decode_bench

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
    placed: psio::time::TimePoint<psio::time::Seconds>,
    lines: Vec<Line>,
    tags: Vec<String>,
    memo: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct Balance {
    account: u64,
    amount: u64,
    updated: u32,
}

fn order(i: u64) -> Order {
    Order {
        id: i,
        customer: format!("customer-{i}"),
        placed: psio::time::TimePoint::from_ticks(1_795_342_530 + i as i64),
        lines: (0..4)
            .map(|l| Line { sku: i * 10 + l, quantity: l as u32 + 1, price: 1_000 + l, note: (l == 0).then(|| "gift".into()) })
            .collect(),
        tags: vec!["new".into(), "priority".into()],
        memo: Some("leave at the door".into()),
    }
}

fn time<R>(label: &str, rounds: u32, mut f: impl FnMut() -> R) -> f64 {
    // Warm up, then the best of five runs.
    for _ in 0..rounds / 10 + 1 {
        black_box(f());
    }
    let mut best = f64::MAX;
    for _ in 0..5 {
        let start = Instant::now();
        for _ in 0..rounds {
            black_box(f());
        }
        best = best.min(start.elapsed().as_nanos() as f64 / rounds as f64);
    }
    println!("{label:<44} {best:>10.1} ns");
    best
}

fn main() {
    let one = psio::to_pssz(&order(7));
    let many = psio::to_pssz(&(0..1000).map(order).collect::<Vec<_>>());
    let balances = psio::to_pssz(&(0..1000).map(|i| Balance { account: i, amount: i * 3, updated: i as u32 }).collect::<Vec<_>>());
    println!("Order: {} bytes; 1000 orders: {} bytes; 1000 balances: {} bytes\n", one.len(), many.len(), balances.len());

    let rows: [(&str, u32, Box<dyn Fn() -> (f64, f64, f64)>); 3] = [
        ("Order", 200_000, {
            let one = one.clone();
            Box::new(move || {
                (
                    time("  validate_pssz", 200_000, || psio::validate_pssz::<Order>(black_box(&one)).unwrap()),
                    time("  from_pssz", 200_000, || psio::from_pssz::<Order>(black_box(&one)).unwrap()),
                    time("  from_pssz_trusted", 200_000, || psio::from_pssz_trusted::<Order>(black_box(&one))),
                )
            })
        }),
        ("Vec<Order> x1000", 300, {
            let many = many.clone();
            Box::new(move || {
                (
                    time("  validate_pssz", 300, || psio::validate_pssz::<Vec<Order>>(black_box(&many)).unwrap()),
                    time("  from_pssz", 300, || psio::from_pssz::<Vec<Order>>(black_box(&many)).unwrap()),
                    time("  from_pssz_trusted", 300, || psio::from_pssz_trusted::<Vec<Order>>(black_box(&many))),
                )
            })
        }),
        ("Vec<Balance> x1000 (fixed layout)", 20_000, {
            let balances = balances.clone();
            Box::new(move || {
                (
                    time("  validate_pssz", 20_000, || psio::validate_pssz::<Vec<Balance>>(black_box(&balances)).unwrap()),
                    time("  from_pssz", 20_000, || psio::from_pssz::<Vec<Balance>>(black_box(&balances)).unwrap()),
                    time("  from_pssz_trusted", 20_000, || psio::from_pssz_trusted::<Vec<Balance>>(black_box(&balances))),
                )
            })
        }),
    ];
    for (name, _, run) in rows {
        println!("{name}");
        let (_, checked, trusted) = run();
        println!("  trusted / checked                            {:>10.2}\n", trusted / checked);
    }
    // The two decoders give the same values.
    assert_eq!(psio::from_pssz_trusted::<Vec<Order>>(&many), psio::from_pssz::<Vec<Order>>(&many).unwrap());
}
