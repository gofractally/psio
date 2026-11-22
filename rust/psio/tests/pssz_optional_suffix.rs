use psio::pssz::*;

#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Base {
    name: String,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Extended {
    name: String,
    extra: Option<String>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Twice {
    name: String,
    middle: Option<u32>,
    last: Option<String>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Required {
    name: String,
    extra: String,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct OddBase {
    flag: bool,
    name: String,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct OddExtended {
    flag: bool,
    name: String,
    extra: Option<String>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct OldOuter {
    inner: Base,
    values: Vec<Base>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct NewOuter {
    inner: Extended,
    values: Vec<Extended>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
#[psio(transparent)]
struct OptionalWrapper(Option<String>);
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Wrapped {
    name: String,
    extra: OptionalWrapper,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
#[psio(definition_will_not_change)]
struct Unit;

fn profiles<F: PsszFormat>() {
    let base = try_encode::<F, _>(&Base { name: "a".into() }).unwrap();
    let none = try_encode::<F, _>(&Extended {
        name: "a".into(),
        extra: None,
    })
    .unwrap();
    assert_eq!(base, none, "trailing None is pruned");
    assert_eq!(
        decode_strict::<F, Extended>(&base).unwrap(),
        Extended {
            name: "a".into(),
            extra: None
        }
    );
    let some = try_encode::<F, _>(&Extended {
        name: "a".into(),
        extra: Some("b".into()),
    })
    .unwrap();
    let old = decode_checked::<F, Base>(&some).unwrap();
    assert_eq!(old.value.name, "a");
    assert!(old.has_unknown);
    assert!(decode_strict::<F, Base>(&some).is_err());
    assert!(decode_checked::<F, Base>(&some)
        .unwrap()
        .into_known()
        .is_err());
    assert!(!decode_checked::<F, Base>(&base).unwrap().has_unknown);
    assert!(decode_strict::<F, Required>(&base).is_err());
    let twice = try_encode::<F, _>(&Twice {
        name: "a".into(),
        middle: None,
        last: Some("b".into()),
    })
    .unwrap();
    assert_eq!(
        decode_checked::<F, Base>(&twice).unwrap().value.name,
        "a"
    );
    assert_eq!(decode_strict::<F, Twice>(&twice).unwrap().middle, None);
    let odd = try_encode::<F, _>(&OddExtended {
        flag: true,
        name: "a".into(),
        extra: Some("b".into()),
    })
    .unwrap();
    let odd = decode_checked::<F, OddBase>(&odd).unwrap();
    assert_eq!(
        odd.value,
        OddBase {
            flag: true,
            name: "a".into()
        }
    );
    assert!(odd.has_unknown);
    assert_eq!(
        try_encode::<F, _>(&Wrapped {
            name: "a".into(),
            extra: OptionalWrapper(None)
        })
        .unwrap(),
        base
    );
    assert_eq!(
        decode_strict::<F, Wrapped>(&base).unwrap().extra,
        OptionalWrapper(None)
    );
    assert_eq!(try_encode::<F, _>(&Option::<Unit>::None).unwrap(), [0]);
    assert_eq!(try_encode::<F, _>(&Some(Unit)).unwrap(), [1]);
    assert_eq!(
        decode_strict::<F, Option<Unit>>(&[1]).unwrap(),
        Some(Unit)
    );
    assert!(try_encode::<F, Option<Base>>(&None).unwrap().is_empty());
    assert_eq!(
        try_encode::<F, _>(&Some(Base { name: "a".into() })).unwrap(),
        base
    );
    let old_view = psio::pssz_view::PsszView::<Base, F>::new_checked(&some).unwrap();
    assert!(old_view.has_unknown);
    assert_eq!(old_view.value.name().view().unwrap(), "a");
    let new_view = psio::pssz_view::PsszView::<Extended, F>::new_strict(&base).unwrap();
    assert!(!new_view.extra().has_value());
    assert_eq!(
        new_view.extra().bytes(),
        [0],
        "missing Option<String> needs its canonical None selector"
    );
    let twice_view = psio::pssz_view::PsszView::<Twice, F>::new_strict(&twice).unwrap();
    assert!(!twice_view.middle().has_value());
    assert_eq!(twice_view.last().unwrap().view().unwrap(), "b");
    let odd_bytes = try_encode::<F, _>(&OddExtended {
        flag: true,
        name: "a".into(),
        extra: Some("b".into()),
    })
    .unwrap();
    let odd_view = psio::pssz_view::PsszView::<OddBase, F>::new_checked(&odd_bytes).unwrap();
    assert!(odd_view.value.flag().get());
    assert_eq!(odd_view.value.name().view().unwrap(), "a");
}

#[test]
fn optional_suffix_upgrades_roundtrip_across_all_explicit_widths() {
    profiles::<Pssz8>();
    profiles::<Pssz16>();
    profiles::<Pssz32>();
}

#[test]
fn nested_unknown_flags_propagate_without_leaking_between_calls() {
    let bytes = try_encode::<Pssz32, _>(&NewOuter {
        inner: Extended {
            name: "a".into(),
            extra: Some("b".into()),
        },
        values: vec![Extended {
            name: "c".into(),
            extra: Some("d".into()),
        }],
    })
    .unwrap();
    let decoded = decode_checked::<Pssz32, OldOuter>(&bytes).unwrap();
    assert!(decoded.has_unknown);
    assert_eq!(decoded.value.inner.name, "a");
    assert_eq!(decoded.value.values[0].name, "c");
    assert!(decode_strict::<Pssz32, OldOuter>(&bytes).is_err());
    assert!(
        !decode_checked::<Pssz32, NewOuter>(&bytes)
            .unwrap()
            .has_unknown
    );
    let view = psio::pssz_view::PsszView::<OldOuter, Pssz32>::new_checked(&bytes).unwrap();
    assert!(view.has_unknown);
    assert_eq!(view.value.inner().name().view().unwrap(), "a");
    assert_eq!(view.value.values().get(0).name().view().unwrap(), "c");
}

#[test]
fn validated_view_does_not_materialize_payload_values() {
    struct ValidateOnly;
    impl<F: PsszFormat> PsszPack<F> for ValidateOnly {
        const MIN_ENCODED_SIZE: usize = 0;
        const MAX_ENCODED_SIZE: Option<usize> = None;
        const IS_FIXED_SIZE: bool = false;
        const FIXED_SIZE: usize = 0;
        fn pssz_size(&self) -> usize {
            3
        }
        fn pssz_pack(&self, out: &mut Vec<u8>) {
            out.extend_from_slice(b"abc");
        }
    }
    impl<F: PsszFormat> PsszUnpack<F> for ValidateOnly {
        fn pssz_unpack(_: &[u8]) -> PsszResult<Self> {
            panic!("view validation must not unpack payloads")
        }
    }
    impl<F: PsszFormat> PsszValidate<F> for ValidateOnly {
        fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
            <String as PsszValidate<F>>::pssz_validate(bytes)
        }
    }
    #[derive(PsszPack, PsszUnpack, PsszView)]
    struct Wrapper {
        inner: ValidateOnly,
    }
    let bytes = try_encode::<Pssz32, _>(&Wrapper {
        inner: ValidateOnly,
    })
    .unwrap();
    let view = psio::pssz_view::PsszView::<Wrapper, Pssz32>::new_strict(&bytes).unwrap();
    assert_eq!(view.inner().bytes(), b"abc");
    assert_eq!(view.inner().bytes().as_ptr(), bytes[8..].as_ptr());
}

#[test]
fn malformed_headers_and_unknown_offsets_are_rejected_without_panics() {
    for bytes in [
        vec![3, 0, 0, 0, 3, 0, 0, 0],
        vec![5, 0, 0, 0, 5, 0, 0, 0, 0],
        vec![8, 0, 0, 0, 7, 0, 0, 0, 8, 0, 0, 0],
        vec![8, 0, 0, 0, 8, 0, 0, 0, 7, 0, 0, 0],
        vec![8, 0, 0, 0, 8, 0, 0, 0, 255, 0, 0, 0],
    ] {
        let result = std::panic::catch_unwind(|| decode_checked::<Pssz32, Base>(&bytes));
        assert!(result.is_ok());
        assert!(result.unwrap().is_err());
    }
}

#[test]
fn none_in_nontrailing_slot_is_preserved_and_noncanonical_trailing_none_can_be_normalized() {
    let bytes = try_encode::<Pssz8, _>(&Twice {
        name: "a".into(),
        middle: None,
        last: Some("b".into()),
    })
    .unwrap();
    assert_eq!(bytes, [3, 3, 4, 4, b'a', 1, b'b']);
    // Pruning is mandatory (docs/pssz-optional-evolution.md, as C++ does):
    // a record that stores a trailing None has a second encoding of the same
    // value, so EVERY reader that knows the field rejects it. (This test
    // previously expected decode_strict to accept these bytes and
    // re-encode them as [4, 0, 0, 0, 4, 0, 0, 0, b'a'].)
    let unpruned = [8, 0, 0, 0, 8, 0, 0, 0, 9, 0, 0, 0, b'a', 0];
    const UNPRUNED: PsszError = PsszError::new("pssz: trailing absent optional field was not pruned");
    assert_eq!(decode_strict::<Pssz32, Extended>(&unpruned).unwrap_err(), UNPRUNED);
    assert_eq!(decode::<Pssz32, Extended>(&unpruned).unwrap_err(), UNPRUNED);
    assert_eq!(decode_checked::<Pssz32, Extended>(&unpruned).unwrap_err(), UNPRUNED);
    assert_eq!(validate::<Pssz32, Extended>(&unpruned).unwrap_err(), UNPRUNED);
    assert_eq!(validate_strict::<Pssz32, Extended>(&unpruned).unwrap_err(), UNPRUNED);
    assert!(psio::pssz_view::PsszView::<Extended, Pssz32>::new(&unpruned).is_err());
    // An older reader that does not know `extra` cannot judge the writer's
    // last slot: it accepts the frame and reports unknown fields.
    assert!(decode_checked::<Pssz32, Base>(&unpruned).unwrap().has_unknown);
    // The canonical encoding of the same value is the pruned one.
    let value = decode_strict::<Pssz32, Extended>(&[4, 0, 0, 0, 4, 0, 0, 0, b'a']).unwrap();
    assert_eq!(value.extra, None);
    assert_eq!(
        try_encode::<Pssz32, _>(&value).unwrap(),
        [4, 0, 0, 0, 4, 0, 0, 0, b'a']
    );
    assert!(validate_canonical_for_profile::<Pssz32, Extended>(&unpruned).is_err());
    assert!(
        validate_canonical_for_profile::<Pssz32, Extended>(&[4, 0, 0, 0, 4, 0, 0, 0, b'a']).is_ok()
    );
}
