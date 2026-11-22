use psio::pssz::*;
use serde::Deserialize;

#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
struct AbiEmpty;
#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct AbiUnit;
#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct AbiFixed {
    id: u32,
}
#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
struct AbiScalars {
    flag: bool,
    id: u32,
}
#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
struct AbiNested {
    child: AbiScalars,
    optional: Option<AbiScalars>,
    labels: Vec<String>,
}
#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
enum AbiVariant {
    Unit,
    Scalar(u32),
    Named { code: u16, text: String },
}
#[derive(Debug, PartialEq, Deserialize, PsszPack, PsszUnpack)]
enum TupleVariant {
    Scalar(u32),
    Pair(u16, String),
}

fn check<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F> + PartialEq + std::fmt::Debug>(
    value: T,
    bytes: &[u8],
) {
    assert_eq!(try_encode::<F, _>(&value).unwrap(), bytes);
    assert_eq!(decode::<F, T>(bytes).unwrap(), value);
}
fn fixture<F: PsszFormat>(name: &str, value: serde_json::Value, bytes: &[u8]) {
    match name {
        "empty_record" => check::<F, _>(AbiEmpty, bytes),
        "unit_record" => check::<F, _>(AbiUnit, bytes),
        "fixed_record" => check::<F, _>(serde_json::from_value::<AbiFixed>(value).unwrap(), bytes),
        "scalar_record" => {
            check::<F, _>(serde_json::from_value::<AbiScalars>(value).unwrap(), bytes)
        }
        "optional_record_none" | "optional_record_some" => check::<F, _>(
            serde_json::from_value::<Option<AbiScalars>>(value).unwrap(),
            bytes,
        ),
        "nested_record" => {
            check::<F, _>(serde_json::from_value::<AbiNested>(value).unwrap(), bytes)
        }
        "variant_unit" | "variant_scalar" | "variant_named" => {
            check::<F, _>(serde_json::from_value::<AbiVariant>(value).unwrap(), bytes)
        }
        "zero_argument_tuple" => check::<F, _>((), bytes),
        "positional_tuple" => check::<F, _>(
            serde_json::from_value::<(u32, String)>(value).unwrap(),
            bytes,
        ),
        "variant_tuple" => check::<F, _>(
            serde_json::from_value::<TupleVariant>(value).unwrap(),
            bytes,
        ),
        _ => panic!("unknown shared ABI fixture {name}"),
    }
}

#[test]
fn static_cpp_fixtures_match_explicit_rust_derives_and_profiles() {
    let mut cases = 0;
    for line in include_str!("fixtures/pssz-abi-golden.jsonl").lines() {
        let row: serde_json::Value = serde_json::from_str(line).unwrap();
        let bytes = hex::decode(row["hex"].as_str().unwrap()).unwrap();
        let name = row["name"].as_str().unwrap();
        let value = row["value"].clone();
        match row["profile"].as_str().unwrap() {
            "pssz8" => fixture::<Pssz8>(name, value, &bytes),
            "pssz16" => fixture::<Pssz16>(name, value, &bytes),
            "pssz32" => fixture::<Pssz32>(name, value, &bytes),
            other => panic!("unknown profile {other}"),
        }
        cases += 1;
    }
    assert!(
        cases >= 30,
        "shared corpus must include actual C++ unit/empty records"
    );
}
