//! Every kind of type the one-shape views read (`psio::view`;
//! gofractally/psio#13): each is read through one function that is generic
//! over the format, against PSSZ and, where FlatBuffers has the kind,
//! against FlatBuffers too.

use std::collections::{BTreeMap, BTreeSet};

use psio::flatbuf::Flatbuf;
use psio::pssz::{
    BoundedList, BoundedMap, BoundedSet, BoundedString, Pssz, Pssz16, Pssz32, Pssz8, PsszUnder, RawString, Uint256,
};
use psio::view::{self, ListView, MapView, Reads, SetView, View};
use psio::{FbPack, FbUnpack, FbView, PsszPack, PsszUnpack, PsszView};

/// Runs `f`, which may panic; reports whether it did.
fn panics(f: impl FnOnce() + std::panic::UnwindSafe) -> bool {
    std::panic::catch_unwind(f).is_err()
}

/// Every truncation of `bytes` and three corruptions of every byte.
fn corruptions(bytes: &[u8]) -> Vec<Vec<u8>> {
    let mut out: Vec<Vec<u8>> = (0..bytes.len()).map(|end| bytes[..end].to_vec()).collect();
    for at in 0..bytes.len() {
        for flip in [0x01u8, 0x80, 0xff] {
            let mut changed = bytes.to_vec();
            changed[at] ^= flip;
            out.push(changed);
        }
    }
    out
}

/// Reading everything through a trusted view of bytes nobody validated
/// gives a value or panics; returns how many of each. (A read outside the
/// buffer would be a panic too: there is no `unsafe` on this path.)
fn survives(bytes: &[u8], read: impl Fn(&[u8]) + std::panic::RefUnwindSafe) -> [usize; 2] {
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let mut outcomes = [0usize; 2];
    for changed in corruptions(bytes) {
        outcomes[panics(|| read(&changed)) as usize] += 1;
    }
    std::panic::set_hook(hook);
    outcomes
}

// ── The types, in both formats ──────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[repr(u8)]
#[view(pssz, flatbuf)]
enum Color {
    Red = 0,
    Green = 1,
    Blue = 2,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Item {
    product: String,
    qty: u32,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Circle {
    radius: u32,
    label: String,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Square {
    side: u32,
}

/// An enum with payloads: a PSSZ variant, a FlatBuffers union.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
enum Figure {
    Nothing,
    Circle(Circle),
    Square(Square),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Catalog {
    name: String,
    color: Color,
    figure: Figure,
    prices: BTreeMap<String, u32>,
    codes: BTreeSet<u32>,
    by_id: BTreeMap<u32, Item>,
    names: BTreeSet<String>,
    raw: RawString,
    #[psio(max = 8)]
    tags: Vec<String>,
    #[psio(max = 32)]
    title: String,
    #[psio(max = 4, element(max = 16))]
    lines: Vec<String>,
    kind: Option<Color>,
    spare: Option<Item>,
}

fn catalog() -> Catalog {
    Catalog {
        name: "spring".into(),
        color: Color::Blue,
        figure: Figure::Circle(Circle { radius: 3, label: "round".into() }),
        prices: [("apple".to_owned(), 5), ("pear".to_owned(), 7), ("zest".to_owned(), 1)].into(),
        codes: [9, 2, 40].into(),
        by_id: [(7, Item { product: "nut".into(), qty: 3 }), (2, Item { product: "bolt".into(), qty: 8 })].into(),
        names: ["b".to_owned(), "a".to_owned()].into(),
        raw: RawString(vec![0xff, 0x00, 0x7f]),
        tags: vec!["x".into(), "yz".into()],
        title: "A title".into(),
        lines: vec!["one".into(), "two".into()],
        kind: Some(Color::Green),
        spare: None,
    }
}

// ── Written once, for every format ──────────────────────────────────────────

fn item<F: Reads<Item>>(item: View<'_, Item, F>) -> Item {
    Item { product: item.product().to_owned(), qty: item.qty() }
}

/// An enum with payloads reads as an enum to `match` on.
fn figure<F: Reads<Figure>>(figure: View<'_, Figure, F>) -> Figure {
    match figure {
        FigureView::Nothing => Figure::Nothing,
        FigureView::Circle(circle) => Figure::Circle(Circle { radius: circle.radius(), label: circle.label().to_owned() }),
        FigureView::Square(square) => Figure::Square(Square { side: square.side() }),
    }
}

/// Every member, read through the view.
fn read<F: Reads<Catalog>>(catalog: View<'_, Catalog, F>) -> Catalog {
    Catalog {
        name: catalog.name().to_owned(),
        color: catalog.color(),
        figure: figure(catalog.figure()),
        prices: catalog.prices().iter().map(|(name, price)| (name.to_owned(), price)).collect(),
        codes: catalog.codes().iter().collect(),
        by_id: catalog.by_id().into_iter().map(|(id, value)| (id, item(value))).collect(),
        names: catalog.names().iter().map(str::to_owned).collect(),
        raw: RawString(catalog.raw().to_vec()),
        tags: catalog.tags().iter().map(str::to_owned).collect(),
        title: catalog.title().to_owned(),
        lines: catalog.lines().iter().map(str::to_owned).collect(),
        kind: catalog.kind(),
        spare: catalog.spare().map(item),
    }
}

/// What maps and sets give.
fn lookups<F: Reads<Catalog>>(catalog: View<'_, Catalog, F>) {
    let prices = catalog.prices();
    assert_eq!(prices.len(), 3);
    assert!(!prices.is_empty());
    assert_eq!(prices.entry(0), Some(("apple", 5)));
    assert_eq!(prices.entry(3), None);
    assert_eq!(prices.keys().collect::<Vec<_>>(), ["apple", "pear", "zest"]);
    assert_eq!(prices.values().sum::<u32>(), 13);
    assert_eq!(prices.iter().rev().next(), Some(("zest", 1)));
    // By key: a binary search over the entries, which are in key order in
    // every format.
    assert_eq!(prices.get("pear"), Some(7));
    assert_eq!(prices.get("plum"), None);
    assert_eq!(prices.find("zest"), Some(2));
    assert!(prices.contains_key("apple") && !prices.contains_key(""));
    let mut total = 0;
    for (_, price) in &prices {
        total += price;
    }
    assert_eq!(total, 13);

    let by_id = catalog.by_id();
    assert_eq!(by_id.get(7).map(|item| item.qty()), Some(3));
    assert_eq!(by_id.get(2).map(|item| item.product()), Some("bolt"));
    assert!(by_id.get(3).is_none());
    assert_eq!(by_id.keys().collect::<Vec<_>>(), [2, 7]);

    let codes = catalog.codes();
    assert_eq!(codes.len(), 3);
    assert_eq!(codes.iter().collect::<Vec<_>>(), [2, 9, 40]);
    assert_eq!(codes.get(1), Some(9));
    assert_eq!(codes.get(3), None);
    assert!(codes.contains(40) && !codes.contains(41));
    assert_eq!(codes.find(9), Some(1));
    let names = catalog.names();
    assert!(names.contains("a") && names.contains("b") && !names.contains("c"));
    assert_eq!(names.into_iter().collect::<Vec<_>>(), ["a", "b"]);
}

#[test]
fn one_function_reads_every_kind_in_both_formats() {
    let value = catalog();
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);

    let in_pssz = View::<Catalog, Pssz>::new(&pssz).unwrap();
    let in_flatbuf = View::<Catalog, Flatbuf>::new(&flatbuf).unwrap();
    assert_eq!(read(in_pssz), value);
    assert_eq!(read(in_flatbuf), value);
    lookups(in_pssz);
    lookups(in_flatbuf);

    // The other alternatives, and nothing where something is optional.
    for figure in [Figure::Nothing, Figure::Square(Square { side: 4 })] {
        let value = Catalog { figure, kind: None, spare: Some(Item { product: "s".into(), qty: 1 }), ..catalog() };
        assert_eq!(read(View::<Catalog, Pssz>::new(&psio::to_pssz(&value)).unwrap()), value);
        assert_eq!(read(View::<Catalog, Flatbuf>::new(&psio::to_flatbuf(&value)).unwrap()), value);
    }
}

#[test]
fn members_read_as_their_natural_types() {
    let value = catalog();
    let bytes = psio::to_pssz(&value);
    let catalog = View::<Catalog, Pssz>::new(&bytes).unwrap();
    let color: Color = catalog.color();
    let kind: Option<Color> = catalog.kind();
    let raw: &[u8] = catalog.raw();
    let title: &str = catalog.title();
    let prices: MapView<'_, BTreeMap<String, u32>, Pssz> = catalog.prices();
    let codes: SetView<'_, BTreeSet<u32>, Pssz> = catalog.codes();
    // A member with a bound is the bounded type: it is read at the widths
    // the bound selects.
    let tags: ListView<'_, BoundedList<String, 8>, Pssz> = catalog.tags();
    let lines: ListView<'_, BoundedList<BoundedString<16>, 4>, Pssz> = catalog.lines();
    assert_eq!((color, kind, raw, title), (Color::Blue, Some(Color::Green), &[0xff, 0x00, 0x7f][..], "A title"));
    assert_eq!((prices.len(), codes.len(), tags.len(), lines.at_index(1)), (3, 3, 2, "two"));
    assert!(matches!(catalog.figure(), FigureView::Circle(circle) if circle.radius() == 3));
    // The format's own view agrees about the bounded member.
    assert_eq!(psio::PsszView::<Catalog>::new(&bytes).unwrap().tags().get(1).view().unwrap(), "yz");

    // In FlatBuffers the same accessors have the same types but for the tag.
    let bytes = psio::to_flatbuf(&value);
    let catalog = View::<Catalog, Flatbuf>::new(&bytes).unwrap();
    let tags: ListView<'_, BoundedList<String, 8>, Flatbuf> = catalog.tags();
    let prices: MapView<'_, BTreeMap<String, u32>, Flatbuf> = catalog.prices();
    assert_eq!((tags.at_index(0), prices.get("zest")), ("x", Some(1)));
}

#[test]
fn bytes_nobody_validated() {
    let value = catalog();
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);
    // `new` is the format's own validator.
    for changed in corruptions(&pssz) {
        assert_eq!(View::<Catalog, Pssz>::new(&changed).is_ok(), psio::PsszView::<Catalog>::new(&changed).is_ok());
    }
    for changed in corruptions(&flatbuf) {
        assert_eq!(
            View::<Catalog, Flatbuf>::new(&changed).is_ok(),
            psio::flatbuf::FbView::<Catalog>::new(&changed).is_ok()
        );
    }
    // A trusted PSSZ view of them reads or panics.
    let outcomes = survives(&pssz, |bytes| {
        let _ = read(View::<Catalog, Pssz>::trusted(bytes));
    });
    assert!(outcomes[0] > 0 && outcomes[1] > 0, "some corruptions read, some panic: {outcomes:?}");
    // A trusted FlatBuffers view never panics.
    for changed in corruptions(&flatbuf) {
        let _ = read(View::<Catalog, Flatbuf>::trusted(&changed));
    }
}

// ── A record that holds itself ──────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Tree {
    label: String,
    children: Vec<Tree>,
}

fn tree<F: Reads<Tree>>(node: View<'_, Tree, F>) -> Tree {
    Tree { label: node.label().to_owned(), children: node.children().into_iter().map(tree).collect() }
}

fn depth<F: Reads<Tree>>(node: View<'_, Tree, F>) -> usize {
    1 + node.children().iter().map(depth).max().unwrap_or(0)
}

// Two records that hold each other.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Folder {
    name: String,
    entries: Vec<Entry>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Entry {
    size: u32,
    folder: Option<Folder>,
}

fn sizes<F: Reads<Folder>>(folder: View<'_, Folder, F>) -> u32 {
    folder.entries().iter().map(|entry| entry.size() + entry.folder().map(|folder| sizes(folder)).unwrap_or(0)).sum()
}

#[test]
fn records_that_hold_themselves() {
    let leaf = |label: &str| Tree { label: label.into(), children: vec![] };
    let value = Tree {
        label: "root".into(),
        children: vec![leaf("a"), Tree { label: "b".into(), children: vec![leaf("c"), leaf("d")] }],
    };
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);
    assert_eq!(tree(View::<Tree, Pssz>::new(&pssz).unwrap()), value);
    assert_eq!(tree(View::<Tree, Flatbuf>::new(&flatbuf).unwrap()), value);
    assert_eq!(depth(View::<Tree, Pssz>::new(&pssz).unwrap()), 3);
    assert_eq!(depth(View::<Tree, Flatbuf>::new(&flatbuf).unwrap()), 3);

    let value = Folder {
        name: "top".into(),
        entries: vec![
            Entry { size: 1, folder: None },
            Entry { size: 2, folder: Some(Folder { name: "in".into(), entries: vec![Entry { size: 40, folder: None }] }) },
        ],
    };
    assert_eq!(sizes(View::<Folder, Pssz>::new(&psio::to_pssz(&value)).unwrap()), 43);
    assert_eq!(sizes(View::<Folder, Flatbuf>::new(&psio::to_flatbuf(&value)).unwrap()), 43);
}

// ── What only PSSZ has ──────────────────────────────────────────────────────

/// A tuple struct: its members are `field_0`, `field_1`, as in `PsszView`.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
struct Measure(u32, String);

/// A transparent type: the bytes of its one member.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
#[psio(transparent)]
struct Batch {
    items: Vec<Item>,
}

/// An enum whose alternatives hold other things than one record.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
enum Token {
    End,
    Number(u32),
    Word(String),
    Pair(u32, String),
    Thing(Item),
    Many(Vec<Item>),
    Tint(Color),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
struct Extra {
    pair: (u32, String),
    triple: (bool, Item, Vec<u16>),
    boxed: Box<Item>,
    bounded: BoundedList<u32, 4>,
    short: BoundedString<16>,
    by_code: BoundedMap<u32, String, 4>,
    words: BoundedSet<String, 4>,
    big: Uint256,
    deep: Vec<Option<Vec<Item>>>,
    grid: [[u8; 2]; 2],
    measure: Measure,
    batch: Batch,
    tokens: Vec<Token>,
    #[psio(max = 4)]
    limited: BTreeMap<u32, Item>,
}

fn extra() -> Extra {
    Extra {
        pair: (5, "five".into()),
        triple: (true, Item { product: "t".into(), qty: 1 }, vec![1, 2, 3]),
        boxed: Box::new(Item { product: "boxed".into(), qty: 9 }),
        bounded: BoundedList(vec![4, 5, 6]),
        short: BoundedString("short".into()),
        by_code: BoundedMap([(1, "one".to_owned()), (2, "two".to_owned())].into()),
        words: BoundedSet(["w".to_owned(), "v".to_owned()].into()),
        big: Uint256 { limb: [1, 2, 3, 4] },
        deep: vec![Some(vec![Item { product: "d".into(), qty: 2 }]), None, Some(vec![])],
        grid: [[1, 2], [3, 4]],
        measure: Measure(12, "m".into()),
        batch: Batch { items: vec![Item { product: "b".into(), qty: 6 }] },
        tokens: vec![
            Token::End,
            Token::Number(8),
            Token::Word("w".into()),
            Token::Pair(1, "p".into()),
            Token::Thing(Item { product: "i".into(), qty: 4 }),
            Token::Many(vec![Item { product: "m".into(), qty: 5 }]),
            Token::Tint(Color::Red),
        ],
        limited: [(3, Item { product: "l".into(), qty: 7 })].into(),
    }
}

fn token<F: Reads<Token>>(token: View<'_, Token, F>) -> Token {
    match token {
        TokenView::End => Token::End,
        TokenView::Number(n) => Token::Number(n),
        TokenView::Word(word) => Token::Word(word.to_owned()),
        TokenView::Pair(n, word) => Token::Pair(n, word.to_owned()),
        TokenView::Thing(thing) => Token::Thing(item(thing)),
        TokenView::Many(many) => Token::Many(many.into_iter().map(item).collect()),
        TokenView::Tint(color) => Token::Tint(color),
    }
}

fn read_extra<F: Reads<Extra>>(extra: View<'_, Extra, F>) -> Extra {
    let (n, word) = extra.pair();
    let (flag, thing, numbers) = extra.triple();
    let grid = extra.grid();
    let row = |i| {
        let row = grid.at_index(i);
        [row.at_index(0), row.at_index(1)]
    };
    let measure = extra.measure();
    Extra {
        pair: (n, word.to_owned()),
        triple: (flag, item(thing), numbers.iter().collect()),
        boxed: Box::new(item(extra.boxed())),
        bounded: BoundedList(extra.bounded().iter().collect()),
        short: BoundedString(extra.short().to_owned()),
        by_code: BoundedMap(extra.by_code().iter().map(|(code, name)| (code, name.to_owned())).collect()),
        words: BoundedSet(extra.words().iter().map(str::to_owned).collect()),
        big: extra.big(),
        deep: extra.deep().iter().map(|items| items.map(|items| items.into_iter().map(item).collect())).collect(),
        grid: [row(0), row(1)],
        measure: Measure(measure.field_0(), measure.field_1().to_owned()),
        batch: Batch { items: extra.batch().items().into_iter().map(item).collect() },
        tokens: extra.tokens().into_iter().map(token).collect(),
        limited: extra.limited().iter().map(|(id, value)| (id, item(value))).collect(),
    }
}

#[test]
fn what_only_pssz_has() {
    let value = extra();
    let bytes = psio::to_pssz(&value);
    let view = View::<Extra, Pssz>::new(&bytes).unwrap();
    assert_eq!(read_extra(view), value);
    assert_eq!(view.by_code().get(2), Some("two"));
    assert!(view.words().contains("v"));
    assert_eq!(view.limited().get(3).map(|item| item.qty()), Some(7));
    let limited: MapView<'_, BoundedMap<u32, Item, 4>, Pssz> = view.limited();
    assert_eq!(limited.len(), 1);

    for changed in corruptions(&bytes) {
        assert_eq!(View::<Extra, Pssz>::new(&changed).is_ok(), psio::PsszView::<Extra>::new(&changed).is_ok());
    }
    let outcomes = survives(&bytes, |bytes| {
        let _ = read_extra(View::<Extra, Pssz>::trusted(bytes));
    });
    assert!(outcomes[0] > 0 && outcomes[1] > 0, "some corruptions read, some panic: {outcomes:?}");
}

// ── The root of a buffer that is not a record ───────────────────────────────

#[test]
fn any_value_is_read_from_the_root() {
    let tokens = vec![Token::Number(1), Token::Word("two".into())];
    let bytes = psio::to_pssz(&tokens);
    let list = view::read::<Vec<Token>, Pssz>(&bytes).unwrap();
    assert_eq!(list.into_iter().map(token).collect::<Vec<_>>(), tokens);
    assert!(view::read::<Vec<Token>, Pssz>(&[0xff, 0xff, 0xff]).is_err());

    let one = Token::Thing(Item { product: "x".into(), qty: 1 });
    let bytes = psio::to_pssz(&one);
    assert_eq!(token(view::read::<Token, Pssz>(&bytes).unwrap()), one);
    assert_eq!(token(view::read_trusted::<Token, Pssz>(&bytes)), one);
    let mut unknown = bytes.clone();
    unknown[0] = 200;
    assert!(view::read::<Token, Pssz>(&unknown).is_err());
    assert!(panics(move || {
        let _ = view::read_trusted::<Token, Pssz>(&unknown);
    }));

    let map: BTreeMap<String, Vec<u32>> = [("a".to_owned(), vec![1, 2]), ("b".to_owned(), vec![])].into();
    let bytes = psio::to_pssz(&map);
    let read = view::read::<BTreeMap<String, Vec<u32>>, Pssz>(&bytes).unwrap();
    assert_eq!(read.get("a").map(|list| list.iter().sum::<u32>()), Some(3));
    assert_eq!(view::read::<String, Pssz>(&psio::to_pssz(&"text".to_owned())).unwrap(), "text");

    // A record is read from the root in both formats: `View::new` is this.
    let value = catalog();
    let flatbuf = psio::to_flatbuf(&value);
    assert_eq!(read_catalog(view::read::<Catalog, Flatbuf>(&flatbuf).unwrap()), value);
    assert_eq!(read_catalog(view::read::<Catalog, Pssz>(&psio::to_pssz(&value)).unwrap()), value);
}

fn read_catalog<F: Reads<Catalog>>(catalog: View<'_, Catalog, F>) -> Catalog {
    read(catalog)
}

// ── Every PSSZ format tag ───────────────────────────────────────────────────

#[test]
fn a_record_is_viewed_under_every_pssz_tag() {
    let value = Tree { label: "r".into(), children: vec![Tree { label: "c".into(), children: vec![] }] };
    let auto = psio::to_pssz(&value);
    assert_eq!(tree(View::<Tree, Pssz>::new(&auto).unwrap()), value);
    assert_eq!(tree(View::<Tree, PsszUnder<1000>>::new(&auto).unwrap()), value);
    // The fixed-width profiles: other bytes, the same functions.
    let narrow = psio::pssz::encode::<Pssz8, _>(&value);
    let wide = psio::pssz::encode::<Pssz32, _>(&value);
    assert_ne!(narrow, wide);
    assert_eq!(tree(View::<Tree, Pssz8>::new(&narrow).unwrap()), value);
    assert_eq!(tree(View::<Tree, Pssz16>::new(&psio::pssz::encode::<Pssz16, _>(&value)).unwrap()), value);
    assert_eq!(tree(View::<Tree, Pssz32>::new(&wide).unwrap()), value);
    assert!(View::<Tree, Pssz8>::new(&wide).is_err());

    let value = extra();
    let bytes = psio::pssz::encode::<Pssz32, _>(&value);
    assert_eq!(read_extra(View::<Extra, Pssz32>::new(&bytes).unwrap()), value);
}

// ── A member's type under another name ──────────────────────────────────────

mod elsewhere {
    pub(super) use super::Item as Thing;
    pub type Count = u32;
}
use elsewhere::{Count, Thing};

/// `#[derive(View)]` never asks what kind of type a member is: an alias, a
/// renamed import and a path are the type they name.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Aliased {
    /// A value under another name.
    count: Count,
    /// A record under another name.
    thing: Thing,
    more: Vec<elsewhere::Thing>,
}

fn aliased<F: Reads<Aliased>>(view: View<'_, Aliased, F>) -> (u32, String, u32) {
    (view.count(), view.thing().product().to_owned(), view.more().iter().map(|thing| thing.qty()).sum())
}

#[test]
fn a_member_whose_type_has_another_name() {
    let value = Aliased {
        count: 3,
        thing: Item { product: "aliased".into(), qty: 1 },
        more: vec![Item { product: "a".into(), qty: 10 }, Item { product: "b".into(), qty: 5 }],
    };
    let expected = (3, "aliased".to_owned(), 15);
    assert_eq!(aliased(View::<Aliased, Pssz>::new(&psio::to_pssz(&value)).unwrap()), expected);
    assert_eq!(aliased(View::<Aliased, Flatbuf>::new(&psio::to_flatbuf(&value)).unwrap()), expected);
}
