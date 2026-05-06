//! Cross-language byte-identity validation for `psio::pjson`.
//!
//! The conformance corpus at `conformance/fixtures/**/*.json` is the
//! cross-language oracle — every `wire_hex` is byte-equal between
//! the Rust and C++ conformance drivers (verified by
//! `tools/run-conformance.sh`'s 105/105 cross-validation gate).
//!
//! This test runs each fixture's `wire_hex` through `psio::pjson`'s
//! decode → encode pipeline and asserts the bytes survive unchanged.
//! That confirms the LIBRARY (not just the conformance driver) reads
//! and writes the spec-correct wire format. By transitivity, the lib
//! agrees byte-for-byte with the C++ driver.
//!
//! Reject fixtures (`must_reject: true`) test that the lib's decoder
//! rejects malformed wires.
//!
//! Fixtures the lib doesn't yet handle (e.g., features deferred from
//! the lib while present in the spec-correct conformance driver) are
//! collected in `LIB_NOT_YET_SUPPORTED` with the reason; the test
//! reports them as skipped rather than failing.

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

/// Fixture IDs the lib doesn't fully round-trip yet. Each entry needs
/// a one-line reason. As migration proceeds, entries get removed.
fn lib_not_yet_supported() -> HashSet<&'static str> {
    let mut s = HashSet::new();

    // Lib gap #1: `Value::Float(f64)` is width-erased. Decoding an
    // f16 or f32 wire widens to f64; re-encoding then emits 9 bytes
    // instead of the 3- or 5-byte source form. Fix requires changing
    // the Value variant to carry `width_log2` (or similar) and
    // threading it through encode/decode.
    s.insert("canonical_nan_f16");
    s.insert("canonical_nan_f32");
    s.insert("ieee_float_f16_one");
    s.insert("ieee_float_f32_one");
    s.insert("json_ingress_fractional_picker");

    // Lib gap #2: f128 decode is explicitly unimplemented.
    // `psio::pjson::parse_value` returns a `NotImplemented binary128`
    // error. Fix requires adding a binary128 codec (the conformance
    // driver vendors Berkeley SoftFloat for this; the lib doesn't).
    s.insert("canonical_nan_f128");
    s.insert("ieee_float_f128_one");
    s.insert("ieee_float_f128_pos_inf");
    s.insert("ieee_float_f128_minus_one");

    // Lib gap #3: NaN canonicalization. Even at f64 the lib doesn't
    // canonicalize NaN bits on encode, so a non-canonical NaN input
    // would round-trip with original bits rather than `0x7FF8...`
    // canonical. The current `canonical_nan_f64` fixture happens to
    // already be canonical so it'd pass — but listed here under
    // "canonical-encoder kernel hasn't migrated to the lib yet" as
    // a tracked gap until the kernel lands and we can prove it.
    s.insert("canonical_nan_f64");

    s
}

#[derive(Debug)]
struct Fixture {
    path: PathBuf,
    id: String,
    wire_hex: String,
    must_round_trip: bool,
    must_reject: bool,
}

fn fixtures_root() -> PathBuf {
    // Manifest dir is rust/psio/. Fixtures live at repo-root/conformance/fixtures.
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest.parent().unwrap().parent().unwrap().join("conformance")
}

fn parse_hex(s: &str) -> Vec<u8> {
    let s = s.trim();
    let mut out = Vec::with_capacity(s.len() / 2);
    let mut chars = s.chars().filter(|c| !c.is_whitespace());
    while let (Some(a), Some(b)) = (chars.next(), chars.next()) {
        let h = u8::from_str_radix(&format!("{}{}", a, b), 16)
            .expect("fixture wire_hex must be valid hex");
        out.push(h);
    }
    out
}

fn load_fixtures(root: &Path, accept_reject: bool) -> Vec<Fixture> {
    let mut out = Vec::new();
    let dirs = if accept_reject {
        vec![root.join("fixtures-reject")]
    } else {
        vec![root.join("fixtures")]
    };
    for dir in dirs {
        if !dir.exists() { continue; }
        walk_dir(&dir, &mut out);
    }
    out
}

fn walk_dir(dir: &Path, out: &mut Vec<Fixture>) {
    for entry in fs::read_dir(dir).expect("read fixtures dir") {
        let entry = entry.expect("dirent");
        let path = entry.path();
        if path.is_dir() {
            walk_dir(&path, out);
            continue;
        }
        if path.extension().and_then(|s| s.to_str()) != Some("json") { continue; }

        let text = fs::read_to_string(&path).expect("read fixture");
        let v: serde_json::Value =
            serde_json::from_str(&text).expect("fixture JSON parse");
        let id = v.get("id").and_then(|x| x.as_str()).unwrap_or("<unnamed>").to_string();
        let wire_hex = v.get("wire_hex").and_then(|x| x.as_str())
            .expect("fixture missing wire_hex").to_string();
        let must_round_trip = v.get("must_round_trip")
            .and_then(|x| x.as_bool()).unwrap_or(false);
        let must_reject = v.get("must_reject")
            .and_then(|x| x.as_bool()).unwrap_or(false);
        out.push(Fixture { path, id, wire_hex, must_round_trip, must_reject });
    }
}

#[test]
fn lib_decode_encode_round_trips_every_corpus_fixture() {
    let root = fixtures_root();
    let fixtures = load_fixtures(&root, false);
    assert!(!fixtures.is_empty(),
            "no fixtures found under {:?}", root.join("fixtures"));

    let skip = lib_not_yet_supported();
    let mut tested = 0;
    let mut skipped: Vec<&str> = Vec::new();
    let mut failures: Vec<String> = Vec::new();

    for f in &fixtures {
        if !f.must_round_trip { continue; }
        if skip.contains(f.id.as_str()) {
            skipped.push(&f.id);
            continue;
        }

        let wire = parse_hex(&f.wire_hex);
        // Decode via the lib.
        let value = match psio::pjson::decode(&wire) {
            Ok(v)  => v,
            Err(e) => {
                failures.push(format!(
                    "{}: lib decode failed: {} (wire={})",
                    f.id, e.0, f.wire_hex));
                continue;
            }
        };
        // Re-encode via the lib.
        let mut out = Vec::with_capacity(psio::pjson::value_size(&value));
        psio::pjson::encode(&value, &mut out);
        if out != wire {
            failures.push(format!(
                "{}: re-encoded bytes differ\n  expected: {}\n  got:      {}",
                f.id,
                f.wire_hex,
                hex_encode(&out)));
            continue;
        }
        tested += 1;
    }

    if !failures.is_empty() {
        let n = failures.len();
        for line in &failures { eprintln!("  {}", line); }
        panic!("{} of {} round-trip-able fixtures failed (passed {}, skipped {})",
               n, tested + n, tested, skipped.len());
    }
    eprintln!("psio::pjson round-tripped {} fixtures byte-for-byte ({} skipped)",
              tested, skipped.len());
}

#[test]
fn lib_rejects_every_reject_fixture() {
    let root = fixtures_root();
    let fixtures = load_fixtures(&root, true);
    assert!(!fixtures.is_empty(),
            "no reject fixtures found under {:?}", root.join("fixtures-reject"));

    let mut tested = 0;
    let mut failures: Vec<String> = Vec::new();

    for f in &fixtures {
        if !f.must_reject { continue; }
        let wire = parse_hex(&f.wire_hex);
        match psio::pjson::decode(&wire) {
            Ok(v)  => failures.push(format!(
                "{}: expected reject but decoded OK: {:?}", f.id, v)),
            Err(_) => { tested += 1; }
        }
    }

    if !failures.is_empty() {
        for line in &failures { eprintln!("  {}", line); }
        panic!("{} of {} reject fixtures decoded successfully (should have rejected)",
               failures.len(), tested + failures.len());
    }
    eprintln!("psio::pjson rejected {} ill-formed fixtures", tested);
}

fn hex_encode(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{:02x}", b));
    }
    s
}
