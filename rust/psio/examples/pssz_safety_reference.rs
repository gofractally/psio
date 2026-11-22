//! Valid explicit-profile bytes: prints the JSON object pinned in
//! tests/fixtures/pssz-safety-explicit.json. The same values declared for
//! the f6 reference revision are tests/probes/pssz_safety_reference_before.rs.
use psio::pssz::*;

#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct Action {
    id: u32,
    name: String,
    active: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
#[psio(definition_will_not_change)]
struct Fixed {
    id: u32,
    active: bool,
}
#[repr(C, packed)]
#[derive(Clone, Copy, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
#[psio(definition_will_not_change)]
struct Packed {
    first: bool,
    second: u8,
}

fn emit<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(name: &str, value: &T) {
    let wire = encode::<F, _>(value);
    let decoded = decode::<F, T>(&wire).unwrap();
    assert_eq!(encode::<F, _>(&decoded), wire);
    let hex: String = wire.iter().map(|b| format!("{b:02x}")).collect();
    println!("\"{name}-{}\":\"{hex}\",", F::OFFSET_BYTES);
}
fn width<F: PsszFormat>() {
    emit::<F, _>("primitive", &0xdeadbeefu32);
    emit::<F, _>("bools", &vec![false, true, false]);
    emit::<F, _>("numbers", &vec![1u64, 256, u64::MAX]);
    emit::<F, _>("fixed-array", &[true, false]);
    emit::<F, _>(
        "strings",
        &vec![String::new(), "hello".into(), "world".into()],
    );
    emit::<F, _>("variable-array", &["a".to_owned(), "bc".to_owned()]);
    emit::<F, _>("some", &Some("value".to_owned()));
    emit::<F, _>("none", &None::<String>);
    emit::<F, _>(
        "record",
        &Action {
            id: 42,
            name: "action".into(),
            active: true,
        },
    );
    emit::<F, _>(
        "record-vector",
        &vec![
            Action {
                id: 1,
                name: String::new(),
                active: false,
            },
            Action {
                id: 2,
                name: "row".into(),
                active: true,
            },
        ],
    );
    emit::<F, _>(
        "dwnc-vector",
        &vec![
            Fixed {
                id: 7,
                active: true,
            },
            Fixed {
                id: 8,
                active: false,
            },
        ],
    );
    emit::<F, _>(
        "dwnc-packed",
        &Packed {
            first: true,
            second: 37,
        },
    );
    emit::<F, _>("boxed-array", &Box::new([1u32, 2, 3]));
}
fn main() {
    println!("{{");
    width::<Pssz8>();
    width::<Pssz16>();
    width::<Pssz32>();
    println!("\"reference_marker\":\"f6-explicit-valid-bytes\"\n}}");
}
