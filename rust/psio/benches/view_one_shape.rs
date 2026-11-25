//! `psio::view` (one shape for every format; gofractally/psio#13) against
//! the formats' own views, on the same bytes:
//!
//! * `members`: every member of a record that holds a nested record, a
//!   string, an optional string and a list (one element of it read);
//! * `iterate16`: a list of 16 records, every member of each.
//!
//! Each reader returns a checksum of what it read, so nothing is optimized
//! away and the three readers of a format can be compared.

use criterion::{black_box, criterion_group, criterion_main, Criterion};
use psio::flatbuf::{FbView as FbViewOf, Flatbuf};
use psio::pssz::Pssz;
use psio::view::{Reads, View};
use psio::{FbPack, FbUnpack, FbView, PsszPack, PsszUnpack, PsszView};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Item {
    product: String,
    qty: u32,
    price: f64,
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[psio(definition_will_not_change)]
#[view(pssz, flatbuf)]
struct Point {
    x: i32,
    y: i32,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Order {
    id: u64,
    name: String,
    items: Vec<Item>,
    origin: Point,
    note: Option<String>,
    paid: bool,
}

fn order() -> Order {
    Order {
        id: 10042,
        name: "Alice Stone".into(),
        items: (0..16).map(|i| Item { product: format!("product-{i:02}"), qty: i + 1, price: 1.5 * i as f64 }).collect(),
        origin: Point { x: 7, y: 8 },
        note: Some("gift-wrap please".into()),
        paid: true,
    }
}

// ── One shape: the same two functions for both formats ──────────────────────

fn members<F: Reads<Order>>(order: View<'_, Order, F>) -> u64 {
    let origin = order.origin();
    let first = order.items().at_index(0);
    order.id()
        + order.name().len() as u64
        + origin.x() as u64
        + origin.y() as u64
        + order.note().map_or(0, str::len) as u64
        + order.paid() as u64
        + first.product().len() as u64
        + first.qty() as u64
        + first.price() as u64
}

fn iterate<F: Reads<Order>>(order: View<'_, Order, F>) -> u64 {
    let mut sum = 0;
    for item in order.items() {
        sum += item.product().len() as u64 + item.qty() as u64 + item.price() as u64;
    }
    sum
}

// ── The formats' own views ──────────────────────────────────────────────────

fn members_pssz(order: psio::PsszView<'_, Order>) -> u64 {
    let origin = order.origin();
    let first = order.items().get(0);
    order.id().get()
        + order.name().view().unwrap().len() as u64
        + origin.x().get() as u64
        + origin.y().get() as u64
        + order.note().get().map_or(0, |note| note.view().unwrap().len()) as u64
        + order.paid().get() as u64
        + first.product().view().unwrap().len() as u64
        + first.qty().get() as u64
        + first.price().get() as u64
}

fn iterate_pssz(order: psio::PsszView<'_, Order>) -> u64 {
    let mut sum = 0;
    for item in order.items().iter() {
        sum += item.product().view().unwrap().len() as u64 + item.qty().get() as u64 + item.price().get() as u64;
    }
    sum
}

fn members_fb(order: FbViewOf<'_, Order>) -> u64 {
    let origin = order.origin();
    let first = order.items().get(0).unwrap();
    order.id()
        + order.name().len() as u64
        + origin.x() as u64
        + origin.y() as u64
        + order.note().map_or(0, str::len) as u64
        + order.paid() as u64
        + first.product().len() as u64
        + first.qty() as u64
        + first.price() as u64
}

fn iterate_fb(order: FbViewOf<'_, Order>) -> u64 {
    let mut sum = 0;
    for item in order.items().iter() {
        sum += item.product().len() as u64 + item.qty() as u64 + item.price() as u64;
    }
    sum
}

fn bench(c: &mut Criterion) {
    let value = order();
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);

    // The readers agree before they are timed.
    let expected = members_pssz(psio::PsszView::<Order>::new(&pssz).unwrap());
    assert_eq!(members(View::<Order, Pssz>::new(&pssz).unwrap()), expected);
    assert_eq!(members(View::<Order, Flatbuf>::new(&flatbuf).unwrap()), expected);
    assert_eq!(members_fb(FbViewOf::<Order>::new(&flatbuf).unwrap()), expected);
    let expected = iterate_pssz(psio::PsszView::<Order>::new(&pssz).unwrap());
    assert_eq!(iterate(View::<Order, Pssz>::new(&pssz).unwrap()), expected);
    assert_eq!(iterate(View::<Order, Flatbuf>::new(&flatbuf).unwrap()), expected);
    assert_eq!(iterate_fb(FbViewOf::<Order>::new(&flatbuf).unwrap()), expected);

    let mut g = c.benchmark_group("members");
    g.bench_function("pssz/PsszView", |b| b.iter(|| members_pssz(psio::PsszView::<Order>::trusted(black_box(&pssz)))));
    g.bench_function("pssz/View", |b| b.iter(|| members(View::<Order, Pssz>::trusted(black_box(&pssz)))));
    g.bench_function("flatbuf/FbView", |b| b.iter(|| members_fb(FbViewOf::<Order>::trusted(black_box(&flatbuf)))));
    g.bench_function("flatbuf/View", |b| b.iter(|| members(View::<Order, Flatbuf>::trusted(black_box(&flatbuf)))));
    g.finish();

    let mut g = c.benchmark_group("iterate16");
    g.bench_function("pssz/PsszView", |b| b.iter(|| iterate_pssz(psio::PsszView::<Order>::trusted(black_box(&pssz)))));
    g.bench_function("pssz/View", |b| b.iter(|| iterate(View::<Order, Pssz>::trusted(black_box(&pssz)))));
    g.bench_function("flatbuf/FbView", |b| b.iter(|| iterate_fb(FbViewOf::<Order>::trusted(black_box(&flatbuf)))));
    g.bench_function("flatbuf/View", |b| b.iter(|| iterate(View::<Order, Flatbuf>::trusted(black_box(&flatbuf)))));
    g.finish();
}

criterion_group!(benches, bench);
criterion_main!(benches);
