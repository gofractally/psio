use psio::pssz::{dynamic as codec, schema_ir::*, *};
use serde_json::json;

fn bits(data: &[u8], count: u64) -> codec::Value {
    codec::Value::Bits {
        bytes: data.into(),
        bit_count: count,
    }
}
fn emit<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F> + ToPsszSchema>(
    name: &str,
    value: T,
    expected: codec::Value,
) {
    let bytes = try_encode::<F, _>(&value).unwrap();
    let (schema, root) = schema_for::<T>().unwrap();
    assert_eq!(
        codec::encode::<F>(&schema, &root, &expected).unwrap(),
        bytes
    );
    assert_eq!(
        codec::decode_exact::<F>(&schema, &root, &bytes).unwrap(),
        expected
    );
    println!(
        "{}",
        json!({"contract":"packed-bits","profile":format!("pssz{}",F::OFFSET_BYTES*8),"name":name,"schema":schema,"root_schema":root,"typed_value":expected,"expected_accept":true,"hex":hex::encode(bytes)})
    );
}
fn reject<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F> + ToPsszSchema>(name: &str, bytes: &[u8]) {
    let (schema, root) = schema_for::<T>().unwrap();
    assert!(decode_strict::<F, T>(bytes).is_err());
    assert!(codec::decode_exact::<F>(&schema, &root, bytes).is_err());
    println!(
        "{}",
        json!({"contract":"packed-bits","profile":format!("pssz{}",F::OFFSET_BYTES*8),"name":name,"schema":schema,"root_schema":root,"typed_value":null,"expected_accept":false,"invalid_fixture_path":"independent-malformed-wire","hex":hex::encode(bytes)})
    );
}
fn profile<F: PsszFormat>() {
    emit::<F, _>("vector_zero", Bitvector::<0>::default(), bits(&[], 0));
    emit::<F, _>("vector_one_false", Bitvector::<1>::default(), bits(&[0], 1));
    emit::<F, _>(
        "vector_one_true",
        Bitvector::<1> { bytes: vec![1] },
        bits(&[1], 1),
    );
    emit::<F, _>(
        "vector_nine",
        Bitvector::<9> {
            bytes: vec![0xa5, 1],
        },
        bits(&[0xa5, 1], 9),
    );
    emit::<F, _>("list_zero_bound", Bitlist::<0>::default(), bits(&[], 0));
    emit::<F, _>("list_empty", Bitlist::<10>::default(), bits(&[], 0));
    emit::<F, _>(
        "list_trailing_zero_three",
        Bitlist::<10> {
            bits: vec![true, false, false],
        },
        bits(&[1], 3),
    );
    emit::<F, _>(
        "list_eight_zero",
        Bitlist::<10> {
            bits: vec![false; 8],
        },
        bits(&[0], 8),
    );
    emit::<F, _>(
        "list_ten_trailing_zero",
        Bitlist::<10> {
            bits: vec![
                true, false, false, false, false, false, false, false, false, false,
            ],
        },
        bits(&[1, 0], 10),
    );
    emit::<F, _>(
        "option_vector_zero_none",
        Option::<Bitvector<0>>::None,
        codec::Value::Optional(None),
    );
    emit::<F, _>(
        "option_vector_zero_some",
        Some(Bitvector::<0>::default()),
        codec::Value::Optional(Some(Box::new(bits(&[], 0)))),
    );
    emit::<F, _>(
        "option_vector_nine_none",
        Option::<Bitvector<9>>::None,
        codec::Value::Optional(None),
    );
    emit::<F, _>(
        "option_vector_nine_some",
        Some(Bitvector::<9> {
            bytes: vec![0xa5, 1],
        }),
        codec::Value::Optional(Some(Box::new(bits(&[0xa5, 1], 9)))),
    );
    emit::<F, _>(
        "option_list_none",
        Option::<Bitlist<10>>::None,
        codec::Value::Optional(None),
    );
    emit::<F, _>(
        "option_list_empty",
        Some(Bitlist::<10>::default()),
        codec::Value::Optional(Some(Box::new(bits(&[], 0)))),
    );
    emit::<F, _>(
        "option_list_trailing_zero",
        Some(Bitlist::<10> {
            bits: vec![true, false, false],
        }),
        codec::Value::Optional(Some(Box::new(bits(&[1], 3)))),
    );
    reject::<F, Bitvector<9>>("bad_vector_short", &[1]);
    reject::<F, Bitvector<9>>("bad_vector_extra", &[1, 1, 0]);
    reject::<F, Bitvector<9>>("bad_vector_unused", &[1, 128]);
    reject::<F, Bitlist<10>>("bad_list_empty", &[]);
    reject::<F, Bitlist<10>>("bad_list_zero", &[0]);
    reject::<F, Bitlist<10>>("bad_list_trailing_zero", &[1, 0]);
    reject::<F, Bitlist<10>>("bad_list_bound", &[0, 8]);
}
fn main() {
    profile::<Pssz8>();
    profile::<Pssz16>();
    profile::<Pssz32>();
}
