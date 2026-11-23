//! Hex text for bytes, the Rust counterpart of C++ `psio/to_hex.hpp`.
//!
//! These are the routines the `IJson` writer and reader use for a byte
//! sequence, so `to_hex(bytes)` is exactly the text between the quotes of
//! `to_ijson(&bytes)` and `from_hex` accepts exactly what `from_ijson` accepts
//! there.
//!
//! * Writing: two digits per byte, no prefix, no separator. [`to_hex`] writes
//!   UPPER case (`A`–`F`), as C++ `psio::to_hex` and `IJson` do;
//!   [`to_hex_lower`] writes lower case for text whose convention is lower
//!   case (URLs, printed keys).
//! * Reading: upper and lower case digits are both accepted, mixed freely.
//!   Anything else — a `0x` prefix, white space, a separator — is an error,
//!   and so is an odd number of digits. The fixed-destination forms
//!   ([`from_hex_into`], [`from_hex_array`]) also require exactly two digits
//!   per destination byte, like the C++ fixed-destination `from_hex`.
//!
//! ```
//! let text = psio::to_hex([0xDE, 0xAD, 0xBE, 0xEF]);
//! assert_eq!(text, "DEADBEEF");
//! assert_eq!(psio::to_hex_lower([0xDE, 0xAD]), "dead");
//! assert_eq!(psio::from_hex("deadBEEF").unwrap(), [0xDE, 0xAD, 0xBE, 0xEF]);
//!
//! let id: [u8; 2] = psio::from_hex_array("0a0B").unwrap();
//! assert_eq!(id, [10, 11]);
//!
//! let err = psio::from_hex("12x4").unwrap_err();
//! assert_eq!(err.offset, 2);
//! ```

const UPPER: &[u8; 16] = b"0123456789ABCDEF";
const LOWER: &[u8; 16] = b"0123456789abcdef";

/// Why text is not the hex form of bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HexError {
    /// Description, one of the `hex: …` constants of this module.
    pub what: &'static str,
    /// Byte offset in the text: of the offending character for a non-hex
    /// character, otherwise the length of the text.
    pub offset: usize,
}
impl std::fmt::Display for HexError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} (at byte {})", self.what, self.offset)
    }
}
impl std::error::Error for HexError {}

pub(crate) const NON_HEX: &str = "hex: non-hex character";
pub(crate) const ODD_LENGTH: &str = "hex: odd number of digits";
pub(crate) const WRONG_LENGTH: &str = "hex: wrong number of digits for the destination";

/// The value of one hex digit, either case.
pub(crate) fn digit(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

/// The digits of `bytes`, two per byte, handed to `put` in order.
#[inline]
fn encode(bytes: &[u8], table: &[u8; 16], mut put: impl FnMut(u8)) {
    for &b in bytes {
        put(table[(b >> 4) as usize]);
        put(table[(b & 15) as usize]);
    }
}

/// Appends the upper-case digits of `bytes` to UTF-8 output (the JSON writer).
pub(crate) fn encode_upper(bytes: &[u8], out: &mut Vec<u8>) {
    out.reserve(bytes.len() * 2);
    encode(bytes, UPPER, |c| out.push(c));
}

/// How many leading bytes of `src` are hex digits.
pub(crate) fn digit_run(src: &[u8]) -> usize {
    src.iter().take_while(|&&c| digit(c).is_some()).count()
}

/// Decodes `digits` — all hex digits, exactly `2 * out.len()` of them, as
/// [`digit_run`] and a length check establish — into `out`.
pub(crate) fn decode(digits: &[u8], out: &mut [u8]) {
    debug_assert_eq!(digits.len(), out.len() * 2);
    for (byte, pair) in out.iter_mut().zip(digits.chunks_exact(2)) {
        *byte = (digit(pair[0]).unwrap_or(0) << 4) | digit(pair[1]).unwrap_or(0);
    }
}

/// [`decode`] into a new vector; `digits` are all hex and even in number.
pub(crate) fn decode_vec(digits: &[u8]) -> Vec<u8> {
    let mut out = vec![0; digits.len() / 2];
    decode(digits, &mut out);
    out
}

/// The text as hex digits: every character a digit, an even number of them.
fn checked(text: &str) -> Result<&[u8], HexError> {
    let src = text.as_bytes();
    let run = digit_run(src);
    if run != src.len() {
        return Err(HexError { what: NON_HEX, offset: run });
    }
    if run % 2 != 0 {
        return Err(HexError { what: ODD_LENGTH, offset: run });
    }
    Ok(src)
}

/// The upper-case hex text of `bytes`: two digits per byte, no prefix. This
/// is the text C++ `psio::to_hex` produces and `IJson` writes for bytes.
///
/// ```
/// assert_eq!(psio::to_hex(b"\x00\x7f\xff"), "007FFF");
/// assert_eq!(psio::to_hex(Vec::<u8>::new()), "");
/// assert_eq!(psio::to_ijson(&vec![0u8, 0x7f, 0xff]), "\"007FFF\"");
/// ```
pub fn to_hex(bytes: impl AsRef<[u8]>) -> String {
    let mut out = String::new();
    to_hex_into(bytes, &mut out);
    out
}

/// Appends the upper-case hex text of `bytes` to `out`.
///
/// ```
/// let mut url = String::from("/block/");
/// psio::to_hex_into([0xAB, 0x01], &mut url);
/// assert_eq!(url, "/block/AB01");
/// ```
pub fn to_hex_into(bytes: impl AsRef<[u8]>, out: &mut String) {
    let bytes = bytes.as_ref();
    out.reserve(bytes.len() * 2);
    encode(bytes, UPPER, |c| out.push(c as char));
}

/// The lower-case hex text of `bytes`. [`from_hex`] reads it back like any
/// other case; it is not the text `IJson` writes.
///
/// ```
/// assert_eq!(psio::to_hex_lower([0xAB, 0x01]), "ab01");
/// ```
pub fn to_hex_lower(bytes: impl AsRef<[u8]>) -> String {
    let mut out = String::new();
    to_hex_lower_into(bytes, &mut out);
    out
}

/// Appends the lower-case hex text of `bytes` to `out`.
///
/// ```
/// let mut key = String::from("key:");
/// psio::to_hex_lower_into([0xAB, 0x01], &mut key);
/// assert_eq!(key, "key:ab01");
/// ```
pub fn to_hex_lower_into(bytes: impl AsRef<[u8]>, out: &mut String) {
    let bytes = bytes.as_ref();
    out.reserve(bytes.len() * 2);
    encode(bytes, LOWER, |c| out.push(c as char));
}

/// The bytes of hex text. Upper and lower case digits are accepted; a
/// character that is not a hex digit is an error at its offset, and an odd
/// number of digits is an error at the end of the text. The empty text is no
/// bytes.
///
/// ```
/// assert_eq!(psio::from_hex("00ff7F").unwrap(), [0, 255, 127]);
/// assert!(psio::from_hex("").unwrap().is_empty());
/// assert_eq!(psio::from_hex("0x12").unwrap_err().offset, 1);
/// assert!(psio::from_hex("123").is_err());
/// ```
pub fn from_hex(text: &str) -> Result<Vec<u8>, HexError> {
    checked(text).map(decode_vec)
}

/// Fills `out` from hex text of exactly `2 * out.len()` digits, without
/// allocating (the fixed-destination `from_hex` of C++). On an error `out` is
/// untouched.
///
/// ```
/// let mut hash = [0u8; 4];
/// psio::from_hex_into("c0ffee00", &mut hash).unwrap();
/// assert_eq!(hash, [0xC0, 0xFF, 0xEE, 0x00]);
/// assert!(psio::from_hex_into("c0ffee", &mut hash).is_err());
/// ```
pub fn from_hex_into(text: &str, out: &mut [u8]) -> Result<(), HexError> {
    let digits = checked(text)?;
    if digits.len() / 2 != out.len() {
        return Err(HexError { what: WRONG_LENGTH, offset: digits.len() });
    }
    decode(digits, out);
    Ok(())
}

/// An array of `N` bytes from hex text of exactly `2 * N` digits.
///
/// ```
/// let id = psio::from_hex_array::<3>("0A0b0C").unwrap();
/// assert_eq!(id, [10, 11, 12]);
/// assert!(psio::from_hex_array::<3>("0A0b").is_err());
/// ```
pub fn from_hex_array<const N: usize>(text: &str) -> Result<[u8; N], HexError> {
    let mut out = [0u8; N];
    from_hex_into(text, &mut out)?;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{from_ijson, to_ijson};

    fn err(what: &'static str, offset: usize) -> HexError {
        HexError { what, offset }
    }

    #[test]
    fn round_trips() {
        let all: Vec<u8> = (0..=255).collect();
        for bytes in [&[][..], &[0], &[0xff], &[1, 2, 3], b"hello", &all] {
            let upper = to_hex(bytes);
            let lower = to_hex_lower(bytes);
            assert_eq!(upper.len(), bytes.len() * 2);
            assert_eq!(upper, lower.to_ascii_uppercase());
            assert_eq!(lower, upper.to_ascii_lowercase());
            assert_eq!(from_hex(&upper).unwrap(), bytes);
            assert_eq!(from_hex(&lower).unwrap(), bytes);
        }
        assert_eq!(to_hex([0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]), "0123456789ABCDEF");
        assert_eq!(to_hex_lower([0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]), "0123456789abcdef");
    }

    #[test]
    fn accepts_owned_and_borrowed_bytes() {
        assert_eq!(to_hex(vec![0xa0u8]), "A0");
        assert_eq!(to_hex(&[0xa0u8][..]), "A0");
        assert_eq!(to_hex([0xa0u8; 2]), "A0A0");
        assert_eq!(to_hex("hi"), "6869");
    }

    #[test]
    fn into_appends() {
        let mut out = String::from("x");
        to_hex_into([0xab], &mut out);
        to_hex_lower_into([0xcd], &mut out);
        to_hex_into([], &mut out);
        assert_eq!(out, "xABcd");
    }

    #[test]
    fn empty() {
        assert_eq!(to_hex([]), "");
        assert_eq!(to_hex_lower([]), "");
        assert_eq!(from_hex(""), Ok(vec![]));
        assert_eq!(from_hex_into("", &mut []), Ok(()));
        assert_eq!(from_hex_array::<0>(""), Ok([]));
    }

    #[test]
    fn upper_lower_and_mixed_case_input() {
        assert_eq!(from_hex("DEADBEEF").unwrap(), [0xde, 0xad, 0xbe, 0xef]);
        assert_eq!(from_hex("deadbeef").unwrap(), [0xde, 0xad, 0xbe, 0xef]);
        assert_eq!(from_hex("dEaDBeeF").unwrap(), [0xde, 0xad, 0xbe, 0xef]);
        assert_eq!(from_hex_array::<2>("aB0c"), Ok([0xab, 0x0c]));
    }

    #[test]
    fn odd_length() {
        assert_eq!(from_hex("a"), Err(err(ODD_LENGTH, 1)));
        assert_eq!(from_hex("abc"), Err(err(ODD_LENGTH, 3)));
        assert_eq!(from_hex_into("abc", &mut [0; 1]), Err(err(ODD_LENGTH, 3)));
        assert_eq!(from_hex_array::<2>("abc"), Err(err(ODD_LENGTH, 3)));
    }

    #[test]
    fn bad_character() {
        assert_eq!(from_hex("g0"), Err(err(NON_HEX, 0)));
        assert_eq!(from_hex("0g"), Err(err(NON_HEX, 1)));
        assert_eq!(from_hex("0x12"), Err(err(NON_HEX, 1)));
        assert_eq!(from_hex("12 34"), Err(err(NON_HEX, 2)));
        assert_eq!(from_hex("12:34"), Err(err(NON_HEX, 2)));
        assert_eq!(from_hex("1234\n"), Err(err(NON_HEX, 4)));
        assert_eq!(from_hex("12\u{e9}4"), Err(err(NON_HEX, 2)));
        // A bad character is reported before the length.
        assert_eq!(from_hex("12z"), Err(err(NON_HEX, 2)));
        assert_eq!(from_hex_array::<4>("12z4"), Err(err(NON_HEX, 2)));
        assert_eq!(
            from_hex("0g").unwrap_err().to_string(),
            "hex: non-hex character (at byte 1)"
        );
    }

    #[test]
    fn wrong_length_for_the_fixed_forms() {
        let mut out = [7u8; 2];
        assert_eq!(from_hex_into("ab", &mut out), Err(err(WRONG_LENGTH, 2)));
        assert_eq!(from_hex_into("abcdef", &mut out), Err(err(WRONG_LENGTH, 6)));
        assert_eq!(from_hex_into("", &mut out), Err(err(WRONG_LENGTH, 0)));
        assert_eq!(from_hex_into("abzz", &mut out), Err(err(NON_HEX, 2)));
        assert_eq!(out, [7, 7], "untouched on every error");
        assert_eq!(from_hex_into("abCD", &mut out), Ok(()));
        assert_eq!(out, [0xab, 0xcd]);

        assert_eq!(from_hex_array::<2>("ab"), Err(err(WRONG_LENGTH, 2)));
        assert_eq!(from_hex_array::<2>("abcdef"), Err(err(WRONG_LENGTH, 6)));
        assert_eq!(from_hex_array::<0>("ab"), Err(err(WRONG_LENGTH, 2)));
        assert_eq!(from_hex_array::<32>(&"5a".repeat(32)), Ok([0x5a; 32]));
    }

    #[test]
    fn agrees_with_the_ijson_byte_text() {
        let all: Vec<u8> = (0..=255).collect();
        for bytes in [vec![], vec![0], vec![0xde, 0xad, 0xbe, 0xef], b"psio".to_vec(), all] {
            let quoted = format!("\"{}\"", to_hex(&bytes));
            assert_eq!(to_ijson(&bytes), quoted);
            assert_eq!(from_ijson::<Vec<u8>>(&quoted).unwrap(), bytes);
            // The reader takes lower case too, as from_hex does.
            let lower = format!("\"{}\"", to_hex_lower(&bytes));
            assert_eq!(from_ijson::<Vec<u8>>(&lower).unwrap(), from_hex(&to_hex_lower(&bytes)).unwrap());
        }
        let id = [0x01u8, 0xab, 0xff, 0x10];
        assert_eq!(to_ijson(&id), format!("\"{}\"", to_hex(id)));
        assert_eq!(from_ijson::<[u8; 4]>("\"01abFF10\"").unwrap(), from_hex_array::<4>("01abFF10").unwrap());

        // The same text is rejected by both.
        for bad in ["abc", "0x12", "12 34", "zz"] {
            assert!(from_hex(bad).is_err(), "{bad}");
            assert!(from_ijson::<Vec<u8>>(&format!("\"{bad}\"")).is_err(), "{bad}");
        }
        assert!(from_hex_array::<4>("0102").is_err());
        assert!(from_ijson::<[u8; 4]>("\"0102\"").is_err());
    }
}
