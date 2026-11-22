//! Schema fidelity: what a consumer holding only the Schema IR derives must
//! be what the typed codec does (docs/json.md "Width fidelity", "Raw strings
//! and UTF-8 strings", "Explicit-width profiles", "Custom types through a
//! schema"). Here the IR is the one `ToPsszSchema` builds for Rust types and
//! the reference is the typed Rust codec; the C++-recorded IR and bytes are
//! replayed in schema_conformance.rs.

use psio::json::{self, IJson, Json, TextForm};
use psio::pssz::dynamic::{self, CustomHandlers, Value};
use psio::pssz::schema_ir::{
    schema_for, wire_custom_id, AnyType, AnyValue, Attribute, Custom, Int, List, Member, Object,
    Schema, ToPsszSchema,
};
use psio::pssz::{
    auto_pssz_width, from_pssz, try_to_pssz, BoundedList, BoundedString, Pssz, Pssz16, Pssz32,
    Pssz8, PsszError, PsszFormat, PsszPack, PsszUnpack, RawString,
};

macro_rules! record {
    ($(#[$attr:meta])* $name:ident { $($field:ident : $ty:ty),* $(,)? }) => {
        #[derive(
            Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
            psio::ToPsszSchema,
        )]
        $(#[$attr])*
        struct $name { $($field: $ty),* }
    };
}

// ── Boxes ───────────────────────────────────────────────────────────────────

record!(Leaf { n: u16, t: BoundedString<4> }); // bounded: at most 11 bytes
record!(Direct { id: u32, leaf: Leaf }); // no box: 1-byte offsets
record!(BoxRec { id: u32, leaf: Box<Leaf> }); // boxed: no bound, 4-byte offsets
record!(#[pssz(max_dynamic_data = 200)] BoxCapRec { id: u32, leaf: Box<Leaf> });
record!(BoxOptRec { id: u32, leaf: Option<Box<Leaf>>, tail: u8 });
record!(#[pssz(max_dynamic_data = 200)] BoxOptCapRec { id: u32, leaf: Option<Box<Leaf>>, tail: u8 });
record!(BoxVecRec { id: u8, leaves: BoundedList<Box<Leaf>, 3> });
record!(#[pssz(max_dynamic_data = 60000)] BoxVecCapRec { id: u8, leaves: Vec<Box<Leaf>> });
record!(BoxFixedRec { id: u8, word: Box<u32>, pair: Box<[u8; 2]> });
record!(BoxOuter { k: u8, capped: BoxCapRec, open: BoxRec, direct: Option<Direct> });
record!(BoxedOuter { k: u8, inner: Box<BoxCapRec> });
record!(Chain { value: u8, next: Option<Box<Chain>> });

fn leaf(n: u16, t: &str) -> Leaf {
    Leaf { n, t: BoundedString(t.into()) }
}
fn boxed(n: u16, t: &str) -> Box<Leaf> {
    Box::new(leaf(n, t))
}

/// The schema-derived width is the typed one, and the typed bytes go
/// through the schema unchanged, in the automatic profile and in every
/// explicit one; the texts agree as well.
fn agrees<T>(value: &T) -> usize
where
    T: ToPsszSchema + json::JsonText + PartialEq + std::fmt::Debug,
    T: PsszPack<Pssz> + PsszUnpack<Pssz> + PsszPack<Pssz8> + PsszPack<Pssz16> + PsszPack<Pssz32>,
{
    let (schema, root) = schema_for::<T>().unwrap();
    let width = dynamic::auto_width(&schema, &root).unwrap();
    assert_eq!(width, auto_pssz_width::<T>(), "{value:?}: width");
    fn round_trip<F: PsszFormat, T: PsszPack<F>>(schema: &Schema, root: &AnyType, value: &T) {
        // An explicit width may be too narrow for the value: both refuse.
        let Ok(bytes) = try_to_pssz::<F, T>(value) else { return };
        let decoded = dynamic::decode_exact::<F>(schema, root, &bytes).expect("schema decode");
        assert_eq!(dynamic::encode::<F>(schema, root, &decoded).unwrap(), bytes);
    }
    round_trip::<Pssz, T>(&schema, &root, value);
    round_trip::<Pssz8, T>(&schema, &root, value);
    round_trip::<Pssz16, T>(&schema, &root, value);
    round_trip::<Pssz32, T>(&schema, &root, value);
    let bytes = try_to_pssz::<Pssz, T>(value).unwrap();
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
    // A box is its inner type in text.
    let text = json::to_string::<IJson, T>(value).unwrap();
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, text);
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, text.as_bytes()).unwrap(), bytes);
    assert_eq!(&from_pssz::<Pssz, T>(&bytes).unwrap(), value);
    width
}

#[test]
fn a_box_gives_its_container_no_inferred_bound_through_the_schema_either() {
    // Members of records, with and without an explicit cap.
    assert_eq!(agrees(&leaf(7, "abcd")), 1);
    assert_eq!(agrees(&Direct { id: 1, leaf: leaf(2, "ab") }), 1);
    assert_eq!(agrees(&BoxRec { id: 1, leaf: boxed(2, "ab") }), 4);
    assert_eq!(agrees(&BoxRec { id: 0, leaf: boxed(0, "") }), 4);
    assert_eq!(agrees(&BoxCapRec { id: 1, leaf: boxed(2, "ab") }), 1);
    // Optionals.
    assert_eq!(agrees(&BoxOptRec { id: 3, leaf: Some(boxed(4, "xyz")), tail: 9 }), 4);
    assert_eq!(agrees(&BoxOptRec { id: 3, leaf: None, tail: 9 }), 4);
    assert_eq!(agrees(&BoxOptCapRec { id: 3, leaf: Some(boxed(4, "xyz")), tail: 9 }), 1);
    assert_eq!(agrees(&BoxOptCapRec { id: 3, leaf: None, tail: 9 }), 1);
    // Vectors.
    let three = vec![boxed(1, "a"), boxed(2, ""), boxed(3, "abcd")];
    assert_eq!(agrees(&BoxVecRec { id: 5, leaves: BoundedList(three.clone()) }), 4);
    assert_eq!(agrees(&BoxVecRec { id: 5, leaves: BoundedList(vec![]) }), 4);
    assert_eq!(agrees(&BoxVecCapRec { id: 5, leaves: three.clone() }), 2);
    // Fixed payloads stay inline, the record still has no bound.
    assert_eq!(agrees(&BoxFixedRec { id: 1, word: Box::new(0x0102_0304), pair: Box::new([5, 6]) }), 4);
    // Nested: a capped child reserves its cap, an open one makes the parent
    // unbounded; a boxed capped child is unbounded again.
    assert_eq!(
        agrees(&BoxOuter {
            k: 1,
            capped: BoxCapRec { id: 2, leaf: boxed(3, "c") },
            open: BoxRec { id: 4, leaf: boxed(5, "open") },
            direct: Some(Direct { id: 6, leaf: leaf(7, "d") }),
        }),
        4
    );
    assert_eq!(agrees(&BoxedOuter { k: 1, inner: Box::new(BoxCapRec { id: 2, leaf: boxed(3, "c") }) }), 4);
    // Structural positions.
    assert_eq!(agrees(&boxed(9, "top")), 4);
    assert_eq!(agrees(&vec![boxed(1, "a"), boxed(2, "bb")]), 4);
    assert_eq!(agrees(&Vec::<Box<Leaf>>::new()), 4);
    assert_eq!(agrees(&Some(boxed(1, "a"))), 4);
    assert_eq!(agrees(&None::<Box<Leaf>>), 4);
    assert_eq!(agrees(&(boxed(1, "t"), 2u8)), 4);
    assert_eq!(agrees(&[boxed(1, "a"), boxed(2, "b")]), 4);
    assert_eq!(agrees(&vec![BoxRec { id: 1, leaf: boxed(2, "ab") }]), 4);
    // An optional capped record reserves the cap: bounded again.
    assert_eq!(agrees(&Some(BoxCapRec { id: 1, leaf: boxed(2, "ab") })), 1);
    // Recursion goes through a box.
    let chain = Chain { value: 1, next: Some(Box::new(Chain { value: 7, next: None })) };
    assert_eq!(agrees(&chain), 4);

    // The marker is what carries it: the IR of the boxed member is a Custom
    // "box" over the named inner type, and the same schema without the
    // marker derives the narrower width.
    let (schema, root) = schema_for::<BoxRec>().unwrap();
    let AnyValue::Object(record) = &schema.resolve(&root).unwrap().value else { panic!("Object") };
    let member = schema.resolve(&record.members[1].r#type).unwrap();
    assert_eq!(
        member.value,
        AnyValue::Custom(Custom { r#type: Box::new(AnyType::named("Leaf")), id: "box".into() })
    );
    assert_eq!(schema.wire(&record.members[1].r#type).unwrap(), schema.get("Leaf").unwrap());
    assert_eq!(schema.wire_boxed(&record.members[1].r#type).unwrap().1, true);
    assert_eq!(schema.wire_boxed(&record.members[0].r#type).unwrap().1, false);
    let mut unmarked = schema.clone();
    for ty in unmarked.types.iter_mut() {
        if matches!(&ty.r#type.value, AnyValue::Custom(c) if c.id == "box") {
            *ty.r#type = AnyType::named("Leaf");
        }
    }
    assert_eq!(dynamic::auto_width(&unmarked, &root).unwrap(), 1);
    assert_eq!(dynamic::auto_width(&schema, &root).unwrap(), 4);
    // The boxed bounded string is still bounded and still UTF-8.
    let value = BoxRec { id: 1, leaf: boxed(7, "abcd") };
    let mut bytes = try_to_pssz::<Pssz, _>(&value).unwrap();
    let last = bytes.len() - 1;
    bytes[last] = 0xff;
    assert!(from_pssz::<Pssz, BoxRec>(&bytes).is_err());
    assert!(dynamic::validate_pssz(&schema, &root, &bytes).is_err());
    assert!(dynamic::json_to_pssz::<Json>(&schema, &root, br#"{"id":1,"leaf":{"n":7,"t":"abcde"}}"#).is_err());
}

// ── Raw strings and UTF-8 strings ───────────────────────────────────────────

record!(Texts { text: String, raw: RawString, list: Vec<RawString>, maybe: Option<RawString> });

#[test]
fn a_raw_string_is_any_bytes_through_the_schema_and_a_utf8_string_is_validated() {
    let (schema, root) = schema_for::<Texts>().unwrap();
    let id_of = |index: usize| {
        let AnyValue::Object(record) = &schema.resolve(&root).unwrap().value else { panic!() };
        match &schema.resolve(&record.members[index].r#type).unwrap().value {
            AnyValue::Custom(custom) => custom.id.clone(),
            other => panic!("{other:?}"),
        }
    };
    assert_eq!((id_of(0).as_str(), id_of(1).as_str()), ("utf8", "string"));

    fn check<F: PsszFormat>(schema: &Schema, root: &AnyType)
    where
        Texts: PsszPack<F> + PsszUnpack<F>,
    {
        let value = Texts {
            text: "t\u{e9}".into(),
            raw: RawString(vec![0xff, 0xfe]),
            list: vec![RawString(b"ok".to_vec()), RawString(vec![0xc0, 0x80])],
            maybe: Some(RawString(vec![0x80])),
        };
        let bytes = try_to_pssz::<F, _>(&value).unwrap();
        // Valid PSSZ: validate, decode, re-encode unchanged.
        let decoded = dynamic::decode_exact::<F>(schema, root, &bytes).unwrap();
        assert_eq!(dynamic::encode::<F>(schema, root, &decoded).unwrap(), bytes);
        assert!(!dynamic::decode_compatible::<F>(schema, root, &bytes).unwrap().has_unknown);
        // The bytes are carried as bytes; text that is UTF-8 as text.
        let Value::Record(fields) = &decoded else { panic!("record") };
        assert_eq!(fields[0].1, Value::String("t\u{e9}".into()));
        assert_eq!(fields[1].1, Value::Bytes(vec![0xff, 0xfe]));
        assert_eq!(
            fields[2].1,
            Value::Sequence(vec![Value::String("ok".into()), Value::Bytes(vec![0xc0, 0x80])])
        );
        // The UTF-8 member is still checked, on read and on write.
        let at = bytes.windows(2).position(|pair| pair == [b't', 0xc3]).unwrap();
        let mut bad = bytes.clone();
        bad[at + 1] = 0xff;
        assert!(from_pssz::<F, Texts>(&bad).is_err());
        assert_eq!(
            dynamic::decode_exact::<F>(schema, root, &bad).unwrap_err().0,
            "pssz dynamic: invalid UTF-8"
        );
        let mut fields = fields.clone();
        fields[0].1 = Value::Bytes(vec![0xff]);
        assert!(dynamic::encode::<F>(schema, root, &Value::Record(fields)).is_err());
    }
    check::<Pssz>(&schema, &root);
    check::<Pssz8>(&schema, &root);
    check::<Pssz16>(&schema, &root);
    check::<Pssz32>(&schema, &root);

    // No JSON or I-JSON text for bytes that are not UTF-8: an error in the
    // schema path as in the typed one, whichever member holds them.
    let texts = |raw: Vec<u8>, list: Vec<Vec<u8>>, maybe: Option<Vec<u8>>| Texts {
        text: "t".into(),
        raw: RawString(raw),
        list: list.into_iter().map(RawString).collect(),
        maybe: maybe.map(RawString),
    };
    for value in [
        texts(vec![0xff], vec![], None),
        texts(b"ok".to_vec(), vec![b"ok".to_vec(), vec![0xc3]], None),
        texts(b"ok".to_vec(), vec![], Some(vec![0xed, 0xa0, 0x80])),
    ] {
        let bytes = try_to_pssz::<Pssz, _>(&value).unwrap();
        assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
        let typed = json::to_string::<Json, _>(&value).unwrap_err().what;
        assert_eq!(typed, "json: string is not valid UTF-8");
        assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap_err().what(), typed);
        assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap_err().what(), typed);
    }
    // A raw string whose bytes are UTF-8 is text in both directions.
    let value = texts("r\u{e9}".as_bytes().to_vec(), vec![b"a".to_vec()], Some(vec![]));
    let bytes = try_to_pssz::<Pssz, _>(&value).unwrap();
    let text = json::to_string::<IJson, _>(&value).unwrap();
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, text);
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, text.as_bytes()).unwrap(), bytes);

    // By hand: the two ids over the same byte list.
    let bytes_of = |id: &str| {
        AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::List(List {
                r#type: Box::new(AnyType::new(AnyValue::Int(Int { bits: 8, isSigned: false }))),
            }))),
            id: id.into(),
        }))
    };
    let schema = Schema::default();
    let (raw, utf8) = (bytes_of("string"), bytes_of("utf8"));
    assert_eq!(dynamic::validate_pssz(&schema, &raw, &[0xff]), Ok(false));
    assert!(dynamic::validate_pssz(&schema, &utf8, &[0xff]).is_err());
    assert_eq!(
        dynamic::decode_exact::<Pssz>(&schema, &raw, &[0xff]).unwrap(),
        Value::Bytes(vec![0xff])
    );
    assert_eq!(dynamic::encode::<Pssz>(&schema, &raw, &Value::Bytes(vec![0xff])).unwrap(), [0xff]);
    assert!(dynamic::encode::<Pssz>(&schema, &utf8, &Value::Bytes(vec![0xff])).is_err());
    assert!(dynamic::encode::<Pssz>(&schema, &utf8, &Value::Bytes(b"ok".to_vec())).is_err());
    assert_eq!(dynamic::encode::<Pssz>(&schema, &utf8, &Value::String("ok".into())).unwrap(), b"ok");
    assert!(dynamic::value_to_json::<Json>(&schema, &raw, &Value::Bytes(vec![0xff])).is_err());
    assert_eq!(
        dynamic::value_to_json::<Json>(&schema, &raw, &Value::Bytes(b"ok".to_vec())).unwrap(),
        "\"ok\""
    );
    assert!(wire_custom_id("string") && wire_custom_id("utf8") && wire_custom_id("unknown"));
    assert!(!wire_custom_id("box") && !wire_custom_id("AccountName"));
}

// ── maxDynamicData under an explicit width ──────────────────────────────────

record!(#[pssz(max_dynamic_data = 12)] Small { tag: u8, label: RawString });
record!(Holder { first: Small, items: Vec<Small>, maybe: Option<Small> });
record!(#[pssz(max_dynamic_data = 64)] CappedHolder { first: Small, second: Small });

#[test]
fn an_explicit_width_enforces_the_cap_of_the_top_level_type_only() {
    fn small(n: usize) -> Small {
        Small { tag: 1, label: RawString(vec![b'x'; n]) }
    }
    /// Typed and schema-driven agree on whether the value encodes, on the
    /// bytes, and on whether the bytes are read back.
    fn agree<F: PsszFormat, T>(value: &T) -> bool
    where
        T: ToPsszSchema + PsszPack<F> + PsszUnpack<F> + std::fmt::Debug,
    {
        let (schema, root) = schema_for::<T>().unwrap();
        // The schema is accepted under an explicit width.
        let text = serde_json::to_string(&schema).unwrap();
        assert!(text.contains("maxDynamicData"));
        let typed = try_to_pssz::<F, T>(value);
        let Ok(bytes) = &typed else { return false };
        let decoded = dynamic::decode_exact::<F>(&schema, &root, bytes).unwrap_or_else(|e| {
            panic!("{value:?}: typed wrote what the schema refuses: {}", e.0)
        });
        assert_eq!(&dynamic::encode::<F>(&schema, &root, &decoded).unwrap(), bytes);
        assert!(from_pssz::<F, T>(bytes).is_ok());
        true
    }
    // Under an explicit width the size of Small's frame is tag + offset +
    // header + label: the cap of 12 is enforced when Small is the root ...
    fn top<F: PsszFormat>()
    where
        Small: PsszPack<F> + PsszUnpack<F>,
    {
        let (schema, root) = schema_for::<Small>().unwrap();
        let at_cap = 12 - (1 + 2 * F::OFFSET_BYTES);
        assert!(agree::<F, Small>(&small(at_cap)));
        // ... over it: an error on write and on read, typed and through
        // the schema.
        let over = small(at_cap + 1);
        assert!(try_to_pssz::<F, _>(&over).is_err());
        let decoded = Value::Record(vec![
            ("tag".into(), Value::Unsigned(1)),
            ("label".into(), Value::String("x".repeat(at_cap + 1))),
        ]);
        assert_eq!(
            dynamic::encode::<F>(&schema, &root, &decoded).unwrap_err().0,
            "pssz dynamic: encoded size exceeds maxDynamicData cap"
        );
        // The frame itself is well-formed: only the cap refuses it.
        let (uncapped_schema, uncapped_root) = {
            let mut schema = schema.clone();
            for ty in schema.types.iter_mut() {
                if let AnyValue::Object(record) = &mut ty.r#type.value {
                    record.attributes.clear();
                }
            }
            (schema, root.clone())
        };
        let frame = dynamic::encode::<F>(&uncapped_schema, &uncapped_root, &decoded).unwrap();
        assert_eq!(frame.len(), 13);
        assert!(dynamic::decode_exact::<F>(&uncapped_schema, &uncapped_root, &frame).is_ok());
        assert!(dynamic::decode_exact::<F>(&schema, &root, &frame).is_err());
        assert!(dynamic::decode_compatible::<F>(&schema, &root, &frame).is_err());
        assert!(from_pssz::<F, Small>(&frame).is_err());
    }
    top::<Pssz8>();
    top::<Pssz16>();
    top::<Pssz32>();

    // ... and is not looked up when Small is nested: member, list element,
    // optional payload.
    fn nested<F: PsszFormat>()
    where
        Holder: PsszPack<F> + PsszUnpack<F>,
        CappedHolder: PsszPack<F> + PsszUnpack<F>,
    {
        let over = || small(40);
        assert!(agree::<F, Holder>(&Holder { first: over(), items: vec![], maybe: None }));
        assert!(agree::<F, Holder>(&Holder { first: small(1), items: vec![over(), over()], maybe: None }));
        assert!(agree::<F, Holder>(&Holder { first: small(1), items: vec![], maybe: Some(over()) }));
        assert!(agree::<F, Vec<Small>>(&vec![over()]));
        assert!(agree::<F, Option<Small>>(&Some(over())));
        // A capped parent enforces its own cap, not its members'.
        assert!(agree::<F, CappedHolder>(&CappedHolder { first: small(13), second: small(0) }));
        let big = CappedHolder { first: small(30), second: small(30) };
        assert!(!agree::<F, CappedHolder>(&big));
        let (schema, root) = schema_for::<CappedHolder>().unwrap();
        let member = |n: usize| {
            Value::Record(vec![
                ("tag".into(), Value::Unsigned(1)),
                ("label".into(), Value::String("x".repeat(n))),
            ])
        };
        let value = Value::Record(vec![("first".into(), member(30)), ("second".into(), member(30))]);
        assert!(dynamic::encode::<F>(&schema, &root, &value).is_err());
    }
    nested::<Pssz8>();
    nested::<Pssz16>();
    nested::<Pssz32>();

    // The automatic profile enforces every record's cap, nested ones too.
    let (schema, root) = schema_for::<Holder>().unwrap();
    let holder = Holder { first: small(40), items: vec![], maybe: None };
    assert!(try_to_pssz::<Pssz, _>(&holder).is_err());
    let wide = try_to_pssz::<Pssz32, _>(&holder).unwrap();
    assert!(dynamic::decode_exact::<Pssz32>(&schema, &root, &wide).is_ok());
    let value = dynamic::decode_exact::<Pssz32>(&schema, &root, &wide).unwrap();
    assert_eq!(
        dynamic::encode::<Pssz>(&schema, &root, &value).unwrap_err().0,
        "pssz dynamic: encoded size exceeds maxDynamicData cap"
    );
    // maxFields has no explicit-width implementation: still refused there.
    let fields = AnyType::new(AnyValue::Object(Object {
        members: vec![],
        attributes: vec![Attribute { name: "maxFields".into(), value: Some("4".into()) }],
    }));
    let empty = Schema::default();
    assert_eq!(
        dynamic::encode::<Pssz16>(&empty, &fields, &Value::Record(vec![])).unwrap_err().0,
        dynamic::UNSUPPORTED_CONSTRAINT
    );
    assert!(dynamic::encode::<Pssz>(&empty, &fields, &Value::Record(vec![])).is_ok());
}

// ── Custom text presentations ───────────────────────────────────────────────

/// Seconds since the epoch, written "<seconds>s": a custom text form over a
/// transparent u32, registered under an explicit id.
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[pssz(transparent, custom = "Seconds")]
struct Stamp(u32);
impl TextForm for Stamp {
    fn to_text(&self, out: &mut String) {
        out.push_str(&format!("{}s", self.0));
    }
    fn from_text(text: &str) -> Option<Self> {
        let digits = text.strip_suffix('s')?;
        let canonical = !digits.is_empty()
            && digits.bytes().all(|b| b.is_ascii_digit())
            && (digits == "0" || !digits.starts_with('0'));
        if canonical { digits.parse().ok().map(Stamp) } else { None }
    }
}
psio::json_text_form!(Stamp);

/// A custom text form over a record, under the type's own name: "x,y".
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[pssz(definition_will_not_change, custom)]
struct Point {
    x: i16,
    y: i16,
}
impl TextForm for Point {
    fn to_text(&self, out: &mut String) {
        out.push_str(&format!("{},{}", self.x, self.y));
    }
    fn from_text(text: &str) -> Option<Self> {
        let (x, y) = text.split_once(',')?;
        Some(Point { x: x.parse().ok()?, y: y.parse().ok()? })
    }
}
psio::json_text_form!(Point);

record!(Event { at: Stamp, place: Point, history: Vec<Stamp>, maybe: Option<Point>, boxed: Box<Stamp>, n: u64 });

#[test]
fn a_custom_text_form_is_its_wire_type_in_binary_and_its_handler_in_text() {
    // The derive: Custom { wire type, id }; the id defaults to the name.
    assert_eq!(Stamp::custom_id().as_deref(), Some("Seconds"));
    assert_eq!(Point::custom_id().as_deref(), Some("Point"));
    assert_eq!(Event::custom_id(), None);
    let (schema, root) = schema_for::<Event>().unwrap();
    assert_eq!(
        schema.get("Stamp").unwrap().value,
        AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::Int(Int { bits: 32, isSigned: false }))),
            id: "Seconds".into(),
        })
    );
    let AnyValue::Custom(point) = &schema.get("Point").unwrap().value else { panic!("Custom") };
    assert_eq!(point.id, "Point");
    assert!(matches!(point.r#type.value, AnyValue::Object(_)));

    let event = Event {
        at: Stamp(7),
        place: Point { x: -1, y: 2 },
        history: vec![Stamp(0), Stamp(u32::MAX)],
        maybe: Some(Point { x: 3, y: -4 }),
        boxed: Box::new(Stamp(9)),
        n: u64::MAX,
    };
    // Binary: exactly the wire type, with or without handlers: same width,
    // same bytes, same validation.
    assert_eq!(dynamic::auto_width(&schema, &root).unwrap(), auto_pssz_width::<Event>());
    for id in ["Stamp", "Point"] {
        let named = AnyType::named(id);
        let AnyValue::Custom(custom) = &schema.get(id).unwrap().value else { panic!("Custom") };
        assert_eq!(
            dynamic::auto_width(&schema, &named).unwrap(),
            dynamic::auto_width(&schema, &custom.r#type).unwrap()
        );
        assert_eq!(schema.wire(&named).unwrap(), schema.wire(&custom.r#type).unwrap());
    }
    assert_eq!(dynamic::auto_width(&schema, &AnyType::named("Stamp")).unwrap(), auto_pssz_width::<Stamp>());
    assert_eq!(dynamic::auto_width(&schema, &AnyType::named("Point")).unwrap(), auto_pssz_width::<Point>());
    let bytes = try_to_pssz::<Pssz, _>(&event).unwrap();
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
    let decoded = dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
    assert_eq!(dynamic::encode::<Pssz>(&schema, &root, &decoded).unwrap(), bytes);
    let wide = try_to_pssz::<Pssz32, _>(&event).unwrap();
    let decoded32 = dynamic::decode_exact::<Pssz32>(&schema, &root, &wide).unwrap();
    assert_eq!(decoded32, decoded);
    assert!(dynamic::validate_pssz(&schema, &root, &bytes[..bytes.len() - 1]).is_err());

    // Text with the handlers: the typed text. One is the type's own
    // TextForm registered in one call, the other written by hand against
    // the PSSZ bytes of the wire type (4 little-endian bytes).
    let mut handlers = CustomHandlers::new();
    handlers.add_type::<Point>().unwrap();
    handlers.add(
        "Seconds",
        |bytes: &[u8]| {
            let seconds = u32::from_le_bytes(bytes.try_into().map_err(|_| PsszError("Seconds: not a u32"))?);
            Ok(format!("{seconds}s"))
        },
        |text: &str| {
            Stamp::from_text(text)
                .map(|stamp| stamp.0.to_le_bytes().to_vec())
                .ok_or(PsszError("Seconds: expected <seconds>s"))
        },
    );
    assert!(handlers.contains("Seconds") && handlers.contains("Point") && !handlers.contains("Stamp"));
    assert!(!handlers.is_strict());
    let json_text = r#"{"at":"7s","place":"-1,2","history":["0s","4294967295s"],"maybe":"3,-4","boxed":"9s","n":18446744073709551615}"#;
    let ijson_text = r#"{"at":"7s","place":"-1,2","history":["0s","4294967295s"],"maybe":"3,-4","boxed":"9s","n":"18446744073709551615"}"#;
    assert_eq!(json::to_string::<Json, _>(&event).unwrap(), json_text);
    assert_eq!(json::to_string::<IJson, _>(&event).unwrap(), ijson_text);
    assert_eq!(dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &handlers).unwrap().0, json_text);
    assert_eq!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &handlers).unwrap().0, ijson_text);
    assert_eq!(dynamic::json_to_pssz_with::<Json>(&schema, &root, json_text.as_bytes(), &handlers).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, ijson_text.as_bytes(), &handlers).unwrap(), bytes);
    // Text <-> Value directly.
    let value = dynamic::json_to_value_with::<IJson>(&schema, &root, ijson_text.as_bytes(), &handlers).unwrap();
    assert_eq!(value, decoded);
    assert_eq!(dynamic::value_to_json_with::<IJson>(&schema, &root, &value, &handlers).unwrap(), ijson_text);

    // Without: the wire type's own text, which reads back to the same bytes.
    let plain = r#"{"at":7,"place":{"x":-1,"y":2},"history":[0,4294967295],"maybe":{"x":3,"y":-4},"boxed":9,"n":18446744073709551615}"#;
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, plain);
    assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, plain.as_bytes()).unwrap(), bytes);
    assert_eq!(dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &CustomHandlers::new()).unwrap().0, plain);
    // With a handler the plain form is not also accepted, nor is text the
    // handler refuses, nor a string where the handler's text is expected
    // to be one but the JSON value is not.
    for bad in [
        plain.to_string(),
        json_text.replace("\"7s\"", "7"),
        json_text.replace("\"7s\"", "\"7\""),
        json_text.replace("\"7s\"", "\"07s\""),
        json_text.replace("\"-1,2\"", "\"-1;2\""),
        json_text.replace("\"-1,2\"", "{\"x\":-1,\"y\":2}"),
        json_text.replace("\"3,-4\"", "\"3,99999\""),
        json_text.replace("\"9s\"", "null"),
    ] {
        assert!(
            dynamic::json_to_pssz_with::<Json>(&schema, &root, bad.as_bytes(), &handlers).is_err(),
            "{bad}"
        );
    }
    // null is still the absent optional, not the handler's business.
    let none = json_text.replace("\"3,-4\"", "null");
    let bytes_none = dynamic::json_to_pssz_with::<Json>(&schema, &root, none.as_bytes(), &handlers).unwrap();
    assert_eq!(from_pssz::<Pssz, Event>(&bytes_none).unwrap().maybe, None);

    // Strict: every custom id needs a handler, in both directions.
    const NO_HANDLER: &str = "pssz dynamic: no handler registered for a custom type";
    let mut strict = CustomHandlers::new();
    strict.set_strict(true).add_type::<Point>().unwrap();
    assert!(strict.is_strict());
    assert_eq!(dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &strict).unwrap_err().what(), NO_HANDLER);
    assert_eq!(
        dynamic::json_to_pssz_with::<Json>(&schema, &root, json_text.as_bytes(), &strict).unwrap_err().what(),
        NO_HANDLER
    );
    assert_eq!(
        dynamic::json_to_value_with::<Json>(&schema, &root, plain.as_bytes(), &strict).unwrap_err().what(),
        NO_HANDLER
    );
    strict.add_type_as::<Stamp>("Seconds");
    assert_eq!(dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &strict).unwrap().0, json_text);
    // A box is never a presentation: strict does not ask for its handler.
    let (boxed_schema, boxed_root) = schema_for::<BoxRec>().unwrap();
    let boxed_bytes = try_to_pssz::<Pssz, _>(&BoxRec { id: 1, leaf: boxed(2, "ab") }).unwrap();
    let mut empty_strict = CustomHandlers::new();
    empty_strict.set_strict(true);
    assert_eq!(
        dynamic::pssz_to_json_with::<Json>(&boxed_schema, &boxed_root, &boxed_bytes, &empty_strict).unwrap().0,
        r#"{"id":1,"leaf":{"n":2,"t":"ab"}}"#
    );
    // A type that is not declared custom has no id to register under.
    #[derive(psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
    #[pssz(transparent)]
    struct Plain(u32);
    impl TextForm for Plain {
        fn to_text(&self, out: &mut String) {
            out.push_str(&self.0.to_string());
        }
        fn from_text(text: &str) -> Option<Self> {
            text.parse().ok().map(Plain)
        }
    }
    assert!(CustomHandlers::new().add_type::<Plain>().is_err());
    assert!(CustomHandlers::new().add_type_as::<Plain>("Plain").contains("Plain"));

    // "unknown" marks a type with no codec: refused in binary and in text.
    let unknown = AnyType::new(AnyValue::Custom(Custom {
        r#type: Box::new(AnyType::new(AnyValue::Int(Int { bits: 8, isSigned: false }))),
        id: "unknown".into(),
    }));
    let empty = Schema::default();
    assert!(dynamic::validate_pssz(&empty, &unknown, &[1]).is_err());
    assert!(dynamic::json_to_pssz::<Json>(&empty, &unknown, b"1").is_err());
    // Any other id over a member is that member's type on the wire.
    let presented = AnyType::new(AnyValue::Object(Object {
        members: vec![Member {
            name: "when".into(),
            r#type: Box::new(AnyType::new(AnyValue::Custom(Custom {
                r#type: Box::new(AnyType::new(AnyValue::Int(Int { bits: 32, isSigned: false }))),
                id: "Seconds".into(),
            }))),
            attributes: vec![],
        }],
        attributes: vec![],
    }));
    let bytes = dynamic::json_to_pssz_with::<Json>(&empty, &presented, br#"{"when":"5s"}"#, &handlers).unwrap();
    assert_eq!(bytes, [4, 5, 0, 0, 0]);
    assert_eq!(dynamic::auto_width(&empty, &presented).unwrap(), 1);
    assert_eq!(dynamic::pssz_to_json::<Json>(&empty, &presented, &bytes).unwrap().0, r#"{"when":5}"#);
}
