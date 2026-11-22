// `#[derive(psio::Serde)]` and `psio::serde_bridge`: serde impls whose
// behaviour is PSIO's JSON rules. With serde_json the text is PSIO's IJson
// text and what is accepted is what `psio::from_ijson` accepts; every IJson
// text of conformance/json-vectors.json is run through the bridge in
// json_conformance.rs.

use psio::key::ByKey;
use psio::pssz::{
    BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, Nested, RawString, Uint256,
};
use psio::time::{Seconds, TimePoint};
type TimePointSec = TimePoint<Seconds>;
use psio::{JsonText, PsszPack, PsszUnpack, Serde};
use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
#[psio(invariant = "within_limit")]
struct Balance {
    amount: u64,
    limit: u64,
}
fn within_limit(balance: &Balance) -> Result<(), String> {
    if balance.amount > balance.limit {
        Err(format!("balance {} exceeds limit {}", balance.amount, balance.limit))
    } else {
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
#[repr(u8)]
#[serde(rename_all = "snake_case")]
enum Status {
    NotStarted = 0,
    Active = 1,
    #[psio(other)]
    Unknown(u8),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
enum Action {
    Transfer(Balance),
    #[serde(rename = "memo")]
    Note(String),
    Pair((u8, i64)),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
#[psio(transparent)]
struct Id(u128);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
struct Wrapper<T> {
    value: T,
    tags: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
#[serde(rename_all = "camelCase")]
struct Everything {
    small: u32,
    big_unsigned: u64,
    big_signed: i64,
    id: Id,
    wide: Uint256,
    ratio: f64,
    flag: bool,
    bytes: Vec<u8>,
    hash: [u8; 4],
    text: String,
    raw: RawString,
    short: BoundedString<8>,
    short_raw: BoundedRawString<8>,
    numbers: BoundedList<u16, 4>,
    maybe: Option<u32>,
    status: Status,
    actions: Vec<Action>,
    by_name: BTreeMap<String, u32>,
    by_number: BoundedMap<u64, String, 4>,
    set: BoundedSet<u8, 4>,
    ordered: ByKey<f64>,
    nested: Nested<Balance>,
    when: TimePointSec,
    wrapped: Wrapper<Balance>,
    #[psio(skip)]
    cache: u32,
    #[serde(rename = "type")]
    kind: u8,
    unit: (),
}

fn everything() -> Everything {
    Everything {
        small: 7,
        big_unsigned: u64::MAX,
        big_signed: -(1 << 60),
        id: Id(u128::MAX),
        wide: Uint256::from_u64(5),
        ratio: 0.25,
        flag: true,
        bytes: vec![0xde, 0xad],
        hash: [1, 2, 3, 0xff],
        text: "h\u{e9}llo \"q\" \n".into(),
        raw: RawString::from("raw"),
        short: BoundedString("abc".into()),
        short_raw: BoundedRawString(b"xyz".to_vec()),
        numbers: BoundedList(vec![1, 2, 3]),
        maybe: None,
        status: Status::Active,
        actions: vec![
            Action::Transfer(Balance { amount: 1, limit: 2 }),
            Action::Note("n".into()),
            Action::Pair((1, -2)),
        ],
        by_name: BTreeMap::from([("a".to_string(), 1), ("b".to_string(), 2)]),
        by_number: BoundedMap(BTreeMap::from([(1 << 60, "big".to_string())])),
        set: BoundedSet([3u8, 1].into_iter().collect()),
        ordered: ByKey(1.5),
        nested: Nested::new(&Balance { amount: 3, limit: 4 }).unwrap(),
        when: TimePointSec::from_ticks(1_700_000_000),
        wrapped: Wrapper { value: Balance { amount: 5, limit: 6 }, tags: vec!["t".into()] },
        cache: 0,
        kind: 9,
        unit: (),
    }
}

#[test]
fn serde_json_writes_the_ijson_text_and_reads_it_back() {
    let value = everything();
    let text = serde_json::to_string(&value).unwrap();
    assert_eq!(text, psio::to_ijson(&value));
    assert_eq!(serde_json::to_vec(&value).unwrap(), psio::to_ijson(&value).into_bytes());
    assert_eq!(serde_json::from_str::<Everything>(&text).unwrap(), value);
    assert_eq!(serde_json::from_slice::<Everything>(text.as_bytes()).unwrap(), value);
    assert_eq!(serde_json::from_reader::<_, Everything>(text.as_bytes()).unwrap(), value);
    // The I-JSON rules, visible in the text.
    for part in [
        r#""small":7"#,
        r#""bigUnsigned":"18446744073709551615""#,
        r#""bigSigned":"-1152921504606846976""#,
        r#""id":"340282366920938463463374607431768211455""#,
        r#""bytes":"DEAD""#,
        r#""hash":"010203FF""#,
        r#""maybe":null"#,
        r#""status":"active""#,
        r#"{"Transfer":{"amount":1,"limit":2}}"#,
        r#"{"memo":"n"}"#,
        r#""byName":{"a":1,"b":2}"#,
        r#""when":"2023-11-14T22:13:20Z""#,
        r#""type":9"#,
    ] {
        assert!(text.contains(part), "{part} in {text}");
    }
    assert!(!text.contains("cache"));

    // Pretty-printed: the same value.
    let pretty = serde_json::to_string_pretty(&value).unwrap();
    assert_eq!(psio::from_ijson::<Everything>(&pretty).unwrap(), value);
    assert_eq!(serde_json::from_str::<Everything>(&pretty).unwrap(), value);

    // Through serde_json::Value, in both directions.
    let tree = serde_json::to_value(&value).unwrap();
    assert_eq!(tree.to_string(), text);
    assert_eq!(serde_json::from_value::<Everything>(tree).unwrap(), value);
}

/// `serde_json::from_str` and `psio::from_ijson` on the same text.
fn same_outcome<T>(text: &str) -> Option<T>
where
    T: psio::json::JsonText + serde::de::DeserializeOwned + PartialEq + std::fmt::Debug,
{
    let typed = psio::from_ijson::<T>(text);
    let through_serde = serde_json::from_str::<T>(text);
    match (&typed, &through_serde) {
        (Ok(a), Ok(b)) => assert_eq!(a, b, "{text}"),
        (Err(a), Err(b)) => {
            // serde_json reports its own syntax errors; a typed error is
            // PSIO's, message included.
            let message = b.to_string();
            assert!(
                message.contains(&*a.what) || serde_json::from_str::<serde_json::Value>(text).is_err(),
                "{text}: {a} / {b}"
            );
        }
        _ => panic!("{text}: from_ijson {typed:?}, serde_json {through_serde:?}"),
    }
    typed.ok()
}

#[test]
fn serde_json_accepts_and_rejects_what_from_ijson_does() {
    // 64-bit integers: a bare number within 2^53 - 1, a string beyond.
    for (text, ok) in [
        (r#"{"amount":1,"limit":2}"#, true),
        (r#"{"amount":"1","limit":"2"}"#, true),
        (r#"{"amount":9007199254740991,"limit":9007199254740991}"#, true),
        (r#"{"amount":9007199254740992,"limit":"18446744073709551615"}"#, false),
        (r#"{"amount":"9007199254740992","limit":"18446744073709551615"}"#, true),
        (r#"{"amount":18446744073709551615,"limit":18446744073709551615}"#, false),
        (r#"{"amount":1.0,"limit":2}"#, false),
        (r#"{"amount":1e0,"limit":2}"#, false),
        (r#"{"amount":-0,"limit":2}"#, false),
        (r#"{"amount":"01","limit":2}"#, false),
        (r#"{"amount":"\u0031","limit":2}"#, false),
        (r#"{"amount":-1,"limit":2}"#, false),
        // Members: any order, unknown skipped, the last of a repeated key.
        (r#"{"limit":2,"amount":1}"#, true),
        (r#"{"amount":1,"limit":2,"extra":{"deep":[1,2,{"x":null}]}}"#, true),
        (r#"{"amount":5,"amount":1,"limit":2}"#, true),
        (r#"{"amount":"x","amount":1,"limit":2}"#, false),
        (r#"{"amount":1}"#, false),
        (r#"{"amount":1,"limit":2} x"#, false),
        (r#"[1,2]"#, false),
        ("", false),
        (" {\"amount\" : 1 ,\n\"limit\":2 } ", true),
        // The invariant.
        (r#"{"amount":7,"limit":5}"#, false),
    ] {
        assert_eq!(same_outcome::<Balance>(text).is_some(), ok, "{text}");
    }
    assert_eq!(
        serde_json::from_str::<Balance>(r#"{"amount":7,"limit":5}"#).unwrap_err().to_string(),
        "balance 7 exceeds limit 5 (at byte 0 of ijson)"
    );
    assert_eq!(
        serde_json::to_string(&Balance { amount: 7, limit: 5 }).unwrap_err().to_string(),
        "balance 7 exceeds limit 5 (at byte 22 of ijson)"
    );

    // A missing optional member is None; null is None.
    #[derive(Debug, PartialEq, JsonText, Serde)]
    struct Profile {
        id: u32,
        nickname: Option<String>,
    }
    for text in [r#"{"id":1}"#, r#"{"id":1,"nickname":null}"#] {
        assert_eq!(same_outcome::<Profile>(text), Some(Profile { id: 1, nickname: None }));
    }
    assert_eq!(same_outcome::<Profile>(r#"{"nickname":"n"}"#), None);

    // Enumerators and named variants.
    for (text, ok) in [
        (r#""active""#, true),
        (r#""Active""#, false),
        ("1", true),
        (r#""1""#, true),
        (r#""active:1""#, true),
        (r#""active:2""#, false),
        ("9", true),
        ("256", false),
    ] {
        assert_eq!(same_outcome::<Status>(text).is_some(), ok, "{text}");
    }
    for (text, ok) in [
        (r#"{"memo":"n"}"#, true),
        (r#"{"Note":"n"}"#, false),
        (r#"{"Pair":[1,-2]}"#, true),
        (r#"{"Pair":[1,-2,3]}"#, false),
        (r#"[1,"n"]"#, false),
        (r#"{"memo":"n","Pair":[1,2]}"#, false),
        (r#"{}"#, false),
    ] {
        assert_eq!(same_outcome::<Action>(text).is_some(), ok, "{text}");
    }

    // Strings and bytes.
    for (text, ok) in [
        (r#""plain""#, true),
        (r#""esc \u00e9 \ud83d\ude00 \n""#, true),
        (r#""lone \ud800""#, false),
        ("\"raw \u{1}\"", false),
        (r#""non\uffffcharacter""#, false),
        ("5", false),
    ] {
        // (`ByKey<T>` is a PSIO type of `T`'s text: std's own `String`,
        // `f64` and `Vec` have serde's impls, not PSIO's.)
        assert_eq!(same_outcome::<ByKey<String>>(text).is_some(), ok, "{text}");
        assert_eq!(same_outcome::<RawString>(text).is_some(), ok, "{text}");
    }
    for (text, ok) in [
        (r#""DEAD""#, true),
        (r#""dead""#, true),
        (r#""DEA""#, false),
        (r#""\u0044EAD""#, false),
        ("[222,173]", false),
    ] {
        assert_eq!(same_outcome::<Wrapper<Vec<u8>>>(&format!(r#"{{"value":{text},"tags":[]}}"#)).is_some(), ok, "{text}");
    }
    // Floats, wide integers, bounds, depth.
    for (text, ok) in [("1", true), ("-0", true), ("1.5e300", true), ("1e999", false), ("\"1\"", false)] {
        assert_eq!(same_outcome::<ByKey<f64>>(text).is_some(), ok, "{text}");
    }
    assert_eq!(serde_json::from_str::<ByKey<f64>>("-0").unwrap().0.to_bits(), (-0.0f64).to_bits());
    for (text, ok) in [
        (r#""340282366920938463463374607431768211455""#, true),
        (r#""340282366920938463463374607431768211456""#, false),
        ("5", true),
        ("9007199254740992", false),
    ] {
        assert_eq!(same_outcome::<Id>(text).is_some(), ok, "{text}");
    }
    for (text, ok) in [(r#""12345678""#, true), (r#""123456789""#, false)] {
        assert_eq!(same_outcome::<BoundedString<8>>(text).is_some(), ok, "{text}");
    }
    let deep = |n: usize| format!("{}{}", "[".repeat(n), "]".repeat(n));
    type Deep = ByKey<Vec<Vec<Vec<Vec<u8>>>>>;
    assert!(same_outcome::<Deep>(&deep(3)).is_some());
    assert!(same_outcome::<Deep>(&deep(5)).is_none());
    assert!(same_outcome::<Wrapper<u8>>(&format!(r#"{{"value":1,"tags":[],"x":{}}}"#, deep(60))).is_some());
    assert!(same_outcome::<Wrapper<u8>>(&format!(r#"{{"value":1,"tags":[],"x":{}}}"#, deep(70))).is_none());
    assert!(same_outcome::<Wrapper<u8>>(&format!(r#"{{"value":1,"tags":[],"x":{}}}"#, deep(200))).is_none());
}

#[test]
fn serde_json_spells_some_floats_and_escapes_its_own_way() {
    // The same digits; each side reads the other's text.
    for (value, psio_text, serde_text) in [
        (0.25f64, "0.25", "0.25"),
        (1.0, "1", "1"),
        (-3.0, "-3", "-3"),
        (1e21, "1e+21", "1e+21"),
        (0.1, "0.1", "0.1"),
        (1e-7, "1e-07", "1e-7"),
        (1.5e-5, "1.5e-05", "0.000015"),
        (-0.0, "-0", "-0.0"),
        (1e15, "1e+15", "1000000000000000.0"),
        (123456789012345680000.0, "123456789012345683968", "1.2345678901234568e+20"),
    ] {
        assert_eq!(psio::to_ijson(&value), psio_text);
        assert_eq!(serde_json::to_string(&ByKey(value)).unwrap(), serde_text);
        assert_eq!(psio::from_ijson::<f64>(serde_text).unwrap().to_bits(), value.to_bits());
        assert_eq!(serde_json::from_str::<ByKey<f64>>(psio_text).unwrap().0.to_bits(), value.to_bits());
    }
    let text = RawString::from("\u{8}\u{c}");
    assert_eq!(psio::to_ijson(&text), r#""\u0008\u000c""#);
    assert_eq!(serde_json::to_string(&text).unwrap(), r#""\b\f""#);
    assert_eq!(psio::from_ijson::<RawString>(r#""\b\f""#).unwrap(), text);
}

// A struct that uses serde's own derive and holds PSIO types: those members
// have PSIO's text.
#[derive(Debug, PartialEq, serde::Serialize, serde::Deserialize)]
struct Mixed {
    name: String,
    raw: RawString,
    id: Uint256,
    list: BoundedList<u64, 2>,
    when: TimePointSec,
    balance: Balance,
    #[serde(with = "psio::serde_bridge")]
    amount: u64,
    #[serde(with = "psio::serde_bridge")]
    blob: Vec<u8>,
}

#[test]
fn psio_types_inside_a_serde_derived_struct_have_psio_text() {
    let mixed = Mixed {
        name: "n".into(),
        raw: RawString::from("r"),
        id: Uint256::from_u64(9),
        list: BoundedList(vec![1, u64::MAX]),
        when: TimePointSec::from_ticks(0),
        balance: Balance { amount: 1, limit: 2 },
        amount: u64::MAX,
        blob: vec![0xab],
    };
    let text = serde_json::to_string(&mixed).unwrap();
    assert_eq!(
        text,
        r#"{"name":"n","raw":"r","id":"9","list":[1,"18446744073709551615"],"when":"1970-01-01T00:00:00Z","balance":{"amount":1,"limit":2},"amount":"18446744073709551615","blob":"AB"}"#
    );
    assert_eq!(serde_json::from_str::<Mixed>(&text).unwrap(), mixed);
    // The bound and the invariant of the members are enforced.
    assert!(serde_json::from_str::<Mixed>(&text.replace(r#"[1,"#, r#"[1,2,"#)).is_err());
    let error = serde_json::from_str::<Mixed>(&text.replace(r#""amount":1,"#, r#""amount":3,"#));
    assert!(error.unwrap_err().to_string().contains("balance 3 exceeds limit 2"));
}

#[test]
fn another_self_describing_format_round_trips() {
    // MessagePack (rmp-serde): a record is a map of string keys.
    let value = everything();
    let packed = rmp_serde::to_vec(&value).unwrap();
    assert_eq!(rmp_serde::from_slice::<Everything>(&packed).unwrap(), value);
    // What it holds is the data model of the IJson text: a map of string
    // keys, read here by a struct of serde's own derive.
    #[derive(Debug, PartialEq, serde::Serialize, serde::Deserialize)]
    struct Plain {
        amount: u64,
        limit: String,
    }
    let packed = rmp_serde::to_vec(&Balance { amount: 1, limit: u64::MAX }).unwrap();
    assert_eq!(
        rmp_serde::from_slice::<Plain>(&packed).unwrap(),
        Plain { amount: 1, limit: "18446744073709551615".into() }
    );
    assert_eq!(packed, rmp_serde::to_vec_named(&Plain { amount: 1, limit: u64::MAX.to_string() }).unwrap());
    // PSIO's acceptance holds there too: the invariant, and a 64-bit
    // integer beyond 2^53 - 1 that is not a string.
    let over = rmp_serde::to_vec(&BTreeMap::from([("amount", 7u64), ("limit", 5)])).unwrap();
    let error = rmp_serde::from_slice::<Balance>(&over).unwrap_err();
    assert!(error.to_string().contains("balance 7 exceeds limit 5"), "{error}");
    let wide = rmp_serde::to_vec(&BTreeMap::from([("amount", 1u64), ("limit", u64::MAX)])).unwrap();
    assert!(rmp_serde::from_slice::<Balance>(&wide).is_err());
    let fine = rmp_serde::to_vec(&BTreeMap::from([("amount", 1u64), ("limit", 5)])).unwrap();
    assert_eq!(rmp_serde::from_slice::<Balance>(&fine).unwrap(), Balance { amount: 1, limit: 5 });
    // A float that reaches the reader as an f64 is read as its text.
    let floats = rmp_serde::to_vec(&ByKey(1e21f64)).unwrap();
    assert_eq!(rmp_serde::from_slice::<ByKey<f64>>(&floats).unwrap().0, 1e21);
}

#[test]
fn a_format_that_is_not_self_describing_writes_but_cannot_read() {
    let balance = Balance { amount: 1, limit: 2 };
    // bincode: a map of two string keys (length 2; "amount" -> 1; "limit" -> 2).
    let bytes = bincode::serialize(&balance).unwrap();
    let mut expected = Vec::new();
    expected.extend(2u64.to_le_bytes());
    for (name, value) in [("amount", 1u64), ("limit", 2)] {
        expected.extend((name.len() as u64).to_le_bytes());
        expected.extend(name.as_bytes());
        expected.extend(value.to_le_bytes());
    }
    assert_eq!(bytes, expected);
    assert!(bincode::deserialize::<Balance>(&bytes).is_err());
    let bytes = postcard::to_allocvec(&balance).unwrap();
    assert!(postcard::from_bytes::<Balance>(&bytes).is_err());
}
