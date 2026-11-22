//! Names as 64-bit numbers: the two arithmetic coders of C++
//! `psio/compress_name.hpp` and `psio/name.hpp`, with identical output.
//!
//! * **Method names** ([`method_to_number`], [`number_to_method`],
//!   [`is_hash_name`]; C++ `psio::detail::method_to_number`, JS
//!   `nameToNumber`): letters `a`-`z` (case is ignored; a digit or `_`
//!   reads as a letter's symbol) of any length. A name the coder cannot fit
//!   in 64 bits becomes a hash of its bytes with bit 56 set, written back
//!   as `#` and sixteen letters.
//! * **Identifier names** ([`name_to_number`], [`number_to_name`],
//!   [`NameId`]; C++ `psio::name_to_number`, `psio::name_id`): `a`-`z`,
//!   `0`-`9` and `-`, at most 18 characters, not starting with a digit or
//!   `-`. A name that does not fit is 0; there is no hash form. (As in
//!   C++, a name with a doubled hyphen is given a number that decodes to
//!   another text.)
//!
//! The two encodings are different tables and are not interchangeable.
//! Both encoders are `const fn`, so a number can be a constant:
//!
//! ```
//! use psio::compress_name::{method_to_number, name_to_number, number_to_method, number_to_name};
//!
//! const TRANSFER: u64 = method_to_number("transfer");
//! assert_eq!(number_to_method(TRANSFER), "transfer");
//! assert_eq!(method_to_number("spiderman"), 311625498215);
//!
//! const ALICE: u64 = name_to_number("alice");
//! assert_eq!(number_to_name(ALICE), "alice");
//! assert_eq!(name_to_number("not a name"), 0);
//! ```
//!
//! Derived from code by Mark Thomas Nelson (MIT License, Copyright (c)
//! 2014; "Data Compression With Arithmetic Coding"), modified by Daniel
//! Larimer for short names, with tables that depend on the previous symbol.

mod tables;

type CodeValue = u32;
const CODE_VALUE_BITS: u32 = (u32::BITS + 3) / 2;
const MAX_CODE: CodeValue = (1 << CODE_VALUE_BITS) - 1;
const ONE_FOURTH: CodeValue = 1 << (CODE_VALUE_BITS - 2);
const ONE_HALF: CodeValue = 2 * ONE_FOURTH;
const THREE_FOURTHS: CodeValue = 3 * ONE_FOURTH;

/// The bit that marks a method number as a hash (C++ `is_hash_name`).
const HASH_BIT: u64 = 1 << (64 - 8);

/// The bit writer of both encoders: bits fill a byte from its high bit, bytes
/// fill the number from its low byte.
struct Bits {
    bit: u8,
    next_byte: u8,
    mask: u8,
    output: u64,
    /// Bits written (the method coder's limit of 63).
    count: u32,
    /// The method coder's rule: a set bit at position 63 voids the number.
    method: bool,
}

impl Bits {
    const fn new(method: bool) -> Self {
        Self { bit: 0, next_byte: 0, mask: 0x80, output: 0, count: 0, method }
    }
    const fn put(mut self, b: bool) -> Self {
        if self.method {
            if b && self.count == 63 {
                self.output = 0;
                self.bit = 64;
            } else {
                self.count += 1;
                if b {
                    self.next_byte |= self.mask;
                }
                self.mask >>= 1;
                if self.mask == 0 && self.bit < 64 {
                    self.output |= (self.next_byte as u64) << self.bit;
                    self.bit += 8;
                    self.mask = 0x80;
                    self.next_byte = 0;
                }
            }
        } else {
            if b {
                self.next_byte |= self.mask;
            }
            self.mask >>= 1;
            if self.mask == 0 {
                if self.bit < 64 {
                    self.output |= (self.next_byte as u64) << self.bit;
                }
                self.bit = self.bit.wrapping_add(8);
                self.mask = 0x80;
                self.next_byte = 0;
            }
        }
        self
    }
    const fn put_plus_pending(mut self, bit: bool, mut pending: u32) -> Self {
        self = self.put(bit);
        while pending > 0 {
            self = self.put(!bit);
            pending -= 1;
        }
        self
    }
}

/// The arithmetic encoder shared by the two coders. Returns the writer and
/// whether the whole input, terminator included, was consumed.
const fn encode<const W: usize, const W1: usize>(
    input: &[u8],
    char_to_symbol: &[u8; 256],
    model_cf: &[[u16; W1]; W],
    method: bool,
) -> Option<(Bits, bool)> {
    let mut bits = Bits::new(method);
    let mut at = 0usize;
    let mut last = 0usize;
    let mut pending: u32 = 0;
    let mut low: CodeValue = 0;
    let mut high: CodeValue = MAX_CODE;
    let mut c: usize = 1;
    while c != 0 && bits.bit < 64 {
        c = if at == input.len() {
            0
        } else {
            let symbol = char_to_symbol[input[at] as usize] as usize;
            at += 1;
            symbol
        };
        let row = &model_cf[last];
        let (p_low, p_high, p_count) = (row[c] as CodeValue, row[c + 1] as CodeValue, row[W] as CodeValue);
        last = c;
        let range = high.wrapping_sub(low).wrapping_add(1);
        if p_count == 0 {
            return None;
        }
        high = low.wrapping_add(range.wrapping_mul(p_high) / p_count).wrapping_sub(1);
        low = low.wrapping_add(range.wrapping_mul(p_low) / p_count);
        while bits.bit < 64 {
            if high < ONE_HALF {
                bits = bits.put_plus_pending(false, pending);
                pending = 0;
            } else if low >= ONE_HALF {
                bits = bits.put_plus_pending(true, pending);
                pending = 0;
            } else if low >= ONE_FOURTH && high < THREE_FOURTHS {
                pending += 1;
                low -= ONE_FOURTH;
                high -= ONE_FOURTH;
            } else {
                break;
            }
            high <<= 1;
            high += 1;
            low <<= 1;
            high &= MAX_CODE;
            low &= MAX_CODE;
        }
    }
    pending += 1;
    bits = bits.put_plus_pending(low >= ONE_FOURTH, pending);
    Some((bits, at == input.len() && c == 0))
}

/// The arithmetic decoder shared by the two coders.
fn decode<const W: usize, const W1: usize>(
    mut input: u64,
    symbol_to_char: &[u8; W],
    model_cf: &[[u16; W1]; W],
) -> String {
    let mut current_byte: u64 = 0;
    let mut last_mask: u8 = 1;
    // Extra bits are brought in past the 64: they improve accuracy.
    let mut not_eof: i32 = 64 + 16;
    let mut get_bit = |not_eof: &mut i32| -> CodeValue {
        if last_mask == 1 {
            current_byte = input & 0xff;
            input >>= 8;
            last_mask = 0x80;
        } else {
            last_mask >>= 1;
        }
        *not_eof -= 1;
        (current_byte & last_mask as u64 != 0) as CodeValue
    };

    let mut out = String::new();
    let mut last = 0usize;
    let mut high: CodeValue = MAX_CODE;
    let mut low: CodeValue = 0;
    let mut value: CodeValue = 0;
    let mut i = 0;
    while i < CODE_VALUE_BITS && not_eof != 0 {
        value = (value << 1).wrapping_add(get_bit(&mut not_eof));
        i += 1;
    }
    while not_eof != 0 {
        let range = high.wrapping_sub(low).wrapping_add(1);
        if range == 0 {
            return "RUNTIME LOGIC ERROR".into();
        }
        let row = &model_cf[last];
        let count = row[W] as CodeValue;
        let scaled = value.wrapping_sub(low).wrapping_add(1).wrapping_mul(count).wrapping_sub(1) / range;
        // The symbol whose interval holds the scaled value; none: the end.
        let mut c = 0usize;
        let (mut p_low, mut p_high, mut p_count) = (0, 1, 1);
        for symbol in 0..W {
            if scaled < row[symbol + 1] as CodeValue {
                if count == 0 {
                    last = 0;
                } else {
                    c = symbol;
                    (p_low, p_high, p_count) = (row[symbol] as CodeValue, row[symbol + 1] as CodeValue, count);
                    last = c;
                }
                break;
            }
        }
        if c == 0 {
            break;
        }
        out.push(symbol_to_char[c] as char);
        high = low.wrapping_add(range.wrapping_mul(p_high) / p_count).wrapping_sub(1);
        low = low.wrapping_add(range.wrapping_mul(p_low) / p_count);
        while not_eof != 0 {
            if high < ONE_HALF {
                // The bit is a zero.
            } else if low >= ONE_HALF {
                value = value.wrapping_sub(ONE_HALF);
                low -= ONE_HALF;
                high -= ONE_HALF;
            } else if low >= ONE_FOURTH && high < THREE_FOURTHS {
                value = value.wrapping_sub(ONE_FOURTH);
                low -= ONE_FOURTH;
                high -= ONE_FOURTH;
            } else {
                break;
            }
            low <<= 1;
            high <<= 1;
            high += 1;
            value = (value << 1).wrapping_add(get_bit(&mut not_eof));
        }
    }
    out
}

/// C++ `psio::detail::seahash`: the hash behind a method name that does not
/// compress.
const fn seahash(input: &[u8]) -> u64 {
    const fn g(mut x: u64) -> u64 {
        const P: u64 = 0x6eed_0e9d_a4d9_4a4f;
        x = x.wrapping_mul(P);
        x ^= (x >> 32) >> (x >> 60);
        x.wrapping_mul(P)
    }
    let mut state: [u64; 4] =
        [0x16f1_1fe8_9b0d_677c, 0xb480_a793_d8e6_c86c, 0x6fe2_e5aa_f078_ebc9, 0x14f9_94a4_c525_9381];
    let mut i = 0;
    while i < input.len() {
        let mut n: u64 = 0;
        let bytes = if input.len() - i < 8 { input.len() - i } else { 8 };
        let mut j = 0;
        while j < bytes {
            n = n.wrapping_add((input[i + j] as u64) << (j * 8));
            j += 1;
        }
        let slot = (i / 8) % 4;
        state[slot] = g(state[slot] ^ n);
        i += 8;
    }
    g(state[0] ^ state[1] ^ state[2] ^ state[3] ^ input.len() as u64)
}

/// The number of a method name (C++ `psio::detail::method_to_number`).
///
/// * `""` is 0.
/// * A name of the coder's characters that fits is its arithmetic code.
/// * A name that holds another character is 0.
/// * A name of the coder's characters that does not fit is its hash with
///   bit 56 set ([`is_hash_name`]).
/// * `#` and sixteen letters is the hash those letters spell (what
///   [`number_to_method`] writes for a hash); `#` with any other length is
///   `u64::MAX`.
pub const fn method_to_number(name: &str) -> u64 {
    use tables::method::{CHAR_TO_SYMBOL, MODEL_CF};
    let input = name.as_bytes();
    if input.is_empty() {
        return 0;
    }
    if input[0] == b'#' {
        if input.len() != 17 {
            return u64::MAX;
        }
        let mut output: u64 = 0;
        let mut i = 1;
        while i < 17 {
            // As C++: the symbol minus one, as a byte, at its nibble.
            let symbol = CHAR_TO_SYMBOL[input[i] as usize].wrapping_sub(1) as u64;
            output |= symbol << (4 * (i - 1));
            i += 1;
        }
        return output;
    }
    let mut i = 0;
    while i < input.len() {
        if CHAR_TO_SYMBOL[input[i] as usize] == 0 {
            return 0;
        }
        i += 1;
    }
    let Some((bits, consumed)) = encode(input, &CHAR_TO_SYMBOL, &MODEL_CF, true) else {
        return 0;
    };
    let mut output = bits.output;
    if bits.mask != 0x80 && bits.bit < 64 {
        output |= (bits.next_byte as u64) << bits.bit;
    }
    if !consumed {
        output = 0;
    }
    if output == 0 {
        output = seahash(input) | HASH_BIT;
    }
    output
}

/// Whether a method number is a hash, not an arithmetic code (C++
/// `psio::detail::is_hash_name`).
pub const fn is_hash_name(number: u64) -> bool {
    number & HASH_BIT != 0
}

/// The name of a method number (C++ `psio::detail::number_to_method`): the
/// name that compressed to it, or `#` and sixteen letters for a hash.
pub fn number_to_method(number: u64) -> String {
    use tables::method::{MODEL_CF, SYMBOL_TO_CHAR};
    if number == 0 {
        return String::new();
    }
    if is_hash_name(number) {
        let mut out = String::with_capacity(17);
        out.push('#');
        let mut rest = number;
        for _ in 0..16 {
            out.push(SYMBOL_TO_CHAR[(rest & 0x0f) as usize + 1] as char);
            rest >>= 4;
        }
        return out;
    }
    decode(number, &SYMBOL_TO_CHAR, &MODEL_CF)
}

/// The number of an identifier name (C++ `psio::name_to_number`): 0 when
/// the name is empty, longer than 18 characters, starts with a digit or
/// `-`, holds a character outside `a`-`z`, `0`-`9`, `-`, or does not fit
/// in 64 bits.
pub const fn name_to_number(name: &str) -> u64 {
    use tables::name::{CHAR_TO_SYMBOL, MODEL_CF};
    let input = name.as_bytes();
    if input.is_empty() || input.len() > 18 {
        return 0;
    }
    // C++ compares a `char`: every byte that is not above '9' as a signed
    // char, which no byte of a valid name's first character is.
    if input[0] <= b'9' || input[0] >= 0x80 {
        return 0;
    }
    let mut i = 0;
    while i < input.len() {
        if CHAR_TO_SYMBOL[input[i] as usize] == 0 {
            return 0;
        }
        i += 1;
    }
    let Some((bits, consumed)) = encode(input, &CHAR_TO_SYMBOL, &MODEL_CF, false) else {
        return 0;
    };
    let mut output = bits.output;
    if bits.mask != 0x80 {
        if bits.bit < 64 {
            output |= (bits.next_byte as u64) << bits.bit;
        } else {
            output = 0;
        }
    }
    if !consumed {
        return 0;
    }
    output
}

/// The name of an identifier number (C++ `psio::number_to_name`); `""` for
/// 0. A number no name compresses to still decodes to some text: compare
/// `name_to_number(&text)` with the number to know.
pub fn number_to_name(number: u64) -> String {
    use tables::name::{MODEL_CF, SYMBOL_TO_CHAR};
    if number == 0 {
        return String::new();
    }
    decode(number, &SYMBOL_TO_CHAR, &MODEL_CF)
}

/// A compressed identifier name (C++ `psio::name_id`): a record of one
/// `u64` that never changes, whose text is the name.
#[derive(
    Debug, Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash,
    crate::PsszPack, crate::PsszUnpack, crate::ToPsszSchema, crate::Key,
)]
#[psio(crate = "crate", definition_will_not_change, name = "name_id")]
pub struct NameId {
    pub value: u64,
}

impl NameId {
    pub const fn new(value: u64) -> Self {
        Self { value }
    }
    /// The id of `name`; 0 when it is not a name.
    pub const fn from_name(name: &str) -> Self {
        Self { value: name_to_number(name) }
    }
    pub const fn is_zero(&self) -> bool {
        self.value == 0
    }
}

impl std::fmt::Display for NameId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&number_to_name(self.value))
    }
}

/// The text is not a name: [`name_to_number`] gives 0 for it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NotAName;

impl std::fmt::Display for NotAName {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("not a compressible name (a-z, 0-9, '-', at most 18 characters, starting with a letter)")
    }
}
impl std::error::Error for NotAName {}

impl std::str::FromStr for NameId {
    type Err = NotAName;
    /// `""` is the zero id; any other text must be a name.
    fn from_str(text: &str) -> Result<Self, NotAName> {
        let id = Self::from_name(text);
        if id.value == 0 && !text.is_empty() {
            return Err(NotAName);
        }
        Ok(id)
    }
}
