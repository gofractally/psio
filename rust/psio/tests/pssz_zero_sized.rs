use psio::pssz::*;

#[derive(Debug, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct AbiUnit;

#[test]
fn zero_size_fixed_values_never_produce_ambiguous_lists_or_divide_by_zero() {
    assert!(try_encode::<Pssz32, _>(&vec![AbiUnit]).is_err());
    assert_eq!(decode::<Pssz32, Vec<AbiUnit>>(&[]).unwrap(), []);
    assert!(decode::<Pssz32, Vec<AbiUnit>>(&[0]).is_err());
    assert!(try_encode::<Pssz32, _>(&[AbiUnit, AbiUnit])
        .unwrap()
        .is_empty());
    assert_eq!(
        decode::<Pssz32, [AbiUnit; 2]>(&[]).unwrap(),
        [AbiUnit, AbiUnit]
    );
    assert!(try_encode::<Pssz32, _>(&[AbiUnit; 0]).unwrap().is_empty());
    assert!(decode::<Pssz32, [AbiUnit; 0]>(&[0]).is_err());
    assert_eq!(try_encode::<Pssz32, _>(&Some(AbiUnit)).unwrap(), [1]);
    assert_eq!(
        decode::<Pssz32, Option<AbiUnit>>(&[1]).unwrap(),
        Some(AbiUnit)
    );
}
