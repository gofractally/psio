//! `conformance/interface-vectors.json` (described in
//! `conformance/interface-vectors.md`): what a method declaration becomes.
//!
//! The interface is declared here in Rust; the fixture is what C++ built
//! from the same declaration. Every row is checked on the typed path (the
//! argument records and `Method::result_to_pssz` / `result_from_pssz`) and
//! on the schema-driven path over the fixture's own schema, and the schema
//! Rust builds is the fixture's byte for byte (its PSSZ and its Json text).

use psio::interface::{Interface, Method};
use psio::json::{self, IJson, Json, JsonText};
use psio::pssz::dynamic;
use psio::pssz::schema_ir::{AnyType, AnyValue as V, Schema, SchemaBuilder};
use psio::pssz::{auto_pssz_width, Pssz, PsszPack, RawString};
use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema, psio::JsonText)]
pub struct Receipt {
    pub id: u64,
    #[psio(max = 32)]
    pub note: String,
}

// conformance/interface-vectors.md, "The declaration".
#[psio::interface(name = "token", package = "iface_conf", derive(Debug, Clone, PartialEq))]
pub trait Token {
    #[pssz(max_dynamic_data = 256)]
    #[psio(max_fields = 8)]
    fn transfer(
        &mut self,
        #[psio(max = 16)] to: String,
        amount: u64,
        #[psio(max = 80)] memo: Option<String>,
    ) -> Receipt;
    #[psio(returns(max = 4, element(max = 16)))]
    fn names(&self, limit: u32) -> Vec<String>;
    #[pssz(max_dynamic_data = 64)]
    fn note(&mut self, #[psio(max = 62)] text: RawString);
    #[psio(positional)]
    fn add(&mut self, a: u32, b: u32) -> u64;
    fn ping(&mut self);
    fn batch(&mut self, #[psio(max = 4, element(max = 8))] names: Vec<RawString>, tip: u8) -> bool;
}

#[derive(serde::Deserialize)]
struct MethodRow {
    name: String,
    params: Vec<String>,
    named: bool,
    args: String,
    result: bool,
    max_dynamic_data: Option<u64>,
    max_fields: Option<u64>,
    width: usize,
    inferred_bound: Option<u64>,
    max_encoded_size: Option<u64>,
}
#[derive(serde::Deserialize)]
struct Row {
    name: String,
    kind: String,
    method: String,
    pssz: Option<String>,
    json: Option<String>,
    ijson: Option<String>,
    text: Option<String>,
    #[allow(dead_code)]
    reason: Option<String>,
}
#[derive(serde::Deserialize)]
struct Fixture {
    version: u32,
    interface: String,
    schema_ir: String,
    schema_json: String,
    methods: Vec<MethodRow>,
    vectors: Vec<Row>,
}

fn fixture() -> Fixture {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/interface-vectors.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}
fn unhex(hex: &str) -> Vec<u8> {
    (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn the_declaration_builds_the_fixture_schema() {
    let fx = fixture();
    assert_eq!(fx.version, 1);
    assert_eq!(fx.interface, token::Tag::NAME);
    let theirs: Schema = psio::from_pssz(&unhex(&fx.schema_ir)).unwrap();
    // The schema's two serialized forms are one schema, and it is valid.
    assert_eq!(json::decode::<Json, Schema>(&fx.schema_json).unwrap(), theirs);
    assert_eq!(json::try_encode::<Json, _>(&theirs).unwrap(), fx.schema_json);
    dynamic::validate_schema(&theirs).unwrap();

    // What the C++ emitter does: the interface, then every argument list
    // under the name `token.<method>`.
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<token::Tag>().unwrap();
    builder.insert_argument_lists::<token::Tag>().unwrap();
    let ours = builder.finish();
    dynamic::validate_schema(&ours).unwrap();

    // The same schema, byte for byte: named and anonymous nodes, the order
    // of `types`, the `@N` numbering, the aliases and the attributes.
    assert_eq!(json::try_encode::<Json, _>(&ours).unwrap(), fx.schema_json);
    assert_eq!(hex(&psio::to_pssz(&ours)), fx.schema_ir);
    assert_eq!(ours, theirs);

    // Naming the argument lists one by one is the same thing, and naming
    // one again adds nothing.
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<token::Tag>().unwrap();
    builder.insert_named::<token::args::transfer>("token.transfer").unwrap();
    builder.insert_named::<token::args::names>("token.names").unwrap();
    builder.insert_named::<token::args::note>("token.note").unwrap();
    builder.insert_named::<token::args::add>("token.add").unwrap();
    builder.insert_named::<token::args::ping>("token.ping").unwrap();
    builder.insert_named::<token::args::batch>("token.batch").unwrap();
    builder.insert_named::<token::args::batch>("token.batch").unwrap();
    builder.insert_argument_lists::<token::Tag>().unwrap();
    // A name another entry has is refused.
    assert!(builder.insert_named::<u8>("token.batch").is_err());
    assert!(builder.insert_named::<u8>("@99").is_err());
    assert_eq!(builder.finish(), theirs);

    // An argument list is an anonymous type: `token.transfer` is an alias
    // of its `@N` node, an Object with `tuple`, then `maxFields` and
    // `maxDynamicData` when declared.
    let alias = ours.types.iter().position(|entry| entry.name == "token.transfer").unwrap();
    assert_eq!(ours.types[alias - 1].name, "@8");
    assert_eq!(*ours.types[alias].r#type, AnyType::named("@8"));
    match &ours.types[alias - 1].r#type.value {
        V::Object(o) => assert_eq!(
            o.attributes.iter().map(|a| (a.name.as_str(), a.value.as_deref())).collect::<Vec<_>>(),
            [("tuple", Some("named")), ("maxFields", Some("8")), ("maxDynamicData", Some("256"))]
        ),
        other => panic!("{other:?}"),
    }
}

fn method_row<M: Method + PsszPack<Pssz>>(row: &MethodRow) {
    assert_eq!(M::NAME, row.name);
    assert_eq!(M::PARAM_NAMES, row.params.iter().map(String::as_str).collect::<Vec<_>>(), "{}", row.name);
    assert_eq!(M::NAMED, row.named, "{}", row.name);
    assert_eq!(M::HAS_RESULT, row.result, "{}", row.name);
    assert_eq!(<M as PsszPack<Pssz>>::MAX_DYNAMIC_DATA.map(|n| n as u64), row.max_dynamic_data, "{}", row.name);
    assert_eq!(auto_pssz_width::<M>(), row.width, "{}", row.name);
    // The inferred bound: the cap is not part of it. The maximum encoded
    // size: the largest argument list (docs/pssz-spec.md 3.7).
    assert_eq!(<M as PsszPack<Pssz>>::INFERRED_BOUND.map(|n| n as u64), row.inferred_bound, "{}", row.name);
    assert_eq!(<M as PsszPack<Pssz>>::MAX_ENCODED_SIZE.map(|n| n as u64), row.max_encoded_size, "{}", row.name);
    assert_eq!(psio::pssz::inferred_bound::<M>(), <M as PsszPack<Pssz>>::INFERRED_BOUND);
    assert_eq!(psio::pssz::max_encoded_size::<M>(), <M as PsszPack<Pssz>>::MAX_ENCODED_SIZE);
    let info = &token::Tag::METHODS[M::INDEX];
    assert_eq!((info.name, info.params, info.named, info.has_result), (M::NAME, M::PARAM_NAMES, M::NAMED, M::HAS_RESULT));
    assert_eq!(row.args, format!("token.{}", row.name));
}

#[test]
fn the_methods_are_what_the_fixture_says() {
    let fx = fixture();
    let rows: BTreeMap<&str, &MethodRow> = fx.methods.iter().map(|m| (m.name.as_str(), m)).collect();
    assert_eq!(fx.methods.iter().map(|m| m.name.as_str()).collect::<Vec<_>>(), token::Tag::METHODS.iter().map(|m| m.name).collect::<Vec<_>>());
    method_row::<token::args::transfer>(rows["transfer"]);
    method_row::<token::args::names>(rows["names"]);
    method_row::<token::args::note>(rows["note"]);
    method_row::<token::args::add>(rows["add"]);
    method_row::<token::args::ping>(rows["ping"]);
    method_row::<token::args::batch>(rows["batch"]);
    // The parameter-count cap is the Func's and the argument list's
    // `maxFields` (compared in the schema test); the fixture restates it.
    assert_eq!(rows["transfer"].max_fields, Some(8));
    assert!(fx.methods.iter().filter(|m| m.name != "transfer").all(|m| m.max_fields.is_none()));
}

/// One row on the typed path. `M` is the method's argument record.
fn typed<M>(row: &Row) -> Result<(), String>
where
    M: Method + JsonText + PartialEq + std::fmt::Debug + psio::pssz::PsszValidate<Pssz>,
    M::Result: JsonText + PartialEq + std::fmt::Debug,
{
    let bytes = row.pssz.as_deref().map(unhex);
    let texts = |json: &Option<String>, ijson: &Option<String>| -> Result<(String, String), String> {
        Ok((json.clone().ok_or("no json")?, ijson.clone().ok_or("no ijson")?))
    };
    match row.kind.as_str() {
        "args" => {
            let bytes = bytes.ok_or("no pssz")?;
            let (json_text, ijson_text) = texts(&row.json, &row.ijson)?;
            let value = psio::from_pssz::<M>(&bytes).map_err(|e| format!("from_pssz: {e}"))?;
            if psio::try_to_pssz(&value).map_err(|e| e.to_string())? != bytes {
                return Err("re-encoded bytes differ".into());
            }
            // Without a cap the arguments are the tuple of the parameters
            // (a tuple has no cap of its own, so under one the widths differ).
            if <M as PsszPack<Pssz>>::MAX_DYNAMIC_DATA.is_none() {
                psio::from_pssz::<M::Tuple>(&bytes).map_err(|e| format!("as the tuple: {e}"))?;
            }
            let written = json::try_encode::<Json, _>(&value).map_err(|e| e.to_string())?;
            if written != json_text {
                return Err(format!("json is {written}"));
            }
            let written = json::try_encode::<IJson, _>(&value).map_err(|e| e.to_string())?;
            if written != ijson_text {
                return Err(format!("ijson is {written}"));
            }
            if json::decode::<Json, M>(&json_text).as_ref() != Ok(&value)
                || json::decode::<IJson, M>(&ijson_text).as_ref() != Ok(&value)
            {
                return Err("the text does not read back".into());
            }
        }
        "args-accept" => {
            let bytes = bytes.ok_or("no pssz")?;
            let text = row.text.as_ref().ok_or("no text")?;
            for (format, value) in [("json", json::decode::<Json, M>(text)), ("ijson", json::decode::<IJson, M>(text))] {
                let value = value.map_err(|e| format!("{format}: {e}"))?;
                if psio::try_to_pssz(&value).map_err(|e| e.to_string())? != bytes {
                    return Err(format!("{format}: other arguments"));
                }
                let canonical = json::try_encode::<Json, _>(&value).map_err(|e| e.to_string())?;
                if Some(&canonical) != row.json.as_ref() {
                    return Err(format!("canonical text is {canonical}"));
                }
            }
        }
        "args-reject" => {
            let text = row.text.as_ref().ok_or("no text")?;
            if let Ok(value) = json::decode::<Json, M>(text) {
                return Err(format!("json accepted {value:?}"));
            }
            if let Ok(value) = json::decode::<IJson, M>(text) {
                return Err(format!("ijson accepted {value:?}"));
            }
            if json::validate::<Json, M>(text).is_ok() || json::validate::<IJson, M>(text).is_ok() {
                return Err("validated".into());
            }
        }
        "args-pssz-reject" => {
            let bytes = bytes.ok_or("no pssz")?;
            if let Ok(value) = psio::from_pssz::<M>(&bytes) {
                return Err(format!("accepted {value:?}"));
            }
            if psio::validate_pssz::<M>(&bytes).is_ok() {
                return Err("validated".into());
            }
        }
        "result" => {
            let bytes = bytes.ok_or("no pssz")?;
            let (json_text, ijson_text) = texts(&row.json, &row.ijson)?;
            // The result is encoded as the type `returns(...)` makes of it.
            let value = M::result_from_pssz(&bytes).map_err(|e| format!("result_from_pssz: {e}"))?;
            if M::result_to_pssz(&value).map_err(|e| e.to_string())? != bytes {
                return Err(format!("result bytes are {}", hex(&M::result_to_pssz(&value).unwrap())));
            }
            let written = json::try_encode::<Json, _>(&value).map_err(|e| e.to_string())?;
            if written != json_text {
                return Err(format!("json is {written}"));
            }
            let written = json::try_encode::<IJson, _>(&value).map_err(|e| e.to_string())?;
            if written != ijson_text {
                return Err(format!("ijson is {written}"));
            }
            if json::decode::<Json, M::Result>(&json_text).as_ref() != Ok(&value)
                || json::decode::<IJson, M::Result>(&ijson_text).as_ref() != Ok(&value)
            {
                return Err("the text does not read back".into());
            }
        }
        "result-pssz-reject" => {
            let bytes = bytes.ok_or("no pssz")?;
            if let Ok(value) = M::result_from_pssz(&bytes) {
                return Err(format!("accepted {value:?}"));
            }
        }
        other => return Err(format!("unknown kind {other}")),
    }
    Ok(())
}

/// One row on the schema-driven path, over the fixture's own schema: the
/// root is `token.<method>` for arguments and `Func.result` for a result.
fn schema_driven(schema: &Schema, row: &Row) -> Result<(), String> {
    let func = schema.interfaces[0].funcs.iter().find(|f| f.name == row.method).ok_or("no such method")?;
    let root = if row.kind.starts_with("result") {
        func.result.as_deref().cloned().ok_or("the method has no result")?
    } else {
        AnyType::named(format!("token.{}", row.method))
    };
    let bytes = row.pssz.as_deref().map(unhex);
    let text = |t: &Option<String>| t.clone().ok_or("the row lacks a text");
    match row.kind.as_str() {
        "args" | "result" => {
            let bytes = bytes.ok_or("no pssz")?;
            if dynamic::validate(schema, &root, &bytes) != Ok(false) {
                return Err("the frame does not validate".into());
            }
            let (json_text, ijson_text) = (text(&row.json)?, text(&row.ijson)?);
            let (written, _) = dynamic::json::pssz_to_json::<Json>(schema, &root, &bytes).map_err(|e| e.to_string())?;
            if written != json_text {
                return Err(format!("json is {written}"));
            }
            let (written, _) = dynamic::json::pssz_to_json::<IJson>(schema, &root, &bytes).map_err(|e| e.to_string())?;
            if written != ijson_text {
                return Err(format!("ijson is {written}"));
            }
            if dynamic::json::json_to_pssz::<Json>(schema, &root, json_text.as_bytes()).as_deref() != Ok(&bytes[..])
                || dynamic::json::json_to_pssz::<IJson>(schema, &root, ijson_text.as_bytes()).as_deref() != Ok(&bytes[..])
            {
                return Err("the text does not read back".into());
            }
        }
        "args-accept" => {
            let bytes = bytes.ok_or("no pssz")?;
            let source = text(&row.text)?;
            let a = dynamic::json::json_to_pssz::<Json>(schema, &root, source.as_bytes()).map_err(|e| e.to_string())?;
            let b = dynamic::json::json_to_pssz::<IJson>(schema, &root, source.as_bytes()).map_err(|e| e.to_string())?;
            if a != bytes || b != bytes {
                return Err("other arguments".into());
            }
            let (canonical, _) = dynamic::json::pssz_to_json::<Json>(schema, &root, &a).map_err(|e| e.to_string())?;
            if Some(&canonical) != row.json.as_ref() {
                return Err(format!("canonical text is {canonical}"));
            }
        }
        "args-reject" => {
            let source = text(&row.text)?;
            if dynamic::json::json_to_pssz::<Json>(schema, &root, source.as_bytes()).is_ok()
                || dynamic::json::json_to_pssz::<IJson>(schema, &root, source.as_bytes()).is_ok()
            {
                return Err("accepted".into());
            }
        }
        "args-pssz-reject" | "result-pssz-reject" => {
            let bytes = bytes.ok_or("no pssz")?;
            if dynamic::validate(schema, &root, &bytes).is_ok() {
                return Err("validated".into());
            }
            if dynamic::decode_exact::<Pssz>(schema, &root, &bytes).is_ok() {
                return Err("decoded".into());
            }
        }
        other => return Err(format!("unknown kind {other}")),
    }
    Ok(())
}

#[test]
fn every_row_holds_typed_and_schema_driven() {
    let fx = fixture();
    let schema: Schema = psio::from_pssz(&unhex(&fx.schema_ir)).unwrap();
    let mut failures = Vec::new();
    let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
    for row in &fx.vectors {
        *kinds.entry(row.kind.as_str()).or_default() += 1;
        let result = match row.method.as_str() {
            "transfer" => typed::<token::args::transfer>(row),
            "names" => typed::<token::args::names>(row),
            "note" => typed::<token::args::note>(row),
            "add" => typed::<token::args::add>(row),
            "ping" => typed::<token::args::ping>(row),
            "batch" => typed::<token::args::batch>(row),
            other => Err(format!("unknown method {other}")),
        };
        if let Err(why) = result {
            failures.push(format!("{} (typed): {why}", row.name));
        }
        if let Err(why) = schema_driven(&schema, row) {
            failures.push(format!("{} (schema): {why}", row.name));
        }
    }
    assert!(failures.is_empty(), "{} failures:\n{}", failures.len(), failures.join("\n"));
    // Nothing skipped: every row of every kind, on both paths.
    let want: BTreeMap<&str, usize> = [
        ("args", 13),
        ("args-accept", 11),
        ("args-reject", 17),
        ("args-pssz-reject", 4),
        ("result", 7),
        ("result-pssz-reject", 3),
    ]
    .into();
    assert_eq!(kinds, want);
    assert_eq!(fx.vectors.len(), 55);
    assert_eq!(fx.methods.len(), 6);
}

/// Dispatch reads the arguments the fixture pins and answers with the
/// result's bounded encoding.
#[test]
fn dispatch_speaks_the_fixture_bytes() {
    struct Ledger;
    impl Token for Ledger {
        fn transfer(&mut self, to: String, amount: u64, memo: Option<String>) -> Receipt {
            Receipt { id: amount + 2, note: format!("{to}{}", memo.map_or(String::new(), |m| format!(":{m}"))) }
        }
        fn names(&self, limit: u32) -> Vec<String> {
            ["alice", "bob", "carol", "dave", "eve"].iter().take(limit as usize).map(|s| s.to_string()).collect()
        }
        fn note(&mut self, _text: RawString) {}
        fn add(&mut self, a: u32, b: u32) -> u64 {
            a as u64 + b as u64
        }
        fn ping(&mut self) {}
        fn batch(&mut self, names: Vec<RawString>, _tip: u8) -> bool {
            !names.is_empty()
        }
    }
    let mut ledger = Ledger;
    // result/names/two: a 1-byte offset table, from `returns(...)`.
    let reply = token::dispatch_name(&mut ledger, "names", &unhex("0402000000")).unwrap();
    assert_eq!(hex(&reply), "0207616c696365626f62");
    // Five names are over the result's bound: the result has no encoding.
    assert!(matches!(
        token::dispatch_name(&mut ledger, "names", &unhex("0405000000")),
        Err(psio::interface::DispatchError::Result(_))
    ));
    // args/add/sum, args/ping/nothing, args/batch/two.
    assert_eq!(hex(&token::dispatch_name(&mut ledger, "add", &unhex("080100000002000000")).unwrap()), "0300000000000000");
    assert_eq!(token::dispatch_name(&mut ledger, "ping", &unhex("00")).unwrap(), b"");
    assert_eq!(token::dispatch_name(&mut ledger, "batch", &unhex("0202070203616263")).unwrap(), [1]);
    // args-pssz-reject/transfer/to_over_bound.
    assert!(matches!(
        token::dispatch_name(&mut ledger, "transfer", &unhex("0a000a0001000000000000006161616161616161616161616161616161")),
        Err(psio::interface::DispatchError::Arguments(_))
    ));
}
