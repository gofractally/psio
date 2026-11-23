//! `#[psio::interface]`: methods and interfaces (C++ `PSIO_REFLECT(T,
//! method(...))` + `PSIO_INTERFACE(T)`).

use psio::interface::{Caller, DispatchError, Interface, Method, MethodInfo, MethodTable, Receiver};
use psio::pssz::schema_ir::{self, AnyValue, SchemaBuilder};
use psio::pssz::{PsszPack, PsszUnpack};
use psio::{JsonText, ToPsszSchema};
use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
pub struct Asset {
    pub symbol: String,
    pub amount: u64,
}

#[psio::interface(name = "token", derive(Debug, Clone, PartialEq))]
pub trait Token {
    fn transfer(&mut self, to: String, amount: u64, memo: String);
    fn balance_of(&self, owner: String) -> u64;
    fn issue(&mut self, to: String, asset: Asset, note: Option<String>) -> Asset;
    fn total(&self) -> u64;
    fn pair(&self, a: u8, b: u32) -> (u32, u8);
    fn version() -> u32;
    fn reset(&mut self);
}

#[derive(Default)]
struct Ledger {
    balances: BTreeMap<String, u64>,
    memos: Vec<String>,
}

impl Token for Ledger {
    fn transfer(&mut self, to: String, amount: u64, memo: String) {
        *self.balances.entry(to).or_default() += amount;
        self.memos.push(memo);
    }
    fn balance_of(&self, owner: String) -> u64 {
        self.balances.get(&owner).copied().unwrap_or(0)
    }
    fn issue(&mut self, to: String, asset: Asset, note: Option<String>) -> Asset {
        *self.balances.entry(to).or_default() += asset.amount;
        self.memos.extend(note);
        asset
    }
    fn total(&self) -> u64 {
        self.balances.values().sum()
    }
    fn pair(&self, a: u8, b: u32) -> (u32, u8) {
        (b, a)
    }
    fn version() -> u32 {
        3
    }
    fn reset(&mut self) {
        self.balances.clear();
    }
}

#[test]
fn metadata_is_the_declaration() {
    assert_eq!(token::Tag::NAME, "token");
    assert_eq!(token::Tag::PACKAGE, None);
    assert_eq!(token::METHOD_COUNT, 7);
    let names: Vec<_> = token::Tag::METHODS.iter().map(|m| m.name).collect();
    assert_eq!(names, ["transfer", "balance_of", "issue", "total", "pair", "version", "reset"]);
    assert_eq!(
        token::Tag::METHODS[0],
        MethodInfo { name: "transfer", params: &["to", "amount", "memo"], named: true, receiver: Receiver::Mut, has_result: false }
    );
    assert_eq!(
        token::Tag::METHODS[1],
        MethodInfo { name: "balance_of", params: &["owner"], named: true, receiver: Receiver::Ref, has_result: true }
    );
    assert_eq!(
        token::Tag::METHODS[5],
        MethodInfo { name: "version", params: &[], named: true, receiver: Receiver::None, has_result: true }
    );
    for (index, method) in token::Tag::METHODS.iter().enumerate() {
        assert_eq!(token::Tag::index_of_method(method.name), Some(index));
    }
    assert_eq!(token::Tag::index_of_method("burn"), None);

    // The same facts on the argument record.
    assert_eq!(<token::args::issue as Method>::INDEX, 2);
    assert_eq!(token::args::issue::INDEX, 2);
    assert_eq!(<token::args::issue as Method>::NAME, "issue");
    assert_eq!(<token::args::issue as Method>::PARAM_NAMES, ["to", "asset", "note"]);
    assert!(<token::args::issue as Method>::HAS_RESULT);
    assert!(!<token::args::reset as Method>::HAS_RESULT);
    fn result_is<M: Method<Result = R>, R>() {}
    result_is::<token::args::issue, Asset>();
    result_is::<token::args::transfer, ()>();
    result_is::<token::args::pair, (u32, u8)>();
}

/// The rule: the arguments of a call are the tuple of the parameter types.
/// The record has the tuple's bytes and the tuple's offset width, and reads
/// what the tuple writes.
fn same_as_tuple<A, T>(args: A, tuple: T)
where
    A: Method + PartialEq + std::fmt::Debug + Clone + Into<T> + From<T>,
    A: PsszPack<psio::pssz::Pssz16> + PsszPack<psio::pssz::Pssz32>,
    T: PsszPack<psio::Pssz> + PsszUnpack<psio::Pssz> + PartialEq + std::fmt::Debug + Clone,
    T: PsszPack<psio::pssz::Pssz16> + PsszPack<psio::pssz::Pssz32>,
{
    let bytes = psio::to_pssz(&args);
    assert_eq!(bytes, psio::to_pssz(&tuple));
    assert_eq!(psio::pssz::auto_pssz_width::<A>(), psio::pssz::auto_pssz_width::<T>());
    assert_eq!(<A as PsszPack<psio::Pssz>>::AUTO_BOUND, <T as PsszPack<psio::Pssz>>::AUTO_BOUND);
    assert_eq!(<A as PsszPack<psio::Pssz>>::IS_FIXED_SIZE, <T as PsszPack<psio::Pssz>>::IS_FIXED_SIZE);
    assert_eq!(psio::from_pssz::<A>(&bytes).unwrap(), args);
    assert_eq!(psio::from_pssz::<T>(&bytes).unwrap(), tuple);
    // Explicit widths too.
    assert_eq!(
        psio::pssz::encode::<psio::pssz::Pssz16, A>(&args),
        psio::pssz::encode::<psio::pssz::Pssz16, T>(&tuple)
    );
    assert_eq!(
        psio::pssz::encode::<psio::pssz::Pssz32, A>(&args),
        psio::pssz::encode::<psio::pssz::Pssz32, T>(&tuple)
    );
    // And the conversions.
    assert_eq!(A::from(tuple.clone()), args);
    assert_eq!(Into::<T>::into(args), tuple);
}

#[test]
fn the_arguments_are_the_tuple_of_the_parameters() {
    same_as_tuple(
        token::args::transfer { to: "bob".into(), amount: 5, memo: "hi".into() },
        ("bob".to_string(), 5u64, "hi".to_string()),
    );
    same_as_tuple(token::args::balance_of { owner: "alice".into() }, ("alice".to_string(),));
    let asset = Asset { symbol: "TOK".into(), amount: 9 };
    // A trailing optional: absent is not written, present is.
    same_as_tuple(
        token::args::issue { to: "bob".into(), asset: asset.clone(), note: None },
        ("bob".to_string(), asset.clone(), None::<String>),
    );
    same_as_tuple(
        token::args::issue { to: "bob".into(), asset: asset.clone(), note: Some("n".into()) },
        ("bob".to_string(), asset.clone(), Some("n".to_string())),
    );
    // No parameters: the empty tuple.
    same_as_tuple(token::args::total {}, ());
    same_as_tuple(token::args::version {}, ());
    // Fixed members only.
    same_as_tuple(token::args::pair { a: 1, b: 2 }, (1u8, 2u32));

    // The bytes themselves, as C++ writes std::tuple<std::string, uint64_t,
    // std::string>{"alhi"...}: pssz-vectors "method-args/transfer" has the
    // bounded form of this tuple; the unbounded one is four-byte framed.
    assert_eq!(
        psio::to_pssz(&token::args::balance_of { owner: "al".into() }),
        psio::to_pssz(&("al".to_string(),))
    );

    // Old arguments read as the new ones (a trailing optional was added).
    let old = psio::to_pssz(&("bob".to_string(), asset.clone()));
    assert_eq!(
        psio::from_pssz::<token::args::issue>(&old).unwrap(),
        token::args::issue { to: "bob".into(), asset, note: None }
    );
}

#[test]
fn the_argument_record_has_text_and_a_schema() {
    let args = token::args::transfer { to: "bob".into(), amount: 5, memo: "hi".into() };
    assert_eq!(psio::to_json(&args), r#"{"to":"bob","amount":5,"memo":"hi"}"#);
    assert_eq!(psio::from_json::<token::args::transfer>(&psio::to_json(&args)).unwrap(), args);
    // The tuple is C++'s text of the arguments.
    let tuple: <token::args::transfer as Method>::Tuple = args.into();
    assert_eq!(psio::to_json(&tuple), r#"["bob",5,"hi"]"#);

    let (schema, root) = schema_ir::schema_for::<token::args::transfer>().unwrap();
    assert_eq!(root, schema_ir::AnyType::named("token.transfer"));
    let AnyValue::Object(object) = &schema.resolve(&root).unwrap().value else { panic!("an object") };
    let names: Vec<_> = object.members.iter().map(|m| m.name.as_str()).collect();
    assert_eq!(names, ["to", "amount", "memo"]);
}

#[test]
fn dispatch_decodes_calls_and_encodes() {
    let mut ledger = Ledger::default();
    let transfer = psio::to_pssz(&("bob".to_string(), 5u64, "hi".to_string()));
    // No result: no bytes.
    assert_eq!(token::dispatch(&mut ledger, 0, &transfer).unwrap(), Vec::<u8>::new());
    assert_eq!(ledger.memos, ["hi"]);

    let reply = token::dispatch_name(&mut ledger, "balance_of", &psio::to_pssz(&("bob".to_string(),))).unwrap();
    assert_eq!(psio::from_pssz::<u64>(&reply).unwrap(), 5);
    assert_eq!(<token::args::balance_of as Method>::result_from_pssz(&reply).unwrap(), 5);

    let asset = Asset { symbol: "TOK".into(), amount: 9 };
    let reply = token::dispatch(
        &mut ledger,
        token::args::issue::INDEX,
        &psio::to_pssz(&token::args::issue { to: "bob".into(), asset: asset.clone(), note: Some("n".into()) }),
    )
    .unwrap();
    assert_eq!(psio::from_pssz::<Asset>(&reply).unwrap(), asset);
    assert_eq!(ledger.memos, ["hi", "n"]);

    // &self, static and tuple results.
    let reply = token::dispatch_name(&mut ledger, "total", &psio::to_pssz(&())).unwrap();
    assert_eq!(psio::from_pssz::<u64>(&reply).unwrap(), 14);
    let reply = token::dispatch_name(&mut ledger, "version", &psio::to_pssz(&())).unwrap();
    assert_eq!(psio::from_pssz::<u32>(&reply).unwrap(), 3);
    let reply = token::dispatch_name(&mut ledger, "pair", &psio::to_pssz(&(1u8, 2u32))).unwrap();
    assert_eq!(psio::from_pssz::<(u32, u8)>(&reply).unwrap(), (2, 1));

    // Refusals: an unknown method, and arguments that are not the method's.
    assert_eq!(token::dispatch(&mut ledger, 7, &transfer), Err(DispatchError::UnknownMethod));
    assert_eq!(token::dispatch_name(&mut ledger, "burn", &transfer), Err(DispatchError::UnknownMethod));
    for bad in [&b""[..], &[0xff; 3], &transfer[..5]] {
        let before = ledger.memos.len();
        assert!(matches!(token::dispatch(&mut ledger, 0, bad), Err(DispatchError::Arguments(_))), "{bad:?}");
        assert_eq!(ledger.memos.len(), before, "the method did not run");
    }
    // A String argument is UTF-8.
    let raw = psio::to_pssz(&(psio::pssz::RawString(vec![0xff]),));
    assert!(matches!(token::dispatch_name(&mut ledger, "balance_of", &raw), Err(DispatchError::Arguments(_))));

    // A method with no result accepts only no bytes as its result.
    assert!(<token::args::reset as Method>::result_from_pssz(&[]).is_ok());
    assert!(<token::args::reset as Method>::result_from_pssz(&[0]).is_err());
    assert_eq!(<token::args::reset as Method>::result_to_pssz(&()).unwrap(), Vec::<u8>::new());
}

/// A caller that runs the call on a local implementation, through the
/// encoded form: what a remote call does on both ends.
struct Loopback(std::cell::RefCell<Ledger>);
impl Caller for Loopback {
    type Output<R> = Result<R, DispatchError>;
    fn call<M: Method>(&self, args: M) -> Self::Output<M::Result> {
        let reply = token::dispatch(&mut *self.0.borrow_mut(), M::INDEX, &psio::to_pssz(&args))?;
        M::result_from_pssz(&reply).map_err(DispatchError::Result)
    }
}

/// A caller that encodes the call and does not make it.
struct Record;
impl Caller for Record {
    type Output<R> = (usize, &'static str, Vec<u8>);
    fn call<M: Method>(&self, args: M) -> Self::Output<M::Result> {
        (M::INDEX, M::NAME, psio::to_pssz(&args))
    }
}

#[test]
fn a_proxy_hands_each_call_to_its_caller() {
    let proxy = token::Proxy(Loopback(Default::default()));
    proxy.transfer("bob".into(), 5, "hi".into()).unwrap();
    assert_eq!(proxy.balance_of("bob".into()).unwrap(), 5);
    let asset = Asset { symbol: "TOK".into(), amount: 9 };
    assert_eq!(proxy.issue("bob".into(), asset.clone(), None).unwrap(), asset);
    assert_eq!(proxy.total().unwrap(), 14);
    assert_eq!(proxy.version().unwrap(), 3);
    assert_eq!(proxy.pair(1, 2).unwrap(), (2, 1));
    proxy.reset().unwrap();
    assert_eq!(proxy.total().unwrap(), 0);

    let proxy = token::Proxy(Record);
    assert_eq!(
        proxy.transfer("bob".into(), 5, "hi".into()),
        (0, "transfer", psio::to_pssz(&("bob".to_string(), 5u64, "hi".to_string())))
    );
    assert_eq!(proxy.version(), (5, "version", psio::to_pssz(&())));
}

#[test]
fn a_method_table_finds_a_method_by_any_key() {
    let table = MethodTable::of::<token::Tag, _>(|method| (method.name.as_bytes()[0], method.params.len())).unwrap();
    assert_eq!(table.index_of(&(b't', 3)), Some(0));
    assert_eq!(table.index_of(&(b't', 0)), Some(3));
    assert_eq!(table.index_of(&(b'x', 0)), None);
    // Two methods with one key are refused, by name.
    let error = MethodTable::of::<token::Tag, _>(|method| method.params.len()).unwrap_err();
    assert_eq!((error.first, error.second), ("total", "version"));
}

// ── The Schema IR ───────────────────────────────────────────────────────────
//
// The C++ declaration this mirrors (cpp/tests/schema_builder_tests.cpp):
//
//   struct datetime { std::uint64_t seconds = 0; };
//   PSIO_REFLECT(datetime, seconds)
//   struct sb_wall_clock { static datetime now(); static datetime resolution(); };
//   PSIO_PACKAGE(sb_clocks, "0.2.3");
//   PSIO_INTERFACE(sb_wall_clock, types(datetime), funcs(func(now), func(resolution)))

#[allow(non_camel_case_types)]
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
pub struct datetime {
    pub seconds: u64,
}

#[psio::interface(name = "sb_wall_clock", package = "sb_clocks", version = "0.2.3", types(datetime))]
pub trait WallClock {
    fn now() -> datetime;
    fn resolution() -> datetime;
}

#[test]
fn insert_interface_adds_the_interface_its_functions_and_its_types() {
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<wall_clock::Tag>().unwrap();
    let schema = builder.finish();
    assert_eq!(schema.package.name, "sb_clocks");
    assert_eq!(schema.package.version, "0.2.3");
    assert_eq!(schema.interfaces.len(), 1);
    let interface = &schema.interfaces[0];
    assert_eq!(interface.name, "sb_wall_clock");
    assert_eq!(interface.type_names, ["datetime"]);
    assert_eq!(interface.funcs.len(), 2);
    assert_eq!(interface.funcs[0].name, "now");
    assert_eq!(interface.funcs[1].name, "resolution");
    let result = interface.funcs[0].result.as_ref().unwrap();
    assert!(matches!(schema.resolve(result).unwrap().value, AnyValue::Object(_)));

    // The whole schema, as its I-JSON text (docs/json.md, "The Schema IR as
    // JSON"): what C++ psio::SchemaBuilder{}.insert_interface<sb_wall_clock>()
    // writes for the declaration above.
    let text = psio::to_ijson(&schema);
    assert_eq!(
        text,
        concat!(
            r#"{"package":{"name":"sb_clocks","version":"0.2.3","attributes":[]},"#,
            r#""interfaces":[{"name":"sb_wall_clock","type_names":["datetime"],"funcs":["#,
            r#"{"name":"now","params":[],"result":{"value":{"Type":{"type":"datetime"}}},"attributes":[]},"#,
            r#"{"name":"resolution","params":[],"result":{"value":{"Type":{"type":"datetime"}}},"attributes":[]}"#,
            r#"],"attributes":[]}],"worlds":[],"uses":[],"#,
            r#""types":[{"name":"datetime","type":{"value":{"Object":{"members":["#,
            r#"{"name":"seconds","type":{"value":{"Type":{"type":"@1"}}},"attributes":[]}],"attributes":[]}}},"attributes":[]},"#,
            r#"{"name":"@1","type":{"value":{"Int":{"bits":64,"isSigned":false}}},"attributes":[]}],"ir_version":1}"#
        )
    );
    // And it is a value: it round-trips through pSSZ.
    assert_eq!(psio::from_pssz::<schema_ir::Schema>(&psio::to_pssz(&schema)).unwrap(), schema);
}

#[test]
fn the_interface_of_a_trait_with_parameters() {
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<token::Tag>().unwrap().insert_interface::<wall_clock::Tag>().unwrap();
    let schema = builder.finish();
    assert_eq!(schema.interfaces.len(), 2);
    // The token interface names no package; the clock's is reported.
    assert_eq!(schema.package.name, "sb_clocks");
    let token = &schema.interfaces[0];
    assert_eq!(token.name, "token");
    assert!(token.type_names.is_empty());
    assert!(token.attributes.is_empty());
    let names: Vec<_> = token.funcs.iter().map(|f| f.name.as_str()).collect();
    assert_eq!(names, ["transfer", "balance_of", "issue", "total", "pair", "version", "reset"]);

    // transfer(to: string, amount: u64, memo: string), no result.
    let transfer = &token.funcs[0];
    let params: Vec<_> = transfer.params.iter().map(|p| p.name.as_str()).collect();
    assert_eq!(params, ["to", "amount", "memo"]);
    assert!(transfer.result.is_none());
    assert!(transfer.attributes.is_empty());
    assert!(transfer.params.iter().all(|p| p.attributes.is_empty()));
    // The same type is one schema entry.
    assert_eq!(transfer.params[0].r#type, transfer.params[2].r#type);
    let text = schema.wire(&transfer.params[0].r#type).unwrap();
    assert!(matches!(&text.value, AnyValue::Custom(custom) if custom.id == "utf8"), "{text:?}");
    assert!(matches!(
        schema.resolve(&transfer.params[1].r#type).unwrap().value,
        AnyValue::Int(schema_ir::Int { bits: 64, isSigned: false })
    ));

    // issue(to, asset: Asset, note: optional<string>) -> Asset.
    let issue = &token.funcs[2];
    assert_eq!(*issue.params[1].r#type, schema_ir::AnyType::named("Asset"));
    assert_eq!(**issue.result.as_ref().unwrap(), schema_ir::AnyType::named("Asset"));
    assert!(matches!(schema.resolve(&issue.params[2].r#type).unwrap().value, AnyValue::Option(_)));
    // pair(a, b) -> tuple<u32, u8>.
    let pair = &token.funcs[4];
    assert!(matches!(schema.resolve(pair.result.as_ref().unwrap()).unwrap().value, AnyValue::Tuple(_)));
    // version() has no parameters, reset() neither parameters nor a result.
    assert!(token.funcs[5].params.is_empty() && token.funcs[5].result.is_some());
    assert!(token.funcs[6].params.is_empty() && token.funcs[6].result.is_none());

    // The schema is self-contained: every reference resolves.
    for func in &token.funcs {
        for param in &func.params {
            schema.resolve(&param.r#type).unwrap();
        }
    }
}

// ── Visibility and options ──────────────────────────────────────────────────

#[allow(dead_code)]
mod inner {
    #[psio::interface]
    trait Private {
        fn ping(&self, nonce: u32) -> u32;
    }
    #[psio::interface(module = "api")]
    pub(crate) trait CrateWide {
        fn ping(&self, nonce: u32) -> u32;
    }
    #[psio::interface]
    pub(super) trait HTTPServer2 {
        fn r#type(&self, r#in: u32) -> u32;
    }

    pub fn names() -> [&'static str; 3] {
        use psio::interface::Interface;
        [private::Tag::NAME, api::Tag::NAME, http_server2::Tag::NAME]
    }
    pub fn private_args() -> Vec<u8> {
        psio::to_pssz(&private::args::ping { nonce: 1 })
    }
}

#[test]
fn the_module_follows_the_trait() {
    assert_eq!(inner::names(), ["Private", "CrateWide", "HTTPServer2"]);
    assert_eq!(inner::private_args(), psio::to_pssz(&(1u32,)));
    assert_eq!(psio::to_pssz(&inner::api::args::ping { nonce: 1 }), psio::to_pssz(&(1u32,)));
    // Raw identifiers are names without `r#`.
    assert_eq!(inner::http_server2::Tag::METHODS[0].name, "type");
    assert_eq!(inner::http_server2::Tag::METHODS[0].params, ["in"]);
    assert_eq!(psio::to_json(&inner::http_server2::args::r#type { r#in: 7 }), r#"{"in":7}"#);
}

// ── Parameter annotations, the argument cap and the result's bounds ─────────
//
// The C++ declaration (cpp/tests/method_annotation_tests.cpp, docs/pssz-spec
// 16):
//
//   PSIO_REFLECT(token,
//      method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>),
//             maxDynamicData(256)),
//      method(name_of, id, returns(utf8 | max<32>)), ...)

#[psio::interface(name = "bounded", derive(Debug, Clone, PartialEq))]
pub trait Bounded {
    #[pssz(max_dynamic_data = 256)]
    fn transfer(&mut self, #[psio(max = 16)] to: String, amount: u64, #[psio(max = 80)] memo: String);
    /// No cap: the width is the inferred bound's.
    fn transfer_uncapped(&mut self, #[psio(max = 16)] to: String, amount: u64, #[psio(max = 80)] memo: String);
    #[psio(returns(max = 32))]
    fn name_of(&self, id: u32) -> String;
    fn batch(&mut self, #[psio(max = 4, element(max = 8))] names: Vec<psio::pssz::RawString>, fee: Option<u32>);
    #[pssz(max_dynamic_data = 64)]
    fn note(&mut self, text: psio::pssz::RawString);
    #[psio(returns(max = 2, element(max = 3)))]
    fn tags(&self) -> Vec<String>;
}

struct Names(Vec<String>);
impl Bounded for Names {
    fn transfer(&mut self, to: String, _amount: u64, memo: String) {
        self.0.push(format!("{to}:{memo}"));
    }
    fn transfer_uncapped(&mut self, to: String, _amount: u64, _memo: String) {
        self.0.push(to);
    }
    fn name_of(&self, id: u32) -> String {
        "n".repeat(id as usize)
    }
    fn batch(&mut self, names: Vec<psio::pssz::RawString>, _fee: Option<u32>) {
        self.0.push(names.len().to_string());
    }
    fn note(&mut self, text: psio::pssz::RawString) {
        self.0.push(text.0.len().to_string());
    }
    fn tags(&self) -> Vec<String> {
        self.0.clone()
    }
}

/// The schema of the fixture, which C++ wrote from its declarations.
fn fixture_schema() -> schema_ir::Schema {
    let text = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/pssz-vectors.json"))
        .unwrap();
    let fixture: serde_json::Value = serde_json::from_str(&text).unwrap();
    let hex = fixture["schema_ir"].as_str().unwrap();
    let bytes: Vec<u8> =
        (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect();
    psio::from_pssz(&bytes).unwrap()
}

/// A node with every reference replaced by what it refers to, so two
/// schemas that number their anonymous types differently compare.
fn resolved(schema: &schema_ir::Schema, ty: &schema_ir::AnyType) -> schema_ir::AnyType {
    use schema_ir::{AnyType, AnyValue as V};
    let go = |ty: &AnyType| Box::new(resolved(schema, ty));
    let members = |members: &Vec<schema_ir::Member>| {
        members
            .iter()
            .map(|m| schema_ir::Member { name: m.name.clone(), r#type: go(&m.r#type), attributes: m.attributes.clone() })
            .collect()
    };
    AnyType::new(match &ty.value {
        V::Type(reference) => return resolved(schema, schema.get(&reference.r#type).expect("a known type")),
        V::Object(o) => V::Object(schema_ir::Object { members: members(&o.members), attributes: o.attributes.clone() }),
        V::Struct(o) => V::Struct(schema_ir::Struct { members: members(&o.members), attributes: o.attributes.clone() }),
        V::List(l) => V::List(schema_ir::List { r#type: go(&l.r#type) }),
        V::BoundedList(l) => V::BoundedList(schema_ir::BoundedList { r#type: go(&l.r#type), maxCount: l.maxCount }),
        V::Option(o) => V::Option(schema_ir::Optional { r#type: go(&o.r#type) }),
        V::Custom(c) => V::Custom(schema_ir::Custom { r#type: go(&c.r#type), id: c.id.clone() }),
        V::Tuple(t) => V::Tuple(schema_ir::Tuple { members: t.members.iter().map(|m| go(m)).collect() }),
        other => other.clone(),
    })
}

#[test]
fn the_cap_and_the_parameter_bounds_select_the_width_and_are_enforced() {
    use bounded::args;
    use psio::pssz::{auto_pssz_width, BoundedList, BoundedRawString, BoundedString, PsszPack, RawString};
    // conformance/pssz-vectors.json: tuple<string<=16,u64,string<=80>@cap=256
    // has 2-byte offsets (the cap rules), the uncapped tuple 1-byte ones
    // (the inferred bound, 107).
    assert_eq!(auto_pssz_width::<args::transfer>(), 2);
    assert_eq!(<args::transfer as PsszPack<psio::Pssz>>::MAX_DYNAMIC_DATA, Some(256));
    assert_eq!(auto_pssz_width::<args::transfer_uncapped>(), 1);
    assert_eq!(<args::transfer_uncapped as PsszPack<psio::Pssz>>::MAX_ENCODED_SIZE, Some(107));
    assert_eq!(<args::batch as PsszPack<psio::Pssz>>::MAX_ENCODED_SIZE, Some(44));
    assert_eq!(auto_pssz_width::<args::note>(), 1);

    // `method-args/transfer/...` of the fixture: ("al", 5, "hi").
    let uncapped = args::transfer_uncapped { to: "al".into(), amount: 5, memo: "hi".into() };
    assert_eq!(hex(&psio::to_pssz(&uncapped)), "0a0a05000000000000000c616c6869");
    // The uncapped record is the tuple of the bounded parameters.
    let tuple: <args::transfer_uncapped as Method>::Tuple =
        (BoundedString("al".into()), 5u64, BoundedString("hi".into()));
    assert_eq!(psio::to_pssz(&uncapped), psio::to_pssz(&tuple));
    let batch = args::batch { names: vec![RawString(b"a".to_vec())], fee: None };
    let batch_tuple: <args::batch as Method>::Tuple = (BoundedList(vec![BoundedRawString(b"a".to_vec())]), None);
    assert_eq!(psio::to_pssz(&batch), psio::to_pssz(&batch_tuple));
    // Under the cap the same arguments have 2-byte offsets.
    let capped = args::transfer { to: "al".into(), amount: 5, memo: "hi".into() };
    assert_eq!(psio::to_pssz(&capped).len(), psio::to_pssz(&uncapped).len() + 3);

    // A parameter outside its annotation is refused by the encoder and, as
    // a frame, by dispatch.
    let mut target = Names(vec![]);
    let over = args::transfer { to: "x".repeat(17), amount: 0, memo: String::new() };
    assert!(psio::try_to_pssz(&over).is_err());
    let frame = psio::pssz::encode::<psio::pssz::Pssz16, _>(&("x".repeat(17), 0u64, String::new()));
    assert!(matches!(bounded::dispatch_name(&mut target, "transfer", &frame), Err(DispatchError::Arguments(_))));
    let frame = psio::pssz::encode::<psio::pssz::Pssz16, _>(&("al".to_string(), 0u64, "m".repeat(81)));
    assert!(matches!(bounded::dispatch_name(&mut target, "transfer", &frame), Err(DispatchError::Arguments(_))));
    assert!(target.0.is_empty(), "the method did not run");
    bounded::dispatch_name(&mut target, "transfer", &psio::to_pssz(&capped)).unwrap();
    assert_eq!(target.0, ["al:hi"]);
    // The cap bounds the whole argument record.
    assert!(psio::try_to_pssz(&args::note { text: RawString(vec![0; 64]) }).is_err());
    assert!(psio::try_to_pssz(&args::note { text: RawString(vec![0; 60]) }).is_ok());
    assert!(psio::try_to_pssz(&args::batch { names: vec![RawString(vec![]); 5], fee: None }).is_err());
    assert!(psio::try_to_pssz(&args::batch { names: vec![RawString(vec![0; 9])], fee: None }).is_err());

    // returns(...): the result is checked when it is written and when read.
    let reply = bounded::dispatch_name(&mut target, "name_of", &psio::to_pssz(&(32u32,))).unwrap();
    assert_eq!(<args::name_of as Method>::result_from_pssz(&reply).unwrap(), "n".repeat(32));
    assert!(matches!(
        bounded::dispatch_name(&mut target, "name_of", &psio::to_pssz(&(33u32,))),
        Err(DispatchError::Result(_))
    ));
    assert!(<args::name_of as Method>::result_from_pssz(&psio::to_pssz(&"n".repeat(33))).is_err());
    assert!(<args::name_of as Method>::result_to_pssz(&"n".repeat(33)).is_err());
    assert!(<args::tags as Method>::result_to_pssz(&vec!["abc".to_string(); 2]).is_ok());
    assert!(<args::tags as Method>::result_to_pssz(&vec!["abc".to_string(); 3]).is_err());
    assert!(<args::tags as Method>::result_to_pssz(&vec!["abcd".to_string()]).is_err());
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn the_interface_schema_carries_the_bounds_and_the_cap() {
    use schema_ir::{AnyType, AnyValue as V, Attribute};
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<bounded::Tag>().unwrap();
    let schema = builder.finish();
    let funcs = &schema.interfaces[0].funcs;
    let u8_type = AnyType::new(V::Int(schema_ir::Int { bits: 8, isSigned: false }));
    let text = |n: u64| {
        AnyType::new(V::Custom(schema_ir::Custom {
            r#type: Box::new(AnyType::new(V::BoundedList(schema_ir::BoundedList {
                r#type: Box::new(u8_type.clone()),
                maxCount: n,
            }))),
            id: "utf8".into(),
        }))
    };
    let cap = |n: &str| vec![Attribute { name: "maxDynamicData".into(), value: Some(n.into()) }];

    // transfer(to: utf8 <= 16, amount, memo: utf8 <= 80) under a cap of 256:
    // what C++ method_annotation_tests.cpp checks of its interface schema.
    assert_eq!(funcs[0].name, "transfer");
    assert_eq!(resolved(&schema, &funcs[0].params[0].r#type), text(16));
    assert!(matches!(resolved(&schema, &funcs[0].params[1].r#type).value, V::Int(_)));
    assert_eq!(resolved(&schema, &funcs[0].params[2].r#type), text(80));
    assert_eq!(funcs[0].attributes, cap("256"));
    assert!(funcs[1].attributes.is_empty());
    // name_of returns utf8 <= 32.
    assert_eq!(resolved(&schema, funcs[2].result.as_ref().unwrap()), text(32));
    assert!(funcs[2].attributes.is_empty());
    assert_eq!(funcs[4].attributes, cap("64"));

    // The argument record's own schema is the fixture's: C++ describes a
    // method's argument tuple as the Object of its parameters carrying
    // maxDynamicData (conformance/pssz-vectors.md).
    let fixture = fixture_schema();
    let (ours, root) = schema_ir::schema_for::<bounded::args::transfer>().unwrap();
    let theirs = fixture.get("tuple<string<=16,u64,string<=80>@cap=256").expect("the fixture type");
    assert_eq!(resolved(&ours, &root), resolved(&fixture, theirs));
    let (ours, root) = schema_ir::schema_for::<bounded::args::note>().unwrap();
    let theirs = fixture.get("tuple<raw_string>@cap=64").expect("the fixture type");
    assert_eq!(resolved(&ours, &root), resolved(&fixture, theirs));
}
