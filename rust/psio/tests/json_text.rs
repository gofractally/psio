//! Rust-only behaviour of the Json / IJson text formats; the cross-language
//! cases are in json_conformance.rs.

use psio::json::{self, IJson, Json, JsonFormat};
use psio::pssz::RawString;

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Point {
    x: i32,
    y: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, psio::JsonText)]
#[repr(u8)]
enum Color {
    Red = 0,
    Green = 1,
    Blue = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, psio::JsonText)]
#[repr(u64)]
enum Big {
    Huge = u64::MAX,
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Meters(u64);

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Generic<T> {
    value: T,
    r#type: Color,
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Tree {
    children: Vec<Tree>,
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
enum Shape {
    Point(Point),
    #[json(name = "string")]
    Label(String),
    Boxed(Box<Shape>),
}

type Mixed = (Generic<u64>, Vec<Shape>, Option<Vec<u8>>, RawString, f64, (), Vec<Option<i64>>);

fn mixed() -> Mixed {
    (
        Generic { value: u64::MAX, r#type: Color::Blue },
        vec![
            Shape::Point(Point { x: 1, y: -2 }),
            Shape::Label("s\n\u{e9}".into()),
            Shape::Boxed(Box::new(Shape::Label("in".into()))),
        ],
        Some(vec![1, 2, 255]),
        RawString::from("text \"q\" \u{1}"),
        -1.25e-7,
        (),
        vec![None, Some(5)],
    )
}

#[test]
fn round_trips_and_is_deterministic() {
    fn check<F: JsonFormat>(want: &str) {
        let value = mixed();
        let text = json::to_string::<F, _>(&value).unwrap();
        assert_eq!(text, want);
        assert_eq!(json::to_string::<F, _>(&value).unwrap(), text);
        assert_eq!(json::from_str::<F, Mixed>(&text).unwrap(), value);
    }
    check::<Json>(concat!(
        r#"[{"value":18446744073709551615,"type":"Blue"},"#,
        r#"[[0,{"x":1,"y":-2}],[1,"s\n"#, "\u{e9}", r#""],[2,[1,"in"]]],"#,
        r#"[1,2,255],"text \"q\" \"#, "u0001", r#"",-1.25e-07,[],[null,5]]"#
    ));
    check::<IJson>(concat!(
        r#"[{"value":"18446744073709551615","type":"Blue"},"#,
        r#"[{"Point":{"x":1,"y":-2}},{"string":"s\n"#, "\u{e9}", r#""},{"Boxed":{"string":"in"}}],"#,
        r#""0102FF","text \"q\" \"#, "u0001", r#"","#, r#"-1.25e-07,[],[null,5]]"#
    ));
}

#[test]
fn enums_newtypes_and_generics() {
    // A declared enumerator is written as its name, and read from the
    // name or the number.
    assert_eq!(json::to_string::<IJson, _>(&Color::Blue).unwrap(), "\"Blue\"");
    assert_eq!(json::to_string::<Json, _>(&Color::Blue).unwrap(), "\"Blue\"");
    assert_eq!(json::from_str::<Json, Color>("1").unwrap(), Color::Green);
    assert_eq!(json::from_str::<Json, Color>("\"Red\"").unwrap(), Color::Red);
    // A Rust enum holds only its enumerators: an undeclared value is an error.
    assert!(json::from_str::<Json, Color>("3").is_err());
    assert!(json::from_str::<Json, Color>("\"red\"").is_err());
    assert_eq!(json::to_string::<IJson, _>(&Big::Huge).unwrap(), "\"Huge\"");
    assert_eq!(json::to_string::<Json, _>(&Big::Huge).unwrap(), "\"Huge\"");
    assert_eq!(json::from_str::<IJson, Big>("\"18446744073709551615\"").unwrap(), Big::Huge);
    assert_eq!(json::from_str::<Json, Big>("18446744073709551615").unwrap(), Big::Huge);
    assert_eq!(json::to_string::<IJson, _>(&Meters(5)).unwrap(), "5");
    assert_eq!(json::from_str::<IJson, Meters>("\"5\"").unwrap(), Meters(5));
    let g = Generic { value: vec![1u8, 2], r#type: Color::Red };
    assert_eq!(json::to_string::<IJson, _>(&g).unwrap(), r#"{"value":"0102","type":"Red"}"#);
    assert_eq!(json::from_str::<IJson, Generic<Vec<u8>>>(r#"{"type":0,"value":"0102"}"#).unwrap(), g);
}

#[test]
fn values_without_a_json_text_fail_to_encode() {
    // A raw string is a JSON string when its bytes are UTF-8 ...
    let ok = RawString::from("t\u{e9}xt");
    assert_eq!(json::to_string::<Json, _>(&ok).unwrap(), "\"t\u{e9}xt\"");
    assert_eq!(json::to_string::<IJson, _>(&ok).unwrap(), "\"t\u{e9}xt\"");
    // ... and has no JSON text otherwise: an error, never lossy output.
    for raw in [RawString(vec![0xff]), RawString(vec![b'a', 0xc0, 0x80]), RawString(vec![0xed, 0xa0, 0x80])] {
        let e = json::to_vec::<Json, _>(&raw).unwrap_err();
        assert_eq!(e.what, "json: string is not valid UTF-8");
        assert_eq!(e.format, "json");
        assert_eq!(json::to_vec::<IJson, _>(&raw).unwrap_err().format, "ijson");
        assert!(psio::encode::<IJson, _>(&vec![Some(raw.clone())]).is_err());
    }
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert!(json::to_vec::<IJson, _>(&bad).is_err());
        assert!(json::to_vec::<Json, _>(&bad).is_err());
    }
    assert!(json::to_vec::<IJson, _>(&f32::NAN).is_err());
    // Noncharacters are JSON but not I-JSON.
    let nonchar = String::from("\u{ffff}");
    assert!(json::to_vec::<Json, _>(&nonchar).is_ok());
    assert!(json::to_vec::<IJson, _>(&nonchar).is_err());
}

#[test]
fn depth_bombs_are_rejected_without_deep_recursion() {
    let arrays = vec![b'['; 1_000_000];
    assert!(json::validate::<Json, Tree>(&arrays).is_err());
    assert!(json::validate::<IJson, Vec<Vec<u8>>>(&arrays).is_err());
    let nested = "{\"children\":[".repeat(300_000);
    assert!(json::from_str::<IJson, Tree>(&nested).is_err());
    assert!(json::from_str::<Json, Tree>(&nested).is_err());
    let unknown = format!("{{\"x\":{}", "{\"k\":[".repeat(300_000));
    assert!(json::from_str::<IJson, Point>(&unknown).is_err());
    assert!(json::from_str::<Json, Point>(&unknown).is_err());
    // Exactly the limit: 64 nested containers are accepted, 65 are not.
    type Deep = Vec<Vec<Vec<Vec<u32>>>>;
    assert!(json::from_str::<Json, Deep>("[[[[1]]]]").is_ok());
    let ok = format!("{{\"x\":1,\"y\":2,\"z\":{}{}}}", "[".repeat(63), "]".repeat(63));
    let bad = format!("{{\"x\":1,\"y\":2,\"z\":{}{}}}", "[".repeat(64), "]".repeat(64));
    assert!(json::from_str::<IJson, Point>(&ok).is_ok());
    assert_eq!(json::from_str::<IJson, Point>(&bad).unwrap_err().what, "json: max depth exceeded");
}

#[test]
fn errors_carry_the_offset_and_format() {
    let e = json::from_str::<Json, Point>(r#"{"x":1,"y":true}"#).unwrap_err();
    assert_eq!((e.format, e.offset), ("json", 11));
    // A repeated key is not an error in either format: the last one wins.
    assert_eq!(json::from_str::<IJson, Point>(r#"{"x":1,"x":3,"y":2}"#).unwrap(), Point { x: 3, y: 2 });
    // Each occurrence is decoded as it is seen, so it must be well-typed.
    assert!(json::from_str::<IJson, Point>(r#"{"x":true,"x":3,"y":2}"#).is_err());
    assert!(json::from_str::<Json, Point>(r#"{"x":true,"x":3,"y":2}"#).is_err());
    assert_eq!(json::from_str::<Json, Point>(r#"{"x":1,"x":3,"y":2}"#).unwrap(), Point { x: 3, y: 2 });
    assert_eq!(json::from_str::<Json, Point>("").unwrap_err().what, "json: empty input");
}

#[test]
fn mutated_text_never_panics_and_accepted_text_round_trips() {
    fn run<F: JsonFormat>() {
        let value = mixed();
        let text = json::to_vec::<F, _>(&value).unwrap();
        let alphabet: &[u8] = b"0123456789\"\\[]{},:.-+eEaAfFnulrtx \n\x01\x7f\xc3\xff";
        let mut state = 0x2545F4914F6CDD1Du64;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state as usize
        };
        let mut accepted = 0;
        for _ in 0..60_000 {
            let mut bad = text.clone();
            for _ in 0..1 + next() % 3 {
                let at = next() % bad.len();
                match next() % 4 {
                    0 => bad[at] = alphabet[next() % alphabet.len()],
                    1 => {
                        let end = (at + 1 + next() % 3).min(bad.len());
                        bad.drain(at..end);
                    }
                    2 => bad.insert(at, alphabet[next() % alphabet.len()]),
                    _ => bad.truncate(at),
                }
                if bad.is_empty() {
                    bad.push(b'x');
                }
            }
            let read = json::from_slice::<F, Mixed>(&bad);
            assert_eq!(read.is_ok(), json::validate::<F, Mixed>(&bad).is_ok());
            if let Ok(v) = read {
                accepted += 1;
                let again = json::to_vec::<F, _>(&v).unwrap();
                assert_eq!(json::from_slice::<F, Mixed>(&again).unwrap(), v);
            }
        }
        assert!(accepted > 0);
    }
    run::<Json>();
    run::<IJson>();
}

// ── The syntax layer as a cursor ────────────────────────────────────────────

mod cursor {
    use psio::json::{JsonCursor, JsonKind, TextCursor, MAX_DEPTH};

    #[test]
    fn hands_out_the_events_of_a_document_in_order() {
        let text = br#" { "a" : [ 1 , -2.5e3 , "x\ny" , true , null ] , "b" : { } , "c" : "plain" } "#;
        let mut c = TextCursor::new(text, false).unwrap();
        assert_eq!(c.kind().unwrap(), JsonKind::Object);
        c.begin_object().unwrap();
        assert_eq!(c.next_field().unwrap().as_deref(), Some("a"));
        assert_eq!(c.kind().unwrap(), JsonKind::Array);
        c.begin_array().unwrap();
        assert!(c.next_element().unwrap());
        assert_eq!(c.kind().unwrap(), JsonKind::Number);
        assert_eq!(c.number().unwrap(), "1");
        assert!(c.next_element().unwrap());
        assert_eq!(c.number().unwrap(), "-2.5e3");
        assert!(c.next_element().unwrap());
        assert_eq!(c.kind().unwrap(), JsonKind::String);
        let (text, escaped) = c.string().unwrap();
        assert_eq!((&*text, escaped), ("x\ny", true));
        assert!(c.next_element().unwrap());
        assert_eq!(c.kind().unwrap(), JsonKind::Bool);
        assert!(c.boolean().unwrap());
        assert!(c.next_element().unwrap());
        assert_eq!(c.kind().unwrap(), JsonKind::Null);
        c.null().unwrap();
        assert!(!c.next_element().unwrap());
        assert_eq!(c.next_field().unwrap().as_deref(), Some("b"));
        c.begin_object().unwrap();
        assert_eq!(c.next_field().unwrap(), None);
        assert_eq!(c.next_field().unwrap().as_deref(), Some("c"));
        let (text, escaped) = c.string().unwrap();
        assert_eq!((&*text, escaped), ("plain", false));
        assert_eq!(c.next_field().unwrap(), None);
        c.finish().unwrap();
        // Closed: there is no container to advance in.
        assert!(c.next_element().is_err());
        assert!(c.next_field().is_err());
    }

    #[test]
    fn skips_values_and_reports_syntax_errors() {
        let mut c = TextCursor::new(br#"{"skip":{"x":[1,{"y":"z"}],"w":1e9},"keep":7}"#, true).unwrap();
        c.begin_object().unwrap();
        assert_eq!(c.next_field().unwrap().as_deref(), Some("skip"));
        c.skip().unwrap();
        assert_eq!(c.next_field().unwrap().as_deref(), Some("keep"));
        assert_eq!(c.number().unwrap(), "7");
        assert_eq!(c.next_field().unwrap(), None);
        c.finish().unwrap();

        assert_eq!(TextCursor::new(b"", false).err().unwrap().what, "json: empty input");
        let rejects = |text: &[u8], run: &dyn Fn(&mut TextCursor<'_>) -> bool| {
            let mut c = TextCursor::new(text, true).unwrap();
            assert!(!run(&mut c), "{}", String::from_utf8_lossy(text));
        };
        // A missing comma, a trailing comma, a missing colon.
        rejects(b"[1 2]", &|c| {
            c.begin_array().is_ok()
                && c.next_element().is_ok()
                && c.number().is_ok()
                && c.next_element().is_ok()
        });
        rejects(b"[1,]", &|c| {
            c.begin_array().is_ok()
                && c.next_element().is_ok()
                && c.number().is_ok()
                && c.next_element().is_ok()
                && c.kind().is_ok()
        });
        rejects(br#"{"a" 1}"#, &|c| c.begin_object().is_ok() && c.next_field().is_ok());
        rejects(br#"{"a":1,}"#, &|c| {
            c.begin_object().is_ok()
                && c.next_field().is_ok()
                && c.number().is_ok()
                && c.next_field().is_ok()
        });
        // Wrong event for the text, bad numbers, bad strings, trailing bytes.
        rejects(b"[", &|c| c.begin_object().is_ok());
        rejects(b"x", &|c| c.kind().is_ok());
        rejects(b" ", &|c| c.kind().is_ok());
        rejects(b"\"1\"", &|c| c.number().is_ok());
        rejects(b"01", &|c| c.number().is_ok() && c.finish().is_ok());
        rejects(b"1.", &|c| c.number().is_ok());
        rejects(b"-", &|c| c.number().is_ok());
        rejects(b"tru", &|c| c.boolean().is_ok());
        rejects(b"nul", &|c| c.null().is_ok());
        rejects(b"\"abc", &|c| c.string().is_ok());
        rejects(b"\"\\ud800\"", &|c| c.string().is_ok());
        rejects(b"\"\xff\"", &|c| c.string().is_ok());
        rejects(b"1 1", &|c| c.number().is_ok() && c.finish().is_ok());
        rejects(b"[1", &|c| {
            c.begin_array().is_ok()
                && c.next_element().is_ok()
                && c.number().is_ok()
                && c.next_element().is_ok()
        });
        rejects(b"{\"a\":[1,", &|c| {
            c.begin_object().is_ok() && c.next_field().is_ok() && c.skip().is_ok()
        });
        // The I-JSON string rule: values, keys and skipped values.
        let nonchar = "\"\u{fffe}\"".as_bytes().to_vec();
        rejects(&nonchar, &|c| c.string().is_ok());
        rejects(&nonchar, &|c| c.skip().is_ok());
        let key = "{\"\u{fffe}\":1}".as_bytes().to_vec();
        rejects(&key, &|c| c.begin_object().is_ok() && c.next_field().is_ok());
        assert!(TextCursor::new(&nonchar, false).unwrap().string().is_ok());
        // A type-layer error carries the position.
        let mut c = TextCursor::new(b"[1]", false).unwrap();
        c.begin_array().unwrap();
        let error = c.error("walk: wrong shape");
        assert_eq!((error.what, error.offset, error.format), ("walk: wrong shape", 1, "json"));
    }

    #[test]
    fn nesting_is_limited_for_opened_and_skipped_containers() {
        let open = |depth: usize| {
            let text = "[".repeat(depth);
            let mut c = TextCursor::new(text.as_bytes(), false).unwrap();
            (0..depth).try_for_each(|_| {
                c.begin_array()?;
                c.next_element().map(|_| ())
            })
        };
        assert!(open(MAX_DEPTH - 1).is_ok());
        assert_eq!(open(MAX_DEPTH + 1).unwrap_err().what, "json: max depth exceeded");
        assert!(open(100_000).is_err());

        let nested = |depth: usize| format!("{}{}", "[".repeat(depth), "]".repeat(depth));
        let text = nested(MAX_DEPTH);
        assert!(TextCursor::new(text.as_bytes(), false).unwrap().skip().is_ok());
        let text = nested(MAX_DEPTH + 1);
        assert!(TextCursor::new(text.as_bytes(), false).unwrap().skip().is_err());
        // Skipping counts from the cursor's own depth.
        let text = format!("[{}]", nested(MAX_DEPTH));
        let mut c = TextCursor::new(text.as_bytes(), false).unwrap();
        c.begin_array().unwrap();
        assert!(c.next_element().unwrap());
        assert!(c.skip().is_err());
        let bomb = "{\"a\":".repeat(100_000);
        assert!(TextCursor::new(bomb.as_bytes(), true).unwrap().skip().is_err());
    }
}

// ── Member and variant lookup, in any order ──────────────────────────────────
//
// WideRecord has more members than the derive's expected-order threshold
// (64), OrderProbe fewer: both member-matching paths are exercised.

#[derive(psio::JsonText, Debug, Clone, PartialEq, Default)]
struct WideRecord {
    member_00: u32,
    member_01_x: u32,
    member_02_x_x: u32,
    member_03_x_x_x: u32,
    member_04: u32,
    member_05_x: u32,
    member_06_x_x: u32,
    member_07_x_x_x: u32,
    member_08: u32,
    member_09_x: u32,
    member_10_x_x: u32,
    member_11_x_x_x: u32,
    member_12: u32,
    member_13_x: u32,
    member_14_x_x: u32,
    member_15_x_x_x: u32,
    member_16: u32,
    member_17_x: u32,
    member_18_x_x: u32,
    member_19_x_x_x: u32,
    member_20: u32,
    member_21_x: u32,
    member_22_x_x: u32,
    member_23_x_x_x: u32,
    member_24: u32,
    member_25_x: u32,
    member_26_x_x: u32,
    member_27_x_x_x: u32,
    member_28: u32,
    member_29_x: u32,
    member_30_x_x: u32,
    member_31_x_x_x: u32,
    member_32: u32,
    member_33_x: u32,
    member_34_x_x: u32,
    member_35_x_x_x: u32,
    member_36: u32,
    member_37_x: u32,
    member_38_x_x: u32,
    member_39_x_x_x: u32,
    member_40: u32,
    member_41_x: u32,
    member_42_x_x: u32,
    member_43_x_x_x: u32,
    member_44: u32,
    member_45_x: u32,
    member_46_x_x: u32,
    member_47_x_x_x: u32,
    member_48: u32,
    member_49_x: u32,
    member_50_x_x: u32,
    member_51_x_x_x: u32,
    member_52: u32,
    member_53_x: u32,
    member_54_x_x: u32,
    member_55_x_x_x: u32,
    member_56: u32,
    member_57_x: u32,
    member_58_x_x: u32,
    member_59_x_x_x: u32,
    member_60: u32,
    member_61_x: u32,
    member_62_x_x: u32,
    member_63_x_x_x: u32,
    member_64: u32,
    member_65_x: u32,
    member_66_x_x: u32,
    member_67_x_x_x: u32,
    member_68: u32,
    member_69_x: u32,
    member_70_x_x: u32,
    member_71_x_x_x: u32,
    member_72: u32,
    member_73_x: u32,
    member_74_x_x: u32,
    member_75_x_x_x: u32,
    member_76: u32,
    member_77_x: u32,
    member_78_x_x: u32,
    member_79_x_x_x: u32,
}

#[derive(psio::JsonText, Debug, Clone, PartialEq)]
enum ManyAlternatives {
    Alt0(u32),
    Alt1(u32),
    Alt2(u32),
    Alt3(u32),
    Alt4(u32),
    Alt5(u32),
    Alt6(u32),
    Alt7(u32),
    Alt8(u32),
    #[json(name = "string")]
    Text(String),
}

#[derive(psio::JsonText, Debug, Clone, PartialEq, Default)]
struct OrderProbe {
    a: u32,
    b: Option<u32>,
    c: String,
    d: Option<String>,
    e: Vec<u32>,
    f: u64,
}

fn wide_names() -> Vec<&'static str> {
    vec!["member_00", "member_01_x", "member_02_x_x", "member_03_x_x_x", "member_04", "member_05_x", "member_06_x_x", "member_07_x_x_x", "member_08", "member_09_x", "member_10_x_x", "member_11_x_x_x", "member_12", "member_13_x", "member_14_x_x", "member_15_x_x_x", "member_16", "member_17_x", "member_18_x_x", "member_19_x_x_x", "member_20", "member_21_x", "member_22_x_x", "member_23_x_x_x", "member_24", "member_25_x", "member_26_x_x", "member_27_x_x_x", "member_28", "member_29_x", "member_30_x_x", "member_31_x_x_x", "member_32", "member_33_x", "member_34_x_x", "member_35_x_x_x", "member_36", "member_37_x", "member_38_x_x", "member_39_x_x_x", "member_40", "member_41_x", "member_42_x_x", "member_43_x_x_x", "member_44", "member_45_x", "member_46_x_x", "member_47_x_x_x", "member_48", "member_49_x", "member_50_x_x", "member_51_x_x_x", "member_52", "member_53_x", "member_54_x_x", "member_55_x_x_x", "member_56", "member_57_x", "member_58_x_x", "member_59_x_x_x", "member_60", "member_61_x", "member_62_x_x", "member_63_x_x_x", "member_64", "member_65_x", "member_66_x_x", "member_67_x_x_x", "member_68", "member_69_x", "member_70_x_x", "member_71_x_x_x", "member_72", "member_73_x", "member_74_x_x", "member_75_x_x_x", "member_76", "member_77_x", "member_78_x_x", "member_79_x_x_x"]
}

fn wide_text(order: &[usize]) -> String {
    let names = wide_names();
    let members: Vec<String> =
        order.iter().map(|i| format!("\"{}\":{}", names[*i], (i + 1) * 3)).collect();
    format!("{{{}}}", members.join(","))
}

#[test]
fn wide_record_reads_in_any_member_order() {
    let names = wide_names();
    let declared: Vec<usize> = (0..names.len()).collect();
    let want: WideRecord = json::from_str::<Json, _>(&wide_text(&declared)).unwrap();
    assert_eq!(json::to_string::<Json, _>(&want).unwrap(), wide_text(&declared));
    assert_eq!(want.member_00, 3);
    assert_eq!(want.member_59_x_x_x, 180);

    let mut reversed = declared.clone();
    reversed.reverse();
    assert_eq!(json::from_str::<Json, WideRecord>(&wide_text(&reversed)).unwrap(), want);
    assert_eq!(json::from_str::<IJson, WideRecord>(&wide_text(&reversed)).unwrap(), want);

    // A fixed pseudo-random shuffle, several rounds.
    let mut order = declared.clone();
    let mut state = 0x9E37_79B9_7F4A_7C15u64;
    for _ in 0..25 {
        for i in (1..order.len()).rev() {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            order.swap(i, (state >> 33) as usize % (i + 1));
        }
        assert_eq!(json::from_str::<Json, WideRecord>(&wide_text(&order)).unwrap(), want);
    }

    // A missing member, an unknown key, near-miss names, a repeated key.
    for drop in [0usize, 31, 59] {
        let fewer: Vec<usize> = declared.iter().copied().filter(|i| *i != drop).collect();
        assert!(json::from_str::<Json, WideRecord>(&wide_text(&fewer)).is_err());
    }
    let text = wide_text(&reversed);
    let extra = format!("{{\"member_0\":[1,{{}}],\"member_000\":null,\"\":0,{}", &text[1..]);
    assert_eq!(json::from_str::<Json, WideRecord>(&extra).unwrap(), want);
    let repeated = format!("{},\"member_00\":7}}", &text[..text.len() - 1]);
    assert_eq!(json::from_str::<Json, WideRecord>(&repeated).unwrap().member_00, 7);
    let bad_then_good = format!("{{\"member_00\":\"x\",{}", &wide_text(&declared)[1..]);
    assert!(json::from_str::<Json, WideRecord>(&bad_then_good).is_err());
}

#[test]
fn small_record_reads_in_every_member_order() {
    let want = OrderProbe { a: 1, b: Some(2), c: "x".into(), d: Some("y".into()), e: vec![4, 5], f: 6 };
    let members =
        ["\"a\":1", "\"b\":2", "\"c\":\"x\"", "\"d\":\"y\"", "\"e\":[4,5]", "\"f\":6"];
    fn permute(items: &mut Vec<usize>, k: usize, out: &mut Vec<Vec<usize>>) {
        if k == items.len() {
            out.push(items.clone());
            return;
        }
        for i in k..items.len() {
            items.swap(k, i);
            permute(items, k + 1, out);
            items.swap(k, i);
        }
    }
    let mut orders = Vec::new();
    permute(&mut (0..members.len()).collect(), 0, &mut orders);
    assert_eq!(orders.len(), 720);
    for order in orders {
        let text =
            format!("{{{}}}", order.iter().map(|i| members[*i]).collect::<Vec<_>>().join(","));
        assert_eq!(json::from_str::<Json, OrderProbe>(&text).unwrap(), want, "{text}");
        assert_eq!(json::from_str::<IJson, OrderProbe>(&text).unwrap(), want, "{text}");
    }
    // Optional members left out; repeated keys, the last one wins.
    assert_eq!(
        json::from_str::<Json, OrderProbe>(r#"{"f":6,"e":[],"c":"x","a":1}"#).unwrap(),
        OrderProbe { a: 1, c: "x".into(), f: 6, ..Default::default() }
    );
    assert_eq!(
        json::from_str::<Json, OrderProbe>(r#"{"a":1,"a":2,"c":"x","e":[1],"f":6,"e":[2,3],"c":"y","a":9}"#)
            .unwrap(),
        OrderProbe { a: 9, c: "y".into(), e: vec![2, 3], f: 6, ..Default::default() }
    );
    assert!(json::from_str::<Json, OrderProbe>(r#"{"a":"bad","a":2,"c":"x","e":[],"f":6}"#).is_err());
    assert!(json::from_str::<Json, OrderProbe>(r#"{"c":"x","e":[],"f":6}"#).is_err());
}

#[test]
fn variant_alternatives_resolve_by_name() {
    for (index, name) in ["Alt0", "Alt1", "Alt2", "Alt3", "Alt4", "Alt5", "Alt6", "Alt7", "Alt8"]
        .iter()
        .enumerate()
    {
        let text = format!("{{\"{name}\":{index}}}");
        let value: ManyAlternatives = json::from_str::<IJson, _>(&text).unwrap();
        assert_eq!(json::to_string::<IJson, _>(&value).unwrap(), text);
        let positional = format!("[{index},{index}]");
        assert_eq!(json::from_str::<Json, ManyAlternatives>(&positional).unwrap(), value);
    }
    assert_eq!(
        json::from_str::<IJson, ManyAlternatives>(r#"{"string":"s"}"#).unwrap(),
        ManyAlternatives::Text("s".into())
    );
    for bad in [r#"{"Alt9":1}"#, r#"{"Alt":1}"#, r#"{"Alt00":1}"#, r#"{"":1}"#, r#"{"Text":"s"}"#, "{}"]
    {
        assert!(json::from_str::<IJson, ManyAlternatives>(bad).is_err(), "{bad}");
    }
    assert!(json::from_str::<Json, ManyAlternatives>("[10,1]").is_err());
}
