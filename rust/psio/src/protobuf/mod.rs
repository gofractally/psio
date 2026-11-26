//! Protocol Buffers (proto3) from Rust types: the wire and the accepted set
//! of C++ `psio::protobuf` (`cpp/include/psio/protobuf.hpp`,
//! `docs/protobuf.md`). No dependency on a protobuf library; the bytes are
//! the ones the official implementation writes deterministically
//! (`conformance/protobuf-vectors.json`).
//!
//! ```
//! #[derive(Debug, PartialEq, psio::Protobuf)]
//! struct Profile {
//!     id: u64,
//!     name: String,
//!     #[psio(pb_sint)]
//!     balance: i64,
//!     tags: Vec<String>,
//! }
//!
//! let profile = Profile { id: 7, name: "Alice".into(), balance: -3, tags: vec![] };
//! let bytes = psio::to_protobuf(&profile);
//! assert_eq!(bytes, [0x08, 7, 0x12, 5, b'A', b'l', b'i', b'c', b'e', 0x18, 5]);
//! psio::validate_protobuf::<Profile>(&bytes).unwrap();
//! assert_eq!(psio::from_protobuf::<Profile>(&bytes).unwrap(), profile);
//! ```
//!
//! The mapping (`#[derive(Protobuf)]`):
//!
//! | Rust | protobuf |
//! |---|---|
//! | `bool` | `bool` |
//! | `i8` / `i16` / `i32` | `int32` (`pb_sint`: `sint32`, `pb_fixed`: `sfixed32`) |
//! | `u8` / `u16` / `u32` | `uint32` (`pb_fixed`: `fixed32`) |
//! | `i64` / `u64` | `int64` / `uint64` (`sint64`, `sfixed64` / `fixed64`) |
//! | `f32` / `f64` | `float` / `double` |
//! | field-less `#[repr(int)]` enum | `enum` (open with a `#[psio(other)]` variant) |
//! | `String` | `string` (UTF-8, checked when read) |
//! | `Vec<u8>` | `bytes` |
//! | `[u8; N]` | `bytes` of exactly N (all zero as a member: not written) |
//! | struct | message |
//! | `Option<X>` | `optional X` |
//! | `Vec<X>` / `BTreeSet<X>` | `repeated X` (packed for scalars) |
//! | `BTreeMap<K, V>` | `map<K, V>` |
//! | enum whose variants hold one value each | `oneof` (a unit first variant: none set) |
//!
//! Field numbers are `#[psio(field = N)]`, else the 1-based declaration
//! position; a oneof member takes one number per alternative, counting up
//! from its own. Fields are written in ascending number order, a member
//! that is not optional is not written when it is zero / empty, and an
//! absent member reads as the proto3 zero value (not `Default`). Unknown
//! fields, fields of another wire type and groups are skipped. One reader
//! serves [`validate_protobuf`] and [`from_protobuf`]: they accept the
//! same bytes.

use std::collections::{BTreeMap, BTreeSet};

pub use psio_macros::Protobuf;

/// What bytes are refused for, and where.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PbError {
    message: &'static str,
    offset: u32,
}

impl PbError {
    /// The reason: the text C++ `codec_status::error().what` has for the
    /// same bytes.
    pub fn message(&self) -> &'static str {
        self.message
    }
    /// The position in the bytes the check failed at.
    pub fn offset(&self) -> u32 {
        self.offset
    }
}

impl std::fmt::Display for PbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} (at byte {})", self.message, self.offset)
    }
}

impl std::error::Error for PbError {}

/// The reader's limits (C++ `psio::protobuf::limits`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PbLimits {
    /// Messages nested inside the root message (map entries and skipped
    /// groups count). The official implementations stop at 100.
    pub max_depth: u32,
}

impl Default for PbLimits {
    fn default() -> Self {
        PbLimits { max_depth: 100 }
    }
}

/// A message is at most 2 GiB - 1.
pub const MAX_MESSAGE_SIZE: usize = 0x7fff_ffff;
/// The largest field number.
pub const MAX_FIELD_NUMBER: u32 = (1 << 29) - 1;

/// How an integer is written: `int32`-like, `fixed32`-like (`pb_fixed`) or
/// `sint32`-like (`pb_sint`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PbEnc {
    Plain,
    Fixed,
    Zigzag,
}

pub const WIRE_VARINT: u8 = 0;
pub const WIRE_FIXED64: u8 = 1;
pub const WIRE_LEN: u8 = 2;
pub const WIRE_SGROUP: u8 = 3;
pub const WIRE_EGROUP: u8 = 4;
pub const WIRE_FIXED32: u8 = 5;

/// Whether the field numbers `start .. start + slots` are ones protobuf
/// allows (the derive checks every member with it at compile time).
pub const fn pb_numbers_valid(start: u32, slots: u32) -> bool {
    if slots == 0 {
        return true;
    }
    let end = start as u64 + slots as u64 - 1;
    start >= 1 && end <= MAX_FIELD_NUMBER as u64 && !(start <= 19999 && end >= 19000)
}

pub fn varint_size(v: u64) -> usize {
    if v < 0x80 {
        1
    } else {
        (64 - v.leading_zeros() as usize + 6) / 7
    }
}

pub const fn tag_size(number: u32) -> usize {
    if number < 1 << 4 {
        1
    } else if number < 1 << 11 {
        2
    } else if number < 1 << 18 {
        3
    } else if number < 1 << 25 {
        4
    } else {
        5
    }
}

// ── Writing, back to front ──────────────────────────────────────────────────

/// Writes a message from its last byte to its first, into a buffer of
/// exactly its size: a length is known when its content has been written.
pub struct PbWriter<'a> {
    buf: &'a mut [u8],
    p: usize,
    overrun: bool,
}

impl<'a> PbWriter<'a> {
    fn new(buf: &'a mut [u8]) -> Self {
        let p = buf.len();
        PbWriter { buf, p, overrun: false }
    }
    /// Where the next byte goes (bytes written so far: from here to the end).
    pub fn position(&self) -> usize {
        self.p
    }
    fn room(&mut self, n: usize) -> bool {
        if self.p < n {
            self.overrun = true;
            return false;
        }
        true
    }
    pub fn bytes(&mut self, data: &[u8]) {
        let n = data.len();
        if n != 0 && self.room(n) {
            self.p -= n;
            self.buf[self.p..self.p + n].copy_from_slice(data);
        }
    }
    pub fn varint(&mut self, mut v: u64) {
        let n = varint_size(v);
        if !self.room(n) {
            return;
        }
        self.p -= n;
        let mut q = self.p;
        while v >= 0x80 {
            self.buf[q] = (v as u8) | 0x80;
            q += 1;
            v >>= 7;
        }
        self.buf[q] = v as u8;
    }
    pub fn fixed32(&mut self, v: u32) {
        self.bytes(&v.to_le_bytes());
    }
    pub fn fixed64(&mut self, v: u64) {
        self.bytes(&v.to_le_bytes());
    }
    pub fn tag(&mut self, number: u32, wire: u8) {
        self.varint(((number as u64) << 3) | wire as u64);
    }
    /// The length of what was written since `end`, then the tag.
    pub fn close(&mut self, end: usize, number: u32) {
        self.varint((end - self.p) as u64);
        self.tag(number, WIRE_LEN);
    }
}

// ── Reading ─────────────────────────────────────────────────────────────────

/// The reader: every read is bounded by the `end` of the enclosing message.
pub struct PbReader<'a> {
    bytes: &'a [u8],
    p: usize,
    depth: u32,
    max_depth: u32,
}

type Read<T = ()> = Result<T, PbError>;

impl<'a> PbReader<'a> {
    fn new(bytes: &'a [u8], max_depth: u32) -> Self {
        PbReader { bytes, p: 0, depth: 0, max_depth }
    }
    pub fn position(&self) -> usize {
        self.p
    }
    pub fn fail<T>(&self, message: &'static str) -> Read<T> {
        Err(PbError { message, offset: self.p as u32 })
    }
    /// A varint of at most ten bytes, the last holding one bit.
    pub fn varint(&mut self, end: usize) -> Read<u64> {
        if self.p < end && self.bytes[self.p] < 0x80 {
            self.p += 1;
            return Ok(self.bytes[self.p - 1] as u64);
        }
        let mut out = 0u64;
        let mut at = self.p;
        let mut shift = 0;
        while shift < 70 {
            if at == end {
                return self.fail("protobuf: truncated varint");
            }
            let b = self.bytes[at];
            at += 1;
            if shift == 63 && b > 1 {
                return self.fail("protobuf: varint exceeds 64 bits");
            }
            out |= ((b & 0x7f) as u64) << shift;
            if b < 0x80 {
                self.p = at;
                return Ok(out);
            }
            shift += 7;
        }
        self.fail("protobuf: varint exceeds 64 bits")
    }
    fn varint32(&mut self, end: usize, too_long: &'static str) -> Read<u32> {
        if self.p < end && self.bytes[self.p] < 0x80 {
            self.p += 1;
            return Ok(self.bytes[self.p - 1] as u32);
        }
        let mut out = 0u32;
        let mut at = self.p;
        let mut shift = 0;
        while shift < 35 {
            if at == end {
                return self.fail("protobuf: truncated varint");
            }
            let b = self.bytes[at];
            at += 1;
            if shift == 28 && b > 0x0f {
                return self.fail(too_long);
            }
            out |= ((b & 0x7f) as u32) << shift;
            if b < 0x80 {
                self.p = at;
                return Ok(out);
            }
            shift += 7;
        }
        self.fail(too_long)
    }
    pub fn tag(&mut self, end: usize) -> Read<u32> {
        self.varint32(end, "protobuf: tag exceeds 32 bits")
    }
    /// A length, and that many bytes are there.
    pub fn length(&mut self, end: usize) -> Read<usize> {
        let v = self.varint32(end, "protobuf: length exceeds the input")? as usize;
        if v > end - self.p {
            return self.fail("protobuf: length exceeds the input");
        }
        Ok(v)
    }
    pub fn fixed32(&mut self, end: usize) -> Read<u64> {
        if end - self.p < 4 {
            return self.fail("protobuf: truncated fixed32");
        }
        let v = u32::from_le_bytes(self.bytes[self.p..self.p + 4].try_into().unwrap());
        self.p += 4;
        Ok(v as u64)
    }
    pub fn fixed64(&mut self, end: usize) -> Read<u64> {
        if end - self.p < 8 {
            return self.fail("protobuf: truncated fixed64");
        }
        let v = u64::from_le_bytes(self.bytes[self.p..self.p + 8].try_into().unwrap());
        self.p += 8;
        Ok(v)
    }
    pub fn enter(&mut self) -> Read {
        if self.depth >= self.max_depth {
            return self.fail("protobuf: nested too deeply");
        }
        self.depth += 1;
        Ok(())
    }
    pub fn leave(&mut self) {
        self.depth -= 1;
    }
    /// `n` bytes, taken.
    fn take(&mut self, n: usize) -> &'a [u8] {
        let out = &self.bytes[self.p..self.p + n];
        self.p += n;
        out
    }
    /// Whether the message that ends at `end` has more fields.
    pub fn more(&self, end: usize) -> bool {
        self.p < end
    }
    /// The next tag of a message: its field number and wire type.
    pub fn field(&mut self, end: usize) -> Read<(u32, u8)> {
        let t = self.tag(end)?;
        if t >> 3 == 0 {
            return self.fail("protobuf: field number 0");
        }
        Ok((t >> 3, (t & 7) as u8))
    }
    /// A field nothing reads. A group is skipped without recursion: the
    /// numbers of the open groups are kept in a list as long as the depth.
    pub fn skip(&mut self, end: usize, number: u32, wire: u8) -> Read {
        match wire {
            WIRE_VARINT => self.varint(end).map(drop),
            WIRE_FIXED64 => self.fixed64(end).map(drop),
            WIRE_FIXED32 => self.fixed32(end).map(drop),
            WIRE_LEN => {
                let n = self.length(end)?;
                self.p += n;
                Ok(())
            }
            WIRE_SGROUP => {
                let base = self.depth;
                let mut open = vec![number];
                self.enter()?;
                while let Some(&current) = open.last() {
                    if self.p == end {
                        return self.fail("protobuf: unterminated group");
                    }
                    let t = self.tag(end)?;
                    if t >> 3 == 0 {
                        return self.fail("protobuf: field number 0");
                    }
                    match (t & 7) as u8 {
                        WIRE_EGROUP => {
                            if t >> 3 != current {
                                return self.fail("protobuf: a group ends with another field number");
                            }
                            open.pop();
                            self.leave();
                        }
                        WIRE_SGROUP => {
                            self.enter()?;
                            open.push(t >> 3);
                        }
                        w => self.skip(end, t >> 3, w)?,
                    }
                }
                debug_assert_eq!(self.depth, base);
                Ok(())
            }
            WIRE_EGROUP => self.fail("protobuf: end of a group that was not begun"),
            _ => self.fail("protobuf: unknown wire type"),
        }
    }
}

/// Strict UTF-8 (no overlong forms, no surrogates, at most U+10FFFF): what
/// `std::str::from_utf8` accepts.
fn valid_utf8(bytes: &[u8]) -> bool {
    std::str::from_utf8(bytes).is_ok()
}

// ── A value with one wire form ──────────────────────────────────────────────

/// A value with one wire form: a scalar, a string, bytes or a message —
/// what an `Option`, a list element, a map key / value or a oneof
/// alternative may be. Implemented for the standard types and by
/// `#[derive(Protobuf)]`.
pub trait PbSingle: Sized {
    /// A scalar: packed in a list.
    const SCALAR: bool = false;
    /// 0: not an integer; 1: an unsigned integer (`pb_fixed`); 2: a signed
    /// one (`pb_fixed`, `pb_sint`).
    const INTEGER: u8 = 0;
    fn pb_wire(_enc: PbEnc) -> u8 {
        WIRE_LEN
    }
    /// The proto3 zero value.
    fn pb_zero() -> Self;
    /// Whether a member that is not optional is left out (it is zero).
    fn pb_absent(&self, enc: PbEnc) -> bool;
    /// The value without its tag (with its length where it has one).
    fn pb_payload_size(&self, enc: PbEnc) -> usize;
    fn pb_write_payload(&self, enc: PbEnc, w: &mut PbWriter);
    /// Reads the value (a message: merges into `out`); `None` checks only.
    fn pb_read_payload(r: &mut PbReader, end: usize, enc: PbEnc, out: Option<&mut Self>) -> Read;

    /// Whether the type, or anything a value of it holds, declares
    /// something a value can violate (`psio::contract`).
    #[doc(hidden)]
    fn pb_declares(_seen: &mut crate::contract::Seen) -> bool {
        false
    }
    /// What the type declares about its values, on `self`.
    #[doc(hidden)]
    fn pb_check_declared(&self) -> Result<(), &'static str> {
        Ok(())
    }

    // A `Vec<Self>` member: a repeated field, except for `u8` (bytes).
    #[doc(hidden)]
    fn pb_vec_size(v: &[Self], number: u32, enc: PbEnc) -> usize {
        repeated_size(v.iter(), v.len(), number, enc)
    }
    #[doc(hidden)]
    fn pb_vec_write(v: &[Self], number: u32, enc: PbEnc, w: &mut PbWriter) {
        repeated_write(v.iter().rev(), v.is_empty(), number, enc, w)
    }
    #[doc(hidden)]
    fn pb_vec_read(r: &mut PbReader, end: usize, wire: u8, enc: PbEnc, out: Option<&mut Vec<Self>>) -> Read<bool> {
        repeated_read(r, end, wire, enc, out, |v: &mut Vec<Self>, x| v.push(x), |v, n| v.reserve(n))
    }
}

fn repeated_size<'x, X: PbSingle + 'x>(
    items: impl Iterator<Item = &'x X>,
    len: usize,
    number: u32,
    enc: PbEnc,
) -> usize {
    if len == 0 {
        return 0;
    }
    let tag = tag_size(number);
    if X::SCALAR {
        let payload: usize = match X::pb_wire(enc) {
            WIRE_FIXED32 => 4 * len,
            WIRE_FIXED64 => 8 * len,
            _ => items.map(|x| x.pb_payload_size(enc)).sum(),
        };
        tag + varint_size(payload as u64) + payload
    } else {
        items.map(|x| tag + x.pb_payload_size(enc)).sum()
    }
}

fn repeated_write<'x, X: PbSingle + 'x>(
    reversed: impl Iterator<Item = &'x X>,
    empty: bool,
    number: u32,
    enc: PbEnc,
    w: &mut PbWriter,
) {
    if empty {
        return;
    }
    if X::SCALAR {
        let end = w.position();
        for x in reversed {
            x.pb_write_payload(enc, w);
        }
        w.close(end, number);
    } else {
        for x in reversed {
            x.pb_write_payload(enc, w);
            w.tag(number, WIRE_LEN);
        }
    }
}

fn repeated_read<X: PbSingle, C>(
    r: &mut PbReader,
    end: usize,
    wire: u8,
    enc: PbEnc,
    mut out: Option<&mut C>,
    push: impl Fn(&mut C, X),
    reserve: impl Fn(&mut C, usize),
) -> Read<bool> {
    if X::SCALAR {
        if wire == WIRE_LEN {
            let n = r.length(end)?;
            let sub = r.position() + n;
            let width = match X::pb_wire(enc) {
                WIRE_FIXED32 => 4,
                WIRE_FIXED64 => 8,
                _ => 0,
            };
            if width != 0 {
                if n % width != 0 {
                    return r.fail("protobuf: packed fixed values of a broken length");
                }
                if let Some(c) = out.as_deref_mut() {
                    reserve(c, n / width);
                }
            }
            while r.more(sub) {
                let mut x = X::pb_zero();
                X::pb_read_payload(r, sub, enc, Some(&mut x))?;
                if let Some(c) = out.as_deref_mut() {
                    push(c, x);
                }
            }
            return Ok(true);
        }
        if wire != X::pb_wire(enc) {
            return Ok(false);
        }
        let mut x = X::pb_zero();
        X::pb_read_payload(r, end, enc, Some(&mut x))?;
        if let Some(c) = out {
            push(c, x);
        }
        Ok(true)
    } else {
        if wire != WIRE_LEN {
            return Ok(false);
        }
        match out {
            None => X::pb_read_payload(r, end, enc, None)?,
            Some(c) => {
                let mut x = X::pb_zero();
                X::pb_read_payload(r, end, enc, Some(&mut x))?;
                push(c, x);
            }
        }
        Ok(true)
    }
}

/// The 64 bits a scalar is on the wire (a varint's value, or the bits of a
/// fixed field), and back: C++ `scalar_bits` / `scalar_from_bits`.
#[doc(hidden)]
pub trait PbScalar: Copy {
    const WIDE: bool;
    const INTEGER: u8;
    const FLOAT: bool = false;
    fn bits(self, enc: PbEnc) -> u64;
    /// `None`: the value does not fit the type.
    fn from_bits(bits: u64, enc: PbEnc) -> Option<Self>;
}

fn zigzag64(x: i64) -> u64 {
    ((x as u64) << 1) ^ ((x >> 63) as u64)
}

macro_rules! narrow_signed {
    ($($t:ty),*) => {$(
        impl PbScalar for $t {
            const WIDE: bool = false;
            const INTEGER: u8 = 2;
            fn bits(self, enc: PbEnc) -> u64 {
                let x = self as i64;
                match enc {
                    PbEnc::Zigzag => zigzag64(x),
                    PbEnc::Fixed => x as u32 as u64,
                    PbEnc::Plain => x as u64,
                }
            }
            fn from_bits(bits: u64, enc: PbEnc) -> Option<Self> {
                let mut u = bits as u32;
                if enc == PbEnc::Zigzag {
                    u = (u >> 1) ^ 0u32.wrapping_sub(u & 1);
                }
                <$t>::try_from(u as i32).ok()
            }
        }
    )*};
}
macro_rules! narrow_unsigned {
    ($($t:ty),*) => {$(
        impl PbScalar for $t {
            const WIDE: bool = false;
            const INTEGER: u8 = 1;
            fn bits(self, _enc: PbEnc) -> u64 {
                self as u64
            }
            fn from_bits(bits: u64, _enc: PbEnc) -> Option<Self> {
                <$t>::try_from(bits as u32).ok()
            }
        }
    )*};
}
narrow_signed!(i8, i16, i32);
narrow_unsigned!(u8, u16, u32);

impl PbScalar for i64 {
    const WIDE: bool = true;
    const INTEGER: u8 = 2;
    fn bits(self, enc: PbEnc) -> u64 {
        if enc == PbEnc::Zigzag {
            zigzag64(self)
        } else {
            self as u64
        }
    }
    fn from_bits(bits: u64, enc: PbEnc) -> Option<Self> {
        Some(if enc == PbEnc::Zigzag { ((bits >> 1) ^ 0u64.wrapping_sub(bits & 1)) as i64 } else { bits as i64 })
    }
}
impl PbScalar for u64 {
    const WIDE: bool = true;
    const INTEGER: u8 = 1;
    fn bits(self, _enc: PbEnc) -> u64 {
        self
    }
    fn from_bits(bits: u64, _enc: PbEnc) -> Option<Self> {
        Some(bits)
    }
}
impl PbScalar for bool {
    const WIDE: bool = false;
    const INTEGER: u8 = 0;
    fn bits(self, _enc: PbEnc) -> u64 {
        self as u64
    }
    fn from_bits(bits: u64, _enc: PbEnc) -> Option<Self> {
        Some(bits != 0)
    }
}
impl PbScalar for f32 {
    const WIDE: bool = false;
    const INTEGER: u8 = 0;
    const FLOAT: bool = true;
    fn bits(self, _enc: PbEnc) -> u64 {
        self.to_bits() as u64
    }
    fn from_bits(bits: u64, _enc: PbEnc) -> Option<Self> {
        Some(f32::from_bits(bits as u32))
    }
}
impl PbScalar for f64 {
    const WIDE: bool = true;
    const INTEGER: u8 = 0;
    const FLOAT: bool = true;
    fn bits(self, _enc: PbEnc) -> u64 {
        self.to_bits()
    }
    fn from_bits(bits: u64, _enc: PbEnc) -> Option<Self> {
        Some(f64::from_bits(bits))
    }
}

/// The wire type of a scalar.
#[doc(hidden)]
pub fn scalar_wire<S: PbScalar>(enc: PbEnc) -> u8 {
    if S::FLOAT || enc == PbEnc::Fixed {
        if S::WIDE {
            WIRE_FIXED64
        } else {
            WIRE_FIXED32
        }
    } else {
        WIRE_VARINT
    }
}
#[doc(hidden)]
pub fn scalar_size<S: PbScalar>(v: S, enc: PbEnc) -> usize {
    match scalar_wire::<S>(enc) {
        WIRE_FIXED32 => 4,
        WIRE_FIXED64 => 8,
        _ => varint_size(v.bits(enc)),
    }
}
#[doc(hidden)]
pub fn scalar_write<S: PbScalar>(v: S, enc: PbEnc, w: &mut PbWriter) {
    match scalar_wire::<S>(enc) {
        WIRE_FIXED32 => w.fixed32(v.bits(enc) as u32),
        WIRE_FIXED64 => w.fixed64(v.bits(enc)),
        _ => w.varint(v.bits(enc)),
    }
}
#[doc(hidden)]
pub fn scalar_read<S: PbScalar>(r: &mut PbReader, end: usize, enc: PbEnc, out: Option<&mut S>) -> Read {
    let bits = match scalar_wire::<S>(enc) {
        WIRE_FIXED32 => r.fixed32(end)?,
        WIRE_FIXED64 => r.fixed64(end)?,
        _ => r.varint(end)?,
    };
    match S::from_bits(bits, enc) {
        None => r.fail("protobuf: a value does not fit its type"),
        Some(v) => {
            if let Some(out) = out {
                *out = v;
            }
            Ok(())
        }
    }
}

macro_rules! scalar_single {
    ($($t:ty => $zero:expr),*) => {$(
        impl PbSingle for $t {
            const SCALAR: bool = true;
            const INTEGER: u8 = <$t as PbScalar>::INTEGER;
            fn pb_wire(enc: PbEnc) -> u8 {
                scalar_wire::<$t>(enc)
            }
            fn pb_zero() -> Self {
                $zero
            }
            fn pb_absent(&self, enc: PbEnc) -> bool {
                PbScalar::bits(*self, enc) == 0
            }
            fn pb_payload_size(&self, enc: PbEnc) -> usize {
                scalar_size(*self, enc)
            }
            fn pb_write_payload(&self, enc: PbEnc, w: &mut PbWriter) {
                scalar_write(*self, enc, w)
            }
            fn pb_read_payload(r: &mut PbReader, end: usize, enc: PbEnc, out: Option<&mut Self>) -> Read {
                scalar_read(r, end, enc, out)
            }
        }
        single_field!($t);
    )*};
}

/// `PbField` of a type that is `PbSingle`: a singular field with implicit
/// presence.
macro_rules! single_field {
    ($t:ty) => {
        impl PbField for $t {
            const INTEGER: u8 = <$t as PbSingle>::INTEGER;
            fn pb_new() -> Self {
                <$t as PbSingle>::pb_zero()
            }
            fn pb_size(&self, number: u32, enc: PbEnc) -> usize {
                single_size(self, number, enc)
            }
            fn pb_write(&self, number: u32, enc: PbEnc, w: &mut PbWriter) {
                single_write(self, number, enc, w)
            }
            fn pb_read(r: &mut PbReader, end: usize, _slot: u32, wire: u8, enc: PbEnc, out: Option<&mut Self>) -> Read<bool> {
                single_read(r, end, wire, enc, out)
            }
            fn pb_declares(seen: &mut crate::contract::Seen) -> bool {
                <$t as PbSingle>::pb_declares(seen)
            }
            fn pb_check_declared(&self) -> Result<(), &'static str> {
                <$t as PbSingle>::pb_check_declared(self)
            }
        }
    };
}

/// A singular member with implicit presence (the derive uses these for the
/// types it implements).
#[doc(hidden)]
pub fn single_size<X: PbSingle>(v: &X, number: u32, enc: PbEnc) -> usize {
    if v.pb_absent(enc) {
        0
    } else {
        tag_size(number) + v.pb_payload_size(enc)
    }
}
#[doc(hidden)]
pub fn single_write<X: PbSingle>(v: &X, number: u32, enc: PbEnc, w: &mut PbWriter) {
    if !v.pb_absent(enc) {
        v.pb_write_payload(enc, w);
        w.tag(number, X::pb_wire(enc));
    }
}
#[doc(hidden)]
pub fn single_read<X: PbSingle>(r: &mut PbReader, end: usize, wire: u8, enc: PbEnc, out: Option<&mut X>) -> Read<bool> {
    if wire != X::pb_wire(enc) {
        return Ok(false);
    }
    X::pb_read_payload(r, end, enc, out)?;
    Ok(true)
}

scalar_single!(bool => false, i8 => 0, i16 => 0, i32 => 0, i64 => 0, u16 => 0, u32 => 0, u64 => 0,
               f32 => 0.0, f64 => 0.0);

// `u8`: a uint32 as a member, and `Vec<u8>` is bytes.
impl PbSingle for u8 {
    const SCALAR: bool = true;
    const INTEGER: u8 = 1;
    fn pb_wire(enc: PbEnc) -> u8 {
        scalar_wire::<u8>(enc)
    }
    fn pb_zero() -> Self {
        0
    }
    fn pb_absent(&self, _enc: PbEnc) -> bool {
        *self == 0
    }
    fn pb_payload_size(&self, enc: PbEnc) -> usize {
        scalar_size(*self, enc)
    }
    fn pb_write_payload(&self, enc: PbEnc, w: &mut PbWriter) {
        scalar_write(*self, enc, w)
    }
    fn pb_read_payload(r: &mut PbReader, end: usize, enc: PbEnc, out: Option<&mut Self>) -> Read {
        scalar_read(r, end, enc, out)
    }
    fn pb_vec_size(v: &[u8], number: u32, _enc: PbEnc) -> usize {
        if v.is_empty() {
            0
        } else {
            tag_size(number) + varint_size(v.len() as u64) + v.len()
        }
    }
    fn pb_vec_write(v: &[u8], number: u32, _enc: PbEnc, w: &mut PbWriter) {
        if !v.is_empty() {
            w.bytes(v);
            w.varint(v.len() as u64);
            w.tag(number, WIRE_LEN);
        }
    }
    fn pb_vec_read(r: &mut PbReader, end: usize, wire: u8, enc: PbEnc, out: Option<&mut Vec<u8>>) -> Read<bool> {
        single_read(r, end, wire, enc, out)
    }
}
single_field!(u8);

impl PbSingle for String {
    fn pb_zero() -> Self {
        String::new()
    }
    fn pb_absent(&self, _enc: PbEnc) -> bool {
        self.is_empty()
    }
    fn pb_payload_size(&self, _enc: PbEnc) -> usize {
        varint_size(self.len() as u64) + self.len()
    }
    fn pb_write_payload(&self, _enc: PbEnc, w: &mut PbWriter) {
        w.bytes(self.as_bytes());
        w.varint(self.len() as u64);
    }
    fn pb_read_payload(r: &mut PbReader, end: usize, _enc: PbEnc, out: Option<&mut Self>) -> Read {
        let n = r.length(end)?;
        let Ok(text) = std::str::from_utf8(&r.bytes[r.p..r.p + n]) else {
            return r.fail("protobuf: a string is not UTF-8");
        };
        debug_assert!(valid_utf8(text.as_bytes()));
        if let Some(out) = out {
            out.clear();
            out.push_str(text);
        }
        r.p += n;
        Ok(())
    }
}
single_field!(String);

impl PbSingle for Vec<u8> {
    fn pb_zero() -> Self {
        Vec::new()
    }
    fn pb_absent(&self, _enc: PbEnc) -> bool {
        self.is_empty()
    }
    fn pb_payload_size(&self, _enc: PbEnc) -> usize {
        varint_size(self.len() as u64) + self.len()
    }
    fn pb_write_payload(&self, _enc: PbEnc, w: &mut PbWriter) {
        w.bytes(self);
        w.varint(self.len() as u64);
    }
    fn pb_read_payload(r: &mut PbReader, end: usize, _enc: PbEnc, out: Option<&mut Self>) -> Read {
        let n = r.length(end)?;
        let data = r.take(n);
        if let Some(out) = out {
            out.clear();
            out.extend_from_slice(data);
        }
        Ok(())
    }
}

impl<const N: usize> PbSingle for [u8; N] {
    fn pb_zero() -> Self {
        [0; N]
    }
    fn pb_absent(&self, _enc: PbEnc) -> bool {
        self.iter().all(|b| *b == 0)
    }
    fn pb_payload_size(&self, _enc: PbEnc) -> usize {
        varint_size(N as u64) + N
    }
    fn pb_write_payload(&self, _enc: PbEnc, w: &mut PbWriter) {
        w.bytes(self);
        w.varint(N as u64);
    }
    fn pb_read_payload(r: &mut PbReader, end: usize, _enc: PbEnc, out: Option<&mut Self>) -> Read {
        let n = r.length(end)?;
        if n != N {
            return r.fail("protobuf: bytes of a fixed size have another length");
        }
        let data = r.take(n);
        if let Some(out) = out {
            out.copy_from_slice(data);
        }
        Ok(())
    }
}
impl<const N: usize> PbField for [u8; N] {
    fn pb_new() -> Self {
        [0; N]
    }
    fn pb_size(&self, number: u32, enc: PbEnc) -> usize {
        single_size(self, number, enc)
    }
    fn pb_write(&self, number: u32, enc: PbEnc, w: &mut PbWriter) {
        single_write(self, number, enc, w)
    }
    fn pb_read(r: &mut PbReader, end: usize, _slot: u32, wire: u8, enc: PbEnc, out: Option<&mut Self>) -> Read<bool> {
        single_read(r, end, wire, enc, out)
    }
}

// ── A member ────────────────────────────────────────────────────────────────

/// What a member of a message may be. Implemented for the standard types
/// and by `#[derive(Protobuf)]`.
pub trait PbField: Sized {
    /// The field numbers the member takes (a oneof: one per alternative).
    const SLOTS: u32 = 1;
    /// The integer `pb_fixed` / `pb_sint` speak of: 0 none, 1 unsigned, 2
    /// signed (the member, or what an `Option` / list / set of it holds).
    const INTEGER: u8 = 0;
    /// The value of an absent member.
    fn pb_new() -> Self;
    fn pb_size(&self, number: u32, enc: PbEnc) -> usize;
    fn pb_write(&self, number: u32, enc: PbEnc, w: &mut PbWriter);
    /// Reads the field of number (member's number + `slot`). `false`: the
    /// wire type is not the member's (the field is skipped).
    fn pb_read(r: &mut PbReader, end: usize, slot: u32, wire: u8, enc: PbEnc, out: Option<&mut Self>) -> Read<bool>;

    /// Whether the type, or anything a value of it holds, declares
    /// something a value can violate (`psio::contract`).
    #[doc(hidden)]
    fn pb_declares(_seen: &mut crate::contract::Seen) -> bool {
        false
    }
    /// What the type declares about its values, on `self`.
    #[doc(hidden)]
    fn pb_check_declared(&self) -> Result<(), &'static str> {
        Ok(())
    }
}

impl<X: PbSingle> PbField for Option<X> {
    const INTEGER: u8 = X::INTEGER;
    fn pb_new() -> Self {
        None
    }
    fn pb_size(&self, number: u32, enc: PbEnc) -> usize {
        match self {
            Some(v) => tag_size(number) + v.pb_payload_size(enc),
            None => 0,
        }
    }
    fn pb_write(&self, number: u32, enc: PbEnc, w: &mut PbWriter) {
        if let Some(v) = self {
            v.pb_write_payload(enc, w);
            w.tag(number, X::pb_wire(enc));
        }
    }
    fn pb_read(r: &mut PbReader, end: usize, _slot: u32, wire: u8, enc: PbEnc, out: Option<&mut Self>) -> Read<bool> {
        if wire != X::pb_wire(enc) {
            return Ok(false);
        }
        X::pb_read_payload(r, end, enc, out.map(|o| o.get_or_insert_with(X::pb_zero)))?;
        Ok(true)
    }
    fn pb_declares(seen: &mut crate::contract::Seen) -> bool {
        X::pb_declares(seen)
    }
    fn pb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(X::pb_check_declared)
    }
}

impl<X: PbSingle> PbField for Vec<X> {
    const INTEGER: u8 = X::INTEGER;
    fn pb_new() -> Self {
        Vec::new()
    }
    fn pb_size(&self, number: u32, enc: PbEnc) -> usize {
        X::pb_vec_size(self, number, enc)
    }
    fn pb_write(&self, number: u32, enc: PbEnc, w: &mut PbWriter) {
        X::pb_vec_write(self, number, enc, w)
    }
    fn pb_read(r: &mut PbReader, end: usize, _slot: u32, wire: u8, enc: PbEnc, out: Option<&mut Self>) -> Read<bool> {
        X::pb_vec_read(r, end, wire, enc, out)
    }
    fn pb_declares(seen: &mut crate::contract::Seen) -> bool {
        X::pb_declares(seen)
    }
    fn pb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(X::pb_check_declared)
    }
}

impl<X: PbSingle + Ord> PbField for BTreeSet<X> {
    const INTEGER: u8 = X::INTEGER;
    fn pb_new() -> Self {
        BTreeSet::new()
    }
    fn pb_size(&self, number: u32, enc: PbEnc) -> usize {
        repeated_size(self.iter(), self.len(), number, enc)
    }
    fn pb_write(&self, number: u32, enc: PbEnc, w: &mut PbWriter) {
        repeated_write(self.iter().rev(), self.is_empty(), number, enc, w)
    }
    fn pb_read(r: &mut PbReader, end: usize, _slot: u32, wire: u8, enc: PbEnc, out: Option<&mut Self>) -> Read<bool> {
        repeated_read(
            r,
            end,
            wire,
            enc,
            out,
            |s: &mut BTreeSet<X>, x| {
                s.insert(x);
            },
            |_, _| (),
        )
    }
    fn pb_declares(seen: &mut crate::contract::Seen) -> bool {
        X::pb_declares(seen)
    }
    fn pb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(X::pb_check_declared)
    }
}

/// A map's key: an integer, a bool or a string.
pub trait PbMapKey: PbSingle + Ord {}
impl PbMapKey for bool {}
impl PbMapKey for i8 {}
impl PbMapKey for i16 {}
impl PbMapKey for i32 {}
impl PbMapKey for i64 {}
impl PbMapKey for u16 {}
impl PbMapKey for u32 {}
impl PbMapKey for u64 {}
impl PbMapKey for String {}

impl<K: PbMapKey, V: PbSingle> PbField for BTreeMap<K, V> {
    fn pb_new() -> Self {
        BTreeMap::new()
    }
    fn pb_size(&self, number: u32, _enc: PbEnc) -> usize {
        let tag = tag_size(number);
        self.iter()
            .map(|(k, v)| {
                let entry = 2 + k.pb_payload_size(PbEnc::Plain) + v.pb_payload_size(PbEnc::Plain);
                tag + varint_size(entry as u64) + entry
            })
            .sum()
    }
    fn pb_write(&self, number: u32, _enc: PbEnc, w: &mut PbWriter) {
        for (k, v) in self.iter().rev() {
            let end = w.position();
            v.pb_write_payload(PbEnc::Plain, w);
            w.tag(2, V::pb_wire(PbEnc::Plain));
            k.pb_write_payload(PbEnc::Plain, w);
            w.tag(1, K::pb_wire(PbEnc::Plain));
            w.close(end, number);
        }
    }
    fn pb_read(r: &mut PbReader, end: usize, _slot: u32, wire: u8, _enc: PbEnc, mut out: Option<&mut Self>) -> Read<bool> {
        if wire != WIRE_LEN {
            return Ok(false);
        }
        let n = r.length(end)?;
        r.enter()?;
        let sub = r.position() + n;
        let mut entry = out.as_ref().map(|_| (K::pb_zero(), V::pb_zero()));
        while r.more(sub) {
            let (number, w) = r.field(sub)?;
            if number == 1 && w == K::pb_wire(PbEnc::Plain) {
                K::pb_read_payload(r, sub, PbEnc::Plain, entry.as_mut().map(|e| &mut e.0))?;
            } else if number == 2 && w == V::pb_wire(PbEnc::Plain) {
                V::pb_read_payload(r, sub, PbEnc::Plain, entry.as_mut().map(|e| &mut e.1))?;
            } else {
                r.skip(sub, number, w)?;
            }
        }
        r.leave();
        if let (Some(out), Some((k, v))) = (out.as_deref_mut(), entry) {
            out.insert(k, v);
        }
        Ok(true)
    }
    fn pb_declares(seen: &mut crate::contract::Seen) -> bool {
        // (Both: `||` would leave the mapped type unentered.)
        let key = K::pb_declares(seen);
        V::pb_declares(seen) || key
    }
    fn pb_check_declared(&self) -> Result<(), &'static str> {
        self.iter().try_for_each(|(key, value)| {
            key.pb_check_declared()?;
            value.pb_check_declared()
        })
    }
}

// ── Messages ────────────────────────────────────────────────────────────────

/// A message: what `#[derive(Protobuf)]` implements for a struct.
pub trait PbMessage: Sized {
    /// Every member at its zero value.
    fn pb_message_zero() -> Self;
    /// The size of the fields (no length before them).
    fn pb_body_size(&self) -> usize;
    /// The fields, last first.
    fn pb_write_body(&self, w: &mut PbWriter);
    /// The fields up to `end`, merged into `out`.
    fn pb_read_body(r: &mut PbReader, end: usize, out: Option<&mut Self>) -> Read;

    /// Whether the message, or anything a value of it holds, declares
    /// something a value can violate: a bound or an order on a member
    /// (`#[psio(max = N)]`, `element(...)`, `sorted`, …) or an invariant.
    /// `seen`: the types this search has entered (a message may hold
    /// itself).
    fn pb_message_declares(_seen: &mut crate::contract::Seen) -> bool {
        false
    }
    /// [`pb_message_declares`](Self::pb_message_declares) for the message
    /// as a root; the derive keeps the answer.
    fn pb_has_declarations() -> bool {
        Self::pb_message_declares(&mut crate::contract::Seen::new())
    }
    /// What the message declares about its values, on `self`: the first
    /// violation (the text C++ `validate_protobuf` reports), or `Ok`. The
    /// readers call it on the finished value: a member may be written more
    /// than once (the last scalar wins, repeated members and messages
    /// merge), so nothing can be said field by field (C++
    /// `psio::check_value_contract`).
    fn pb_message_check_declared(&self) -> Result<(), &'static str> {
        Ok(())
    }
}

/// `PbSingle` of a message (the derive forwards to these).
#[doc(hidden)]
pub fn message_payload_size<M: PbMessage>(m: &M) -> usize {
    let n = m.pb_body_size();
    varint_size(n as u64) + n
}
#[doc(hidden)]
pub fn message_write_payload<M: PbMessage>(m: &M, w: &mut PbWriter) {
    let end = w.position();
    m.pb_write_body(w);
    w.varint((end - w.position()) as u64);
}
#[doc(hidden)]
pub fn message_read_payload<M: PbMessage>(r: &mut PbReader, end: usize, out: Option<&mut M>) -> Read {
    let n = r.length(end)?;
    r.enter()?;
    let sub = r.position() + n;
    M::pb_read_body(r, sub, out)?;
    r.leave();
    Ok(())
}
/// A message member that is not optional: not written when all zero.
#[doc(hidden)]
pub fn message_field_size<M: PbMessage>(m: &M, number: u32) -> usize {
    let n = m.pb_body_size();
    if n == 0 {
        0
    } else {
        tag_size(number) + varint_size(n as u64) + n
    }
}
#[doc(hidden)]
pub fn message_field_write<M: PbMessage>(m: &M, number: u32, w: &mut PbWriter) {
    let end = w.position();
    m.pb_write_body(w);
    if w.position() != end {
        w.close(end, number);
    }
}

fn read_root<M: PbMessage>(bytes: &[u8], out: Option<&mut M>, limits: PbLimits) -> Read {
    if bytes.len() > MAX_MESSAGE_SIZE {
        return Err(PbError { message: "protobuf: a message exceeds 2 GiB", offset: 0 });
    }
    let mut r = PbReader::new(bytes, limits.max_depth);
    M::pb_read_body(&mut r, bytes.len(), out)
}

// ── The helper names (docs/helpers.md) ──────────────────────────────────────

/// The format tag (C++ `psio::protobuf`).
pub struct ProtobufFormat;
impl crate::Format for ProtobufFormat {
    type Error = PbError;
}
impl<T: PbMessage> crate::Encode<ProtobufFormat> for T {
    fn encode_into(&self, out: &mut Vec<u8>) -> Result<usize, PbError> {
        let before = out.len();
        to_protobuf_into(self, out);
        Ok(out.len() - before)
    }
    fn encoded_size(&self) -> Result<usize, PbError> {
        Ok(self.pb_body_size())
    }
}
impl<T: PbMessage> crate::Decode<ProtobufFormat> for T {
    fn decode(bytes: &[u8]) -> Result<Self, PbError> {
        from_protobuf(bytes)
    }
}
impl<T: PbMessage> crate::Validate<ProtobufFormat> for T {
    fn validate<P: crate::ValidationPolicy>(bytes: &[u8], _policy: &P) -> Result<(), PbError> {
        validate_protobuf::<T>(bytes)
    }
}

/// Encodes `value` into a fresh buffer (C++ `psio::to_protobuf(v)`).
pub fn to_protobuf<T: PbMessage>(value: &T) -> Vec<u8> {
    let mut out = Vec::new();
    to_protobuf_into(value, &mut out);
    out
}
/// The validated write (C++ `psio::to_protobuf_checked(v)`): the encoding of
/// `value`, or the declaration it violates — what [`validate_protobuf`]
/// would refuse the bytes for. [`to_protobuf`] is the fast write: it holds
/// nothing against what the type declares.
pub fn try_to_protobuf<T: PbMessage>(value: &T) -> Result<Vec<u8>, PbError> {
    if T::pb_has_declarations() {
        value.pb_message_check_declared().map_err(|message| PbError { message, offset: 0 })?;
    }
    Ok(to_protobuf(value))
}
/// Appends the encoding of `value` to `out` (C++ `psio::to_protobuf(v, sink)`).
///
/// Panics when the message exceeds 2 GiB - 1 (protobuf's limit).
pub fn to_protobuf_into<T: PbMessage>(value: &T, out: &mut Vec<u8>) {
    let total = value.pb_body_size();
    assert!(total <= MAX_MESSAGE_SIZE, "protobuf: a message exceeds 2 GiB");
    let before = out.len();
    out.resize(before + total, 0);
    let mut w = PbWriter::new(&mut out[before..]);
    value.pb_write_body(&mut w);
    assert!(!w.overrun && w.p == 0, "protobuf: the size of a value is not the size written");
}
/// The checked decode, in one pass (C++ `psio::from_protobuf<T>(bytes)`).
/// An absent member is its zero value.
pub fn from_protobuf<T: PbMessage>(bytes: &[u8]) -> Result<T, PbError> {
    from_protobuf_with(bytes, PbLimits::default())
}
/// The checked decode under other limits.
pub fn from_protobuf_with<T: PbMessage>(bytes: &[u8], limits: PbLimits) -> Result<T, PbError> {
    let mut out = T::pb_message_zero();
    read_root(bytes, Some(&mut out), limits)?;
    if T::pb_has_declarations() {
        out.pb_message_check_declared().map_err(|message| PbError { message, offset: 0 })?;
    }
    Ok(out)
}
/// Whether `bytes` is one `T` (C++ `psio::validate_protobuf<T>(bytes)`):
/// the reader of [`from_protobuf`], storing nothing. Safe on arbitrary
/// bytes: it never panics, and its work is bounded by the input.
pub fn validate_protobuf<T: PbMessage>(bytes: &[u8]) -> Result<(), PbError> {
    validate_protobuf_with::<T>(bytes, PbLimits::default())
}
/// The same under other limits (C++ `psio::validate_protobuf<T>(bytes, limits)`).
///
/// A message that declares something about its values (a member's bound or
/// order, an invariant) is read into a value, which is then held against
/// the declarations; any other message is read storing nothing.
pub fn validate_protobuf_with<T: PbMessage>(bytes: &[u8], limits: PbLimits) -> Result<(), PbError> {
    if T::pb_has_declarations() {
        from_protobuf_with::<T>(bytes, limits).map(drop)
    } else {
        read_root::<T>(bytes, None, limits)
    }
}
/// Size of the encoding of `value` (C++ `psio::protobuf_size(v)`).
pub fn protobuf_size<T: PbMessage>(value: &T) -> usize {
    value.pb_body_size()
}
