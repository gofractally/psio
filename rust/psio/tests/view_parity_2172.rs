//! Parity with the fracpack views of psibase #2172 (`rust/fracpack/src/view.rs`
//! and the tests `test_view`, `test_array_view`, `test_option_view`,
//! `view_def_wont_change`, `test_view_with_lifetime`, `test_named_view`,
//! `test_single`, `test_private_view` in `rust/test_fracpack/tests/test.rs`):
//! each of those cases, through `psio::view` on PSSZ, and on FlatBuffers
//! where FlatBuffers can hold the value.
//!
//! What differs, by design:
//!
//! * A member reads as its natural type, so there is no `.unpack()` on a
//!   number or a string (`v.qty()` is the `u32`), and an optional is a real
//!   `Option`: its whole method set, `?`, `if let` and `match` come from the
//!   standard library, and there is no `transpose()` to call first.
//! * Elements are values, not references into the buffer, so a list has no
//!   `[]`: `at_index(i)` panics past the end as `[]` does, `get(i)` returns
//!   an `Option`.
//! * A tuple reads as a Rust tuple of what its elements read as.
//!
//! What is not matched is said where the case is.

use psio::flatbuf::Flatbuf;
use psio::pssz::Pssz;
use psio::view::{self, ListView, Reads, View};
use psio::{FbPack, FbUnpack, FbView, PsszPack, PsszUnpack, PsszView};

// ── test_view: values that are not records, at the root ─────────────────────
//
// FlatBuffers: the root of a buffer is a table, so none of these has a
// FlatBuffers form; they are read there as members of a record (below).

#[test]
fn test_view() {
    {
        let data = psio::to_pssz(&"asdf".to_string());
        let v = view::read::<String, Pssz>(&data).unwrap();
        assert_eq!(v.find('d'), Some(2));
    }
    {
        // `View<&str>`: a borrowed string has the bytes of an owned one.
        let data = psio::to_pssz(&"asdf".to_string());
        let v = view::read::<&str, Pssz>(&data).unwrap();
        assert_eq!(v.find('d'), Some(2));
    }
    {
        let data = psio::to_pssz(&vec![42u32, 7u32]);
        let v = view::read::<Vec<u32>, Pssz>(&data).unwrap();
        // (`v[0].unpack()` there.)
        assert_eq!(v.at_index(0), 42);
        assert_eq!(v.at_index(1), 7);
        let mut values = Vec::new();
        for i in v {
            values.push(i);
        }
        assert_eq!(values, vec![42, 7]);
        assert_eq!(v.iter().sum::<u32>(), 49);
        assert!(!v.is_empty());
        assert_eq!(v.len(), 2);
        // The owned list.
        assert_eq!(v.unpack(), vec![42, 7]);
    }
    {
        let data = psio::to_pssz(&vec!["asdf".to_string(), "".to_string()]);
        let v = view::read::<Vec<String>, Pssz>(&data).unwrap();
        assert_eq!(v.at_index(0).find('d'), Some(2));
        assert!(v.at_index(1).is_empty());
    }
    {
        let data = psio::to_pssz(&vec![Some("asdf".to_string()), Some("".to_string()), None]);
        let v = view::read::<Vec<Option<String>>, Pssz>(&data).unwrap();
        assert_eq!(v.at_index(0).unwrap().find('d'), Some(2));
        assert!(v.at_index(1).unwrap().is_empty());
        assert!(v.at_index(2).is_none());
    }
    {
        let data = psio::to_pssz(&Box::new("jkl;".to_string()));
        let v = view::read::<Box<String>, Pssz>(&data).unwrap();
        assert_eq!(v.find(';'), Some(3));
    }
    {
        let data = psio::to_pssz(&("asdf".to_string(), 7u32, Some(42u32), Some("jkl;".to_string())));
        let v = view::read::<(String, u32, Option<u32>, Option<String>), Pssz>(&data).unwrap();
        // (`v.get0().unpack()` … `v.get3().unpack()` there; `transpose()`
        // for the tuple of views. Here the view of a tuple is that tuple.)
        let (text, number, maybe, other) = v;
        assert_eq!(text, "asdf");
        assert_eq!(number, 7);
        assert_eq!(maybe, Some(42));
        assert_eq!(other.unwrap(), "jkl;");
        assert_eq!((v.0, v.1), ("asdf", 7));
    }
    {
        let value = InnerStruct {
            inner_u32: 1234,
            var: None,
            inner_option_u32: None,
            inner_option_str: Some("".to_string()),
            inner_option_vec_u16: None,
            inner_o_vec_o_u16: None,
        };
        let data = psio::to_pssz(&value);
        let v = View::<InnerStruct, Pssz>::new(&data).unwrap();
        assert_eq!(v.inner_u32(), 1234);
        assert!(v.var().is_none());
        assert_eq!(v.inner_option_u32(), None);
        assert_eq!(v.inner_option_str(), Some(""));
        assert!(v.inner_option_vec_u16().is_none());
        assert!(v.inner_o_vec_o_u16().is_none());
        // A view prints as the value it is of.
        assert_eq!(format!("{v:?}"), format!("{value:?}"));
        assert_eq!(v.unpack(), value);

        let value = InnerStruct {
            var: Some(Variant::ItemStr("s".into())),
            inner_option_vec_u16: Some(vec![1, 2]),
            inner_o_vec_o_u16: Some(vec![Some(3), None]),
            ..value
        };
        let data = psio::to_pssz(&value);
        let v = View::<InnerStruct, Pssz>::new(&data).unwrap();
        assert!(matches!(v.var(), Some(VariantView::ItemStr("s"))));
        assert_eq!(v.inner_option_vec_u16().map(|list| list.iter().sum::<u16>()), Some(3));
        assert_eq!(v.inner_o_vec_o_u16().unwrap().iter().collect::<Vec<_>>(), [Some(3), None]);
    }
}

/// `Variant` of the fracpack tests.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
enum Variant {
    ItemU32(u32),
    ItemStr(String),
}

/// `InnerStruct` of the fracpack tests.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
struct InnerStruct {
    inner_u32: u32,
    var: Option<Variant>,
    inner_option_u32: Option<u32>,
    inner_option_str: Option<String>,
    inner_option_vec_u16: Option<Vec<u16>>,
    inner_o_vec_o_u16: Option<Vec<Option<u16>>>,
}

// ── test_array_view ─────────────────────────────────────────────────────────

#[test]
fn test_array_view() {
    let data = psio::to_pssz(&[42u32, 7u32]);
    let v = view::read::<[u32; 2], Pssz>(&data).unwrap();
    assert_eq!(v.at_index(0), 42);
    assert_eq!(v.at_index(1), 7);
    let mut values = Vec::new();
    for i in v {
        values.push(i);
    }
    assert_eq!(values, vec![42, 7]);
    assert_eq!(v.iter().sum::<u32>(), 49);
    assert_eq!(v.map(|i| i + 1), [43, 8]);
    // (`transpose()` there: the array of the elements' views.)
    assert_eq!(v.to_array(), [42, 7]);
    assert_eq!(v.len(), 2);

    // An array of records: an array of views to index and to match on.
    let data = psio::to_pssz(&[Single(1), Single(2)]);
    let v = view::read::<[Single; 2], Pssz>(&data).unwrap();
    let [first, second] = v.to_array();
    assert_eq!((first.field_0(), second.field_0()), (1, 2));
}

// ── test_option_view ────────────────────────────────────────────────────────
//
// The optional is a real `Option`: every method below is the standard
// library's. (There, `OptionView` re-implements them and `transpose()`
// gives the `Option` of a view.)

#[test]
fn test_option_view() {
    let read = |value: Option<u32>| view::read::<Option<u32>, Pssz>(&psio::to_pssz(&value)).unwrap();
    let nested = |value: Option<Option<u32>>| view::read::<Option<Option<u32>>, Pssz>(&psio::to_pssz(&value)).unwrap();
    let vnone = read(None);
    let v1 = read(Some(1));
    let v2 = read(Some(2));
    let i5 = view::read::<u32, Pssz>(&psio::to_pssz(&5u32)).unwrap();
    let nested1 = nested(Some(Some(3)));
    let nested2 = nested(Some(None));
    let nested3 = nested(None);

    assert!(matches!(vnone, None));
    assert!(vnone.is_none());
    assert!(!vnone.is_some());
    if let Some(v) = v1 {
        assert_eq!(v, 1);
    } else {
        panic!("an optional with a value")
    }
    assert!(v1.is_some());
    assert!(!v1.is_none());

    assert_eq!(vnone.and(Some(5)), None);
    assert_eq!(vnone.and(v2), None);
    assert_eq!(v1.and(Some(5)), Some(5));
    assert_eq!(v1.and(v2), Some(2));
    assert_eq!(v1.and(vnone), None);

    assert_eq!(vnone.and_then(|i| Some(i + 10)), None);
    assert_eq!(v1.and_then(|i| Some(i + 10)), Some(11));
    assert_eq!(v1.and_then(|_| v2), Some(2));
    assert_eq!(v1.and_then(|_| vnone), None);

    assert_eq!(v1.expect("asdf"), 1);

    assert_eq!(v1.filter(|x| *x == 1), Some(1));
    assert_eq!(v2.filter(|x| *x == 1), None);
    assert_eq!(vnone.filter(|x| *x == 1), None);

    assert_eq!(nested1.flatten(), Some(3));
    assert_eq!(nested2.flatten(), None);
    assert_eq!(nested3.flatten(), None);
    assert_eq!((nested1, nested2, nested3), (Some(Some(3)), Some(None), None));

    {
        let mut out: u32 = 0;
        v1.inspect(|x| out = *x);
        assert_eq!(out, 1);
        vnone.inspect(|_| out = 42);
        assert_eq!(out, 1);
    }

    assert!(v1.is_none_or(|x| x == 1));
    assert!(!v2.is_none_or(|x| x == 1));
    assert!(vnone.is_none_or(|x| x == 1));

    assert!(v1.is_some_and(|x| x == 1));
    assert!(!v2.is_some_and(|x| x == 1));
    assert!(!vnone.is_some_and(|x| x == 1));

    assert_eq!(v1.map(|x| x + 10), Some(11));
    assert_eq!(vnone.map(|x| x + 10), None);
    assert_eq!(v1.map_or(5, |x| x + 10), 11);
    assert_eq!(vnone.map_or(5, |x| x + 10), 5);
    assert_eq!(v1.map_or_default(|x| x + 10), 11);
    assert_eq!(vnone.map_or_default(|x| x + 10), 0);
    assert_eq!(v1.map_or_else(|| 5, |x| x + 10), 11);
    assert_eq!(vnone.map_or_else(|| 5, |x| x + 10), 5);

    assert_eq!(v1.ok_or(5), Ok(1));
    assert_eq!(vnone.ok_or(5), Err(5));
    assert_eq!(v1.ok_or_else(|| 5), Ok(1));
    assert_eq!(vnone.ok_or_else(|| 5), Err(5));

    assert_eq!(v1.or(v2), Some(1));
    assert_eq!(vnone.or(v2), Some(2));
    assert_eq!(v1.or(vnone), Some(1));
    assert_eq!(vnone.or(vnone), None);
    assert_eq!(v1.or(Some(i5)), Some(1));
    assert_eq!(vnone.or(Some(i5)), Some(5));

    assert_eq!(v1.or_else(|| v2), Some(1));
    assert_eq!(vnone.or_else(|| v2), Some(2));
    assert_eq!(vnone.or_else(|| Some(i5)), Some(5));
    assert_eq!(vnone.or_else(|| None), None);

    assert_eq!(v1.unwrap(), 1);
    assert_eq!(v1.unwrap_or(i5), 1);
    assert_eq!(vnone.unwrap_or(i5), 5);
    assert_eq!(v1.unwrap_or_else(|| i5), 1);
    assert_eq!(vnone.unwrap_or_else(|| i5), 5);

    assert_eq!(v1.xor(v2), None);
    assert_eq!(v1.xor(vnone), Some(1));
    assert_eq!(vnone.xor(v1), Some(1));
    assert_eq!(vnone.xor(vnone), None);
    assert_eq!(v1.xor(Some(i5)), None);
    assert_eq!(vnone.xor(Some(i5)), Some(5));

    // An optional record is an `Option` of its view: the same methods.
    let data = psio::to_pssz(&Some(Single(9)));
    let v = view::read::<Option<Single>, Pssz>(&data).unwrap();
    assert_eq!(v.map(|single| single.field_0()), Some(9));
}

// ── view_def_wont_change ────────────────────────────────────────────────────

/// `DefWontChangeInnerStruct` of the fracpack tests.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
#[psio(definition_will_not_change)]
struct DefWontChangeInnerStruct {
    field_bool: bool,
    field_u32: u32,
    field_var: Variant,
    field_i16: i16,
    field_o_var: Option<Variant>,
    field_str: String,
    field_a_i16_3: [i16; 3],
    field_f32: f32,
    field_o_v_i8: Option<Vec<i8>>,
    field_a_s_2: [String; 2],
    field_f64: f64,
    field_o_str: Option<String>,
    field_v_u16: Vec<u16>,
    field_i32: i32,
}

fn variant<F: Reads<Variant>>(view: View<'_, Variant, F>) -> Variant {
    match view {
        VariantView::ItemU32(n) => Variant::ItemU32(n),
        VariantView::ItemStr(text) => Variant::ItemStr(text.to_owned()),
    }
}

#[test]
fn view_def_wont_change() {
    let value = DefWontChangeInnerStruct {
        field_bool: true,
        field_u32: 44,
        field_var: Variant::ItemStr("xyz".to_string()),
        field_i16: 55,
        field_o_var: Some(Variant::ItemU32(88)),
        field_str: "byebye".to_string(),
        field_a_i16_3: [77, 88, 99],
        field_f32: 6.4,
        field_o_v_i8: Some(vec![44, 55, 66]),
        field_a_s_2: ["aa".to_string(), "bb".to_string()],
        field_f64: 128.128,
        field_o_str: Some("lo".to_string()),
        field_v_u16: vec![3, 2, 1],
        field_i32: -999,
    };
    let data = psio::to_pssz(&value);
    let v = View::<DefWontChangeInnerStruct, Pssz>::new(&data).unwrap();
    assert_eq!(v.field_bool(), value.field_bool);
    assert_eq!(v.field_u32(), value.field_u32);
    assert_eq!(variant(v.field_var()), value.field_var);
    assert_eq!(v.field_i16(), value.field_i16);
    assert_eq!(v.field_o_var().map(variant), value.field_o_var);
    assert_eq!(v.field_str(), value.field_str);
    assert_eq!(v.field_a_i16_3().to_array(), value.field_a_i16_3);
    assert_eq!(v.field_f32(), value.field_f32);
    assert_eq!(v.field_o_v_i8().map(|list| list.unpack()), value.field_o_v_i8);
    assert_eq!(v.field_a_s_2().to_array(), ["aa", "bb"]);
    assert_eq!(v.field_f64(), value.field_f64);
    assert_eq!(v.field_o_str(), value.field_o_str.as_deref());
    assert_eq!(v.field_v_u16().unpack(), value.field_v_u16);
    assert_eq!(v.field_i32(), value.field_i32);
    // `.unpack()` of the whole record, and of a list and an array member.
    assert_eq!(v.unpack(), value);
    assert_eq!(v.field_a_i16_3().unpack(), value.field_a_i16_3);
    assert_eq!(v.field_a_s_2().unpack(), value.field_a_s_2);
}

// ── test_view_with_lifetime ─────────────────────────────────────────────────

/// A record that borrows. PSSZ derives its validators with its decoders
/// (`PsszUnpack`), and a record that borrows has no decoder, so there is no
/// validator for `new` to run: the record says so, and a view of it is made
/// with `trusted`. (The fracpack `Unpack<'a>` borrows, so there `new`
/// validates such a record. Not matched.)
#[derive(PsszPack, PsszView, psio::View)]
#[view(no_validator)]
pub struct ViewWithLifetime<'a> {
    x: &'a str,
    raw: &'a [u8],
}

#[test]
fn test_view_with_lifetime() {
    let value = ViewWithLifetime { x: "apple", raw: &[1, 2, 3] };
    let data = psio::to_pssz(&value);
    assert!(View::<ViewWithLifetime, Pssz>::new(&data).is_err());
    let v = View::<ViewWithLifetime, Pssz>::trusted(&data);
    assert_eq!(v.x(), value.x);
    // A `&[u8]` member reads as the bytes.
    assert_eq!(v.raw(), value.raw);
}

// ── test_named_view, test_single, test_private_view ─────────────────────────

/// `#[fracpack(view_name = "NamedViewType")]` there.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
#[view(name = "NamedViewType")]
pub struct NamedView {
    x: u32,
}

#[test]
fn test_named_view() {
    let value = NamedView { x: 42 };
    let data = psio::to_pssz(&value);
    let v = <NamedViewType<Pssz>>::new(&data).unwrap();
    assert_eq!(v.x(), value.x);
    let v: View<'_, NamedView, Pssz> = v;
    assert_eq!(v.x(), 42);
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
pub struct Single(u32);

#[test]
fn test_single() {
    let value = Single(42);
    let data = psio::to_pssz(&value);
    let v = View::<Single, Pssz>::new(&data).unwrap();
    // (`get0()` there; `field_0()` is the name `PsszView` gives it.)
    assert_eq!(v.field_0(), value.0);
}

mod private {
    use psio::{PsszPack, PsszUnpack, PsszView};
    /// An accessor is as visible as its member: `x` is read in this module
    /// only, `shown` wherever the record is.
    #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
    pub struct Private {
        x: u32,
        pub shown: u32,
    }
    pub fn make() -> Private {
        Private { x: 42, shown: 7 }
    }
    pub fn x(view: psio::view::View<'_, Private, psio::pssz::Pssz>) -> u32 {
        view.x()
    }
}

#[test]
fn test_private_view() {
    let value = private::make();
    let data = psio::to_pssz(&value);
    let v = View::<private::Private, Pssz>::new(&data).unwrap();
    assert_eq!(private::x(v), 42);
    assert_eq!(v.shown(), 7);
    // `v.x()` does not compile here: the method `x` is private.
}

// ── What holds one value ────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, psio::View)]
struct Boxed {
    name: Box<String>,
    one: Box<Single>,
    many: Vec<Box<Single>>,
    maybe: Option<Box<String>>,
}

#[test]
fn test_box_members() {
    let value = Boxed {
        name: Box::new("n".into()),
        one: Box::new(Single(1)),
        many: vec![Box::new(Single(2)), Box::new(Single(3))],
        maybe: Some(Box::new("m".into())),
    };
    let data = psio::to_pssz(&value);
    let v = View::<Boxed, Pssz>::new(&data).unwrap();
    assert_eq!(v.name(), "n");
    assert_eq!(v.one().field_0(), 1);
    assert_eq!(v.many().iter().map(|single| single.field_0()).sum::<u32>(), 5);
    assert_eq!(v.maybe(), Some("m"));
    // `Rc<T>`, `Arc<T>` and `RefCell<T>` read as `T` reads too (`ViewShape`
    // and the PSSZ reader are there), but PSSZ has no encoder for them, so
    // a record cannot hold one and there is nothing to make a view of.
}

// ── The same records in FlatBuffers ─────────────────────────────────────────
//
// What FlatBuffers holds of the above: records with scalars, strings,
// lists, optionals, arrays of scalars in a struct, nested records and
// unions of tables. It has no value that is not a record at the root, no
// tuple, no `Box`, no list of optionals, no optional of an optional, no
// tuple struct, and a union alternative is a table (not a number or a
// string, as `Variant` holds).

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[psio(definition_will_not_change)]
#[view(pssz, flatbuf)]
struct Fixed {
    a: [i16; 3],
    n: u32,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Leaf {
    text: String,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
enum Either {
    Neither,
    Leaf(Leaf),
    Fixed(Wrapped),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf)]
struct Wrapped {
    fixed: Fixed,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, FbPack, FbUnpack, FbView, psio::View)]
#[view(pssz, flatbuf, name = "SharedReader")]
struct Shared {
    field_bool: bool,
    field_u32: u32,
    field_i16: i16,
    field_str: String,
    field_f32: f32,
    field_o_v_i8: Option<Vec<i8>>,
    field_f64: f64,
    field_o_str: Option<String>,
    field_o_u32: Option<u32>,
    field_v_u16: Vec<u16>,
    field_v_str: Vec<String>,
    field_i32: i32,
    fixed: Fixed,
    leaf: Leaf,
    o_leaf: Option<Leaf>,
    leaves: Vec<Leaf>,
    either: Either,
    pub shown: u8,
}

/// One function, for both formats.
fn shared<F: Reads<Shared>>(v: View<'_, Shared, F>, value: &Shared) {
    assert_eq!(v.field_bool(), value.field_bool);
    assert_eq!(v.field_u32(), value.field_u32);
    assert_eq!(v.field_i16(), value.field_i16);
    assert_eq!(v.field_str(), value.field_str);
    assert_eq!(v.field_str().find('y'), Some(1));
    assert_eq!(v.field_f32(), value.field_f32);
    assert_eq!(v.field_o_v_i8().map(|list| list.iter().collect::<Vec<_>>()), value.field_o_v_i8);
    assert_eq!(v.field_f64(), value.field_f64);
    assert_eq!(v.field_o_str(), value.field_o_str.as_deref());
    assert_eq!(v.field_o_u32().map_or(5, |x| x + 10), 98);
    assert!(v.field_o_u32().is_some_and(|x| x == 88));
    let list = v.field_v_u16();
    assert_eq!((list.len(), list.is_empty(), list.at_index(0), list.get(3)), (3, false, 3, None));
    assert_eq!(list.iter().sum::<u16>(), 6);
    let mut seen = Vec::new();
    for text in v.field_v_str() {
        seen.push(text);
    }
    assert_eq!(seen, ["aa", "bb"]);
    assert_eq!(v.field_i32(), value.field_i32);
    let array = v.fixed().a();
    assert_eq!(array.to_array(), value.fixed.a);
    assert_eq!(array.map(|i| i + 1), [78, 89, 100]);
    assert_eq!(v.fixed().n(), 9);
    assert_eq!(v.leaf().text(), "leaf");
    assert_eq!(v.o_leaf().map(|leaf| leaf.text()), Some("maybe"));
    assert_eq!(v.leaves().iter().map(|leaf| leaf.text().len()).sum::<usize>(), 3);
    match v.either() {
        EitherView::Fixed(wrapped) => assert_eq!(wrapped.fixed().n(), 4),
        _ => panic!("the alternative it holds"),
    }
    assert_eq!(v.shown(), 1);
}

#[test]
fn test_view_in_flatbuffers_too() {
    let value = Shared {
        field_bool: true,
        field_u32: 44,
        field_i16: 55,
        field_str: "byebye".to_string(),
        field_f32: 6.4,
        field_o_v_i8: Some(vec![44, 55, 66]),
        field_f64: 128.128,
        field_o_str: Some("lo".to_string()),
        field_o_u32: Some(88),
        field_v_u16: vec![3, 2, 1],
        field_v_str: vec!["aa".to_string(), "bb".to_string()],
        field_i32: -999,
        fixed: Fixed { a: [77, 88, 99], n: 9 },
        leaf: Leaf { text: "leaf".into() },
        o_leaf: Some(Leaf { text: "maybe".into() }),
        leaves: vec![Leaf { text: "a".into() }, Leaf { text: "bc".into() }],
        either: Either::Fixed(Wrapped { fixed: Fixed { a: [1, 2, 3], n: 4 } }),
        shown: 1,
    };
    let pssz = psio::to_pssz(&value);
    let flatbuf = psio::to_flatbuf(&value);
    shared(View::<Shared, Pssz>::new(&pssz).unwrap(), &value);
    shared(View::<Shared, Flatbuf>::new(&flatbuf).unwrap(), &value);
    // The named view, the owned value and the printed value, in both.
    let in_pssz = SharedReader::<Pssz>::new(&pssz).unwrap();
    let in_flatbuf = SharedReader::<Flatbuf>::new(&flatbuf).unwrap();
    assert_eq!(in_pssz.unpack(), value);
    assert_eq!(in_flatbuf.unpack(), value);
    assert_eq!(format!("{in_pssz:?}"), format!("{value:?}"));
    assert_eq!(format!("{in_flatbuf:?}"), format!("{value:?}"));
    let list: ListView<'_, Vec<u16>, Flatbuf> = in_flatbuf.field_v_u16();
    assert_eq!(list.unpack(), value.field_v_u16);
}
