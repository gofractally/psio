//! Safe public traits are wire contracts, not permission to reinterpret memory.
use psio::pssz::*;
use psio::pssz_view::*;

#[derive(Clone, Copy, Debug, PartialEq)]
struct CheckedByte(u8);
impl<F: PsszFormat> PsszPack<F> for CheckedByte {
    const MIN_ENCODED_SIZE: usize = 1;
    const MAX_ENCODED_SIZE: Option<usize> = Some(1);
    const IS_FIXED_SIZE: bool = true;
    const FIXED_SIZE: usize = 1;
    fn pssz_size(&self) -> usize {
        1
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.push(self.0 ^ 0x80);
    }
}
impl<F: PsszFormat> PsszUnpack<F> for CheckedByte {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if bytes != [0x81] {
            return Err(PsszError("custom byte rejected"));
        }
        Ok(Self(1))
    }
}
impl<F: PsszFormat> PsszValidate<F> for CheckedByte {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszUnpack<F>>::pssz_unpack(bytes).map(|_| ())
    }
}
#[derive(Debug, PartialEq)]
struct FixedRecord {
    field: CheckedByte,
}
psio::pssz_struct_dwnc!(FixedRecord { field: CheckedByte });
#[derive(Debug)]
struct Action {
    id: u32,
    label: String,
    active: bool,
}
psio::pssz_struct!(Action {
    id: u32,
    label: String,
    active: bool
});

#[derive(Debug)]
struct Zero;
impl<F: PsszFormat> PsszPack<F> for Zero {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = Some(0);
    const IS_FIXED_SIZE: bool = true;
    const FIXED_SIZE: usize = 0;
    fn pssz_size(&self) -> usize {
        0
    }
    fn pssz_pack(&self, _: &mut Vec<u8>) {}
}
impl<F: PsszFormat> PsszUnpack<F> for Zero {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        if bytes.is_empty() {
            Ok(Self)
        } else {
            Err(PsszError("zero span"))
        }
    }
}
impl<F: PsszFormat> PsszValidate<F> for Zero {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszUnpack<F>>::pssz_unpack(bytes).map(|_| ())
    }
}

fn trait_dispatch<F: PsszFormat>() {
    assert!(decode::<F, Vec<CheckedByte>>(&[2]).is_err());
    assert_eq!(
        decode::<F, Vec<CheckedByte>>(&[0x81]).unwrap(),
        vec![CheckedByte(1)]
    );
    assert_eq!(
        decode::<F, [CheckedByte; 1]>(&[0x81]).unwrap(),
        [CheckedByte(1)]
    );
    assert_eq!(encode::<F, _>(&vec![CheckedByte(1)]), [0x81]);
    assert_eq!(encode::<F, _>(&[CheckedByte(1)]), [0x81]);
    assert_eq!(
        encode::<F, _>(&FixedRecord {
            field: CheckedByte(1)
        }),
        [0x81]
    );
    assert_eq!(
        decode::<F, FixedRecord>(&[0x81]).unwrap().field,
        CheckedByte(1)
    );
    assert!(decode::<F, FixedRecord>(&[2]).is_err());
}
fn truncated_record<F: PsszFormat>() {
    let wire = encode::<F, _>(&Action {
        id: 7,
        label: "action".into(),
        active: true,
    });
    for n in 0..F::HEADER_BYTES + 4 + F::OFFSET_BYTES + 1 {
        let result = std::panic::catch_unwind(|| decode::<F, Action>(&wire[..n]));
        assert!(result.is_ok(), "safe decoder panicked on truncation {n}");
        assert!(result.unwrap().is_err());
    }
}
fn zero_vector<F: PsszFormat>() {
    assert!(decode::<F, Vec<Zero>>(&[]).unwrap().is_empty());
    assert!(validate::<F, Vec<Zero>>(&[]).is_ok());
    let decoded = std::panic::catch_unwind(|| decode::<F, Vec<Zero>>(&[1]));
    assert!(decoded.is_ok(), "zero-size vector divided by zero");
    assert!(decoded.unwrap().is_err());
    let validated = std::panic::catch_unwind(|| validate::<F, Vec<Zero>>(&[1]));
    assert!(validated.is_ok());
    assert!(validated.unwrap().is_err());
}
fn bool_view<F: PsszFormat>() {
    assert!(std::panic::catch_unwind(|| PsszView::<bool, F>::trusted(&[2]).get()).is_err());
}
macro_rules! widths {
    ($name:ident, $function:ident) => {
        #[test]
        fn $name() {
            $function::<Pssz8>();
            $function::<Pssz16>();
            $function::<Pssz32>();
        }
    };
}
widths!(baseline_safe_custom_traits, trait_dispatch);
widths!(baseline_safe_truncation, truncated_record);
widths!(baseline_safe_zero_vector, zero_vector);
widths!(baseline_safe_bool_view, bool_view);

