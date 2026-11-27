//! pjson on any bytes, and what JSON text keeps (gofractally/psio#17; C++
//! `cpp/tests/pjson_contract_tests.cpp`; docs/annotations.md 7.2 "On bytes
//! that are not a frame"; docs/pjson-spec.md §3, §7, §9).
//!
//! * `validate` is the definition: what it accepts, `decode` decodes,
//!   `to_json` converts (except a NaN, an infinity or text that is not
//!   UTF-8, which JSON cannot carry) and a view walks; what it refuses,
//!   they refuse;
//! * no read of any bytes panics: not `validate`, `decode`, `to_json`, the
//!   view, or the typed `from_pjson::<T>`;
//! * a JSON number keeps its value or is an error; text, order and
//!   duplicate names are kept.
#![cfg(feature = "pjson")]

use psio::pjson::{self, from_pjson, to_pjson, Value};
use psio::pjson_json::{from_json, to_json};
use psio::pjson_view::view_of;
use std::panic::{catch_unwind, AssertUnwindSafe};

#[derive(Debug, Clone, PartialEq, Default)]
struct Point {
    x: u32,
    y: u32,
}
psio::pjson_struct!(Point { x: u32, y: u32 });
#[derive(Debug, Clone, PartialEq, Default)]
struct Record {
    id: u64,
    big: u128,
    neg: i64,
    d: f64,
    b: bool,
    s: String,
    o: Option<u32>,
    nums: Vec<u16>,
    rows: Vec<Point>,
    strs: Vec<String>,
    bytes: Vec<u8>,
    nested: Point,
}
psio::pjson_struct!(Record {
    id: u64, big: u128, neg: i64, d: f64, b: bool, s: String, o: Option<u32>, nums: Vec<u16>, rows: Vec<Point>,
    strs: Vec<String>, bytes: Vec<u8>, nested: Point
});
fn record() -> Record {
    Record {
        id: u64::MAX,
        big: u128::MAX,
        neg: i64::MIN,
        d: 0.1,
        b: true,
        s: "héllo 中".into(),
        o: Some(7),
        nums: vec![1, 2, 3],
        rows: vec![Point { x: 1, y: 2 }, Point { x: 3, y: 4 }, Point { x: 5, y: 6 }],
        strs: vec!["a".into(), "bb".into(), String::new()],
        bytes: vec![9, 8],
        nested: Point { x: 5, y: 6 },
    }
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len() / 2).map(|i| u8::from_str_radix(&text[2 * i..2 * i + 2], 16).unwrap()).collect()
}
fn round(json: &str) -> String {
    to_json(&from_json(json).unwrap_or_else(|e| panic!("from_json({json}): {e}"))).unwrap_or_else(|e| panic!("to_json({json}): {e}"))
}

#[test]
fn the_shared_vectors_convert_to_their_json_text() {
    let all: serde_json::Value = serde_json::from_str(include_str!("fixtures/cpp-golden.json")).unwrap();
    let (mut seen, mut not_canonical) = (0, 0);
    for row in all.as_array().unwrap().iter().filter(|row| row["format"] == "pjson_to_json") {
        let id = row["id"].as_str().unwrap();
        let bytes = unhex(row["wire_hex"].as_str().unwrap());
        let canonical = unhex(row["canonical_hex"].as_str().unwrap());
        let json = row["json"].as_str().unwrap();
        // Well-formed: the lazy tag reads it, and it has this JSON text.
        assert!(pjson::lazy::validate(&bytes).is_ok(), "{id}");
        assert!(pjson::lazy::decode(&bytes).is_ok(), "{id}");
        assert_eq!(to_json(&bytes).unwrap(), json, "{id}");
        // psio::pjson reads it exactly when it is the canonical frame of
        // its value, which is what canonicalizing it gives ...
        assert_eq!(pjson::validate(&bytes).is_ok(), bytes == canonical, "{id}");
        assert_eq!(pjson::decode(&bytes).is_ok(), bytes == canonical, "{id}");
        assert_eq!(pjson::canonicalize(&bytes).unwrap(), canonical, "{id}");
        assert!(pjson::validate(&canonical).is_ok(), "{id}");
        // ... and what the canonical transcode of its JSON text gives (a
        // bytes value is a string once it is text).
        if !id.starts_with("bytes") {
            assert_eq!(from_json(json).unwrap(), canonical, "{id}");
            assert_eq!(pjson::canonicalize(&psio::pjson_json::from_json_lazy(json).unwrap()).unwrap(), canonical, "{id}");
        }
        seen += 1;
        not_canonical += (bytes != canonical) as usize;
    }
    assert!(seen >= 46 && not_canonical >= 20, "{seen} {not_canonical}");
}

fn rows(format: &str) -> Vec<(String, Vec<u8>)> {
    let all: serde_json::Value = serde_json::from_str(include_str!("fixtures/cpp-golden.json")).unwrap();
    all.as_array()
        .unwrap()
        .iter()
        .filter(|row| row["format"] == format)
        .map(|row| (row["id"].as_str().unwrap().to_owned(), unhex(row["wire_hex"].as_str().unwrap())))
        .collect()
}

// The float model (pjson_canonical::FLOAT_MODEL; DEFAULT CHOSEN, AWAITING
// OWNER): what an f32 stands for. The id of a row is the float's bits.
#[test]
fn a_native_float_is_the_number_it_stands_for() {
    assert_eq!(psio::pjson_canonical::FLOAT_MODEL, psio::pjson_canonical::FloatReading::DeclaredWidth);
    let mut seen = 0;
    for (id, bytes) in rows("pjson_f32") {
        let f = f32::from_bits(id.parse().unwrap());
        assert_eq!(to_pjson(&f), bytes, "f32 {f:e} ({id})");
        assert!(pjson::validate(&bytes).is_ok(), "{id}");
        let back: f32 = from_pjson(&bytes).unwrap();
        assert!(back.to_bits() == f.to_bits() || (f.is_nan() && back.is_nan()), "{id}");
        // Read as an f64 and written again, it is the same number.
        if !f.is_nan() {
            assert_eq!(to_pjson(&from_pjson::<f64>(&bytes).unwrap()), bytes, "{id}");
        }
        seen += 1;
    }
    for (id, bytes) in rows("pjson_f64") {
        let f = f64::from_bits(id.parse().unwrap());
        assert_eq!(to_pjson(&f), bytes, "f64 {f:e} ({id})");
        let back: f64 = from_pjson(&bytes).unwrap();
        assert!(back.to_bits() == f.to_bits() || (f.is_nan() && back.is_nan()), "{id}");
        seen += 1;
    }
    assert!(seen >= 148 + 274, "{seen}");
    // 0.1 is 0.1 from an f32, an f64 and JSON text.
    assert_eq!(to_pjson(&0.1f32), unhex("700201"));
    assert_eq!(to_pjson(&0.1f64), unhex("700201"));
    assert_eq!(from_json("0.1").unwrap(), unhex("700201"));
    assert_eq!(to_json(&to_pjson(&0.1f32)).unwrap(), "0.1");
    // A binary32 frame read as an f64 goes through its decimal.
    assert_eq!(from_pjson::<f64>(&unhex("62abaaaa3e")).unwrap(), 0.33333334);
    assert_eq!(from_pjson::<f32>(&unhex("62abaaaa3e")).unwrap(), 1.0f32 / 3.0);
    // The double that is exact in binary32 is not the float.
    assert_eq!(to_pjson(&(0.1f32 as f64)), unhex("63000000a09999b93f"));
    assert_eq!(from_pjson::<f64>(&to_pjson(&(0.1f32 as f64))).unwrap(), 0.1f32 as f64);
}

// A list's form is its values', not its element type's.
#[test]
fn a_list_of_numbers_has_the_form_its_values_give_it() {
    let mut seen = 0;
    for (id, bytes) in rows("pjson_list_i64") {
        let list: Vec<i64> = id.split(',').filter(|t| !t.is_empty()).map(|t| t.parse().unwrap()).collect();
        assert_eq!(to_pjson(&list), bytes, "[{id}]");
        assert_eq!(from_pjson::<Vec<i64>>(&bytes).unwrap(), list, "[{id}]");
        if list.iter().all(|&x| x >= 0) {
            let unsigned: Vec<u64> = list.iter().map(|&x| x as u64).collect();
            assert_eq!(to_pjson(&unsigned), bytes, "[{id}] as u64");
            assert_eq!(from_pjson::<Vec<u64>>(&bytes).unwrap(), unsigned, "[{id}]");
        }
        if list.iter().all(|&x| x.unsigned_abs() < 1 << 53) {
            let floats: Vec<f64> = list.iter().map(|&x| x as f64).collect();
            assert_eq!(to_pjson(&floats), bytes, "[{id}] as f64");
            assert_eq!(from_pjson::<Vec<f64>>(&bytes).unwrap(), floats, "[{id}]");
        }
        seen += 1;
    }
    for (id, bytes) in rows("pjson_list_f64") {
        let list: Vec<f64> = id.split(',').filter(|t| !t.is_empty()).map(|t| t.parse().unwrap()).collect();
        assert_eq!(to_pjson(&list), bytes, "[{id}]");
        let back = from_pjson::<Vec<f64>>(&bytes).unwrap();
        assert_eq!(back.iter().map(|x| x.to_bits()).collect::<Vec<_>>(), list.iter().map(|x| x.to_bits()).collect::<Vec<_>>(), "[{id}]");
        assert!(pjson::validate(&bytes).is_ok());
        // The anonymous document that spells the list is the same value.
        if !id.is_empty() {
            assert_eq!(from_json(&format!("[{id}]")).unwrap(), bytes, "[{id}] from JSON");
        }
        seen += 1;
    }
    for (id, bytes) in rows("pjson_list_f32") {
        let list: Vec<f32> = id.split(',').map(|t| t.parse().unwrap()).collect();
        assert_eq!(to_pjson(&list), bytes, "[{id}]");
        assert_eq!(from_pjson::<Vec<f32>>(&bytes).unwrap(), list, "[{id}]");
        assert_eq!(from_json(&format!("[{id}]")).unwrap(), bytes, "[{id}] from JSON");
        seen += 1;
    }
    assert_eq!(seen, 24);
    // The empty list has one form.
    for empty in [to_pjson(&Vec::<u16>::new()), to_pjson(&Vec::<f64>::new()), to_pjson(&Vec::<Point>::new()), to_pjson(&Vec::<String>::new())] {
        assert_eq!(empty, unhex("b0000000"));
    }
    assert!(from_pjson::<Vec<Point>>(&unhex("b0000000")).unwrap().is_empty());
}

// Every typed encoding is canonical, reads back, and is the canonical
// transcode of its own JSON text.
#[test]
fn every_typed_encoding_is_canonical() {
    let mut rng = Rng(11);
    for _ in 0..2000 {
        let mut r = record();
        r.id = rng.next() >> rng.below(64);
        r.neg = -((rng.next() >> (1 + rng.below(63))) as i64);
        r.d = match rng.below(4) {
            0 => rng.below(300) as f64,
            1 => rng.below(1000) as f64 / 100.0,
            2 => (rng.below(1000) as f32 / 7.0) as f64,
            _ => f64::from_bits(rng.next() & 0x7fef_ffff_ffff_ffff),
        };
        r.nums = (0..rng.below(5)).map(|_| (rng.next() >> (48 + rng.below(16))) as u16).collect();
        r.rows = (0..rng.below(4)).map(|_| Point { x: rng.below(1000) as u32, y: rng.next() as u32 }).collect();
        r.strs = (0..rng.below(4)).map(|_| "a".repeat(rng.below(4))).collect();
        r.o = (rng.below(2) == 0).then(|| rng.next() as u32);
        r.bytes = Vec::new(); // (bytes are a string once they are JSON text)
        let bytes = to_pjson(&r);
        assert!(pjson::validate(&bytes).is_ok());
        assert_eq!(pjson::canonicalize(&bytes).unwrap(), bytes);
        assert_eq!(from_pjson::<Record>(&bytes).unwrap(), r);
        let text = to_json(&bytes).unwrap();
        assert_eq!(text.contains("\"bytes\":\"\""), true);
    }
}

#[test]
fn a_well_formed_frame_that_is_not_the_canonical_one() {
    for (frame, canonical) in [
        ("410500", "25"),
        ("700a00", "25"),
        ("63000000000000f03f", "21"),
        ("639a9999999999b93f", "700201"),
        ("62cdcccc3d", "700201"),
        ("73ac40f9030f", "62abaaaa3e"),
        ("63000000000000f87f", "61007e"),
        ("915c7530306539", "90c3a9"),
        ("b0002122230001020300", "b50102030300"),
        ("b60100020003000300", "b50102030300"),
        ("b50000", "b0000000"),
        ("b000c00078211100010100000100", "c100010000000111782100000100"),
    ] {
        let b = unhex(frame);
        assert!(pjson::lazy::validate(&b).is_ok(), "{frame}");
        assert!(pjson::validate(&b).is_err(), "{frame}");
        assert!(pjson::decode(&b).is_err(), "{frame}");
        assert_eq!(pjson::canonicalize(&b).unwrap(), unhex(canonical), "{frame}");
    }
    // Text that is not text has no canonical form.
    for frame in ["90ff", "915c7564383030", "9122"] {
        let b = unhex(frame);
        assert!(pjson::lazy::validate(&b).is_ok(), "{frame}");
        assert!(pjson::validate(&b).is_err(), "{frame}");
        assert!(pjson::canonicalize(&b).is_err(), "{frame}");
    }
}

#[test]
fn a_json_number_keeps_its_value_or_is_an_error() {
    // Integers to 128 bits of magnitude.
    assert_eq!(round("18446744073709551615"), "18446744073709551615");
    assert_eq!(round("340282366920938463463374607431768211455"), "340282366920938463463374607431768211455");
    assert_eq!(round("-340282366920938463463374607431768211455"), "-340282366920938463463374607431768211455");
    // More digits than 128 bits hold: an error ...
    assert!(from_json("340282366920938463463374607431768211456").is_err());
    assert!(from_json("[123456789012345678901234567890123456789012345]").is_err());
    // ... unless the excess is zeros.
    assert_eq!(round(&format!("1{}", "0".repeat(40))), "1e40");
    // What binary64 cannot hold is a decimal, exactly.
    assert_eq!(round("3.14159265358979323846264338327950288"), "3.14159265358979323846264338327950288");
    assert_eq!(round("1e400"), "1e400");
    assert_eq!(round("-1.5e-400"), "-1.5e-400");
    assert_eq!(from_json("1e400").unwrap(), unhex("7002600c"));
    // What it holds exactly stays as it was.
    assert_eq!(round("0.1"), "0.1");
    assert_eq!(round("0.30000000000000004"), "0.30000000000000004");
    assert_eq!(from_json("0.1").unwrap(), unhex("700201"));
    // Minus zero.
    assert_eq!(round("-0"), "-0");
    assert_eq!(round("-0.0"), "-0");
    assert_eq!(from_json("[-0e5]").unwrap(), unhex("b9000000800100"));
    assert_eq!(round("0"), "0");
    // §7.1: trailing zeros are not kept; the value is.
    assert_eq!(round("100.00"), "100");
    assert_eq!(round("1.0"), "1");
    // A NaN or an infinity (from a native float) has no JSON text.
    assert!(to_json(&to_pjson(&f64::INFINITY)).is_err());
    assert!(to_json(&to_pjson(&f64::NAN)).is_err());
    assert!(to_json(&unhex("ba000000000000f07f0100")).is_err());
}

#[test]
fn text_order_and_duplicate_names_are_kept() {
    // Text is the text: nothing but the escapes JSON requires.
    assert_eq!(round("\"中文 café\""), "\"中文 café\"");
    assert_eq!(round("\"\\u4e2d\\u6587 caf\\u00e9\""), "\"中文 café\"");
    assert_eq!(round("\"\\ud83d\\ude00\""), "\"😀\"");
    assert_eq!(round("\"tab\\tquote\\\"back\\\\slash\\/\\b\\f\\r\\n\\u0001\""), "\"tab\\tquote\\\"back\\\\slash/\\b\\f\\r\\n\\u0001\"");
    assert_eq!(round("{\"clé\":\"é\"}"), "{\"clé\":\"é\"}");
    // Order and duplicates.
    assert_eq!(round("{\"z\":1,\"a\":2,\"m\":3}"), "{\"z\":1,\"a\":2,\"m\":3}");
    assert_eq!(round("{\"a\":1,\"a\":2,\"b\":{\"k\":1,\"k\":[1,1]}}"), "{\"a\":1,\"a\":2,\"b\":{\"k\":1,\"k\":[1,1]}}");
    assert_eq!(from_json("{\"a\":1,\"a\":2}").unwrap(), unhex("c000612161221f1f000102010200"));
    // A native struct, its text and back.
    let r = record();
    let bytes = to_pjson(&r);
    assert_eq!(from_pjson::<Record>(&bytes).unwrap(), r);
    let json = to_json(&bytes).unwrap();
    assert!(json.contains("\"s\":\"héllo 中\""), "{json}");
    assert!(json.contains("\"id\":18446744073709551615"), "{json}");
    assert!(json.contains("\"rows\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4},{\"x\":5,\"y\":6}]"), "{json}");
    assert!(json.contains("\"bytes\":\"CQg=\""), "{json}");
    // A native minus zero keeps its sign.
    assert_eq!(to_json(&to_pjson(&-0.0f64)).unwrap(), "-0");
    assert_eq!(from_pjson::<f64>(&to_pjson(&-0.0f64)).unwrap().to_bits(), (-0.0f64).to_bits());
}

// The three guarantees of gofractally/psio#17 (comment 7208563552). The
// same documents are in cpp/tests/pjson_contract_tests.cpp.
#[test]
fn full_json_keeps_values_order_and_duplicate_names() {
    let input = r#"{"z":null,"a":[true,false,-0,0,1e400,0.1,18446744073709551616,-1.5e-400,"é中","1e5"],"a":{"k":1,"k":"1"},"":[],"o":{}}"#;
    let output = r#"{"z":null,"a":[true,false,-0,0,1e400,0.1,18446744073709551616,-1.5e-400,"é中","1e5"],"a":{"k":1,"k":"1"},"":[],"o":{}}"#;
    assert_eq!(round(input), output);
    assert_eq!(round(output), output);
}

#[test]
fn i_json_comes_back_as_it_went_in_and_a_string_stays_a_string() {
    // An I-JSON document: no repeated names, numbers binary64 holds
    // exactly, large integers already strings.
    let doc = r#"{"id":"9007199254740993","n":9007199254740991,"m":-9007199254740991,"x":0.1,"s":"1e400","t":"true","u":"null","e":"","list":["0x10","-0",1.5,false,null],"nested":{"k":"340282366920938463463374607431768211455"}}"#;
    assert_eq!(round(doc), doc);
    let bytes = from_json(doc).unwrap();
    let v = view_of(&bytes);
    assert!(v.find(b"id").unwrap().is_string());
    assert!(v.find(b"s").unwrap().is_string());
    assert!(v.find(b"n").unwrap().is_uint());
}

#[test]
fn null_with_a_nonzero_low_nibble_is_not_a_value() {
    for frame in ["01", "0f", "b00001000100"] {
        let bytes = unhex(frame);
        assert!(pjson::validate(&bytes).is_err(), "{frame}");
        assert!(pjson::decode(&bytes).is_err(), "{frame}");
        assert!(to_json(&bytes).is_err(), "{frame}");
    }
    assert!(pjson::validate(&[0x00]).is_ok());
}

#[test]
fn a_view_reaches_rows_and_typed_elements_without_decoding() {
    let bytes = to_pjson(&record());
    let v = view_of(&bytes);
    // rows[i][key]
    let rows = v.find(b"rows").unwrap();
    assert!(rows.is_row_array());
    assert_eq!(rows.array_len().unwrap(), 3);
    let table = rows.rows().unwrap();
    assert_eq!((table.len(), table.field_count()), (3, 2));
    assert_eq!(table.key(1).unwrap(), b"y");
    assert_eq!(table.get(2).unwrap().find(b"y").unwrap().as_int().unwrap(), 6);
    assert_eq!(table.get(1).unwrap().field(0).unwrap().1.as_int().unwrap(), 3);
    assert!(table.get(1).unwrap().find(b"nope").is_none());
    assert!(table.get(3).is_err());
    // typed array element
    let nums = v.find(b"nums").unwrap();
    assert!(nums.is_typed_array());
    assert_eq!(nums.array_len().unwrap(), 3);
    assert_eq!(nums.typed_int_at(2).unwrap(), 3);
    assert_eq!(nums.typed_f64_at(0).unwrap(), 1.0);
    assert!(nums.typed_int_at(3).is_err());
    // The shared row-array vector.
    let shared = unhex("c10002000000010100000111e57879212200012324000100040200");
    let t = view_of(&shared).rows().unwrap();
    assert_eq!(t.get(1).unwrap().find(b"x").unwrap().as_int().unwrap(), 3);
    // A u64 typed element above 2^63.
    assert_eq!(view_of(&unhex("b8ffffffffffffffff0100")).typed_int_at(0).unwrap(), u64::MAX as i128);
}

// ── Every mutation ──────────────────────────────────────────────────────────

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
}
fn rounds() -> usize {
    std::env::var("PSIO_PJSON_CONTRACT_ROUNDS").ok().and_then(|t| t.parse().ok()).unwrap_or(4_000)
}
fn panic_text(e: Box<dyn std::any::Any + Send>) -> String {
    e.downcast_ref::<String>().cloned().or_else(|| e.downcast_ref::<&str>().map(|s| s.to_string())).unwrap_or_default()
}

// Everything a view reaches; an Err anywhere stops the walk.
fn walk(v: psio::pjson_view::View<'_>, depth: usize, budget: &mut usize) -> pjson::PjsonResult<()> {
    if depth > 80 || *budget == 0 {
        return Ok(());
    }
    *budget -= 1;
    if v.is_row_array() {
        let rows = v.rows()?;
        for i in 0..rows.len() {
            let row = rows.get(i)?;
            for j in 0..row.len() {
                walk(row.field(j)?.1, depth + 1, budget)?;
            }
            let _ = row.find(b"x");
        }
    } else if v.is_typed_array() {
        for i in 0..v.array_len()? {
            let _ = v.typed_f64_at(i)?;
        }
        let _ = v.typed_array_bytes()?;
    } else if v.is_array() {
        for i in 0..v.array_len()? {
            walk(v.array_get(i)?, depth + 1, budget)?;
        }
    } else if v.is_object() {
        for i in 0..v.object_len()? {
            walk(v.object_at(i)?.1, depth + 1, budget)?;
        }
        let _ = v.find(b"id");
    } else if v.is_string() {
        let _ = v.payload();
    } else if v.is_uint() {
        v.as_uint()?;
    } else if v.is_negint() {
        let _ = v.as_int();
    } else if v.is_bool() {
        v.as_bool()?;
    } else if v.type_code() == pjson::tag::IEEE_FLOAT {
        v.as_f64()?;
    }
    Ok(())
}

fn probe(bytes: &[u8], counts: &mut (usize, usize, usize)) {
    counts.0 += 1;
    let hex = || bytes.iter().map(|b| format!("{b:02x}")).collect::<String>();
    let guard = |what: &str, run: &mut dyn FnMut()| {
        if let Err(e) = catch_unwind(AssertUnwindSafe(|| run())) {
            panic!("{what} panicked on {}: {}", hex(), panic_text(e));
        }
    };
    // `valid`: a well-formed frame (the lazy tag). `canonical`: the one
    // encoding of its value (psio::pjson).
    let (mut valid, mut canonical) = (false, false);
    guard("lazy::validate", &mut || valid = pjson::lazy::validate(bytes).is_ok());
    guard("validate", &mut || canonical = pjson::validate(bytes).is_ok());
    assert!(!canonical || valid, "canonical and not well-formed: {}", hex());
    let (mut decoded, mut decoded_canonical) = (false, false);
    guard("lazy::decode", &mut || decoded = pjson::lazy::decode(bytes).is_ok());
    guard("decode", &mut || decoded_canonical = pjson::decode(bytes).is_ok());
    assert_eq!(decoded, valid, "validate and decode disagree on {}", hex());
    assert_eq!(decoded_canonical, canonical, "validate and decode disagree on {}", hex());
    let mut made: pjson::PjsonResult<Vec<u8>> = Ok(Vec::new());
    guard("canonicalize", &mut || made = pjson::canonicalize(bytes));
    let mut json: pjson::PjsonResult<String> = Ok(String::new());
    guard("to_json", &mut || json = to_json(bytes));
    let mut walked: pjson::PjsonResult<()> = Ok(());
    guard("view", &mut || walked = walk(view_of(bytes), 0, &mut 20_000));
    // The typed reads return a Result: they never panic.
    guard("from_pjson::<Record>", &mut || drop(from_pjson::<Record>(bytes)));
    guard("from_pjson::<Point>", &mut || drop(from_pjson::<Point>(bytes)));
    guard("from_pjson::<Vec<Point>>", &mut || drop(from_pjson::<Vec<Point>>(bytes)));
    guard("from_pjson::<Vec<u32>>", &mut || drop(from_pjson::<Vec<u32>>(bytes)));
    guard("from_pjson::<Option<String>>", &mut || drop(from_pjson::<Option<String>>(bytes)));
    guard("from_pjson::<f64>", &mut || {
        let _ = from_pjson::<f64>(bytes);
    });
    guard("Decode::decode", &mut || drop(<Record as psio::Decode<psio::pjson::format::Pjson>>::decode(bytes)));
    guard("Validate::validate", &mut || {
        let _ = <Record as psio::Validate<psio::pjson::format::Pjson>>::validate(bytes, &psio::DefaultSafe);
    });
    if !valid {
        assert!(json.is_err(), "to_json accepted bytes validate refuses: {}", hex());
        counts.2 += walked.is_err() as usize;
        return;
    }
    counts.1 += 1;
    // A validated frame is walked to the end, and converts to JSON unless
    // it holds what JSON cannot carry.
    assert!(walked.is_ok(), "the view failed on a validated frame {}: {walked:?}", hex());
    if let Err(e) = &json {
        let text = e.to_string();
        assert!(
            text.contains("NaN and infinity have no JSON form") || text.contains("is not UTF-8") || text.contains("bad utf8"),
            "to_json failed on a validated frame {}: {text}",
            hex()
        );
    }
    // The canonical form of a well-formed frame: it exists unless the frame
    // holds text that is not text; it is canonical and its own canonical
    // form; it is the frame itself exactly when the frame is canonical; and
    // it is the same value (the JSON text of the frame, read back by the
    // canonical transcoder, has the JSON text of the canonical form).
    let Ok(made) = made else {
        assert!(!canonical, "a canonical frame with no canonical form: {}", hex());
        return;
    };
    assert!(pjson::validate(&made).is_ok(), "the canonical form of {} is not canonical", hex());
    assert_eq!(pjson::canonicalize(&made).unwrap(), made, "{}", hex());
    assert_eq!(made == bytes, canonical, "{}", hex());
    if let Ok(text) = &json {
        let again = from_json(text).unwrap_or_else(|e| panic!("from_json of the text of {}: {e}", hex()));
        assert_eq!(to_json(&again).unwrap(), to_json(&made).unwrap(), "{}", hex());
    }
}

#[test]
fn no_read_panics_and_validate_decode_and_json_agree() {
    let mut seeds: Vec<Vec<u8>> = vec![to_pjson(&record()), to_pjson(&Point { x: 1, y: 2 }), to_pjson(&vec![1u32, 2, 3])];
    for frame in [
        "c00076616c756573b70100000000010000000001000300af00060100",
        "c10002000000010100000111e57879212200012324000100040200",
        "b0002122230001020300",
        "c000612161221f1f000102010200",
        "4fffffffffffffffffffffffffffffffff",
        "5fffffffffffffffffffffffffffffffff",
        "610080",
        "63000000000000f03f",
        "620000803f",
        "7002600c",
        "701d610c",
        "9068c3a9",
        "915c7530306539",
        "a00908",
        "00",
        "11",
        "25",
        "35",
    ] {
        seeds.push(unhex(frame));
    }
    let long_key = "k".repeat(300);
    let mut long = Vec::new();
    pjson::encode(&Value::Object(vec![(long_key.as_bytes(), Value::UInt(1)), (b"short".as_slice(), Value::Str(b"v", 0))]), &mut long);
    seeds.push(long);
    let all: serde_json::Value = serde_json::from_str(include_str!("fixtures/cpp-golden.json")).unwrap();
    for row in all.as_array().unwrap() {
        let format = row["format"].as_str().unwrap();
        if matches!(format, "pjson" | "pjson_extra" | "pjson_typed" | "pjson_invalid") {
            seeds.push(unhex(row["wire_hex"].as_str().unwrap()));
        }
    }
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        let mut counts = (0usize, 0usize, 0usize);
        let mut rng = Rng(17);
        for good in &seeds {
            probe(good, &mut counts);
            for n in 0..good.len() {
                probe(&good[..n], &mut counts);
            }
            for i in 0..good.len() {
                for v in [0u8, 0xff, good[i].wrapping_add(1), good[i].wrapping_sub(1), 0x7f] {
                    let mut bad = good.clone();
                    bad[i] = v;
                    probe(&bad, &mut counts);
                }
            }
            for _ in 0..rounds() {
                let mut bad = good.clone();
                for _ in 0..1 + rng.below(3) {
                    if bad.is_empty() {
                        break;
                    }
                    match rng.below(5) {
                        0 => {
                            let i = rng.below(bad.len());
                            bad[i] = rng.next() as u8;
                        }
                        1 => {
                            let i = rng.below(bad.len());
                            bad[i] = rng.below(bad.len() + 2) as u8;
                        }
                        2 => {
                            let i = rng.below(bad.len());
                            bad.remove(i);
                        }
                        3 => {
                            let i = rng.below(bad.len() + 1);
                            bad.insert(i, rng.next() as u8);
                        }
                        _ => {
                            let n = rng.below(bad.len() + 1);
                            bad.truncate(n);
                        }
                    }
                }
                probe(&bad, &mut counts);
            }
        }
        counts
    }));
    std::panic::set_hook(hook);
    let (tried, accepted, view_failed) = outcome.unwrap_or_else(|e| panic!("{}", panic_text(e)));
    eprintln!("pjson_contract: {tried} mutated frames, {accepted} accepted, the view refused {view_failed}");
    assert!(accepted > seeds.len() && accepted < tried);
    assert!(view_failed > tried / 50);
}
