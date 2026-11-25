//! Views of one shape for every format (`psio::view`; gofractally/psio#13).
//!
//! One record, encoded as PSSZ and as FlatBuffers, read by the same
//! functions: the format is a type parameter.

use psio::flatbuf::Flatbuf;
use psio::pssz::{Pssz, PsszUnder};
use psio::view::{ListView, Reads, View};
use psio::pssz::PsszPack as PsszPackTrait;
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
    points: Vec<Point>,
    origin: Point,
    grid: [Point; 2],
    note: Option<String>,
    paid: bool,
}

fn order() -> Order {
    Order {
        id: 10042,
        name: "Alice Stöne".into(),
        items: vec![
            Item { product: "Widget".into(), qty: 2, price: 9.99 },
            Item { product: "".into(), qty: 1, price: 14.5 },
            Item { product: "Gizmo-Mk.II".into(), qty: 5, price: 3.25 },
        ],
        points: vec![Point { x: 1, y: -1 }, Point { x: 2, y: -2 }],
        origin: Point { x: 7, y: 8 },
        grid: [Point { x: 3, y: 4 }, Point { x: 5, y: 6 }],
        note: Some("gift-wrap please".into()),
        paid: true,
    }
}

// ── Written once, for every format ──────────────────────────────────────────

/// The quantity of the items that have a product name.
fn total<F: Reads<Order>>(order: View<'_, Order, F>) -> u64 {
    let mut total = 0;
    for item in order.items() {
        if !item.product().is_empty() {
            total += item.qty() as u64;
        }
    }
    total
}

/// The note, or "" when there is none.
fn note<'a, F: Reads<Order>>(order: View<'a, Order, F>) -> &'a str {
    order.note().unwrap_or("")
}

fn point<F: Reads<Point>>(point: View<'_, Point, F>) -> Point {
    Point { x: point.x(), y: point.y() }
}

/// Every member, read through the view.
fn read<F: Reads<Order>>(order: View<'_, Order, F>) -> Order {
    let grid = order.grid();
    Order {
        id: order.id(),
        name: order.name().to_owned(),
        items: order
            .items()
            .iter()
            .map(|item| Item { product: item.product().to_owned(), qty: item.qty(), price: item.price() })
            .collect(),
        points: order.points().into_iter().map(point).collect(),
        origin: point(order.origin()),
        // (`get`, not `at_index`: a malformed list read through a trusted
        // view may be shorter than the type says.)
        grid: [0, 1].map(|i| grid.get(i).map(point).unwrap_or(Point { x: 0, y: 0 })),
        note: order.note().map(str::to_owned),
        paid: order.paid(),
    }
}

/// What a list gives.
fn lists<F: Reads<Order>>(order: View<'_, Order, F>) {
    let items = order.items();
    assert_eq!(items.len(), 3);
    assert!(!items.is_empty());
    assert_eq!(items.get(2).map(|item| item.qty()), Some(5));
    assert!(items.get(3).is_none());
    assert_eq!(items.at_index(0).product(), "Widget");
    assert_eq!(items.iter().len(), 3);
    assert_eq!(items.iter().rev().map(|item| item.qty()).collect::<Vec<_>>(), [5, 1, 2]);
    let mut seen = 0;
    for item in &items {
        seen += item.qty();
    }
    assert_eq!(seen, 8);
    assert_eq!(order.grid().len(), 2);
    assert_eq!(order.points().iter().map(|p| p.x() + p.y()).sum::<i32>(), 0);
}

#[test]
fn one_function_reads_both_formats() {
    let value = order();
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);
    assert_ne!(pssz, flatbuf);

    let in_pssz = View::<Order, Pssz>::new(&pssz).unwrap();
    let in_flatbuf = View::<Order, Flatbuf>::new(&flatbuf).unwrap();

    assert_eq!(total(in_pssz), 7);
    assert_eq!(total(in_flatbuf), 7);
    assert_eq!(note(in_pssz), "gift-wrap please");
    assert_eq!(note(in_flatbuf), "gift-wrap please");
    assert_eq!(read(in_pssz), value);
    assert_eq!(read(in_flatbuf), value);
    lists(in_pssz);
    lists(in_flatbuf);

    // Trusted views of the same bytes read the same.
    assert_eq!(read(View::<Order, Pssz>::trusted(&pssz)), value);
    assert_eq!(read(View::<Order, Flatbuf>::trusted(&flatbuf)), value);
}

#[test]
fn members_read_as_their_natural_types() {
    let value = order();
    let bytes = psio::to_pssz(&value);
    let order = View::<Order, Pssz>::new(&bytes).unwrap();
    let id: u64 = order.id();
    let name: &str = order.name();
    let note: Option<&str> = order.note();
    let paid: bool = order.paid();
    let items: ListView<'_, Vec<Item>, Pssz> = order.items();
    let origin: View<'_, Point, Pssz> = order.origin();
    assert_eq!((id, name, note, paid), (10042, "Alice Stöne", Some("gift-wrap please"), true));
    assert_eq!(items.at_index(2).price(), 3.25);
    assert_eq!((origin.x(), origin.y()), (7, 8));

    let none = Order { note: None, ..value };
    let pssz = psio::to_pssz(&none);
    let flatbuf = psio::to_flatbuf(&none);
    assert_eq!(View::<Order, Pssz>::new(&pssz).unwrap().note(), None);
    assert_eq!(View::<Order, Flatbuf>::new(&flatbuf).unwrap().note(), None);
}

// ── A record with a cap: the PSSZ position ──────────────────────────────────

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[pssz(max_dynamic_data = 255)]
#[view(pssz, flatbuf)]
struct Small {
    tags: Vec<String>,
    inner: Vec<Item>,
    label: Option<String>,
}

fn small_tags<F: Reads<Small>>(small: View<'_, Small, F>) -> Vec<String> {
    let mut out: Vec<String> = small.tags().iter().map(str::to_owned).collect();
    for item in small.inner() {
        out.push(format!("{}x{}", item.qty(), item.product()));
    }
    out.extend(small.label().map(str::to_owned));
    out
}

#[test]
fn members_of_a_capped_record_are_read_at_the_caps_widths() {
    let value = Small {
        tags: vec!["a".into(), "bc".into()],
        inner: vec![Item { product: "nut".into(), qty: 3, price: 0.5 }],
        label: Some("z".into()),
    };
    let pssz = psio::to_pssz(&value);
    let small = View::<Small, Pssz>::new(&pssz).unwrap();
    // The format of the members is the position beneath the cap.
    let tags: ListView<'_, Vec<String>, PsszUnder<255>> = small.tags();
    assert_eq!(tags.len(), 2);
    // The existing view agrees, and a 1-byte offset table is what is there.
    let old = psio::PsszView::<Small>::new(&pssz).unwrap();
    assert_eq!(old.tags().get(1).view().unwrap(), "bc");
    assert!(<Vec<String> as PsszPackTrait<PsszUnder<255>>>::OFFSET_WIDTH == 1);

    let expected = vec!["a".to_owned(), "bc".into(), "3xnut".into(), "z".into()];
    assert_eq!(small_tags(small), expected);
    let flatbuf = psio::to_flatbuf(&value);
    assert_eq!(small_tags(View::<Small, Flatbuf>::new(&flatbuf).unwrap()), expected);

    // The same record beneath another capped record: the format is a
    // position, and the function is the same.
    assert_eq!(small_tags(View::<Small, PsszUnder<255>>::new(&pssz).unwrap()), expected);
}

// ── Bytes nobody validated ──────────────────────────────────────────────────

/// Runs `f`, which may panic; reports whether it did. Nothing else can
/// happen: the code under test has no `unsafe` that depends on the bytes.
fn panics(f: impl FnOnce() + std::panic::UnwindSafe) -> bool {
    std::panic::catch_unwind(f).is_err()
}

#[test]
fn new_rejects_what_does_not_validate() {
    let value = order();
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);
    // The validator is the format's own: `new` accepts exactly what the
    // format's view accepts. (A truncated PSSZ record can be a valid, shorter
    // one, so the comparison is with that validator and not with "error".)
    let mut rejected = [0usize; 2];
    for end in 0..pssz.len() {
        let ours = View::<Order, Pssz>::new(&pssz[..end]).is_ok();
        assert_eq!(ours, psio::PsszView::<Order>::new(&pssz[..end]).is_ok(), "pssz truncated to {end}");
        rejected[0] += !ours as usize;
    }
    for end in 0..flatbuf.len() {
        let ours = View::<Order, Flatbuf>::new(&flatbuf[..end]).is_ok();
        assert_eq!(ours, psio::flatbuf::FbView::<Order>::new(&flatbuf[..end]).is_ok(), "flatbuf truncated to {end}");
        rejected[1] += !ours as usize;
    }
    assert!(rejected[0] > pssz.len() / 2 && rejected[1] > flatbuf.len() / 2, "{rejected:?}");
    // The other format's bytes are not this format's.
    assert!(View::<Order, Pssz>::new(&flatbuf).is_err());
    assert!(View::<Order, Flatbuf>::new(&pssz).is_err());
    // A struct is not the root of a FlatBuffers buffer.
    assert!(View::<Point, Flatbuf>::new(&flatbuf).is_err());
}

#[test]
fn a_string_that_is_not_utf8() {
    let value = Item { product: "abc".into(), qty: 1, price: 1.0 };
    let mut pssz = psio::to_pssz(&value);
    let at = pssz.windows(3).position(|w| w == b"abc").unwrap();
    pssz[at] = 0xff;
    assert!(View::<Item, Pssz>::new(&pssz).is_err());
    let item = View::<Item, Pssz>::trusted(&pssz);
    // The other members are still read; the string is refused, not handed
    // out as a `&str` that is not UTF-8.
    assert_eq!(item.qty(), 1);
    assert!(panics(move || {
        item.product();
    }));

    let mut flatbuf = psio::to_flatbuf(&value);
    let at = flatbuf.windows(3).position(|w| w == b"abc").unwrap();
    flatbuf[at] = 0xff;
    assert!(View::<Item, Flatbuf>::new(&flatbuf).is_err());
    // FlatBuffers views read what does not fit as absent.
    assert_eq!(View::<Item, Flatbuf>::trusted(&flatbuf).product(), "");
}

#[test]
fn trusted_views_of_malformed_bytes_panic_or_read_defaults() {
    let value = order();
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);
    // Every truncation and every single-byte corruption: reading everything
    // through a trusted view either gives a value or panics. (Running under
    // the test harness, a read outside the buffer would be a panic too:
    // there is no `unsafe` indexing on this path.)
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let mut outcomes = [0usize; 2];
    let mut try_pssz = |bytes: Vec<u8>| {
        let panicked = panics(move || {
            let _ = read(View::<Order, Pssz>::trusted(&bytes));
        });
        outcomes[panicked as usize] += 1;
    };
    for end in 0..pssz.len() {
        try_pssz(pssz[..end].to_vec());
    }
    for at in 0..pssz.len() {
        for flip in [0x01u8, 0x80, 0xff] {
            let mut bytes = pssz.clone();
            bytes[at] ^= flip;
            try_pssz(bytes);
        }
    }
    std::panic::set_hook(hook);
    assert!(outcomes[0] > 0 && outcomes[1] > 0, "some corruptions read, some panic: {outcomes:?}");

    // FlatBuffers views never panic.
    for end in 0..flatbuf.len() {
        let bytes = flatbuf[..end].to_vec();
        let _ = read(View::<Order, Flatbuf>::trusted(&bytes));
    }
    for at in 0..flatbuf.len() {
        for flip in [0x01u8, 0x80, 0xff] {
            let mut bytes = flatbuf.clone();
            bytes[at] ^= flip;
            let _ = read(View::<Order, Flatbuf>::trusted(&bytes));
        }
    }
}
