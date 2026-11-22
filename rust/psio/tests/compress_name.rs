//! `psio::compress_name` against C++ (RG2).
//!
//! `fixtures/compress-name-vectors.json` was written by the C++ functions
//! themselves (`fixtures/compress-name-vectors.gen.cpp`, compiled against
//! `cpp/include`): for each name, `method_to_number`, `number_to_method` of
//! that, `name_to_number`, `number_to_name` of that; and for each number,
//! both decoders.

use psio::compress_name::{
    is_hash_name, method_to_number, name_to_number, number_to_method, number_to_name, NameId,
};

fn fixture() -> serde_json::Value {
    serde_json::from_str(include_str!("fixtures/compress-name-vectors.json")).unwrap()
}

#[test]
fn every_name_has_the_number_cpp_gives_it() {
    let fixture = fixture();
    let rows = fixture["names"].as_array().unwrap();
    assert!(rows.len() > 1500);
    let (mut compressed, mut hashed, mut names) = (0, 0, 0);
    for row in rows {
        let text = |i: usize| row[i].as_str().unwrap();
        let number = |i: usize| text(i).parse::<u64>().unwrap();
        let name = text(0);
        let method = method_to_number(name);
        assert_eq!(method, number(1), "method_to_number({name:?})");
        assert_eq!(number_to_method(method), text(2), "number_to_method({method}) of {name:?}");
        let id = name_to_number(name);
        assert_eq!(id, number(3), "name_to_number({name:?})");
        assert_eq!(number_to_name(id), text(4), "number_to_name({id}) of {name:?}");
        if id != 0 {
            names += 1;
        }
        if method != 0 && method != u64::MAX && !name.starts_with('#') {
            if is_hash_name(method) {
                hashed += 1;
            } else {
                compressed += 1;
            }
        }
    }
    // The fixture exercises every path.
    assert!(compressed > 300 && hashed > 50 && names > 300, "{compressed} {hashed} {names}");
}

#[test]
fn every_number_has_the_text_cpp_gives_it() {
    let fixture = fixture();
    let rows = fixture["numbers"].as_array().unwrap();
    assert!(rows.len() > 600);
    for row in rows {
        let number: u64 = row[0].as_str().unwrap().parse().unwrap();
        assert_eq!(number_to_method(number), row[1].as_str().unwrap(), "number_to_method({number})");
        assert_eq!(number_to_name(number), row[2].as_str().unwrap(), "number_to_name({number})");
    }
}

/// The vectors psibase pins for its `MethodNumber`
/// (`rust/psibase/src/method_number.rs`).
#[test]
fn the_method_numbers_psibase_pins() {
    for (name, number) in [
        ("", 0u64),
        ("?", 0),
        ("what?", 0),
        ("?what", 0),
        ("eaorintsl?", 0),
        ("????", 0),
        ("a", 32783),
        ("b", 196),
        ("c", 32884),
        ("abc123", 691485674271),
        ("spiderman", 311625498215),
        ("brucewayne", 56488722015273161),
        ("anthonystark", 50913722085663764),
        ("natasharomanoff", 6905860632893337981),
        ("abcdefghijklmnopqrstuvwxyz", 2393445670689189432),
        ("#hneunophpilcroch", 13346021867974402139),
        ("#niiutpmlecuamehe", 796603392265069093),
        // Case is ignored.
        ("A", 32783),
    ] {
        assert_eq!(method_to_number(name), number, "{name:?}");
    }
    for (number, name) in [
        (0u64, ""),
        (32783, "a"),
        (196, "b"),
        (32884, "c"),
        (311625498215, "spiderman"),
        (50913722085663764, "anthonystark"),
        (6905860632893337981, "#psaoryoiluhlrpyn"),
    ] {
        assert_eq!(number_to_method(number), name, "{number}");
    }
    assert!(is_hash_name(6905860632893337981));
    assert!(!is_hash_name(311625498215));
    // A hash round-trips through its text.
    assert_eq!(method_to_number("#psaoryoiluhlrpyn"), 6905860632893337981);
    // '#' with any other length.
    assert_eq!(method_to_number("#"), u64::MAX);
    assert_eq!(method_to_number("#short"), u64::MAX);
}

#[test]
fn the_encoders_are_const_and_never_panic() {
    const TRANSFER: u64 = method_to_number("transfer");
    const ALICE: u64 = name_to_number("alice");
    const ALICE_ID: NameId = NameId::from_name("alice");
    assert_eq!(number_to_method(TRANSFER), "transfer");
    assert_eq!(number_to_name(ALICE), "alice");
    assert_eq!(ALICE_ID.value, ALICE);

    // Any text and any number: an answer, not a panic.
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for round in 0..20_000 {
        let length = (next() % 40) as usize;
        let bytes: Vec<u8> = (0..length)
            .map(|_| if round % 2 == 0 { b"abcxyz019-_#?AZ \x7f"[(next() % 17) as usize] } else { next() as u8 })
            .collect();
        let text = String::from_utf8_lossy(&bytes);
        let method = method_to_number(&text);
        number_to_method(method);
        number_to_name(name_to_number(&text));
        let number = next();
        number_to_method(number);
        number_to_name(number);
    }
}

/// As in C++, the identifier coder is not one-to-one on every text it
/// accepts: a doubled hyphen has no frequency, so the number decodes to
/// another text. (Pinned from the C++ functions; compare
/// `name_to_number(&number_to_name(n)) == n` to know a number is a name's.)
#[test]
fn a_doubled_hyphen_is_accepted_and_does_not_round_trip() {
    assert_eq!(name_to_number("alice"), 6697231);
    assert_eq!(name_to_number("x--b"), 690109440766);
    assert_eq!(number_to_name(690109440766), "x-wqulitcebeu");
    assert_eq!(name_to_number("a--b"), 690100016917);
    assert_eq!(number_to_name(690100016917), "a-wqweevaner");
}

#[test]
fn name_id_is_the_cpp_record() {
    let id: NameId = "alice".parse().unwrap();
    assert_eq!(id.to_string(), "alice");
    assert_eq!(id, NameId::from_name("alice"));
    assert_eq!("".parse::<NameId>().unwrap(), NameId::default());
    assert!("not a name".parse::<NameId>().is_err());
    assert!("1abc".parse::<NameId>().is_err());
    // definitionWillNotChange record of one u64: eight bytes, no header.
    assert_eq!(psio::to_pssz(&id), id.value.to_le_bytes());
    assert_eq!(psio::from_pssz::<NameId>(&psio::to_pssz(&id)).unwrap(), id);
    assert_eq!(psio::to_key(&id), id.value.to_be_bytes());
    let (schema, root) = psio::pssz::schema_ir::schema_for::<NameId>().unwrap();
    assert_eq!(root, psio::pssz::schema_ir::AnyType::named("name_id"));
    assert!(schema.get("name_id").is_some());
}

/// The cases of the JS suite (`js/src/test/compress-name.test.ts`).
#[test]
fn the_cases_of_the_js_suite() {
    assert_eq!(method_to_number(""), 0);
    assert_eq!(number_to_method(0), "");
    for name in ["hello", "world", "test", "name", "get", "set", "run", "init", "transfer", "balance", "account"] {
        let number = method_to_number(name);
        assert_ne!(number, 0, "{name}");
        assert!(!is_hash_name(number), "{name}");
        assert_eq!(number_to_method(number), name);
    }
    let long = method_to_number("thisisaverylongnamethatwontcompress");
    assert!(is_hash_name(long));
    assert!(number_to_method(long).starts_with('#'));
    for letter in 'a'..='z' {
        let name = letter.to_string();
        let number = method_to_number(&name);
        assert_ne!(number, 0);
        assert_eq!(number_to_method(number), name);
    }
}
