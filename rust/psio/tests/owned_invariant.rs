// An invariant may return a message computed from the value
// (`Result<(), String>`, `Result<(), Cow<'static, str>>`) as well as a
// constant (`Result<(), &'static str>`). Every entry point that enforces the
// invariant passes the message through unchanged.

use psio::json::TextForm;
use psio::pssz::dynamic::{self, CustomHandlers};
use psio::pssz::{Nested, Pssz, PsszError};
use psio::pssz_schema_ir::{AnyType, SchemaBuilder};
use psio::{IJson, Json, JsonText, Key, PsszPack, PsszUnpack, PsszView, ToPsszSchema};
use std::borrow::Cow;

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, JsonText, ToPsszSchema, Key)]
#[psio(definition_will_not_change, invariant = "within_limit")]
struct Balance {
    amount: u32,
    limit: u32,
}

fn within_limit(balance: &Balance) -> Result<(), String> {
    if balance.amount > balance.limit {
        Err(format!("balance {} exceeds limit {}", balance.amount, balance.limit))
    } else {
        Ok(())
    }
}

const OVER: &str = "balance 7 exceeds limit 5";

fn over() -> Balance {
    Balance { amount: 7, limit: 5 }
}
fn over_pssz() -> Vec<u8> {
    let _unchecked = psio::pssz_checked::WriteUnchecked::enter();
    psio::try_to_pssz(&over()).unwrap()
}

#[test]
fn pssz_writers_and_readers_report_the_computed_message() {
    assert_eq!(psio::try_to_pssz(&over()), Err(PsszError::from(OVER.to_string())));
    let mut out = vec![1, 2, 3];
    assert_eq!(psio::try_to_pssz_into(&over(), &mut out).unwrap_err().message(), OVER);
    assert_eq!(out, [1, 2, 3]);

    let bytes = over_pssz();
    assert_eq!(psio::from_pssz::<Balance>(&bytes).unwrap_err().message(), OVER);
    assert_eq!(psio::validate_pssz::<Balance>(&bytes).unwrap_err().message(), OVER);
    assert_eq!(psio::pssz::decode_checked::<Pssz, Balance>(&bytes).unwrap_err().message(), OVER);
    assert_eq!(PsszView::<Balance>::new(&bytes).err().unwrap().message(), OVER);
    assert_eq!(PsszView::<Balance>::new_strict(&bytes).err().unwrap().message(), OVER);
    assert_eq!(PsszView::<Balance>::new_checked(&bytes).err().unwrap().message(), OVER);
    let mut writable = bytes.clone();
    assert_eq!(psio::PsszMutView::<Balance>::new(&mut writable).err().unwrap().message(), OVER);

    // Inside other values: the innermost failure is the one reported.
    let list = vec![Balance { amount: 1, limit: 2 }, Balance { amount: 9, limit: 3 }];
    assert_eq!(psio::try_to_pssz(&list).unwrap_err().message(), "balance 9 exceeds limit 3");
    assert_eq!(psio::try_to_pssz(&Some(over())).unwrap_err().message(), OVER);
    assert_eq!(Nested::<Balance>::new(&over()).unwrap_err().message(), OVER);
    let adopted = Nested::<Balance>::unchecked(bytes.clone());
    assert_eq!(adopted.validate().unwrap_err().message(), OVER);
    let frame = psio::try_to_pssz(&adopted).unwrap();
    assert_eq!(psio::from_pssz::<Nested<Balance>>(&frame).unwrap_err().message(), OVER);

    // A value that satisfies the invariant is unaffected.
    let fine = Balance { amount: 5, limit: 7 };
    let bytes = psio::to_pssz(&fine);
    assert_eq!(psio::from_pssz::<Balance>(&bytes), Ok(fine.clone()));
    let mut bytes = bytes;
    let mut view = psio::PsszMutView::<Balance>::new(&mut bytes).unwrap();
    assert_eq!(view.set(&over()).unwrap_err().message(), OVER);
    assert_eq!(psio::from_pssz::<Balance>(&bytes), Ok(fine));
}

#[test]
fn the_message_is_owned_and_a_constant_one_is_borrowed() {
    let PsszError(message) = psio::try_to_pssz(&over()).unwrap_err();
    assert!(matches!(message, Cow::Owned(_)));
    let PsszError(message) = psio::from_pssz::<u32>(&[1]).unwrap_err();
    assert!(matches!(message, Cow::Borrowed(_)));
    const CONSTANT: PsszError = PsszError::new("a constant message");
    assert!(matches!(CONSTANT.0, Cow::Borrowed("a constant message")));
}

#[test]
fn key_writers_and_readers_report_the_computed_message() {
    assert_eq!(psio::try_to_key(&over()).unwrap_err().message(), OVER);
    let mut out = vec![9];
    assert_eq!(psio::try_to_key_into(&over(), &mut out).unwrap_err().message(), OVER);
    assert_eq!(out, [9]);
    let key = psio::to_key(&(7u32, 5u32));
    assert_eq!(psio::from_key::<Balance>(&key).unwrap_err().message(), OVER);
    assert_eq!(psio::validate_key::<Balance>(&key).unwrap_err().message(), OVER);
    assert_eq!(
        psio::key::try_key_prefix::<(Balance, u8), _>(&(&over(),)).unwrap_err().message(),
        OVER
    );
    assert!(matches!(psio::try_to_key(&over()).unwrap_err().0, Cow::Owned(_)));
}

#[test]
fn json_writers_and_readers_report_the_computed_message() {
    let error = psio::try_to_json(&over()).unwrap_err();
    assert_eq!((&*error.what, error.format), (OVER, "json"));
    assert!(matches!(error.what, Cow::Owned(_)));
    let error = psio::try_to_ijson(&over()).unwrap_err();
    assert_eq!((&*error.what, error.format), (OVER, "ijson"));
    let mut out = String::from("x");
    assert_eq!(psio::try_to_json_into(&over(), &mut out).unwrap_err().what, OVER);
    assert_eq!(out, "x");

    let text = r#"{"amount":7,"limit":5}"#;
    assert_eq!(psio::from_json::<Balance>(text).unwrap_err().what, OVER);
    assert_eq!(psio::from_ijson::<Balance>(text).unwrap_err().what, OVER);
    assert_eq!(psio::validate_json::<Balance>(text).unwrap_err().what, OVER);
    assert_eq!(psio::validate_ijson::<Balance>(text).unwrap_err().what, OVER);
    // The offset is where the value starts.
    let error = psio::from_json::<Vec<Balance>>(r#"[{"amount":1,"limit":1},{"amount":8,"limit":2}]"#)
        .unwrap_err();
    assert_eq!((&*error.what, error.offset), ("balance 8 exceeds limit 2", 24));
}

// A custom text form with an invariant: `json_text_form!(T, invariant = path)`.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, Key)]
#[psio(transparent, custom, invariant = "even")]
struct Even(u32);

fn even(value: &Even) -> Result<(), Cow<'static, str>> {
    match value.0 {
        0 => Err(Cow::Borrowed("Even: zero is not allowed")),
        n if n % 2 == 1 => Err(Cow::Owned(format!("Even: {n} is odd"))),
        _ => Ok(()),
    }
}
impl TextForm for Even {
    fn to_text(&self, out: &mut String) {
        out.push_str(&format!("#{}", self.0));
    }
    fn from_text(text: &str) -> Option<Self> {
        text.strip_prefix('#')?.parse().ok().map(Even)
    }
}
psio::json_text_form!(Even, invariant = even);

#[test]
fn a_text_form_with_an_invariant_reports_the_computed_message() {
    assert_eq!(psio::to_json(&Even(4)), r##""#4""##);
    assert_eq!(psio::try_to_json(&Even(3)).unwrap_err().what, "Even: 3 is odd");
    assert_eq!(psio::try_to_ijson(&Even(3)).unwrap_err().what, "Even: 3 is odd");
    assert_eq!(psio::from_json::<Even>(r##""#9""##).unwrap_err().what, "Even: 9 is odd");
    assert_eq!(psio::from_ijson::<Even>(r##""#9""##).unwrap_err().what, "Even: 9 is odd");
    let constant = psio::from_json::<Even>(r##""#0""##).unwrap_err();
    assert_eq!(constant.what, "Even: zero is not allowed");
    assert!(matches!(constant.what, Cow::Borrowed(_)));
    assert_eq!(psio::try_to_pssz(&Even(3)).unwrap_err().message(), "Even: 3 is odd");
    assert_eq!(psio::try_to_key(&Even(3)).unwrap_err().message(), "Even: 3 is odd");
}

// A constant message compiles as before.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Key)]
#[psio(transparent, invariant = "nonzero")]
struct Id(u32);

fn nonzero(id: &Id) -> Result<(), &'static str> {
    if id.0 == 0 { Err("Id: zero") } else { Ok(()) }
}

#[test]
fn a_constant_message_allocates_nothing() {
    assert!(matches!(psio::try_to_pssz(&Id(0)).unwrap_err().0, Cow::Borrowed("Id: zero")));
    assert!(matches!(psio::try_to_key(&Id(0)).unwrap_err().0, Cow::Borrowed("Id: zero")));
    assert!(matches!(psio::try_to_json(&Id(0)).unwrap_err().what, Cow::Borrowed("Id: zero")));
    assert!(matches!(psio::from_json::<Id>("0").unwrap_err().what, Cow::Borrowed("Id: zero")));
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
struct Account {
    owner: String,
    balance: Balance,
    step: Even,
}

#[test]
fn schema_driven_handlers_report_the_computed_message() {
    let mut builder = SchemaBuilder::default();
    let root: AnyType = builder.insert::<Account>().unwrap();
    let schema = builder.finish();

    let mut handlers = CustomHandlers::new();
    handlers.add_invariant_type::<Balance>().unwrap();
    handlers.add_type::<Even>().unwrap();

    let bad_balance = {
        let _unchecked = psio::pssz_checked::WriteUnchecked::enter();
        psio::try_to_pssz(&Account { owner: "a".into(), balance: over(), step: Even(2) }).unwrap()
    };
    let bad_step = {
        let _unchecked = psio::pssz_checked::WriteUnchecked::enter();
        psio::try_to_pssz(&Account {
            owner: "a".into(),
            balance: Balance { amount: 1, limit: 2 },
            step: Even(11),
        })
        .unwrap()
    };

    // Without a registry the nodes are their wire types.
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bad_balance), Ok(false));

    // Binary entry points.
    let error = dynamic::validate_pssz_with(&schema, &root, &bad_balance, &handlers).unwrap_err();
    assert_eq!(error.what(), OVER);
    let error = dynamic::validate_pssz_with(&schema, &root, &bad_step, &handlers).unwrap_err();
    assert_eq!(error.what(), "Even: 11 is odd");
    let options = dynamic::Options::handlers(&handlers);
    let error = dynamic::decode_exact_with::<Pssz>(&schema, &root, &bad_balance, options).unwrap_err();
    assert_eq!(error.message(), OVER);
    let value = dynamic::decode_exact::<Pssz>(&schema, &root, &bad_balance).unwrap();
    let error = dynamic::encode_with::<Pssz>(&schema, &root, &value, options).unwrap_err();
    assert_eq!(error.message(), OVER);
    assert!(matches!(error.0, Cow::Owned(_)));

    // Key entry points.
    let error = dynamic::to_key_with(&schema, &root, &value, options).unwrap_err();
    assert_eq!(error.message(), OVER);
    let key = dynamic::to_key(&schema, &root, &value).unwrap();
    let error = dynamic::validate_key_with(&schema, &root, &key, options).unwrap_err();
    assert_eq!(error.message(), OVER);
    let error = dynamic::from_key_with(&schema, &root, &key, options).unwrap_err();
    assert_eq!(error.message(), OVER);

    // Text entry points, both formats.
    let error = dynamic::pssz_to_json_with::<Json>(&schema, &root, &bad_balance, &handlers).unwrap_err();
    assert_eq!(error.what(), OVER);
    let error = dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bad_step, &handlers).unwrap_err();
    assert_eq!(error.what(), "Even: 11 is odd");
    let text = r##"{"owner":"a","balance":{"amount":7,"limit":5},"step":"#2"}"##;
    let error = dynamic::json_to_pssz_with::<Json>(&schema, &root, text.as_bytes(), &handlers).unwrap_err();
    assert_eq!(error.what(), OVER);
    let error = dynamic::json_to_pssz_with::<IJson>(&schema, &root, text.as_bytes(), &handlers).unwrap_err();
    assert_eq!(error.what(), OVER);
    let text = r##"{"owner":"a","balance":{"amount":1,"limit":5},"step":"#13"}"##;
    let error = dynamic::json_to_pssz_with::<Json>(&schema, &root, text.as_bytes(), &handlers).unwrap_err();
    assert_eq!(error.what(), "Even: 13 is odd");
    // The typed reader agrees.
    assert_eq!(psio::from_json::<Account>(text).unwrap_err().what, "Even: 13 is odd");

    // A handler written by hand returns an owned message the same way.
    let mut by_hand = CustomHandlers::new();
    by_hand.add_check("Balance", |bytes: &[u8]| {
        let amount = u32::from_le_bytes(bytes[0..4].try_into().unwrap());
        let limit = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
        if amount > limit {
            Err(PsszError::from(format!("by hand: {amount} > {limit}")))
        } else {
            Ok(())
        }
    });
    let error = dynamic::validate_pssz_with(&schema, &root, &bad_balance, &by_hand).unwrap_err();
    assert_eq!(error.what(), "by hand: 7 > 5");
}
