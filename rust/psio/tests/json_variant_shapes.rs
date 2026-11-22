//! `JsonText` for every variant shape `PsszPack` encodes (RG7): the text of
//! an alternative is the text of its payload, and equals what the
//! schema-driven codec writes from the type's schema and pSSZ bytes.

use psio::pssz::dynamic::json::{json_to_pssz, pssz_to_json};
use psio::pssz::schema_ir::schema_for;
use psio::{IJson, Json, JsonText, PsszPack, PsszUnpack, Serde, ToPsszSchema};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText, Serde)]
enum Shape {
    /// No field: the empty record.
    Empty,
    /// One unnamed field: that value.
    One(u32),
    /// Two unnamed fields: their tuple.
    Pair(u64, String),
    /// Named fields: a record.
    Rect { width: u32, height: u32, label: Option<String> },
    #[psio(name = "renamed")]
    Named { big_value: u64 },
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
#[psio(rename_all = "camelCase")]
enum Cased {
    NothingHere,
    #[serde(rename_all = "camelCase")]
    WithFields { first_name: String, last_name: String },
    Other(u8),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
struct Holder {
    shapes: Vec<Shape>,
    maybe: Option<Shape>,
}

fn all() -> Vec<Shape> {
    vec![
        Shape::Empty,
        Shape::One(7),
        Shape::Pair(u64::MAX, "p".into()),
        Shape::Rect { width: 3, height: 4, label: None },
        Shape::Rect { width: 3, height: 4, label: Some("r".into()) },
        Shape::Named { big_value: u64::MAX },
    ]
}

#[test]
fn each_shape_has_its_payloads_text() {
    let ijson: Vec<String> = all().iter().map(psio::to_ijson).collect();
    assert_eq!(
        ijson,
        [
            r#"{"Empty":{}}"#,
            r#"{"One":7}"#,
            r#"{"Pair":["18446744073709551615","p"]}"#,
            r#"{"Rect":{"width":3,"height":4,"label":null}}"#,
            r#"{"Rect":{"width":3,"height":4,"label":"r"}}"#,
            r#"{"renamed":{"big_value":"18446744073709551615"}}"#,
        ]
    );
    let json: Vec<String> = all().iter().map(psio::to_json).collect();
    assert_eq!(
        json,
        [
            r#"[0,{}]"#,
            r#"[1,7]"#,
            r#"[2,[18446744073709551615,"p"]]"#,
            r#"[3,{"width":3,"height":4,"label":null}]"#,
            r#"[3,{"width":3,"height":4,"label":"r"}]"#,
            r#"[4,{"big_value":18446744073709551615}]"#,
        ]
    );
    for value in all() {
        assert_eq!(psio::from_ijson::<Shape>(psio::to_ijson(&value)).unwrap(), value);
        assert_eq!(psio::from_json::<Shape>(psio::to_json(&value)).unwrap(), value);
        psio::validate_ijson::<Shape>(psio::to_ijson(&value)).unwrap();
        // serde, through the bridge.
        let text = serde_json::to_string(&value).unwrap();
        assert_eq!(text, psio::to_ijson(&value));
        assert_eq!(serde_json::from_str::<Shape>(&text).unwrap(), value);
    }
}

#[test]
fn the_typed_text_is_the_schema_driven_text() {
    let (schema, root) = schema_for::<Shape>().unwrap();
    for value in all() {
        let bytes = psio::to_pssz(&value);
        let (text, _) = pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap();
        assert_eq!(text, psio::to_ijson(&value), "{value:?}");
        let (text, _) = pssz_to_json::<Json>(&schema, &root, &bytes).unwrap();
        assert_eq!(text, psio::to_json(&value), "{value:?}");
        // And back: the text gives the typed bytes.
        assert_eq!(json_to_pssz::<IJson>(&schema, &root, psio::to_ijson(&value).as_bytes()).unwrap(), bytes);
        assert_eq!(json_to_pssz::<Json>(&schema, &root, psio::to_json(&value).as_bytes()).unwrap(), bytes);
    }
    // Inside a record, a list and an optional.
    let holder = Holder { shapes: all(), maybe: Some(Shape::Empty) };
    let (schema, root) = schema_for::<Holder>().unwrap();
    let bytes = psio::to_pssz(&holder);
    let (text, _) = pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap();
    assert_eq!(text, psio::to_ijson(&holder));
    assert_eq!(psio::from_ijson::<Holder>(&text).unwrap(), holder);

    let (schema, root) = schema_for::<Cased>().unwrap();
    for value in [
        Cased::NothingHere,
        Cased::WithFields { first_name: "a".into(), last_name: "b".into() },
        Cased::Other(1),
    ] {
        let bytes = psio::to_pssz(&value);
        let (text, _) = pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap();
        assert_eq!(text, psio::to_ijson(&value), "{value:?}");
        assert_eq!(psio::from_ijson::<Cased>(&text).unwrap(), value);
    }
    assert_eq!(psio::to_ijson(&Cased::NothingHere), r#"{"nothingHere":{}}"#);
    assert_eq!(
        psio::to_ijson(&Cased::WithFields { first_name: "a".into(), last_name: "b".into() }),
        r#"{"withFields":{"firstName":"a","lastName":"b"}}"#
    );
}

#[test]
fn a_payload_is_read_by_its_own_rules() {
    // A record payload: members in any order, an unknown member skipped, a
    // missing optional member None, a missing required member an error.
    assert_eq!(
        psio::from_ijson::<Shape>(r#"{"Rect":{"height":4,"x":[1,{}],"width":3}}"#).unwrap(),
        Shape::Rect { width: 3, height: 4, label: None }
    );
    assert!(psio::from_ijson::<Shape>(r#"{"Rect":{"height":4}}"#).is_err());
    // The empty record skips unknown members too; nothing else is one.
    assert_eq!(psio::from_ijson::<Shape>(r#"{"Empty":{"later":1}}"#).unwrap(), Shape::Empty);
    for bad in [r#"{"Empty":null}"#, r#"{"Empty":[]}"#, r#"{"Empty":0}"#, r#""Empty""#, r#"{"Empty"}"#] {
        assert!(psio::from_ijson::<Shape>(bad).is_err(), "{bad}");
    }
    // A tuple payload has exactly its arity.
    for bad in [r#"{"Pair":["1"]}"#, r#"{"Pair":["1","p",3]}"#, r#"{"Pair":{"0":"1","1":"p"}}"#] {
        assert!(psio::from_ijson::<Shape>(bad).is_err(), "{bad}");
    }
    assert_eq!(psio::from_ijson::<Shape>(r#"{"Pair":[1,"p"]}"#).unwrap(), Shape::Pair(1, "p".into()));
    // The alternative is named as declared.
    assert!(psio::from_ijson::<Shape>(r#"{"Named":{"big_value":"1"}}"#).is_err());
    assert_eq!(psio::from_ijson::<Shape>(r#"{"renamed":{"big_value":"1"}}"#).unwrap(), Shape::Named { big_value: 1 });
}
