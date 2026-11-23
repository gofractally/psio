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
