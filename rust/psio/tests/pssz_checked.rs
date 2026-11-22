use psio::pssz::*;

#[derive(Debug, PartialEq)]
struct Record {
    flag: bool,
    text: String,
    other: String,
}
psio::pssz_struct!(Record {
    flag: bool,
    text: String,
    other: String
});

#[derive(Debug, PartialEq)]
struct Fixed {
    value: u32,
}
psio::pssz_struct_dwnc!(Fixed { value: u32 });

#[test]
fn exact_scalar_spans_and_valid_bool_values() {
    assert!(decode::<Pssz32, u32>(&[0; 5]).is_err());
    assert!(decode::<Pssz32, bool>(&[0, 0]).is_err());
    for bytes in [&[2][..], &[255][..]] {
        assert!(decode::<Pssz32, Vec<bool>>(bytes).is_err());
        assert!(decode::<Pssz32, [bool; 1]>(bytes).is_err());
        assert!(decode::<Pssz32, Box<[bool; 1]>>(bytes).is_err());
    }
}

#[test]
fn fixed_custom_types_are_walked_instead_of_memcopied() {
    let values = vec![Fixed { value: 0x12345678 }];
    let bytes = try_encode::<Pssz32, _>(&values).unwrap();
    assert_eq!(bytes, [0x78, 0x56, 0x34, 0x12]);
    assert_eq!(decode::<Pssz32, Vec<Fixed>>(&bytes).unwrap(), values);
}

#[test]
fn variable_optional_record_uses_cpp_selector_rule() {
    // docs/pssz-spec.md 5.5, as the C++ codec now implements it: an
    // extensible record always has its header, so Option<Record> needs no
    // selector. (This previously expected [1, 3, 1, 3, 4, 'a', 'b'] and [0].)
    let record = Record {
        flag: true,
        text: "a".into(),
        other: "b".into(),
    };
    let bytes = try_encode::<Pssz8, _>(&Some(record)).unwrap();
    assert_eq!(bytes, [3, 1, 3, 4, b'a', b'b']);
    assert_eq!(try_encode::<Pssz8, Option<Record>>(&None).unwrap(), [] as [u8; 0]);
    assert_eq!(decode::<Pssz8, Option<Record>>(&bytes).unwrap().unwrap().text, "a");
    assert!(decode::<Pssz8, Option<Record>>(&[]).unwrap().is_none());
    // Payloads that can be empty keep the selector; nested options follow
    // from each level's own minimum.
    assert_eq!(try_encode::<Pssz8, Option<String>>(&None).unwrap(), [0]);
    assert_eq!(try_encode::<Pssz8, _>(&Some(String::new())).unwrap(), [1]);
    assert_eq!(try_encode::<Pssz8, Option<Option<String>>>(&None).unwrap(), [] as [u8; 0]);
    assert_eq!(try_encode::<Pssz8, Option<Option<String>>>(&Some(None)).unwrap(), [0]);
    assert_eq!(try_encode::<Pssz8, Option<Option<u32>>>(&None).unwrap(), [0]);
    assert_eq!(try_encode::<Pssz8, Option<Option<u32>>>(&Some(None)).unwrap(), [1]);
    assert_eq!(decode::<Pssz8, Option<Option<String>>>(&[0]).unwrap(), Some(None));
    assert_eq!(decode::<Pssz8, Option<Option<u32>>>(&[1]).unwrap(), Some(None));
}

#[test]
fn checked_encode_reports_nested_offset_and_bound_overflow() {
    let values = vec!["x".repeat(255), "last".into()];
    assert!(try_encode::<Pssz8, _>(&values).is_err());
    assert!(try_encode::<Pssz8, _>(&Some(values)).is_err());
    assert!(try_encode::<Pssz32, _>(&BoundedString::<2>("long".into())).is_err());
    assert!(try_encode::<Pssz32, _>(&BoundedList::<u8, 1>(vec![1, 2])).is_err());
    use psio::pssz_checked::{encode_record, Layout};
    assert!(encode_record::<Pssz32>(
        &[(
            Layout {
                fixed: true,
                size: usize::MAX,
                optional: false,
                selector: false,
                omit: false,
            },
            vec![]
        )],
        false,
        &mut vec![]
    )
    .is_err());
}

#[test]
fn canonical_tables_reject_gaps_zero_offsets_and_trailing_bytes() {
    assert!(decode::<Pssz8, Vec<String>>(&[0, 1]).is_err());
    assert!(decode::<Pssz8, [String; 1]>(&[2, 0, b'x']).is_err());
    assert!(decode::<Pssz8, [String; 0]>(&[0]).is_err());
    assert!(decode::<Pssz8, Fixed>(&[1, 0, 0, 0, 9]).is_err());
    assert!(decode::<Pssz8, Record>(&[3, 1, 4, 4, 0]).is_err());
    assert!(decode::<Pssz8, Record>(&[2, 1, 3, 3]).is_err());
}

#[test]
fn arbitrary_small_buffers_return_errors_without_panics() {
    for size in 0..24 {
        for seed in 0u8..=255 {
            let bytes: Vec<_> = (0..size).map(|i| seed.wrapping_add(i as u8 * 7)).collect();
            assert!(std::panic::catch_unwind(|| decode::<Pssz8, Record>(&bytes)).is_ok());
            assert!(
                std::panic::catch_unwind(|| decode::<Pssz8, Vec<Vec<bool>>>(&bytes)).is_ok()
            );
            assert!(std::panic::catch_unwind(|| decode::<Pssz16, [String; 2]>(&bytes)).is_ok());
        }
    }
}
