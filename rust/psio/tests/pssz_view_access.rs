//! Views: every accessor is O(1) and safe on any bytes.
//!
//! An accessor finds its field or element from the header or the first
//! offset and its own slot; the whole frame is checked by `PsszView::new`,
//! once, not by each access. On bytes nobody validated a `try_*` accessor
//! returns an error or a view (never a panic), a plain one may panic, and
//! neither reads outside the buffer.

use psio::pssz::{BoundedList, PsszView};
use psio::{PsszPack, PsszUnpack, PsszView as DeriveView};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, DeriveView)]
struct Item {
    product: String,
    qty: u32,
    price: f64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, DeriveView)]
#[psio(definition_will_not_change)]
struct Point {
    x: i32,
    y: i32,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, DeriveView)]
struct Order {
    id: u64,
    name: String,
    items: Vec<Item>,
    points: Vec<Point>,
    tags: BoundedList<String, 4>,
    origin: Point,
    grid: [Point; 2],
    note: Option<String>,
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
        tags: BoundedList(vec!["a".into(), "bc".into()]),
        origin: Point { x: 7, y: 8 },
        grid: [Point { x: 3, y: 4 }, Point { x: 5, y: 6 }],
        note: Some("gift-wrap please".into()),
    }
}

/// Everything a view of an order gives, through the fallible accessors.
fn read(view: PsszView<'_, Order>) -> psio::pssz::PsszResult<Order> {
    let items = view.try_items()?;
    let points = view.try_points()?;
    let tags = view.try_tags()?;
    let grid = view.try_grid()?;
    let origin = view.try_origin()?;
    let note = view.try_note()?;
    let item = |index| -> psio::pssz::PsszResult<Item> {
        let item = items.try_get(index)?;
        Ok(Item {
            product: item.try_product()?.view()?.to_owned(),
            qty: number(item.try_qty()?.bytes())? as u32,
            price: f64::from_bits(number(item.try_price()?.bytes())?),
        })
    };
    let point = |view: PsszView<'_, Point>| -> psio::pssz::PsszResult<Point> {
        Ok(Point { x: number(view.try_x()?.bytes())? as i32, y: number(view.try_y()?.bytes())? as i32 })
    };
    Ok(Order {
        id: number(view.try_id()?.bytes())?,
        name: view.try_name()?.view()?.to_owned(),
        items: (0..items.try_len()?).map(item).collect::<Result<_, _>>()?,
        points: (0..points.try_len()?).map(|index| point(points.try_get(index)?)).collect::<Result<_, _>>()?,
        tags: BoundedList(
            (0..tags.try_len()?)
                .map(|index| Ok(tags.try_get(index)?.view()?.to_owned()))
                .collect::<psio::pssz::PsszResult<_>>()?,
        ),
        origin: point(origin)?,
        grid: [point(grid.try_get(0)?)?, point(grid.try_get(1)?)?],
        note: if note.try_has_value()? { Some(note.unwrap().view()?.to_owned()) } else { None },
    })
}
/// A little-endian number of at most eight bytes, without the panicking
/// `get` of a view of unvalidated bytes.
fn number(bytes: &[u8]) -> psio::pssz::PsszResult<u64> {
    if bytes.len() > 8 || bytes.is_empty() {
        return Err(psio::pssz::PsszError::new("test: not a number"));
    }
    let mut raw = [0u8; 8];
    raw[..bytes.len()].copy_from_slice(bytes);
    let value = u64::from_le_bytes(raw);
    // Sign-extend what was narrower.
    let shift = 64 - 8 * bytes.len() as u32;
    Ok((((value << shift) as i64) >> shift) as u64)
}

#[test]
fn a_view_reads_what_was_written() {
    let value = order();
    let bytes = psio::to_pssz(&value);
    let view = PsszView::<Order>::new(&bytes).unwrap();
    assert_eq!(read(view).unwrap(), value);
    assert_eq!(read(PsszView::trusted(&bytes)).unwrap(), value);

    // The plain accessors and the iterators.
    assert_eq!(view.id().get(), value.id);
    assert_eq!(view.name().view().unwrap(), value.name);
    assert_eq!(view.items().len(), 3);
    let products: Vec<&str> = view.items().iter().map(|item| item.product().view().unwrap()).collect();
    assert_eq!(products, ["Widget", "", "Gizmo-Mk.II"]);
    assert_eq!(view.items().iter().len(), 3);
    assert_eq!(view.items().iter().map(|item| item.qty().get()).sum::<u32>(), 8);
    assert_eq!(view.points().iter().map(|point| point.x().get() + point.y().get()).sum::<i32>(), 0);
    let tags: Vec<&str> = view.tags().iter().map(|tag| tag.view().unwrap()).collect();
    assert_eq!(tags, ["a", "bc"]);
    assert_eq!(view.grid().get(1).y().get(), 6);
    assert_eq!(view.note().get().unwrap().view().unwrap(), "gift-wrap please");
    // An empty list.
    let empty = psio::to_pssz(&Order { items: vec![], points: vec![], ..order() });
    let view = PsszView::<Order>::new(&empty).unwrap();
    assert_eq!(view.items().iter().count(), 0);
    assert!(view.items().is_empty() && view.points().is_empty());
    assert!(view.items().try_get(0).is_err());
}

/// An accessor looks at the slots it needs and no others: a list whose
/// table is damaged further on still gives its length and its sound
/// elements (the validator is what refuses the buffer).
#[test]
fn an_accessor_reads_its_own_slots_only() {
    let list: Vec<String> = (0..1000).map(|index| format!("element {index}")).collect();
    let mut bytes = psio::to_pssz(&list);
    // The offset of element 500 now points beyond the frame.
    bytes[500 * 4..500 * 4 + 4].copy_from_slice(&u32::MAX.to_le_bytes());
    assert!(PsszView::<Vec<String>>::new(&bytes).is_err());
    let view = PsszView::<Vec<String>>::trusted(&bytes);
    assert_eq!(view.try_len().unwrap(), 1000);
    assert_eq!(view.try_get(3).unwrap().view().unwrap(), "element 3");
    assert_eq!(view.try_get(999).unwrap().view().unwrap(), "element 999");
    // The two elements whose spans the damaged offset bounds.
    assert!(view.try_get(499).is_err());
    assert!(view.try_get(500).is_err());
    assert!(view.try_get(1000).is_err());
}

#[test]
fn a_string_view_checks_its_text() {
    for text in ["", "ascii only", "tschüß", "日本語", "a\u{0}b"] {
        let bytes = psio::to_pssz(text);
        assert_eq!(PsszView::<String>::trusted(&bytes).view().unwrap(), text);
        assert_eq!(psio::from_pssz::<String>(&bytes).unwrap(), text);
        assert_eq!(psio::from_pssz_trusted::<String>(&bytes), text);
        psio::validate_pssz::<String>(&bytes).unwrap();
    }
    // Not UTF-8: a lone continuation byte, a truncated sequence, an
    // overlong form, a surrogate; alone and behind ASCII.
    for bad in [&[0x80u8][..], &[0xc3], &[0xc0, 0xaf], &[0xed, 0xa0, 0x80], &[0xff]] {
        for prefix in ["", "plain ascii text of some length "] {
            let bytes: Vec<u8> = prefix.bytes().chain(bad.iter().copied()).collect();
            assert!(PsszView::<String>::trusted(&bytes).view().is_err());
            assert!(psio::from_pssz::<String>(&bytes).is_err());
            assert!(psio::validate_pssz::<String>(&bytes).is_err());
            assert!(std::panic::catch_unwind(|| psio::from_pssz_trusted::<String>(&bytes)).is_err());
        }
    }
}

/// On bytes nobody validated the fallible accessors return, whatever the
/// bytes are; and when the validator accepts a buffer, the view reads the
/// value the decoder reads.
#[test]
fn the_fallible_accessors_never_panic() {
    let valid = psio::to_pssz(&order());
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    let mut accepted = 0;
    let mut check = |bytes: &[u8]| {
        let through_view = read(PsszView::trusted(bytes));
        if let Ok(value) = psio::from_pssz::<Order>(bytes) {
            assert_eq!(through_view.as_ref(), Ok(&value));
            accepted += 1;
        }
    };
    for end in 0..=valid.len() {
        check(&valid[..end]);
    }
    let rounds = if cfg!(miri) { 60 } else { 6000 };
    for _ in 0..rounds {
        let mut bytes = valid.clone();
        for _ in 0..1 + next() % 3 {
            let at = (next() % bytes.len() as u64) as usize;
            bytes[at] = next() as u8;
        }
        check(&bytes);
    }
    for _ in 0..rounds / 3 {
        let length = (next() % 80) as usize;
        let bytes: Vec<u8> = (0..length).map(|_| next() as u8).collect();
        check(&bytes);
    }
    assert!(accepted > 1);
}
