use psio::pssz::{schema_ir::*, *};
use psio::{PsszPack, PsszUnpack, ToPsszSchema};

#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
struct Example<T> {
    id: u32,
    text: T,
    next: Option<Box<Example<T>>>,
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
#[pssz(definition_will_not_change)]
struct Fixed {
    flag: bool,
    id: u32,
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
enum Choice {
    Unit,
    Single(u32),
    Pair(u16, String),
    Named { flag: bool },
}
trait Source {
    type Value;
}
struct Provider;
impl Source for Provider {
    type Value = u32;
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
struct Associated<T: Source> {
    value: T::Value,
}

#[test]
fn derives_populate_actual_cpp_ir_with_references_bounds_and_layout_attributes() {
    let (schema, root) = schema_for::<Example<psio::pssz::BoundedString<8>>>().unwrap();
    let AnyValue::Object(object) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Object IR")
    };
    assert_eq!(
        object
            .members
            .iter()
            .map(|m| m.name.as_str())
            .collect::<Vec<_>>(),
        ["id", "text", "next"]
    );
    let AnyValue::Custom(text) = &schema.resolve(&object.members[1].r#type).unwrap().value else {
        panic!("expected string adapter")
    };
    assert_eq!(text.id, "string");
    let AnyValue::BoundedList(bound) = &text.r#type.value else {
        panic!("expected bounded bytes")
    };
    assert_eq!(bound.maxCount, 8);
    let (fixed_schema, fixed_root) = schema_for::<Fixed>().unwrap();
    let AnyValue::Object(fixed) = &fixed_schema.resolve(&fixed_root).unwrap().value else {
        panic!("expected Object")
    };
    assert_eq!(
        fixed.attributes,
        [Attribute {
            name: "definitionWillNotChange".into(),
            value: Some("".into())
        }]
    );
    let bytes = try_to_pssz::<Pssz32, _>(&schema).unwrap();
    assert_eq!(from_pssz::<Pssz32, Schema>(&bytes).unwrap(), schema);
}

#[test]
fn plain_tuples_use_tuple_ir_and_enum_payloads_preserve_extensibility() {
    let (schema, root) = schema_for::<(u32, String)>().unwrap();
    let AnyValue::Tuple(tuple) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Tuple IR")
    };
    assert_eq!(tuple.members.len(), 2);
    let (schema, root) = schema_for::<Choice>().unwrap();
    let AnyValue::Variant(choice) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Variant IR")
    };
    assert_eq!(
        choice
            .members
            .iter()
            .map(|m| m.name.as_str())
            .collect::<Vec<_>>(),
        ["Unit", "Single", "Pair", "Named"]
    );
    assert!(matches!(
        choice.members[0].r#type.value,
        AnyValue::Struct(_)
    ));
    assert!(matches!(choice.members[2].r#type.value, AnyValue::Tuple(_)));
    assert!(matches!(
        choice.members[3].r#type.value,
        AnyValue::Object(_)
    ));
}

#[test]
fn generic_bounds_follow_serialized_associated_fields() {
    let value = Associated::<Provider> { value: 7 };
    let bytes = try_to_pssz::<Pssz32, _>(&value).unwrap();
    assert_eq!(
        from_pssz::<Pssz32, Associated<Provider>>(&bytes)
            .unwrap()
            .value,
        7
    );
    schema_for::<Associated<Provider>>().unwrap();
}

#[test]
fn schema_alias_resolution_rejects_cycles_and_unknown_names() {
    let mut schema = Schema::default();
    schema.types.push(Member {
        name: "cycle".into(),
        r#type: Box::new(AnyType::named("cycle")),
        attributes: vec![],
    });
    assert!(schema.resolve(&schema.types[0].r#type).is_err());
    assert!(schema.resolve(&AnyType::named("missing")).is_err());
}

#[test]
fn schema_builder_reserves_preorder_slots_and_rolls_back_failed_insertions() {
    let (schema, root) = schema_for::<(u32, Vec<Option<String>>)>().unwrap();
    assert_eq!(
        schema
            .types
            .iter()
            .map(|m| m.name.as_str())
            .collect::<Vec<_>>(),
        ["@0", "@1", "@2", "@3", "@4", "@5"]
    );
    assert_eq!(root, AnyType::named("@0"));
    assert!(matches!(schema.types[0].r#type.value, AnyValue::Tuple(_)));
    struct Failing;
    impl ToPsszSchema for Failing {
        fn build_schema(builder: &mut SchemaBuilder) -> psio::pssz::PsszResult<AnyType> {
            builder.insert::<String>()?;
            Err(PsszError("intentional schema fixture failure"))
        }
    }
    let mut builder = SchemaBuilder::new();
    builder.insert::<u8>().unwrap();
    assert!(builder.insert::<Failing>().is_err());
    assert_eq!(builder.insert::<u16>().unwrap(), AnyType::named("@1"));
    let schema = builder.finish();
    assert_eq!(schema.types.len(), 2);
    assert!(matches!(
        schema.types[0].r#type.value,
        AnyValue::Int(Int { bits: 8, .. })
    ));
    assert!(matches!(
        schema.types[1].r#type.value,
        AnyValue::Int(Int { bits: 16, .. })
    ));
}

fn integer(bits: u32) -> AnyType {
    AnyType::new(AnyValue::Int(Int {
        bits,
        isSigned: false,
    }))
}
fn member(name: &str) -> Member {
    Member {
        name: name.into(),
        r#type: Box::new(integer(32)),
        attributes: vec![Attribute {
            name: "visible".into(),
            value: None,
        }],
    }
}
fn ir_sample(name: &str) -> AnyType {
    AnyType::new(match name {
        "Struct" => AnyValue::Struct(Struct {
            members: vec![member("x")],
            attributes: vec![Attribute {
                name: "definitionWillNotChange".into(),
                value: Some("".into()),
            }],
        }),
        "Object" => AnyValue::Object(Object {
            members: vec![member("x")],
            attributes: vec![Attribute {
                name: "doc".into(),
                value: Some("example".into()),
            }],
        }),
        "Array" => AnyValue::Array(Array {
            r#type: Box::new(integer(8)),
            len: 3,
        }),
        "List" => AnyValue::List(List {
            r#type: Box::new(integer(32)),
        }),
        "BoundedList" => AnyValue::BoundedList(psio::pssz::schema_ir::BoundedList {
            r#type: Box::new(integer(16)),
            maxCount: 3,
        }),
        "Option" => AnyValue::Option(Optional {
            r#type: Box::new(integer(64)),
        }),
        "Variant" => AnyValue::Variant(Variant {
            members: vec![member("Left"), member("Right")],
            attributes: vec![],
        }),
        "Tuple" => AnyValue::Tuple(Tuple {
            members: vec![Box::new(integer(16)), Box::new(integer(32))],
        }),
        "Int" => return integer(32),
        "Float" => AnyValue::Float(Float {
            exp: 8,
            mantissa: 24,
        }),
        "FracPack" => AnyValue::FracPack(FracPack {
            r#type: Box::new(integer(8)),
        }),
        "Custom" => AnyValue::Custom(Custom {
            r#type: Box::new(integer(64)),
            id: "account".into(),
        }),
        "Resource" => AnyValue::Resource(Resource {
            name: "handle".into(),
            methods: vec![Func {
                name: "run".into(),
                params: vec![member("value")],
                result: None,
                attributes: vec![],
            }],
            attributes: vec![Attribute {
                name: "capability".into(),
                value: None,
            }],
        }),
        "Type" => AnyValue::Type(Type {
            r#type: "named".into(),
        }),
        other => panic!("unknown actual C++ IR fixture {other}"),
    })
}
fn schema_sample() -> Schema {
    Schema {
        package: Package {
            name: "abi".into(),
            version: "1".into(),
            attributes: vec![Attribute {
                name: "stable".into(),
                value: None,
            }],
        },
        interfaces: vec![Interface {
            name: "api".into(),
            type_names: vec!["named".into()],
            funcs: vec![Func {
                name: "zero".into(),
                params: vec![],
                result: None,
                attributes: vec![],
            }],
            attributes: vec![],
        }],
        worlds: vec![World {
            name: "world".into(),
            imports: vec![UseRef {
                package: "base".into(),
                interface_name: "common".into(),
            }],
            exports: vec!["api".into()],
            attributes: vec![],
        }],
        uses: vec![Use {
            package: "base".into(),
            interface_name: "common".into(),
            version: "1".into(),
            items: vec![UseItem {
                name: "type".into(),
                alias: Some("alias".into()),
            }],
        }],
        types: vec![Member {
            name: "named".into(),
            r#type: Box::new(AnyType::new(AnyValue::Object(Object {
                members: vec![member("x")],
                attributes: vec![],
            }))),
            attributes: vec![Attribute {
                name: "doc".into(),
                value: Some("named type".into()),
            }],
        }],
    }
}

#[test]
fn literal_cpp_ir_alternatives_and_envelope_match_bytes_in_both_directions() {
    let mut rows = 0;
    for line in include_str!("fixtures/pssz-schema-ir-golden.jsonl").lines() {
        let row: serde_json::Value = serde_json::from_str(line).unwrap();
        let bytes = hex::decode(row["hex"].as_str().unwrap()).unwrap();
        let name = row["name"].as_str().unwrap();
        if name == "Schema" {
            let value = schema_sample();
            let json = serde_json::to_value(&value).unwrap();
            assert!(json["interfaces"][0].get("type_names").is_some());
            assert!(json["interfaces"][0].get("typenames").is_none());
            assert_eq!(try_to_pssz::<Pssz32, _>(&value).unwrap(), bytes);
            assert_eq!(from_pssz::<Pssz32, Schema>(&bytes).unwrap(), value);
        } else {
            let value = ir_sample(name);
            assert_eq!(
                try_to_pssz::<Pssz32, _>(&value).unwrap(),
                bytes,
                "IR alternative {name}"
            );
            assert_eq!(from_pssz::<Pssz32, AnyType>(&bytes).unwrap(), value);
        }
        rows += 1;
    }
    assert_eq!(rows, 15);
}

#[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
struct StringKinds {
    text: String,
    bounded_text: BoundedString<8>,
    raw: RawString,
    bounded_raw: BoundedRawString<8>,
    level: Level,
}
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
#[repr(u16)]
enum Level {
    Low = 1,
    High = 2,
}

#[test]
fn schema_distinguishes_utf8_strings_from_raw_strings_and_enums_are_integers() {
    use psio::pssz::dynamic as codec;
    let (schema, root) = schema_for::<StringKinds>().unwrap();
    let AnyValue::Object(object) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Object IR")
    };
    let adapter = |index: usize| {
        let AnyValue::Custom(custom) = &schema.resolve(&object.members[index].r#type).unwrap().value
        else {
            panic!("expected an adapter")
        };
        let bound = match &custom.r#type.value {
            AnyValue::List(_) => None,
            AnyValue::BoundedList(list) => Some(list.maxCount),
            _ => panic!("expected a byte list"),
        };
        (custom.id.clone(), bound)
    };
    // UTF-8 strings use the "string" adapter (as C++ utf8_string does); raw
    // strings are byte lists the IR codecs do not UTF-8-check.
    assert_eq!(adapter(0), ("string".to_string(), None));
    assert_eq!(adapter(1), ("string".to_string(), Some(8)));
    assert_eq!(adapter(2), ("hex".to_string(), None));
    assert_eq!(adapter(3), ("hex".to_string(), Some(8)));
    // A field-less enum is its underlying integer, as in C++.
    let AnyValue::Int(level) = &schema.resolve(&object.members[4].r#type).unwrap().value else {
        panic!("expected Int")
    };
    assert_eq!((level.bits, level.isSigned), (16, false));

    // The schema-driven codec accepts exactly what the typed codec accepts.
    let value = StringKinds {
        text: "\u{e9}".into(),
        bounded_text: BoundedString("ab".into()),
        raw: RawString(vec![0xff]),
        bounded_raw: BoundedRawString(vec![0xc0, 0x80]),
        level: Level::High,
    };
    let bytes = try_to_pssz::<Pssz32, _>(&value).unwrap();
    let dynamic = codec::decode_exact::<Pssz32>(&schema, &root, &bytes).unwrap();
    assert_eq!(codec::encode::<Pssz32>(&schema, &root, &dynamic).unwrap(), bytes);
    // Invalid UTF-8 in the UTF-8 field is rejected by both.
    let mut bad = bytes.clone();
    let at = bad.iter().position(|b| *b == 0xc3).unwrap();
    bad[at] = 0xff;
    assert!(from_pssz::<Pssz32, StringKinds>(&bad).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &root, &bad).is_err());
    // The schema-driven codec has no type-derived width profile yet.
    assert!(codec::encode::<Pssz>(&schema, &root, &dynamic).is_err());
    assert!(codec::decode_exact::<Pssz>(&schema, &root, &bytes).is_err());
}
