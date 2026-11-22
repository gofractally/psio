use codec::Value;
use psio::pssz::{dynamic as codec, schema_ir::*, *};

fn value(
    schema: &Schema,
    ty: &AnyType,
    json: &serde_json::Value,
    explicit: Option<&serde_json::Value>,
) -> Value {
    match &schema.resolve(ty).unwrap().value {
        AnyValue::Int(i) if i.isSigned => Value::Signed(json.to_string().parse().unwrap()),
        AnyValue::Int(_) => Value::Unsigned(json.to_string().parse().unwrap()),
        AnyValue::Custom(c) if c.id == "bool" => Value::Bool(json.as_bool().unwrap()),
        AnyValue::Custom(c) if c.id == "string" => Value::String(json.as_str().unwrap().into()),
        AnyValue::Option(o) => {
            if let Some(tagged) = explicit {
                if tagged.get("None").is_some() {
                    return Value::Optional(None);
                }
                let inner = tagged.get("Some").expect("tagged optional");
                let nested = matches!(
                    schema.resolve(&o.r#type).unwrap().value,
                    AnyValue::Option(_)
                )
                .then_some(inner);
                return Value::Optional(Some(Box::new(value(schema, &o.r#type, inner, nested))));
            }
            if json.is_null() {
                Value::Optional(None)
            } else {
                Value::Optional(Some(Box::new(value(schema, &o.r#type, json, None))))
            }
        }
        AnyValue::Object(o) => Value::Record(
            o.members
                .iter()
                .map(|m| {
                    (
                        m.name.clone(),
                        value(schema, &m.r#type, &json[&m.name], None),
                    )
                })
                .collect(),
        ),
        AnyValue::Struct(s) => Value::Record(
            s.members
                .iter()
                .map(|m| {
                    (
                        m.name.clone(),
                        value(schema, &m.r#type, &json[&m.name], None),
                    )
                })
                .collect(),
        ),
        AnyValue::Tuple(t) => Value::Sequence(
            t.members
                .iter()
                .enumerate()
                .map(|(i, ty)| value(schema, ty, &json[i], None))
                .collect(),
        ),
        AnyValue::List(l) => Value::Sequence(
            json.as_array()
                .unwrap()
                .iter()
                .map(|v| value(schema, &l.r#type, v, None))
                .collect(),
        ),
        other => panic!("unsupported test fixture IR {other:?}"),
    }
}
fn profile<F: PsszFormat>(row: &serde_json::Value) {
    let mut schema: Schema = serde_json::from_value(row["schema"].clone()).unwrap();
    let root: AnyType = serde_json::from_value(row["root_schema"].clone()).unwrap();
    // The fixture was recorded before the IR was versioned: its schemas
    // carry no marker and are refused as they are. They are read under
    // explicit widths (nothing is derived) and their only string is the raw
    // "string" of C++ std::string, as in version 1, so the reader states the
    // version itself.
    assert_eq!(schema.ir_version, None, "{}", row["name"]);
    assert!(codec::decode_exact::<F>(&schema, &root, &[])
        .is_err_and(|e| e.0.contains("no ir_version marker")));
    schema.ir_version = Some(psio::pssz::schema_ir::IR_VERSION);
    let bytes = hex::decode(row["hex"].as_str().unwrap()).unwrap();
    let explicit = (!row["option_value"].is_null()).then_some(&row["option_value"]);
    let expected = value(&schema, &root, &row["value"], explicit);
    if row["expected_accept"] == false {
        assert!(codec::encode::<F>(&schema, &root, &expected).is_err());
        assert!(codec::decode_exact::<F>(&schema, &root, &bytes).is_err());
        return;
    }
    assert_eq!(
        codec::encode::<F>(&schema, &root, &expected).unwrap(),
        bytes,
        "runtime encode {}",
        row["name"]
    );
    assert_eq!(
        codec::decode_exact::<F>(&schema, &root, &bytes).unwrap(),
        expected,
        "runtime decode {}",
        row["name"]
    );
}

#[test]
fn malformed_optional_suffixes_agree_across_material_views_and_actual_ir() {
    #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::ToPsszSchema)]
    struct Old {
        flag: bool,
        name: String,
    }
    #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::ToPsszSchema)]
    struct New {
        flag: bool,
        name: String,
        extra: Option<String>,
    }
    fn check<F: PsszFormat>(bytes: &[u8], schema: &Schema, root: &AnyType) {
        let result = std::panic::catch_unwind(|| {
            let typed = from_pssz_compatible::<F, Old>(bytes);
            let view = psio::pssz_view::validated_view_compatible::<Old, F>(bytes);
            let runtime = codec::decode_compatible::<F>(schema, root, bytes);
            assert_eq!(typed.is_ok(), view.is_ok(), "typed/view: {bytes:02x?}");
            assert_eq!(typed.is_ok(), runtime.is_ok(), "typed/IR: {bytes:02x?}");
            if let (Ok(typed), Ok(view), Ok(runtime)) = (typed, view, runtime) {
                assert_eq!(typed.has_unknown, view.has_unknown);
                assert_eq!(typed.has_unknown, runtime.has_unknown);
                assert_eq!(typed.value.flag, view.value.flag().get());
                assert_eq!(typed.value.name, view.value.name().view().unwrap());
                assert_eq!(
                    runtime.value,
                    Value::Record(vec![
                        ("flag".into(), Value::Bool(typed.value.flag)),
                        ("name".into(), Value::String(typed.value.name)),
                    ])
                );
                assert_eq!(from_pssz_exact::<F, Old>(bytes).is_ok(), !typed.has_unknown);
            }
        });
        assert!(result.is_ok(), "malformed input panicked: {bytes:02x?}");
    }
    fn run<F: PsszFormat>() {
        let (schema, root) = schema_for::<Old>().unwrap();
        let seeds = [
            try_to_pssz::<F, _>(&Old {
                flag: true,
                name: "ab".into(),
            })
            .unwrap(),
            try_to_pssz::<F, _>(&New {
                flag: false,
                name: "a".into(),
                extra: None,
            })
            .unwrap(),
            try_to_pssz::<F, _>(&New {
                flag: true,
                name: "a".into(),
                extra: Some("b".into()),
            })
            .unwrap(),
        ];
        for seed in seeds {
            for len in 0..=seed.len() {
                check::<F>(&seed[..len], &schema, &root);
            }
            for index in 0..seed.len() {
                for byte in [0, 1, 2, 3, 4, 7, 8, 9, 15, 127, 128, 255] {
                    let mut changed = seed.clone();
                    changed[index] = byte;
                    check::<F>(&changed, &schema, &root);
                }
            }
            for suffix in [0, 1, 255] {
                let mut changed = seed.clone();
                changed.push(suffix);
                check::<F>(&changed, &schema, &root);
            }
        }
    }
    run::<Pssz8>();
    run::<Pssz16>();
    run::<Pssz32>();
}

#[test]
fn actual_ir_runtime_codec_matches_all_shared_static_bytes_and_value_states() {
    let mut cases = 0;
    for line in include_str!("fixtures/optional-suffix-shared.jsonl").lines() {
        let row: serde_json::Value = serde_json::from_str(line).unwrap();
        match row["profile"].as_str().unwrap() {
            "pssz8" => profile::<Pssz8>(&row),
            "pssz16" => profile::<Pssz16>(&row),
            "pssz32" => profile::<Pssz32>(&row),
            _ => panic!("unknown profile"),
        }
        cases += 1;
    }
    assert_eq!(cases, 63);
}

#[test]
fn runtime_codec_handles_integer_bounds_and_rejects_legacy_markers() {
    let schema = Schema::default();
    let u8_ty = AnyType::new(AnyValue::Int(Int {
        bits: 8,
        isSigned: false,
    }));
    assert!(codec::encode::<Pssz32>(&schema, &u8_ty, &Value::Unsigned(256)).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &u8_ty, &[1, 2]).is_err());
    let u128_ty = AnyType::new(AnyValue::Int(Int {
        bits: 128,
        isSigned: false,
    }));
    let bytes = codec::encode::<Pssz32>(&schema, &u128_ty, &Value::Unsigned(u128::MAX)).unwrap();
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &u128_ty, &bytes).unwrap(),
        Value::Unsigned(u128::MAX)
    );
    let legacy = AnyType::new(AnyValue::Nested(Nested {
        r#type: Box::new(u8_ty),
    }));
    assert!(codec::encode::<Pssz32>(&schema, &legacy, &Value::Unsigned(1)).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &legacy, &[1]).is_err());

    // Aliases through a recursive optional are not a finite minimum-size
    // definition. Schema analysis has its own monotone recursion budget.
    let mut recursive = Schema::default();
    recursive.types.push(Member {
        name: "recursive".into(),
        r#type: Box::new(AnyType::new(AnyValue::Option(Optional {
            r#type: Box::new(AnyType::named("recursive")),
        }))),
        attributes: vec![],
    });
    let root = AnyType::named("recursive");
    assert!(codec::encode::<Pssz32>(&recursive, &root, &Value::Optional(None)).is_err());
    assert!(codec::decode_exact::<Pssz32>(&recursive, &root, &[]).is_err());
    recursive.types[0].r#type = Box::new(AnyType::new(AnyValue::Struct(Struct {
        members: vec![Member {
            name: "self".into(),
            r#type: Box::new(AnyType::named("recursive")),
            attributes: vec![],
        }],
        attributes: vec![],
    })));
    let optional = AnyType::new(AnyValue::Option(Optional {
        r#type: Box::new(root),
    }));
    assert!(codec::encode::<Pssz32>(&recursive, &optional, &Value::Optional(None)).is_err());
}

#[test]
fn cpp_char_adapter_and_hex_byte_containers_preserve_all_byte_values() {
    let schema = Schema::default();
    let u8_type = AnyType::new(AnyValue::Int(Int {
        bits: 8,
        isSigned: false,
    }));
    let char_type = AnyType::new(AnyValue::Custom(Custom {
        r#type: Box::new(u8_type.clone()),
        id: "char".into(),
    }));
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &char_type, &Value::Unsigned(255)).unwrap(),
        [255]
    );
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &char_type, &[255]).unwrap(),
        Value::Unsigned(255)
    );
    assert!(codec::encode::<Pssz32>(&schema, &char_type, &Value::Unsigned(256)).is_err());
    let bytes_type = AnyType::new(AnyValue::Custom(Custom {
        r#type: Box::new(AnyType::new(AnyValue::List(List {
            r#type: Box::new(char_type),
        }))),
        id: "hex".into(),
    }));
    let value = Value::Bytes(vec![0, 128, 255]);
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &bytes_type, &value).unwrap(),
        [0, 128, 255]
    );
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &bytes_type, &[0, 128, 255]).unwrap(),
        value
    );
    let all: Vec<u8> = (0..=255).collect();
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &bytes_type, &Value::Bytes(all.clone())).unwrap(),
        all
    );
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &bytes_type, &all).unwrap(),
        Value::Bytes(all.clone())
    );
    let AnyValue::Custom(ref custom) = bytes_type.value else {
        unreachable!()
    };
    let AnyValue::List(ref list) = custom.r#type.value else {
        unreachable!()
    };
    for count in [0, 256] {
        let array = AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::Array(Array {
                r#type: list.r#type.clone(),
                len: count,
            }))),
            id: "hex".into(),
        }));
        let bytes = if count == 0 { vec![] } else { all.clone() };
        assert_eq!(
            codec::encode::<Pssz32>(&schema, &array, &Value::Bytes(bytes.clone())).unwrap(),
            bytes
        );
        assert_eq!(
            codec::decode_exact::<Pssz32>(&schema, &array, &bytes).unwrap(),
            Value::Bytes(bytes)
        );
    }
    for byte in 0..=255 {
        let value = Value::Unsigned(byte);
        assert_eq!(
            codec::encode::<Pssz32>(&schema, &list.r#type, &value).unwrap(),
            [byte as u8]
        );
        assert_eq!(
            codec::decode_exact::<Pssz32>(&schema, &list.r#type, &[byte as u8]).unwrap(),
            value
        );
    }
    let bad_bool = AnyType::new(AnyValue::Custom(Custom {
        r#type: Box::new(u8_type),
        id: "bool".into(),
    }));
    assert!(codec::encode::<Pssz32>(&schema, &bad_bool, &Value::Bool(true)).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &bad_bool, &[1]).is_err());
}

#[test]
fn runtime_schema_constraints_are_enforced_or_explicitly_rejected() {
    let schema = Schema::default();
    // An explicit width takes maxDynamicData as the typed Pssz8/16/32 do:
    // the type is accepted and the cap of the top-level type is enforced.
    let capped = |cap: &str| {
        AnyType::new(AnyValue::Object(Object {
            members: vec![],
            attributes: vec![Attribute { name: "maxDynamicData".into(), value: Some(cap.into()) }],
        }))
    };
    let (zero, four) = (capped("0"), capped("4"));
    assert!(codec::encode::<Pssz32>(&schema, &zero, &Value::Record(vec![])).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &zero, &[0; 4]).is_err());
    assert!(codec::decode_compatible::<Pssz32>(&schema, &zero, &[0; 4]).is_err());
    assert_eq!(codec::encode::<Pssz32>(&schema, &four, &Value::Record(vec![])).unwrap(), [0; 4]);
    assert!(codec::decode_exact::<Pssz32>(&schema, &four, &[0; 4]).is_ok());
    // A nested cap is not looked up; a malformed one is still an error.
    let nested = AnyType::new(AnyValue::Option(Optional { r#type: Box::new(zero) }));
    let some = Value::Optional(Some(Box::new(Value::Record(vec![]))));
    assert_eq!(codec::encode::<Pssz32>(&schema, &nested, &some).unwrap(), [0; 4]);
    assert!(codec::encode::<Pssz>(&schema, &nested, &some).is_err());
    assert!(codec::encode::<Pssz32>(&schema, &capped("x"), &Value::Record(vec![])).is_err());
    for name in ["maxFields", "unknownConstraint"] {
        let root = AnyType::new(AnyValue::Object(Object {
            members: vec![],
            attributes: vec![Attribute {
                name: name.into(),
                value: Some("0".into()),
            }],
        }));
        assert!(codec::encode::<Pssz32>(&schema, &root, &Value::Record(vec![])).is_err());
        assert!(codec::decode_exact::<Pssz32>(&schema, &root, &[0; 4]).is_err());
        assert!(codec::decode_compatible::<Pssz32>(&schema, &root, &[0; 4]).is_err());
        // Preflight also rejects unsupported constraints under an absent
        // optional; a schema is never silently accepted by skipping its cap.
        let optional = AnyType::new(AnyValue::Option(Optional {
            r#type: Box::new(root),
        }));
        assert!(codec::encode::<Pssz32>(&schema, &optional, &Value::Optional(None)).is_err());
    }
    let root = AnyType::new(AnyValue::Object(Object {
        members: vec![],
        attributes: vec![Attribute {
            name: "doc".into(),
            value: Some("description".into()),
        }],
    }));
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &Value::Record(vec![])).unwrap(),
        [0; 4]
    );
}

#[test]
fn byte_vectors_traverse_elements_but_strings_are_atomic_at_depth_limit() {
    #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::ToPsszSchema)]
    enum Nest<T> {
        Next(Box<Nest<T>>),
        Leaf(T),
    }
    fn value(mut payload: Value) -> Value {
        payload = Value::Variant {
            selector: 1,
            value: Box::new(payload),
        };
        for _ in 0..63 {
            payload = Value::Variant {
                selector: 0,
                value: Box::new(payload),
            };
        }
        payload
    }
    fn bytes(payload: &[u8]) -> Vec<u8> {
        let mut out = vec![0; 63];
        out.push(1);
        out.extend(payload);
        out
    }
    let (schema, root) = schema_for::<Nest<Vec<u8>>>().unwrap();
    let nonempty = bytes(&[7]);
    assert!(from_pssz_exact::<Pssz32, Nest<Vec<u8>>>(&nonempty).is_err());
    assert!(psio::pssz_view::validated_view_exact::<Nest<Vec<u8>>, Pssz32>(&nonempty).is_err());
    assert!(codec::decode_exact::<Pssz32>(&schema, &root, &nonempty).is_err());
    assert!(codec::encode::<Pssz32>(&schema, &root, &value(Value::Bytes(vec![7]))).is_err());
    let empty = bytes(&[]);
    assert!(from_pssz_exact::<Pssz32, Nest<Vec<u8>>>(&empty).is_ok());
    assert!(codec::decode_exact::<Pssz32>(&schema, &root, &empty).is_ok());
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &value(Value::Bytes(vec![]))).unwrap(),
        empty
    );
    let (schema, root) = schema_for::<Nest<String>>().unwrap();
    let text = bytes(b"a");
    assert!(from_pssz_exact::<Pssz32, Nest<String>>(&text).is_ok());
    assert!(psio::pssz_view::validated_view_exact::<Nest<String>, Pssz32>(&text).is_ok());
    assert!(codec::decode_exact::<Pssz32>(&schema, &root, &text).is_ok());
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &value(Value::String("a".into()))).unwrap(),
        text
    );
}

#[test]
fn runtime_optional_extensions_report_unknown_and_bound_known_payloads() {
    #[derive(psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
    struct Old {
        name: String,
    }
    #[derive(psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
    struct New {
        name: String,
        extra: Option<String>,
    }
    let (schema, root) = schema_for::<Old>().unwrap();
    let bytes = try_to_pssz::<Pssz32, _>(&New {
        name: "a".into(),
        extra: Some("b".into()),
    })
    .unwrap();
    assert!(codec::decode_exact::<Pssz32>(&schema, &root, &bytes).is_err());
    let old = codec::decode_compatible::<Pssz32>(&schema, &root, &bytes).unwrap();
    assert!(old.has_unknown);
    assert_eq!(
        old.value,
        Value::Record(vec![("name".into(), Value::String("a".into()))])
    );
    let (schema, root) = schema_for::<New>().unwrap();
    let bytes = try_to_pssz::<Pssz32, _>(&Old { name: "a".into() }).unwrap();
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &root, &bytes).unwrap(),
        Value::Record(vec![
            ("name".into(), Value::String("a".into())),
            ("extra".into(), Value::Optional(None))
        ])
    );
}

// ── The automatic profile and the JSON path, on hand-built IR ───────────────

mod automatic {
    use super::*;
    use psio::json::{IJson, Json};

    fn attribute(name: &str, value: &str) -> Attribute {
        Attribute { name: name.into(), value: Some(value.into()) }
    }
    fn int(bits: u32, signed: bool) -> AnyType {
        AnyType::new(AnyValue::Int(Int { bits, isSigned: signed }))
    }
    fn list(element: AnyType) -> AnyType {
        AnyType::new(AnyValue::List(List { r#type: Box::new(element) }))
    }
    fn adapter(id: &str, inner: AnyType) -> AnyType {
        AnyType::new(AnyValue::Custom(Custom { r#type: Box::new(inner), id: id.into() }))
    }
    fn option(inner: AnyType) -> AnyType {
        AnyType::new(AnyValue::Option(Optional { r#type: Box::new(inner) }))
    }
    fn member(name: &str, ty: AnyType, attributes: Vec<Attribute>) -> Member {
        Member { name: name.into(), r#type: Box::new(ty), attributes }
    }
    fn object(members: Vec<Member>, attributes: Vec<Attribute>) -> AnyType {
        AnyType::new(AnyValue::Object(Object { members, attributes }))
    }
    fn string() -> AnyType {
        adapter("string", list(int(8, false)))
    }
    fn record(fields: Vec<(&str, Value)>) -> Value {
        Value::Record(fields.into_iter().map(|(n, v)| (n.to_string(), v)).collect())
    }
    fn some(value: Value) -> Value {
        Value::Optional(Some(Box::new(value)))
    }
    fn numbers(values: &[u128]) -> Value {
        Value::Sequence(values.iter().map(|v| Value::Unsigned(*v)).collect())
    }

    #[test]
    fn member_length_bounds_are_enforced_and_select_the_width() {
        let schema = Schema::default();
        // text: 2..=4 bytes; ids: exactly 2 u16; tag: at most 3 bytes when
        // present; blob: at most 2 bytes.
        let root = object(
            vec![
                member("text", string(), vec![attribute("lengthBound", "min=2,max=4")]),
                member("ids", list(int(16, false)), vec![attribute("lengthBound", "exact=2")]),
                member("blob", adapter("hex", list(int(8, false))), vec![attribute("lengthBound", "max=2")]),
                member("tag", option(string()), vec![attribute("lengthBound", "max=3")]),
            ],
            vec![],
        );
        let value = |text: &str, ids: &[u128], blob: &[u8], tag: Option<&str>| {
            record(vec![
                ("text", Value::String(text.into())),
                ("ids", numbers(ids)),
                ("blob", Value::Bytes(blob.to_vec())),
                ("tag", match tag {
                    Some(tag) => some(Value::String(tag.into())),
                    None => Value::Optional(None),
                }),
            ])
        };
        // Bound: header 4 + (4+4) + (2*2+4) + (2+4) + (3+1+4) = 34: 1-byte
        // offsets.
        assert_eq!(codec::auto_width(&schema, &root).unwrap(), 1);
        let good = value("abcd", &[1, 2], &[9, 8], Some("xyz"));
        let bytes = codec::encode::<Pssz>(&schema, &root, &good).unwrap();
        assert_eq!(
            bytes,
            // The optional string can be empty: it carries a selector byte.
            [4, 4, 8, 12, 14, b'a', b'b', b'c', b'd', 1, 0, 2, 0, 9, 8, 1, b'x', b'y', b'z']
        );
        assert_eq!(codec::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap(), good);
        assert_eq!(codec::validate(&schema, &root, &bytes), Ok(false));
        let text = r#"{"text":"abcd","ids":[1,2],"blob":"0908","tag":"xyz"}"#;
        assert_eq!(codec::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, text);
        assert_eq!(codec::json_to_pssz::<IJson>(&schema, &root, text.as_bytes()).unwrap(), bytes);
        assert_eq!(
            codec::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0,
            r#"{"text":"abcd","ids":[1,2],"blob":[9,8],"tag":"xyz"}"#
        );
        // An absent optional satisfies its bound.
        let absent = value("ab", &[1, 2], &[], None);
        let pruned = codec::encode::<Pssz>(&schema, &root, &absent).unwrap();
        assert_eq!(pruned, [3, 3, 5, 9, b'a', b'b', 1, 0, 2, 0]);
        assert_eq!(codec::decode_exact::<Pssz>(&schema, &root, &pruned).unwrap(), absent);

        // Each bound, on every path. The wide profile encodes the frames
        // the automatic one must refuse to read.
        let wide = |members: Vec<Member>| object(members, vec![attribute("maxDynamicData", "255")]);
        let AnyValue::Object(bounded) = &root.value else { unreachable!() };
        let unbounded = wide(
            bounded
                .members
                .iter()
                .map(|m| member(&m.name, (*m.r#type).clone(), vec![]))
                .collect(),
        );
        for (bad, json) in [
            (value("a", &[1, 2], &[], None), r#"{"text":"a","ids":[1,2],"blob":""}"#),
            (value("abcde", &[1, 2], &[], None), r#"{"text":"abcde","ids":[1,2],"blob":""}"#),
            (value("ab", &[1], &[], None), r#"{"text":"ab","ids":[1],"blob":""}"#),
            (value("ab", &[1, 2, 3], &[], None), r#"{"text":"ab","ids":[1,2,3],"blob":""}"#),
            (value("ab", &[1, 2], &[1, 2, 3], None), r#"{"text":"ab","ids":[1,2],"blob":"010203"}"#),
            (
                value("ab", &[1, 2], &[], Some("wxyz")),
                r#"{"text":"ab","ids":[1,2],"blob":"","tag":"wxyz"}"#,
            ),
        ] {
            assert!(codec::encode::<Pssz>(&schema, &root, &bad).is_err(), "{json}");
            assert!(codec::encode::<Pssz8>(&schema, &root, &bad).is_err(), "{json}");
            assert!(codec::value_to_json::<IJson>(&schema, &root, &bad).is_err(), "{json}");
            assert!(codec::json_to_pssz::<IJson>(&schema, &root, json.as_bytes()).is_err(), "{json}");
            // The same frame without the bounds is fine; with them it is
            // not a value of the type.
            let frame = codec::encode::<Pssz>(&schema, &unbounded, &bad).unwrap();
            assert!(codec::json_to_pssz::<IJson>(&schema, &unbounded, json.as_bytes()).is_ok());
            assert!(codec::decode_compatible::<Pssz>(&schema, &root, &frame).is_err(), "{json}");
            assert!(codec::validate(&schema, &root, &frame).is_err(), "{json}");
            assert!(codec::pssz_to_json::<Json>(&schema, &root, &frame).is_err(), "{json}");
        }
        // The explicit profiles enforce the bounds too.
        let narrow = codec::encode::<Pssz8>(&schema, &root, &good).unwrap();
        assert_eq!(narrow, bytes);
        assert_eq!(codec::decode_exact::<Pssz16>(&schema, &root, &codec::encode::<Pssz16>(&schema, &root, &good).unwrap()).unwrap(), good);
    }

    #[test]
    fn malformed_attributes_are_schema_errors_on_every_entry_point() {
        let schema = Schema::default();
        let rejected = |root: &AnyType| {
            assert!(codec::encode::<Pssz>(&schema, root, &record(vec![])).is_err());
            assert!(codec::decode_compatible::<Pssz>(&schema, root, &[0; 4]).is_err());
            assert!(codec::validate(&schema, root, &[0; 4]).is_err());
            assert!(codec::auto_width(&schema, root).is_err());
            assert!(codec::json_to_pssz::<Json>(&schema, root, b"{}").is_err());
            assert!(codec::json_to_value::<IJson>(&schema, root, b"{}").is_err());
            assert!(codec::pssz_to_json::<IJson>(&schema, root, &[0; 4]).is_err());
            assert!(codec::value_to_json::<Json>(&schema, root, &record(vec![])).is_err());
        };
        for bound in ["", "max", "max=", "max=x", "max=01", "max=-1", "max=1,max=2", "most=1",
                      "max=1,", "max=99999999999999999999999", " max=1"] {
            rejected(&object(
                vec![member("m", option(string()), vec![attribute("lengthBound", bound)])],
                vec![],
            ));
        }
        rejected(&object(
            vec![member("m", option(string()), vec![Attribute { name: "lengthBound".into(), value: None }])],
            vec![],
        ));
        for cap in ["", "x", "01", "-1", "1.5", "18446744073709551616"] {
            rejected(&object(vec![], vec![attribute("maxDynamicData", cap)]));
        }
        for form in ["", "Number", "hex"] {
            for name in ["jsonInt", "ijsonInt"] {
                rejected(&object(
                    vec![member("m", option(int(64, false)), vec![attribute(name, form)])],
                    vec![],
                ));
            }
        }
        // Attributes on the wrong site, and unknown ones.
        rejected(&object(vec![], vec![attribute("lengthBound", "max=1")]));
        rejected(&object(vec![], vec![attribute("ijsonName", "x")]));
        rejected(&object(
            vec![member("m", option(string()), vec![attribute("maxDynamicData", "8")])],
            vec![],
        ));
        rejected(&object(
            vec![member("m", option(string()), vec![attribute("ijsonName", "x")])],
            vec![],
        ));
        rejected(&object(
            vec![member("m", option(string()), vec![attribute("definitionWillNotChange", "")])],
            vec![],
        ));

        // What each profile accepts.
        let annotated = object(
            vec![member(
                "m",
                option(int(64, false)),
                vec![
                    attribute("lengthBound", "max=1"),
                    attribute("jsonInt", "string"),
                    attribute("ijsonInt", "number"),
                    attribute("doc", "text"),
                ],
            )],
            vec![],
        );
        assert_eq!(codec::encode::<Pssz32>(&schema, &annotated, &record(vec![])).unwrap(), [0; 4]);
        assert_eq!(codec::encode::<Pssz>(&schema, &annotated, &record(vec![])).unwrap(), [0]);
        let capped = object(vec![], vec![attribute("maxDynamicData", "300"), attribute("maxFields", "4")]);
        assert_eq!(codec::encode::<Pssz>(&schema, &capped, &record(vec![])).unwrap(), [0, 0]);
        assert_eq!(codec::auto_width(&schema, &capped).unwrap(), 2);
        for explicit in [
            codec::encode::<Pssz8>(&schema, &capped, &record(vec![])),
            codec::encode::<Pssz16>(&schema, &capped, &record(vec![])),
            codec::encode::<Pssz32>(&schema, &capped, &record(vec![])),
        ] {
            assert_eq!(explicit.unwrap_err().0, codec::UNSUPPORTED_CONSTRAINT);
        }
    }

    #[test]
    fn caps_rule_the_width_and_are_enforced_at_every_record() {
        let mut schema = Schema::default();
        // Inner: cap 10 (1-byte offsets). Outer holds two: 4 + 2 * (10 + 4)
        // = 32, 1-byte offsets as well.
        schema.types.push(member(
            "Inner",
            object(vec![member("text", string(), vec![])], vec![attribute("maxDynamicData", "10")]),
            vec![],
        ));
        let outer = object(
            vec![
                member("a", AnyType::named("Inner"), vec![]),
                member("b", AnyType::named("Inner"), vec![]),
            ],
            vec![],
        );
        let inner = AnyType::named("Inner");
        assert_eq!(codec::auto_width(&schema, &inner).unwrap(), 1);
        assert_eq!(codec::auto_width(&schema, &outer).unwrap(), 1);
        let text = |s: &str| record(vec![("text", Value::String(s.into()))]);
        // Header, slot and 8 bytes: exactly the cap.
        let at_cap = codec::encode::<Pssz>(&schema, &inner, &text("12345678")).unwrap();
        assert_eq!(at_cap.len(), 10);
        assert_eq!(codec::validate(&schema, &inner, &at_cap), Ok(false));
        assert!(codec::encode::<Pssz>(&schema, &inner, &text("123456789")).is_err());
        assert!(codec::json_to_pssz::<Json>(&schema, &inner, br#"{"text":"123456789"}"#).is_err());
        let mut over = at_cap.clone();
        over.push(b'9');
        assert!(codec::decode_compatible::<Pssz>(&schema, &inner, &over).is_err());
        assert!(codec::validate(&schema, &inner, &over).is_err());
        // Nested: the child's cap applies inside the parent.
        let both = record(vec![("a", text("12345678")), ("b", text(""))]);
        let bytes = codec::encode::<Pssz>(&schema, &outer, &both).unwrap();
        assert_eq!(&bytes[..3], [2, 2, 12]);
        assert_eq!(codec::decode_exact::<Pssz>(&schema, &outer, &bytes).unwrap(), both);
        let bad = record(vec![("a", text("")), ("b", text("123456789"))]);
        assert!(codec::encode::<Pssz>(&schema, &outer, &bad).is_err());
        let mut tail = bytes.clone();
        tail.extend_from_slice(b"..........");
        assert!(codec::decode_compatible::<Pssz>(&schema, &outer, &tail).is_err());
    }

    #[test]
    fn wide_integers_are_bytes_on_the_wire_and_decimal_in_json() {
        let schema = Schema::default();
        let u256 = int(256, false);
        let raw: Vec<u8> = (1..=32).collect();
        for bytes in [
            codec::encode::<Pssz>(&schema, &u256, &Value::Bytes(raw.clone())).unwrap(),
            codec::encode::<Pssz8>(&schema, &u256, &Value::Bytes(raw.clone())).unwrap(),
        ] {
            assert_eq!(bytes, raw);
        }
        assert_eq!(codec::decode_exact::<Pssz>(&schema, &u256, &raw).unwrap(), Value::Bytes(raw.clone()));
        assert!(codec::decode_exact::<Pssz>(&schema, &u256, &raw[..31]).is_err());
        assert!(codec::encode::<Pssz>(&schema, &u256, &Value::Bytes(raw[..31].to_vec())).is_err());
        assert!(codec::encode::<Pssz>(&schema, &u256, &Value::Unsigned(1)).is_err());
        assert!(codec::encode::<Pssz>(&schema, &int(256, true), &Value::Bytes(raw.clone())).is_err());
        // The typed codec agrees on the bytes.
        let typed = Uint256 { limb: [1, 2, 3, 4] };
        let wire = try_to_pssz::<Pssz, _>(&typed).unwrap();
        let (typed_schema, typed_root) = schema_for::<Uint256>().unwrap();
        assert_eq!(schema.resolve(&u256).unwrap(), typed_schema.resolve(&typed_root).unwrap());
        let value = codec::decode_exact::<Pssz>(&typed_schema, &typed_root, &wire).unwrap();
        assert_eq!(codec::encode::<Pssz>(&typed_schema, &typed_root, &value).unwrap(), wire);

        let no_form = |root: &AnyType, bytes: &[u8], text: &[u8]| {
            for error in [
                codec::pssz_to_json::<Json>(&schema, root, bytes).unwrap_err(),
                codec::pssz_to_json::<IJson>(&schema, root, bytes).unwrap_err(),
                codec::json_to_pssz::<Json>(&schema, root, text).unwrap_err(),
                codec::json_to_pssz::<IJson>(&schema, root, text).unwrap_err(),
            ] {
                assert!(error.what().contains("no JSON form"), "{error}");
            }
        };
        // 128-bit and unsigned 256-bit integers are decimal: a bare number
        // under Json, a string under IJson. Any other wide width has no form.
        let decimal = "14528991250861404666834535435384615765856667510756806797353855100662256435713";
        assert_eq!(codec::pssz_to_json::<Json>(&schema, &u256, &raw).unwrap().0, decimal);
        assert_eq!(
            codec::pssz_to_json::<IJson>(&schema, &u256, &raw).unwrap().0,
            format!("\"{decimal}\"")
        );
        assert_eq!(codec::json_to_pssz::<Json>(&schema, &u256, decimal.as_bytes()).unwrap(), raw);
        let mut one = [0u8; 16];
        one[0] = 1;
        assert_eq!(codec::json_to_pssz::<Json>(&schema, &int(128, false), b"1").unwrap(), one);
        assert_eq!(codec::json_to_pssz::<IJson>(&schema, &int(128, true), b"\"1\"").unwrap(), one);
        assert_eq!(codec::pssz_to_json::<IJson>(&schema, &int(128, true), &[0xff; 16]).unwrap().0, "\"-1\"");
        assert_eq!(codec::pssz_to_json::<Json>(&schema, &int(128, true), &[0xff; 16]).unwrap().0, "-1");
        // A signed 256-bit integer is not a type of the codec at all.
        assert!(codec::json_to_pssz::<Json>(&schema, &int(256, true), b"1").is_err());
        let bit = || int(1, false);
        let bitvector = adapter(
            "bitvector",
            AnyType::new(AnyValue::Array(Array { r#type: Box::new(bit()), len: 4 })),
        );
        no_form(&bitvector, &[5], b"[1,0,1,0]");
        let bitlist = adapter(
            "bitlist",
            AnyType::new(AnyValue::BoundedList(psio::pssz::schema_ir::BoundedList {
                r#type: Box::new(bit()),
                maxCount: 4,
            })),
        );
        no_form(&bitlist, &[1], b"[]");
        // A non-finite float has no JSON text in either format.
        let f64_type = AnyType::new(AnyValue::Float(Float { exp: 11, mantissa: 53 }));
        for value in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
            let bytes = value.to_le_bytes();
            assert!(codec::validate(&schema, &f64_type, &bytes).is_ok());
            assert!(codec::pssz_to_json::<Json>(&schema, &f64_type, &bytes).is_err());
            assert!(codec::pssz_to_json::<IJson>(&schema, &f64_type, &bytes).is_err());
        }
        assert!(codec::json_to_pssz::<Json>(&schema, &f64_type, b"1e999").is_err());
        assert_eq!(codec::json_to_pssz::<Json>(&schema, &f64_type, b"1e-999").unwrap(), [0; 8]);
    }

    #[test]
    fn integer_forms_reach_through_containers_and_reset_inside_records() {
        let schema = Schema::default();
        let u64_type = || int(64, false);
        let inner = object(vec![member("n", u64_type(), vec![])], vec![]);
        let root = object(
            vec![
                member("a", list(option(u64_type())), vec![attribute("jsonInt", "string")]),
                member("b", inner.clone(), vec![attribute("jsonInt", "string")]),
                member(
                    "c",
                    AnyType::new(AnyValue::Tuple(Tuple { members: vec![Box::new(u64_type())] })),
                    vec![attribute("jsonInt", "string"), attribute("ijsonInt", "string")],
                ),
                member("d", u64_type(), vec![attribute("ijsonInt", "number")]),
                member("e", adapter("hex", list(int(8, false))), vec![attribute("jsonInt", "string")]),
            ],
            vec![],
        );
        let big = u64::MAX as u128;
        let value = record(vec![
            ("a", Value::Sequence(vec![some(Value::Unsigned(big)), Value::Optional(None)])),
            ("b", record(vec![("n", Value::Unsigned(big))])),
            ("c", numbers(&[big])),
            ("d", Value::Unsigned(big)),
            ("e", Value::Bytes(vec![7, 255])),
        ]);
        let json = r#"{"a":["18446744073709551615",null],"b":{"n":18446744073709551615},"c":[18446744073709551615],"d":18446744073709551615,"e":["7","255"]}"#;
        let ijson = r#"{"a":["18446744073709551615",null],"b":{"n":"18446744073709551615"},"c":["18446744073709551615"],"d":18446744073709551615,"e":"07FF"}"#;
        assert_eq!(codec::value_to_json::<Json>(&schema, &root, &value).unwrap(), json);
        assert_eq!(codec::value_to_json::<IJson>(&schema, &root, &value).unwrap(), ijson);
        assert_eq!(codec::json_to_value::<Json>(&schema, &root, json.as_bytes()).unwrap(), value);
        assert_eq!(codec::json_to_value::<IJson>(&schema, &root, ijson.as_bytes()).unwrap(), value);
        // The other form is refused where one is forced.
        let reject = |text: &str| {
            assert!(codec::json_to_value::<Json>(&schema, &root, text.as_bytes()).is_err(), "{text}");
        };
        reject(&json.replace(r#"["18446744073709551615",null]"#, "[18446744073709551615,null]"));
        reject(&json.replace(r#"{"n":18446744073709551615}"#, r#"{"n":"18446744073709551615"}"#));
        reject(&json.replace(r#""c":[18446744073709551615]"#, r#""c":["18446744073709551615"]"#));
        reject(&json.replace(r#"["7","255"]"#, "[7,255]"));
        assert!(codec::json_to_value::<IJson>(
            &schema,
            &root,
            ijson.replace(r#""d":18446744073709551615"#, r#""d":"18446744073709551615""#).as_bytes()
        )
        .is_err());
        // Integer strings are written literally: no escapes, canonical.
        let one = object(vec![member("n", u64_type(), vec![attribute("jsonInt", "string")])], vec![]);
        for text in [r#"{"n":"\u0031"}"#, r#"{"n":"01"}"#, r#"{"n":"-0"}"#, r#"{"n":"1.0"}"#,
                     r#"{"n":""}"#, r#"{"n":"18446744073709551616"}"#, r#"{"n":" 1"}"#] {
            assert!(codec::json_to_value::<Json>(&schema, &one, text.as_bytes()).is_err(), "{text}");
        }
        assert_eq!(
            codec::json_to_value::<Json>(&schema, &one, br#"{"n":"1","n":"2","zz":[{}]}"#).unwrap(),
            record(vec![("n", Value::Unsigned(2))])
        );
    }

    #[test]
    fn schema_recursion_without_text_is_limited() {
        // Option<Option<...<u8>>>, deeper than the node limit: each level is
        // a schema step that consumes no text.
        let mut schema = Schema::default();
        let mut ty = int(8, false);
        for _ in 0..200 {
            ty = option(ty);
        }
        schema.types.push(member("Deep", ty, vec![]));
        let deep = AnyType::named("Deep");
        assert!(codec::json_to_value::<Json>(&schema, &deep, b"1").is_err());
        assert!(codec::json_to_pssz::<IJson>(&schema, &deep, b"1").is_err());
        assert_eq!(
            codec::json_to_value::<Json>(&schema, &deep, b"null").unwrap(),
            Value::Optional(None)
        );
        // A type that is only itself.
        schema.types.push(member("Loop", option(AnyType::named("Loop")), vec![]));
        let looped = AnyType::named("Loop");
        assert!(codec::json_to_pssz::<Json>(&schema, &looped, b"1").is_err());
        assert!(codec::json_to_pssz::<Json>(&schema, &looped, b"[[[[").is_err());
        assert!(codec::validate(&schema, &looped, &[1; 300]).is_err());
        // A hand-built value deeper than anything decodable.
        let mut value = Value::Unsigned(1);
        for _ in 0..200 {
            value = some(value);
        }
        assert!(codec::encode::<Pssz>(&schema, &deep, &value).is_err());
    }
}
