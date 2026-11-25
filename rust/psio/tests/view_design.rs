//! The properties the design of `psio::view` is held to
//! (gofractally/psio#13): each was a defect of the first prototype.

use psio::flatbuf::Flatbuf;
use psio::pssz::{Pssz, Pssz16, Pssz32, Pssz8, PsszFormat, PsszResult, PsszUnder};
use psio::view::pssz::PsszFamily;
use psio::view::{self, Nil, Reads, View, ViewIn, ViewShape, ViewTag};
use psio::{FbPack, FbUnpack, FbView, PsszPack, PsszUnpack, PsszView};

// ── A record that holds itself, and two that hold each other ────────────────

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Node {
    id: u32,
    children: Vec<Node>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
struct Chain {
    id: u32,
    next: Option<Box<Chain>>,
}

fn depth<F: Reads<Node>>(node: View<'_, Node, F>) -> u32 {
    1 + node.children().iter().map(|child| depth(child)).max().unwrap_or(0)
}

fn length<F: Reads<Chain>>(link: View<'_, Chain, F>) -> u32 {
    1 + link.next().map(|next| length(next)).unwrap_or(0)
}

#[test]
fn recursive_records() {
    let leaf = |id| Node { id, children: vec![] };
    let value = Node { id: 1, children: vec![leaf(2), Node { id: 3, children: vec![leaf(4)] }] };
    assert_eq!(depth(View::<Node, Pssz>::new(&psio::to_pssz(&value)).unwrap()), 3);
    assert_eq!(depth(View::<Node, Flatbuf>::new(&psio::to_flatbuf(&value)).unwrap()), 3);
    let value = Chain { id: 1, next: Some(Box::new(Chain { id: 2, next: None })) };
    assert_eq!(length(View::<Chain, Pssz>::new(&psio::to_pssz(&value)).unwrap()), 2);
}

// ── A view's lifetime shrinks, as a reference's does ────────────────────────

fn shorter<'s, F: Reads<Node>>(node: View<'s, Node, F>, _with: &'s u8) -> u32 {
    node.id()
}

fn longer<'long>(node: View<'long, Node, Pssz>) -> u32 {
    let local = 0u8;
    // Compiles only when `View` is covariant in its lifetime.
    shorter(node, &local)
}

fn list_shrinks<'long, 's>(list: view::ListView<'long, Vec<Node>, Pssz>, _with: &'s u8) -> view::ListView<'s, Vec<Node>, Pssz>
where
    'long: 's,
{
    list
}

#[test]
fn a_view_is_covariant_in_its_lifetime() {
    let bytes = psio::to_pssz(&Node { id: 9, children: vec![] });
    let node = View::<Node, Pssz>::new(&bytes).unwrap();
    assert_eq!(longer(node), 9);
    let local = 0u8;
    assert_eq!(list_shrinks(node.children(), &local).len(), 0);
}

// ── A member's kind is not read off how its type is written ─────────────────

mod inner {
    use psio::{PsszPack, PsszUnpack, PsszView};
    #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
    pub struct Item {
        pub product: String,
        pub qty: u32,
    }
}
use inner::Item as LineItem;

type Qty = u32;

/// A leaf of the user's own: a value that reads as itself. It says what it
/// reads as, and how PSSZ reads it.
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView)]
#[psio(transparent)]
struct Cents {
    amount: u64,
}
impl ViewShape for Cents {
    type Members = ();
    type Shape<'a, F: ViewTag, R: view::FieldList<F, ()>> = Cents;
}
impl<F: PsszFormat> ViewIn<PsszFamily, F> for Cents {
    type Fields = Nil<F, F>;
    fn validate(bytes: &[u8]) -> PsszResult<()> {
        psio::PsszView::<Cents, F>::new(bytes).map(|_| ())
    }
    fn trusted_root<'a>(bytes: &'a [u8]) -> Cents {
        Cents { amount: view::read_trusted::<u64, F>(bytes) }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
#[repr(u8)]
enum Level {
    Low = 0,
    High = 1,
}

/// A transparent newtype: the bytes of its one member.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
#[psio(transparent)]
struct Label {
    text: String,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
struct Order {
    qty: Qty,
    items: Vec<LineItem>,
    first: inner::Item,
    name: std::string::String,
    price: Cents,
    level: Level,
    label: Label,
    wide: u128,
    signed: i128,
    #[psio(skip)]
    cached: u32,
    after: u8,
}

fn summary<F: Reads<Order>>(order: View<'_, Order, F>) -> (u32, u32, String, u64, Level, String, u128, i128, u8) {
    (
        order.qty(),
        order.items().iter().map(|item| item.qty()).sum::<u32>() + order.first().qty(),
        order.name().to_owned(),
        order.price().amount,
        order.level(),
        order.label().text().to_owned(),
        order.wide(),
        order.signed(),
        order.after(),
    )
}

#[test]
fn members_of_every_kind_under_any_name() {
    let item = |qty| inner::Item { product: "p".into(), qty };
    let value = Order {
        qty: 4,
        items: vec![item(1), item(2)],
        first: item(10),
        name: "n".into(),
        price: Cents { amount: 199 },
        level: Level::High,
        label: Label { text: "l".into() },
        wide: u128::MAX - 1,
        signed: i128::MIN + 1,
        cached: 77,
        after: 5,
    };
    let expected = (4, 13, "n".to_owned(), 199, Level::High, "l".to_owned(), u128::MAX - 1, i128::MIN + 1, 5);
    let bytes = psio::to_pssz(&value);
    assert_eq!(summary(View::<Order, Pssz>::new(&bytes).unwrap()), expected);
    // The owned value: a skipped member is not on the wire.
    let unpacked = View::<Order, Pssz>::new(&bytes).unwrap().unpack();
    assert_eq!(unpacked, Order { cached: 0, ..value });
}

// ── Every PSSZ position ─────────────────────────────────────────────────────

#[test]
fn a_record_is_viewed_at_every_pssz_position() {
    let value = inner::Item { product: "nut".into(), qty: 3 };
    fn qty<F: Reads<inner::Item>>(item: View<'_, inner::Item, F>) -> u32 {
        item.qty()
    }
    assert_eq!(qty(View::<LineItem, Pssz>::new(&psio::to_pssz(&value)).unwrap()), 3);
    assert_eq!(qty(View::<LineItem, PsszUnder<255>>::new(&psio::to_pssz(&value)).unwrap()), 3);
    assert_eq!(qty(View::<LineItem, Pssz8>::new(&psio::pssz::encode::<Pssz8, _>(&value)).unwrap()), 3);
    assert_eq!(qty(View::<LineItem, Pssz16>::new(&psio::pssz::encode::<Pssz16, _>(&value)).unwrap()), 3);
    assert_eq!(qty(View::<LineItem, Pssz32>::new(&psio::pssz::encode::<Pssz32, _>(&value)).unwrap()), 3);
}

// ── Code generic over the format makes a view and uses `?` ──────────────────

fn parse<F: Reads<inner::Item>>(bytes: &[u8]) -> Result<u32, Box<dyn std::error::Error>> {
    Ok(View::<inner::Item, F>::new(bytes)?.qty())
}

#[test]
fn the_error_of_a_format_is_an_error() {
    let bytes = psio::to_pssz(&inner::Item { product: "nut".into(), qty: 3 });
    assert_eq!(parse::<Pssz>(&bytes).unwrap(), 3);
    assert!(parse::<Pssz>(&[1, 2, 3]).is_err());
}

// ── A FlatBuffers struct is not the root of a buffer ────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[psio(definition_will_not_change)]
#[view(pssz, flatbuf)]
struct Point {
    x: i32,
    y: i32,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Place {
    at: Point,
    id: u64,
}

#[test]
fn a_struct_read_as_the_root_reads_as_absent() {
    let bytes = psio::to_flatbuf(&Place { at: Point { x: 7, y: 8 }, id: 0x0102_0304_0506_0708 });
    assert_eq!(View::<Place, Flatbuf>::new(&bytes).unwrap().at().x(), 7);
    // `new` refuses; `trusted` does not read the root table as the struct:
    // it reads what an absent struct reads as.
    assert!(View::<Point, Flatbuf>::new(&bytes).is_err());
    let point = View::<Point, Flatbuf>::trusted(&bytes);
    assert_eq!((point.x(), point.y()), (0, 0));
}

// ── Views print ─────────────────────────────────────────────────────────────

#[test]
fn views_print_what_they_are_of() {
    let value = Node { id: 1, children: vec![Node { id: 2, children: vec![] }] };
    let bytes = psio::to_pssz(&value);
    let node = View::<Node, Pssz>::new(&bytes).unwrap();
    assert_eq!(format!("{node:?}"), format!("{value:?}"));
    assert_eq!(format!("{:?}", node.children()), format!("{:?}", value.children));
    assert_eq!(node.unpack(), value);
    assert_eq!(node.children().unpack(), value.children);
}
