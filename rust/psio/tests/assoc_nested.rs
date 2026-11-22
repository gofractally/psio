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

// `#[serde(transparent)]` says what `#[psio(transparent)]` says.
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
#[psio(transparent, invariant = "even")]
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

// ── pssz_evolves_from over maps, sets and nested values ─────────────────────

mod evolution {
    use psio::pssz::{
        from_pssz, pssz_evolves_from, to_pssz, BoundedMap, BoundedSet, Checked, Deferred, Nested,
    };
    use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
    struct V1 {
        a: u32,
    }
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
    struct V2 {
        a: u32,
        b: Option<u8>,
    }
    // Not an evolution of V1: the appended field cannot be absent.
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
    struct Bad {
        a: u32,
        b: u8,
    }
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
    struct Holder1 {
        id: u8,
        by_name: BTreeMap<String, V1>,
        tags: BTreeSet<u16>,
        args: Nested<V1>,
    }
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
    struct Holder2 {
        id: u8,
        by_name: BTreeMap<String, V2>,
        tags: BTreeSet<u16>,
        args: Nested<V2>,
        more: Option<BTreeMap<u8, Nested<V2>>>,
    }

    const _: () = {
        assert!(pssz_evolves_from::<V1, V2>());
        assert!(!pssz_evolves_from::<V2, V1>());
        assert!(!pssz_evolves_from::<V1, Bad>());

        // A map evolves when its value type does; the container is not on
        // the wire.
        assert!(pssz_evolves_from::<BTreeMap<String, V1>, BTreeMap<String, V1>>());
        assert!(pssz_evolves_from::<BTreeMap<String, V1>, BTreeMap<String, V2>>());
        assert!(pssz_evolves_from::<BTreeMap<u32, V1>, HashMap<u32, V2>>());
        assert!(pssz_evolves_from::<BTreeMap<u8, BTreeMap<u8, V1>>, BTreeMap<u8, BTreeMap<u8, V2>>>());
        assert!(pssz_evolves_from::<BTreeMap<u8, Vec<Nested<V1>>>, BTreeMap<u8, Vec<Nested<V2>>>>());
        assert!(pssz_evolves_from::<BoundedMap<u32, V1, 4>, BoundedMap<u32, V2, 4>>());
        assert!(!pssz_evolves_from::<BTreeMap<String, V2>, BTreeMap<String, V1>>());
        assert!(!pssz_evolves_from::<BTreeMap<String, V1>, BTreeMap<String, Bad>>());
        assert!(!pssz_evolves_from::<BTreeMap<u32, u8>, BTreeMap<u32, u16>>());
        // The key type cannot change, not even into a type it evolves into.
        assert!(!pssz_evolves_from::<BTreeMap<u32, V1>, BTreeMap<u64, V1>>());
        assert!(!pssz_evolves_from::<BTreeMap<u32, V1>, BTreeMap<i32, V1>>());
        assert!(!pssz_evolves_from::<BTreeMap<(u8,), u8>, BTreeMap<(u8, Option<u8>), u8>>());
        assert!(pssz_evolves_from::<BTreeMap<(u8, String), u8>, BTreeMap<(u8, String), u8>>());
        // The maximum is part of the type.
        assert!(!pssz_evolves_from::<BoundedMap<u32, V1, 4>, BoundedMap<u32, V1, 8>>());
        assert!(!pssz_evolves_from::<BoundedMap<u32, V1, 4>, BTreeMap<u32, V1>>());
        assert!(!pssz_evolves_from::<BTreeMap<u32, V1>, BoundedMap<u32, V1, 4>>());
        // A map is not its list of entries, in either direction.
        assert!(!pssz_evolves_from::<BTreeMap<u32, V1>, Vec<(u32, V1)>>());
        assert!(!pssz_evolves_from::<Vec<(u32, V1)>, BTreeMap<u32, V1>>());
        assert!(!pssz_evolves_from::<BTreeMap<u32, u8>, BTreeSet<(u32, u8)>>());

        // A set evolves only from a set of the same element type.
        assert!(pssz_evolves_from::<BTreeSet<u32>, BTreeSet<u32>>());
        assert!(pssz_evolves_from::<BTreeSet<String>, HashSet<String>>());
        assert!(pssz_evolves_from::<BoundedSet<u32, 4>, BoundedSet<u32, 4>>());
        assert!(pssz_evolves_from::<BTreeSet<Nested<u32>>, BTreeSet<Nested<u32>>>());
        assert!(!pssz_evolves_from::<BTreeSet<u32>, BTreeSet<u64>>());
        assert!(!pssz_evolves_from::<BTreeSet<(u8,)>, BTreeSet<(u8, Option<u8>)>>());
        assert!(!pssz_evolves_from::<BTreeSet<Nested<u32>>, BTreeSet<Nested<u64>>>());
        assert!(!pssz_evolves_from::<BoundedSet<u32, 4>, BoundedSet<u32, 8>>());
        assert!(!pssz_evolves_from::<BoundedSet<u32, 4>, BTreeSet<u32>>());
        assert!(!pssz_evolves_from::<BTreeSet<u32>, Vec<u32>>());
        assert!(!pssz_evolves_from::<Vec<u32>, BTreeSet<u32>>());

        // A nested value evolves as its inner type does, as a top-level type.
        assert!(pssz_evolves_from::<Nested<V1>, Nested<V1>>());
        assert!(pssz_evolves_from::<Nested<V1>, Nested<V2>>());
        assert!(pssz_evolves_from::<Nested<V1, Checked>, Nested<V2, Deferred>>());
        assert!(pssz_evolves_from::<Option<Nested<V1>>, Option<Nested<V2>>>());
        assert!(pssz_evolves_from::<Nested<Nested<V1>>, Nested<Nested<V2>>>());
        assert!(pssz_evolves_from::<Nested<BTreeMap<u8, V1>>, Nested<BTreeMap<u8, V2>>>());
        assert!(pssz_evolves_from::<Nested<V1, Checked, 64>, Nested<V2, Checked, 64>>());
        assert!(!pssz_evolves_from::<Nested<V2>, Nested<V1>>());
        assert!(!pssz_evolves_from::<Nested<V1>, Nested<Bad>>());
        assert!(!pssz_evolves_from::<Nested<u32>, Nested<u64>>());
        // The byte bound is part of the type.
        assert!(!pssz_evolves_from::<Nested<V1, Checked, 64>, Nested<V1, Checked, 128>>());
        assert!(!pssz_evolves_from::<Nested<V1>, Nested<V1, Checked, 64>>());
        // A nested value is not a byte list.
        assert!(!pssz_evolves_from::<Nested<V1>, Vec<u8>>());
        assert!(!pssz_evolves_from::<Vec<u8>, Nested<V1>>());
        assert!(!pssz_evolves_from::<Nested<V1>, V1>());

        // Members of a record.
        assert!(pssz_evolves_from::<Holder1, Holder2>());
        assert!(!pssz_evolves_from::<Holder2, Holder1>());
    };

    #[test]
    fn an_accepted_evolution_reads_in_both_directions() {
        let old = Holder1 {
            id: 3,
            by_name: [("a".to_string(), V1 { a: 1 }), ("b".to_string(), V1 { a: 2 })].into(),
            tags: [7, 9].into(),
            args: Nested::new(&V1 { a: 5 }).unwrap(),
        };
        let as_new = from_pssz::<Holder2>(&to_pssz(&old)).unwrap();
        assert_eq!(as_new.by_name["b"], V2 { a: 2, b: None });
        assert_eq!(as_new.args.decode().unwrap(), V2 { a: 5, b: None });
        assert_eq!(as_new.more, None);

        let new = Holder2 {
            id: 3,
            by_name: [("a".to_string(), V2 { a: 1, b: Some(8) })].into(),
            tags: [7].into(),
            args: Nested::new(&V2 { a: 5, b: Some(6) }).unwrap(),
            more: Some([(1u8, Nested::new(&V2 { a: 0, b: None }).unwrap())].into()),
        };
        let as_old = from_pssz::<Holder1>(&to_pssz(&new)).unwrap();
        assert_eq!(as_old.by_name["a"], V1 { a: 1 });
        assert_eq!(as_old.tags, [7].into());
        assert_eq!(as_old.args.decode().unwrap(), V1 { a: 5 });
    }
}

// ── Views of maps, sets, nested values and tuples ───────────────────────────

mod views {
    use psio::pssz::{
        decode, encode, from_pssz, to_pssz, validate_pssz, BoundedMap, BoundedSet, Checked,
        Deferred, Nested, Pssz, Pssz16, PsszMutView, PsszView,
    };
    use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

    #[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
    #[psio(definition_will_not_change)]
    struct Point {
        x: u32,
        y: u32,
    }
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
    struct Args {
        to: String,
        amount: u64,
    }
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
    struct Holder {
        id: u8,
        by_name: BTreeMap<String, Point>,
        tags: BTreeSet<u16>,
        args: Nested<Args>,
        limits: BoundedMap<u8, u8, 2>,
        seen: BoundedSet<u16, 3>,
        totals: HashMap<u32, u64>,
        names: HashSet<String>,
        raw: Nested<Args, Deferred>,
        pair: (u8, String),
        deep: BTreeMap<u8, Nested<Vec<Point>, Checked, 64>>,
    }

    fn holder() -> Holder {
        Holder {
            id: 3,
            by_name: [
                ("b".to_string(), Point { x: 3, y: 4 }),
                ("".to_string(), Point { x: 1, y: 2 }),
                ("ab".to_string(), Point { x: 5, y: 6 }),
            ]
            .into(),
            // Key order is not the order of the little-endian bytes: 2
            // (00 02 as a key) is before 513 (02 01).
            tags: [513, 2].into(),
            args: Nested::new(&Args { to: "bob".into(), amount: 7 }).unwrap(),
            limits: BoundedMap([(9u8, 1u8), (4, 2)].into()),
            seen: BoundedSet([7u16, 5].into()),
            totals: [(1u32, 10u64), (0x0100, 20), (2, 30)].into(),
            names: ["z".to_string(), "a".to_string()].into(),
            raw: Nested::new(&Args { to: "eve".into(), amount: 1 }).unwrap(),
            pair: (8, "p".into()),
            deep: [(1u8, Nested::new(&vec![Point { x: 1, y: 1 }, Point { x: 2, y: 2 }]).unwrap())]
                .into(),
        }
    }

    #[test]
    fn read_views_reach_entries_elements_and_inner_values() {
        let value = holder();
        let bytes = to_pssz(&value);
        let view = PsszView::<Holder>::new(&bytes).unwrap();

        // A map: its entries in ascending key order.
        let by_name = view.by_name();
        assert_eq!((by_name.len(), by_name.is_empty()), (3, false));
        let read: Vec<(String, u32, u32)> = (0..by_name.len())
            .map(|i| {
                let entry = by_name.get(i);
                (
                    entry.key().view().unwrap().to_string(),
                    entry.value().x().get(),
                    entry.value().y().get(),
                )
            })
            .collect();
        assert_eq!(read, [("".into(), 1, 2), ("ab".into(), 5, 6), ("b".into(), 3, 4)]);
        assert_eq!(decode::<Pssz, (String, Point)>(by_name.get(2).bytes()).unwrap().0, "b");
        assert!(by_name.try_get(3).is_err());

        let totals = view.totals();
        let read: Vec<(u32, u64)> =
            (0..totals.len()).map(|i| (totals.get(i).key().get(), totals.get(i).value().get())).collect();
        assert_eq!(read, [(1, 10), (2, 30), (0x0100, 20)]);
        let limits = view.limits();
        assert_eq!(limits.len(), 2);
        assert_eq!((limits.get(0).key().get(), limits.get(0).value().get()), (4, 2));
        assert_eq!((limits.get(1).key().get(), limits.get(1).value().get()), (9, 1));

        // A set: its elements in ascending key order.
        let tags = view.tags();
        assert_eq!((tags.len(), tags.get(0).get(), tags.get(1).get()), (2, 2, 513));
        assert!(tags.try_get(2).is_err());
        let seen = view.seen();
        assert_eq!((seen.len(), seen.get(0).get(), seen.get(1).get()), (2, 5, 7));
        let names = view.names();
        assert_eq!(names.len(), 2);
        assert_eq!(names.get(0).view().unwrap(), "a");
        assert_eq!(names.get(1).view().unwrap(), "z");

        // A nested value: the view of the inner value.
        let args = view.args();
        assert_eq!(args.len(), value.args.bytes().len());
        assert_eq!(args.get().to().view().unwrap(), "bob");
        assert_eq!(args.get().amount().get(), 7);
        assert_eq!(args.try_get().unwrap().amount().get(), 7);
        assert_eq!(view.raw().try_get().unwrap().to().view().unwrap(), "eve");
        let inner = view.deep().get(0).value().get();
        assert_eq!((inner.len(), inner.get(1).x().get()), (2, 2));

        // A tuple: its members by position.
        assert_eq!(view.pair().field::<0>().get(), 8);
        assert_eq!(view.pair().field::<1>().view().unwrap(), "p");

        // Empty containers.
        let empty = to_pssz(&BTreeMap::<u8, u8>::new());
        let empty_view = PsszView::<BTreeMap<u8, u8>>::new(&empty).unwrap();
        assert!(empty_view.is_empty() && empty_view.try_get(0).is_err());
        assert!(PsszView::<BTreeSet<u8>>::new(&to_pssz(&BTreeSet::<u8>::new())).unwrap().is_empty());
    }

    #[test]
    fn the_inner_value_of_a_nested_view_is_in_the_automatic_profile() {
        let value = holder();
        let bytes = encode::<Pssz16, _>(&value);
        assert_ne!(bytes, to_pssz(&value));
        let view = PsszView::<Holder, Pssz16>::new(&bytes).unwrap();
        let inner: PsszView<Args, Pssz> = view.args().get();
        assert_eq!(inner.bytes(), to_pssz(&Args { to: "bob".into(), amount: 7 }));
        assert_eq!(inner.amount().get(), 7);
        assert_eq!(view.by_name().get(1).key().view().unwrap(), "ab");
        assert_eq!(view.tags().get(1).get(), 513);
    }

    #[test]
    fn a_deferred_nested_view_validates_on_request() {
        // The bytes of a deferred nested value are not examined with the
        // enclosing value: three bytes that are not an Args.
        let bytes = to_pssz(&Nested::<Args, Deferred>::unchecked(vec![1, 2, 3]));
        let view = PsszView::<Nested<Args, Deferred>>::new(&bytes).unwrap();
        assert_eq!(view.len(), 3);
        assert!(view.try_get().is_err());
        assert!(PsszView::<Nested<Args>>::new(&bytes).is_err());
    }

    #[test]
    fn mutable_views_write_mapped_values_and_inner_values_in_place() {
        let mut value = holder();
        let mut bytes = to_pssz(&value);
        let before = bytes.len();
        {
            let mut writer = PsszMutView::<Holder>::new(&mut bytes).unwrap();

            // The mapped value of an entry; its key is read-only.
            let mut by_name = writer.by_name_mut();
            assert_eq!(by_name.len(), 3);
            let mut entry = by_name.get_mut(1);
            assert_eq!(entry.key().view().unwrap(), "ab");
            assert_eq!(entry.value().x().get(), 5);
            entry.value_mut().x_mut().set(&50).unwrap();
            entry.value_mut().set(&Point { x: 51, y: 60 }).unwrap();
            assert!(by_name.try_get_mut(3).is_err());

            let mut totals = writer.totals_mut();
            let mut last = totals.get_mut(2);
            assert_eq!(last.key().get(), 0x0100);
            last.value_mut().set(&21).unwrap();
            writer.limits_mut().get_mut(0).value_mut().set(&200).unwrap();

            // The inner value of a nested value, and of one inside a map.
            writer.args_mut().get_mut().amount_mut().set(&70).unwrap();
            let mut deep = writer.deep_mut();
            deep.get_mut(0).value_mut().get_mut().get_mut(1).y_mut().set(&22).unwrap();

            // A tuple member.
            writer.pair_mut().field_mut::<0>().set(&80).unwrap();
        }
        value.by_name.insert("ab".into(), Point { x: 51, y: 60 });
        value.totals.insert(0x0100, 21);
        value.limits.0.insert(4, 200);
        value.args = Nested::new(&Args { to: "bob".into(), amount: 70 }).unwrap();
        value.deep.insert(1, Nested::new(&vec![Point { x: 1, y: 1 }, Point { x: 2, y: 22 }]).unwrap());
        value.pair.0 = 80;
        // The frame is the canonical encoding of the modified value.
        assert_eq!(bytes.len(), before);
        validate_pssz::<Holder>(&bytes).unwrap();
        assert_eq!(from_pssz::<Holder>(&bytes).unwrap(), value);
        assert_eq!(bytes, to_pssz(&value));
    }
}
