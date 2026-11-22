use psio::pssz::{schema_ir::*, *};
use psio::{PsszPack, PsszUnpack, ToPsszSchema};

#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
struct Example<T> {
    id: u32,
    text: T,
    next: Option<Box<Example<T>>>,
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
#[psio(definition_will_not_change)]
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
    // A bounded UTF-8 string: the "utf8" adapter over bounded bytes.
    assert_eq!(text.id, "utf8");
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
    let bytes = try_encode::<Pssz32, _>(&schema).unwrap();
    assert_eq!(decode::<Pssz32, Schema>(&bytes).unwrap(), schema);
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
    let bytes = try_encode::<Pssz32, _>(&value).unwrap();
    assert_eq!(
        decode::<Pssz32, Associated<Provider>>(&bytes)
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
            Err(PsszError::new("intentional schema fixture failure"))
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
        "Nested" => AnyValue::Nested(psio::pssz::schema_ir::Nested {
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
        // The recorded C++ bytes were written before `ir_version` existed:
        // the appended field is absent, and they still decode.
        ir_version: None,
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
            assert_eq!(try_encode::<Pssz32, _>(&value).unwrap(), bytes);
            assert_eq!(decode::<Pssz32, Schema>(&bytes).unwrap(), value);
        } else {
            let value = ir_sample(name);
            assert_eq!(
                try_encode::<Pssz32, _>(&value).unwrap(),
                bytes,
                "IR alternative {name}"
            );
            assert_eq!(decode::<Pssz32, AnyType>(&bytes).unwrap(), value);
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
    // The schema tells them apart, as C++ utf8_string and std::string do:
    // a UTF-8 string is the "utf8" adapter, a raw string is "string". Byte
    // lists stay "hex".
    assert_eq!(adapter(0), ("utf8".to_string(), None));
    assert_eq!(adapter(1), ("utf8".to_string(), Some(8)));
    assert_eq!(adapter(2), ("string".to_string(), None));
    assert_eq!(adapter(3), ("string".to_string(), Some(8)));
    // A field-less enum is an Enum (its names and values) over its
    // underlying integer, which is what it is on the wire, as in C++ with
    // PSIO_REFLECT_ENUM.
    let AnyValue::Enum(named) = &schema.resolve(&object.members[4].r#type).unwrap().value else {
        panic!("expected Enum")
    };
    let names: Vec<_> = named.members.iter().map(|m| (m.name.as_str(), m.value)).collect();
    assert_eq!(names, [("Low", 1), ("High", 2)]);
    let AnyValue::Int(level) = &schema.wire(&object.members[4].r#type).unwrap().value else {
        panic!("expected Int")
    };
    assert_eq!((level.bits, level.isSigned), (16, false));

    // The schema-driven codec round-trips every value whose strings are
    // UTF-8.
    let value = StringKinds {
        text: "\u{e9}".into(),
        bounded_text: BoundedString("ab".into()),
        raw: RawString("raw \u{e9}".as_bytes().to_vec()),
        bounded_raw: BoundedRawString(b"bytes".to_vec()),
        level: Level::High,
    };
    let bytes = try_encode::<Pssz32, _>(&value).unwrap();
    let dynamic = codec::decode_exact::<Pssz32>(&schema, &root, &bytes).unwrap();
    assert_eq!(codec::encode::<Pssz32>(&schema, &root, &dynamic).unwrap(), bytes);
    // Invalid UTF-8 in the UTF-8 field is rejected by both.
    let mut bad = bytes.clone();
    let at = bad.iter().position(|b| *b == 0xc3).unwrap();
    bad[at] = 0xff;
    assert!(decode::<Pssz32, StringKinds>(&bad).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &root, &bad).is_err());
    // A raw string holding non-UTF-8 bytes is a valid typed value and, the
    // schema describing it as a raw "string", a valid one through the
    // schema too: it decodes and re-encodes unchanged. It has no JSON text.
    let non_utf8 = StringKinds {
        text: "t".into(),
        bounded_text: BoundedString("ab".into()),
        raw: RawString(vec![0xff]),
        bounded_raw: BoundedRawString(vec![0xc0, 0x80]),
        level: Level::High,
    };
    let raw_bytes = try_encode::<Pssz32, _>(&non_utf8).unwrap();
    assert_eq!(decode::<Pssz32, StringKinds>(&raw_bytes).unwrap(), non_utf8);
    let raw_value = codec::decode_exact::<Pssz32>(&schema, &root, &raw_bytes).unwrap();
    assert_eq!(codec::encode::<Pssz32>(&schema, &root, &raw_value).unwrap(), raw_bytes);
    // The automatic (type-derived width) profile gives the typed `Pssz`
    // bytes and reads them back to the same value.
    let auto_bytes = try_to_pssz(&value).unwrap();
    assert_eq!(codec::encode::<Pssz>(&schema, &root, &dynamic).unwrap(), auto_bytes);
    assert_eq!(codec::decode_exact::<Pssz>(&schema, &root, &auto_bytes).unwrap(), dynamic);
    assert_eq!(
        codec::auto_width(&schema, &root).unwrap(),
        psio::pssz::auto_pssz_width::<StringKinds>()
    );
    // The same under the automatic profile; the text writers refuse it.
    let raw_auto = try_to_pssz(&non_utf8).unwrap();
    assert_eq!(from_pssz::<StringKinds>(&raw_auto).unwrap(), non_utf8);
    let raw_value = codec::decode_exact::<Pssz>(&schema, &root, &raw_auto).unwrap();
    assert_eq!(codec::encode::<Pssz>(&schema, &root, &raw_value).unwrap(), raw_auto);
    assert_eq!(codec::validate(&schema, &root, &raw_auto), Ok(false));
    for error in [
        codec::pssz_to_json::<psio::json::Json>(&schema, &root, &raw_auto).unwrap_err(),
        codec::pssz_to_json::<psio::json::IJson>(&schema, &root, &raw_auto).unwrap_err(),
    ] {
        assert_eq!(error.what(), "json: string is not valid UTF-8");
    }
    // Invalid UTF-8 in a "utf8" member stays an error under this profile.
    let mut bad_auto = auto_bytes.clone();
    let at = bad_auto.iter().position(|b| *b == 0xc3).unwrap();
    bad_auto[at] = 0xff;
    assert!(codec::validate(&schema, &root, &bad_auto).is_err());
}

#[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
#[pssz(max_dynamic_data = 300)]
struct Annotated {
    #[json(json_string)]
    a: u64,
    #[json(ijson_number)]
    b: Vec<u64>,
    #[json(json_number, ijson_string)]
    c: Option<u32>,
    d: psio::pssz::BoundedList<u8, 2>,
    e: Uint256,
    f: psio::pssz::BoundedList<u16, 3>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
#[psio(definition_will_not_change)] #[pssz(max_dynamic_data = 16)]
#[json(json_string, ijson_number)]
struct TypeDefaults {
    x: u64,
    #[json(json_number)]
    y: i64,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
enum Named {
    #[psio(name = "uint32")]
    Number(u32),
    Plain(String),
    #[psio(name = "uint8[]")]
    Bytes(Vec<u8>),
}

#[test]
fn derives_record_caps_integer_forms_and_variant_names() {
    use psio::pssz::dynamic as codec;
    let attribute = |name: &str, value: &str| Attribute {
        name: name.into(),
        value: Some(value.into()),
    };
    let (schema, root) = schema_for::<Annotated>().unwrap();
    let AnyValue::Object(object) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Object IR")
    };
    // C++ SchemaBuilder: Attribute{"maxDynamicData", "300"}.
    assert_eq!(object.attributes, [attribute("maxDynamicData", "300")]);
    let attributes: Vec<_> = object.members.iter().map(|m| m.attributes.clone()).collect();
    assert_eq!(attributes[0], [attribute("jsonInt", "string")]);
    assert_eq!(attributes[1], [attribute("ijsonInt", "number")]);
    assert_eq!(attributes[2], [attribute("jsonInt", "number"), attribute("ijsonInt", "string")]);
    assert!(attributes[3..].iter().all(Vec::is_empty));
    // Bounded bytes are "hex" over the bounded list, like Vec<u8>; any other
    // bounded list is the list itself.
    let resolved = |index: usize| &schema.resolve(&object.members[index].r#type).unwrap().value;
    let AnyValue::Custom(bytes) = resolved(3) else {
        panic!("expected the hex adapter")
    };
    assert_eq!(bytes.id, "hex");
    assert!(matches!(
        &schema.resolve(&bytes.r#type).unwrap().value,
        AnyValue::BoundedList(l) if l.maxCount == 2
    ));
    assert!(matches!(resolved(5), AnyValue::BoundedList(l) if l.maxCount == 3));
    let wide = schema.resolve(&object.members[4].r#type).unwrap();
    assert!(matches!(&wide.value, AnyValue::Int(i) if i.bits == 256 && !i.isSigned));

    let (defaults, defaults_root) = schema_for::<TypeDefaults>().unwrap();
    let AnyValue::Object(object) = &defaults.resolve(&defaults_root).unwrap().value else {
        panic!("expected Object IR")
    };
    // The order C++ emits: definitionWillNotChange (value ""), then the cap.
    assert_eq!(
        object.attributes,
        [attribute("definitionWillNotChange", ""), attribute("maxDynamicData", "16")]
    );
    assert_eq!(
        object.members[0].attributes,
        [attribute("jsonInt", "string"), attribute("ijsonInt", "number")]
    );
    assert_eq!(
        object.members[1].attributes,
        [attribute("jsonInt", "number"), attribute("ijsonInt", "number")]
    );

    let (named, named_root) = schema_for::<Named>().unwrap();
    let AnyValue::Variant(variant) = &named.resolve(&named_root).unwrap().value else {
        panic!("expected Variant IR")
    };
    let members: Vec<_> =
        variant.members.iter().map(|m| (m.name.as_str(), m.attributes.clone())).collect();
    assert_eq!(
        members,
        [
            ("Number", vec![attribute("ijsonName", "uint32")]),
            ("Plain", vec![]),
            ("Bytes", vec![attribute("ijsonName", "uint8[]")]),
        ]
    );

    // The generated IR drives the schema path to the typed bytes, and the
    // text follows the recorded forms and names.
    let value = Annotated {
        a: 1,
        b: vec![u64::MAX],
        c: Some(7),
        d: psio::pssz::BoundedList(vec![1, 255]),
        e: Uint256 { limb: [1, 0, 0, 0] },
        f: psio::pssz::BoundedList(vec![5]),
    };
    let bytes = try_to_pssz(&value).unwrap();
    assert_eq!(codec::auto_width(&schema, &root).unwrap(), auto_pssz_width::<Annotated>());
    assert_eq!(codec::auto_width(&schema, &root).unwrap(), 2);
    let dynamic = codec::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
    assert_eq!(codec::encode::<Pssz>(&schema, &root, &dynamic).unwrap(), bytes);
    // The 256-bit member is decimal: bare under Json, a string under IJson.
    assert_eq!(
        codec::pssz_to_json::<psio::json::Json>(&schema, &root, &bytes).unwrap().0,
        r#"{"a":"1","b":[18446744073709551615],"c":7,"d":[1,255],"e":1,"f":[5]}"#
    );
    assert_eq!(
        codec::pssz_to_json::<psio::json::IJson>(&schema, &root, &bytes).unwrap().0,
        r#"{"a":1,"b":[18446744073709551615],"c":"7","d":"01FF","e":"1","f":[5]}"#
    );
    assert_eq!(
        codec::auto_width(&defaults, &defaults_root).unwrap(),
        auto_pssz_width::<TypeDefaults>()
    );
    let typed = TypeDefaults { x: 1, y: -2 };
    let bytes = try_to_pssz(&typed).unwrap();
    assert_eq!(
        codec::pssz_to_json::<psio::json::Json>(&defaults, &defaults_root, &bytes).unwrap().0,
        r#"{"x":"1","y":-2}"#
    );
    assert_eq!(
        codec::pssz_to_json::<psio::json::IJson>(&defaults, &defaults_root, &bytes).unwrap().0,
        r#"{"x":1,"y":-2}"#
    );
    for (value, ijson, json) in [
        (Named::Number(7), r#"{"uint32":7}"#, "[0,7]"),
        (Named::Plain("p".into()), r#"{"Plain":"p"}"#, r#"[1,"p"]"#),
        (Named::Bytes(vec![1, 255]), r#"{"uint8[]":"01FF"}"#, "[2,[1,255]]"),
    ] {
        let bytes = try_to_pssz(&value).unwrap();
        assert_eq!(
            codec::pssz_to_json::<psio::json::IJson>(&named, &named_root, &bytes).unwrap().0,
            ijson
        );
        assert_eq!(
            codec::pssz_to_json::<psio::json::Json>(&named, &named_root, &bytes).unwrap().0,
            json
        );
        assert_eq!(
            codec::json_to_pssz::<psio::json::IJson>(&named, &named_root, ijson.as_bytes()).unwrap(),
            bytes
        );
        assert_eq!(
            codec::json_to_pssz::<psio::json::Json>(&named, &named_root, json.as_bytes()).unwrap(),
            bytes
        );
    }
    // The member name is not an I-JSON name once one is given.
    assert!(codec::json_to_pssz::<psio::json::IJson>(&named, &named_root, br#"{"Number":7}"#)
        .is_err());
}
