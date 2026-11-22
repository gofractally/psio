//! Cross-language time / key conformance: consumes
//! conformance/time-key-vectors.json (conformance/time-key-vectors.md), the
//! fixture emitted from the C++ implementation
//! (cpp/tests/time_key_conformance_emit.cpp) and also checked by ctest and
//! the JS tests. Every row of every kind is checked through the typed Rust
//! API: `psio::pssz`, `psio::key`, `psio::json` and `psio::time`.

use psio::json::{self, IJson, Json, JsonText};
use psio::key::{from_key, key_prefix, key_prefix_upper_bound, key_size, to_key, validate_key};
use psio::key::{KeyPack, KeyUnpack};
use psio::pssz::schema_ir::{schema_for, AnyValue, ToPsszSchema};
use psio::pssz::{Pssz, PsszPack, PsszUnpack};
use psio::time::{Duration, Micros, Millis, Nanos, Seconds, TimePoint, Unit};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Debug;

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
}

#[derive(Debug, serde::Deserialize)]
struct TypeRow {
    id: String,
    composite: bool,
}

#[derive(Debug, serde::Deserialize)]
struct Fixture {
    version: u32,
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
            if back != value {
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
    match row.r#type.as_str() {
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
        ["bound", "key-order", "key-prefix", "key-reject", "text-accept", "text-reject", "value"]
    );
    for ty in &fx.types {
        assert!(types.contains(ty.id.as_str()), "no rows for {}", ty.id);
        // Prefix rows exist only for composite types.
        let has_prefix =
            fx.vectors.iter().any(|r| r.r#type == ty.id && r.kind == "key-prefix");
        assert!(!has_prefix || ty.composite, "prefix rows for non-composite {}", ty.id);
    }
    assert_eq!(types.len(), fx.types.len() + 1); // + "bytes" of the bound rows
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
    // Records of time values derive their schema like any other record.
    schema_for::<Stamps>().expect("Stamps schema");
    schema_for::<Event>().expect("Event schema");
}
