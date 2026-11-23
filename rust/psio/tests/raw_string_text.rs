//! The text conveniences of the raw string types (`RawString`,
//! `BoundedRawString<N>`, `RawStr`): a raw string is any bytes that are
//! usually text, and it reads like text without a helper. None of it takes
//! part in the wire, key, JSON or schema form.

use std::borrow::{Borrow, Cow};
use std::collections::hash_map::DefaultHasher;
use std::collections::{BTreeSet, HashMap};
use std::hash::{Hash, Hasher};

use psio::pssz::{BoundedRawString, RawStr, RawString};

const TEXT: &str = "t\u{e9}xt";
/// Not UTF-8: a lone 0xff between two ASCII bytes.
const BYTES: &[u8] = b"a\xffb";

type Short = BoundedRawString<8>;

fn hash_of<T: Hash + ?Sized>(value: &T) -> u64 {
    let mut h = DefaultHasher::new();
    value.hash(&mut h);
    h.finish()
}

/// Every raw string type holding `bytes`, checked by one body.
macro_rules! for_each_raw {
    ($bytes:expr, |$v:ident| $body:block) => {{
        let bytes: &[u8] = $bytes;
        { let $v = RawString::from(bytes); $body }
        { let $v = RawStr(bytes); $body }
        { let $v = Short::try_from(bytes).unwrap(); $body }
    }};
}

#[test]
fn bytes_and_length() {
    for_each_raw!(TEXT.as_bytes(), |v| {
        assert_eq!(v.as_bytes(), TEXT.as_bytes());
        assert_eq!(v.len(), 5); // bytes, not chars
        assert!(!v.is_empty());
    });
    for_each_raw!(b"", |v| {
        assert_eq!(v.len(), 0);
        assert!(v.is_empty());
    });
    assert!(RawString::new().is_empty());
    assert!(Short::new().is_empty());
}

#[test]
fn text_access_is_checked_or_lossy() {
    for_each_raw!(TEXT.as_bytes(), |v| {
        assert_eq!(v.to_str().unwrap(), TEXT);
        assert!(matches!(v.to_string_lossy(), Cow::Borrowed(TEXT)));
    });
    for_each_raw!(BYTES, |v| {
        assert_eq!(v.to_str().unwrap_err().to_string(), "pssz: invalid UTF-8 in string");
        assert_eq!(v.to_string_lossy(), "a\u{fffd}b");
        assert_eq!(v.to_string_lossy(), String::from_utf8_lossy(BYTES));
    });
}

#[test]
fn display_is_lossy() {
    for_each_raw!(TEXT.as_bytes(), |v| {
        assert_eq!(format!("{v}"), TEXT);
        assert_eq!(v.to_string(), TEXT);
        assert_eq!(format!("[{v:>7}]"), "[   t\u{e9}xt]");
        assert_eq!(format!("[{v:.2}]"), "[t\u{e9}]");
    });
    for_each_raw!(BYTES, |v| {
        assert_eq!(format!("{v}"), "a\u{fffd}b");
    });
    for_each_raw!(b"", |v| {
        assert_eq!(format!("{v}"), "");
    });
}

#[test]
fn debug_is_text_or_escaped_bytes() {
    assert_eq!(format!("{:?}", RawString::from("a \"q\"\n")), r#"RawString("a \"q\"\n")"#);
    assert_eq!(format!("{:?}", RawStr(TEXT.as_bytes())), "RawStr(\"t\u{e9}xt\")");
    assert_eq!(format!("{:?}", Short::try_from("abc").unwrap()), r#"BoundedRawString("abc")"#);
    assert_eq!(format!("{:?}", RawString::new()), r#"RawString("")"#);

    assert_eq!(format!("{:?}", RawString::from(BYTES)), r#"RawString(b"a\xffb")"#);
    assert_eq!(format!("{:?}", RawStr(b"\x00\"\xc0\x80")), r#"RawStr(b"\x00\"\xc0\x80")"#);
    assert_eq!(format!("{:?}", Short::try_from(BYTES).unwrap()), r#"BoundedRawString(b"a\xffb")"#);
    // In a derived Debug of a record.
    #[derive(Debug)]
    #[allow(dead_code)]
    struct Memo { memo: RawString }
    assert_eq!(format!("{:?}", Memo { memo: "hi".into() }), r#"Memo { memo: RawString("hi") }"#);
}

#[test]
fn equality_with_text_and_bytes_both_ways() {
    for_each_raw!(TEXT.as_bytes(), |v| {
        let string = TEXT.to_string();
        let vec = TEXT.as_bytes().to_vec();
        let slice: &[u8] = TEXT.as_bytes();
        // raw == other
        assert!(v == *TEXT);
        assert!(v == TEXT);
        assert!(v == string);
        assert!(v == *slice);
        assert!(v == slice);
        assert!(v == vec);
        // other == raw
        assert!(*TEXT == v);
        assert!(TEXT == v);
        assert!(string == v);
        assert!(*slice == v);
        assert!(slice == v);
        assert!(vec == v);
        // and the negatives
        assert!(v != "text");
        assert!("text" != v);
        assert!(v != String::new());
        assert!(v != &b"t"[..]);
        assert_eq!(v, TEXT);
        assert_ne!(v, "");
    });
    for_each_raw!(b"abc", |v| {
        assert!(v == b"abc");
        assert!(v == *b"abc");
        assert!(b"abc" == v);
        assert!(*b"abc" == v);
        assert!(v != b"abd");
        assert!(v != b"ab");
    });
    // Bytes that are not text equal bytes, and never the lossy text.
    for_each_raw!(BYTES, |v| {
        assert!(v == BYTES);
        assert!(BYTES == v);
        assert!(v != "a\u{fffd}b");
        assert!(v != v.to_string_lossy().into_owned());
    });
    // Owned and borrowed compare.
    assert!(RawString::from(BYTES) == RawStr(BYTES));
    assert!(RawStr(BYTES) == RawString::from(BYTES));
    assert!(RawString::from("a") != RawStr(b"b"));
}

#[test]
fn unbounded_conversions() {
    assert_eq!(RawString::from("abc").0, b"abc");
    assert_eq!(RawString::from(String::from("abc")).0, b"abc");
    assert_eq!(RawString::from(BYTES.to_vec()).0, BYTES);
    assert_eq!(RawString::from(BYTES).0, BYTES);
    assert_eq!(RawString::from(RawStr(BYTES)).0, BYTES);
    let into: RawString = "abc".into();
    assert_eq!(into, "abc");

    assert_eq!(RawString::from(BYTES).into_bytes(), BYTES);
    assert_eq!(Vec::<u8>::from(RawString::from(BYTES)), BYTES);

    assert_eq!(RawStr::from("abc").0, b"abc");
    assert_eq!(RawStr::from(BYTES).0, BYTES);
    let owned = RawString::from(BYTES);
    assert_eq!(RawStr::from(&owned), owned.as_raw_str());
    assert_eq!(owned.as_raw_str().to_owned(), owned);
}

#[test]
fn string_from_a_raw_string_fails_on_non_utf8() {
    assert_eq!(String::try_from(RawString::from(TEXT)).unwrap(), TEXT);
    assert_eq!(String::try_from(Short::try_from(TEXT).unwrap()).unwrap(), TEXT);
    let e = String::try_from(RawString::from(BYTES)).unwrap_err();
    assert_eq!(e.utf8_error().valid_up_to(), 1);
    assert_eq!(e.into_bytes(), BYTES); // the bytes come back
    assert!(String::try_from(Short::try_from(BYTES).unwrap()).is_err());
    for bad in [&b"\xff"[..], b"a\xc0\x80", b"\xed\xa0\x80"] {
        assert!(String::try_from(RawString::from(bad)).is_err());
    }
}

#[test]
fn bounded_constructors_enforce_the_bound() {
    let fits = "12345678";
    let over = "123456789";
    assert_eq!(Short::try_from(fits).unwrap(), fits);
    assert_eq!(Short::try_from(fits.to_string()).unwrap(), fits);
    assert_eq!(Short::try_from(fits.as_bytes()).unwrap(), fits);
    assert_eq!(Short::try_from(fits.as_bytes().to_vec()).unwrap(), fits);
    assert_eq!(Short::try_from(RawString::from(fits)).unwrap(), fits);
    assert_eq!(Short::try_from_bytes(fits.into()).unwrap(), fits);

    let overflow = "pssz: BoundedRawString overflow";
    assert_eq!(Short::try_from(over).unwrap_err().to_string(), overflow);
    assert_eq!(Short::try_from(over.to_string()).unwrap_err().to_string(), overflow);
    assert_eq!(Short::try_from(over.as_bytes()).unwrap_err().to_string(), overflow);
    assert_eq!(Short::try_from(over.as_bytes().to_vec()).unwrap_err().to_string(), overflow);
    assert_eq!(Short::try_from(RawString::from(over)).unwrap_err().to_string(), overflow);
    assert_eq!(Short::try_from_bytes(over.into()).unwrap_err().to_string(), overflow);
    // The bound counts bytes: four two-byte characters fit, five do not.
    assert!(Short::try_from("\u{e9}\u{e9}\u{e9}\u{e9}").is_ok());
    assert!(Short::try_from("\u{e9}\u{e9}\u{e9}\u{e9}\u{e9}").is_err());
    assert!(BoundedRawString::<0>::try_from("").is_ok());
    assert!(BoundedRawString::<0>::try_from("a").is_err());

    // Non-UTF-8 bytes are welcome within the bound.
    let raw = Short::try_from(BYTES).unwrap();
    assert_eq!(raw.as_raw_str(), RawStr(BYTES));
    assert_eq!(RawString::from(raw.clone()), BYTES);
    assert_eq!(Vec::<u8>::from(raw.clone()), BYTES);
    assert_eq!(raw.into_bytes(), BYTES);

    // The public field can still hold too much; every encoder refuses it.
    let too_long = BoundedRawString::<2>(b"abc".to_vec());
    assert!(psio::pssz::try_to_pssz(&too_long).is_err());
    assert!(psio::try_to_json(&too_long).is_err());
    assert!(psio::key::try_to_key(&too_long).is_err());
}

#[test]
fn derefs_and_borrows_as_bytes() {
    fn takes_bytes(bytes: impl AsRef<[u8]>) -> usize { bytes.as_ref().len() }
    for_each_raw!(BYTES, |v| {
        // Deref<Target = [u8]>: slice methods apply.
        assert!(v.starts_with(b"a"));
        assert_eq!(v[1], 0xff);
        assert_eq!(v.iter().copied().max(), Some(0xff));
        assert_eq!(v.to_vec(), BYTES);
        let slice: &[u8] = &v;
        assert_eq!(slice, BYTES);
        let as_ref: &[u8] = v.as_ref();
        assert_eq!(as_ref, BYTES);
        let borrowed: &[u8] = v.borrow();
        assert_eq!(borrowed, BYTES);
        assert_eq!(takes_bytes(&v), 3);
    });
    assert_eq!(takes_bytes(RawString::from("abcd")), 4);
}

#[test]
fn hash_and_order_are_those_of_the_bytes() {
    let samples: [&[u8]; 7] = [b"", b"a", b"a\xff", b"ab", b"b", b"\xff", TEXT.as_bytes()];
    for a in samples {
        // Borrow<[u8]> requires the hash of the bytes.
        assert_eq!(hash_of(&RawString::from(a)), hash_of(a));
        assert_eq!(hash_of(&RawStr(a)), hash_of(a));
        assert_eq!(hash_of(&Short::try_from(a).unwrap()), hash_of(a));
        for b in samples {
            assert_eq!(RawString::from(a).cmp(&RawString::from(b)), a.cmp(b));
            assert_eq!(RawStr(a).cmp(&RawStr(b)), a.cmp(b));
            assert_eq!(Short::try_from(a).unwrap().cmp(&Short::try_from(b).unwrap()), a.cmp(b));
            assert_eq!(RawString::from(a) == RawString::from(b), a == b);
        }
    }
    // So maps and sets keyed by a raw string are looked up by bytes.
    let mut map: HashMap<RawString, u32> = HashMap::new();
    map.insert("name".into(), 1);
    map.insert(BYTES.into(), 2);
    assert_eq!(map.get(&b"name"[..]), Some(&1));
    assert_eq!(map.get("name".as_bytes()), Some(&1));
    assert_eq!(map.get(BYTES), Some(&2));
    assert_eq!(map.get(&b"other"[..]), None);
    let set: BTreeSet<RawString> = ["b", "a", "c"].into_iter().map(RawString::from).collect();
    assert!(set.contains(&b"b"[..]));
    assert_eq!(set.into_iter().collect::<Vec<_>>(), ["a", "b", "c"]);
}

/// The conveniences are not a format: the encodings are what they were.
#[test]
fn wire_key_json_and_schema_are_untouched() {
    use psio::pssz::schema_ir::schema_for;

    // pSSZ: the bytes, for text and non-text alike.
    for bytes in [TEXT.as_bytes(), BYTES, b""] {
        let raw = RawString::from(bytes);
        let short = Short::try_from(bytes).unwrap();
        assert_eq!(psio::pssz::to_pssz(&raw), bytes);
        assert_eq!(psio::pssz::to_pssz(&raw.as_raw_str()), bytes);
        assert_eq!(psio::pssz::to_pssz(&short), bytes);
        assert_eq!(psio::pssz::from_pssz::<RawString>(bytes).unwrap(), raw);
        assert_eq!(psio::pssz::from_pssz::<Short>(bytes).unwrap(), short);
        // Key: the escaped bytes, the same for both types.
        let key = psio::key::to_key(&raw);
        assert_eq!(key, psio::key::to_key(&short));
        assert_eq!(psio::key::from_key::<RawString>(&key).unwrap(), raw);
        assert_eq!(psio::key::from_key::<Short>(&key).unwrap(), short);
    }
    assert_eq!(psio::key::to_key(&RawString::from(&b"a\x00b"[..])), b"a\x00\x01b\x00\x00");

    // JSON: a string when the bytes are UTF-8, an error (never the lossy
    // text of Display) when they are not.
    assert_eq!(psio::to_json(&RawString::from(TEXT)), "\"t\u{e9}xt\"");
    assert_eq!(psio::to_json(&Short::try_from(TEXT).unwrap()), "\"t\u{e9}xt\"");
    assert_eq!(psio::try_to_json(&RawString::from(BYTES)).unwrap_err().what, "json: string is not valid UTF-8");
    assert_eq!(psio::try_to_json(&Short::try_from(BYTES).unwrap()).unwrap_err().what, "json: string is not valid UTF-8");
    assert_eq!(psio::from_json::<RawString>("\"t\\u00e9xt\"").unwrap(), TEXT);
    assert_eq!(psio::from_json::<Short>("\"abc\"").unwrap(), "abc");
    assert!(psio::from_json::<Short>("\"123456789\"").is_err());

    // Schema: a raw string is still the Custom string, not utf8.
    let (raw_schema, raw_type) = schema_for::<RawString>().unwrap();
    let (text_schema, text_type) = schema_for::<String>().unwrap();
    let raw_json = format!("{}{}", psio::to_json(&raw_schema), psio::to_json(&raw_type));
    let text_json = format!("{}{}", psio::to_json(&text_schema), psio::to_json(&text_type));
    assert_ne!(raw_json, text_json);
    assert!(raw_json.contains("string"), "{raw_json}");
    assert!(!raw_json.contains("utf8"), "{raw_json}");
}
