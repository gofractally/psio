//! Nested values, maps and sets, transparent types and invariants through
//! the typed Rust API, where the shared fixtures do not reach: the IR Rust
//! builds for them, the bounded forms, the unordered containers, `ByKey`,
//! and the agreement of the typed codecs with the schema-driven ones.

use psio::json::{self, IJson, Json};
use psio::key::{from_key, key_bytes, to_key, try_to_key, ByKey};
use psio::pssz::schema_ir::{schema_for, AnyValue};
use psio::pssz::{from_pssz, try_to_pssz, 
    auto_pssz_width, dynamic, decode, decode_strict, validate, try_encode, BoundedMap,
    BoundedSet, Checked, Deferred, Nested, Pssz, Pssz16, RawString,
};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

#[derive(
    Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::ToPsszSchema,
)]
struct Args {
    to: String,
    amount: u64,
}

#[derive(
    Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::ToPsszSchema,
)]
struct Capped {
    id: u8,
    note: Nested<RawString, Checked, 8>,
    seen: BoundedSet<u16, 3>,
    limits: BoundedMap<u8, u8, 2>,
}

#[derive(
    Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::ToPsszSchema,
)]
struct Small {
    id: u8,
    note: Nested<RawString, Checked, 8>,
    seen: BoundedSet<u16, 3>,
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn the_ir_rust_builds_for_maps_sets_and_nested_values() {
    let (schema, root) = schema_for::<BTreeMap<String, u32>>().unwrap();
    let AnyValue::Custom(map) = &schema.resolve(&root).unwrap().value else { panic!("Custom") };
    assert_eq!(map.id, "map");
    let AnyValue::List(list) = &map.r#type.value else { panic!("List") };
    let AnyValue::Tuple(entry) = &schema.resolve(&list.r#type).unwrap().value else { panic!("Tuple") };
    assert_eq!(entry.members.len(), 2);
    // The container is not part of the type.
    assert_eq!(schema_for::<HashMap<String, u32>>().unwrap().0, schema);

    // A set of bytes is a set, not a byte string.
    let (schema, root) = schema_for::<BTreeSet<u8>>().unwrap();
    let AnyValue::Custom(set) = &schema.resolve(&root).unwrap().value else { panic!("Custom") };
    assert_eq!(set.id, "set");
    assert!(matches!(set.r#type.value, AnyValue::List(_)));

    let (schema, root) = schema_for::<Nested<Args>>().unwrap();
    assert!(matches!(schema.resolve(&root).unwrap().value, AnyValue::Nested(_)));
    // The policy is the reading program's: not in the schema.
    assert_eq!(schema_for::<Nested<Args, Deferred>>().unwrap().0.types.len(), schema.types.len());

    // Bounds: a BoundedList under the map / set; a member lengthBound on a
    // nested member (it counts the bytes of the inner encoding).
    let (schema, root) = schema_for::<Capped>().unwrap();
    let AnyValue::Object(record) = &schema.resolve(&root).unwrap().value else { panic!("Object") };
    let attributes = |name: &str| {
        let member = record.members.iter().find(|m| m.name == name).unwrap();
        member.attributes.iter().map(|a| (a.name.clone(), a.value.clone())).collect::<Vec<_>>()
    };
    assert_eq!(attributes("note"), [("lengthBound".to_string(), Some("max=8".to_string()))]);
    assert!(attributes("seen").is_empty());
    let node = |name: &str| {
        let member = record.members.iter().find(|m| m.name == name).unwrap();
        schema.resolve(&member.r#type).unwrap().clone()
    };
    let AnyValue::Custom(seen) = node("seen").value else { panic!("Custom") };
    assert!(matches!(&seen.r#type.value, AnyValue::BoundedList(l) if l.maxCount == 3));
    // The schema derives the typed offset width from them.
    // A map entry is a tuple, which has no inferred bound: 4, as typed.
    assert_eq!(dynamic::auto_width(&schema, &root).unwrap(), auto_pssz_width::<Capped>());
    assert_eq!(auto_pssz_width::<Capped>(), 4);
    // Without it the bounds select one-byte offsets: 4 (header) + 1 + (8 + 4) + (6 + 4).
    let (schema, root) = schema_for::<Small>().unwrap();
    assert_eq!(dynamic::auto_width(&schema, &root).unwrap(), auto_pssz_width::<Small>());
    assert_eq!(auto_pssz_width::<Small>(), 1);
    let small = Small {
        id: 1,
        note: Nested::new(&RawString::from("12345678")).unwrap(),
        seen: BoundedSet([3u16, 1, 2].into_iter().collect()),
    };
    let bytes = try_to_pssz(&small).unwrap();
    // Header (3), id, the two one-byte offsets (3 and 3 + 8), the payloads.
    assert_eq!(hex(&bytes), "0301030b".to_string() + "3132333435363738" + "010002000300");
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
    assert_eq!(from_pssz::<Small>(&bytes).unwrap(), small);
}

#[test]
fn the_schema_path_over_the_rust_ir_agrees_with_the_typed_codecs() {
    let value = Capped {
        id: 7,
        note: Nested::new(&RawString::from("note")).unwrap(),
        seen: BoundedSet([513u16, 2].into_iter().collect()),
        limits: BoundedMap([(9u8, 1u8), (2, 2)].into_iter().collect()),
    };
    let (schema, root) = schema_for::<Capped>().unwrap();
    let bytes = try_to_pssz(&value).unwrap();
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
    for (text, schema_text) in [
        (
            psio::to_json(&value),
            dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0,
        ),
        (
            psio::to_ijson(&value),
            dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0,
        ),
    ] {
        assert_eq!(text, schema_text);
        assert_eq!(text, r#"{"id":7,"note":"note","seen":[2,513],"limits":[[2,2],[9,1]]}"#);
        assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, text.as_bytes()).unwrap(), bytes);
        assert_eq!(psio::from_json::<Capped>(&text).unwrap(), value);
    }
    // Over a bound: no encoding, typed or through the schema.
    let long = r#"{"id":7,"note":"123456789","seen":[],"limits":[]}"#;
    assert!(psio::from_json::<Capped>(long).is_err());
    assert!(dynamic::json_to_pssz::<Json>(&schema, &root, long.as_bytes()).is_err());
    let many = r#"{"id":7,"note":"","seen":[1,2,3,4],"limits":[]}"#;
    assert!(psio::from_json::<Capped>(many).is_err());
    assert!(dynamic::json_to_pssz::<Json>(&schema, &root, many.as_bytes()).is_err());
    assert!(try_to_pssz(&Capped {
        note: Nested::unchecked(b"123456789".to_vec()),
        ..value.clone()
    })
    .is_err());

    // The schema-driven key of a value is the typed key.
    type Key = (u32, BTreeMap<String, BTreeSet<u8>>, Nested<Args>);
    let key: Key = (
        7,
        [("b".to_string(), [3u8, 0].into_iter().collect()), ("a".to_string(), BTreeSet::new())]
            .into_iter()
            .collect(),
        Nested::new(&Args { to: "bob".into(), amount: 5 }).unwrap(),
    );
    let (schema, root) = schema_for::<Key>().unwrap();
    let bytes = try_to_pssz(&key).unwrap();
    let value = dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
    assert_eq!(dynamic::to_key(&schema, &root, &value).unwrap(), to_key(&key));
    assert_eq!(from_key::<Key>(&to_key(&key)).unwrap(), key);
    // An explicit width around a nested value: the inner bytes stay in the
    // automatic profile, typed and through the schema.
    let wide = try_encode::<Pssz16, _>(&key).unwrap();
    assert_eq!(dynamic::decode_exact::<Pssz16>(&schema, &root, &wide).unwrap(), value);
    assert_eq!(dynamic::encode::<Pssz16>(&schema, &root, &value).unwrap(), wide);
}

#[test]
fn keys_of_maps_and_sets() {
    // A set of octets has the key of the sorted byte list (string framing).
    let set: HashSet<u8> = [3, 0, 1].into_iter().collect();
    assert_eq!(to_key(&set), to_key(&vec![0u8, 1, 3]));
    assert_eq!(hex(&to_key(&set)), "0001" .to_string() + "01030000");
    // Any other set: 01 + element per element, then 00, ascending.
    let set: BTreeSet<u16> = [513, 2].into_iter().collect();
    assert_eq!(hex(&to_key(&set)), "0100020102010" .to_string() + "0");
    assert_eq!(to_key(&set), to_key(&vec![2u16, 513]));
    // A map is the key of the list of its entries.
    let map: HashMap<String, u32> = [("b".to_string(), 2), ("a".to_string(), 1)].into();
    assert_eq!(to_key(&map), to_key(&vec![("a".to_string(), 1u32), ("b".to_string(), 2)]));
    assert_eq!(from_key::<HashMap<String, u32>>(&to_key(&map)).unwrap(), map);
    // Readers require the order and reject a repeated key.
    let unsorted = to_key(&vec![("b".to_string(), 2u32), ("a".to_string(), 1)]);
    assert_eq!(
        from_key::<BTreeMap<String, u32>>(&unsorted).map_err(|e| e.0),
        Err("key: map entries are not in strictly ascending key order")
    );
    let twice = to_key(&vec![2u16, 2]);
    assert_eq!(
        from_key::<BTreeSet<u16>>(&twice).map_err(|e| e.0),
        Err("key: set elements are not in strictly ascending order")
    );
    let bounded = to_key(&vec![1u16, 2, 3, 4]);
    assert!(from_key::<BoundedSet<u16, 3>>(&bounded).is_err());
    assert!(from_key::<BoundedSet<u16, 4>>(&bounded).is_ok());
}

#[test]
fn by_key_orders_any_key_type_by_its_key_bytes() {
    let mut values = [2.5, f64::NAN, -0.0, f64::NEG_INFINITY, -f64::NAN, 0.0, 1e300].map(ByKey).to_vec();
    values.sort();
    values.dedup();
    let bits: Vec<u64> = values.iter().map(|v| v.0.to_bits()).collect();
    // -NaN, -inf, zero (one of the two), 2.5, 1e300, +NaN.
    assert_eq!(bits.len(), 6);
    assert!(values[0].0.is_nan() && values[0].0.is_sign_negative());
    assert_eq!(values[1].0, f64::NEG_INFINITY);
    assert_eq!(values[2].0, 0.0);
    assert!(values[5].0.is_nan() && values[5].0.is_sign_positive());
    // It is its value in every format.
    assert_eq!(key_bytes(&ByKey(2.5f64)), key_bytes(&2.5f64));
    assert_eq!(try_to_pssz(&ByKey(7u32)).unwrap(), try_to_pssz(&7u32).unwrap());
    assert_eq!(psio::to_json(&ByKey(7u32)), "7");
    assert_eq!(schema_for::<ByKey<u32>>().unwrap(), schema_for::<u32>().unwrap());
    let set: BTreeSet<ByKey<f64>> = values.into_iter().collect();
    let bytes = try_to_pssz(&set).unwrap();
    assert_eq!(decode::<Pssz, BTreeSet<ByKey<f64>>>(&bytes).unwrap().len(), 6);
}

#[test]
fn nested_values_keep_their_bytes_and_validate_as_their_type() {
    let args = Args { to: "bob".into(), amount: 5 };
    let nested = Nested::<Args>::new(&args).unwrap();
    assert_eq!(nested.bytes(), try_to_pssz(&args).unwrap());
    assert_eq!(nested.decode().unwrap(), args);
    assert_eq!(nested.validate(), Ok(false));
    // The same bytes as the byte list holding them.
    assert_eq!(
        try_to_pssz(&nested).unwrap(),
        try_to_pssz(&nested.bytes().to_vec()).unwrap()
    );
    // Adopted bytes are validated; garbage is refused by every reader.
    assert!(Nested::<Args>::checked(vec![1, 2, 3]).is_err());
    let garbage = Nested::<Args>::unchecked(vec![1, 2, 3]);
    let frame = try_to_pssz(&(7u8, garbage.clone())).unwrap();
    assert!(validate::<Pssz, (u8, Nested<Args>)>(&frame).is_err());
    assert!(decode::<Pssz, (u8, Nested<Args>)>(&frame).is_err());
    assert!(from_key::<Nested<Args>>(&to_key(&garbage)).is_err());
    // Deferred: the enclosing reader does not look inside, in any mode; the
    // holder validates before reading through it.
    let later = decode_strict::<Pssz, (u8, Nested<Args, Deferred>)>(&frame).unwrap().1;
    assert_eq!(later.bytes(), [1, 2, 3]);
    assert!(later.validate().is_err() && later.decode().is_err());
    // Order, equality and key are those of the bytes: 256 before 1.
    let a = Nested::<u32>::new(&256).unwrap();
    let b = Nested::<u32>::new(&1).unwrap();
    assert!(a < b && to_key(&a) < to_key(&b));
    assert_eq!(Nested::<u32>::default().decode().unwrap(), 0);
}

// `#[serde(transparent)]` says what `#[pssz(transparent)]` says.
#[derive(
    Debug, Clone, PartialEq, Eq, PartialOrd, Ord, serde::Serialize, serde::Deserialize,
    psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::ToPsszSchema, psio::Key,
)]
#[serde(transparent)]
struct Tag {
    text: String,
}

fn even(value: &Even) -> Result<(), &'static str> {
    if value.n % 2 == 0 { Ok(()) } else { Err("Even: the number is odd") }
}
#[derive(
    Debug, Clone, PartialEq, Eq, PartialOrd, Ord, psio::PsszPack, psio::PsszUnpack, psio::JsonText,
    psio::ToPsszSchema, psio::Key,
)]
#[pssz(transparent, invariant = "even")]
struct Even {
    n: u32,
}

#[test]
fn transparent_types_and_invariants_in_every_format() {
    let tag = Tag { text: "a".into() };
    assert_eq!(try_to_pssz(&tag).unwrap(), b"a");
    assert_eq!(psio::to_json(&tag), r#""a""#);
    assert_eq!(serde_json::to_string(&tag).unwrap(), r#""a""#);
    assert_eq!(to_key(&tag), to_key(&"a".to_string()));
    // A transparent string keys a JSON object.
    let map: BTreeMap<Tag, u32> = [(tag.clone(), 1)].into();
    assert_eq!(psio::to_ijson(&map), r#"{"a":1}"#);
    assert_eq!(psio::from_ijson::<BTreeMap<Tag, u32>>(r#"{"a":1}"#).unwrap(), map);
    // A transparent type with neither a text form nor an invariant is a
    // named alias of its member's type.
    let (schema, root) = schema_for::<Tag>().unwrap();
    assert!(matches!(&schema.resolve(&root).unwrap().value, AnyValue::Custom(c) if c.id == "utf8"));
    assert!(matches!(schema.get("Tag").unwrap().value, AnyValue::Type(_)), "a named alias");

    // An invariant makes the type a custom node under its name, and is
    // enforced by every reader and writer.
    let (schema, root) = schema_for::<Even>().unwrap();
    let AnyValue::Custom(custom) = &schema.resolve(&root).unwrap().value else { panic!("Custom") };
    assert_eq!(custom.id, "Even");
    const ODD: &str = "Even: the number is odd";
    let odd = Even { n: 3 };
    assert_eq!(try_to_pssz(&odd).map_err(|e| e.0), Err(ODD));
    assert_eq!(from_pssz::<Even>(&[3, 0, 0, 0]).map_err(|e| e.0), Err(ODD));
    assert_eq!(validate::<Pssz, Vec<Even>>(&[2, 0, 0, 0, 3, 0, 0, 0]).map_err(|e| e.0), Err(ODD));
    assert_eq!(psio::try_to_json(&odd).map_err(|e| e.what), Err(ODD));
    assert_eq!(psio::from_json::<Even>("3").map_err(|e| e.what), Err(ODD));
    assert_eq!(try_to_key(&odd).map_err(|e| e.0), Err(ODD));
    assert_eq!(from_key::<Even>(&[0, 0, 0, 3]).map_err(|e| e.0), Err(ODD));
    assert_eq!(from_key::<Even>(&[0, 0, 0, 4]), Ok(Even { n: 4 }));
    // The order of a map is defined for every key, valid or not.
    assert_eq!(key_bytes(&odd), [0, 0, 0, 3]);
    // Through the schema: the type's own check, registered under its id.
    let mut handlers = dynamic::CustomHandlers::new();
    handlers.add_invariant_type::<Even>().unwrap();
    assert_eq!(dynamic::validate_pssz(&schema, &root, &[3, 0, 0, 0]), Ok(false));
    assert_eq!(
        dynamic::validate_pssz_with(&schema, &root, &[3, 0, 0, 0], &handlers).map_err(|e| e.what()),
        Err(ODD)
    );
    assert_eq!(dynamic::validate_pssz_with(&schema, &root, &[4, 0, 0, 0], &handlers), Ok(false));
    assert_eq!(
        dynamic::json_to_pssz_with::<Json>(&schema, &root, b"3", &handlers).map_err(|e| e.what()),
        Err(ODD)
    );
}
