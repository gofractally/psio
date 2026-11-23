//! The typed encoder against a recording of itself.
//!
//! A deterministic stream of values over every kind of type the encoder
//! knows (records that are extensible, fixed-layout, capped, annotated,
//! recursive; lists, arrays, maps, sets, tuples, optionals, variants, bit
//! types, hand-written codecs that misbehave) is encoded under every
//! profile. Each result — the bytes, or the error message — goes into a
//! transcript, and the transcript's hash is compared with the one recorded
//! from the encoder as it was before it wrote in place (commit c6d9940,
//! where every node was encoded into a buffer of its own and copied).
//!
//! So: any change to a byte, to an error, or to which of two errors is
//! reported first, fails here. To see what changed, set
//! `PSSZ_ENCODE_TRANSCRIPT=<file>` on both sides and diff the files.

use psio::pssz::{
    encode_into, try_encode, Bitlist, Bitvector, BoundedList, BoundedString, Pssz, Pssz16, Pssz32, Pssz8,
    PsszError, PsszFormat, PsszPack, PsszResult, Uint256,
};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

// ── Hand-written codecs ─────────────────────────────────────────────────────

/// Fixed-size by declaration (2 bytes); writes `0` bytes of them.
#[derive(Debug, Clone, PartialEq)]
struct Sloppy(u8);
impl<F: PsszFormat> PsszPack<F> for Sloppy {
    const MIN_ENCODED_SIZE: usize = 2;
    const MAX_ENCODED_SIZE: Option<usize> = Some(2);
    const IS_FIXED_SIZE: bool = true;
    const FIXED_SIZE: usize = 2;
    fn pssz_size(&self) -> usize {
        2
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        // 0..=3 bytes: only 2 is what the type declares.
        for i in 0..self.0 % 4 {
            out.push(0xA0 + i);
        }
    }
}

/// Variable-size; refuses some values with its own message, after it wrote.
#[derive(Debug, Clone, PartialEq)]
struct Picky(u8);
impl<F: PsszFormat> PsszPack<F> for Picky {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = false;
    const FIXED_SIZE: usize = 0;
    fn pssz_size(&self) -> usize {
        (self.0 % 5) as usize
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.extend(std::iter::repeat(self.0).take((self.0 % 5) as usize));
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        if self.0 % 7 == 3 {
            return Err(PsszError::new("picky: refused"));
        }
        <Self as PsszPack<F>>::pssz_pack(self, out);
        Ok(())
    }
}

/// A hand-written node that holds a derived value: the depth of what it
/// holds continues from its own.
#[derive(Debug, Clone, PartialEq)]
struct Holder(Box<Chain>);
impl<F: PsszFormat> PsszPack<F> for Holder {
    const MIN_ENCODED_SIZE: usize = 1;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = false;
    const FIXED_SIZE: usize = 0;
    fn pssz_size(&self) -> usize {
        <Chain as PsszPack<F>>::pssz_size(&self.0)
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Chain as PsszPack<F>>::pssz_pack(&self.0, out)
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        let _node = psio::pssz_checked::EncodeDepth::enter()?;
        <Chain as PsszPack<F>>::pssz_pack_checked(&self.0, out)
    }
}

// ── Derived shapes ──────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct Plain {
    a: u32,
    s: String,
    v: Vec<u16>,
    o: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[psio(definition_will_not_change)]
struct Fixed {
    a: u8,
    b: u16,
    c: u32,
    d: u64,
    e: bool,
    f: [u16; 3],
    g: i128,
    h: f64,
    i: Uint256,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[psio(definition_will_not_change)]
struct FixedOuter {
    x: Fixed,
    y: [Fixed; 2],
    z: i8,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[psio(definition_will_not_change)]
struct FrozenVariable {
    id: u32,
    s: String,
    t: Vec<String>,
    last: u8,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[pssz(max_dynamic_data = 255)]
struct Capped {
    tags: Vec<String>,
    inner: Vec<Plain>,
    note: Option<String>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[pssz(max_dynamic_data = 40000)]
struct CappedWide {
    tags: Vec<String>,
    pairs: Vec<(u8, String)>,
    capped: Vec<Capped>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct Annotated {
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    #[psio(max = 4)]
    name: String,
    #[psio(max = 3)]
    words: Vec<u32>,
    #[psio(max = 6)]
    note: Option<String>,
    #[psio(max = 3, keys(max = 4), values(max = 2))]
    map: BTreeMap<String, Vec<u8>>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct Trailing {
    a: u8,
    b: Option<u8>,
    c: Option<String>,
    d: Option<Vec<u8>>,
    e: Option<Option<u16>>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
enum Choice {
    Unit,
    One(String),
    Two(u32, String),
    Named { a: u8, b: Option<String> },
    Record(Plain),
    Fixed(u64),
}

#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack)]
#[repr(u8)]
enum Color {
    Red = 1,
    Green = 2,
    #[psio(other)]
    Unknown(u8),
}

fn even(value: &Even) -> Result<(), &'static str> {
    if value.n % 2 == 0 {
        Ok(())
    } else {
        Err("even: the number is odd")
    }
}
#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[psio(definition_will_not_change, invariant = "even")]
struct Even {
    n: u16,
}

fn short(value: &Short) -> Result<(), String> {
    if value.text.len() < 6 {
        Ok(())
    } else {
        Err(format!("short: {} bytes", value.text.len()))
    }
}
#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[psio(invariant = "short")]
struct Short {
    text: String,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
#[psio(transparent)]
struct Wrapped(Vec<String>);

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct HandWritten {
    before: String,
    sloppy: Sloppy,
    picky: Picky,
    list: Vec<Sloppy>,
    array: [Sloppy; 2],
    maybe: Option<Picky>,
    pickies: Vec<Picky>,
    even: Even,
    evens: Vec<Even>,
    short: Short,
    color: Color,
    colors: Vec<Color>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct Everything {
    plain: Plain,
    list: Vec<Plain>,
    fixed: FixedOuter,
    fixed_list: Vec<Fixed>,
    frozen: FrozenVariable,
    trailing: Trailing,
    tuple: (u8, String, Option<u16>),
    unit: (),
    map: BTreeMap<u32, String>,
    set: BTreeSet<u16>,
    bounded: BoundedList<String, 3>,
    text: BoundedString<5>,
    choice: Choice,
    choices: Vec<Choice>,
    wrapped: Wrapped,
    bits: Bitvector<9>,
    bitlist: Bitlist<12>,
    floats: Vec<f32>,
    bytes: Vec<u8>,
    signed: [i64; 2],
    grid: Vec<Vec<u32>>,
    annotated: Annotated,
    capped: Capped,
    next: Option<Box<Everything>>,
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct Chain {
    value: u8,
    next: Option<Box<Chain>>,
    held: Option<Holder>,
}

/// Lists of numbers at a chosen depth: record `k` is node `2k`, its lists
/// are nodes `2k + 2` and their elements `2k + 3`.
#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct DeepNumbers {
    numbers: Option<Vec<u16>>,
    flags: Option<Vec<bool>>,
    next: Option<Box<DeepNumbers>>,
}
fn deep_numbers(records: usize, numbers: Option<Vec<u16>>, flags: Option<Vec<bool>>) -> DeepNumbers {
    let mut deep = DeepNumbers { numbers, flags, next: None };
    for _ in 1..records {
        deep = DeepNumbers { numbers: None, flags: None, next: Some(Box::new(deep)) };
    }
    deep
}

// ── Values ──────────────────────────────────────────────────────────────────

struct Random(u64);
impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    fn chance(&mut self, one_in: u64) -> bool {
        self.below(one_in) == 0
    }
    /// Mostly short; now and then long enough to cross a narrow width, a
    /// bound or a cap.
    fn text(&mut self) -> String {
        let len = match self.below(40) {
            0 => 250 + self.below(20),
            1 => 60 + self.below(10),
            2..=9 => 0,
            _ => self.below(18),
        };
        (0..len).map(|_| (b'a' + self.below(26) as u8) as char).collect()
    }
    fn list<T>(&mut self, max: u64, mut item: impl FnMut(&mut Self) -> T) -> Vec<T> {
        let count = if self.chance(30) { max * 3 } else { self.below(max + 1) };
        (0..count).map(|_| item(self)).collect()
    }
    fn maybe<T>(&mut self, item: impl FnOnce(&mut Self) -> T) -> Option<T> {
        if self.chance(2) {
            Some(item(self))
        } else {
            None
        }
    }
    fn plain(&mut self) -> Plain {
        Plain {
            a: self.next() as u32,
            s: self.text(),
            v: self.list(5, |r| r.next() as u16),
            o: self.maybe(|r| r.next() as u32),
        }
    }
    fn fixed(&mut self) -> Fixed {
        Fixed {
            a: self.next() as u8,
            b: self.next() as u16,
            c: self.next() as u32,
            d: self.next(),
            e: self.chance(2),
            f: [self.next() as u16, self.next() as u16, self.next() as u16],
            g: ((self.next() as u128) << 64 | self.next() as u128) as i128,
            h: self.next() as f64 / 7.0,
            i: Uint256 { limb: [self.next(), self.next(), self.next(), self.next()] },
        }
    }
    fn capped(&mut self) -> Capped {
        Capped { tags: self.list(3, Self::text), inner: self.list(2, Self::plain), note: self.maybe(Self::text) }
    }
    fn annotated(&mut self) -> Annotated {
        Annotated {
            tags: self.list(4, Self::text),
            name: {
                let mut name = self.text();
                if !self.chance(6) {
                    name.truncate(4);
                }
                name
            },
            words: self.list(2, |r| r.next() as u32),
            note: self.maybe(|r| {
                let mut note = r.text();
                if !r.chance(6) {
                    note.truncate(6);
                }
                note
            }),
            map: self
                .list(2, |r| {
                    let mut key = r.text();
                    key.truncate(4 + r.below(2) as usize);
                    (key, r.list(2, |r| r.next() as u8))
                })
                .into_iter()
                .collect(),
        }
    }
    fn trailing(&mut self) -> Trailing {
        Trailing {
            a: self.next() as u8,
            b: self.maybe(|r| r.next() as u8),
            c: self.maybe(Self::text),
            d: self.maybe(|r| r.list(3, |r| r.next() as u8)),
            e: self.maybe(|r| r.maybe(|r| r.next() as u16)),
        }
    }
    fn choice(&mut self) -> Choice {
        match self.below(6) {
            0 => Choice::Unit,
            1 => Choice::One(self.text()),
            2 => Choice::Two(self.next() as u32, self.text()),
            3 => Choice::Named { a: self.next() as u8, b: self.maybe(Self::text) },
            4 => Choice::Record(self.plain()),
            _ => Choice::Fixed(self.next()),
        }
    }
    fn color(&mut self) -> Color {
        match self.below(12) {
            0..=4 => Color::Red,
            5..=8 => Color::Green,
            // 1 and 2 are declared: the catch-all may not hold them.
            _ => Color::Unknown(self.below(5) as u8),
        }
    }
    fn hand_written(&mut self) -> HandWritten {
        // Mostly well-behaved, so that the later members are reached.
        let sloppy = |r: &mut Self| Sloppy(if r.chance(12) { r.next() as u8 } else { 2 });
        let picky = |r: &mut Self| Picky(if r.chance(12) { 3 } else { r.below(3) as u8 });
        let even = |r: &mut Self| Even { n: (r.next() as u16) & if r.chance(12) { 0xffff } else { 0xfffe } };
        HandWritten {
            before: self.text(),
            sloppy: sloppy(self),
            picky: picky(self),
            list: self.list(3, sloppy),
            array: [sloppy(self), sloppy(self)],
            maybe: self.maybe(picky),
            pickies: self.list(3, picky),
            even: even(self),
            evens: self.list(3, even),
            short: Short { text: { let mut t = self.text(); if !self.chance(8) { t.truncate(5); } t } },
            color: self.color(),
            colors: self.list(3, Self::color),
        }
    }
    fn everything(&mut self, depth: u32) -> Everything {
        let mut bits = Bitvector::<9>::new();
        for i in 0..9 {
            bits.set(i, self.chance(2));
        }
        let mut bitlist = Bitlist::<12>::new();
        let most = if self.chance(10) { 20 } else { 12 };
        for _ in 0..self.below(most) {
            bitlist.push(self.chance(2));
        }
        Everything {
            plain: self.plain(),
            list: self.list(3, Self::plain),
            fixed: FixedOuter { x: self.fixed(), y: [self.fixed(), self.fixed()], z: self.next() as i8 },
            fixed_list: self.list(3, Self::fixed),
            frozen: FrozenVariable {
                id: self.next() as u32,
                s: self.text(),
                t: self.list(3, Self::text),
                last: self.next() as u8,
            },
            trailing: self.trailing(),
            tuple: (self.next() as u8, self.text(), self.maybe(|r| r.next() as u16)),
            unit: (),
            map: self.list(3, |r| (r.next() as u32, r.text())).into_iter().collect(),
            set: self.list(4, |r| r.next() as u16).into_iter().collect(),
            bounded: {
                let most = if self.chance(8) { 5 } else { 3 };
                BoundedList(self.list(most, Self::text))
            },
            text: BoundedString({
                let mut text = self.text();
                if !self.chance(8) {
                    text.truncate(5);
                }
                text
            }),
            choice: self.choice(),
            choices: self.list(3, Self::choice),
            wrapped: Wrapped(self.list(3, Self::text)),
            bits,
            bitlist,
            floats: self.list(4, |r| r.next() as f32 / 3.0),
            bytes: self.list(9, |r| r.next() as u8),
            signed: [self.next() as i64, self.next() as i64],
            grid: self.list(3, |r| r.list(3, |r| r.next() as u32)),
            annotated: self.annotated(),
            capped: self.capped(),
            next: if depth > 0 && self.chance(3) { Some(Box::new(self.everything(depth - 1))) } else { None },
        }
    }
}

/// `links` records deep; a hand-written node every `hold` links (0: never).
fn chain(links: usize, hold: usize) -> Chain {
    let mut chain = Chain { value: 0, next: None, held: None };
    for link in 1..=links {
        chain = if hold != 0 && link % hold == 0 {
            Chain { value: link as u8, next: None, held: Some(Holder(Box::new(chain))) }
        } else {
            Chain { value: link as u8, next: Some(Box::new(chain)), held: None }
        };
    }
    chain
}

// ── The transcript ──────────────────────────────────────────────────────────

struct Transcript {
    text: String,
    entries: usize,
}
impl Transcript {
    fn one<F: PsszFormat, T: PsszPack<F>>(&mut self, profile: &str, name: &str, value: &T) {
        let result = try_encode::<F, T>(value);
        // The append is a transaction: what was there stays, and on error
        // nothing is added. Both entry points agree with `try_encode`.
        let mut appended = vec![9u8, 8, 7];
        let into = encode_into::<F, T>(value, &mut appended);
        let mut packed = vec![9u8, 8, 7];
        let checked = value.pssz_pack_checked(&mut packed);
        assert_eq!(&appended[..3], [9, 8, 7]);
        assert_eq!(&packed[..3], [9, 8, 7]);
        match &result {
            Ok(bytes) => {
                assert_eq!(into, Ok(()), "{name} {profile}");
                assert_eq!(&appended[3..], &bytes[..], "{name} {profile}");
                // A record's cap is the top level's to check under an
                // explicit profile: `pssz_pack_checked` alone may accept.
                assert_eq!(checked, Ok(()), "{name} {profile}");
                assert_eq!(&packed[3..], &bytes[..], "{name} {profile}");
            }
            Err(error) => {
                assert_eq!(into.as_ref(), Err(error), "{name} {profile}");
                assert_eq!(appended.len(), 3, "{name} {profile}");
                if checked.is_err() {
                    assert_eq!(packed.len(), 3, "{name} {profile}");
                }
            }
        }
        write!(self.text, "{name} {profile} ").unwrap();
        match result {
            Ok(bytes) => {
                for byte in bytes {
                    write!(self.text, "{byte:02x}").unwrap();
                }
            }
            Err(error) => write!(self.text, "error: {error}").unwrap(),
        }
        match checked {
            Ok(()) => self.text.push_str(" | ok\n"),
            Err(error) => writeln!(self.text, " | error: {error}").unwrap(),
        }
        self.entries += 1;
    }
    fn all<T>(&mut self, name: &str, value: &T)
    where
        T: PsszPack<Pssz> + PsszPack<Pssz8> + PsszPack<Pssz16> + PsszPack<Pssz32>,
    {
        self.one::<Pssz, T>("auto", name, value);
        self.one::<Pssz8, T>("8", name, value);
        self.one::<Pssz16, T>("16", name, value);
        self.one::<Pssz32, T>("32", name, value);
    }
}

fn fnv1a(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |hash, byte| (hash ^ *byte as u64).wrapping_mul(0x0000_0100_0000_01b3))
}

fn transcript() -> Transcript {
    let mut t = Transcript { text: String::new(), entries: 0 };
    let mut r = Random(0x9E37_79B9_7F4A_7C15);
    for _ in 0..400 {
        t.all("plain", &r.plain());
        t.all("fixed", &r.fixed());
        t.all("trailing", &r.trailing());
        t.all("choice", &r.choice());
        t.all("capped", &r.capped());
        t.all("annotated", &r.annotated());
        t.all("hand-written", &r.hand_written());
        t.all("strings", &r.list(12, Random::text));
        t.all("plains", &r.list(4, Random::plain));
        t.all("fixeds", &r.list(4, Random::fixed));
        t.all("maybe", &r.maybe(Random::text));
        t.all("pair", &(r.text(), r.next() as u16, r.maybe(Random::text)));
        t.all("words", &r.list(40, |r| r.next() as u16));
        t.all("array", &[r.text(), r.text(), r.text()]);
        t.all(
            "capped-wide",
            &CappedWide {
                tags: r.list(6, Random::text),
                pairs: r.list(4, |r| (r.next() as u8, r.text())),
                capped: r.list(2, Random::capped),
            },
        );
    }
    for _ in 0..150 {
        t.all("everything", &r.everything(2));
    }
    // The depth limit: records only, and with hand-written nodes between.
    for links in 28..40 {
        for hold in [0, 1, 2, 3, 5] {
            t.all(&format!("chain {links}/{hold}"), &chain(links, hold));
        }
    }
    // The elements of a list are nodes, also where they are written in bulk.
    for records in 29..=34 {
        for numbers in [None, Some(vec![]), Some(vec![1u16, 2, 3])] {
            t.all(&format!("deep numbers {records}"), &deep_numbers(records, numbers, None));
        }
        for flags in [Some(vec![]), Some(vec![true, false])] {
            t.all(&format!("deep flags {records}"), &deep_numbers(records, None, flags));
        }
    }
    for links in [60, 62, 63, 64, 65, 66, 70, 130] {
        t.all(&format!("chain {links}/0"), &chain(links, 0));
        t.all(&format!("chain {links}/7"), &chain(links, 7));
    }
    t
}

/// Entries and FNV-1a of the transcript at commit c6d9940 (before the
/// encoder wrote in place).
const RECORDED: (usize, u64) = (25024, 0xe71a_9ac5_c37a_2732);

#[test]
fn bytes_and_errors_are_those_recorded() {
    let transcript = transcript();
    if let Ok(path) = std::env::var("PSSZ_ENCODE_TRANSCRIPT") {
        std::fs::write(path, &transcript.text).unwrap();
    }
    let errors = transcript.text.matches("error: ").count();
    println!("entries {} errors {} hash {:#018x}", transcript.entries, errors, fnv1a(transcript.text.as_bytes()));
    // The stream has to exercise the refusals as well as the bytes.
    assert!(errors > transcript.entries / 20, "too few errors: {errors}");
    assert_eq!((transcript.entries, fnv1a(transcript.text.as_bytes())), RECORDED);
}

// ── What the recording cannot show ──────────────────────────────────────────

/// Appends nothing and takes away what the caller wrote.
#[derive(Debug, Clone, PartialEq)]
struct Shrinker;
impl<F: PsszFormat> PsszPack<F> for Shrinker {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = false;
    const FIXED_SIZE: usize = 0;
    fn pssz_size(&self) -> usize {
        0
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.clear();
    }
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack)]
struct HoldsShrinker {
    id: u64,
    first: String,
    shrinker: Shrinker,
    after: u32,
    last: String,
}

/// The writers share one output buffer, so a hand-written codec can damage
/// what its caller wrote. That is a bug in the codec and the bytes are then
/// worthless; what the callers owe is not to panic and not to read or write
/// out of bounds.
#[test]
fn a_codec_that_removes_output_is_survived() {
    fn run<F: PsszFormat>()
    where
        HoldsShrinker: PsszPack<F>,
    {
        let record = HoldsShrinker { id: 1, first: "first".into(), shrinker: Shrinker, after: 2, last: "last".into() };
        let _ = try_encode::<F, _>(&record);
        let _ = try_encode::<F, _>(&vec![record.clone(), record.clone()]);
        let _ = try_encode::<F, _>(&Some(record.clone()));
        let _ = try_encode::<F, _>(&(1u8, record.clone(), "tail".to_string()));
        let _ = try_encode::<F, _>(&vec![Shrinker, Shrinker]);
        let _ = try_encode::<F, _>(&Some(Shrinker));
        let _ = try_encode::<F, _>(&[Shrinker, Shrinker]);
        let _ = try_encode::<F, _>(&Shrinker);
        let mut out = vec![1, 2, 3];
        let _ = encode_into::<F, _>(&record, &mut out);
        let mut map = BTreeMap::new();
        map.insert(1u8, record);
        let _ = try_encode::<F, _>(&map);
    }
    run::<Pssz>();
    run::<Pssz8>();
    run::<Pssz16>();
    run::<Pssz32>();
}

/// Says anything about its size; encodes as the `u32` it holds.
#[derive(Debug, Clone, PartialEq)]
struct Hinted(u32, usize);
impl<F: PsszFormat> PsszPack<F> for Hinted {
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = false;
    const FIXED_SIZE: usize = 0;
    fn pssz_size(&self) -> usize {
        4
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.0.to_le_bytes());
    }
    fn pssz_size_hint(&self) -> usize {
        self.1
    }
}

/// The size hint only reserves: a wrong one changes no byte, and one that
/// asks for the address space is not followed.
#[test]
fn the_size_hint_is_not_trusted() {
    for hint in [0, 1, 4, 5, 4096, usize::MAX / 2, usize::MAX] {
        assert_eq!(try_encode::<Pssz, _>(&Hinted(7, hint)).unwrap(), [7, 0, 0, 0]);
        let list = vec![Hinted(1, hint), Hinted(2, hint)];
        assert_eq!(try_encode::<Pssz, _>(&list).unwrap(), [8, 0, 0, 0, 12, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0]);
        let mut out = vec![9];
        encode_into::<Pssz, _>(&Some(Hinted(3, hint)), &mut out).unwrap();
        assert_eq!(out, [9, 1, 3, 0, 0, 0]);
    }
}

/// A list or an array of numbers is written with one copy of its memory
/// (`PsszPack::pssz_bulk_bytes`): the same bytes as number by number.
#[test]
fn numbers_in_bulk_are_their_little_endian_bytes() {
    macro_rules! check {
        ($($ty:ty),*) => {$({
            let values: Vec<$ty> = (0..37u32).map(|i| (i.wrapping_mul(0x9E37_79B9) ^ (i << 7)) as $ty).collect();
            let expected: Vec<u8> = values.iter().flat_map(|v| v.to_le_bytes()).collect();
            fn all<F: PsszFormat>(values: &[$ty], expected: &[u8]) {
                for count in [0, 1, 2, 7, values.len()] {
                    let size = std::mem::size_of::<$ty>();
                    let list = values[..count].to_vec();
                    assert_eq!(try_encode::<F, _>(&list).unwrap(), &expected[..count * size]);
                    assert_eq!(try_encode::<F, [$ty]>(&list[..]).unwrap(), &expected[..count * size]);
                }
                let array: [$ty; 5] = values[..5].try_into().unwrap();
                let size = std::mem::size_of::<$ty>();
                assert_eq!(try_encode::<F, _>(&array).unwrap(), &expected[..5 * size]);
                // In a slot of a fixed region, behind a byte that leaves
                // the array unaligned in the output.
                let framed = try_encode::<F, _>(&(1u8, array, [array, array])).unwrap();
                let header = <(u8, [$ty; 5], [[$ty; 5]; 2]) as PsszPack<F>>::OFFSET_WIDTH;
                assert_eq!(framed[header], 1);
                assert_eq!(&framed[header + 1..header + 1 + 5 * size], &expected[..5 * size]);
                assert_eq!(&framed[header + 1 + 5 * size..header + 1 + 10 * size], &expected[..5 * size]);
                assert_eq!(&framed[header + 1 + 10 * size..], &expected[..5 * size]);
            }
            all::<Pssz>(&values, &expected);
            all::<Pssz8>(&values, &expected);
            all::<Pssz32>(&values, &expected);
        })*};
    }
    check!(u8, i8, u16, i16, u32, i32, u64, i64, u128, i128);
    let floats: Vec<f32> = vec![0.0, -0.0, 1.5, f32::NAN, f32::INFINITY, f32::MIN_POSITIVE, -7.25e-12];
    assert_eq!(
        try_encode::<Pssz, _>(&floats).unwrap(),
        floats.iter().flat_map(|v| v.to_le_bytes()).collect::<Vec<u8>>()
    );
    let doubles: Vec<f64> = vec![0.0, -0.0, 1.5, f64::NAN, f64::NEG_INFINITY, f64::MAX];
    assert_eq!(
        try_encode::<Pssz, _>(&doubles).unwrap(),
        doubles.iter().flat_map(|v| v.to_le_bytes()).collect::<Vec<u8>>()
    );
}
