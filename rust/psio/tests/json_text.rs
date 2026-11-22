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
        r#"[{"value":18446744073709551615,"type":2},"#,
        r#"[[0,{"x":1,"y":-2}],[1,"s\n"#, "\u{e9}", r#""],[2,[1,"in"]]],"#,
        r#"[1,2,255],"text \"q\" \"#, "u0001", r#"",-1.25e-07,[],[null,5]]"#
    ));
    check::<IJson>(concat!(
        r#"[{"value":"18446744073709551615","type":2},"#,
        r#"[{"Point":{"x":1,"y":-2}},{"string":"s\n"#, "\u{e9}", r#""},{"Boxed":{"string":"in"}}],"#,
        r#""0102FF","text \"q\" \"#, "u0001", r#"","#, r#"-1.25e-07,[],[null,5]]"#
    ));
}

#[test]
fn enums_newtypes_and_generics() {
    assert_eq!(json::to_string::<IJson, _>(&Color::Blue).unwrap(), "2");
    assert_eq!(json::from_str::<Json, Color>("1").unwrap(), Color::Green);
    // A Rust enum holds only its enumerators: an undeclared value is an error.
    assert!(json::from_str::<Json, Color>("3").is_err());
    assert!(json::from_str::<Json, Color>("\"Red\"").is_err());
    assert_eq!(json::to_string::<IJson, _>(&Big::Huge).unwrap(), "\"18446744073709551615\"");
    assert_eq!(json::to_string::<Json, _>(&Big::Huge).unwrap(), "18446744073709551615");
    assert_eq!(json::from_str::<IJson, Big>("\"18446744073709551615\"").unwrap(), Big::Huge);
    assert_eq!(json::to_string::<IJson, _>(&Meters(5)).unwrap(), "5");
    assert_eq!(json::from_str::<IJson, Meters>("\"5\"").unwrap(), Meters(5));
    let g = Generic { value: vec![1u8, 2], r#type: Color::Red };
    assert_eq!(json::to_string::<IJson, _>(&g).unwrap(), r#"{"value":"0102","type":0}"#);
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
