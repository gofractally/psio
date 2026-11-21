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
