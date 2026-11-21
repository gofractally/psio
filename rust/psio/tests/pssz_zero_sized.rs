use psio::pssz::*;

#[derive(Debug, PartialEq, PsszPack, PsszUnpack)]
#[pssz(definition_will_not_change)]
struct AbiUnit;

#[test]
fn zero_size_fixed_values_never_produce_ambiguous_lists_or_divide_by_zero() {
    assert!(try_to_pssz::<Pssz32, _>(&vec![AbiUnit]).is_err());
    assert_eq!(from_pssz::<Pssz32, Vec<AbiUnit>>(&[]).unwrap(), []);
    assert!(from_pssz::<Pssz32, Vec<AbiUnit>>(&[0]).is_err());
    assert!(try_to_pssz::<Pssz32, _>(&[AbiUnit, AbiUnit])
        .unwrap()
        .is_empty());
    assert_eq!(
        from_pssz::<Pssz32, [AbiUnit; 2]>(&[]).unwrap(),
        [AbiUnit, AbiUnit]
    );
    assert!(try_to_pssz::<Pssz32, _>(&[AbiUnit; 0]).unwrap().is_empty());
    assert!(from_pssz::<Pssz32, [AbiUnit; 0]>(&[0]).is_err());
    assert_eq!(try_to_pssz::<Pssz32, _>(&Some(AbiUnit)).unwrap(), [1]);
    assert_eq!(
        from_pssz::<Pssz32, Option<AbiUnit>>(&[1]).unwrap(),
        Some(AbiUnit)
    );
}
