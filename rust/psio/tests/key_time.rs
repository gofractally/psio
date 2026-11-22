//! `psio::key` and `psio::time`: the properties of docs/key-spec.md and
//! docs/time.md checked on random values (the byte-level agreement with C++
//! is tests/time_key_conformance.rs).

use psio::key::{
    from_key, key_prefix, key_prefix_upper_bound, key_size, to_key, validate_key, KeyPack, KeyUnpack,
};
use psio::pssz::{Bitvector, BoundedList, BoundedRawString, BoundedString, RawString, Uint256};
use psio::time::{
    parse_time_schema_id, time_fraction_digits, time_schema_id, Duration, Micros, Millis, Nanos,
    Seconds, TimeKind, TimePoint, Unit,
};
use std::fmt::Debug;
use std::time::SystemTime;

// ── A small deterministic generator ─────────────────────────────────────────

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        // splitmix64
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// Random values that stress the framing: edges, embedded 00 / 01 / FF,
/// empty and shared prefixes.
trait Gen: Sized {
    fn gen(rng: &mut Rng) -> Self;
}
macro_rules! gen_int {
    ($($ty:ty),*) => {$(
        impl Gen for $ty {
            fn gen(rng: &mut Rng) -> Self {
                const EDGES: [$ty; 7] =
                    [<$ty>::MIN, <$ty>::MIN + 1, 0, 1, 2, <$ty>::MAX - 1, <$ty>::MAX];
                if rng.below(2) == 0 { EDGES[rng.below(7) as usize] } else { rng.next() as $ty }
            }
        }
    )*};
}
gen_int!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128);
impl Gen for bool {
    fn gen(rng: &mut Rng) -> Self {
        rng.below(2) == 1
    }
}
impl Gen for f64 {
    fn gen(rng: &mut Rng) -> Self {
        const EDGES: [f64; 10] = [
            0.0, -0.0, 1.0, -1.0, f64::INFINITY, f64::NEG_INFINITY, f64::MIN_POSITIVE, 5e-324,
            f64::MAX, f64::MIN,
        ];
        if rng.below(2) == 0 {
            EDGES[rng.below(10) as usize]
        } else {
            (rng.next() as i64) as f64 / 1048576.0
        }
    }
}
impl Gen for f32 {
    fn gen(rng: &mut Rng) -> Self {
        f64::gen(rng) as f32
    }
}
impl Gen for String {
    fn gen(rng: &mut Rng) -> Self {
        const ALPHABET: [&str; 8] = ["\0", "\u{1}", "\u{2}", "a", "b", "\u{7f}", "\u{80}", "\u{10000}"];
        (0..rng.below(5)).map(|_| ALPHABET[rng.below(8) as usize]).collect()
    }
}
impl Gen for RawString {
    fn gen(rng: &mut Rng) -> Self {
        const ALPHABET: [u8; 7] = [0x00, 0x01, 0x02, b'a', 0x7f, 0x80, 0xff];
        RawString((0..rng.below(5)).map(|_| ALPHABET[rng.below(7) as usize]).collect())
    }
}
impl<T: Gen> Gen for Option<T> {
    fn gen(rng: &mut Rng) -> Self {
        (rng.below(3) != 0).then(|| T::gen(rng))
    }
}
impl<T: Gen> Gen for Vec<T> {
    fn gen(rng: &mut Rng) -> Self {
        (0..rng.below(4)).map(|_| T::gen(rng)).collect()
    }
}
impl<T: Gen, const N: usize> Gen for [T; N] {
    fn gen(rng: &mut Rng) -> Self {
        std::array::from_fn(|_| T::gen(rng))
    }
}
impl<U: Unit, R: Gen> Gen for TimePoint<U, R> {
    fn gen(rng: &mut Rng) -> Self {
        Self::from_ticks(R::gen(rng))
    }
}
impl<U: Unit, R: Gen> Gen for Duration<U, R> {
    fn gen(rng: &mut Rng) -> Self {
        Self::from_ticks(R::gen(rng))
    }
}
macro_rules! gen_tuple {
    ($($ty:ident),*) => {
        impl<$($ty: Gen),*> Gen for ($($ty,)*) {
            #[allow(unused_variables, clippy::unused_unit)]
            fn gen(rng: &mut Rng) -> Self { ($($ty::gen(rng),)*) }
        }
    };
}
gen_tuple!();
gen_tuple!(A);
gen_tuple!(A, B);
gen_tuple!(A, B, C);
gen_tuple!(A, B, C, D);

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, psio::Key)]
struct Account {
    shard: u32,
    owner: String,
    seq: Option<i64>,
}
impl Gen for Account {
    fn gen(rng: &mut Rng) -> Self {
        Account { shard: Gen::gen(rng), owner: Gen::gen(rng), seq: Gen::gen(rng) }
    }
}

// The variant index orders first, as for C++ std::variant.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, psio::Key)]
enum Choice {
    Number(u8),
    Text(String),
    Pair((i16, String)),
}
impl Gen for Choice {
    fn gen(rng: &mut Rng) -> Self {
        match rng.below(3) {
            0 => Choice::Number(Gen::gen(rng)),
            1 => Choice::Text(Gen::gen(rng)),
            _ => Choice::Pair(Gen::gen(rng)),
        }
    }
}

/// The whole contract for one key type over random values:
///   order        a < b  <=>  key(a) < key(b);  a == b  <=>  same bytes
///   prefix-free  no key is a proper prefix of another
///   round trip, validate, size
fn check_order<T: Gen + KeyPack + KeyUnpack + PartialOrd + Debug>(count: usize) {
    let mut rng = Rng(0x5eed);
    let mut items: Vec<(T, Vec<u8>)> = Vec::new();
    for _ in 0..count {
        let value = T::gen(&mut rng);
        let key = to_key(&value);
        assert_eq!(key_size(&value), key.len(), "{value:?}");
        if let Some(fixed) = T::FIXED_SIZE {
            assert_eq!(fixed, key.len(), "{value:?}");
        }
        validate_key::<T>(&key).unwrap_or_else(|e| panic!("{value:?}: {e}"));
        let back = from_key::<T>(&key).unwrap();
        assert_eq!(to_key(&back), key, "{value:?}");
        assert!(back == value, "{value:?} decoded as {back:?}");
        if !key.is_empty() {
            assert!(validate_key::<T>(&key[..key.len() - 1]).is_err(), "{value:?} truncated");
        }
        let mut longer = key.clone();
        longer.push(0);
        assert!(validate_key::<T>(&longer).is_err(), "{value:?} with a trailing byte");
        items.push((value, key));
    }
    for (a, ka) in &items {
        for (b, kb) in &items {
            assert_eq!(a < b, ka < kb, "{a:?} vs {b:?}");
            assert_eq!(a == b, ka == kb, "{a:?} vs {b:?}");
            if ka != kb {
                assert!(!kb.starts_with(ka), "{a:?} is a prefix of {b:?}");
            }
        }
    }
    // Sorting by bytes is sorting by value.
    let mut by_value: Vec<&(T, Vec<u8>)> = items.iter().collect();
    let mut by_bytes = by_value.clone();
    by_value.sort_by(|x, y| x.0.partial_cmp(&y.0).expect("no NaN is generated"));
    by_bytes.sort_by(|x, y| x.1.cmp(&y.1));
    for (x, y) in by_value.iter().zip(&by_bytes) {
        assert_eq!(x.1, y.1);
    }
}

#[test]
fn byte_order_is_value_order_for_every_element_kind() {
    check_order::<bool>(40);
    check_order::<u8>(200);
    check_order::<i8>(200);
    check_order::<u16>(200);
    check_order::<i16>(200);
    check_order::<u32>(200);
    check_order::<i32>(200);
    check_order::<u64>(200);
    check_order::<i64>(200);
    check_order::<u128>(200);
    check_order::<i128>(200);
    check_order::<f32>(200);
    check_order::<f64>(200);
    check_order::<String>(300);
    check_order::<RawString>(300);
    check_order::<Option<String>>(300);
    check_order::<Option<Option<u8>>>(200);
    check_order::<Vec<u8>>(300);
    check_order::<Vec<i8>>(300);
    check_order::<Vec<i16>>(300);
    check_order::<Vec<bool>>(200);
    check_order::<Vec<String>>(300);
    check_order::<Vec<Vec<u8>>>(300);
    check_order::<Vec<Option<u8>>>(300);
    check_order::<[i16; 3]>(200);
    check_order::<[String; 2]>(200);
    check_order::<[u8; 3]>(200);
    check_order::<Box<String>>(100);
    check_order::<Account>(300);
    check_order::<Choice>(300);
    check_order::<TimePoint<Seconds>>(200);
    check_order::<TimePoint<Micros, u64>>(200);
    check_order::<Duration<Millis>>(200);
    check_order::<Duration<Seconds, i8>>(200);
}
impl<T: Gen> Gen for Box<T> {
    fn gen(rng: &mut Rng) -> Self {
        Box::new(T::gen(rng))
    }
}

#[test]
fn tuples_order_lexicographically_by_element() {
    // Variable-length elements followed by more elements are the hard case.
    check_order::<(String, u8)>(400);
    check_order::<(String, String)>(400);
    check_order::<(RawString, i8)>(400);
    check_order::<(Vec<u8>, i8)>(400);
    check_order::<(Vec<i16>, i8)>(400);
    check_order::<(Vec<String>, String)>(400);
    check_order::<(Option<String>, String)>(400);
    check_order::<(Option<u8>, u8)>(400);
    check_order::<(u32, String, Option<i64>)>(400);
    check_order::<(i64, u64, bool)>(400);
    check_order::<(f64, String, f32)>(400);
    check_order::<(Account, String)>(400);
    check_order::<(Choice, u8)>(400);
    check_order::<(TimePoint<Seconds>, String, Duration<Nanos>)>(400);
    check_order::<((String, u8), (), String)>(400);
}

#[test]
fn a_tuple_is_its_elements_concatenated() {
    assert!(to_key(&()).is_empty());
    assert_eq!(key_size(&()), 0);
    assert_eq!(from_key::<()>(&[]), Ok(()));
    assert!(from_key::<()>(&[0]).is_err());

    let t = (0x0102u16, "a\0b".to_string(), -2i32, Some(true));
    assert_eq!(to_key(&t), [0x01, 0x02, 0x61, 0x00, 0x01, 0x62, 0x00, 0x00, 0x7f, 0xff, 0xff, 0xfe, 0x01, 0x01]);
    assert_eq!(from_key::<(u16, String, i32, Option<bool>)>(&to_key(&t)).unwrap(), t);

    // A record is the tuple of its fields; nested tuples flatten.
    let a = Account { shard: 7, owner: "bob".into(), seq: Some(-1) };
    assert_eq!(to_key(&a), to_key(&(a.shard, a.owner.clone(), a.seq)));
    assert_eq!(to_key(&((1u8, ()), "x")), [0x01, 0x78, 0x00, 0x00]);
    // Tuples of references encode like values.
    assert_eq!(to_key(&(&a.shard, &a.owner)), to_key(&(a.shard, a.owner.clone())));

    assert_eq!(<(u32, u8, i16)>::FIXED_SIZE, Some(7));
    assert_eq!(<(u32, String)>::FIXED_SIZE, None);
    assert_eq!(<[u16; 3]>::FIXED_SIZE, Some(6));
    assert_eq!(Account::FIXED_SIZE, None);
    assert_eq!(<(TimePoint<Seconds>, u32)>::FIXED_SIZE, Some(12));
}

#[test]
fn prefix_keys() {
    type K = (u32, String, Option<i64>);
    assert!(key_prefix::<K, _>(&()).is_empty());
    // A literal is the key's u32: four bytes, not those of an i32.
    assert_eq!(key_prefix::<K, _>(&(7,)), [0, 0, 0, 7]);
    assert_ne!(key_prefix::<K, _>(&(7,)), to_key(&7i32));
    // &str stands for a String element; elements may be borrowed.
    assert_eq!(key_prefix::<K, _>(&(7, "bob")), [0, 0, 0, 7, b'b', b'o', b'b', 0, 0]);
    let owner = String::from("bob");
    assert_eq!(key_prefix::<K, _>(&(&7, &owner)), key_prefix::<K, _>(&(7, "bob")));
    assert_eq!(key_prefix::<K, _>(&(7, "bob", Some(5))), to_key(&(7u32, "bob", Some(5i64))));
    assert_eq!(key_prefix::<K, _>(&(7, "bob", None)), to_key(&(7u32, "bob", None::<i64>)));
    // A slice stands for a Vec element.
    assert_eq!(
        key_prefix::<(Vec<u8>, u8), _>(&(&[1u8, 0][..],)),
        to_key(&vec![1u8, 0])
    );
    // A derived record is a composite key: its fields are the elements.
    assert_eq!(key_prefix::<Account, _>(&(7, "bob")), key_prefix::<K, _>(&(7, "bob")));
    let a = Account { shard: 7, owner: "bob".into(), seq: Some(5) };
    assert_eq!(key_prefix::<Account, _>(&(&a.shard, &a.owner, &a.seq)), to_key(&a));

    // The key of every leading sub-tuple is a byte-prefix of the full key
    // and the full key lies in [prefix, bound).
    let mut rng = Rng(11);
    for _ in 0..500 {
        let v: (u32, String, Option<i64>, Vec<String>) = Gen::gen(&mut rng);
        let full = to_key(&v);
        type Full = (u32, String, Option<i64>, Vec<String>);
        let prefixes = [
            key_prefix::<Full, _>(&()),
            key_prefix::<Full, _>(&(&v.0,)),
            key_prefix::<Full, _>(&(&v.0, &v.1)),
            key_prefix::<Full, _>(&(&v.0, &v.1, &v.2)),
            key_prefix::<Full, _>(&(&v.0, &v.1, &v.2, &v.3)),
        ];
        assert_eq!(prefixes[1], to_key(&(v.0,)));
        assert_eq!(prefixes[2], to_key(&(v.0, &v.1)));
        assert_eq!(prefixes[3], to_key(&(v.0, &v.1, v.2)));
        assert_eq!(prefixes[4], full);
        for p in &prefixes {
            assert!(full.starts_with(p));
            assert!(p <= &full);
            if let Some(bound) = key_prefix_upper_bound(p) {
                assert!(full < bound);
            }
        }
    }
}

#[test]
fn prefix_upper_bound() {
    let bound = |p: &[u8]| key_prefix_upper_bound(p);
    assert_eq!(bound(&[]), None); // every key
    assert_eq!(bound(&[0xff]), None); // nothing is above every ff…
    assert_eq!(bound(&[0xff, 0xff, 0xff]), None);
    assert_eq!(bound(&[0x00]), Some(vec![0x01]));
    assert_eq!(bound(&[0x12, 0x34]), Some(vec![0x12, 0x35]));
    assert_eq!(bound(&[0x12, 0xff]), Some(vec![0x13])); // trailing ff dropped
    assert_eq!(bound(&[0x12, 0xff, 0xff]), Some(vec![0x13]));
    assert_eq!(bound(&[0xff, 0xfe]), Some(vec![0xff, 0xff]));
    assert_eq!(bound(&[0x7f]), Some(vec![0x80]));
    assert_eq!(bound(&[0xfe]), Some(vec![0xff]));

    // Definition: starts_with(k, p)  <=>  p <= k < bound(p), for all k.
    let mut rng = Rng(42);
    let bytes = |max: u64, rng: &mut Rng| -> Vec<u8> {
        const ALPHABET: [u8; 7] = [0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 0xff];
        (0..rng.below(max + 1)).map(|_| ALPHABET[rng.below(7) as usize]).collect()
    };
    for _ in 0..50_000 {
        let (p, k) = (bytes(3, &mut rng), bytes(5, &mut rng));
        let b = key_prefix_upper_bound(&p);
        let in_range = p <= k && b.as_ref().map_or(true, |b| &k < b);
        assert_eq!(k.starts_with(&p), in_range, "{p:x?} {k:x?}");
        if let Some(b) = b {
            assert!(p < b && !b.starts_with(&p));
        }
    }
}

#[test]
fn a_prefix_scan_returns_exactly_the_matching_rows() {
    type K = (u32, String, Option<i64>);
    let mut rng = Rng(3);
    let mut store = std::collections::BTreeMap::<Vec<u8>, K>::new();
    for _ in 0..3000 {
        let mut k: K = Gen::gen(&mut rng);
        k.0 %= 3;
        store.insert(to_key(&k), k);
    }
    let scan = |prefix: Vec<u8>| -> Vec<K> {
        match key_prefix_upper_bound(&prefix) {
            Some(bound) => store.range(prefix..bound).map(|(_, v)| v.clone()).collect(),
            None => store.range(prefix..).map(|(_, v)| v.clone()).collect(),
        }
    };
    assert_eq!(scan(key_prefix::<K, _>(&())).len(), store.len());
    for shard in 0..4u32 {
        let want: Vec<K> = store.values().filter(|k| k.0 == shard).cloned().collect();
        assert_eq!(scan(key_prefix::<K, _>(&(shard,))), want);
        for owner in ["", "a", "\0", "\u{80}", "a\0", "b"] {
            let want: Vec<K> =
                store.values().filter(|k| k.0 == shard && k.1 == owner).cloned().collect();
            assert_eq!(scan(key_prefix::<K, _>(&(shard, owner))), want);
        }
    }
    let all = scan(Vec::new());
    assert!(all.windows(2).all(|w| w[0] < w[1]));
}

#[test]
fn strings_lists_and_fixed_sequences() {
    // Empty, absent and zero are distinct and ordered.
    let (none, empty, zero) =
        (to_key(&None::<String>), to_key(&Some("")), to_key(&Some("\0")));
    assert_eq!(none, [0]);
    assert_eq!(empty, [1, 0, 0]);
    assert_eq!(zero, [1, 0, 1, 0, 0]);
    assert!(none < empty && empty < zero);

    // Octet lists share the string framing; i8 is sign-flipped so it sorts.
    assert_eq!(to_key(&vec![0x61u8, 0]), [0x61, 0x00, 0x01, 0x00, 0x00]);
    assert_eq!(to_key(&vec![-128i8, 0, 127]), [0x00, 0x01, 0x80, 0xff, 0x00, 0x00]);
    assert_eq!(from_key::<Vec<i8>>(&[0x00, 0x01, 0x80, 0xff, 0x00, 0x00]).unwrap(), [-128, 0, 127]);
    assert_eq!(to_key(&RawString(vec![b'a', 0, 0xff])), to_key(&vec![b'a', 0, 0xff]));
    assert_eq!(to_key("a\0"), to_key(&vec![b'a', 0]));
    // Other lists: 01 + element, then 00.
    assert_eq!(to_key(&vec![1u16, 2]), [1, 0, 1, 1, 0, 2, 0]);
    assert_eq!(to_key(&vec![true, false]), [1, 1, 1, 0, 0]);
    assert_eq!(to_key(&[1u16, 2][..]), to_key(&vec![1u16, 2]));
    // Fixed arrays have no framing.
    assert_eq!(to_key(&[1u16, 2]), [0, 1, 0, 2]);
    assert_eq!(to_key(&[0u8, 0, 9]), [0, 0, 9]);
    assert!(to_key(&[0u8; 0]).is_empty());

    // Bounded types: the unbounded encoding with the bound checked on read.
    let s = BoundedString::<4>("h\u{e9}".into());
    assert_eq!(to_key(&s), to_key("h\u{e9}"));
    assert_eq!(from_key::<BoundedString<4>>(&to_key("abcd")).unwrap().0, "abcd");
    assert!(from_key::<BoundedString<4>>(&to_key("abcde")).is_err());
    assert!(from_key::<String>(&[0xff, 0, 0]).is_err()); // not UTF-8
    assert_eq!(from_key::<RawString>(&[0xff, 0, 0]).unwrap().0, [0xff]);
    assert!(from_key::<BoundedRawString<1>>(&[0xff, 0xff, 0, 0]).is_err());
    let l = BoundedList::<u16, 2>(vec![1, 2]);
    assert_eq!(to_key(&l), to_key(&vec![1u16, 2]));
    assert_eq!(from_key::<BoundedList<u16, 2>>(&to_key(&l)).unwrap(), l);
    assert!(from_key::<BoundedList<u16, 2>>(&to_key(&vec![1u16, 2, 3])).is_err());
    assert!(from_key::<BoundedList<u8, 2>>(&to_key(&vec![1u8, 2, 3])).is_err());

    // Wide integers and bitvectors.
    assert_eq!(to_key(&Uint256 { limb: [1, 0, 0, 2] })[..8], [0, 0, 0, 0, 0, 0, 0, 2]);
    assert_eq!(to_key(&Uint256 { limb: [1, 0, 0, 2] })[31], 1);
    assert_eq!(
        from_key::<Uint256>(&to_key(&Uint256 { limb: [1, 2, 3, 4] })).unwrap(),
        Uint256 { limb: [1, 2, 3, 4] }
    );
    assert_eq!(to_key(&1u128)[15], 1);
    assert_eq!(to_key(&-1i128)[0], 0x7f);
    let mut bits = Bitvector::<4>::new();
    bits.set(0, true);
    bits.set(3, true);
    assert_eq!(to_key(&bits), [0x09]);
    assert_eq!(from_key::<Bitvector<4>>(&[0x09]).unwrap(), bits);
    assert!(from_key::<Bitvector<4>>(&[0x10]).is_err()); // padding bit set
}

#[test]
fn floats_where_byte_order_is_not_partial_ord() {
    // -0 and +0 are equal values and one key; it decodes to +0.
    assert_eq!(to_key(&-0.0f64), to_key(&0.0f64));
    assert!(from_key::<f64>(&to_key(&-0.0f64)).unwrap().is_sign_positive());
    assert!(from_key::<f64>(&[0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]).is_err());
    assert!(from_key::<f32>(&[0x7f, 0xff, 0xff, 0xff]).is_err());

    // NaN is unordered under PartialOrd; its key has a place. Sign clear:
    // above +inf. Sign set: below -inf. Payload bits are kept.
    let nan = f64::NAN.copysign(1.0);
    assert!(to_key(&f64::INFINITY) < to_key(&nan));
    assert!(to_key(&-nan) < to_key(&f64::NEG_INFINITY));
    assert!(from_key::<f64>(&to_key(&nan)).unwrap().is_nan());
    assert!(from_key::<f64>(&to_key(&-nan)).unwrap().is_sign_negative());
    let payload = f64::from_bits(nan.to_bits() | 1);
    assert_ne!(to_key(&payload), to_key(&nan));
    assert_eq!(from_key::<f64>(&to_key(&payload)).unwrap().to_bits(), payload.to_bits());
    let nan32 = f32::NAN.copysign(1.0);
    assert!(to_key(&f32::INFINITY) < to_key(&nan32));
    assert!(to_key(&-nan32) < to_key(&f32::NEG_INFINITY));
}

#[test]
fn reading_is_checked_and_total() {
    type K = (u16, String, Option<bool>, Vec<u16>);
    let k: K = (1, "ab".into(), Some(true), vec![5]);
    let good = to_key(&k);
    assert_eq!(good, [0, 1, 0x61, 0x62, 0, 0, 1, 1, 1, 0, 5, 0]);
    for n in 0..good.len() {
        assert!(validate_key::<K>(&good[..n]).is_err(), "prefix of length {n}");
    }
    let rejects = |bytes: &[u8]| validate_key::<K>(bytes).is_err() && from_key::<K>(bytes).is_err();
    assert!(rejects(&[0, 1, 0x61, 0x62, 0, 0, 1, 1, 1, 0, 5, 0, 0])); // trailing byte
    assert!(rejects(&[0, 1, 0x61, 0x62, 0, 2, 1, 1, 1, 0, 5, 0])); // 00 02: no such escape
    assert!(rejects(&[0, 1, 0x61, 0x62, 0])); // unterminated string
    assert!(rejects(&[0, 1, 0x61, 0x62, 0, 0, 2])); // optional marker
    assert!(rejects(&[0, 1, 0x61, 0x62, 0, 0, 1, 2, 0])); // bool
    assert!(rejects(&[0, 1, 0x61, 0x62, 0, 0, 1, 1, 2, 0, 5, 0])); // list marker
    assert!(rejects(&[0, 1, 0x61, 0x62, 0, 0, 1, 1, 1, 0])); // truncated element
    assert!(from_key::<bool>(&[2]).is_err());
    assert!(from_key::<u32>(&[0, 0, 0]).is_err());
    assert!(from_key::<u32>(&[0, 0, 0, 0, 0]).is_err());
    assert!(from_key::<Choice>(&[3, 0]).is_err()); // variant index
    assert_eq!(from_key::<Choice>(&[0, 9]).unwrap(), Choice::Number(9));

    // Random corruption: never a panic, and anything accepted is canonical
    // (it re-encodes to exactly the bytes read).
    let mut rng = Rng(7);
    let mut accepted = 0;
    for _ in 0..200_000 {
        let mut b = good.clone();
        for _ in 0..1 + rng.below(3) {
            let at = rng.below(b.len() as u64) as usize;
            b[at] = if rng.below(4) == 0 { rng.next() as u8 } else { rng.below(3) as u8 };
        }
        if rng.below(4) == 0 {
            b.truncate(rng.below(b.len() as u64 + 1) as usize);
        }
        if let Ok(value) = from_key::<K>(&b) {
            assert_eq!(to_key(&value), b);
            accepted += 1;
        }
    }
    assert!(accepted > 0);
    // The same for a type mix with every framing, on arbitrary bytes.
    type Mix = (Vec<Option<String>>, Choice, f32, Vec<i8>, Account, [bool; 2], TimePoint<Millis>);
    for _ in 0..200_000 {
        let b: Vec<u8> = (0..rng.below(24))
            .map(|_| if rng.below(3) == 0 { rng.next() as u8 } else { rng.below(2) as u8 })
            .collect();
        if let Ok(value) = from_key::<Mix>(&b) {
            assert_eq!(to_key(&value), b);
        }
    }
}

// ── Time ────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Minutes;
impl Unit for Minutes {
    const NUM: u64 = 60;
    const DEN: u64 = 1;
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Thirds;
impl Unit for Thirds {
    const NUM: u64 = 1;
    const DEN: u64 = 3;
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Deci;
impl Unit for Deci {
    const NUM: u64 = 1;
    const DEN: u64 = 10;
}

#[test]
fn time_keys_order_chronologically() {
    assert_eq!(to_key(&TimePoint::<Seconds>::from_ticks(0)), [0x80, 0, 0, 0, 0, 0, 0, 0]);
    assert_eq!(to_key(&TimePoint::<Seconds>::from_ticks(-1)), [0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
    assert_eq!(to_key(&Duration::<Millis>::from_ticks(1)), [0x80, 0, 0, 0, 0, 0, 0, 1]);
    assert_eq!(to_key(&Duration::<Seconds, u32>::from_ticks(5)), [0, 0, 0, 5]);
    assert_eq!(to_key(&TimePoint::<Seconds, i32>::from_ticks(-2)), [0x7f, 0xff, 0xff, 0xfe]);
    let (before, epoch, after) = (
        TimePoint::<Seconds>::from_ticks(-86400),
        TimePoint::<Seconds>::epoch(),
        TimePoint::<Seconds>::from_ticks(1795342530),
    );
    assert!(before < epoch && epoch < after);
    assert!(to_key(&before) < to_key(&epoch) && to_key(&epoch) < to_key(&after));
}

#[test]
fn time_text() {
    let text = |ticks: i64| TimePoint::<Seconds>::from_ticks(ticks).to_rfc3339();
    assert_eq!(text(0).unwrap(), "1970-01-01T00:00:00Z");
    assert_eq!(text(1795342530).unwrap(), "2026-11-22T10:15:30Z");
    assert_eq!(text(-1).unwrap(), "1969-12-31T23:59:59Z");
    assert_eq!(text(951782400).unwrap(), "2000-02-29T00:00:00Z");
    assert_eq!(text(4107542400).unwrap(), "2100-03-01T00:00:00Z");
    assert_eq!(text(-62167219200).unwrap(), "0000-01-01T00:00:00Z");
    assert_eq!(text(-62162035201).unwrap(), "0000-02-29T23:59:59Z");
    assert_eq!(text(253402300799).unwrap(), "9999-12-31T23:59:59Z");
    assert!(text(-62167219201).is_err());
    assert!(text(253402300800).is_err());
    assert!(text(i64::MAX).is_err() && text(i64::MIN).is_err());
    assert_eq!(TimePoint::<Millis>::from_ticks(-1).to_rfc3339().unwrap(), "1969-12-31T23:59:59.999Z");
    assert_eq!(TimePoint::<Micros>::from_ticks(0).to_rfc3339().unwrap(), "1970-01-01T00:00:00.000000Z");
    assert_eq!(TimePoint::<Nanos>::MAX.to_rfc3339().unwrap(), "2262-04-11T23:47:16.854775807Z");
    assert_eq!(TimePoint::<Nanos>::MIN.to_rfc3339().unwrap(), "1677-09-21T00:12:43.145224192Z");
    assert_eq!(TimePoint::<Deci>::from_ticks(15).to_rfc3339().unwrap(), "1970-01-01T00:00:01.5Z");
    assert_eq!(TimePoint::<Seconds, u32>::MAX.to_rfc3339().unwrap(), "2106-02-07T06:28:15Z");

    let sec = |s: &str| TimePoint::<Seconds>::from_rfc3339(s).map(|t| t.ticks());
    assert_eq!(sec("2026-11-22T10:15:30Z"), Ok(1795342530));
    assert_eq!(sec("2026-11-22t10:15:30z"), Ok(1795342530));
    assert_eq!(sec("2026-11-22T12:15:30+02:00"), Ok(1795342530));
    assert_eq!(sec("2026-11-22T04:45:30-05:30"), Ok(1795342530));
    assert_eq!(sec("2026-11-22T10:15:30.000Z"), Ok(1795342530));
    assert!(sec("2026-11-22T10:15:30.5Z").is_err());
    assert!(sec("2016-12-31T23:59:60Z").is_err());
    assert!(sec("2026-02-29T00:00:00Z").is_err());
    assert!(sec("2026-11-22T10:15:30").is_err());
    assert!(sec("2026-11-22 10:15:30Z").is_err());
    assert!(sec("0000-01-01T00:00:00+00:01").is_err());
    assert!(sec("2026-11-22T10:15:30Z\u{e9}").is_err());
    assert!(sec("2026-11-22T10:15:30+é2:00").is_err());
    assert!(TimePoint::<Seconds, u32>::from_rfc3339("2106-02-07T06:28:16Z").is_err());
    assert!(TimePoint::<Seconds, u32>::from_rfc3339("1969-12-31T23:59:59Z").is_err());

    // Round trip on random instants, every unit; never a panic on garbage.
    let mut rng = Rng(20261122);
    for _ in 0..100_000 {
        let secs = -62167219200 + rng.below(253402300800 + 62167219200) as i64;
        let t = TimePoint::<Seconds>::from_ticks(secs);
        assert_eq!(TimePoint::<Seconds>::from_rfc3339(&t.to_rfc3339().unwrap()), Ok(t));
        let us = TimePoint::<Micros>::from_ticks(secs * 1_000_000 + rng.below(1_000_000) as i64);
        assert_eq!(TimePoint::<Micros>::from_rfc3339(&us.to_rfc3339().unwrap()), Ok(us));
        let ns = TimePoint::<Nanos>::from_ticks(rng.next() as i64);
        assert_eq!(TimePoint::<Nanos>::from_rfc3339(&ns.to_rfc3339().unwrap()), Ok(ns));
        let mut text = t.to_rfc3339().unwrap().into_bytes();
        let at = rng.below(text.len() as u64) as usize;
        text[at] = b" 0159:-+.TZz/"[rng.below(13) as usize];
        if let Ok(read) = TimePoint::<Millis>::from_rfc3339(std::str::from_utf8(&text).unwrap()) {
            read.to_rfc3339().expect("an accepted text is inside the text range");
        }
    }
}

#[test]
fn time_schema_ids() {
    assert_eq!(TimePoint::<Micros>::schema_id(), "time-point-micros");
    assert_eq!(Duration::<Seconds, u8>::schema_id(), "duration-seconds");
    assert_eq!(Duration::<Minutes, i32>::schema_id(), "duration-60/1");
    assert_eq!(TimePoint::<Thirds>::schema_id(), "time-point-1/3");
    for kind in [
        TimePoint::<Seconds>::KIND,
        TimePoint::<Nanos>::KIND,
        Duration::<Millis>::KIND,
        Duration::<Minutes>::KIND,
        TimePoint::<Thirds>::KIND,
        TimeKind { point: false, num: 86400, den: 1, monotonic: false },
    ] {
        assert_eq!(parse_time_schema_id(&time_schema_id(kind)), Some(kind));
    }
    for bad in [
        "time-point", "time-point-", "duration-1/1", "duration-1/1000", "duration-2/4",
        "duration-01/7", "duration-1/0", "duration-0/1", "duration-7", "duration-1/7 ",
        "time-seconds", "Duration-seconds", "duration-minutes", "hex", "duration-+1/7",
    ] {
        assert_eq!(parse_time_schema_id(bad), None, "{bad}");
    }
    assert_eq!(time_fraction_digits(1, 1), Some(0));
    assert_eq!(time_fraction_digits(1, 1_000_000_000), Some(9));
    assert_eq!(time_fraction_digits(1, 60), None);
    assert_eq!(time_fraction_digits(60, 1), None);
}

#[test]
fn system_time_conversions_are_exact_or_fail() {
    use std::time::Duration as StdDuration;
    let epoch = SystemTime::UNIX_EPOCH;

    let t = epoch + StdDuration::new(1795342530, 123_456_000);
    assert_eq!(TimePoint::<Micros>::try_from(t).unwrap().ticks(), 1795342530123456);
    assert_eq!(TimePoint::<Nanos>::try_from(t).unwrap().ticks(), 1795342530123456000);
    // Not a whole number of milliseconds / seconds: refused, never rounded.
    assert!(TimePoint::<Millis>::try_from(t).is_err());
    assert!(TimePoint::<Seconds>::try_from(t).is_err());
    assert_eq!(TimePoint::<Seconds>::try_from(epoch + StdDuration::from_secs(60)).unwrap().ticks(), 60);
    assert_eq!(TimePoint::<Minutes>::try_from(epoch + StdDuration::from_secs(120)).unwrap().ticks(), 2);
    assert!(TimePoint::<Minutes>::try_from(epoch + StdDuration::from_secs(90)).is_err());
    // Before the epoch.
    let before = epoch - StdDuration::new(1, 500_000_000);
    assert_eq!(TimePoint::<Millis>::try_from(before).unwrap().ticks(), -1500);
    assert!(TimePoint::<Millis, u32>::try_from(before).is_err()); // unsigned ticks
    // Does not fit the tick type.
    assert!(TimePoint::<Seconds, i8>::try_from(epoch + StdDuration::from_secs(128)).is_err());
    assert_eq!(TimePoint::<Seconds, i8>::try_from(epoch + StdDuration::from_secs(127)).unwrap().ticks(), 127);

    // And back.
    assert_eq!(SystemTime::try_from(TimePoint::<Micros>::from_ticks(1795342530123456)).unwrap(), t);
    assert_eq!(SystemTime::try_from(TimePoint::<Millis>::from_ticks(-1500)).unwrap(), before);
    assert_eq!(SystemTime::try_from(TimePoint::<Minutes>::from_ticks(2)).unwrap(), epoch + StdDuration::from_secs(120));
    // A third of a second is not a whole number of nanoseconds.
    assert!(SystemTime::try_from(TimePoint::<Thirds>::from_ticks(1)).is_err());
    assert_eq!(SystemTime::try_from(TimePoint::<Thirds>::from_ticks(3)).unwrap(), epoch + StdDuration::from_secs(1));
    // Round trip.
    let mut rng = Rng(9);
    for _ in 0..10_000 {
        let tp = TimePoint::<Micros>::from_ticks((rng.next() >> 14) as i64 - (1 << 48));
        assert_eq!(TimePoint::<Micros>::try_from(SystemTime::try_from(tp).unwrap()), Ok(tp));
    }

    // Durations.
    assert_eq!(Duration::<Millis>::try_from(StdDuration::from_millis(1500)).unwrap().ticks(), 1500);
    assert!(Duration::<Seconds>::try_from(StdDuration::from_millis(1500)).is_err());
    assert!(Duration::<Seconds, u8>::try_from(StdDuration::from_secs(256)).is_err());
    assert_eq!(StdDuration::try_from(Duration::<Minutes, i32>::from_ticks(2)).unwrap(), StdDuration::from_secs(120));
    assert!(StdDuration::try_from(Duration::<Seconds>::from_ticks(-1)).is_err()); // negative
    assert_eq!(StdDuration::try_from(Duration::<Nanos, u64>::MAX).unwrap(), StdDuration::from_nanos(u64::MAX));
}

#[test]
fn time_json_and_pssz() {
    use psio::json::{self, IJson, Json};
    use psio::pssz::{from_pssz, to_pssz, decode, Pssz, PsszPack};

    let t = TimePoint::<Millis>::from_ticks(1795342530123);
    assert_eq!(psio::to_json(&t), "\"2026-11-22T10:15:30.123Z\"");
    assert_eq!(psio::to_ijson(&t), "\"2026-11-22T10:15:30.123Z\"");
    assert_eq!(psio::from_json::<TimePoint<Millis>>("\"2026-11-22T12:15:30.123+02:00\""), Ok(t));
    assert_eq!(psio::from_ijson::<TimePoint<Millis>>(" \"2026-11-22T10:15:30.123000\\u005a\" "), Ok(t));
    assert!(psio::from_json::<TimePoint<Millis>>("1795342530123").is_err());
    assert!(psio::from_json::<TimePoint<Millis>>("\"2026-11-22T10:15:30.1234Z\"").is_err());
    // No text outside years 0000..9999: an error, not a panic.
    assert!(psio::try_to_json(&TimePoint::<Seconds>::MAX).is_err());
    assert!(psio::try_to_ijson(&vec![TimePoint::<Micros>::MIN]).is_err());

    // A duration follows the integer rules of the format.
    let d = Duration::<Nanos>::from_ticks(9007199254740993);
    assert_eq!(psio::to_json(&d), "9007199254740993");
    assert_eq!(psio::to_ijson(&d), "\"9007199254740993\"");
    assert_eq!(psio::from_ijson::<Duration<Nanos>>("\"9007199254740993\""), Ok(d));
    assert!(psio::from_ijson::<Duration<Nanos>>("9007199254740993").is_err());
    assert!(psio::from_json::<Duration<Nanos>>("\"PT1S\"").is_err());
    assert_eq!(psio::to_json(&Duration::<Minutes, i32>::from_ticks(-90)), "-90");
    // A list of one-byte tick counts is a list of numbers, not a byte string.
    let bytes = vec![Duration::<Seconds, u8>::from_ticks(1), Duration::from_ticks(255)];
    assert_eq!(psio::to_ijson(&bytes), "[1,255]");
    assert_eq!(psio::from_ijson::<Vec<Duration<Seconds, u8>>>("[1,255]"), Ok(bytes));

    // pssz: the tick integer, fixed size.
    assert_eq!(to_pssz(&TimePoint::<Seconds>::from_ticks(1795342530)), 1795342530i64.to_le_bytes());
    assert_eq!(to_pssz(&Duration::<Minutes, i32>::from_ticks(-90)), (-90i32).to_le_bytes());
    assert!(<TimePoint<Micros> as PsszPack<Pssz>>::IS_FIXED_SIZE);
    assert_eq!(<TimePoint<Seconds, u32> as PsszPack<Pssz>>::FIXED_SIZE, 4);
    assert_eq!(from_pssz::<TimePoint<Micros>>(&(-1i64).to_le_bytes()).unwrap().ticks(), -1);
    assert!(from_pssz::<TimePoint<Micros>>(&[0; 7]).is_err());
    let list = vec![TimePoint::<Seconds>::MIN, TimePoint::epoch(), TimePoint::MAX];
    assert_eq!(decode::<Pssz, Vec<TimePoint<Seconds>>>(&to_pssz(&list)).unwrap(), list);
    assert_eq!(to_pssz(&list).len(), 24);
}

// ── Monotonic time points (a clock other than the system clock) ────────────
// The Rust side of the two monotonic test cases of cpp/tests/time_tests.cpp.

mod monotonic {
    use super::Minutes;
    use psio::json::{self, IJson, Json};
    use psio::key::{from_key, to_key, validate_key};
    use psio::pssz::schema_ir::{schema_for, AnyValue, ToPsszSchema};
    use psio::pssz::{try_to_pssz, 
        decode_strict, pssz_evolves_from, validate, Pssz, PsszPack,
    };
    use psio::time::{
        parse_time_schema_id, reserved_time_schema_id, time_schema_id, Duration, Micros, Millis,
        MonotonicPoint, Nanos, Seconds, TimePoint, Unit,
    };

    #[derive(
        Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
        psio::ToPsszSchema, psio::Key,
    )]
    struct Timer {
        expires: MonotonicPoint<Micros>,
        started: Option<MonotonicPoint<Seconds>>,
        laps: Vec<MonotonicPoint<Millis, u32>>,
    }
    #[derive(
        Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
        psio::ToPsszSchema, psio::Key,
    )]
    struct TimerInts {
        expires: i64,
        started: Option<i64>,
        laps: Vec<u32>,
    }

    // Member integer-form attributes apply to the tick count.
    #[derive(Debug, Clone, PartialEq, psio::JsonText)]
    struct Forms {
        #[json(json_string, ijson_string)]
        quoted: MonotonicPoint<Seconds>,
        #[json(json_number, ijson_number)]
        plain: MonotonicPoint<Nanos>,
    }
    #[derive(Debug, Clone, PartialEq, psio::JsonText)]
    struct FormsInts {
        #[json(json_string, ijson_string)]
        quoted: i64,
        #[json(json_number, ijson_number)]
        plain: i64,
    }

    // A time value is its own type in an evolution check.
    const _: () = {
        assert!(<MonotonicPoint<Millis, u32> as PsszPack<Pssz>>::IS_FIXED_SIZE);
        assert!(<MonotonicPoint<Millis, u32> as PsszPack<Pssz>>::FIXED_SIZE == 4);
        assert!(pssz_evolves_from::<MonotonicPoint<Micros>, MonotonicPoint<Micros>>());
        assert!(!pssz_evolves_from::<MonotonicPoint<Micros>, TimePoint<Micros>>());
        assert!(!pssz_evolves_from::<TimePoint<Micros>, MonotonicPoint<Micros>>());
        assert!(!pssz_evolves_from::<MonotonicPoint<Micros>, Duration<Micros>>());
        assert!(!pssz_evolves_from::<Duration<Micros>, MonotonicPoint<Micros>>());
        assert!(!pssz_evolves_from::<MonotonicPoint<Micros>, i64>());
        assert!(!pssz_evolves_from::<i64, MonotonicPoint<Micros>>());
        assert!(!pssz_evolves_from::<MonotonicPoint<Micros>, MonotonicPoint<Millis>>());
        assert!(!pssz_evolves_from::<MonotonicPoint<Seconds>, MonotonicPoint<Seconds, u64>>());
        assert!(!pssz_evolves_from::<TimerInts, Timer>());
        assert!(pssz_evolves_from::<Timer, Timer>());
    };

    #[test]
    fn a_monotonic_time_point_is_its_tick_count_in_every_format() {
        let timer = Timer {
            expires: MonotonicPoint::from_ticks(9007199254740993),
            started: Some(MonotonicPoint::from_ticks(-5)),
            laps: vec![MonotonicPoint::from_ticks(0), MonotonicPoint::MAX],
        };
        let ints =
            TimerInts { expires: 9007199254740993, started: Some(-5), laps: vec![0, 4294967295] };

        // Binary and key: the tick integer.
        let bytes = try_to_pssz(&timer).unwrap();
        assert_eq!(bytes, try_to_pssz(&ints).unwrap());
        assert_eq!(to_key(&timer), to_key(&ints));
        validate::<Pssz, Timer>(&bytes).unwrap();
        assert_eq!(decode_strict::<Pssz, Timer>(&bytes).unwrap(), timer);
        let key = to_key(&timer);
        validate_key::<Timer>(&key).unwrap();
        assert_eq!(from_key::<Timer>(&key).unwrap(), timer);
        // Key order is tick order.
        let before = MonotonicPoint::<Micros>::from_ticks(-1);
        let after = MonotonicPoint::<Micros>::default();
        assert!(before < after && to_key(&before) < to_key(&after));
        assert_eq!(
            try_to_pssz(&MonotonicPoint::<Nanos>::from_ticks(7)).unwrap(),
            7i64.to_le_bytes()
        );
        // Hostile bytes are refused, never read out of bounds.
        assert!(decode_strict::<Pssz, MonotonicPoint<Micros>>(&[0; 7]).is_err());
        assert!(decode_strict::<Pssz, MonotonicPoint<Micros>>(&[0; 9]).is_err());
        assert!(from_key::<MonotonicPoint<Micros>>(&[0x80; 7]).is_err());
        // Truncations and mutations are decided without a panic, and what
        // the checked reader accepts re-encodes.
        for cut in 0..bytes.len() {
            let _ = decode_strict::<Pssz, Timer>(&bytes[..cut]);
            for bit in 0..8 {
                let mut mutated = bytes.clone();
                mutated[cut] ^= 1 << bit;
                if let Ok(value) = decode_strict::<Pssz, Timer>(&mutated) {
                    assert!(validate::<Pssz, Timer>(&mutated).is_ok(), "{cut}.{bit}");
                    try_to_pssz(&value).unwrap();
                }
                let mut key = to_key(&timer);
                if cut < key.len() {
                    key[cut] ^= 1 << bit;
                    assert_eq!(validate_key::<Timer>(&key).is_ok(), from_key::<Timer>(&key).is_ok());
                }
            }
        }

        // Text: the tick count under the integer rules, never a date.
        let text = psio::to_json(&timer);
        let itext = psio::to_ijson(&timer);
        assert_eq!(text, r#"{"expires":9007199254740993,"started":-5,"laps":[0,4294967295]}"#);
        assert_eq!(itext, r#"{"expires":"9007199254740993","started":-5,"laps":[0,4294967295]}"#);
        assert_eq!(text, psio::to_json(&ints));
        assert_eq!(itext, psio::to_ijson(&ints));
        assert_eq!(psio::from_json::<Timer>(&text), Ok(timer.clone()));
        assert_eq!(psio::from_ijson::<Timer>(&itext), Ok(timer.clone()));
        let date = r#"{"expires":"2026-11-22T10:15:30Z","started":null,"laps":[]}"#;
        assert!(psio::from_json::<Timer>(date).is_err());
        assert!(psio::from_ijson::<Timer>(date).is_err());
        assert!(psio::validate_ijson::<Timer>(date.as_bytes()).is_err());
        // ijson: a wide tick count beyond 2^53 - 1 is a string, not a number.
        assert!(psio::from_ijson::<MonotonicPoint<Micros>>("9007199254740993").is_err());

        let forms =
            Forms { quoted: MonotonicPoint::from_ticks(7), plain: MonotonicPoint::MAX };
        let form_ints = FormsInts { quoted: 7, plain: i64::MAX };
        for ijson in [false, true] {
            let (a, b) = if ijson {
                (psio::try_to_ijson(&forms), psio::try_to_ijson(&form_ints))
            } else {
                (psio::try_to_json(&forms), psio::try_to_json(&form_ints))
            };
            let a = a.unwrap();
            assert_eq!(a, b.unwrap());
            assert_eq!(a, r#"{"quoted":"7","plain":9223372036854775807}"#);
        }
        let text = psio::to_ijson(&forms);
        assert_eq!(psio::from_ijson::<Forms>(&text), Ok(forms));
    }

    /// `monotonic-<unit>` is a built-in presentation: the schema-driven
    /// codec presents the ticks, with no handler and under a strict registry.
    #[test]
    fn the_schema_driven_codec_presents_ticks() {
        use psio::pssz::dynamic::{self, CustomHandlers};
        let (schema, root) = schema_for::<Timer>().unwrap();
        let timer = Timer {
            expires: MonotonicPoint::from_ticks(9007199254740993),
            started: Some(MonotonicPoint::from_ticks(-5)),
            laps: vec![MonotonicPoint::from_ticks(0), MonotonicPoint::MAX],
        };
        let bytes = try_to_pssz(&timer).unwrap();
        let text = psio::to_json(&timer);
        let itext = psio::to_ijson(&timer);
        assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
        assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, text);
        for strict in [false, true] {
            let mut handlers = CustomHandlers::new();
            handlers.set_strict(strict);
            // A handler under a time id is never consulted.
            handlers.add(
                "monotonic-micros",
                |_: &[u8]| Ok("hijacked".to_string()),
                |_: &str| Ok(vec![0u8; 8]),
            );
            assert_eq!(
                dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &handlers).unwrap().0,
                text
            );
            assert_eq!(
                dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &handlers).unwrap().0,
                itext
            );
            assert_eq!(
                dynamic::json_to_pssz_with::<Json>(&schema, &root, text.as_bytes(), &handlers)
                    .unwrap(),
                bytes
            );
            assert_eq!(
                dynamic::json_to_pssz_with::<IJson>(&schema, &root, itext.as_bytes(), &handlers)
                    .unwrap(),
                bytes
            );
        }
        for bad in [
            r#"{"expires":"2026-11-22T10:15:30Z","started":null,"laps":[]}"#,
            r#"{"expires":0,"started":"1970-01-01T00:00:00Z","laps":[]}"#,
            r#"{"expires":0,"started":null,"laps":[4294967296]}"#,
            r#"{"expires":0.5,"started":null,"laps":[]}"#,
        ] {
            assert!(psio::from_json::<Timer>(bad).is_err(), "{bad}");
            assert!(dynamic::json_to_pssz::<Json>(&schema, &root, bad.as_bytes()).is_err(), "{bad}");
            assert!(dynamic::json_to_pssz::<IJson>(&schema, &root, bad.as_bytes()).is_err(), "{bad}");
        }
    }

    #[test]
    fn ordering_and_arithmetic_with_durations() {
        type Point = MonotonicPoint<Millis, u32>;
        let (a, b) = (Point::from_ticks(10), Point::from_ticks(25));
        assert!(a < b && Point::MIN < a && b < Point::MAX);
        assert_eq!(Point::default().ticks(), 0);
        assert_eq!(a.checked_add(Duration::from_ticks(15)), Some(b));
        assert_eq!(b.checked_sub(Duration::from_ticks(15)), Some(a));
        assert_eq!(b.checked_duration_since(a), Some(Duration::from_ticks(15)));
        // Exact or nothing: the result must fit the tick type.
        assert_eq!(a.checked_duration_since(b), None);
        assert_eq!(Point::MAX.checked_add(Duration::from_ticks(1)), None);
        assert_eq!(a.checked_sub(Duration::from_ticks(11)), None);
        let (c, d) = (MonotonicPoint::<Nanos>::MIN, MonotonicPoint::<Nanos>::MAX);
        assert_eq!(d.checked_duration_since(c), None);
        assert_eq!(c.checked_duration_since(c), Some(Duration::from_ticks(0)));
        assert_eq!(c.checked_add(Duration::MAX), Some(MonotonicPoint::from_ticks(-1)));
        // A time point subtracts the same way.
        let (t0, t1) = (TimePoint::<Seconds>::from_ticks(-5), TimePoint::<Seconds>::from_ticks(5));
        assert_eq!(t1.checked_duration_since(t0), Some(Duration::from_ticks(10)));
        assert_eq!(t0.checked_add(Duration::from_ticks(10)), Some(t1));
        assert_eq!(t1.checked_sub(Duration::from_ticks(10)), Some(t0));
        assert_eq!(TimePoint::<Seconds>::MIN.checked_sub(Duration::from_ticks(1)), None);
    }

    #[test]
    fn the_schema_marks_a_monotonic_time_point_with_its_own_id() {
        fn check<U: Unit, R: psio::time::Tick>(id: &str, bits: u32, signed: bool) {
            let (schema, root) = schema_for::<MonotonicPoint<U, R>>().expect("schema");
            let root = schema.resolve(&root).expect("resolve");
            let AnyValue::Custom(custom) = &root.value else { panic!("not Custom: {root:?}") };
            assert_eq!(custom.id, id);
            assert_eq!(MonotonicPoint::<U, R>::schema_id(), id);
            let inner = schema.resolve(&custom.r#type).expect("resolve");
            let AnyValue::Int(int) = &inner.value else { panic!("not Int") };
            assert_eq!((int.bits, int.isSigned), (bits, signed), "{id}");
            let kind = parse_time_schema_id(id).expect(id);
            assert!(kind.monotonic && !kind.point, "{id}");
            assert_eq!((kind.num, kind.den), (U::NUM, U::DEN), "{id}");
            assert_eq!(kind, MonotonicPoint::<U, R>::KIND);
            assert_eq!(time_schema_id(kind), id);
            assert!(reserved_time_schema_id(id));
        }
        check::<Seconds, i64>("monotonic-seconds", 64, true);
        check::<Micros, i64>("monotonic-micros", 64, true);
        check::<Millis, u32>("monotonic-millis", 32, false);
        check::<Nanos, i64>("monotonic-nanos", 64, true);
        check::<Minutes, i32>("monotonic-60/1", 32, true);
        // A time point of a unit with no RFC 3339 form still has tick text.
        assert_eq!(
            psio::to_json(&MonotonicPoint::<Minutes, i32>::default()),
            "0"
        );
        assert_eq!(psio::from_ijson::<MonotonicPoint<Minutes, i32>>("-90").unwrap().ticks(), -90);

        for bad in [
            "monotonic", "monotonic-", "monotonic-1/1000", "monotonic-2/4", "monotonic-minutes",
            "Monotonic-seconds",
        ] {
            assert_eq!(parse_time_schema_id(bad), None, "{bad}");
        }
        assert!(reserved_time_schema_id("monotonic"));
        assert!(reserved_time_schema_id("monotonic-anything"));
        assert!(!reserved_time_schema_id("monotonically"));
        // A time point and a monotonic time point of one unit are different ids.
        assert_ne!(TimePoint::<Micros>::schema_id(), MonotonicPoint::<Micros>::schema_id());
        fn _is_schema<T: ToPsszSchema>() {}
        _is_schema::<Timer>();
    }
}
