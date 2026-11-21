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
    let schema: Schema = serde_json::from_value(row["schema"].clone()).unwrap();
    let root: AnyType = serde_json::from_value(row["root_schema"].clone()).unwrap();
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
                assert_eq!(typed.value.name, view.value.name().view());
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
    let legacy = AnyType::new(AnyValue::FracPack(FracPack {
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
    for name in ["maxFields", "maxDynamicData", "unknownConstraint"] {
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
