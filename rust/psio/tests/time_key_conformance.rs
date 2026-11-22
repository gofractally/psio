//! Cross-language time / key conformance: consumes
//! conformance/time-key-vectors.json (conformance/time-key-vectors.md), the
//! fixture emitted from the C++ implementation
//! (cpp/tests/time_key_conformance_emit.cpp) and also checked by ctest and
//! the JS tests. Every row of every kind is checked through the typed Rust
//! API: `psio::pssz`, `psio::key`, `psio::json` and `psio::time`.

use psio::json::{self, IJson, Json, JsonText};
use psio::key::{from_key, key_prefix, key_prefix_upper_bound, key_size, to_key, validate_key};
use psio::key::{try_key_prefix, try_to_key, ByKey, KeyPack, KeyUnpack};
use psio::pssz::Nested;
use psio::pssz::schema_ir::{schema_for, AnyValue, ToPsszSchema};
use psio::pssz::{Pssz, PsszPack, PsszUnpack};
use psio::time::{Duration, Micros, Millis, MonotonicPoint, Nanos, Seconds, TimePoint, Unit};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Debug;

// The transparent wrappers, custom types and invariants of
// conformance/custom-types.md.
#[path = "custom/text.rs"]
mod text;
#[path = "custom/types.rs"]
mod custom_types;
#[path = "custom/handlers.rs"]
mod custom_handlers;
use custom_types::*;

/// A user-declared unit: minutes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct Minutes;
impl Unit for Minutes {
    const NUM: u64 = 60;
    const DEN: u64 = 1;
}

macro_rules! record {
    ($(#[$attr:meta])* struct $name:ident { $($field:ident: $ty:ty,)* }) => {
        #[derive(
            Debug, Clone, PartialEq, PartialOrd, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
            psio::ToPsszSchema, psio::Key,
        )]
        $(#[$attr])*
        struct $name { $($field: $ty,)* }
    };
}

record! {
    #[pssz(definition_will_not_change)]
    struct Stamps {
        created: TimePoint<Seconds>,
        updated: TimePoint<Micros>,
        ttl: Duration<Millis>,
        legacy: TimePoint<Seconds, u32>,
    }
}
record! {
    struct Event {
        name: String,
        at: TimePoint<Millis>,
        precise: Option<TimePoint<Nanos>>,
        history: Vec<TimePoint<Seconds>>,
        elapsed: Duration<Nanos>,
        timeout: Duration<Minutes, i32>,
    }
}
record! {
    struct Timer {
        expires: MonotonicPoint<Micros>,
        started: Option<MonotonicPoint<Seconds>>,
        laps: Vec<MonotonicPoint<Millis, u32>>,
    }
}
record! {
    struct Account {
        shard: u32,
        owner: String,
        seq: Option<i64>,
    }
}

// C++ std::variant<std::uint8_t, std::string>.
#[derive(
    Debug, Clone, PartialEq, PartialOrd, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::ToPsszSchema, psio::Key,
)]
enum U8OrString {
    #[json(name = "uint8")]
    U8(u8),
    #[json(name = "string")]
    Text(String),
}

// A transparent wrapper of a composite key is that composite key: its
// prefixes are the tuple's.
#[derive(
    Debug, Clone, PartialEq, PartialOrd, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::ToPsszSchema, psio::Key,
)]
#[pssz(transparent)]
struct ShardKey {
    key: (u32, String),
}

// The wire types of the types that have an invariant: what a reader that
// knows only the wire type (a schema with no handler) reads, and what a
// writer that does not check would write (`key-invariant` rows).
#[derive(Debug, Clone, PartialEq, PartialOrd, psio::PsszPack, psio::PsszUnpack, psio::Key)]
#[pssz(definition_will_not_change)]
struct NameWire {
    value: u64,
}
#[derive(Debug, Clone, PartialEq, PartialOrd, psio::PsszPack, psio::PsszUnpack, psio::Key)]
struct RangeWire {
    lo: u32,
    hi: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, PartialOrd, psio::PsszPack, psio::PsszUnpack, psio::Key)]
#[repr(u8)]
enum LevelWire {
    Low = 0,
    #[pssz(other)]
    Other(u8),
}
impl From<NameWire> for Name {
    fn from(wire: NameWire) -> Self {
        Name { value: wire.value }
    }
}
impl From<RangeWire> for Range {
    fn from(wire: RangeWire) -> Self {
        Range { lo: wire.lo, hi: wire.hi }
    }
}
impl From<LevelWire> for Level {
    fn from(wire: LevelWire) -> Self {
        match wire {
            LevelWire::Low => Level::Low,
            LevelWire::Other(1) => Level::Mid,
            LevelWire::Other(2) => Level::High,
            LevelWire::Other(3) => Level::Top,
            LevelWire::Other(n) => Level::Unknown(n),
        }
    }
}
type Guards = (Name, Option<Range>, Vec<Level>, DerKey);
type GuardsWire = (NameWire, Option<RangeWire>, Vec<LevelWire>, Vec<u8>);

type KInts = (bool, i8, i16, i32, i64, u8, u16, u32, u64);
type KTime = (TimePoint<Micros>, String, Duration<Seconds>);

#[derive(Debug, serde::Deserialize)]
struct Row {
    name: String,
    kind: String,
    r#type: String,
    pssz: Option<String>,
    key: Option<String>,
    json: Option<String>,
    ijson: Option<String>,
    text: Option<String>,
    prefix: Option<String>,
    bound: Option<String>,
    count: Option<usize>,
    values: Option<Vec<String>>,
    reason: Option<String>,
}

#[derive(Debug, serde::Deserialize)]
struct TypeRow {
    id: String,
    composite: bool,
}

#[derive(Debug, serde::Deserialize)]
struct Fixture {
    version: u32,
    schema_ir: String,
    types: Vec<TypeRow>,
    vectors: Vec<Row>,
}

fn fixture() -> Fixture {
    let path = format!("{}/../../conformance/time-key-vectors.json", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    serde_json::from_str(&text).expect("fixture JSON")
}

fn unhex(text: &str) -> Vec<u8> {
    assert!(text.len() % 2 == 0, "odd hex");
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).expect("hex")).collect()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

trait Conf:
    PsszPack<Pssz> + PsszUnpack<Pssz> + JsonText + KeyPack + KeyUnpack + PartialEq + PartialOrd + Debug
{
}
impl<T> Conf for T where
    T: PsszPack<Pssz> + PsszUnpack<Pssz> + JsonText + KeyPack + KeyUnpack + PartialEq + PartialOrd + Debug
{
}

type PrefixFn<T> = fn(&T, usize) -> Vec<u8>;

fn value_of<T: Conf>(pssz: &Option<String>) -> Result<T, String> {
    let bytes = unhex(pssz.as_ref().ok_or("row has no pssz")?);
    let value = psio::pssz::from_pssz_exact::<Pssz, T>(&bytes).map_err(|e| format!("from_pssz {e:?}"))?;
    let back = psio::pssz::try_to_pssz::<Pssz, T>(&value).map_err(|e| format!("to_pssz {e:?}"))?;
    if back != bytes {
        return Err(format!("pssz re-encodes as {}", hex(&back)));
    }
    Ok(value)
}

fn read_both<T: Conf>(text: &str) -> (Result<T, String>, Result<T, String>) {
    let a = json::from_str::<Json, T>(text).map_err(|e| e.to_string());
    let b = json::from_str::<IJson, T>(text).map_err(|e| e.to_string());
    // validate agrees with decode.
    assert_eq!(json::validate::<Json, T>(text.as_bytes()).is_ok(), a.is_ok(), "json validate: {text}");
    assert_eq!(json::validate::<IJson, T>(text.as_bytes()).is_ok(), b.is_ok(), "ijson validate: {text}");
    (a, b)
}

fn check<T: Conf>(row: &Row, prefix: Option<PrefixFn<T>>) -> Result<(), String> {
    match row.kind.as_str() {
        "value" => {
            let value = value_of::<T>(&row.pssz)?;
            let key = to_key(&value);
            if Some(hex(&key)) != row.key {
                return Err(format!("key is {}", hex(&key)));
            }
            if key_size(&value) != key.len() {
                return Err("key_size".into());
            }
            validate_key::<T>(&key).map_err(|e| format!("validate_key {e}"))?;
            let back = from_key::<T>(&key).map_err(|e| format!("from_key {e}"))?;
            // A NaN is not `==` itself: the value a key gives back is then
            // identified by its bits (the PSSZ bytes).
            if back != value
                && psio::pssz::try_to_pssz::<Pssz, T>(&back).ok().map(|b| hex(&b)) != row.pssz
            {
                return Err(format!("from_key gives {back:?}, want {value:?}"));
            }
            let j = json::to_string::<Json, T>(&value);
            let ij = json::to_string::<IJson, T>(&value);
            match (&row.json, &row.ijson) {
                (Some(want_j), Some(want_ij)) => {
                    let (j, ij) = (j.map_err(|e| e.to_string())?, ij.map_err(|e| e.to_string())?);
                    if &j != want_j || &ij != want_ij {
                        return Err(format!("text is {j} / {ij}"));
                    }
                    if json::from_str::<Json, T>(&j).ok().as_ref() != Some(&value)
                        || json::from_str::<IJson, T>(&ij).ok().as_ref() != Some(&value)
                    {
                        return Err("text does not read back".into());
                    }
                }
                (None, None) => {
                    if j.is_ok() || ij.is_ok() {
                        return Err("a value without text was written".into());
                    }
                }
                _ => return Err("row has one of json / ijson".into()),
            }
            Ok(())
        }
        "text-accept" => {
            let want = value_of::<T>(&row.pssz)?;
            let text = row.text.as_ref().ok_or("no text")?;
            let (a, b) = read_both::<T>(text);
            if a.as_ref() != Ok(&want) || b.as_ref() != Ok(&want) {
                return Err(format!("read {a:?} / {b:?}, want {want:?}"));
            }
            let canonical = json::to_string::<Json, T>(&want).map_err(|e| e.to_string())?;
            if Some(&canonical) != row.json.as_ref() {
                return Err(format!("canonical text is {canonical}"));
            }
            Ok(())
        }
        "text-reject" => {
            let (a, b) = read_both::<T>(row.text.as_ref().ok_or("no text")?);
            if a.is_ok() || b.is_ok() {
                return Err(format!("accepted as {a:?} / {b:?}"));
            }
            Ok(())
        }
        "key-reject" => {
            let bytes = unhex(row.key.as_ref().ok_or("no key")?);
            if validate_key::<T>(&bytes).is_ok() || from_key::<T>(&bytes).is_ok() {
                return Err("accepted".into());
            }
            Ok(())
        }
        "key-order" => {
            let mut last: Option<(T, Vec<u8>)> = None;
            for (i, pssz) in row.values.as_ref().ok_or("no values")?.iter().enumerate() {
                let value = value_of::<T>(&Some(pssz.clone()))?;
                let key = to_key(&value);
                if let Some((last_value, last_key)) = &last {
                    if last_key >= &key {
                        return Err(format!("keys not ascending at {i}"));
                    }
                    if !(last_value < &value) {
                        return Err(format!("values not ascending at {i}"));
                    }
                }
                last = Some((value, key));
            }
            Ok(())
        }
        "key-order-bits" => {
            // Key order where `<` has no answer (NaNs): the keys ascend and
            // each key gives the exact bit pattern back.
            let mut last: Option<Vec<u8>> = None;
            for (i, pssz) in row.values.as_ref().ok_or("no values")?.iter().enumerate() {
                let value = value_of::<T>(&Some(pssz.clone()))?;
                let key = to_key(&value);
                if last.as_ref().is_some_and(|last| last >= &key) {
                    return Err(format!("keys not ascending at {i}"));
                }
                validate_key::<T>(&key).map_err(|e| format!("validate_key {e}"))?;
                let back = from_key::<T>(&key).map_err(|e| format!("from_key {e}"))?;
                let bits = psio::pssz::try_to_pssz::<Pssz, T>(&back).map_err(|e| format!("{e:?}"))?;
                if &hex(&bits) != pssz {
                    return Err(format!("key gives {} back at {i}", hex(&bits)));
                }
                last = Some(key);
            }
            Ok(())
        }
        "key-prefix" => {
            let prefix = prefix.ok_or("key-prefix row for a non-composite type")?;
            let value = value_of::<T>(&row.pssz)?;
            let got = prefix(&value, row.count.ok_or("no count")?);
            if Some(hex(&got)) != row.prefix {
                return Err(format!("prefix is {}", hex(&got)));
            }
            if key_prefix_upper_bound(&got).map(|b| hex(&b)) != row.bound {
                return Err("bound".into());
            }
            if !to_key(&value).starts_with(&got) {
                return Err("not a prefix of the full key".into());
            }
            Ok(())
        }
        other => Err(format!("unknown kind {other}")),
    }
}

/// A `key-invariant` row: the key and the bytes of a value of `T`'s wire
/// type `W` that violates an invariant of `T`. Every typed reader rejects
/// both with the invariant's message; a reader that knows only the wire type
/// reads them; no typed writer writes the value.
fn check_invariant<T, W>(row: &Row, into: impl Fn(W) -> T) -> Result<(), String>
where
    T: Conf,
    W: PsszPack<Pssz> + PsszUnpack<Pssz> + KeyPack + KeyUnpack + Debug,
{
    let key = unhex(row.key.as_ref().ok_or("no key")?);
    let pssz = unhex(row.pssz.as_ref().ok_or("no pssz")?);
    let reason = row.reason.as_deref().ok_or("no reason")?;
    let rejections = [
        ("validate_key", validate_key::<T>(&key).map_err(|e| e.0)),
        ("from_key", from_key::<T>(&key).map(drop).map_err(|e| e.0)),
        ("pssz_validate", psio::pssz::from_pssz_exact::<Pssz, T>(&pssz).map(drop).map_err(|e| e.0)),
        ("from_pssz", psio::pssz::from_pssz::<Pssz, T>(&pssz).map(drop).map_err(|e| e.0)),
    ];
    for (what, outcome) in rejections {
        if outcome != Err(reason) {
            return Err(format!("{what}: {outcome:?}, want {reason:?}"));
        }
    }
    // The wire type: the key decodes to the value `pssz`.
    let wire = from_key::<W>(&key).map_err(|e| format!("wire from_key {e}"))?;
    let bytes = psio::pssz::try_to_pssz::<Pssz, W>(&wire).map_err(|e| format!("wire to_pssz {e:?}"))?;
    if bytes != pssz {
        return Err(format!("the wire type's key decodes to {}", hex(&bytes)));
    }
    if to_key(&wire) != key {
        return Err("the wire type's key does not round-trip".into());
    }
    // The same value as a `T`: it has no key and no encoding.
    let value = into(wire);
    let outcomes = [
        ("try_to_key", try_to_key(&value).map(drop).map_err(|e| e.0)),
        ("try_to_pssz", psio::pssz::try_to_pssz::<Pssz, T>(&value).map(drop).map_err(|e| e.0)),
    ];
    for (what, outcome) in outcomes {
        if outcome != Err(reason) {
            return Err(format!("{what}: {outcome:?}, want {reason:?}"));
        }
    }
    if !std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| to_key(&value))).is_err() {
        return Err("to_key wrote a value that violates an invariant".into());
    }
    Ok(())
}

// `|v, count|` → key_prefix::<T, _>(&(&v.f0, ..., &v.f{count-1})), one
// typed call per prefix length.
macro_rules! prefix_steps {
    ($v:ident, $ty:ty, $count:ident, $n:expr; [$($done:tt)*]) => {
        if $count == $n {
            return key_prefix::<$ty, _>(&($(&$v.$done,)*));
        }
    };
    ($v:ident, $ty:ty, $count:ident, $n:expr; [$($done:tt)*] $next:tt $($rest:tt)*) => {
        if $count == $n {
            return key_prefix::<$ty, _>(&($(&$v.$done,)*));
        }
        prefix_steps!($v, $ty, $count, $n + 1; [$($done)* $next] $($rest)*);
    };
}
macro_rules! composite {
    ($row:expr, $ty:ty; $($field:tt)*) => {{
        fn prefix(v: &$ty, count: usize) -> Vec<u8> {
            let _ = v;
            prefix_steps!(v, $ty, count, 0; [] $($field)*);
            panic!("prefix count {count} out of range");
        }
        check::<$ty>($row, Some(prefix))
    }};
}

fn dispatch(row: &Row) -> Result<(), String> {
    if row.kind == "bound" {
        let prefix = unhex(row.prefix.as_ref().ok_or("no prefix")?);
        return if key_prefix_upper_bound(&prefix).map(|b| hex(&b)) == row.bound {
            Ok(())
        } else {
            Err("bound".into())
        };
    }
    if row.kind == "key-invariant" {
        return match row.r#type.as_str() {
            "Name" => check_invariant::<Name, NameWire>(row, Name::from),
            "DerKey" => check_invariant::<DerKey, Vec<u8>>(row, |data| DerKey { data }),
            "Range" => check_invariant::<Range, RangeWire>(row, Range::from),
            "Level" => check_invariant::<Level, LevelWire>(row, Level::from),
            "tuple<Name,optional<Range>,list<Level>,DerKey>" => {
                check_invariant::<Guards, GuardsWire>(row, |(name, range, levels, data)| {
                    (
                        name.into(),
                        range.map(Range::from),
                        levels.into_iter().map(Level::from).collect(),
                        DerKey { data },
                    )
                })
            }
            other => Err(format!("unknown type {other}")),
        };
    }
    match row.r#type.as_str() {
        // Nested values, pairs, maps and sets.
        "tuple<u32,nested<Account>>" => composite!(row, (u32, Nested<Account>); 0 1),
        "pair<u32,string>" => composite!(row, (u32, String); 0 1),
        "map<string,u32>" => check::<BTreeMap<String, u32>>(row, None),
        "map<tuple<u8,string>,list<i16>>" => check::<BTreeMap<(u8, String), Vec<i16>>>(row, None),
        "map<string,map<u8,set<string>>>" => {
            check::<BTreeMap<String, BTreeMap<u8, BTreeSet<String>>>>(row, None)
        }
        "set<u8>" => check::<BTreeSet<u8>>(row, None),
        "set<string>" => check::<BTreeSet<String>>(row, None),
        "set<i64>" => check::<BTreeSet<i64>>(row, None),
        "set<f64>" => check::<BTreeSet<ByKey<f64>>>(row, None),
        "tuple<map<u16,string>,u8>" => composite!(row, (BTreeMap<u16, String>, u8); 0 1),
        "set<nested<u32>>" => check::<BTreeSet<Nested<u32>>>(row, None),
        // Transparent wrappers, custom types, invariants.
        "Id" => check::<Id>(row, None),
        "Blob" => check::<Blob>(row, None),
        "Endpoint" => check::<Endpoint>(row, None),
        "Ratio" => check::<Ratio>(row, None),
        "Ref" => check::<Ref>(row, None),
        "Name" => check::<Name>(row, None),
        "DerKey" => check::<DerKey>(row, None),
        "Range" => check::<Range>(row, None),
        "Level" => check::<Level>(row, None),
        "ShardKey" => {
            fn prefix(v: &ShardKey, count: usize) -> Vec<u8> {
                match count {
                    0 => key_prefix::<ShardKey, _>(&()),
                    1 => key_prefix::<ShardKey, _>(&(&v.key.0,)),
                    2 => key_prefix::<ShardKey, _>(&(&v.key.0, &v.key.1)),
                    _ => panic!("prefix count {count} out of range"),
                }
            }
            check::<ShardKey>(row, Some(prefix))
        }
        "tuple<Name,optional<Range>,list<Level>,DerKey>" => composite!(row, Guards; 0 1 2 3),
        "time_point<seconds>" => check::<TimePoint<Seconds>>(row, None),
        "time_point<millis>" => check::<TimePoint<Millis>>(row, None),
        "time_point<micros>" => check::<TimePoint<Micros>>(row, None),
        "time_point<nanos>" => check::<TimePoint<Nanos>>(row, None),
        "time_point<seconds,u32>" => check::<TimePoint<Seconds, u32>>(row, None),
        "duration<seconds>" => check::<Duration<Seconds>>(row, None),
        "duration<millis>" => check::<Duration<Millis>>(row, None),
        "duration<micros>" => check::<Duration<Micros>>(row, None),
        "duration<nanos>" => check::<Duration<Nanos>>(row, None),
        "duration<60/1,i32>" => check::<Duration<Minutes, i32>>(row, None),
        "monotonic<seconds>" => check::<MonotonicPoint<Seconds>>(row, None),
        "monotonic<micros>" => check::<MonotonicPoint<Micros>>(row, None),
        "monotonic<millis,u32>" => check::<MonotonicPoint<Millis, u32>>(row, None),
        "Timer" => composite!(row, Timer; expires started laps),
        "tuple<monotonic<micros>,string>" => {
            composite!(row, (MonotonicPoint<Micros>, String); 0 1)
        }
        "Stamps" => composite!(row, Stamps; created updated ttl legacy),
        "Event" => composite!(row, Event; name at precise history elapsed timeout),
        "Account" => composite!(row, Account; shard owner seq),
        "tuple<>" => composite!(row, ();),
        "tuple<u32,string>" => composite!(row, (u32, String); 0 1),
        "tuple<u32,string,optional<i64>>" => composite!(row, (u32, String, Option<i64>); 0 1 2),
        "tuple<string,u8>" => composite!(row, (String, u8); 0 1),
        "tuple<string,string>" => composite!(row, (String, String); 0 1),
        "tuple<bytes,i8>" => composite!(row, (Vec<u8>, i8); 0 1),
        "tuple<list<i16>,i8>" => composite!(row, (Vec<i16>, i8); 0 1),
        "tuple<optional<string>,string>" => composite!(row, (Option<String>, String); 0 1),
        "tuple<list<string>,string>" => composite!(row, (Vec<String>, String); 0 1),
        "tuple<bool,i8,i16,i32,i64,u8,u16,u32,u64>" => composite!(row, KInts; 0 1 2 3 4 5 6 7 8),
        "tuple<f32,f64>" => composite!(row, (f32, f64); 0 1),
        "tuple<time_point<micros>,string,duration<seconds>>" => composite!(row, KTime; 0 1 2),
        "tuple<array<u16,2>,string>" => composite!(row, ([u16; 2], String); 0 1),
        "tuple<variant<u8,string>,u8>" => composite!(row, (U8OrString, u8); 0 1),
        "tuple<tuple<string,u8>,string>" => composite!(row, ((String, u8), String); 0 1),
        other => Err(format!("unknown type {other}")),
    }
}

#[test]
fn every_vector_holds() {
    let fx = fixture();
    assert_eq!(fx.version, 1);
    assert!(fx.vectors.len() >= 500);
    let mut names = BTreeSet::new();
    let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
    let mut types = BTreeSet::new();
    let mut failures = Vec::new();
    let mut checked = 0;
    for row in &fx.vectors {
        assert!(names.insert(&row.name), "duplicate row {}", row.name);
        match dispatch(row) {
            Ok(()) => checked += 1,
            Err(why) => failures.push(format!("{}: {why}", row.name)),
        }
        *kinds.entry(&row.kind).or_default() += 1;
        types.insert(row.r#type.as_str());
    }
    assert!(failures.is_empty(), "{} rows failed:\n{}", failures.len(), failures.join("\n"));
    // Nothing skipped: every row was checked, every kind and declared type
    // has rows, and every type with rows is one the fixture declares.
    assert_eq!(checked, fx.vectors.len());
    assert_eq!(
        kinds.keys().copied().collect::<Vec<_>>(),
        [
            "bound",
            "key-invariant",
            "key-order",
            "key-order-bits",
            "key-prefix",
            "key-reject",
            "text-accept",
            "text-reject",
            "value"
        ]
    );
    for ty in &fx.types {
        assert!(types.contains(ty.id.as_str()), "no rows for {}", ty.id);
        // Prefix rows exist only for composite types.
        let has_prefix =
            fx.vectors.iter().any(|r| r.r#type == ty.id && r.kind == "key-prefix");
        assert!(!has_prefix || ty.composite, "prefix rows for non-composite {}", ty.id);
    }
    assert_eq!(types.len(), fx.types.len() + 1); // + "bytes" of the bound rows    assert_eq!(kinds["key-invariant"], 10);
    assert_eq!(checked, 1147, "every row of the fixture was checked");

    // A prefix encodes elements: each is checked as the value it is; the
    // composite as a whole is not (a prefix is not a value of it).
    let bad = Name { value: 1 << 60 };
    assert_eq!(
        try_key_prefix::<Guards, _>(&(&bad,)).map_err(|e| e.0),
        Err("Name: value is not a canonical name")
    );
    let good = Name { value: 5 };
    assert_eq!(try_key_prefix::<Guards, _>(&(&good,)), Ok(to_key(&good)));
    assert_eq!(
        try_key_prefix::<Guards, _>(&(&good, &Some(Range { lo: 2, hi: 1 }))).map_err(|e| e.0),
        Err("Range: lo is greater than hi")
    );
}

/// The Schema IR Rust generates for the time types is the IR C++ generates:
/// `Custom{Int{bits, signed}, id}` with the id of docs/time.md.
#[test]
fn schema_ids() {
    fn id_of<T: ToPsszSchema>() -> (String, u32, bool) {
        let (schema, root) = schema_for::<T>().expect("schema");
        let root = schema.resolve(&root).expect("resolve");
        let AnyValue::Custom(custom) = &root.value else { panic!("not Custom: {root:?}") };
        let inner = schema.resolve(&custom.r#type).expect("resolve");
        let AnyValue::Int(int) = &inner.value else { panic!("not Int") };
        (custom.id.clone(), int.bits, int.isSigned)
    }
    assert_eq!(id_of::<TimePoint<Seconds>>(), ("time-point-seconds".into(), 64, true));
    assert_eq!(id_of::<TimePoint<Millis>>(), ("time-point-millis".into(), 64, true));
    assert_eq!(id_of::<TimePoint<Micros>>(), ("time-point-micros".into(), 64, true));
    assert_eq!(id_of::<TimePoint<Nanos>>(), ("time-point-nanos".into(), 64, true));
    assert_eq!(id_of::<TimePoint<Seconds, u32>>(), ("time-point-seconds".into(), 32, false));
    assert_eq!(id_of::<Duration<Seconds>>(), ("duration-seconds".into(), 64, true));
    assert_eq!(id_of::<Duration<Millis>>(), ("duration-millis".into(), 64, true));
    assert_eq!(id_of::<Duration<Micros>>(), ("duration-micros".into(), 64, true));
    assert_eq!(id_of::<Duration<Nanos>>(), ("duration-nanos".into(), 64, true));
    assert_eq!(id_of::<Duration<Minutes, i32>>(), ("duration-60/1".into(), 32, true));
    assert_eq!(id_of::<TimePoint<Minutes, i16>>(), ("time-point-60/1".into(), 16, true));
    assert_eq!(id_of::<MonotonicPoint<Seconds>>(), ("monotonic-seconds".into(), 64, true));
    assert_eq!(id_of::<MonotonicPoint<Micros>>(), ("monotonic-micros".into(), 64, true));
    assert_eq!(id_of::<MonotonicPoint<Millis, u32>>(), ("monotonic-millis".into(), 32, false));
    assert_eq!(id_of::<MonotonicPoint<Minutes, i16>>(), ("monotonic-60/1".into(), 16, true));
    schema_for::<Timer>().expect("Timer schema");
    // Records of time values derive their schema like any other record.
    schema_for::<Stamps>().expect("Stamps schema");
    schema_for::<Event>().expect("Event schema");
}

/// The same rows through the schema-driven codec (`pssz::dynamic`), using
/// nothing but the Schema IR recorded in the fixture: the binary form of a
/// time value is its tick integer, its text is the built-in presentation of
/// its time id.
#[test]
fn the_schema_ir_drives_pssz_and_text() {
    use psio::pssz::dynamic;
    use psio::pssz::schema_ir::{AnyType, Schema};
    let fx = fixture();
    let ir = unhex(&fx.schema_ir);
    psio::pssz::pssz_validate::<Pssz, Schema>(&ir).expect("schema_ir validates");
    let schema = psio::pssz::from_pssz::<Pssz, Schema>(&ir).expect("schema_ir decodes");
    // Every custom id must be handled: the time ids are built in, and the
    // custom types of conformance/custom-types.md have their handlers
    // (written by hand over the wire types' bytes).
    let mut strict = dynamic::CustomHandlers::new();
    custom_handlers::fixture_custom_handlers(&mut strict);
    strict.set_strict(true);
    let checked = dynamic::Options::handlers(&strict);

    let (mut values, mut accepted, mut rejected) = (0, 0, 0);
    let (mut keys, mut violated) = (0, 0);
    // The key of the value `pssz` through the schema-driven key encoder,
    // from the IR and the bytes alone.
    let key_of = |root: &AnyType, pssz: &[u8]| -> Result<Vec<u8>, String> {
        let value = dynamic::decode_exact::<Pssz>(&schema, root, pssz).map_err(|e| e.0.to_string())?;
        dynamic::to_key(&schema, root, &value).map_err(|e| e.0.to_string())
    };
    for row in &fx.vectors {
        let name = &row.name;
        if row.kind == "bound" {
            continue;
        }
        let root = AnyType::named(row.r#type.clone());
        match row.kind.as_str() {
            "value" => {
                let bytes = unhex(row.pssz.as_ref().unwrap());
                assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false), "{name}");
                assert_eq!(dynamic::validate_pssz_with(&schema, &root, &bytes, &strict), Ok(false), "{name}");
                // The key, with and without the registry.
                let key = unhex(row.key.as_ref().unwrap());
                assert_eq!(key_of(&root, &bytes), Ok(key.clone()), "{name}: key");
                let value = dynamic::decode_exact_with::<Pssz>(&schema, &root, &bytes, checked).unwrap();
                assert_eq!(dynamic::to_key_with(&schema, &root, &value, checked), Ok(key), "{name}: key");
                keys += 1;
                let json = dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &strict);
                let ijson = dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &strict);
                assert_eq!(json.is_ok(), row.json.is_some(), "{name}: json {json:?}");
                assert_eq!(ijson.is_ok(), row.ijson.is_some(), "{name}: ijson {ijson:?}");
                if let (Some(want_json), Some(want_ijson)) = (&row.json, &row.ijson) {
                    assert_eq!(&json.unwrap().0, want_json, "{name}");
                    assert_eq!(&ijson.unwrap().0, want_ijson, "{name}");
                    assert_eq!(
                        dynamic::json_to_pssz_with::<Json>(&schema, &root, want_json.as_bytes(), &strict)
                            .unwrap(),
                        bytes,
                        "{name}: json read"
                    );
                    assert_eq!(
                        dynamic::json_to_pssz_with::<IJson>(&schema, &root, want_ijson.as_bytes(), &strict)
                            .unwrap(),
                        bytes,
                        "{name}: ijson read"
                    );
                }
                values += 1;
            }
            "text-accept" => {
                let text = row.text.as_ref().unwrap().as_bytes();
                let want = unhex(row.pssz.as_ref().unwrap());
                assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, text).unwrap(), want, "{name}");
                assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, text).unwrap(), want, "{name}");
                assert_eq!(
                    &dynamic::pssz_to_json::<Json>(&schema, &root, &want).unwrap().0,
                    row.json.as_ref().unwrap(),
                    "{name}: canonical"
                );
                accepted += 1;
            }
            "text-reject" => {
                let text = row.text.as_ref().unwrap().as_bytes();
                assert!(dynamic::json_to_pssz::<Json>(&schema, &root, text).is_err(), "{name}");
                assert!(dynamic::json_to_pssz::<IJson>(&schema, &root, text).is_err(), "{name}");
                rejected += 1;
            }
            "key-order" | "key-order-bits" => {
                // The schema-driven keys ascend bytewise too.
                let mut last: Option<Vec<u8>> = None;
                for pssz in row.values.as_ref().unwrap() {
                    let bytes = unhex(pssz);
                    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false), "{name}");
                    let key = key_of(&root, &bytes).unwrap_or_else(|e| panic!("{name}: key: {e}"));
                    assert!(last.as_ref().is_none_or(|last| *last < key), "{name}: keys not ascending");
                    last = Some(key);
                }
            }
            "key-prefix" => {
                // The full key of the value starts with the prefix.
                let bytes = unhex(row.pssz.as_ref().unwrap());
                let key = key_of(&root, &bytes).unwrap_or_else(|e| panic!("{name}: key: {e}"));
                assert!(key.starts_with(&unhex(row.prefix.as_ref().unwrap())), "{name}");
            }
            "key-invariant" => {
                // A schema with no handler reads the wire type: the bytes
                // validate and their key is the row's. With the handlers the
                // value is refused, with the invariant's message, by the
                // readers and by the key encoder.
                let bytes = unhex(row.pssz.as_ref().unwrap());
                let reason = row.reason.as_ref().unwrap();
                assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false), "{name}");
                assert_eq!(key_of(&root, &bytes), Ok(unhex(row.key.as_ref().unwrap())), "{name}: key");
                assert_eq!(
                    dynamic::validate_pssz_with(&schema, &root, &bytes, &strict).map_err(|e| e.what().to_string()),
                    Err(reason.clone()),
                    "{name}"
                );
                let value = dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
                assert_eq!(
                    dynamic::to_key_with(&schema, &root, &value, checked).map_err(|e| e.0.to_string()),
                    Err(reason.clone()),
                    "{name}: key with the handlers"
                );
                violated += 1;
            }
            // The schema-driven codec has a key encoder, not a key reader.
            "key-reject" => (),
            other => panic!("{name}: unknown kind {other}"),
        }
    }
    assert!(values > 100 && accepted > 20 && rejected > 40, "{values} {accepted} {rejected}");
    assert_eq!(keys, fx.vectors.iter().filter(|row| row.kind == "value").count());
    assert_eq!(violated, 10, "key-invariant rows");
}
