use psio::pssz::*;

#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Empty;
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
#[psio(transparent)]
struct Account(u64);
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Generic<T> {
    id: u32,
    nested: Option<Vec<T>>,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Pair(u16, String);
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
enum Message<T> {
    Unit,
    Value(T),
    Pair(u16, String),
    Named { flag: bool, name: String },
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct Node {
    value: u8,
    next: Option<Box<Node>>,
}

fn roundtrip<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F> + std::fmt::Debug + PartialEq>(
    value: T,
) {
    let bytes = try_encode::<F, _>(&value).unwrap();
    assert_eq!(decode::<F, T>(&bytes).unwrap(), value);
}

#[test]
fn generic_records_tuple_structs_transparent_wrappers_and_enums() {
    roundtrip::<Pssz8, _>(Generic {
        id: 7,
        nested: Some(vec!["a".to_string(), "bc".into()]),
    });
    roundtrip::<Pssz16, _>(Pair(9, "hello".into()));
    roundtrip::<Pssz32, _>(Account(42));
    assert_eq!(
        try_encode::<Pssz32, _>(&Account(42)).unwrap(),
        42u64.to_le_bytes()
    );
    assert_eq!(try_encode::<Pssz32, _>(&Empty).unwrap(), [0, 0, 0, 0]);
    for value in [
        Message::Unit,
        Message::Value(42u32),
        Message::Pair(3, "hi".into()),
        Message::Named {
            flag: true,
            name: "n".into(),
        },
    ] {
        roundtrip::<Pssz32, _>(value);
    }
    assert_eq!(
        try_encode::<Pssz32, _>(&Message::<u32>::Unit).unwrap(),
        [0]
    );
}

#[test]
fn zero_argument_and_positional_tuples_are_extensible_records() {
    assert_eq!(try_encode::<Pssz8, _>(&()).unwrap(), [0]);
    assert_eq!(try_encode::<Pssz16, _>(&()).unwrap(), [0, 0]);
    assert_eq!(try_encode::<Pssz32, _>(&()).unwrap(), [0, 0, 0, 0]);
    assert!(decode::<Pssz32, ()>(&[]).is_err());
    assert!(decode::<Pssz32, ()>(&[0, 0, 0, 0, 1]).is_err());
    assert_eq!(
        try_encode::<Pssz32, _>(&(7u32, "hi".to_string())).unwrap(),
        [8, 0, 0, 0, 7, 0, 0, 0, 8, 0, 0, 0, b'h', b'i']
    );
    roundtrip::<Pssz32, _>((7u32, "hi".to_string()));
}

#[test]
fn rejects_invalid_enums_trailing_unit_payload_and_recursive_depth() {
    assert!(decode::<Pssz32, Message<u32>>(&[0, 1]).is_err());
    assert!(decode::<Pssz32, Message<u32>>(&[4]).is_err());
    assert!(decode::<Pssz32, Message<u32>>(&[]).is_err());
    let mut chain = Node {
        value: 0,
        next: None,
    };
    for _ in 0..130 {
        chain = Node {
            value: 1,
            next: Some(Box::new(chain)),
        };
    }
    assert!(try_encode::<Pssz32, _>(&chain).is_err());
    let mut destination = vec![9, 8];
    assert!(<Node as PsszPack<Pssz32>>::pssz_pack_checked(&chain, &mut destination).is_err());
    assert_eq!(destination, [9, 8]);
    roundtrip::<Pssz32, _>(Node {
        value: 1,
        next: None,
    });
}

#[test]
fn checked_append_is_transactional_for_failing_enum_payload() {
    let value = Message::Value(vec!["x".repeat(255), "last".into()]);
    let mut output = vec![9, 8];
    assert!(
        <Message<Vec<String>> as PsszPack<Pssz8>>::pssz_pack_checked(&value, &mut output).is_err()
    );
    assert_eq!(output, [9, 8]);
}

#[cfg(feature = "experimental-frac")]
#[test]
fn frac_and_pssz_derives_coexist_on_one_type() {
    use psio::experimental::frac::{Pack, Unpack};
    #[derive(Debug, PartialEq, Pack, Unpack, PsszPack, PsszUnpack, PsszView)]
    struct Both {
        value: String,
    }
    let value = Both {
        value: "abc".into(),
    };
    let frac = value.packed();
    let pssz = try_encode::<Pssz32, _>(&value).unwrap();
    assert_ne!(frac, pssz);
    assert_eq!(Both::unpacked(&frac).unwrap(), value);
    assert_eq!(decode::<Pssz32, Both>(&pssz).unwrap(), value);
}

#[test]
fn borrowed_action_arguments_pack_like_their_owned_shapes() {
    #[derive(PsszPack)]
    struct Borrowed<'a> {
        name: &'a str,
        bytes: &'a [u8],
    }
    #[derive(PsszPack)]
    struct Owned {
        name: String,
        bytes: Vec<u8>,
    }
    assert_eq!(
        try_encode::<Pssz32, _>(&Borrowed {
            name: "hello",
            bytes: &[1, 2, 3]
        })
        .unwrap(),
        try_encode::<Pssz32, _>(&Owned {
            name: "hello".into(),
            bytes: vec![1, 2, 3]
        })
        .unwrap()
    );
}

#[test]
fn generated_paths_are_hygienic_with_shadowed_prelude_names() {
    #[allow(dead_code)]
    mod shadow {
        pub struct Vec;
        pub struct Box;
        pub struct String;
        pub struct Option;
        pub struct Some;
        pub struct None;
        pub struct Ok;
        pub struct Err;
        #[derive(
            Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::ToPsszSchema,
        )]
        #[psio(crate = "::psio")]
        pub struct Record {
            pub value: u32,
            pub suffix: ::core::option::Option<u8>,
        }
        #[derive(
            Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::ToPsszSchema,
        )]
        pub enum Choice {
            Unit,
            Pair(u8, u16),
        }
    }
    roundtrip::<Pssz32, _>(shadow::Record {
        value: 7,
        suffix: None,
    });
    roundtrip::<Pssz32, _>(shadow::Choice::Pair(1, 2));
    schema_ir::schema_for::<shadow::Record>().unwrap();
    schema_ir::schema_for::<shadow::Choice>().unwrap();
}

#[test]
fn checked_encoding_counts_present_wire_nodes_and_skips_synthesized_defaults() {
    #[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
    struct AtLimit {
        suffix: Option<u8>,
    }
    #[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
    enum Nest {
        Next(Box<Nest>),
        Leaf(AtLimit),
    }
    fn nest(value: AtLimit) -> Nest {
        let mut value = Nest::Leaf(value);
        for _ in 0..63 {
            value = Nest::Next(Box::new(value));
        }
        value
    }
    let value = nest(AtLimit { suffix: None });
    let bytes = try_encode::<Pssz32, _>(&value).unwrap();
    assert_eq!(decode_strict::<Pssz32, Nest>(&bytes).unwrap(), value);
    psio::pssz_view::PsszView::<Nest, Pssz32>::new_strict(&bytes).unwrap();
    let (schema, root) = schema_ir::schema_for::<Nest>().unwrap();
    let dynamic = dynamic::decode_exact::<Pssz32>(&schema, &root, &bytes).unwrap();
    assert_eq!(
        dynamic::encode::<Pssz32>(&schema, &root, &dynamic).unwrap(),
        bytes
    );
    let too_deep = nest(AtLimit { suffix: Some(7) });
    assert!(try_encode::<Pssz32, _>(&too_deep).is_err());
    let mut bytes = vec![0; 63]; // Nest::Next selectors; leaf selector then payload.
    bytes.push(1);
    bytes.extend(try_encode::<Pssz32, _>(&AtLimit { suffix: Some(7) }).unwrap());
    assert!(decode_strict::<Pssz32, Nest>(&bytes).is_err());
    assert!(psio::pssz_view::PsszView::<Nest, Pssz32>::new_strict(&bytes).is_err());
    assert!(dynamic::decode_exact::<Pssz32>(&schema, &root, &bytes).is_err());
    // A structurally present but unpruned None still traverses its Option
    // node at 65. The equivalent canonical pruned default above does not.
    let mut present_none = vec![0; 63];
    present_none.push(1);
    present_none.extend([4, 0, 0, 0, 4, 0, 0, 0]);
    assert!(decode_strict::<Pssz32, Nest>(&present_none).is_err());
    assert!(psio::pssz_view::PsszView::<Nest, Pssz32>::new_strict(&present_none).is_err());
    assert!(dynamic::decode_exact::<Pssz32>(&schema, &root, &present_none).is_err());
}

#[derive(Debug, PartialEq, Eq, PsszPack, PsszUnpack, PsszView)]
struct Named {
    name: String,
    value: u32,
}

#[test]
fn a_record_has_the_bytes_cpp_writes_and_named_view_accessors() {
    // C++: Named{"alice", 77} under pssz32: the header (the fixed region is
    // 8 bytes), the offset of `name`, `value`, then the bytes of `name`.
    let expected: Vec<u8> = [
        &[8u8, 0, 0, 0][..],
        &[8, 0, 0, 0],
        &[77, 0, 0, 0],
        b"alice",
    ]
    .concat();
    let named = Named { name: "alice".into(), value: 77 };
    assert_eq!(encode::<Pssz32, _>(&named), expected);
    assert_eq!(decode::<Pssz32, Named>(&expected).unwrap(), named);
    let view = PsszView::<Named, Pssz32>::new(&expected).unwrap();
    assert_eq!(view.name().view().unwrap(), "alice");
    assert_eq!(view.value().get(), 77);
}

// A packed record: its fields may be unaligned, so the generated code copies
// them instead of borrowing.
#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView)]
#[psio(definition_will_not_change)]
struct PackedFixed {
    tag: u8,
    wide: u64,
    half: u16,
}
#[repr(packed(2))]
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, PsszView)]
struct PackedOpen {
    tag: u8,
    wide: u64,
    more: Option<u32>,
}
#[repr(C, packed)]
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack)]
#[psio(transparent)]
struct PackedId(u64);

#[derive(Debug, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct UnpackedFixed {
    tag: u8,
    wide: u64,
    half: u16,
}
#[derive(Debug, PartialEq, PsszPack, PsszUnpack)]
struct UnpackedOpen {
    tag: u8,
    wide: u64,
    more: Option<u32>,
}

#[test]
fn packed_records_encode_like_unpacked_ones() {
    assert_eq!(std::mem::size_of::<PackedFixed>(), 11);
    let fixed = PackedFixed { tag: 1, wide: 0x0102030405060708, half: 0x0a0b };
    let bytes = to_pssz(&fixed);
    assert_eq!(bytes, to_pssz(&UnpackedFixed { tag: 1, wide: 0x0102030405060708, half: 0x0a0b }));
    assert_eq!(bytes.len(), 11);
    assert_eq!(from_pssz::<PackedFixed>(&bytes).unwrap(), fixed);
    let view = PsszView::<PackedFixed>::new(&bytes).unwrap();
    assert_eq!(view.wide().get(), 0x0102030405060708);

    for more in [None, Some(5)] {
        let open = PackedOpen { tag: 2, wide: 9, more };
        let bytes = to_pssz(&open);
        assert_eq!(bytes, to_pssz(&UnpackedOpen { tag: 2, wide: 9, more }));
        assert_eq!(from_pssz::<PackedOpen>(&bytes).unwrap(), open);
        validate_pssz::<PackedOpen>(&bytes).unwrap();
    }
    assert_eq!(to_pssz(&PackedId(7)), to_pssz(&7u64));
    assert_eq!(from_pssz::<PackedId>(&to_pssz(&7u64)).unwrap(), PackedId(7));
}
