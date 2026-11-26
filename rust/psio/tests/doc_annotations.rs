//! The Rust snippets of docs/annotations.md and docs/tutorial-annotations.md.
//! Mirrors cpp/tests/doc_annotations_examples.cpp (same values, same bytes).

use psio::pssz::{auto_pssz_width, pssz_evolves_from, RawString};
use psio::{JsonText, PsszPack, PsszUnpack, ToPsszSchema};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(" ")
}

// ── tut-1 ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
struct AccountPlain {
    id: u64,
    owner: String,
    tags: Vec<String>,
    balance: u64,
}

// ── tut-2 ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
struct Account {
    id: u64,
    #[psio(max = 32)]
    owner: String, // a Rust String is UTF-8: C++ `utf8 | max<32>`
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
}

// ── tut-3 ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
#[pssz(max_dynamic_data = 1024)]
struct AccountV1 {
    id: u64,
    #[psio(max = 32)]
    owner: String,
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
}

// ── tut-4 ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
#[pssz(max_dynamic_data = 1024)]
struct AccountV2 {
    id: u64,
    #[psio(max = 32)]
    owner: String,
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
    #[psio(max = 64)]
    email: Option<String>, // new: optional, last
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct AccountGrown {
    id: u64,
    #[psio(max = 32)]
    owner: String,
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
    // Account has 255 - 187 = 68 bytes of slack; this needs 1 + 1 + 100.
    #[psio(max = 100)]
    bio: Option<String>,
}

const _: () = {
    assert!(auto_pssz_width::<AccountPlain>() == 4);
    assert!(auto_pssz_width::<Account>() == 1);
    assert!(auto_pssz_width::<AccountV1>() == 2);
    assert!(auto_pssz_width::<AccountV2>() == 2);
    assert!(pssz_evolves_from::<AccountV1, AccountV2>());
    assert!(matches!(<Account as psio::pssz::PsszPack<psio::pssz::Pssz>>::AUTO_BOUND, Some(187)));
    assert!(matches!(
        <AccountGrown as psio::pssz::PsszPack<psio::pssz::Pssz>>::AUTO_BOUND,
        Some(293) // (header + 3 slots) x 2 + 8 + 32 + 136 + 8 + (1 + 100)
    ));
    assert!(auto_pssz_width::<AccountGrown>() == 2);
    assert!(!pssz_evolves_from::<Account, AccountGrown>());
};

// ── tut-5 ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
struct Note {
    #[psio(max = 64)]
    blob: RawString, // raw bytes: C++ std::string
    #[psio(max = 64)]
    text: String, // UTF-8: C++ `utf8`
}

// ── tut-6 ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
struct Totals {
    #[json(json_string, ijson_string)]
    supply: u64,
    #[json(ijson_number)]
    holders: u64,
    decimals: u32,
}

#[test]
fn bounded_record() {
    let alice = Account { id: 7, owner: "alice".into(), tags: vec!["vip".into(), "beta".into()], balance: 100 };
    let bytes = psio::to_pssz(&alice);
    // The bytes C++ writes for the same declaration: 1-byte header and
    // slots, and a 1-byte table in the list (02 and 05, the offsets of
    // "vip" and "beta" from the start of the list).
    assert_eq!(
        hex(&bytes),
        "12 07 00 00 00 00 00 00 00 12 17 64 00 00 00 00 00 00 00 \
         61 6c 69 63 65 02 05 76 69 70 62 65 74 61"
    );
    assert_eq!(bytes.len(), 33);
    // The same value as the plain type: 4-byte header, slots and table.
    let plain = AccountPlain { id: 7, owner: "alice".into(), tags: vec!["vip".into(), "beta".into()], balance: 100 };
    assert_eq!(psio::to_pssz(&plain).len(), 48);
    assert_eq!(psio::from_pssz::<Account>(&bytes).unwrap(), alice);

    let long_owner = Account { owner: "x".repeat(33), ..alice.clone() };
    assert!(psio::try_to_pssz(&long_owner).is_err());
    let nine_tags = Account { tags: vec!["t".into(); 9], ..alice.clone() };
    assert!(psio::try_to_pssz(&nine_tags).is_err());
}

#[test]
fn evolution() {
    let v1 = AccountV1 { id: 7, owner: "alice".into(), tags: vec!["vip".into()], balance: 100 };
    let v2_none = AccountV2 { id: 7, owner: "alice".into(), tags: vec!["vip".into()], balance: 100, email: None };
    let v2_some = AccountV2 { email: Some("alice@example.com".into()), ..v2_none.clone() };
    assert_eq!(psio::to_pssz(&v2_none), psio::to_pssz(&v1));
    // 2-byte header and slots (the cap), a 1-byte table in the list (its own bound).
    assert_eq!(psio::to_pssz(&v1).len(), 2 + 8 + 2 + 2 + 8 + 5 + (1 + 3));
    assert_eq!(psio::from_pssz::<AccountV2>(&psio::to_pssz(&v1)).unwrap(), v2_none);
    assert_eq!(psio::from_pssz::<AccountV1>(&psio::to_pssz(&v2_some)).unwrap(), v1);
}

#[test]
fn raw_and_utf8() {
    let binary = Note { blob: RawString(vec![0xff, 0xfe]), text: "café".into() };
    assert_eq!(psio::from_pssz::<Note>(&psio::to_pssz(&binary)).unwrap(), binary);
    assert!(psio::try_to_json(&binary).is_err());
}

#[test]
fn json_forms() {
    let a = AccountV2 { id: 7, owner: "alice".into(), tags: vec!["vip".into()], balance: 9007199254740993, email: None };
    assert_eq!(psio::to_json(&a), r#"{"id":7,"owner":"alice","tags":["vip"],"balance":9007199254740993,"email":null}"#);
    assert_eq!(psio::to_ijson(&a), r#"{"id":7,"owner":"alice","tags":["vip"],"balance":"9007199254740993","email":null}"#);
    let t = Totals { supply: 5, holders: 5, decimals: 4 };
    assert_eq!(psio::to_ijson(&t), r#"{"supply":"5","holders":5,"decimals":4}"#);
}

#[test]
fn keys() {
    assert_eq!(hex(&psio::to_key(&258u64)), "00 00 00 00 00 00 01 02");
    assert_eq!(hex(&psio::to_key(&("bob".to_string(), 7u64))), "62 6f 62 00 00 00 00 00 00 00 00 00 07");
}

// ── tut-9 ──
#[psio::interface]
pub trait Bank {
    fn open(&mut self, #[psio(max = 32)] owner: String, #[psio(max = 8, element(max = 16))] tags: Vec<String>);
    #[pssz(max_dynamic_data = 1024)]
    fn transfer(&mut self, from: u64, to: u64, amount: u64, #[psio(max = 80)] memo: String);
    #[psio(returns(max = 8))]
    fn symbol(&self, token: u32) -> String;
}

#[test]
fn methods() {
    // open has no cap: (header + 2 slots) x 1 byte + owner 32 + tags 136 = 171.
    assert_eq!(<bank::args::open as psio::pssz::PsszPack<psio::pssz::Pssz>>::AUTO_BOUND, Some(171));
    assert_eq!(auto_pssz_width::<bank::args::open>(), 1);
    assert_eq!(auto_pssz_width::<bank::args::transfer>(), 2);
    let args = bank::args::transfer { from: 1, to: 2, amount: 500, memo: "rent".into() };
    assert_eq!(psio::to_pssz(&args).len(), 2 + 8 + 8 + 8 + 2 + 4);
    let long = bank::args::transfer { from: 1, to: 2, amount: 500, memo: "m".repeat(81) };
    assert!(psio::try_to_pssz(&long).is_err());
}

// ── tut-7: a record key ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, psio::Key)]
struct OwnerKey {
    #[psio(max = 32)]
    owner: String,
    id: u64,
}

#[test]
fn record_keys() {
    let key = psio::to_key(&OwnerKey { owner: "bob".into(), id: 7 });
    assert_eq!(key, psio::to_key(&("bob".to_string(), 7u64)));
    assert_eq!(psio::from_key::<OwnerKey>(&key).unwrap(), OwnerKey { owner: "bob".into(), id: 7 });
    // Rust checks member bounds when it WRITES a key too (C++ checks on read only).
    assert!(psio::try_to_key(&OwnerKey { owner: "x".repeat(33), id: 7 }).is_err());
}

// ── tut-8 ──
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText, psio::Key)]
#[psio(transparent)]
struct AccountId(u64);

fn upper_case(s: &Symbol) -> Result<(), &'static str> {
    if s.code.0.iter().all(|c| c.is_ascii_uppercase()) { Ok(()) } else { Err("symbol is not upper case") }
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
#[psio(invariant = "upper_case")]
struct Symbol {
    #[psio(max = 8)]
    code: RawString,
}

#[test]
fn transparent_and_invariant() {
    assert_eq!(psio::to_pssz(&AccountId(42)), psio::to_pssz(&42u64));
    assert_eq!(psio::to_json(&AccountId(42)), "42");
    assert!(psio::try_to_pssz(&Symbol { code: RawString(b"usd".to_vec()) }).is_err());
    let mut bytes = psio::to_pssz(&Symbol { code: RawString(b"USD".to_vec()) });
    *bytes.last_mut().unwrap() = b'd';
    assert!(psio::from_pssz::<Symbol>(&bytes).is_err());
}

// ── ref-enum / ref-records ──
#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText)]
#[repr(u8)]
enum Status {
    Pending = 0,
    Active = 2,
    Closed = 3,
    #[psio(other)]
    Unknown(u8),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
#[psio(definition_will_not_change)]
struct Ticket {
    id: u32,
    status: Status,
}

#[test]
fn enums_and_fixed_records() {
    let t = Ticket { id: 7, status: Status::Active };
    assert_eq!(hex(&psio::to_pssz(&t)), "07 00 00 00 02");
    assert_eq!(psio::from_pssz::<Ticket>(&[7, 0, 0, 0, 9]).unwrap().status, Status::Unknown(9));
}

// ── tut-4c: a published bound may only widen ──
macro_rules! account_revision {
    ($name:ident, cap = $cap:literal, owner = [$($owner:tt)*] $owner_ty:ty, tags = $tags:literal) => {
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
        #[pssz(max_dynamic_data = $cap)]
        struct $name {
            id: u64,
            $($owner)*
            owner: $owner_ty,
            #[psio(max = 8, element(max = $tags))]
            tags: Vec<String>,
            balance: u64,
        }
    };
}
account_revision!(AccountWider, cap = 1024, owner = [#[psio(max = 64)]] String, tags = 16); // was max = 32
account_revision!(AccountTighter, cap = 1024, owner = [#[psio(max = 16)]] String, tags = 16);
account_revision!(AccountWideTags, cap = 1024, owner = [#[psio(max = 32)]] String, tags = 40); // was 16
account_revision!(AccountRawOwner, cap = 1024, owner = [#[psio(max = 32)]] RawString, tags = 16);
account_revision!(AccountCapRaised, cap = 4096, owner = [#[psio(max = 32)]] String, tags = 16); // was 1024

const _: () = {
    // A wider bound is compatible in one direction: the new reader reads
    // every record the old writer produced.
    assert!(pssz_evolves_from::<AccountV1, AccountWider>());
    assert!(!pssz_evolves_from::<AccountWider, AccountV1>()); // never back
    // A tighter bound is refused: the new reader would reject stored records.
    assert!(!pssz_evolves_from::<AccountV1, AccountTighter>());
    // So is a wider bound that moves an offset width: 8 tags of <= 40 bytes
    // are 8 + 320 > 255, and the list's table would go from 1 byte to 2.
    assert!(!pssz_evolves_from::<AccountV1, AccountWideTags>());
    // UTF-8 or raw is what the member is: it cannot be added or dropped.
    assert!(!pssz_evolves_from::<AccountV1, AccountRawOwner>());
    assert!(!pssz_evolves_from::<AccountRawOwner, AccountV1>());
    // The cap may grow inside its width class, and may not shrink.
    assert!(pssz_evolves_from::<AccountV1, AccountCapRaised>());
    assert!(!pssz_evolves_from::<AccountCapRaised, AccountV1>());
    // The inferred bound stays what the fields add up to; the cap is what
    // rules, and the largest encoding has the header and the two slots at
    // the width the cap selects (2 bytes, not 1).
    assert!(matches!(<AccountV1 as psio::pssz::PsszPack<psio::pssz::Pssz>>::INFERRED_BOUND, Some(187)));
    assert!(matches!(<AccountV1 as psio::pssz::PsszPack<psio::pssz::Pssz>>::AUTO_BOUND, Some(1024)));
    assert!(matches!(<AccountV1 as psio::pssz::PsszPack<psio::pssz::Pssz>>::MAX_ENCODED_SIZE, Some(190)));
};

#[test]
fn a_published_bound_may_only_widen() {
    let full = AccountV1 { id: 7, owner: "a".repeat(32), tags: vec!["b".repeat(16)], balance: 1 };
    let bytes = psio::to_pssz(&full);
    assert!(psio::validate_pssz::<AccountWider>(&bytes).is_ok());
    assert!(psio::validate_pssz::<AccountTighter>(&bytes).is_err());
}

// ── tut-8c: a first-class bounded string ──
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, PsszPack, PsszUnpack, JsonText, ToPsszSchema, psio::Key)]
#[psio(transparent)]
pub struct Memo {
    #[psio(max = 80)]
    pub contents: String,
}
// The same type with the bound in the member's type.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
#[psio(transparent, name = "Memo")]
struct MemoInType {
    contents: psio::pssz::BoundedString<80>,
}
// A record that holds one: header 1 + id 8 + slot 1 + memo 80.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
struct Receipt {
    id: u64,
    memo: Memo,
}

#[test]
fn a_transparent_type_with_a_bounded_member() {
    use psio::pssz::{BoundedString, Pssz, PsszPack as Pack};
    assert_eq!(<Memo as Pack<Pssz>>::INFERRED_BOUND, Some(80));
    assert_eq!(<Receipt as Pack<Pssz>>::INFERRED_BOUND, Some(1 + 8 + 1 + 80));
    assert_eq!(<Receipt as Pack<Pssz>>::MAX_ENCODED_SIZE, Some(1 + 8 + 1 + 80));
    assert_eq!(auto_pssz_width::<Receipt>(), 1);
    // Memo has the bytes, the key and the text of the bounded string.
    let memo = Memo { contents: "rent".into() };
    let bounded = BoundedString::<80>("rent".into());
    assert_eq!(psio::to_pssz(&memo), psio::to_pssz(&bounded));
    assert_eq!(psio::to_key(&memo), psio::to_key(&bounded));
    assert_eq!(psio::to_json(&memo), r#""rent""#);
    assert_eq!(psio::from_pssz::<Memo>(&psio::to_pssz(&memo)).unwrap(), memo);
    // Its bound holds wherever a Memo appears.
    let long = Memo { contents: "m".repeat(81) };
    assert!(psio::try_to_pssz(&long).is_err());
    assert!(psio::key::try_to_key(&long).is_err());
    assert!(psio::try_to_pssz(&Receipt { id: 1, memo: long }).is_err());
    assert!(psio::validate_pssz::<Memo>(&[b'm'; 81]).is_err());
    assert!(psio::validate_pssz::<Memo>(&[0xff]).is_err());
    assert!(psio::json::validate::<psio::json::Json, Memo>(format!("\"{}\"", "m".repeat(81))).is_err());
    // Inside a record it costs one 1-byte slot: 1 + 8 + 1 + 4.
    let receipt = Receipt { id: 1, memo };
    assert_eq!(psio::to_pssz(&receipt).len(), 14);
    assert_eq!(psio::to_json(&receipt), r#"{"id":1,"memo":"rent"}"#);
    assert_eq!(psio::from_pssz::<Receipt>(&psio::to_pssz(&receipt)).unwrap(), receipt);
    // The Schema IR is the wrapper-type spelling's: a named alias of the
    // bounded UTF-8 string.
    use psio::pssz::schema_ir::schema_for;
    assert_eq!(schema_for::<Memo>().unwrap(), schema_for::<MemoInType>().unwrap());
    let text = psio::json::try_encode::<psio::json::IJson, _>(&schema_for::<Memo>().unwrap().0).unwrap();
    assert!(text.contains(r#""maxCount":80}}},"id":"utf8""#), "{text}");
    const _: () = assert!(pssz_evolves_from::<Memo, Memo>());
}

// ── tut-9b / tut-9c / tut-9-json ──
#[psio::interface(module = "bank_v2")]
pub trait BankV2 {
    #[pssz(max_dynamic_data = 1024)]
    fn transfer(&mut self, from: u64, to: u64, amount: u64, #[psio(max = 80)] memo: String, fee: Option<u64>);
}
#[psio::interface]
pub trait Payments {
    #[pssz(max_dynamic_data = 1024)]
    fn pay(&mut self, from: u64, to: u64, amount: u64, memo: Memo);
}
const _: () = assert!(pssz_evolves_from::<bank::args::transfer, bank_v2::args::transfer>());

#[test]
fn method_arguments_in_text_and_under_a_cap() {
    use psio::json::{decode, validate, Json};
    use psio::pssz::{Pssz, PsszPack as Pack};
    // The inferred bound is that of the parameters, not the cap: header 1
    // + slot 1 + from 8 + to 8 + amount 8 + memo 80. The largest argument
    // list has the header and the slot 2 bytes wide (the cap's class).
    assert_eq!(<bank::args::transfer as Pack<Pssz>>::INFERRED_BOUND, Some(2 + 8 + 8 + 8 + 80));
    assert_eq!(<bank::args::transfer as Pack<Pssz>>::MAX_ENCODED_SIZE, Some(2 * 2 + 8 + 8 + 8 + 80));
    assert_eq!(<bank::args::transfer as Pack<Pssz>>::MAX_DYNAMIC_DATA, Some(1024));
    assert_eq!(<bank::args::open as Pack<Pssz>>::INFERRED_BOUND, Some(3 + 32 + (8 + 8 * 16)));
    let args = bank::args::transfer { from: 1, to: 2, amount: 500, memo: "rent".into() };
    let bytes = psio::to_pssz(&args);
    // An optional argument appended under the cap: old callers still work.
    let grown = bank_v2::args::transfer { from: 1, to: 2, amount: 500, memo: "rent".into(), fee: None };
    assert_eq!(psio::to_pssz(&grown), bytes);
    assert_eq!(psio::from_pssz::<bank_v2::args::transfer>(&bytes).unwrap().fee, None);
    // The return value is checked against returns(...).
    use psio::interface::Method;
    assert_eq!(<bank::args::symbol as Method>::result_to_pssz(&"USD".to_string()).unwrap(), b"USD");
    assert!(<bank::args::symbol as Method>::try_result_to_pssz(&"X".repeat(9)).is_err());

    // In text an argument list is written as an object keyed by parameter
    // name; binary stays the positional tuple.
    assert_eq!(psio::to_json(&args), r#"{"from":1,"to":2,"amount":500,"memo":"rent"}"#);
    // It is read as that object (members in any order) or as the
    // positional array.
    let read = |text: &str| decode::<Json, bank::args::transfer>(text).map(|args| args.memo);
    assert_eq!(read(r#"{"memo":"rent","amount":500,"to":2,"from":1}"#).unwrap(), "rent");
    assert_eq!(read(r#"[1,2,500,"rent"]"#).unwrap(), "rent");
    // The bounds hold in both forms; a missing required argument is an error.
    assert!(validate::<Json, bank::args::transfer>(r#"{"from":1,"to":2,"amount":500}"#).is_err());
    assert!(validate::<Json, bank::args::transfer>(r#"[1,2,500]"#).is_err());
    assert!(validate::<Json, bank::args::transfer>(format!(r#"[1,2,500,"{}"]"#, "m".repeat(81))).is_err());
    // A missing optional argument is None.
    let text = r#"{"from":1,"to":2,"amount":500,"memo":"rent"}"#;
    assert_eq!(decode::<Json, bank_v2::args::transfer>(text).unwrap().fee, None);

    // A Memo parameter: the same bytes and the same text as Bank::transfer.
    let pay = payments::args::pay { from: 1, to: 2, amount: 500, memo: Memo { contents: "rent".into() } };
    assert_eq!(psio::to_pssz(&pay), bytes);
    assert_eq!(psio::to_json(&pay), psio::to_json(&args));
    let long = payments::args::pay { from: 1, to: 2, amount: 500, memo: Memo { contents: "m".repeat(81) } };
    assert!(psio::try_to_pssz(&long).is_err());
    // The argument list's schema names the type; its node is the bounded
    // string, and the Object says it is a named argument list.
    let (schema, _) = psio::pssz::schema_ir::schema_for::<payments::args::pay>().unwrap();
    let text = psio::json::try_encode::<psio::json::IJson, _>(&schema).unwrap();
    assert!(text.contains(r#""maxCount":80}}},"id":"utf8""#), "{text}");
    assert!(text.contains(r#"{"name":"tuple","value":"named"}"#), "{text}");
}

// ── ref-sorted: order constraints ──
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
struct Ranked {
    #[psio(sorted, max = 4)]
    scores: Vec<u32>, // non-descending
    #[psio(unique)]
    names: Vec<String>, // strictly ascending: no duplicate
    #[psio(unique_keys)]
    entries: Vec<(String, u32)>, // no two with the same .0
}
// The same members without the constraints.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
struct Unranked {
    #[psio(max = 4)]
    scores: Vec<u32>,
    names: Vec<String>,
    entries: Vec<(String, u32)>,
}
// An order constraint is part of the type: it cannot be added or dropped later.
const _: () = {
    assert!(!pssz_evolves_from::<Unranked, Ranked>());
    assert!(!pssz_evolves_from::<Ranked, Unranked>());
};

#[test]
fn order_constraints_are_enforced() {
    let ranked = Ranked {
        scores: vec![1, 1, 2],
        names: vec!["a".into(), "b".into()],
        entries: vec![("x".into(), 1), ("a".into(), 1)],
    };
    let bytes = psio::to_pssz(&ranked);
    // No byte changes: the frame is the unconstrained record's.
    let unranked =
        Unranked { scores: ranked.scores.clone(), names: ranked.names.clone(), entries: ranked.entries.clone() };
    assert_eq!(bytes, psio::to_pssz(&unranked));
    assert_eq!(psio::from_pssz::<Ranked>(&bytes).unwrap(), ranked);
    // A writer refuses a value out of order ...
    assert!(psio::try_to_pssz(&Ranked { scores: vec![2, 1], ..ranked.clone() }).is_err());
    assert!(psio::try_to_pssz(&Ranked { names: vec!["a".into(), "a".into()], ..ranked.clone() }).is_err());
    let twice = vec![("a".to_string(), 1), ("b".to_string(), 2), ("a".to_string(), 3)];
    assert!(psio::try_to_pssz(&Ranked { entries: twice.clone(), ..ranked.clone() }).is_err());
    assert!(psio::json::try_encode::<psio::json::Json, _>(&Ranked { scores: vec![2, 1], ..ranked.clone() }).is_err());
    // ... and every checking reader rejects the frame an unchecked writer
    // produced.
    let frame = psio::to_pssz(&Unranked { scores: vec![2, 1], ..unranked.clone() });
    assert!(psio::validate_pssz::<Ranked>(&frame).is_err());
    assert!(psio::from_pssz::<Ranked>(&frame).is_err());
    let frame = psio::to_pssz(&Unranked { entries: twice, ..unranked });
    assert!(psio::validate_pssz::<Ranked>(&frame).is_err());
}

// ── ref-max-fields: a cap on the number of fields ──
macro_rules! settings {
    ($name:ident $(, max_fields = $max:literal)? { $($field:ident),* }) => {
        #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema)]
        $(#[psio(max_fields = $max)])?
        #[pssz(max_dynamic_data = 64)]
        struct $name { volume: u32, $($field: Option<u32>),* }
    };
}
settings!(Settings, max_fields = 3 { balance });
settings!(SettingsGrown, max_fields = 3 { balance, treble }); // a third field: within the cap
settings!(SettingsRaised, max_fields = 8 { balance, treble }); // a revision may not raise the cap ...
settings!(SettingsDropped { balance, treble }); // ... or drop it
const _: () = {
    assert!(pssz_evolves_from::<Settings, SettingsGrown>());
    assert!(!pssz_evolves_from::<Settings, SettingsRaised>());
    assert!(!pssz_evolves_from::<Settings, SettingsDropped>());
};

#[test]
fn max_fields_is_in_the_schema() {
    let (schema, root) = psio::pssz::schema_ir::schema_for::<Settings>().unwrap();
    let text = psio::json::try_encode::<psio::json::IJson, _>(&schema).unwrap();
    assert!(text.contains(r#"{"name":"maxFields","value":"3"},{"name":"maxDynamicData","value":"64"}"#), "{text}");
    assert_eq!(psio::pssz::dynamic::auto_width(&schema, &root), Ok(1));
    psio::pssz::dynamic::validate_schema(&schema).unwrap();
}

// ── ref-result: a method result has the bytes of its returns(...) type ──
#[psio::interface]
pub trait Directory {
    #[psio(returns(max = 4, element(max = 16)))]
    fn names(&self, limit: u32) -> Vec<String>;
}

#[test]
fn a_method_result_has_the_bytes_of_its_returns_type() {
    use psio::interface::Method;
    use psio::pssz::{BoundedList, BoundedString};
    let two = vec!["alice".to_string(), "bob".to_string()];
    let bytes = <directory::args::names as Method>::result_to_pssz(&two).unwrap();
    // returns(max = 4, element(max = 16)): a 1-byte offset table (02 07) ...
    assert_eq!(hex(&bytes), "02 07 61 6c 69 63 65 62 6f 62");
    // ... where the plain Vec<String> has a 4-byte one.
    assert_eq!(psio::to_pssz(&two).len(), 2 * 4 + 8);
    assert_eq!(<directory::args::names as Method>::result_from_pssz(&bytes).unwrap(), two);
    // A result outside returns(...) has no encoding.
    assert!(<directory::args::names as Method>::try_result_to_pssz(&vec!["a".to_string(); 5]).is_err());
    assert!(<directory::args::names as Method>::try_result_to_pssz(&vec!["x".repeat(17)]).is_err());
    // The same bytes as the wrapper-type spelling of the result.
    let same: BoundedList<BoundedString<16>, 4> =
        BoundedList(vec![BoundedString("alice".into()), BoundedString("bob".into())]);
    assert_eq!(psio::to_pssz(&same), bytes);
    // A string result has no width of its own.
    assert_eq!(hex(&<bank::args::symbol as Method>::result_to_pssz(&"USD".to_string()).unwrap()), "55 53 44");
}

// ── ref-names: names as numbers ──
#[test]
fn names_as_numbers() {
    use psio::compress_name::{method_to_number, number_to_method, number_to_name};
    assert_eq!(number_to_name(6697231), "alice");
    assert_eq!(number_to_method(method_to_number("Transfer")), "transfer");
    // A hash number is shown as '#' and 16 letters, and reads back.
    assert_eq!(number_to_method(0xd55d_f307_0b87_58eb), "#hmlnslheseoypnnp");
    assert_eq!(method_to_number("#hmlnslheseoypnnp"), 0xd55d_f307_0b87_58eb);
}
