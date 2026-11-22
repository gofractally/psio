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
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
#[psio(definition_will_not_change)]
struct FixedRecord {
    field: CheckedByte,
}
#[derive(Debug, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct Action {
    id: u32,
    label: String,
    active: bool,
}

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

#[derive(Debug)]
struct Huge;
impl<F: PsszFormat> PsszPack<F> for Huge {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = true;
    const FIXED_SIZE: usize = usize::MAX;
    fn pssz_size(&self) -> usize {
        usize::MAX
    }
    fn pssz_pack(&self, _: &mut Vec<u8>) {
        unreachable!()
    }
}
impl<F: PsszFormat> PsszUnpack<F> for Huge {
    fn pssz_unpack(_: &[u8]) -> PsszResult<Self> {
        Err(PsszError("huge value unsupported"))
    }
}
impl<F: PsszFormat> PsszValidate<F> for Huge {
    fn pssz_validate(_: &[u8]) -> PsszResult<()> {
        Err(PsszError("huge value unsupported"))
    }
}
#[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct HugeRecord {
    first: Huge,
    second: Huge,
}

#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct Leaf {
    id: u32,
    flag: bool,
    count: u64,
}
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct Nested {
    leaf: Leaf,
    list: Vec<Leaf>,
    note: Option<String>,
    final_value: u16,
}

fn bool_containers<F: PsszFormat>() {
    for invalid in [2, 127, 255] {
        assert!(decode::<F, Vec<bool>>(&[0, invalid]).is_err());
        assert!(decode::<F, [bool; 2]>(&[0, invalid]).is_err());
        assert!(decode::<F, Box<[bool; 2]>>(&[0, invalid]).is_err());
        assert!(validate::<F, Vec<bool>>(&[0, invalid]).is_err());
        assert!(validate::<F, [bool; 2]>(&[0, invalid]).is_err());
        assert!(PsszView::<Vec<bool>, F>::new(&[0, invalid]).is_err());
        assert!(PsszView::<[bool; 2], F>::new(&[0, invalid]).is_err());
    }
    assert_eq!(decode::<F, Vec<bool>>(&[0, 1]).unwrap(), [false, true]);
    assert_eq!(
        *decode::<F, Box<[bool; 2]>>(&[0, 1]).unwrap(),
        [false, true]
    );
    assert!(PsszView::<Vec<bool>, F>::new(&[0, 1])
        .unwrap()
        .get(1)
        .get());
}
widths!(invalid_bool_never_becomes_a_rust_bool, bool_containers);

fn zero_arrays<F: PsszFormat>() {
    assert!(decode::<F, [Zero; 3]>(&[]).is_ok());
    assert_eq!(decode::<F, Box<[Zero; 3]>>(&[]).unwrap().len(), 3);
    assert_eq!(decode::<F, Box<[Zero; 0]>>(&[]).unwrap().len(), 0);
    assert_eq!(decode::<F, Box<[String; 0]>>(&[]).unwrap().len(), 0);
    assert!(decode::<F, [String; 0]>(&[1]).is_err());
    assert!(validate::<F, [Zero; 3]>(&[]).is_ok());
    assert!(validate::<F, Box<[Zero; 0]>>(&[]).is_ok());
    assert!(PsszView::<[Zero; 3], F>::new(&[])
        .unwrap()
        .get(2)
        .bytes()
        .is_empty());
    assert!(std::panic::catch_unwind(|| encode::<F, _>(&vec![Zero])).is_err());
    assert_eq!(encode::<F, _>(&Some(Zero)), [1]); // existing MIN/presence mechanism
    assert_eq!(encode::<F, _>(&None::<Zero>), [0]);
    assert!(decode::<F, Option<Zero>>(&[1]).unwrap().is_some());
}
widths!(
    zero_sized_fixed_arrays_have_known_count_without_raw_allocation,
    zero_arrays
);

fn malformed<F: PsszFormat>() {
    let width = F::OFFSET_BYTES;
    let zeros = vec![0; width];
    assert!(decode::<F, Vec<String>>(&zeros).is_err());
    assert!(validate::<F, Vec<String>>(&zeros).is_err());
    assert!(PsszView::<Vec<String>, F>::new(&zeros).is_err());
    assert!(decode::<F, [String; 1]>(&zeros).is_err());
    assert!(validate::<F, [String; 1]>(&zeros).is_err());
    let value = vec![String::new(), "hello".to_owned(), "world".to_owned()];
    let wire = encode::<F, _>(&value);
    assert_eq!(decode::<F, Vec<String>>(&wire).unwrap(), value);
    assert!(validate::<F, Vec<String>>(&wire).is_ok());
    let view = PsszView::<Vec<String>, F>::new(&wire).unwrap();
    assert_eq!(view.get(0).view().unwrap(), "");
    assert_eq!(view.get(2).view().unwrap(), "world");
    for slot in 0..3 {
        let mut bad = wire.clone();
        bad[slot * width..(slot + 1) * width].fill(0);
        assert!(decode::<F, Vec<String>>(&bad).is_err());
        assert!(validate::<F, Vec<String>>(&bad).is_err());
    }
    assert!(decode::<F, [Huge; 2]>(&[]).is_err());
    assert!(decode::<F, Box<[Huge; 2]>>(&[]).is_err());
    assert!(validate::<F, [Huge; 2]>(&[]).is_err());
    assert!(decode::<F, HugeRecord>(&zeros).is_err());
    assert!(decode::<F, String>(&[0xff]).is_err());
    assert!(PsszView::<String, F>::new(&[0xff]).is_err());
    assert!(PsszView::<Option<String>, F>::new(&[0, 1]).is_err());
    assert!(PsszView::<BoundedString<2>, F>::new(b"long").is_err());
    let numbers = encode::<F, _>(&vec![1u32, 2, 3]);
    assert!(PsszView::<BoundedList<u32, 2>, F>::new(&numbers).is_err());
    assert!(
        std::panic::catch_unwind(|| PsszView::<Vec<String>, F>::trusted(&wire).get(usize::MAX)).is_err()
    );
    let fixed = encode::<F, _>(&[1u32, 2]);
    assert!(
        std::panic::catch_unwind(|| PsszView::<[u32; 2], F>::trusted(&fixed).get(usize::MAX)).is_err()
    );
}
widths!(malformed_layouts_and_utf8_are_rejected, malformed);

fn nested<F: PsszFormat>() {
    let value = Nested {
        leaf: Leaf {
            id: 42,
            flag: true,
            count: 9,
        },
        list: vec![Leaf {
            id: 17,
            flag: false,
            count: 5,
        }],
        note: Some("note".into()),
        final_value: 72,
    };
    let wire = encode::<F, _>(&value);
    assert_eq!(decode::<F, Nested>(&wire).unwrap(), value);
    let view = PsszView::<Nested, F>::trusted(&wire);
    assert_eq!(view.leaf().id().get(), 42);
    assert!(!view.list().get(0).flag().get());
    assert_eq!(view.note().unwrap().view().unwrap(), "note");
    assert_eq!(view.final_value().get(), 72);
    // Canonical encodings carry no trailing data: a bare scalar must occupy
    // exactly its span. (This previously asserted a lenient prefix policy.)
    assert!(decode::<F, u16>(&[1, 0, 99]).is_err());
    assert!(validate::<F, u16>(&[1, 0, 99]).is_err());
    assert_eq!(decode::<F, u16>(&[1, 0]).unwrap(), 1);
    assert!(validate::<F, u16>(&[1, 0]).is_ok());
    assert!(decode::<F, bool>(&[1, 0]).is_err());
    assert!(validate::<F, bool>(&[1, 0]).is_err());
    assert!(decode::<F, Option<u16>>(&[1, 0, 99]).is_err());
    assert!(validate::<F, Option<u16>>(&[1, 0, 99]).is_err());
    assert!(PsszView::<Option<u16>, F>::new(&[1, 0, 99]).is_err());
}
widths!(nested_record_children_use_bounded_exact_spans, nested);

use std::sync::atomic::{AtomicUsize, Ordering};
static DROPS: AtomicUsize = AtomicUsize::new(0);
struct Owned(Box<u64>);
#[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct OwnedRecord {
    owner: Owned,
    id: u8,
}
impl Drop for Owned {
    fn drop(&mut self) {
        DROPS.fetch_add(1, Ordering::SeqCst);
    }
}
impl<F: PsszFormat> PsszPack<F> for Owned {
    const MIN_ENCODED_SIZE: usize = 8;
    const MAX_ENCODED_SIZE: Option<usize> = Some(8);
    const IS_FIXED_SIZE: bool = true;
    const FIXED_SIZE: usize = 8;
    fn pssz_size(&self) -> usize {
        8
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.0.to_le_bytes());
    }
}
impl<F: PsszFormat> PsszUnpack<F> for Owned {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        let n = u64::from_le_bytes(bytes.try_into().map_err(|_| PsszError("owned span"))?);
        if n == 254 {
            panic!("custom decoder panic");
        }
        if n == 255 {
            return Err(PsszError("owned rejected"));
        }
        Ok(Self(Box::new(n)))
    }
}
// The validator checks the span only: the values the decoder refuses or
// panics on reach it, which is what `cleanup` exercises.
impl<F: PsszFormat> PsszValidate<F> for Owned {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        if bytes.len() == 8 {
            Ok(())
        } else {
            Err(PsszError("owned span"))
        }
    }
}
fn cleanup<F: PsszFormat>() {
    for failure in [254u64, 255] {
        let wire = [1u64.to_le_bytes(), failure.to_le_bytes()].concat();
        DROPS.store(0, Ordering::SeqCst);
        let result = std::panic::catch_unwind(|| decode::<F, Vec<Owned>>(&wire));
        assert!(result.is_err() || result.unwrap().is_err());
        assert_eq!(DROPS.load(Ordering::SeqCst), 1);
        DROPS.store(0, Ordering::SeqCst);
        let result = std::panic::catch_unwind(|| decode::<F, [Owned; 2]>(&wire));
        assert!(result.is_err() || result.unwrap().is_err());
        assert_eq!(DROPS.load(Ordering::SeqCst), 1);
        DROPS.store(0, Ordering::SeqCst);
        let result = std::panic::catch_unwind(|| decode::<F, Box<[Owned; 2]>>(&wire));
        assert!(result.is_err() || result.unwrap().is_err());
        assert_eq!(DROPS.load(Ordering::SeqCst), 1);
    }
    let wire = [1u64.to_le_bytes(), 2u64.to_le_bytes()].concat();
    DROPS.store(0, Ordering::SeqCst);
    let value = decode::<F, Box<[Owned; 2]>>(&wire).unwrap();
    assert_eq!(encode::<F, _>(&value), wire);
    assert_eq!(*value[1].0, 2);
    drop(value);
    assert_eq!(DROPS.load(Ordering::SeqCst), 2);
    DROPS.store(0, Ordering::SeqCst);
    let record = OwnedRecord {
        owner: Owned(Box::new(42)),
        id: 7,
    };
    let wire = encode::<F, _>(&record);
    let decoded = decode::<F, OwnedRecord>(&wire).unwrap();
    assert_eq!(*decoded.owner.0, 42);
    assert_eq!(decoded.id, 7);
    drop(decoded);
    drop(record);
    assert_eq!(DROPS.load(Ordering::SeqCst), 2);
}
widths!(non_pod_owned_values_and_partial_panic_cleanup, cleanup);

fn totality<F: PsszFormat>() {
    let mut state = 0x719ba42fu64;
    for length in 0..40 {
        for _ in 0..40 {
            let bytes: Vec<u8> = (0..length)
                .map(|_| {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    state as u8
                })
                .collect();
            let _ = decode::<F, Vec<bool>>(&bytes);
            let _ = decode::<F, Vec<String>>(&bytes);
            let _ = decode::<F, [String; 2]>(&bytes);
            let _ = decode::<F, Action>(&bytes);
            let _ = validate::<F, Vec<bool>>(&bytes);
            let _ = validate::<F, Vec<String>>(&bytes);
            let _ = validate::<F, [String; 2]>(&bytes);
            let _ = PsszView::<Vec<bool>, F>::new(&bytes);
            let _ = PsszView::<Vec<String>, F>::new(&bytes);
        }
    }
}
widths!(small_malformed_input_totality, totality);

struct InvalidWidth;
impl PsszFormat for InvalidWidth {
    const OFFSET_BYTES: usize = 0;
    const HEADER_BYTES: usize = 0;
    const MAX_TOTAL: u64 = 0;
}
struct InvalidHeader;
impl PsszFormat for InvalidHeader {
    const OFFSET_BYTES: usize = 1;
    const HEADER_BYTES: usize = 4;
    const MAX_TOTAL: u64 = u32::MAX as u64;
}
fn invalid_format<F: PsszFormat>() {
    assert!(decode::<F, Vec<String>>(&[0]).is_err());
    assert!(decode::<F, bool>(&[1]).is_err());
    assert!(validate::<F, Vec<String>>(&[0]).is_err());
    assert!(validate::<F, bool>(&[1]).is_err());
    assert!(PsszView::<bool, F>::new(&[1]).is_err());
}
#[test]
fn unsupported_public_format_metadata_returns_errors() {
    invalid_format::<InvalidWidth>();
    invalid_format::<InvalidHeader>();
}

#[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct Pair {
    first: String,
    last: String,
}
#[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
struct LargeFixed {
    bytes: [u8; 256],
}
#[test]
fn output_offsets_and_headers_reject_overflow_instead_of_truncating() {
    let value = Pair {
        first: "x".repeat(260),
        last: "last".into(),
    };
    assert!(std::panic::catch_unwind(|| encode::<Pssz8, _>(&value)).is_err());
    assert_eq!(
        decode::<Pssz16, Pair>(&encode::<Pssz16, _>(&value))
            .unwrap()
            .first,
        value.first
    );
    assert!(std::panic::catch_unwind(|| encode::<Pssz8, _>(&vec![String::new(); 256])).is_err());
    assert!(
        std::panic::catch_unwind(|| encode::<Pssz8, _>(&LargeFixed { bytes: [0; 256] })).is_err()
    );
    assert!(
        decode::<Pssz16, LargeFixed>(&encode::<Pssz16, _>(&LargeFixed { bytes: [0; 256] }))
            .is_ok()
    );
}

fn mutated_view<F: PsszFormat + std::panic::RefUnwindSafe>() {
    // A trusted view over bytes that are not a bool panics; it cannot fabricate one.
    let view = PsszView::<bool, F>::trusted(&[2]);
    assert!(std::panic::catch_unwind(|| view.get()).is_err());
    // The string view reports invalid UTF-8 as an error; it does not panic.
    assert!(std::panic::catch_unwind(|| PsszView::<String, F>::trusted(&[0xff]).view().is_err()).unwrap());
    assert!(
        std::panic::catch_unwind(|| PsszView::<Option<String>, F>::trusted(&[0, 1]).has_value())
            .is_err()
    );
    assert!(std::panic::catch_unwind(|| PsszView::<Option<u32>, F>::trusted(&[0]).has_value()).is_err());
    assert!(std::panic::catch_unwind(|| PsszView::<Option<u32>, F>::trusted(&[]).unwrap()).is_err());
    assert!(PsszView::<Bitlist<8>, F>::new(&[0]).is_err());
    assert!(PsszView::<Bitlist<0>, F>::new(&[2]).is_err());
    assert!(PsszView::<Bitvector<9>, F>::new(&[0]).is_err());
}
widths!(
    unchecked_views_reject_invalid_bytes_even_after_public_data_replacement,
    mutated_view
);
