use psio::pssz::*;

fn profile<F: PsszFormat>() {
    let mut vector = Bitvector::<9>::default();
    vector.set(0, true);
    vector.set(8, true);
    let bytes = try_to_pssz::<F, _>(&vector).unwrap();
    assert_eq!(bytes, [1, 1]);
    assert_eq!(from_pssz_exact::<F, Bitvector<9>>(&bytes).unwrap(), vector);
    for invalid in [&[1][..], &[1, 1, 0], &[1, 128]] {
        assert!(from_pssz_exact::<F, Bitvector<9>>(invalid).is_err());
        assert!(psio::pssz_view::validated_view_exact::<Bitvector<9>, F>(invalid).is_err());
    }
    vector.bytes[1] = 128;
    let mut out = vec![9, 8];
    assert!(<Bitvector<9> as PsszPack<F>>::pssz_pack_checked(&vector, &mut out).is_err());
    assert_eq!(out, [9, 8]);
    vector.bytes = vec![];
    assert!(try_to_pssz::<F, _>(&vector).is_err());
    let list = Bitlist::<10> {
        bits: vec![true, false, false],
    };
    assert_eq!(try_to_pssz::<F, _>(&list).unwrap(), [9]);
    assert_eq!(from_pssz_exact::<F, Bitlist<10>>(&[9]).unwrap(), list);
    for invalid in [&[][..], &[0], &[1, 0], &[0, 8]] {
        assert!(from_pssz_exact::<F, Bitlist<10>>(invalid).is_err());
        assert!(psio::pssz_view::validated_view_exact::<Bitlist<10>, F>(invalid).is_err());
    }
    let too_long = Bitlist::<10> {
        bits: vec![false; 11],
    };
    assert!(<Bitlist<10> as PsszPack<F>>::pssz_pack_checked(&too_long, &mut out).is_err());
    assert_eq!(out, [9, 8]);
    assert_eq!(
        try_to_pssz::<F, _>(&Some(Bitlist::<10>::default())).unwrap(),
        [1]
    );
    assert!(try_to_pssz::<F, _>(&Option::<Bitlist<10>>::None)
        .unwrap()
        .is_empty());
    assert_eq!(
        try_to_pssz::<F, _>(&Some(Bitvector::<0>::default())).unwrap(),
        [1]
    );
    assert_eq!(
        try_to_pssz::<F, _>(&Option::<Bitvector<0>>::None).unwrap(),
        [0]
    );
}

#[test]
fn checked_static_bits_enforce_exact_spans_bounds_and_canonical_delimiters() {
    profile::<Pssz8>();
    profile::<Pssz16>();
    profile::<Pssz32>();
}

#[test]
fn runtime_bits_use_logical_count_and_preserve_trailing_zero_bits() {
    use psio::pssz::{dynamic as codec, schema_ir::schema_for};
    let (schema, root) = schema_for::<Bitlist<10>>().unwrap();
    let value = codec::Value::Bits {
        bytes: vec![1],
        bit_count: 3,
    };
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &value).unwrap(),
        [9]
    );
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &root, &[9]).unwrap(),
        value
    );
    for invalid in [
        codec::Value::Bits {
            bytes: vec![128],
            bit_count: 3,
        },
        codec::Value::Bits {
            bytes: vec![1, 0],
            bit_count: 11,
        },
        codec::Value::Bytes(vec![9]),
    ] {
        assert!(codec::encode::<Pssz32>(&schema, &root, &invalid).is_err());
    }
    for wire in [&[][..], &[0], &[1, 0], &[0, 8]] {
        assert!(codec::decode_exact::<Pssz32>(&schema, &root, wire).is_err());
    }
    let (schema, root) = schema_for::<Option<Bitlist<10>>>().unwrap();
    let empty = codec::Value::Optional(Some(Box::new(codec::Value::Bits {
        bytes: vec![],
        bit_count: 0,
    })));
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &empty).unwrap(),
        [1]
    );
    assert_eq!(
        codec::decode_exact::<Pssz32>(&schema, &root, &[1]).unwrap(),
        empty
    );
    assert!(
        codec::encode::<Pssz32>(&schema, &root, &codec::Value::Optional(None))
            .unwrap()
            .is_empty()
    );
    let (schema, root) = schema_for::<Option<Bitvector<0>>>().unwrap();
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &empty).unwrap(),
        [1]
    );
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &codec::Value::Optional(None)).unwrap(),
        [0]
    );
}

#[test]
fn packed_bits_are_atomic_nodes_at_depth_64() {
    use psio::pssz::{dynamic as codec, schema_ir::schema_for};
    #[derive(PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
    enum Nest<T> {
        Next(Box<Nest<T>>),
        Leaf(T),
    }
    fn nested<T>(value: T) -> Nest<T> {
        let mut value = Nest::Leaf(value);
        for _ in 0..63 {
            value = Nest::Next(Box::new(value));
        }
        value
    }
    let value = nested(Bitlist::<10> {
        bits: vec![true, false, false],
    });
    let bytes = try_to_pssz::<Pssz32, _>(&value).unwrap();
    assert!(from_pssz_exact::<Pssz32, Nest<Bitlist<10>>>(&bytes).is_ok());
    assert!(psio::pssz_view::validated_view_exact::<Nest<Bitlist<10>>, Pssz32>(&bytes).is_ok());
    let (schema, root) = schema_for::<Nest<Bitlist<10>>>().unwrap();
    let decoded = codec::decode_exact::<Pssz32>(&schema, &root, &bytes).unwrap();
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &decoded).unwrap(),
        bytes
    );
    let value = nested(Bitvector::<9> { bytes: vec![1, 1] });
    let bytes = try_to_pssz::<Pssz32, _>(&value).unwrap();
    assert!(from_pssz_exact::<Pssz32, Nest<Bitvector<9>>>(&bytes).is_ok());
    let (schema, root) = schema_for::<Nest<Bitvector<9>>>().unwrap();
    let decoded = codec::decode_exact::<Pssz32>(&schema, &root, &bytes).unwrap();
    assert_eq!(
        codec::encode::<Pssz32>(&schema, &root, &decoded).unwrap(),
        bytes
    );
}

#[test]
fn exact_shared_bits_fixture_values_roundtrip_with_actual_ir() {
    use psio::pssz::{dynamic as codec, schema_ir::*};
    fn case<F: PsszFormat>(row: serde_json::Value) {
        let mut schema: Schema = serde_json::from_value(row["schema"].clone()).unwrap();
        // The fixture was recorded before the IR was versioned: its schemas
        // carry no marker and are refused as they are. They hold bit types
        // only, whose meaning version 1 did not change, so the reader states
        // the version itself.
        assert_eq!(schema.ir_version, None, "{}", row["name"]);
        assert!(codec::auto_width(&schema, &AnyType::named("@0")).is_err(), "{}", row["name"]);
        schema.ir_version = Some(IR_VERSION);
        let root: AnyType = serde_json::from_value(row["root_schema"].clone()).unwrap();
        let bytes = hex::decode(row["hex"].as_str().unwrap()).unwrap();
        let decoded = codec::decode_exact::<F>(&schema, &root, &bytes);
        assert_eq!(
            decoded.is_ok(),
            row["expected_accept"] == true,
            "{}",
            row["name"]
        );
        if !row["typed_value"].is_null() {
            let value: codec::Value = serde_json::from_value(row["typed_value"].clone()).unwrap();
            assert_eq!(decoded.unwrap(), value);
            assert_eq!(codec::encode::<F>(&schema, &root, &value).unwrap(), bytes);
        }
    }
    let rows: Vec<_> = include_str!("fixtures/pssz-bits-shared.jsonl")
        .lines()
        .map(|line| serde_json::from_str::<serde_json::Value>(line).unwrap())
        .collect();
    assert_eq!(rows.len(), 69);
    for row in rows {
        match row["profile"].as_str().unwrap() {
            "pssz8" => case::<Pssz8>(row),
            "pssz16" => case::<Pssz16>(row),
            "pssz32" => case::<Pssz32>(row),
            _ => panic!("bad bits width"),
        }
    }
}
