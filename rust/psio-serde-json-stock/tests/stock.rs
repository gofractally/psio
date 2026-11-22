//! `serde_json` behaves as it does without `psio` in the graph (RG9).
//!
//! Every expectation below is what `serde_json` 1.x does with its default
//! features. Each one fails if `arbitrary_precision` (numbers keep their
//! source text and reach serde's buffered deserializers as a map) or
//! `preserve_order` (an object keeps insertion order) is enabled anywhere in
//! the graph.

use psio_serde_json_stock::Transfer;
use serde::Deserialize;
use serde_json::{json, Value};

#[test]
fn psio_is_linked_and_its_serde_bridge_works() {
    let value = Transfer { to: "bob".into(), amount: u64::MAX, ratio: 1.5, memo: None };
    assert_eq!(psio::from_pssz::<Transfer>(&psio::to_pssz(&value)).unwrap(), value);
    let text = serde_json::to_string(&value).unwrap();
    assert_eq!(text, r#"{"to":"bob","amount":"18446744073709551615","ratio":1.5,"memo":null}"#);
    assert_eq!(text, psio::to_ijson(&value));
    assert_eq!(serde_json::from_str::<Transfer>(&text).unwrap(), value);
    // Through a Value as well.
    let tree = serde_json::to_value(&value).unwrap();
    assert_eq!(serde_json::from_value::<Transfer>(tree).unwrap(), value);
}

#[test]
fn serde_aux_reads_a_float_from_a_number() {
    #[derive(Debug, Deserialize)]
    struct D {
        #[serde(deserialize_with = "serde_aux::prelude::deserialize_number_from_string")]
        x: f64,
        #[serde(deserialize_with = "serde_aux::prelude::deserialize_number_from_string")]
        y: f64,
        #[serde(deserialize_with = "serde_aux::prelude::deserialize_number_from_string")]
        n: u64,
    }
    let d: D = serde_json::from_str(r#"{"x":1.5,"y":"2.25","n":"18446744073709551615"}"#).unwrap();
    assert_eq!((d.x, d.y, d.n), (1.5, 2.25, u64::MAX));
}

#[test]
fn an_f32_is_written_as_its_widened_f64() {
    assert_eq!(serde_json::to_value(f32::from_bits(1)).unwrap().to_string(), "1.401298464324817e-45");
    assert_eq!(serde_json::to_value(0.1f32).unwrap().to_string(), "0.10000000149011612");
    // Written directly, an f32 has its own shortest digits, as always.
    assert_eq!(serde_json::to_string(&0.1f32).unwrap(), "0.1");
}

#[test]
fn a_value_holds_numbers_not_source_text() {
    let v: Value = serde_json::from_str("1e2").unwrap();
    assert_eq!(v.to_string(), "100.0");
    assert_eq!(v.as_f64(), Some(100.0));
    assert!(v.is_f64());

    // An integer beyond 64 bits is a float.
    let v: Value = serde_json::from_str("123456789012345678901234567890").unwrap();
    assert!(v.is_f64());
    assert_eq!(v, json!(1.2345678901234568e29));
    assert_ne!(v.to_string(), "123456789012345678901234567890");

    // A float round-trips as serde_json spells it, not as it was written.
    let v: Value = serde_json::from_str("1.50").unwrap();
    assert_eq!(v.to_string(), "1.5");
    assert_eq!(v, json!(1.5));
    let v: Value = serde_json::from_str("0.1E-6").unwrap();
    assert_eq!(v.to_string(), "1e-7");

    // Number is a number to serde's data model, not a one-member map.
    let n: serde_json::Number = serde_json::from_str("7").unwrap();
    assert_eq!(serde_json::to_value(&n).unwrap(), json!(7));
    assert!(n.is_u64());
}

#[test]
fn buffered_deserializers_see_numbers() {
    #[derive(Debug, PartialEq, Deserialize)]
    #[serde(untagged)]
    enum NumberOrText {
        Float(f64),
        Text(String),
    }
    assert_eq!(serde_json::from_str::<NumberOrText>("1.5").unwrap(), NumberOrText::Float(1.5));
    assert_eq!(serde_json::from_str::<NumberOrText>("3").unwrap(), NumberOrText::Float(3.0));

    #[derive(Debug, PartialEq, Deserialize)]
    struct Inner {
        f: f32,
    }
    #[derive(Debug, PartialEq, Deserialize)]
    struct Outer {
        a: u8,
        #[serde(flatten)]
        inner: Inner,
    }
    assert_eq!(
        serde_json::from_str::<Outer>(r#"{"a":1,"f":0.25}"#).unwrap(),
        Outer { a: 1, inner: Inner { f: 0.25 } }
    );
}

#[test]
fn an_object_is_ordered_by_name() {
    let v: Value = serde_json::from_str(r#"{"b":1,"a":2,"c":3}"#).unwrap();
    assert_eq!(v.to_string(), r#"{"a":2,"b":1,"c":3}"#);
}

/// The feature set itself, as Cargo resolves it for a dependent of `psio`
/// with default features (dev-dependencies excluded).
#[test]
fn psio_enables_no_behaviour_changing_serde_json_feature() {
    let manifest = concat!(env!("CARGO_MANIFEST_DIR"), "/Cargo.toml");
    let output = std::process::Command::new(env!("CARGO"))
        .args(["tree", "--manifest-path", manifest, "--locked", "--offline"])
        .args(["-e", "features,normal,build", "-p", "psio-serde-json-stock"])
        .args(["--prefix", "none", "--format", "{p} [{f}]"])
        .output()
        .expect("cargo tree runs");
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    let tree = String::from_utf8(output.stdout).unwrap();
    let lines: Vec<&str> = tree.lines().filter(|l| l.starts_with("serde_json v")).collect();
    assert!(!lines.is_empty(), "serde_json is in the graph:\n{tree}");
    for line in &lines {
        let features = &line[line.find('[').unwrap()..];
        for forbidden in ["arbitrary_precision", "preserve_order", "float_roundtrip", "unbounded_depth"] {
            assert!(!features.contains(forbidden), "serde_json has {forbidden}: {line}");
        }
    }
    // What psio does enable, and nothing else beyond serde_json's defaults.
    assert!(lines.iter().any(|l| l.contains("raw_value")), "{lines:?}");
}
