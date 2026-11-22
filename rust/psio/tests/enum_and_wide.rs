//! Enums (names in text, the `#[pssz(other)]` catch-all, the Schema IR
//! `Enum`), 128- and 256-bit integers in JSON, and non-finite floats: the
//! Rust-side rules of docs/json.md "Enums" and docs/pssz-spec.md §4.5. The
//! cross-language rows are in json_conformance.rs, schema_conformance.rs and
//! pssz_conformance.rs.

use psio::json::{self, IJson, Json, JsonFormat, JsonText};
use psio::pssz::dynamic::{self, Value};
use psio::pssz::schema_ir::{
    schema_for, AnyType, AnyValue, Enum, Enumerator, Float, Int, Member, Object, Schema,
};
use psio::pssz::{
    from_pssz, from_pssz_compatible, from_pssz_exact, pssz_validate, pssz_validate_compatible,
    pssz_validate_exact, to_pssz, try_to_pssz, Pssz, Pssz16, Pssz32, Pssz8, PsszFormat, Uint256,
};
use psio::pssz_view::{try_pssz_view_of, validated_view_compatible, validated_view_exact};

/// No catch-all: the type holds its three enumerators and nothing else.
#[derive(
    Debug, Clone, Copy, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::PsszView, psio::ToPsszSchema,
)]
#[repr(u8)]
enum Strict {
    #[json(name = "red")]
    Red = 0,
    #[json(name = "green")]
    Green = 1,
    #[json(name = "blue")]
    Blue = 7,
}

/// The same enumerators plus the catch-all.
#[derive(
    Debug, Clone, Copy, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::PsszView, psio::ToPsszSchema,
)]
#[repr(u8)]
enum Color {
    #[json(name = "red")]
    Red = 0,
    #[json(name = "green")]
    Green = 1,
    #[json(name = "blue")]
    Blue = 7,
    #[pssz(other)]
    Unknown(u8),
}

/// Signed and sparse; names are the variant identifiers; the catch-all has
/// another name and is declared first.
#[derive(
    Debug, Clone, Copy, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::PsszView, psio::ToPsszSchema,
)]
#[repr(i32)]
enum Status {
    #[pssz(other)]
    Other(i32) = 1000,
    Pending = -1,
    Active = 2,
    Closed,
}

/// A 64-bit enum: its unnamed values follow the 64-bit integer rule. (Rust
/// numbers the catch-all like any variant, so it cannot follow a variant at
/// the repr's maximum without a discriminant of its own.)
#[derive(Debug, Clone, Copy, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack)]
#[repr(u64)]
enum Big {
    #[pssz(other)]
    Unknown(u64),
    Huge = u64::MAX,
}

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::PsszView,
    psio::ToPsszSchema,
)]
struct Holder {
    color: Color,
    status: Status,
    #[json(json_string, ijson_string)]
    quoted: Color,
    list: Vec<Color>,
    maybe: Option<Status>,
}

fn both<T: JsonText + PartialEq + std::fmt::Debug>(text: &str) -> Result<T, String> {
    let a = json::from_str::<Json, T>(text);
    let b = json::from_str::<IJson, T>(text);
    match (a, b) {
        (Ok(a), Ok(b)) => {
            assert_eq!(a, b, "{text}: the formats read different values");
            Ok(a)
        }
        (Err(a), Err(b)) => {
            assert_eq!(a.what, b.what, "{text}");
            Err(a.what.to_string())
        }
        (a, b) => panic!("{text}: json {a:?} but ijson {b:?}"),
    }
}

// ── PSSZ ────────────────────────────────────────────────────────────────────

/// Every reader entry point of the derive, under one format.
fn pssz_accepts<F: PsszFormat, T>(bytes: &[u8]) -> Vec<bool>
where
    T: psio::pssz::PsszPack<F> + psio::pssz::PsszUnpack<F> + psio::pssz::PsszValidate<F>,
{
    vec![
        from_pssz::<F, T>(bytes).is_ok(),
        from_pssz_exact::<F, T>(bytes).is_ok(),
        from_pssz_compatible::<F, T>(bytes).is_ok(),
        pssz_validate::<F, T>(bytes).is_ok(),
        pssz_validate_exact::<F, T>(bytes).is_ok(),
        pssz_validate_compatible::<F, T>(bytes).is_ok(),
        try_pssz_view_of::<T, F>(bytes).is_ok(),
        validated_view_exact::<T, F>(bytes).is_ok(),
        validated_view_compatible::<T, F>(bytes).is_ok(),
    ]
}

#[test]
fn an_enum_without_the_catch_all_rejects_an_undeclared_value_in_pssz() {
    fn check<F: PsszFormat>() {
        for declared in [0u8, 1, 7] {
            assert!(pssz_accepts::<F, Strict>(&[declared]).iter().all(|ok| *ok), "{declared}");
        }
        for undeclared in [2u8, 6, 8, 255] {
            assert!(
                pssz_accepts::<F, Strict>(&[undeclared]).iter().all(|ok| !*ok),
                "{undeclared}"
            );
            assert_eq!(
                from_pssz::<F, Strict>(&[undeclared]).unwrap_err().0,
                "pssz: value is not an enumerator"
            );
            // In every position.
            assert!(from_pssz::<F, Vec<Strict>>(&[0, undeclared]).is_err());
            assert!(pssz_validate::<F, Vec<Strict>>(&[0, undeclared]).is_err());
            assert!(from_pssz::<F, Option<Strict>>(&[undeclared]).is_err());
            assert!(from_pssz::<F, [Strict; 2]>(&[undeclared, 1]).is_err());
        }
        // Wrong size is an error with or without the catch-all.
        assert!(pssz_accepts::<F, Strict>(&[]).iter().all(|ok| !*ok));
        assert!(pssz_accepts::<F, Strict>(&[0, 0]).iter().all(|ok| !*ok));
        assert!(pssz_accepts::<F, Color>(&[]).iter().all(|ok| !*ok));
        assert!(pssz_accepts::<F, Color>(&[0, 0]).iter().all(|ok| !*ok));
    }
    check::<Pssz>();
    check::<Pssz8>();
    check::<Pssz16>();
    check::<Pssz32>();
}

#[test]
fn the_catch_all_takes_undeclared_values_and_never_a_declared_one() {
    fn check<F: PsszFormat>() {
        for byte in 0..=255u8 {
            assert!(pssz_accepts::<F, Color>(&[byte]).iter().all(|ok| *ok), "{byte}");
            let value = from_pssz::<F, Color>(&[byte]).unwrap();
            // A wire value equal to a declared enumerator is that variant.
            let want = match byte {
                0 => Color::Red,
                1 => Color::Green,
                7 => Color::Blue,
                other => Color::Unknown(other),
            };
            assert_eq!(value, want);
            assert_eq!(from_pssz_exact::<F, Color>(&[byte]).unwrap(), want);
            assert_eq!(from_pssz_compatible::<F, Color>(&[byte]).unwrap().value, want);
            // The bytes are the underlying integer, unchanged.
            assert_eq!(try_to_pssz::<F, _>(&value).unwrap(), [byte]);
            assert_eq!(to_pssz::<F, _>(&value), [byte]);
        }
        for (value, wire) in [
            (Status::Pending, -1i32),
            (Status::Active, 2),
            (Status::Closed, 3),
            (Status::Other(0), 0),
            (Status::Other(1000), 1000),
            (Status::Other(i32::MIN), i32::MIN),
            (Status::Other(i32::MAX), i32::MAX),
        ] {
            let bytes = wire.to_le_bytes();
            assert_eq!(try_to_pssz::<F, _>(&value).unwrap(), bytes);
            assert_eq!(from_pssz::<F, Status>(&bytes).unwrap(), value);
            assert!(pssz_accepts::<F, Status>(&bytes).iter().all(|ok| *ok));
        }
    }
    check::<Pssz>();
    check::<Pssz8>();
    check::<Pssz16>();
    check::<Pssz32>();

    // The view reads the underlying integer whatever it is.
    use ColorPsszAccessors as _;
    use HolderPsszAccessors as _;
    assert_eq!(try_pssz_view_of::<Color, Pssz>(&[9]).unwrap().raw_value(), 9);
    assert_eq!(try_pssz_view_of::<Color, Pssz>(&[7]).unwrap().raw_value(), 7);
    let holder = Holder {
        color: Color::Unknown(200),
        status: Status::Other(-77),
        quoted: Color::Blue,
        list: vec![Color::Unknown(3), Color::Red],
        maybe: Some(Status::Other(5)),
    };
    let bytes = try_to_pssz::<Pssz, _>(&holder).unwrap();
    assert_eq!(from_pssz::<Pssz, Holder>(&bytes).unwrap(), holder);
    let view = validated_view_exact::<Holder, Pssz>(&bytes).unwrap();
    assert_eq!(view.color().raw_value(), 200);
    assert_eq!(view.try_status().unwrap().raw_value(), -77);
    assert_eq!(view.quoted().raw_value(), 7);
}

#[test]
fn encoding_the_catch_all_with_a_declared_value_is_an_error() {
    const WHAT: &str = "pssz: catch-all enum variant holds a declared enumerator";
    fn check<F: PsszFormat>() {
        for declared in [0u8, 1, 7] {
            assert_eq!(try_to_pssz::<F, _>(&Color::Unknown(declared)).unwrap_err().0, WHAT);
            // Nothing is appended by a failed checked pack, in any position.
            assert!(try_to_pssz::<F, _>(&vec![Color::Red, Color::Unknown(declared)]).is_err());
            assert!(try_to_pssz::<F, _>(&Some(Color::Unknown(declared))).is_err());
            assert!(try_to_pssz::<F, _>(&Holder {
                color: Color::Red,
                status: Status::Active,
                quoted: Color::Unknown(declared),
                list: vec![],
                maybe: None,
            })
            .is_err());
        }
        for declared in [-1, 2, 3] {
            assert_eq!(try_to_pssz::<F, _>(&Status::Other(declared)).unwrap_err().0, WHAT);
        }
        assert!(try_to_pssz::<F, _>(&Color::Unknown(2)).is_ok());
        assert!(try_to_pssz::<F, _>(&Status::Other(7)).is_ok());
    }
    check::<Pssz>();
    check::<Pssz8>();
    check::<Pssz16>();
    check::<Pssz32>();

    // The infallible entry points (`to_pssz`, `pssz_pack`, `pssz_size`) have
    // no error to return: they panic, like every other derived type whose
    // value has no encoding. They never write a second representation.
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let packed = std::panic::catch_unwind(|| to_pssz::<Pssz, _>(&Color::Unknown(7)));
    let sized = std::panic::catch_unwind(|| {
        <Color as psio::pssz::PsszPack<Pssz>>::pssz_size(&Color::Unknown(0))
    });
    let nested = std::panic::catch_unwind(|| to_pssz::<Pssz, _>(&vec![Status::Other(2)]));
    std::panic::set_hook(hook);
    assert!(packed.is_err() && sized.is_err() && nested.is_err());

    // JSON: the same rule, in both formats.
    for declared in [0u8, 1, 7] {
        for what in [
            json::to_string::<Json, _>(&Color::Unknown(declared)).unwrap_err().what,
            json::to_string::<IJson, _>(&Color::Unknown(declared)).unwrap_err().what,
            json::to_string::<Json, _>(&vec![Color::Unknown(declared)]).unwrap_err().what,
        ] {
            assert_eq!(what, "json: catch-all enum variant holds a declared enumerator");
        }
    }
    assert!(json::to_string::<IJson, _>(&Status::Other(-1)).is_err());
    assert!(psio::encode::<Json, _>(&Status::Other(3)).is_err());
}

// ── JSON, typed ─────────────────────────────────────────────────────────────

#[test]
fn an_enum_is_written_as_its_name_or_as_the_number_of_an_unnamed_value() {
    fn check<F: JsonFormat>() {
        let text = |v: &dyn Fn() -> Result<String, json::JsonTextError>| v().unwrap();
        assert_eq!(text(&|| json::to_string::<F, _>(&Color::Red)), "\"red\"");
        assert_eq!(text(&|| json::to_string::<F, _>(&Color::Blue)), "\"blue\"");
        assert_eq!(text(&|| json::to_string::<F, _>(&Strict::Green)), "\"green\"");
        assert_eq!(text(&|| json::to_string::<F, _>(&Color::Unknown(2))), "2");
        assert_eq!(text(&|| json::to_string::<F, _>(&Color::Unknown(255))), "255");
        // Without #[json(name)] the name is the variant identifier.
        assert_eq!(text(&|| json::to_string::<F, _>(&Status::Pending)), "\"Pending\"");
        assert_eq!(text(&|| json::to_string::<F, _>(&Status::Closed)), "\"Closed\"");
        assert_eq!(text(&|| json::to_string::<F, _>(&Status::Other(-5))), "-5");
        assert_eq!(text(&|| json::to_string::<F, _>(&Status::Other(i32::MIN))), "-2147483648");
        assert_eq!(text(&|| json::to_string::<F, _>(&Big::Huge)), "\"Huge\"");
        assert_eq!(text(&|| json::to_string::<F, _>(&Big::Unknown(7))), "7");
    }
    check::<Json>();
    check::<IJson>();
    // The number follows the integer rule of the repr and the format ...
    assert_eq!(json::to_string::<Json, _>(&Big::Unknown(1 << 53)).unwrap(), "9007199254740992");
    assert_eq!(json::to_string::<IJson, _>(&Big::Unknown(1 << 53)).unwrap(), "\"9007199254740992\"");
    assert_eq!(json::from_str::<IJson, Big>("\"9007199254740992\"").unwrap(), Big::Unknown(1 << 53));
    assert!(json::from_str::<IJson, Big>("9007199254740992").is_err());
    assert_eq!(json::from_str::<Json, Big>("9007199254740992").unwrap(), Big::Unknown(1 << 53));
    // ... and a member's forced form; a name is a string in any case.
    let holder = Holder {
        color: Color::Unknown(9),
        status: Status::Active,
        quoted: Color::Unknown(9),
        list: vec![Color::Green, Color::Unknown(2)],
        maybe: Some(Status::Other(0)),
    };
    let text = r#"{"color":9,"status":"Active","quoted":"9","list":["green",2],"maybe":0}"#;
    assert_eq!(json::to_string::<Json, _>(&holder).unwrap(), text);
    assert_eq!(json::to_string::<IJson, _>(&holder).unwrap(), text);
    assert_eq!(json::from_str::<Json, Holder>(text).unwrap(), holder);
    assert_eq!(json::from_str::<IJson, Holder>(text).unwrap(), holder);
    let named = Holder { quoted: Color::Blue, ..holder.clone() };
    assert_eq!(
        json::to_string::<IJson, _>(&named).unwrap(),
        r#"{"color":9,"status":"Active","quoted":"blue","list":["green",2],"maybe":0}"#
    );
}

#[test]
fn an_enum_is_read_from_its_name_its_number_the_numeric_string_or_both() {
    // The four forms, in both formats.
    for text in ["\"blue\"", "7", "\"7\"", "\"blue:7\"", " \"blue\" ", "\"\\u0062lue\""] {
        assert_eq!(both::<Color>(text), Ok(Color::Blue), "{text}");
        assert_eq!(both::<Strict>(text), Ok(Strict::Blue), "{text}");
    }
    for text in ["\"red\"", "0", "\"0\"", "\"red:0\""] {
        assert_eq!(both::<Color>(text), Ok(Color::Red), "{text}");
    }
    for text in ["\"Pending\"", "-1", "\"-1\"", "\"Pending:-1\""] {
        assert_eq!(both::<Status>(text), Ok(Status::Pending), "{text}");
    }
    for text in ["\"Closed\"", "3", "\"3\"", "\"Closed:3\""] {
        assert_eq!(both::<Status>(text), Ok(Status::Closed), "{text}");
    }
    assert_eq!(both::<Big>("\"Huge:18446744073709551615\""), Ok(Big::Huge));
    assert_eq!(both::<Big>("\"18446744073709551615\""), Ok(Big::Huge));

    // A declared number is the declared variant, never the catch-all.
    for (text, want) in [("0", Color::Red), ("\"1\"", Color::Green), ("7", Color::Blue)] {
        assert_eq!(both::<Color>(text), Ok(want));
    }
    // An undeclared number goes to the catch-all, as a number or a string.
    for text in ["9", "\"9\""] {
        assert_eq!(both::<Color>(text), Ok(Color::Unknown(9)));
    }
    assert_eq!(both::<Status>("\"-5\""), Ok(Status::Other(-5)));
    assert_eq!(both::<Status>("1000"), Ok(Status::Other(1000)));

    // Each error.
    const UNKNOWN: &str = "json: unknown enumerator name";
    const DISAGREE: &str = "json: enumerator name and number disagree";
    const NUMBER: &str = "json: enum number is not a canonical in-range decimal";
    for (text, what) in [
        ("\"purple\"", UNKNOWN),
        ("\"Red\"", UNKNOWN),
        ("\"\"", UNKNOWN),
        ("\" red\"", UNKNOWN),
        ("\"+1\"", UNKNOWN),
        ("\":0\"", UNKNOWN),
        ("\"purple:0\"", UNKNOWN),
        ("\"Unknown\"", UNKNOWN),
        ("\"Unknown:9\"", UNKNOWN),
        ("\"red:1\"", DISAGREE),
        ("\"blue:0\"", DISAGREE),
        ("\"red:9\"", DISAGREE),
        ("\"red:\"", NUMBER),
        ("\"red:00\"", NUMBER),
        ("\"red:256\"", NUMBER),
        ("\"red:-1\"", NUMBER),
        ("\"red:0:0\"", NUMBER),
        ("\"red: 0\"", NUMBER),
        ("\"07\"", NUMBER),
        ("\"256\"", NUMBER),
        ("\"-1\"", NUMBER),
        ("\"-\"", NUMBER),
        ("\"0:red\"", NUMBER),
        ("\"7 \"", NUMBER),
        ("\"0x7\"", NUMBER),
        ("256", "json: integer is not a canonical in-range decimal"),
        ("-1", "json: integer is not a canonical in-range decimal"),
        ("07", "json: integer is not a canonical in-range decimal"),
        ("1.0", "json: integer has a fraction or exponent"),
    ] {
        assert_eq!(both::<Color>(text), Err(what.to_string()), "{text}");
        assert_eq!(both::<Strict>(text), Err(what.to_string()), "{text}");
    }
    for text in ["true", "null", "[\"red\"]", "{}", "\"red\" 1", "\"red"] {
        assert!(both::<Color>(text).is_err(), "{text}");
    }
    for (text, what) in [
        ("\"Pending:1\"", DISAGREE),
        ("\"-0\"", NUMBER),
        ("\"2147483648\"", NUMBER),
        ("\"Active:2147483648\"", NUMBER),
        ("\"pending\"", UNKNOWN),
        ("\"Other\"", UNKNOWN),
    ] {
        assert_eq!(both::<Status>(text), Err(what.to_string()), "{text}");
    }
    assert!(both::<Status>("-0").is_err());
    assert!(both::<Status>("2147483648").is_err());
}

#[test]
fn an_enum_without_the_catch_all_rejects_an_undeclared_number_in_json() {
    for text in ["2", "\"2\"", "9", "\"255\"", "255"] {
        assert_eq!(both::<Strict>(text), Err("json: value is not an enumerator".to_string()));
        assert!(json::validate::<Json, Strict>(text.as_bytes()).is_err());
        assert!(psio::decode::<IJson, Strict>(text.as_bytes()).is_err());
        // The same text is a value of the type with the catch-all.
        assert!(both::<Color>(text).is_ok());
    }
    // In any position.
    assert!(json::from_str::<Json, Vec<Strict>>("[\"red\",2]").is_err());
    assert!(json::from_str::<Json, Option<Strict>>("2").is_err());
    assert_eq!(json::from_str::<Json, Vec<Strict>>("[\"red\",7]").unwrap(), [Strict::Red, Strict::Blue]);
}

// ── Schema IR ───────────────────────────────────────────────────────────────

fn int(bits: u32, signed: bool) -> AnyType {
    AnyType::new(AnyValue::Int(Int { bits, isSigned: signed }))
}
fn enumeration(under: AnyType, members: &[(&str, u64)]) -> AnyType {
    AnyType::new(AnyValue::Enum(Enum {
        r#type: Box::new(under),
        members: members
            .iter()
            .map(|(name, value)| Enumerator { name: (*name).into(), value: *value })
            .collect(),
    }))
}

#[test]
fn the_derive_emits_an_enum_over_the_underlying_integer() {
    let (schema, root) = schema_for::<Color>().unwrap();
    assert_eq!(root, AnyType::named("Color"));
    // The catch-all is not an enumerator; the names are the JSON names.
    let color = enumeration(int(8, false), &[("red", 0), ("green", 1), ("blue", 7)]);
    assert_eq!(schema.get("Color"), Some(&color));
    assert_eq!(schema.types.len(), 1, "the underlying integer is inline");
    // With and without the catch-all: the same schema.
    let (strict, _) = schema_for::<Strict>().unwrap();
    assert_eq!(strict.get("Strict"), Some(&color));
    // A value is sign-extended to 64 bits.
    let (schema, _) = schema_for::<Status>().unwrap();
    assert_eq!(
        schema.get("Status"),
        Some(&enumeration(int(32, true), &[("Pending", u64::MAX), ("Active", 2), ("Closed", 3)]))
    );
    // `resolve` keeps the Enum (text needs it); `wire` is the integer.
    let (schema, root) = schema_for::<Holder>().unwrap();
    let AnyValue::Object(holder) = &schema.resolve(&root).unwrap().value else { panic!() };
    assert!(matches!(schema.resolve(&holder.members[0].r#type).unwrap().value, AnyValue::Enum(_)));
    assert_eq!(schema.wire(&holder.members[0].r#type).unwrap(), &int(8, false));
    assert_eq!(schema.wire(&holder.members[1].r#type).unwrap(), &int(32, true));
}

#[test]
fn enum_is_the_last_alternative_of_any_type_and_old_schemas_are_unchanged() {
    let color = enumeration(int(8, false), &[("red", 0), ("blue", 7)]);
    let bytes = try_to_pssz::<Pssz32, _>(&color).unwrap();
    // AnyType is a one-member record: header, the member's offset, then the
    // variant, whose first byte is the alternative's index.
    assert_eq!(bytes[8], 14);
    assert_eq!(from_pssz::<Pssz32, AnyType>(&bytes).unwrap(), color);
    assert!(pssz_validate::<Pssz32, AnyType>(&bytes).is_ok());
    let auto = try_to_pssz::<Pssz, _>(&color).unwrap();
    assert_eq!(from_pssz::<Pssz, AnyType>(&auto).unwrap(), color);
    // The alternative before it keeps its index, and nothing follows Enum.
    assert_eq!(try_to_pssz::<Pssz32, _>(&AnyType::named("x")).unwrap()[8], 13);
    let mut beyond = bytes.clone();
    beyond[8] = 15;
    assert!(from_pssz::<Pssz32, AnyType>(&beyond).is_err());
    // Bytes of the C++ struct layout: Enum{type, members}, Enumerator{name, value}.
    let one = enumeration(int(8, false), &[("a", 1)]);
    let enum_bytes = match &one.value {
        AnyValue::Enum(e) => try_to_pssz::<Pssz32, _>(e).unwrap(),
        _ => unreachable!(),
    };
    let int8 = try_to_pssz::<Pssz32, _>(&int(8, false)).unwrap();
    let mut want = vec![];
    want.extend(8u32.to_le_bytes()); // header: fixed region of two offsets
    want.extend(8u32.to_le_bytes()); // -> type
    want.extend((8 + int8.len() as u32).to_le_bytes()); // -> members
    want.extend(&int8);
    want.extend(4u32.to_le_bytes()); // members: one offset
    want.extend(12u32.to_le_bytes()); // Enumerator header: offset + u64
    want.extend(12u32.to_le_bytes()); // -> name
    want.extend(1u64.to_le_bytes()); // value
    want.extend(b"a");
    assert_eq!(enum_bytes, want);
}

#[test]
fn the_schema_driven_codec_treats_an_enum_as_its_underlying_integer() {
    let mut schema = Schema::default();
    schema.types.push(Member {
        name: "Color".into(),
        r#type: Box::new(enumeration(int(8, false), &[("red", 0), ("blue", 7)])),
        attributes: vec![],
    });
    schema.types.push(Member {
        name: "Status".into(),
        r#type: Box::new(enumeration(int(32, true), &[("pending", u64::MAX), ("active", 2)])),
        attributes: vec![],
    });
    let color = AnyType::named("Color");
    let status = AnyType::named("Status");
    // Any in-range value is structurally valid, listed or not.
    for byte in [0u8, 7, 2, 255] {
        assert_eq!(dynamic::validate_pssz(&schema, &color, &[byte]), Ok(false));
        let value = dynamic::decode_exact::<Pssz>(&schema, &color, &[byte]).unwrap();
        assert_eq!(value, Value::Unsigned(byte.into()));
        assert_eq!(dynamic::encode::<Pssz>(&schema, &color, &value).unwrap(), [byte]);
        assert_eq!(dynamic::encode::<Pssz8>(&schema, &color, &value).unwrap(), [byte]);
    }
    assert!(dynamic::validate_pssz(&schema, &color, &[]).is_err());
    assert!(dynamic::validate_pssz(&schema, &color, &[0, 0]).is_err());
    assert!(dynamic::encode::<Pssz>(&schema, &color, &Value::Unsigned(256)).is_err());
    assert!(dynamic::encode::<Pssz>(&schema, &color, &Value::Signed(1)).is_err());
    assert_eq!(
        dynamic::decode_exact::<Pssz>(&schema, &status, &(-5i32).to_le_bytes()).unwrap(),
        Value::Signed(-5)
    );
    // Same layout as the integer in a record: fixed, inline, same width.
    let record = |member: AnyType| {
        AnyType::new(AnyValue::Object(Object {
            members: vec![
                Member { name: "c".into(), r#type: Box::new(member), attributes: vec![] },
                Member { name: "n".into(), r#type: Box::new(int(16, false)), attributes: vec![] },
            ],
            attributes: vec![],
        }))
    };
    let with_enum = record(color.clone());
    let with_int = record(int(8, false));
    let value = Value::Record(vec![
        ("c".into(), Value::Unsigned(9)),
        ("n".into(), Value::Unsigned(513)),
    ]);
    let bytes = dynamic::encode::<Pssz>(&schema, &with_enum, &value).unwrap();
    assert_eq!(bytes, dynamic::encode::<Pssz>(&schema, &with_int, &value).unwrap());
    assert_eq!(
        dynamic::auto_width(&schema, &with_enum).unwrap(),
        dynamic::auto_width(&schema, &with_int).unwrap()
    );
    assert_eq!(dynamic::decode_exact::<Pssz>(&schema, &with_enum, &bytes).unwrap(), value);
    let as_enum = dynamic::layout::<Pssz>(&schema, &color).unwrap();
    let as_int = dynamic::layout::<Pssz>(&schema, &int(8, false)).unwrap();
    assert_eq!((as_enum.fixed, as_enum.size), (true, 1));
    assert_eq!((as_enum.fixed, as_enum.size, as_enum.optional), (as_int.fixed, as_int.size, as_int.optional));
    // The typed bytes of the Rust enums go through their own IR.
    let (schema, root) = schema_for::<Holder>().unwrap();
    let holder = Holder {
        color: Color::Unknown(200),
        status: Status::Other(-77),
        quoted: Color::Blue,
        list: vec![Color::Unknown(3), Color::Red],
        maybe: Some(Status::Closed),
    };
    let bytes = try_to_pssz::<Pssz, _>(&holder).unwrap();
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
    for (text, unknown) in [
        (dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap(), false),
        (dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap(), false),
    ] {
        assert_eq!(text.1, unknown);
        assert_eq!(
            text.0,
            r#"{"color":200,"status":-77,"quoted":"blue","list":[3,"red"],"maybe":"Closed"}"#
        );
        assert_eq!(text.0, json::to_string::<Json, _>(&holder).unwrap());
        assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, text.0.as_bytes()).unwrap(), bytes);
    }
}

#[test]
fn the_schema_driven_json_path_reads_and_writes_enums_like_the_typed_one() {
    let schema = Schema::default();
    let color = enumeration(int(8, false), &[("red", 0), ("green", 1), ("blue", 7)]);
    let status = enumeration(int(32, true), &[("pending", u64::MAX), ("active", 2)]);
    fn to<F: JsonFormat>(schema: &Schema, ty: &AnyType, text: &str) -> Result<Vec<u8>, String> {
        dynamic::json_to_pssz::<F>(schema, ty, text.as_bytes()).map_err(|e| e.what().to_string())
    }
    let read = |ty: &AnyType, text: &str| {
        let a = to::<Json>(&schema, ty, text);
        assert_eq!(a, to::<IJson>(&schema, ty, text), "{text}");
        a
    };
    let write = |ty: &AnyType, bytes: &[u8]| {
        let a = dynamic::pssz_to_json::<Json>(&schema, ty, bytes).unwrap().0;
        assert_eq!(a, dynamic::pssz_to_json::<IJson>(&schema, ty, bytes).unwrap().0);
        a
    };
    for text in ["\"blue\"", "7", "\"7\"", "\"blue:7\""] {
        assert_eq!(read(&color, text), Ok(vec![7]), "{text}");
        // The typed reader gives the same value.
        assert_eq!(both::<Color>(text), Ok(Color::Blue));
    }
    assert_eq!(write(&color, &[7]), "\"blue\"");
    assert_eq!(write(&color, &[0]), "\"red\"");
    // Through a schema an unknown number is always accepted and written back
    // as a number: there is no typed enum to refuse it.
    for text in ["9", "\"9\""] {
        assert_eq!(read(&color, text), Ok(vec![9]));
    }
    assert_eq!(write(&color, &[9]), "9");
    assert_eq!(write(&color, &[255]), "255");
    for text in ["\"pending\"", "-1", "\"-1\"", "\"pending:-1\""] {
        assert_eq!(read(&status, text), Ok((-1i32).to_le_bytes().to_vec()), "{text}");
    }
    assert_eq!(write(&status, &(-1i32).to_le_bytes()), "\"pending\"");
    assert_eq!(write(&status, &(-5i32).to_le_bytes()), "-5");
    assert_eq!(write(&status, &2i32.to_le_bytes()), "\"active\"");
    // The errors of the typed reader, with the same descriptions.
    for text in [
        "\"purple\"", "\"Red\"", "\"\"", "\":0\"", "\"purple:0\"", "\"red:1\"", "\"red:\"",
        "\"red:00\"", "\"red:256\"", "\"red:0:0\"", "\"07\"", "\"256\"", "\"-1\"", "\"+1\"",
        "\"0:red\"", "256", "-1",
    ] {
        let typed = both::<Color>(text).unwrap_err();
        assert_eq!(read(&color, text), Err(typed), "{text}");
    }
    for text in ["07", "1.0", "true", "null", "[\"red\"]"] {
        assert!(read(&color, text).is_err(), "{text}");
    }
    for text in ["\"pending:1\"", "\"-0\"", "-0", "2147483648", "\"2147483648\""] {
        assert!(read(&status, text).is_err(), "{text}");
    }
    // When two enumerators share a value the first listed name is written;
    // both names are read.
    let aliased = enumeration(int(8, false), &[("first", 1), ("second", 1)]);
    assert_eq!(write(&aliased, &[1]), "\"first\"");
    assert_eq!(read(&aliased, "\"second\""), Ok(vec![1]));
    // A 64-bit enum: unnamed values follow the 64-bit rule of each format.
    let big = enumeration(int(64, false), &[("huge", u64::MAX)]);
    let wide = (1u64 << 53).to_le_bytes();
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &big, &wide).unwrap().0, "9007199254740992");
    assert_eq!(
        dynamic::pssz_to_json::<IJson>(&schema, &big, &wide).unwrap().0,
        "\"9007199254740992\""
    );
    assert_eq!(write(&big, &u64::MAX.to_le_bytes()), "\"huge\"");
    assert_eq!(to::<IJson>(&schema, &big, "\"9007199254740992\""), Ok(wide.to_vec()));
    assert!(to::<IJson>(&schema, &big, "9007199254740992").is_err());
    assert_eq!(to::<Json>(&schema, &big, "9007199254740992"), Ok(wide.to_vec()));
    // An enum needs an integer of 8 to 64 bits to have a text.
    let odd = enumeration(int(128, false), &[("a", 0)]);
    assert!(to::<Json>(&schema, &odd, "\"a\"").is_err());
    assert!(dynamic::pssz_to_json::<Json>(&schema, &odd, &[0; 16]).is_err());
}

#[test]
fn adding_enumerators_is_a_compatible_upgrade() {
    use psio::pssz::upgrade::validate_optional_upgrade;
    let schema = Schema::default();
    let old = enumeration(int(8, false), &[("red", 0)]);
    let new = enumeration(int(8, false), &[("red", 0), ("blue", 7)]);
    assert!(validate_optional_upgrade(&schema, &old, &schema, &new).is_ok());
    // An enum and its bare integer are the same wire type.
    assert!(validate_optional_upgrade(&schema, &int(8, false), &schema, &new).is_ok());
    // A different underlying integer is not.
    let wider = enumeration(int(16, false), &[("red", 0)]);
    assert!(validate_optional_upgrade(&schema, &old, &schema, &wider).is_err());
}

// ── 128- and 256-bit integers ───────────────────────────────────────────────

fn u256(text: &str) -> Uint256 {
    json::from_str::<Json, Uint256>(text).unwrap_or_else(|e| panic!("{text}: {e}"))
}

#[derive(Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
struct Forced {
    #[json(json_string)]
    a: u128,
    #[json(ijson_number)]
    b: i128,
    #[json(json_string, ijson_number)]
    c: Uint256,
    #[json(ijson_number)]
    d: Vec<u128>,
    e: Option<i128>,
}

#[test]
fn wide_integers_are_decimal_bare_in_json_and_a_string_in_ijson() {
    const U128_MAX: &str = "340282366920938463463374607431768211455";
    const I128_MIN: &str = "-170141183460469231731687303715884105728";
    const U256_MAX: &str =
        "115792089237316195423570985008687907853269984665640564039457584007913129639935";
    let quoted = |text: &str| format!("\"{text}\"");
    assert_eq!(json::to_string::<Json, _>(&u128::MAX).unwrap(), U128_MAX);
    assert_eq!(json::to_string::<IJson, _>(&u128::MAX).unwrap(), quoted(U128_MAX));
    assert_eq!(json::to_string::<Json, _>(&i128::MIN).unwrap(), I128_MIN);
    assert_eq!(json::to_string::<IJson, _>(&i128::MIN).unwrap(), quoted(I128_MIN));
    let max = Uint256 { limb: [u64::MAX; 4] };
    assert_eq!(json::to_string::<Json, _>(&max).unwrap(), U256_MAX);
    assert_eq!(json::to_string::<IJson, _>(&max).unwrap(), quoted(U256_MAX));
    // IJson: a string always, small values included.
    assert_eq!(json::to_string::<IJson, _>(&7u128).unwrap(), "\"7\"");
    assert_eq!(json::to_string::<IJson, _>(&-7i128).unwrap(), "\"-7\"");
    assert_eq!(json::to_string::<IJson, _>(&0i128).unwrap(), "\"0\"");
    assert_eq!(json::to_string::<IJson, _>(&Uint256::from_u64(7)).unwrap(), "\"7\"");
    assert_eq!(json::to_string::<IJson, _>(&Uint256::ZERO).unwrap(), "\"0\"");
    assert_eq!(json::to_string::<Json, _>(&Uint256::ZERO).unwrap(), "0");
    assert_eq!(json::to_string::<Json, _>(&7u128).unwrap(), "7");
    // Limb boundaries of the 256-bit decimal.
    for (limb, text) in [
        ([0, 1, 0, 0], "18446744073709551616"),
        ([0, 0, 1, 0], "340282366920938463463374607431768211456"),
        ([0, 0, 0, 1], "6277101735386680763835789423207666416102355444464034512896"),
        ([10_000_000_000_000_000_000, 0, 0, 0], "10000000000000000000"),
        ([u64::MAX, u64::MAX, 0, 0], U128_MAX),
    ] {
        let value = Uint256 { limb };
        assert_eq!(json::to_string::<Json, _>(&value).unwrap(), text);
        assert_eq!(u256(text), value);
        assert_eq!(json::from_str::<IJson, Uint256>(&quoted(text)).unwrap(), value);
    }

    // Input, both formats: a decimal string ...
    assert_eq!(both::<u128>(&quoted(U128_MAX)), Ok(u128::MAX));
    assert_eq!(both::<i128>(&quoted(I128_MIN)), Ok(i128::MIN));
    assert_eq!(both::<Uint256>(&quoted(U256_MAX)), Ok(max));
    assert_eq!(both::<u128>("\"7\""), Ok(7));
    // ... a bare number within 2^53 - 1 ...
    assert_eq!(both::<u128>("9007199254740991"), Ok((1 << 53) - 1));
    assert_eq!(both::<i128>("-9007199254740991"), Ok(-((1 << 53) - 1)));
    assert_eq!(both::<Uint256>("12"), Ok(Uint256::from_u64(12)));
    assert_eq!(both::<i128>("0"), Ok(0));
    // ... beyond it only under Json.
    assert_eq!(json::from_str::<Json, u128>(U128_MAX).unwrap(), u128::MAX);
    assert_eq!(json::from_str::<Json, i128>(I128_MIN).unwrap(), i128::MIN);
    assert_eq!(json::from_str::<Json, Uint256>(U256_MAX).unwrap(), max);
    for text in ["9007199254740992", U128_MAX] {
        assert_eq!(
            json::from_str::<IJson, u128>(text).unwrap_err().what,
            "ijson: bare integer outside the safe range must be a string"
        );
        assert!(json::from_str::<IJson, Uint256>(text).is_err());
    }
    assert!(json::from_str::<IJson, i128>("-9007199254740992").is_err());
    assert!(json::from_str::<IJson, i128>("9007199254740992").is_err());

    // The legacy "0x" string: read in both formats, never written.
    assert_eq!(both::<u128>("\"0xff\""), Ok(255));
    assert_eq!(both::<u128>("\"0XFF\""), Ok(255));
    assert_eq!(both::<u128>("\"0xFf\""), Ok(255));
    assert_eq!(both::<u128>("\"0x0\""), Ok(0));
    assert_eq!(both::<u128>("\"0x000000000000000000000000000000ff\""), Ok(255));
    assert_eq!(both::<u128>("\"0xffffffffffffffffffffffffffffffff\""), Ok(u128::MAX));
    assert_eq!(both::<i128>("\"-0x2a\""), Ok(-42));
    assert_eq!(both::<i128>("\"0x2a\""), Ok(42));
    assert_eq!(both::<i128>("\"-0x80000000000000000000000000000000\""), Ok(i128::MIN));
    assert_eq!(both::<i128>("\"0x7fffffffffffffffffffffffffffffff\""), Ok(i128::MAX));
    assert_eq!(both::<Uint256>("\"0x100\""), Ok(Uint256::from_u64(256)));
    assert_eq!(both::<Uint256>(&format!("\"0x{}\"", "f".repeat(64))), Ok(max));
    assert_eq!(json::to_string::<IJson, _>(&both::<u128>("\"0xff\"").unwrap()).unwrap(), "\"255\"");
    assert_eq!(json::to_string::<Json, _>(&both::<i128>("\"-0x2a\"").unwrap()).unwrap(), "-42");

    // Errors, in both formats.
    for text in [
        "\"340282366920938463463374607431768211456\"", // over the maximum
        "340282366920938463463374607431768211456",
        "\"-1\"", "-1", "\"01\"", "01", "\"\"", "\"+1\"", "\" 1\"", "\"1 \"", "1.5", "1e3",
        "\"1.5\"", "\"0x\"", "\"0X\"", "\"x1\"", "\"0xfg\"", "\"-0x1\"", "\"0x-1\"",
        "\"0x100000000000000000000000000000000\"", // 33 digits
        "\"0x0ffffffffffffffffffffffffffffffff\"", // 33 digits, in range
        "\"\\u0031\"", "true", "null", "[1]", "0x1",
    ] {
        assert!(both::<u128>(text).is_err(), "{text}");
    }
    for text in [
        "\"170141183460469231731687303715884105728\"",
        "\"-170141183460469231731687303715884105729\"",
        "\"-0\"", "-0", "\"0x80000000000000000000000000000000\"",
        "\"-0x80000000000000000000000000000001\"", "\"--1\"", "\"-\"",
    ] {
        assert!(both::<i128>(text).is_err(), "{text}");
    }
    let over = "115792089237316195423570985008687907853269984665640564039457584007913129639936";
    for text in [
        quoted(over), "\"-1\"".into(), "\"00\"".into(), "\"-0x1\"".into(),
        format!("\"0x1{}\"", "0".repeat(64)), format!("\"{}\"", "9".repeat(80)),
    ] {
        assert!(both::<Uint256>(&text).is_err(), "{text}");
    }
    assert!(json::from_str::<Json, Uint256>(over).is_err());

    // A forced form is written in and reads exactly that form.
    let forced = Forced {
        a: 7,
        b: i128::MIN,
        c: Uint256::from_u64(9),
        d: vec![u128::MAX, 1],
        e: Some(-3),
    };
    let json_text = format!(r#"{{"a":"7","b":{I128_MIN},"c":"9","d":[{U128_MAX},1],"e":-3}}"#);
    let ijson_text = format!(r#"{{"a":"7","b":{I128_MIN},"c":9,"d":[{U128_MAX},1],"e":"-3"}}"#);
    assert_eq!(json::to_string::<Json, _>(&forced).unwrap(), json_text);
    assert_eq!(json::to_string::<IJson, _>(&forced).unwrap(), ijson_text);
    assert_eq!(json::from_str::<Json, Forced>(&json_text).unwrap(), forced);
    assert_eq!(json::from_str::<IJson, Forced>(&ijson_text).unwrap(), forced);
    // json_string refuses a bare number; ijson_number refuses a string and
    // lifts the 2^53 limit.
    assert!(json::from_str::<Json, Forced>(r#"{"a":7,"b":0,"c":"9","d":[],"e":null}"#).is_err());
    assert!(json::from_str::<IJson, Forced>(r#"{"a":"7","b":"0","c":9,"d":[],"e":null}"#).is_err());
    assert!(json::from_str::<IJson, Forced>(r#"{"a":"7","b":0,"c":"9","d":[],"e":null}"#).is_err());
    assert!(json::from_str::<IJson, Forced>(r#"{"a":"7","b":0,"c":9,"d":["1"],"e":null}"#).is_err());
    // The same through the schema the derive emits.
    let (schema, root) = schema_for::<Forced>().unwrap();
    let bytes = try_to_pssz::<Pssz, _>(&forced).unwrap();
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, json_text);
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, ijson_text);
    assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, json_text.as_bytes()).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, ijson_text.as_bytes()).unwrap(), bytes);
    let bad = r#"{"a":7,"b":0,"c":"9","d":[],"e":null}"#;
    assert!(dynamic::json_to_pssz::<Json>(&schema, &root, bad.as_bytes()).is_err());
    for bad in [
        r#"{"a":"7","b":"0","c":9,"d":[],"e":null}"#,
        r#"{"a":"7","b":0,"c":"9","d":[],"e":null}"#,
        r#"{"a":"7","b":0,"c":9,"d":["1"],"e":null}"#,
    ] {
        assert!(dynamic::json_to_pssz::<IJson>(&schema, &root, bad.as_bytes()).is_err(), "{bad}");
    }
}

#[test]
fn the_schema_driven_path_reads_and_writes_wide_integers_like_the_typed_one() {
    let schema = Schema::default();
    fn typed<T: JsonText + psio::pssz::PsszPack<Pssz>>(ijson: bool, text: &str) -> Option<Vec<u8>> {
        let value = if ijson {
            json::from_str::<IJson, T>(text).ok()?
        } else {
            json::from_str::<Json, T>(text).ok()?
        };
        Some(try_to_pssz::<Pssz, _>(&value).unwrap())
    }
    let texts = [
        "0", "\"0\"", "7", "\"7\"", "-7", "\"-7\"", "9007199254740991", "9007199254740992",
        "-9007199254740991", "-9007199254740992", "\"9007199254740992\"",
        "340282366920938463463374607431768211455", "\"340282366920938463463374607431768211455\"",
        "340282366920938463463374607431768211456", "\"340282366920938463463374607431768211456\"",
        "170141183460469231731687303715884105727", "170141183460469231731687303715884105728",
        "-170141183460469231731687303715884105728", "\"-170141183460469231731687303715884105729\"",
        "115792089237316195423570985008687907853269984665640564039457584007913129639935",
        "\"115792089237316195423570985008687907853269984665640564039457584007913129639936\"",
        "\"0xff\"", "\"0XFF\"", "\"-0x2a\"", "\"0x\"", "\"0xfg\"", "\"-0x0\"",
        "\"0x100000000000000000000000000000000\"", "\"0x80000000000000000000000000000000\"",
        "\"-0x80000000000000000000000000000000\"", "\"01\"", "01", "\"\"", "\"+1\"", "\"-0\"",
        "-0", "1.5", "1e3", "true", "null", "\"\\u0037\"", "[1]",
    ];
    for text in texts {
        for ijson in [false, true] {
            let through = |ty: &AnyType| {
                if ijson {
                    dynamic::json_to_pssz::<IJson>(&schema, ty, text.as_bytes()).ok()
                } else {
                    dynamic::json_to_pssz::<Json>(&schema, ty, text.as_bytes()).ok()
                }
            };
            assert_eq!(through(&int(128, false)), typed::<u128>(ijson, text), "u128 {text} {ijson}");
            assert_eq!(through(&int(128, true)), typed::<i128>(ijson, text), "i128 {text} {ijson}");
            assert_eq!(through(&int(256, false)), typed::<Uint256>(ijson, text), "u256 {text} {ijson}");
            assert_eq!(through(&int(256, true)), None, "i256 {text}");
        }
    }
    // Output: the typed text.
    for value in [0u128, 7, (1 << 53) - 1, 1 << 53, u128::MAX] {
        let bytes = value.to_le_bytes();
        let ty = int(128, false);
        assert_eq!(
            dynamic::pssz_to_json::<Json>(&schema, &ty, &bytes).unwrap().0,
            json::to_string::<Json, _>(&value).unwrap()
        );
        assert_eq!(
            dynamic::pssz_to_json::<IJson>(&schema, &ty, &bytes).unwrap().0,
            json::to_string::<IJson, _>(&value).unwrap()
        );
    }
    for value in [0i128, -1, -7, i128::MIN, i128::MAX] {
        let bytes = value.to_le_bytes();
        let ty = int(128, true);
        assert_eq!(
            dynamic::pssz_to_json::<Json>(&schema, &ty, &bytes).unwrap().0,
            json::to_string::<Json, _>(&value).unwrap()
        );
        assert_eq!(
            dynamic::pssz_to_json::<IJson>(&schema, &ty, &bytes).unwrap().0,
            json::to_string::<IJson, _>(&value).unwrap()
        );
    }
    for limb in [[0; 4], [7, 0, 0, 0], [1, 2, 3, 4], [u64::MAX; 4]] {
        let value = Uint256 { limb };
        let bytes = try_to_pssz::<Pssz, _>(&value).unwrap();
        let ty = int(256, false);
        assert_eq!(
            dynamic::pssz_to_json::<Json>(&schema, &ty, &bytes).unwrap().0,
            json::to_string::<Json, _>(&value).unwrap()
        );
        assert_eq!(
            dynamic::pssz_to_json::<IJson>(&schema, &ty, &bytes).unwrap().0,
            json::to_string::<IJson, _>(&value).unwrap()
        );
    }
    // A hand-built value of the wrong shape is an error, not a guess.
    assert!(dynamic::value_to_json::<Json>(&schema, &int(256, false), &Value::Unsigned(1)).is_err());
    assert!(dynamic::value_to_json::<Json>(&schema, &int(256, false), &Value::Bytes(vec![0; 31])).is_err());
    assert!(dynamic::value_to_json::<Json>(&schema, &int(128, true), &Value::Unsigned(1)).is_err());
}

// ── Non-finite floats and float text ────────────────────────────────────────

#[test]
fn non_finite_floats_are_refused_on_output_and_never_read() {
    const WHAT: &str = "json: non-finite float has no JSON text";
    let schema = Schema::default();
    let f64_type = AnyType::new(AnyValue::Float(Float { exp: 11, mantissa: 53 }));
    let f32_type = AnyType::new(AnyValue::Float(Float { exp: 8, mantissa: 24 }));
    for value in [f64::NAN, -f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        // Typed.
        assert_eq!(json::to_string::<Json, _>(&value).unwrap_err().what, WHAT);
        assert_eq!(json::to_string::<IJson, _>(&value).unwrap_err().what, WHAT);
        assert!(psio::encode::<Json, _>(&value).is_err());
        assert!(psio::encode::<IJson, _>(&value).is_err());
        assert!(json::to_string::<Json, _>(&vec![1.0, value]).is_err());
        assert!(json::to_string::<IJson, _>(&Some(value)).is_err());
        // Schema-driven: from bytes and from a value.
        let bytes = value.to_le_bytes();
        assert!(dynamic::validate_pssz(&schema, &f64_type, &bytes).is_ok(), "valid PSSZ");
        for error in [
            dynamic::pssz_to_json::<Json>(&schema, &f64_type, &bytes).unwrap_err(),
            dynamic::pssz_to_json::<IJson>(&schema, &f64_type, &bytes).unwrap_err(),
            dynamic::value_to_json::<Json>(&schema, &f64_type, &Value::Float64(value)).unwrap_err(),
            dynamic::value_to_json::<IJson>(&schema, &f64_type, &Value::Float64(value)).unwrap_err(),
        ] {
            assert_eq!(error.what(), WHAT);
        }
    }
    for value in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
        assert_eq!(json::to_string::<Json, _>(&value).unwrap_err().what, WHAT);
        assert_eq!(json::to_string::<IJson, _>(&value).unwrap_err().what, WHAT);
        let bytes = value.to_le_bytes();
        for error in [
            dynamic::pssz_to_json::<Json>(&schema, &f32_type, &bytes).unwrap_err(),
            dynamic::pssz_to_json::<IJson>(&schema, &f32_type, &bytes).unwrap_err(),
            dynamic::value_to_json::<IJson>(&schema, &f32_type, &Value::Float32(value)).unwrap_err(),
        ] {
            assert_eq!(error.what(), WHAT);
        }
    }
    // No spelling of a non-finite value is read, in either format.
    for text in [
        "NaN", "nan", "-NaN", "Infinity", "-Infinity", "+Infinity", "inf", "-inf", "Inf",
        "\"NaN\"", "\"Infinity\"", "\"-Infinity\"", "\"inf\"", "null", "1e999", "-1e999",
        "1e309", "1.8e308", "0x1p0",
    ] {
        assert!(both::<f64>(text).is_err(), "{text}");
        assert!(both::<f32>(text).is_err(), "{text}");
        for ty in [&f64_type, &f32_type] {
            assert!(dynamic::json_to_pssz::<Json>(&schema, ty, text.as_bytes()).is_err(), "{text}");
            assert!(dynamic::json_to_pssz::<IJson>(&schema, ty, text.as_bytes()).is_err(), "{text}");
        }
    }
    // Finite in f64 but not in f32.
    assert!(both::<f32>("3.5e38").is_err());
    assert!(dynamic::json_to_pssz::<Json>(&schema, &f32_type, b"3.5e38").is_err());
    assert_eq!(both::<f64>("3.5e38"), Ok(3.5e38));
    // Underflow reads as zero.
    assert_eq!(both::<f64>("1e-999"), Ok(0.0));
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &f64_type, b"1e-999").unwrap(), [0; 8]);
}

#[test]
fn float_text_is_the_same_typed_and_schema_driven() {
    let schema = Schema::default();
    let f64_type = AnyType::new(AnyValue::Float(Float { exp: 11, mantissa: 53 }));
    let f32_type = AnyType::new(AnyValue::Float(Float { exp: 8, mantissa: 24 }));
    for (value, text) in [
        (2f64.powi(-24), "5.960464477539063e-08"),
        (5e-324, "5e-324"),
        (2.2250738585072014e-308, "2.2250738585072014e-308"),
        (1e21, "1e+21"),
        (1e20, "1e+20"),
        (1e-7, "1e-07"),
        (1e-5, "1e-05"),
        (0.0001, "1e-04"),
        (0.001, "0.001"),
        (0.1, "0.1"),
        (-0.0, "-0"),
        (0.0, "0"),
        (f64::MAX, "1.7976931348623157e+308"),
        (4503599627370498.0, "4503599627370498"),
    ] {
        for ijson in [false, true] {
            let (typed, through) = if ijson {
                (
                    json::to_string::<IJson, _>(&value).unwrap(),
                    dynamic::pssz_to_json::<IJson>(&schema, &f64_type, &value.to_le_bytes()).unwrap().0,
                )
            } else {
                (
                    json::to_string::<Json, _>(&value).unwrap(),
                    dynamic::pssz_to_json::<Json>(&schema, &f64_type, &value.to_le_bytes()).unwrap().0,
                )
            };
            assert_eq!(typed, text);
            assert_eq!(through, text);
        }
        assert_eq!(json::from_str::<Json, f64>(text).unwrap().to_bits(), value.to_bits());
    }
    for (value, text) in [
        (f32::from_bits(1), "1e-45"),
        (f32::MAX, "3.4028235e+38"),
        (1e10f32, "1e+10"),
        (1e-5f32, "1e-05"),
        (-0.1f32, "-0.1"),
        (16777216f32, "16777216"),
    ] {
        assert_eq!(json::to_string::<Json, _>(&value).unwrap(), text);
        assert_eq!(json::to_string::<IJson, _>(&value).unwrap(), text);
        assert_eq!(
            dynamic::pssz_to_json::<Json>(&schema, &f32_type, &value.to_le_bytes()).unwrap().0,
            text
        );
        assert_eq!(
            dynamic::pssz_to_json::<IJson>(&schema, &f32_type, &value.to_le_bytes()).unwrap().0,
            text
        );
    }
}
