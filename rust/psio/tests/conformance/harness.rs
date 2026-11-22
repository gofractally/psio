// Shared machinery for ../pssz_conformance.rs: fixture loading, the shape
// grammar, and the per-vector checks.

use psio::pssz::*;
use psio::pssz_view::{PsszView};
use std::collections::BTreeMap;

#[derive(Debug, Clone, serde::Deserialize)]
pub struct TypeInfo {
    pub id: String,
    pub shape: String,
    pub auto_width: usize,
    pub max_encoded_size: Option<u64>,
    pub max_dynamic_data: Option<u64>,
    pub fixed: bool,
    pub fixed_size: usize,
    /// The Schema IR in `Fixture::schema_ir` describes the type (used by
    /// ../schema_conformance.rs).
    #[serde(default)]
    pub ir: bool,
    /// The id of a custom type's Schema IR `Custom` node.
    #[serde(default)]
    pub custom_id: Option<String>,
    /// An invariant constrains values of the type (its own, or that of a
    /// type it holds).
    #[serde(default)]
    pub invariant: bool,
}
#[derive(Debug, Clone, serde::Deserialize)]
pub struct Vector {
    pub name: String,
    pub class: String,
    #[serde(rename = "type")]
    pub ty: String,
    pub format: String,
    pub hex: String,
    pub expect: String,
    #[serde(default)]
    pub reason: String,
    #[serde(default)]
    pub reencoded: String,
    #[serde(default)]
    pub value: String,
    #[serde(default)]
    pub writer: String,
}
#[derive(Debug, serde::Deserialize)]
pub struct Fixture {
    pub version: u32,
    /// psio::pssz encoding of one Schema holding every `ir` type under its
    /// id (lowercase hex); consumed by ../schema_conformance.rs.
    #[serde(default)]
    pub schema_ir: String,
    pub types: Vec<TypeInfo>,
    pub vectors: Vec<Vector>,
}

pub fn fixture() -> Fixture {
    let path = format!(
        "{}/../../conformance/pssz-vectors.json",
        env!("CARGO_MANIFEST_DIR")
    );
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let fixture: Fixture = serde_json::from_str(&text).expect("fixture JSON");
    assert_eq!(fixture.version, 1);
    fixture
}

pub fn unhex(s: &str) -> Vec<u8> {
    assert!(s.len() % 2 == 0, "odd hex");
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex"))
        .collect()
}
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// The fixture's shape grammar (conformance/pssz-vectors.md). `id()` is how a
/// type is referred to from another shape: its name for records and enums,
/// its structural shape otherwise. `shape()` is its own definition.
pub trait Conf {
    fn id() -> String;
    fn shape() -> String {
        Self::id()
    }
}
macro_rules! conf_leaf {
    ($($ty:ty => $name:literal),* $(,)?) => {$(
        impl Conf for $ty { fn id() -> String { $name.into() } }
    )*};
}
conf_leaf!(u8 => "u8", u16 => "u16", u32 => "u32", u64 => "u64", u128 => "u128",
           i8 => "i8", i16 => "i16", i32 => "i32", i64 => "i64", i128 => "i128",
           f32 => "f32", f64 => "f64", bool => "bool", Uint256 => "u256",
           String => "string", RawString => "raw_string");
impl<const N: usize> Conf for BoundedString<N> {
    fn id() -> String { format!("string<={N}") }
}
impl<const N: usize> Conf for BoundedRawString<N> {
    fn id() -> String { format!("raw_string<={N}") }
}
impl<T: Conf> Conf for Vec<T> {
    fn id() -> String { format!("list<{}>", T::id()) }
}
impl<T: Conf, const N: usize> Conf for BoundedList<T, N> {
    fn id() -> String { format!("list<{}><={N}", T::id()) }
}
impl<T: Conf, const N: usize> Conf for [T; N] {
    fn id() -> String { format!("array<{},{N}>", T::id()) }
}
impl<T: Conf> Conf for Option<T> {
    fn id() -> String { format!("optional<{}>", T::id()) }
}
impl<T: Conf> Conf for Box<T> {
    fn id() -> String { format!("box<{}>", T::id()) }
}
// A nested value: `nested<T>`, `nested<T,deferred>`, and as a record member
// with a byte bound `nested<T><=N`.
impl<T: Conf, V: NestedValidation, const N: usize> Conf for Nested<T, V, N> {
    fn id() -> String {
        let policy = if V::DEFERRED { ",deferred" } else { "" };
        let bound = if N == usize::MAX { String::new() } else { format!("<={N}") };
        format!("nested<{}{policy}>{bound}", T::id())
    }
}
// The container holding a map or set is not part of its shape.
impl<K: Conf, V: Conf> Conf for BTreeMap<K, V> {
    fn id() -> String { format!("map<{},{}>", K::id(), V::id()) }
}
impl<K: Conf> Conf for std::collections::BTreeSet<K> {
    fn id() -> String { format!("set<{}>", K::id()) }
}
impl<K: Conf, V: Conf> Conf for std::collections::HashMap<K, V> {
    fn id() -> String { format!("map<{},{}>", K::id(), V::id()) }
}
impl<K: Conf> Conf for std::collections::HashSet<K> {
    fn id() -> String { format!("set<{}>", K::id()) }
}
impl<K: Conf, V: Conf, const N: usize> Conf for BoundedMap<K, V, N> {
    fn id() -> String { format!("map<{},{}><={N}", K::id(), V::id()) }
}
impl<K: Conf, const N: usize> Conf for BoundedSet<K, N> {
    fn id() -> String { format!("set<{}><={N}", K::id()) }
}
// A key type in a container by its key order: the type itself.
impl<T: Conf> Conf for ByKey<T> {
    fn id() -> String { T::id() }
    fn shape() -> String { T::shape() }
}
impl<const N: usize> Conf for Bitvector<N> {
    fn id() -> String { format!("bitvector<{N}>") }
}
impl<const N: usize> Conf for Bitlist<N> {
    fn id() -> String { format!("bitlist<{N}>") }
}
macro_rules! conf_tuple {
    ($($ty:ident),*) => {
        impl<$($ty: Conf),*> Conf for ($($ty,)*) {
            fn id() -> String {
                let parts: Vec<String> = vec![$($ty::id()),*];
                format!("tuple<{}>", parts.join(","))
            }
        }
    };
}
conf_tuple!();
conf_tuple!(A);
conf_tuple!(A, B);
conf_tuple!(A, B, C);
conf_tuple!(A, B, C, D);

// Transparent wrappers, custom types and invariants (../custom/types.rs).
// A transparent type's shape is `transparent{member:T}`; `Endpoint` and
// `Ratio` have the structural ids of the variant and the tuple they are.
macro_rules! conf_custom {
    ($($ty:ty => $id:literal, $shape:literal;)*) => {$(
        impl Conf for $ty {
            fn id() -> String { $id.into() }
            fn shape() -> String { $shape.into() }
        }
    )*};
}
use crate::custom_types::*;
conf_custom! {
    Id => "Id", "transparent{value:u64}";
    Tag => "Tag", "transparent{text:raw_string}";
    Blob => "Blob", "transparent{data:list<u8>}";
    Ref => "Ref", "transparent{target:variant<u64,raw_string>}";
    Name => "Name", "dwnc{value:u64}";
    DerKey => "DerKey", "transparent{data:list<u8>}";
    Range => "Range", "record{lo:u32,hi:u32}";
    Level => "Level", "enum<u8>{low=0,mid=1,high=2,top=3}";
    V4 => "V4", "dwnc{address:array<u8,4>,port:u16}";
    V6 => "V6", "dwnc{address:array<u8,16>,port:u16}";
    Endpoint => "variant<V4,V6>", "variant<V4,V6>";
    Ratio => "tuple<i16,u16>", "tuple<i16,u16>";
    NameOrRange => "variant<Name,Range>", "variant<Name,Range>";
    Guarded => "Guarded", "record{owner:Name,delegate:optional<Name>,members:list<Name>,key:DerKey,window:Range,levels:list<Level>,pair:array<Level,2>,either:variant<Name,Range>,ref:Ref,id:Id,blob:optional<Blob>,endpoint:variant<V4,V6>,ratios:list<tuple<i16,u16>>}";
}

pub trait Codec<F: PsszFormat>: PsszPack<F> + PsszUnpack<F> + PsszValidate<F> {}
impl<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F> + PsszValidate<F>> Codec<F> for T {}
pub trait All:
    Codec<Pssz> + Codec<Pssz8> + Codec<Pssz16> + Codec<Pssz32> + Conf + std::fmt::Debug + 'static
{
}
impl<T> All for T where
    T: Codec<Pssz> + Codec<Pssz8> + Codec<Pssz16> + Codec<Pssz32> + Conf + std::fmt::Debug + 'static
{
}

fn check_format<F: PsszFormat, T: Codec<F> + std::fmt::Debug>(v: &Vector) -> Result<(), String> {
    let bytes = unhex(&v.hex);
    let bytes = &bytes[..];
    let fail = |what: &str, detail: String| Err(format!("{what}: {detail}"));
    // No entry point may panic on any vector.
    let validate = validate::<F, T>(bytes);
    let compatible = validate_compatible::<F, T>(bytes);
    let exact = validate_strict::<F, T>(bytes);
    let decode = decode::<F, T>(bytes);
    let decode_compatible = decode_checked::<F, T>(bytes);
    let decode_exact = decode_strict::<F, T>(bytes);
    let view = PsszView::<T, F>::new(bytes).map(|_| ());
    let view_compatible = PsszView::<T, F>::new_checked(bytes).map(|d| d.has_unknown);
    let view_exact = PsszView::<T, F>::new_strict(bytes).map(|_| ());
    match v.expect.as_str() {
        "canonical" | "unknown" => {
            let unknown = v.expect == "unknown";
            if let Err(e) = &validate { return fail("validate rejected", e.to_string()); }
            if compatible != Ok(unknown) { return fail("validate_compatible", format!("{compatible:?}")); }
            if view.is_err() { return fail("view rejected", format!("{view:?}")); }
            if view_compatible != Ok(unknown) { return fail("view_compatible", format!("{view_compatible:?}")); }
            let value = match decode { Ok(v) => v, Err(e) => return fail("decode rejected", e.to_string()) };
            match decode_compatible {
                Ok(d) if d.has_unknown == unknown => (),
                other => return fail("decode_compatible", format!("{:?}", other.map(|d| d.has_unknown))),
            }
            let reencoded = match try_encode::<F, T>(&value) {
                Ok(b) => b,
                Err(e) => return fail("re-encode failed", e.to_string()),
            };
            let expected = if unknown { unhex(&v.reencoded) } else { bytes.to_vec() };
            if reencoded != expected {
                return fail("re-encode differs", format!("{} != {}", hex(&reencoded), hex(&expected)));
            }
            if unknown {
                if exact.is_ok() || decode_exact.is_ok() || view_exact.is_ok() {
                    return fail("exact reader accepted unknown fields", String::new());
                }
            } else if v.strict_rejects() {
                if exact.is_ok() || decode_exact.is_ok() || view_exact.is_ok() {
                    return fail("exact reader accepted what C++ validate_strict rejects", String::new());
                }
            } else {
                if let Err(e) = &exact { return fail("validate_exact rejected", e.to_string()); }
                if let Err(e) = &decode_exact { return fail("decode_exact rejected", e.to_string()); }
                if let Err(e) = &view_exact { return fail("view_exact rejected", e.to_string()); }
                if let Err(e) = validate_canonical_for_profile::<F, T>(bytes) {
                    return fail("canonical check rejected", e.to_string());
                }
            }
            Ok(())
        }
        "reject" => {
            let outcomes: [(&str, Option<PsszError>); 9] = [
                ("validate", validate.err()),
                ("validate_compatible", compatible.err()),
                ("validate_exact", exact.err()),
                ("decode", decode.err()),
                ("decode_compatible", decode_compatible.err()),
                ("decode_exact", decode_exact.err()),
                ("view", view.err()),
                ("view_compatible", view_compatible.err()),
                ("view_exact", view_exact.err()),
            ];
            let accepted: Vec<&str> =
                outcomes.iter().filter(|(_, e)| e.is_none()).map(|(name, _)| *name).collect();
            if !accepted.is_empty() {
                return fail("accepted by", accepted.join(", "));
            }
            // An invariant's message is the type's own, passed through by
            // every entry point; the order rule of maps and sets has one
            // message in every language.
            if v.reason_is_shared() {
                for (name, error) in &outcomes {
                    if error.map(|e| e.0) != Some(v.reason.as_str()) {
                        return fail(name, format!("{error:?}, want {:?}", v.reason));
                    }
                }
            }
            Ok(())
        }
        other => fail("unknown expectation", other.into()),
    }
}

impl Vector {
    /// Valid frames that C++ `validate_strict` (and the Rust exact readers)
    /// nevertheless reject: raw strings holding non-UTF-8 bytes.
    pub fn strict_rejects(&self) -> bool {
        self.class == "string-raw-non-utf8"
    }
    /// Rejections whose `reason` every language reports word for word.
    pub fn reason_is_shared(&self) -> bool {
        self.class == "invariant" || self.class == "map-set-order"
    }
}

pub fn check<T: All>(v: &Vector) -> Result<(), String> {
    match v.format.as_str() {
        "auto" => check_format::<Pssz, T>(v),
        "pssz8" => check_format::<Pssz8, T>(v),
        "pssz16" => check_format::<Pssz16, T>(v),
        "pssz32" => check_format::<Pssz32, T>(v),
        other => Err(format!("unknown format {other}")),
    }
}

pub fn manifest<T: All>(info: &TypeInfo) -> Result<(), String> {
    let mut problems = vec![];
    if T::shape() != info.shape {
        problems.push(format!("shape {} != {}", T::shape(), info.shape));
    }
    let width = <T as PsszPack<Pssz>>::OFFSET_WIDTH;
    if width != info.auto_width {
        problems.push(format!("auto width {width} != {}", info.auto_width));
    }
    let cap = <T as PsszPack<Pssz>>::MAX_DYNAMIC_DATA.map(|c| c as u64);
    if cap != info.max_dynamic_data {
        problems.push(format!("cap {cap:?} != {:?}", info.max_dynamic_data));
    }
    // C++ reports the inferred bound separately from the cap; the cap rules.
    let bound = <T as PsszPack<Pssz>>::AUTO_BOUND.map(|c| c as u64);
    let expected = info.max_dynamic_data.or(info.max_encoded_size);
    if bound != expected {
        problems.push(format!("bound {bound:?} != {expected:?}"));
    }
    let fixed = <T as PsszPack<Pssz>>::IS_FIXED_SIZE;
    if fixed != info.fixed || (fixed && <T as PsszPack<Pssz>>::FIXED_SIZE != info.fixed_size) {
        problems.push(format!(
            "fixed {fixed}/{} != {}/{}",
            <T as PsszPack<Pssz>>::FIXED_SIZE, info.fixed, info.fixed_size
        ));
    }
    if problems.is_empty() { Ok(()) } else { Err(problems.join("; ")) }
}

fn sweep_format<F: PsszFormat, T: Codec<F> + std::fmt::Debug>(bytes: &[u8]) -> Result<usize, String> {
    // Every truncation and every single-byte corruption of a valid frame:
    // no entry point may panic, validate and decode must agree, and whatever
    // is accepted without unknown fields must be canonical.
    let mut cases: Vec<Vec<u8>> = (0..bytes.len()).map(|n| bytes[..n].to_vec()).collect();
    for i in 0..bytes.len() {
        for flip in [0x01u8, 0x5a, 0x80, 0xff] {
            let mut mutated = bytes.to_vec();
            mutated[i] ^= flip;
            cases.push(mutated);
        }
    }
    let count = cases.len();
    for case in cases {
        let valid = validate_compatible::<F, T>(&case);
        let decoded = decode::<F, T>(&case);
        if valid.is_ok() != decoded.is_ok() {
            return Err(format!("validate {valid:?} but decode ok={} for {}", decoded.is_ok(), hex(&case)));
        }
        if valid.is_ok() != PsszView::<T, F>::new(&case).is_ok() {
            return Err(format!("validated view disagrees for {}", hex(&case)));
        }
        if let (Ok(false), Ok(value)) = (valid, decoded) {
            match try_encode::<F, T>(&value) {
                Ok(again) if again == case => (),
                other => return Err(format!("accepted non-canonical {} -> {other:?}", hex(&case))),
            }
        }
    }
    Ok(count)
}
pub fn sweep<T: All>(v: &Vector) -> Result<usize, String> {
    let bytes = unhex(&v.hex);
    match v.format.as_str() {
        "auto" => sweep_format::<Pssz, T>(&bytes),
        "pssz8" => sweep_format::<Pssz8, T>(&bytes),
        "pssz16" => sweep_format::<Pssz16, T>(&bytes),
        "pssz32" => sweep_format::<Pssz32, T>(&bytes),
        other => Err(format!("unknown format {other}")),
    }
}

pub struct Entry {
    pub sweep: fn(&Vector) -> Result<usize, String>,
    pub check: fn(&Vector) -> Result<(), String>,
    pub manifest: fn(&TypeInfo) -> Result<(), String>,
}
pub type Registry = BTreeMap<String, Entry>;
pub fn register<T: All>(registry: &mut Registry) {
    let previous = registry.insert(T::id(), Entry { sweep: sweep::<T>, check: check::<T>, manifest: manifest::<T> });
    assert!(previous.is_none(), "duplicate type id {}", T::id());
}

/// Encode a native Rust value under the fixture's format name.
pub fn encode<T: All>(format: &str, value: &T) -> PsszResult<Vec<u8>> {
    match format {
        "auto" => try_encode::<Pssz, T>(value),
        "pssz8" => try_encode::<Pssz8, T>(value),
        "pssz16" => try_encode::<Pssz16, T>(value),
        "pssz32" => try_encode::<Pssz32, T>(value),
        _ => Err(PsszError("unknown format")),
    }
}
pub type Native = Box<dyn Fn(&str) -> PsszResult<Vec<u8>>>;
pub fn native<T: All>(value: T) -> (String, Native) {
    (T::id(), Box::new(move |format| encode::<T>(format, &value)))
}
