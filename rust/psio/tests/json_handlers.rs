//! Custom handlers that present any JSON value (`CustomHandlers::add_json`;
//! C++ `to_json` / `from_json`, `cpp/tests/json_api_tests.cpp` "a custom
//! handler presents a JSON value"). The motivating shape: a packed
//! `Action{sender, service, method, rawData}` shown as
//! `{"sender":…,"service":…,"method":…,"data":{…}}`, where `data` is the
//! argument list decoded with the CALLED service's schema, which the handler
//! looks up.

use psio::json::{IJson, Json};
use psio::pssz::dynamic::{self, CustomHandlers, JsonContext};
use psio::pssz::schema_ir::{schema_for, AnyType, Schema, SchemaBuilder};
use psio::pssz::PsszError;
use psio::{JsonValue, PsszPack, PsszUnpack, ToPsszSchema};
use std::collections::{BTreeMap, HashMap};
use std::sync::Arc;

/// An action as it is packed: the arguments are bytes only the called
/// service's schema explains.
// (JsonText: the typed text of the wire record, which an interface's
// argument records need of their members; the schema-driven presentation
// is the handler's.)
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, psio::JsonText)]
#[psio(custom = "Action")]
pub struct Action {
    sender: String,
    service: String,
    method: String,
    #[psio(name = "rawData")]
    raw_data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
struct Holder {
    note: String,
    actions: Vec<Action>,
}

/// The called service. Its schema is a second schema, with one type per
/// argument list: `tokens.credit`, `tokens.relay`.
#[psio::interface(name = "tokens")]
pub trait Tokens {
    fn credit(&mut self, amount: u64, memo: Option<String>);
    /// An action inside the arguments of another.
    fn relay(&mut self, inner: Action, times: u32);
}

fn tokens_schema() -> Schema {
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<tokens::Tag>().unwrap();
    builder.insert::<tokens::args::credit>().unwrap();
    builder.insert::<tokens::args::relay>().unwrap();
    builder.finish()
}

fn credit(amount: u64, memo: Option<&str>) -> Vec<u8> {
    psio::to_pssz(&tokens::args::credit { amount, memo: memo.map(Into::into) })
}
fn action(service: &str, method: &str, raw_data: Vec<u8>) -> Action {
    Action { sender: "alice".into(), service: service.into(), method: method.into(), raw_data }
}

/// The schemas of the services an action may call, by service name.
type Services = Arc<HashMap<String, Schema>>;

fn lookup<'s>(services: &'s Services, service: &str, method: &str) -> Result<(&'s Schema, AnyType), PsszError> {
    let schema = services.get(service).ok_or_else(|| PsszError::from(format!("no schema for service {service}")))?;
    let root = AnyType::named(format!("{service}.{method}"));
    if schema.get(&format!("{service}.{method}")).is_none() {
        return Err(format!("service {service} has no method {method}").into());
    }
    Ok((schema, root))
}

fn action_handlers() -> CustomHandlers {
    let services: Services = Arc::new(HashMap::from([("tokens".to_string(), tokens_schema())]));
    let mut handlers = CustomHandlers::new();
    let (writing, reading) = (services.clone(), services);
    handlers.add_json(
        "Action",
        move |pssz: &[u8], context: &JsonContext<'_>| {
            let a: Action = psio::from_pssz(pssz)?;
            let (schema, root) = lookup(&writing, &a.service, &a.method)?;
            // `data`: the arguments, by the called service's schema, with
            // the registry in use (an action nested in them is presented by
            // this same handler) and in the format being written.
            let data = if context.ijson {
                dynamic::pssz_to_json_with::<IJson>(schema, &root, &a.raw_data, context.handlers)?.0
            } else {
                dynamic::pssz_to_json_with::<Json>(schema, &root, &a.raw_data, context.handlers)?.0
            };
            // (any spacing: the codec writes the value compact)
            Ok(format!(
                "{{ \"sender\": {}, \"service\": {}, \"method\": {},\n  \"data\" : {data} }}",
                psio::to_json(&a.sender),
                psio::to_json(&a.service),
                psio::to_json(&a.method),
            ))
        },
        move |json: &str, context: &JsonContext<'_>| {
            const SHAPE: &str = "an action is {sender, service, method, data}";
            let v: JsonValue = psio::from_json(json).map_err(|e| PsszError(e.what))?;
            let text = |key: &str| v.find(key).and_then(JsonValue::as_str).ok_or(PsszError::new(SHAPE));
            let (sender, service, method) = (text("sender")?, text("service")?, text("method")?);
            let data = psio::to_json(v.find("data").ok_or(PsszError::new(SHAPE))?);
            let (schema, root) = lookup(&reading, service, method)?;
            let raw_data = if context.ijson {
                dynamic::json_to_pssz_with::<IJson>(schema, &root, data.as_bytes(), context.handlers)?
            } else {
                dynamic::json_to_pssz_with::<Json>(schema, &root, data.as_bytes(), context.handlers)?
            };
            Ok(psio::to_pssz(&Action {
                sender: sender.into(),
                service: service.into(),
                method: method.into(),
                raw_data,
            }))
        },
    );
    handlers
}

#[test]
fn a_packed_action_is_shown_with_its_arguments_decoded_by_the_called_services_schema() {
    let (schema, root) = schema_for::<Holder>().unwrap();
    let value = Holder {
        note: "n".into(),
        actions: vec![action("tokens", "credit", credit(5, Some("hi"))), action("tokens", "credit", credit(7, None))],
    };
    let bytes = psio::to_pssz(&value);
    let handlers = action_handlers();
    assert!(handlers.has_json_form("Action") && !handlers.has_text_form("Action"));
    // An object where a text handler gives one string: compact, whatever
    // spacing the handler wrote.
    let expected = concat!(
        r#"{"note":"n","actions":["#,
        r#"{"sender":"alice","service":"tokens","method":"credit","data":{"amount":5,"memo":"hi"}},"#,
        r#"{"sender":"alice","service":"tokens","method":"credit","data":{"amount":7,"memo":null}}]}"#
    );
    assert_eq!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &handlers).unwrap().0, expected);
    assert_eq!(dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &handlers).unwrap().0, expected);
    // And back: members in any order, any spacing, a missing optional.
    let input = concat!(
        r#"{ "actions" : [ {"data":{"memo":"hi","amount":5}, "method":"credit", "sender":"alice", "service":"tokens"},"#,
        r#" {"sender":"alice","service":"tokens","method":"credit","data":{"amount":7}} ], "note":"n" }"#
    );
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, input.as_bytes(), &handlers).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz_with::<Json>(&schema, &root, input.as_bytes(), &handlers).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, expected.as_bytes(), &handlers).unwrap(), bytes);
    // The dialect in use reaches the nested conversion: a u64 beyond 2^53
    // is a string under IJson and a number under Json.
    let big = Holder { note: String::new(), actions: vec![action("tokens", "credit", credit(u64::MAX, None))] };
    let big_bytes = psio::to_pssz(&big);
    let ijson = dynamic::pssz_to_json_with::<IJson>(&schema, &root, &big_bytes, &handlers).unwrap().0;
    let json = dynamic::pssz_to_json_with::<Json>(&schema, &root, &big_bytes, &handlers).unwrap().0;
    assert!(ijson.contains(r#""data":{"amount":"18446744073709551615","memo":null}"#), "{ijson}");
    assert!(json.contains(r#""data":{"amount":18446744073709551615,"memo":null}"#), "{json}");
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, ijson.as_bytes(), &handlers).unwrap(), big_bytes);
    assert_eq!(dynamic::json_to_pssz_with::<Json>(&schema, &root, json.as_bytes(), &handlers).unwrap(), big_bytes);
    // Without the handler the node is its wire type.
    let plain = dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0;
    assert!(
        plain.starts_with(r#"{"note":"n","actions":[{"sender":"alice","service":"tokens","method":"credit","rawData":""#),
        "{plain}"
    );
    // The binary side is the wire type's with or without the registry.
    assert!(dynamic::validate_pssz_with(&schema, &root, &bytes, &handlers).is_ok());

    // The handler's own error is the conversion's.
    let other = psio::to_pssz(&Holder { note: "n".into(), actions: vec![action("nft", "mint", vec![])] });
    let error = dynamic::pssz_to_json_with::<IJson>(&schema, &root, &other, &handlers).unwrap_err();
    assert_eq!(error.what(), "no schema for service nft");
    let read = |text: &str| dynamic::json_to_pssz_with::<IJson>(&schema, &root, text.as_bytes(), &handlers);
    let error = read(r#"{"note":"n","actions":[{"sender":"a","service":"nft","method":"mint","data":{}}]}"#).unwrap_err();
    assert_eq!(error.what(), "no schema for service nft");
    let error = read(r#"{"note":"n","actions":[{"sender":"a","service":"tokens","method":"burn","data":{}}]}"#).unwrap_err();
    assert_eq!(error.what(), "service tokens has no method burn");
    assert_eq!(read(r#"{"note":"n","actions":["x"]}"#).unwrap_err().what(), "an action is {sender, service, method, data}");
    // An error inside the nested conversion comes through too.
    let error =
        read(r#"{"note":"n","actions":[{"sender":"a","service":"tokens","method":"credit","data":{"memo":"x"}}]}"#)
            .unwrap_err();
    assert_eq!(error.what(), r#"json: missing required member "amount""#);
    // Arguments that are not a value of the called method's argument list.
    let broken = psio::to_pssz(&Holder { note: "n".into(), actions: vec![action("tokens", "credit", vec![0xff; 3])] });
    assert!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &broken, &handlers).is_err());
}

#[test]
fn an_action_inside_the_arguments_of_another_is_presented_through_the_registry() {
    let (schema, root) = schema_for::<Holder>().unwrap();
    let inner = action("tokens", "credit", credit(1, Some("in")));
    let relay = psio::to_pssz(&tokens::args::relay { inner, times: 2 });
    let bytes = psio::to_pssz(&Holder { note: String::new(), actions: vec![action("tokens", "relay", relay)] });
    let handlers = action_handlers();
    let expected = concat!(
        r#"{"note":"","actions":[{"sender":"alice","service":"tokens","method":"relay","data":"#,
        r#"{"inner":{"sender":"alice","service":"tokens","method":"credit","data":{"amount":1,"memo":"in"}},"times":2}}]}"#
    );
    assert_eq!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &handlers).unwrap().0, expected);
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, expected.as_bytes(), &handlers).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz_with::<Json>(&schema, &root, expected.as_bytes(), &handlers).unwrap(), bytes);
    // The called service's argument list also reads as the positional array.
    let positional = expected.replace(r#"{"amount":1,"memo":"in"}"#, r#"[1,"in"]"#);
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, positional.as_bytes(), &handlers).unwrap(), bytes);
}

#[test]
fn a_handler_cannot_make_the_document_malformed() {
    let (schema, root) = schema_for::<Holder>().unwrap();
    let bytes = psio::to_pssz(&Holder { note: "n".into(), actions: vec![action("tokens", "credit", vec![])] });
    let writes = |text: &'static str| {
        let mut handlers = CustomHandlers::new();
        handlers.add_json(
            "Action",
            move |_: &[u8], _: &JsonContext<'_>| Ok(text.to_string()),
            |_: &str, _: &JsonContext<'_>| Err(PsszError::new("not read")),
        );
        dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &handlers)
    };
    for text in [r#"{"a":1"#, "1,2", "", r#"{"a":1} x"#, "nul", "\"\u{FFFF}\""] {
        assert_eq!(
            writes(text).unwrap_err().what(),
            "json: a custom handler wrote text that is not one JSON value",
            "{text:?}"
        );
    }
    // Any one value will do: a number, an array, a string, null.
    for (text, compact) in [("17", "17"), (r#"[1, "a"]"#, r#"[1,"a"]"#), (r#""text""#, r#""text""#), ("null", "null"), (" 1.50 ", "1.50")]
    {
        assert_eq!(writes(text).unwrap().0, format!(r#"{{"note":"n","actions":[{compact}]}}"#), "{text}");
    }
}

/// A numeric id with both forms: a value is presented by the JSON form, an
/// object key by the text form.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, PsszPack, PsszUnpack, ToPsszSchema, psio::Key)]
#[psio(transparent, custom = "Id")]
struct Id(u32);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
struct Ledger {
    owner: Id,
    balances: BTreeMap<Id, u64>,
}

#[test]
fn a_text_form_still_decides_the_object_key_and_works_unchanged() {
    let (schema, root) = schema_for::<Ledger>().unwrap();
    let value = Ledger { owner: Id(7), balances: BTreeMap::from([(Id(1), 10), (Id(2), 20)]) };
    let bytes = psio::to_pssz(&value);
    let to_text = |pssz: &[u8]| Ok(format!("id-{}", psio::from_pssz::<u32>(pssz)?));
    let from_text = |text: &str| {
        let n: u32 = text.strip_prefix("id-").and_then(|n| n.parse().ok()).ok_or(PsszError::new("not an id"))?;
        Ok(psio::to_pssz(&n))
    };
    // The text form alone: one JSON string, as before.
    let mut text_only = CustomHandlers::new();
    text_only.add("Id", to_text, from_text);
    let text = r#"{"owner":"id-7","balances":{"id-1":10,"id-2":20}}"#;
    assert_eq!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &text_only).unwrap().0, text);
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, text.as_bytes(), &text_only).unwrap(), bytes);
    // Both forms: the value is the JSON form, the key stays the text form.
    let mut both = CustomHandlers::new();
    both.add("Id", to_text, from_text);
    both.add_json(
        "Id",
        |pssz: &[u8], _: &JsonContext<'_>| Ok(format!(r#"{{"id":{}}}"#, psio::from_pssz::<u32>(pssz)?)),
        |json: &str, _: &JsonContext<'_>| {
            let v: JsonValue = psio::from_json(json).map_err(|e| PsszError(e.what))?;
            let n = v.find("id").and_then(JsonValue::as_number).and_then(|n| n.as_u64()).ok_or(PsszError::new("an id is {id}"))?;
            Ok(psio::to_pssz(&(n as u32)))
        },
    );
    assert!(both.has_json_form("Id") && both.has_text_form("Id"));
    let text = r#"{"owner":{"id":7},"balances":{"id-1":10,"id-2":20}}"#;
    assert_eq!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &both).unwrap().0, text);
    assert_eq!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, text.as_bytes(), &both).unwrap(), bytes);
    assert_eq!(
        dynamic::json_to_pssz_with::<IJson>(&schema, &root, br#"{"owner":"id-7","balances":{}}"#, &both)
            .unwrap_err()
            .what(),
        "an id is {id}"
    );
}
