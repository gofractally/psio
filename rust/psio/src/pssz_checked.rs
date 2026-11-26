//! Bounds-checked layout helpers shared by the PSSZ codecs, the proc-macro
//! derives, the borrowed views and the schema-driven codec.
//!
//! Offset width is passed explicitly (`w`): it is a property of the logical
//! type that owns the header / offset slots (`PsszPack::OFFSET_WIDTH`), not of
//! the caller. Under the explicit `Pssz8/16/32` tags every type reports the
//! tag's width; under the automatic `Pssz` tag each type reports its own.
//!
//! The record frame parser is a port of C++
//! `psio::detail::pssz_record_spans` plus `record_has_unpruned_tail`: readers
//! honor the writer's fixed-region header, synthesize None for pruned trailing
//! optionals, skip unknown appended slots (which still delimit the last known
//! variable payload), and reject a stored trailing None.
//!
//! Nothing here uses unsafe code or raw memory copies, and no function panics
//! on untrusted input.
//!
//! The writers of the typed codecs ([`RecordEncoder`],
//! [`encode_sequence_at_depth`]) write a value once, where it belongs in the
//! destination: the fixed region is reserved, each payload is appended behind
//! it and its offset is written back into its slot. Nothing is encoded into a
//! buffer of its own. The depth of a node travels as an argument
//! (`PsszPack::pssz_pack_at_depth`); the thread-local counter is only read
//! where a hand-written codec joins in.
use crate::pssz::{PsszError, PsszFormat, PsszPack, PsszResult, PsszUnpack};
use std::ops::Range;

/// Deepest accepted node. The root is node 0; every present record field,
/// sequence element, `Some` payload and variant payload is one deeper. Matches
/// C++ `kMaxValidationDepth`.
pub const MAX_DEPTH: usize = 64;

/// What the readers of one thread share: one thread-local, so that a
/// top-level read reaches it once on the way in and once on the way out.
struct ReaderState {
    /// The depth of the node a reader called now would be.
    depth: std::cell::Cell<usize>,
    // Outside a session (a direct trait call) unknown optional suffix fields
    // are accepted, as C++ `decode` / `validate` do.
    allow_unknown: std::cell::Cell<bool>,
    saw_unknown: std::cell::Cell<bool>,
}
thread_local! {
    static READER: ReaderState = const {
        ReaderState {
            depth: std::cell::Cell::new(0),
            allow_unknown: std::cell::Cell::new(true),
            saw_unknown: std::cell::Cell::new(false),
        }
    };
}
thread_local! { static ENCODE_DEPTH: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }

/// Scope of one top-level decode or validation. `allow == false` is the exact
/// reader: an unknown optional suffix field anywhere in the tree is an error.
pub struct DecodeSession {
    previous_allow: bool,
    previous_unknown: bool,
}
impl DecodeSession {
    pub fn enter(allow: bool) -> Self {
        Self::enter_at(allow).0
    }
    /// [`enter`](Self::enter), and the depth of the node a reader called now
    /// would be ([`decode_depth`]).
    #[inline]
    pub fn enter_at(allow: bool) -> (Self, usize) {
        READER.with(|reader| {
            (
                Self {
                    previous_allow: reader.allow_unknown.replace(allow),
                    previous_unknown: reader.saw_unknown.replace(false),
                },
                reader.depth.get(),
            )
        })
    }
    pub fn has_unknown(&self) -> bool {
        READER.with(|reader| reader.saw_unknown.get())
    }
}
impl Drop for DecodeSession {
    #[inline]
    fn drop(&mut self) {
        READER.with(|reader| {
            reader.allow_unknown.set(self.previous_allow);
            reader.saw_unknown.set(self.previous_unknown || reader.saw_unknown.get());
        })
    }
}
#[cold]
fn unknown_fields() -> PsszResult<()> {
    READER.with(|reader| {
        reader.saw_unknown.set(true);
        if reader.allow_unknown.get() {
            Ok(())
        } else {
            Err(PsszError::new("pssz: unknown fields rejected by exact reader"))
        }
    })
}

/// Scope in which a checked reader runs inside a trusted read (a type
/// without a trusted reader of its own): unknown fields are accepted, as a
/// compatible reader does, and what the scope saw stays in it.
pub struct TrustedScope {
    allow: bool,
    unknown: bool,
}
impl TrustedScope {
    pub fn enter() -> Self {
        READER.with(|reader| Self { allow: reader.allow_unknown.replace(true), unknown: reader.saw_unknown.get() })
    }
}
impl Drop for TrustedScope {
    fn drop(&mut self) {
        READER.with(|reader| {
            reader.allow_unknown.set(self.allow);
            reader.saw_unknown.set(self.unknown);
        })
    }
}

/// The default `PsszUnpack::pssz_unpack_trusted`: the checked reader, in a
/// [`TrustedScope`]; bytes it refuses are a panic.
#[track_caller]
pub fn unpack_trusted_by_checking<F: PsszFormat, T: PsszUnpack<F>>(bytes: &[u8]) -> T {
    let result = {
        let _scope = TrustedScope::enter();
        // A trusted read holds nothing against what the type declares.
        let _undeclared = Undeclared::enter();
        T::pssz_unpack(bytes)
    };
    match result {
        Ok(value) => value,
        Err(error) => crate::pssz::trusted_bytes_invalid(error),
    }
}

/// The guard of a reader that reads its depth from the thread (a
/// hand-written codec): the readers in this crate and the derives are handed
/// their depth (`PsszUnpack::pssz_unpack_at_depth`).
pub struct DecodeDepth;
impl DecodeDepth {
    #[inline]
    pub fn enter() -> PsszResult<Self> {
        READER.with(|reader| {
            // Root is node 0. Node 64 is allowed; a child at 65 is rejected.
            let depth = reader.depth.get();
            if depth > MAX_DEPTH {
                return Err(PsszError::new("pssz: max depth exceeded"));
            }
            reader.depth.set(depth + 1);
            Ok(Self)
        })
    }
}
impl Drop for DecodeDepth {
    #[inline]
    fn drop(&mut self) {
        READER.with(|reader| reader.depth.set(reader.depth.get().saturating_sub(1)));
    }
}

/// [`DecodeDepth::enter`] for a node that holds no other (a number, a
/// string, a bit type): the same check, with nothing to count and undo.
#[inline]
pub fn decode_leaf() -> PsszResult<()> {
    decode_node(decode_depth())
}

/// The check [`DecodeDepth::enter`] makes, for a reader that is handed the
/// depth of its node: node `depth` exists iff `depth <= MAX_DEPTH`.
#[inline(always)]
pub fn decode_node(depth: usize) -> PsszResult<()> {
    if depth > MAX_DEPTH {
        Err(DEPTH_EXCEEDED)
    } else {
        Ok(())
    }
}

/// The depth of the node a reader called now would be: 0 at the root, and
/// inside a hand-written codec that entered [`DecodeDepth`] the depth of its
/// children.
#[inline]
pub fn decode_depth() -> usize {
    READER.with(|reader| reader.depth.get())
}

/// Scope in which the thread's decode depth is `depth`: how a reader that is
/// handed its depth calls one that reads the counter (the default
/// `PsszUnpack::pssz_unpack_at_depth`, for a hand-written codec).
pub struct DecodeDepthAt(usize);
impl DecodeDepthAt {
    #[inline]
    pub fn enter(depth: usize) -> Self {
        Self(READER.with(|reader| reader.depth.replace(depth)))
    }
}
impl Drop for DecodeDepthAt {
    #[inline]
    fn drop(&mut self) {
        READER.with(|reader| reader.depth.set(self.0));
    }
}

/// Checked encoding uses the same node budget as the checked readers. Wire-
/// transparent wrappers must forward without creating another guard.
pub struct EncodeDepth;
impl EncodeDepth {
    pub fn enter() -> PsszResult<Self> {
        ENCODE_DEPTH.with(|depth| {
            if depth.get() > MAX_DEPTH {
                return Err(PsszError::new("pssz: max depth exceeded"));
            }
            depth.set(depth.get() + 1);
            Ok(Self)
        })
    }
}
impl Drop for EncodeDepth {
    fn drop(&mut self) {
        ENCODE_DEPTH.with(|depth| depth.set(depth.get().saturating_sub(1)));
    }
}

const DEPTH_EXCEEDED: PsszError = PsszError::new("pssz: max depth exceeded");

/// The check [`EncodeDepth::enter`] makes, for a writer that is handed the
/// depth of its node: node `depth` exists iff `depth <= MAX_DEPTH`.
#[inline(always)]
pub fn encode_node(depth: usize) -> PsszResult<()> {
    if depth > MAX_DEPTH {
        Err(DEPTH_EXCEEDED)
    } else {
        Ok(())
    }
}

/// The depth of the node an encoder called now would be: 0 at the root, and
/// inside a hand-written codec's `pssz_pack_checked` the depth of its
/// children.
#[inline]
pub fn encode_depth() -> usize {
    ENCODE_DEPTH.with(|depth| depth.get())
}

/// Scope in which the thread-local encode depth is `depth`: how a writer
/// that is handed its depth calls one that reads the counter (the default
/// `PsszPack::pssz_pack_at_depth`, for a hand-written codec).
pub struct EncodeDepthAt(usize);
impl EncodeDepthAt {
    #[inline]
    pub fn enter(depth: usize) -> Self {
        Self(ENCODE_DEPTH.with(|current| current.replace(depth)))
    }
}
impl Drop for EncodeDepthAt {
    #[inline]
    fn drop(&mut self) {
        ENCODE_DEPTH.with(|current| current.set(self.0));
    }
}

/// `pssz_pack_checked` of a codec that implements `pssz_pack_at_depth`: the
/// depth is the thread's, and on error `out` is left as it was.
#[inline]
pub fn pack_checked<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T, out: &mut Vec<u8>) -> PsszResult<()> {
    let start = out.len();
    let result = value.pssz_pack_at_depth(out, encode_depth());
    if result.is_err() {
        out.truncate(start);
    }
    result
}

/// The most an encoder reserves on the word of `PsszPack::pssz_size_hint`,
/// which is a safe method: a wrong hint costs at most this much memory.
pub const SIZE_HINT_LIMIT: usize = 1 << 30;

/// Room for the whole value where its size is known ahead: one allocation.
/// The hint is not trusted, and failing to reserve it is not an error.
#[inline]
pub fn reserve_hint<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T, out: &mut Vec<u8>) {
    let hint = value.pssz_size_hint();
    if hint > out.capacity() - out.len() && hint <= SIZE_HINT_LIMIT {
        let _ = out.try_reserve_exact(hint);
    }
}

/// `pssz_size` of a type whose size is that of its encoding: the value is
/// encoded and measured, so a value that has no encoding is the error.
pub fn encoded_len<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> PsszResult<usize> {
    let mut out = Vec::new();
    reserve_hint::<F, T>(value, &mut out);
    value.pssz_pack_at_depth(&mut out, encode_depth())?;
    Ok(out.len())
}

/// `pssz_pack_fixed` of a codec that only appends: the value is encoded on
/// its own and copied into its slot. The length is checked, as [`field`]
/// does for a member of a record.
pub fn pack_fixed_by_append<F: PsszFormat, T: PsszPack<F> + ?Sized>(
    value: &T,
    slot: &mut [u8],
    depth: usize,
) -> PsszResult<()> {
    let mut bytes = Vec::new();
    value.pssz_pack_at_depth(&mut bytes, depth)?;
    if bytes.len() != slot.len() {
        return Err(PsszError::new("pssz: fixed encoder size mismatch"));
    }
    slot.copy_from_slice(&bytes);
    Ok(())
}

const ALLOCATION: PsszError = PsszError::new("pssz: output allocation overflow");
const OFFSET_OVERFLOW: PsszError = PsszError::new("pssz: value exceeds the static offset width");

/// Room for `additional` more bytes, as an error instead of an abort.
#[inline(always)]
pub fn reserve(out: &mut Vec<u8>, additional: usize) -> PsszResult<()> {
    if out.capacity() - out.len() < additional {
        reserve_slow(out, additional)
    } else {
        Ok(())
    }
}
#[cold]
#[inline(never)]
fn reserve_slow(out: &mut Vec<u8>, additional: usize) -> PsszResult<()> {
    out.try_reserve(additional).map_err(|_| ALLOCATION)
}

/// Appends `count` zero bytes: a fixed region or an offset table, before it
/// is filled in. A small region (every one whose size is a constant of the
/// type) is a copy of constant length, which the compiler turns into a few
/// stores; `Vec::resize` is a call to `memset`.
#[inline(always)]
pub fn append_zeros(out: &mut Vec<u8>, count: usize) -> PsszResult<()> {
    const ZEROS: [u8; 128] = [0; 128];
    reserve(out, count)?;
    match ZEROS.get(..count) {
        Some(zeros) => out.extend_from_slice(zeros),
        None => out.resize(out.len() + count, 0),
    }
    Ok(())
}

/// Appends `bytes` (a leaf's whole encoding).
#[inline(always)]
pub fn append(out: &mut Vec<u8>, bytes: &[u8]) -> PsszResult<()> {
    reserve(out, bytes.len())?;
    out.extend_from_slice(bytes);
    Ok(())
}

/// The largest value an offset or header `w` bytes wide holds.
#[inline(always)]
const fn offset_limit(w: usize) -> usize {
    match w {
        1 => 0xff,
        2 => 0xffff,
        _ => 0xffff_ffff,
    }
}

/// Stores the low `w` bytes of `value` at `at`. The caller compares the
/// value with [`offset_limit`]: at once (a sequence) or, for the largest one,
/// when the frame is complete (a record). A slot that is not there (a
/// hand-written child took bytes away) is left to that check.
#[inline(always)]
fn store_offset(out: &mut [u8], at: usize, w: usize, value: usize) {
    match w {
        1 => {
            if let Some(slot) = out.get_mut(at) {
                *slot = value as u8;
            }
        }
        2 => {
            if let Some(slot) = out.get_mut(at..at.wrapping_add(2)) {
                slot.copy_from_slice(&(value as u16).to_le_bytes());
            }
        }
        4 => {
            if let Some(slot) = out.get_mut(at..at.wrapping_add(4)) {
                slot.copy_from_slice(&(value as u32).to_le_bytes());
            }
        }
        _ => {}
    }
}

const SHRUNK: PsszError = PsszError::new("pssz: an encoder removed bytes from the output");

// ── Invariants ──────────────────────────────────────────────────────────────

thread_local! { static WRITE_UNCHECKED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) }; }

/// Scope in which the generated writers do not run type invariants: the
/// bytes of a value whose invariant is someone else's to report (a custom
/// handler's `from_text` produces the wire bytes; its `check` then says
/// whether they are a value of the type, with the invariant's own message).
/// Readers are unaffected.
pub struct WriteUnchecked(bool);
impl WriteUnchecked {
    pub fn enter() -> Self {
        Self(WRITE_UNCHECKED.with(|flag| flag.replace(true)))
    }
}
impl Drop for WriteUnchecked {
    fn drop(&mut self) {
        WRITE_UNCHECKED.with(|flag| flag.set(self.0));
    }
}
/// Whether a writer runs the invariant of the value it is writing.
pub fn writer_checks_invariants() -> bool {
    !WRITE_UNCHECKED.with(|flag| flag.get()) && declared()
}

// ── The fast write and the trusted read ─────────────────────────────────────

thread_local! { static UNDECLARED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) }; }

/// Scope of a FAST write (`to_pssz`, `encode`) or of a TRUSTED read of a type
/// that has no reader of its own for it (`from_pssz_trusted`): nothing a type
/// *declares* is held against the value — no bound of a `BoundedString` /
/// `BoundedList` / `BoundedMap` / `Nested` or of a `#[psio(max = N)]`
/// member, no `sorted` / `unique` / `unique_keys`, no bitlist capacity, no
/// invariant. What the format itself needs stays checked: an offset that
/// does not fit its width, the depth limit, a `String` that is not UTF-8.
/// (C++ `encode_preflight::fast()`; docs/annotations.md 7.2.)
pub struct Undeclared(bool);
impl Undeclared {
    pub fn enter() -> Self {
        Self(UNDECLARED.with(|flag| flag.replace(true)))
    }
}
impl Drop for Undeclared {
    fn drop(&mut self) {
        UNDECLARED.with(|flag| flag.set(self.0));
    }
}
/// Scope of an explicit VALIDATING read or VALIDATED write (`from_pssz`,
/// `validate_pssz`, `try_to_pssz`, and the key / JSON equivalents): what the
/// type declares is checked whatever the caller is in the middle of. A fast
/// write or a trusted read in progress — the scope a hand-written codec, a
/// text handler or an invariant runs in — does not reach into a validation
/// it asks for; the enclosing mode comes back when this scope ends.
pub struct Declared(bool);
impl Declared {
    pub fn enter() -> Self {
        Self(UNDECLARED.with(|flag| flag.replace(false)))
    }
}
impl Drop for Declared {
    fn drop(&mut self) {
        UNDECLARED.with(|flag| flag.set(self.0));
    }
}
/// Whether the codec holds a value against what its type declares: true
/// except inside an [`Undeclared`] scope.
#[inline]
pub fn declared() -> bool {
    !UNDECLARED.with(|flag| flag.get())
}

// ── Formats and offsets ─────────────────────────────────────────────────────

pub fn format<F: PsszFormat>() -> PsszResult<()> {
    if !matches!(F::OFFSET_BYTES, 1 | 2 | 4) || F::HEADER_BYTES != F::OFFSET_BYTES {
        return Err(PsszError::new("pssz: unsupported offset/header width"));
    }
    Ok(())
}

#[inline]
fn width(w: usize) -> PsszResult<()> {
    if matches!(w, 1 | 2 | 4) {
        Ok(())
    } else {
        Err(PsszError::new("pssz: unsupported offset/header width"))
    }
}

/// C++ `wire_span_fits`: a variable payload is addressed by at most 32 bits.
#[inline]
pub fn wire_span(len: usize) -> PsszResult<()> {
    if len as u128 > u32::MAX as u128 {
        return Err(PsszError::new(
            "pssz: variable payload exceeds 32-bit address space",
        ));
    }
    Ok(())
}

#[inline]
pub fn read_offset_w(bytes: &[u8], w: usize) -> PsszResult<usize> {
    let b = bytes.get(..w).ok_or(PsszError::new("pssz: offset underrun"))?;
    Ok(match w {
        1 => b[0] as usize,
        2 => u16::from_le_bytes([b[0], b[1]]) as usize,
        4 => u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize,
        _ => return Err(PsszError::new("pssz: unsupported offset/header width")),
    })
}

pub fn write_offset_w(bytes: &mut [u8], w: usize, value: usize) -> PsszResult<()> {
    if bytes.len() != w {
        return Err(PsszError::new("pssz: offset width overflow"));
    }
    const OVERFLOW: PsszError = PsszError::new("pssz: value exceeds the static offset width");
    match w {
        1 => bytes.copy_from_slice(&u8::try_from(value).map_err(|_| OVERFLOW)?.to_le_bytes()),
        2 => bytes.copy_from_slice(&u16::try_from(value).map_err(|_| OVERFLOW)?.to_le_bytes()),
        4 => bytes.copy_from_slice(&u32::try_from(value).map_err(|_| OVERFLOW)?.to_le_bytes()),
        _ => return Err(PsszError::new("pssz: unsupported offset/header width")),
    }
    Ok(())
}

/// Explicit-profile conveniences (the schema-driven codec and fixtures).
pub fn read_offset<F: PsszFormat>(bytes: &[u8]) -> PsszResult<usize> {
    format::<F>()?;
    read_offset_w(bytes, F::OFFSET_BYTES)
}
pub fn write_offset<F: PsszFormat>(bytes: &mut [u8], value: usize) -> PsszResult<()> {
    format::<F>()?;
    write_offset_w(bytes, F::OFFSET_BYTES, value)
}

/// `maxDynamicData` check, shared by encode, decode and validate. Like C++
/// it compares the complete encoded size of the value against the cap.
#[inline]
pub fn check_cap(cap: Option<usize>, len: usize) -> PsszResult<()> {
    match cap {
        Some(cap) if len > cap => Err(PsszError::new(
            "pssz: encoded size exceeds maxDynamicData cap",
        )),
        _ => Ok(()),
    }
}

// ── Record layout ───────────────────────────────────────────────────────────

#[derive(Clone, Copy, Debug)]
pub struct Layout {
    /// Fixed wire size: stored inline in the fixed region.
    pub fixed: bool,
    pub size: usize,
    /// A plain `Option`: may be pruned / missing as a trailing field.
    pub optional: bool,
    /// The optional uses the one-byte selector form (payload can be empty).
    pub selector: bool,
    /// Encoder only: this value is None.
    pub omit: bool,
}

impl Layout {
    pub const fn of<F: PsszFormat, T: PsszPack<F> + ?Sized>() -> Self {
        Self {
            fixed: T::IS_FIXED_SIZE,
            size: T::FIXED_SIZE,
            optional: T::IS_OPTIONAL,
            selector: T::OPTIONAL_SELECTOR,
            omit: false,
        }
    }
    /// Bytes this field occupies in a fixed region whose offsets are `w` wide.
    pub const fn slot(self, w: usize) -> usize {
        if self.fixed {
            self.size
        } else {
            w
        }
    }
}

/// Size of the complete fixed region of a record whose slots are `w` wide.
pub const fn fixed_region(w: usize, layouts: &[Layout]) -> usize {
    let mut total = 0usize;
    let mut at = 0;
    while at < layouts.len() {
        total = total.saturating_add(layouts[at].slot(w));
        at += 1;
    }
    total
}

/// C++ `inferred_bound` for a record or tuple (docs/pssz-spec.md 3.2):
/// the header (unless DWNC) and one slot per field that is not stored
/// inline, at the record's own width (`fit`), plus each field's bound.
pub const fn record_bound(fields: &[(Option<usize>, bool)], dwnc: bool) -> Option<usize> {
    let mut words: usize = if dwnc { 0 } else { 1 };
    let mut total: usize = 0;
    let mut at = 0;
    while at < fields.len() {
        let (bound, fixed) = fields[at];
        let bound = match bound {
            Some(b) => b,
            None => return None,
        };
        if !fixed {
            words += 1;
        }
        total = match total.checked_add(bound) {
            Some(t) => t,
            None => return None,
        };
        at += 1;
    }
    crate::pssz::fit(words, Some(total))
}

/// C++ `inferred_bound` for `count` elements (array, bounded list, the
/// entries of a bounded map or set): the elements and, when they are not
/// fixed-size, the offset table of `count` slots at the sequence's own
/// width (`fit`).
pub const fn sequence_bound(count: usize, element: Option<usize>, fixed: bool) -> Option<usize> {
    if count == 0 {
        return Some(0);
    }
    let element = match element {
        Some(e) => e,
        None => return None,
    };
    let payload = match count.checked_mul(element) {
        Some(p) => p,
        None => return None,
    };
    if fixed {
        Some(payload)
    } else {
        crate::pssz::fit(count, Some(payload))
    }
}

/// The maximum encoded size of a record or tuple (docs/pssz-spec.md 3.7):
/// the header (unless DWNC) and one slot per field that is not stored
/// inline, at `width` — the width the record really has —, plus each
/// field's own maximum; never more than `cap`, and `cap` when a field is
/// unbounded.
pub const fn record_max(
    fields: &[(Option<usize>, bool)],
    dwnc: bool,
    width: usize,
    cap: Option<usize>,
) -> Option<usize> {
    let mut words: usize = if dwnc { 0 } else { 1 };
    let mut total: Option<usize> = Some(0);
    let mut at = 0;
    while at < fields.len() {
        let (bound, fixed) = fields[at];
        if !fixed {
            words += 1;
        }
        total = match (total, bound) {
            (Some(t), Some(b)) => t.checked_add(b),
            _ => None,
        };
        at += 1;
    }
    let sum = match (words.checked_mul(width), total) {
        (Some(w), Some(t)) => w.checked_add(t),
        _ => None,
    };
    match (cap, sum) {
        (Some(cap), Some(sum)) => Some(if sum < cap { sum } else { cap }),
        (Some(cap), None) => Some(cap),
        (None, sum) => sum,
    }
}

/// The maximum encoded size of `count` elements (docs/pssz-spec.md 3.7):
/// each at its own maximum and, when they are not fixed-size, the offset
/// table of `count` slots at `width`, the width the sequence really has.
pub const fn sequence_max(count: usize, element: Option<usize>, fixed: bool, width: usize) -> Option<usize> {
    if count == 0 {
        return Some(0);
    }
    let payload = match element {
        Some(e) => match count.checked_mul(e) {
            Some(p) => p,
            None => return None,
        },
        None => return None,
    };
    if fixed {
        return Some(payload);
    }
    match count.checked_mul(width) {
        Some(table) => table.checked_add(payload),
        None => None,
    }
}

/// The largest of `widths` (0 for none): `PsszPack::RECORD_WIDTH_BENEATH`
/// of a type that holds several.
pub const fn widest(widths: &[usize]) -> usize {
    let mut out = 0;
    let mut at = 0;
    while at < widths.len() {
        if widths[at] > out {
            out = widths[at];
        }
        at += 1;
    }
    out
}

pub const fn bound_add(bound: Option<usize>, extra: usize) -> Option<usize> {
    match bound {
        Some(b) => b.checked_add(extra),
        None => None,
    }
}

fn span_is_none(bytes: &[u8], span: &Range<usize>, selector: bool) -> bool {
    span.is_empty() || (selector && bytes.get(span.start) == Some(&0))
}

/// Resolve a record frame. `spans[i]` is `None` for a trailing optional the
/// writer pruned. Returns whether the writer appended slots this reader does
/// not know. Does not allocate.
#[inline]
pub fn record_spans_into(
    w: usize,
    layouts: &[Layout],
    bytes: &[u8],
    dwnc: bool,
    spans: &mut [Option<Range<usize>>],
) -> PsszResult<bool> {
    width(w)?;
    if layouts.len() != spans.len() {
        return Err(PsszError::new("pssz: invalid record span"));
    }
    for span in spans.iter_mut() {
        *span = None;
    }
    let mut known = 0usize;
    for layout in layouts {
        if layout.optional && layout.fixed {
            return Err(PsszError::new("pssz: invalid reader field layout"));
        }
        known = known
            .checked_add(layout.slot(w))
            .ok_or(PsszError::new("pssz: invalid reader field layout"))?;
    }
    let mut fixed_start = 0usize;
    let mut fixed_size = known;
    if !dwnc {
        if bytes.len() < w {
            return Err(PsszError::new("pssz: record header truncated"));
        }
        fixed_size = read_offset_w(bytes, w)?;
        fixed_start = w;
    }
    let payload_size = bytes.len() - fixed_start;
    if fixed_size > payload_size {
        return Err(PsszError::new("pssz: record fixed region truncated"));
    }
    if fixed_size > known && (fixed_size - known) % w != 0 {
        return Err(PsszError::new("pssz: partial unknown optional slot"));
    }
    let unknown = fixed_size > known;
    if unknown || !(dwnc && layouts.iter().all(|l| l.fixed)) {
        wire_span(payload_size)?;
    }

    let mut previous_field: Option<usize> = None;
    let mut previous_offset = fixed_size;
    let mut seen_variable = false;
    let mut accept = |offset: usize,
                      index: Option<usize>,
                      spans: &mut [Option<Range<usize>>]|
     -> PsszResult<()> {
        if offset > payload_size
            || offset < previous_offset
            || (!seen_variable && offset != fixed_size)
        {
            return Err(PsszError::new("pssz: invalid record payload offset"));
        }
        if let Some(previous) = previous_field {
            if let Some(span) = spans[previous].as_mut() {
                span.end = fixed_start + offset;
            }
        }
        if let Some(index) = index {
            spans[index] = Some(fixed_start + offset..bytes.len());
        }
        previous_field = index;
        previous_offset = offset;
        seen_variable = true;
        Ok(())
    };

    let mut cursor = 0usize;
    let mut missing = false;
    let mut last_present: Option<usize> = None;
    for (i, layout) in layouts.iter().enumerate() {
        let slot = layout.slot(w);
        if missing || slot > fixed_size - cursor {
            if cursor != fixed_size {
                return Err(PsszError::new("pssz: header cuts a known field"));
            }
            if !layout.optional || dwnc {
                return Err(PsszError::new("pssz: required record field missing"));
            }
            missing = true;
            continue;
        }
        last_present = Some(i);
        if layout.fixed {
            spans[i] = Some(fixed_start + cursor..fixed_start + cursor + slot);
        } else {
            let offset = read_offset_w(&bytes[fixed_start + cursor..], w)?;
            accept(offset, Some(i), spans)?;
        }
        cursor += slot;
    }
    if unknown {
        let mut at = known;
        while at < fixed_size {
            let offset = read_offset_w(&bytes[fixed_start + at..], w)?;
            accept(offset, None, spans)?;
            at += w;
        }
    }
    if !seen_variable && payload_size != fixed_size {
        return Err(PsszError::new("pssz: fixed record has trailing payload"));
    }
    // Mandatory pruning: one encoding per value. When the writer's last slot
    // is one this reader knows and it is an absent optional, the writer
    // should have pruned it. With unknown appended slots the reader's own
    // trailing optional is not the last slot and cannot be judged.
    if !dwnc && !unknown {
        if let Some(last) = last_present {
            if layouts[last].optional {
                if let Some(span) = &spans[last] {
                    if span_is_none(bytes, span, layouts[last].selector) {
                        return Err(PsszError::new(
                            "pssz: trailing absent optional field was not pruned",
                        ));
                    }
                }
            }
        }
    }
    Ok(unknown)
}

/// [`record_spans_into`] for a fixed-size record (fixed layout, every field
/// fixed-size): the fields lie at constant positions, so the frame is right
/// iff it has the record's size. The same errors in the same order.
/// `optional_fixed`: a field claims to be both an optional and fixed-size.
#[inline]
pub fn fixed_record_frame(w: usize, size: usize, len: usize, optional_fixed: bool) -> PsszResult<()> {
    width(w)?;
    if optional_fixed {
        return Err(PsszError::new("pssz: invalid reader field layout"));
    }
    if size > len {
        return Err(PsszError::new("pssz: record fixed region truncated"));
    }
    if size != len {
        return Err(PsszError::new("pssz: fixed record has trailing payload"));
    }
    Ok(())
}

const FIELD_SPAN: PsszError = PsszError::new("pssz: field span out of range");

/// A field the writer's frame does not hold: what [`RecordFrame::field`]
/// and [`TrustedFrame::field`] return in place of where it starts.
pub const ABSENT: usize = usize::MAX;

/// The size of the fixed region of the reader's fields: [`fixed_region`],
/// one field at a time, so that it is a constant of a generated reader.
/// `None` on overflow.
#[inline(always)]
pub const fn add_slot<F: PsszFormat, T: PsszPack<F> + ?Sized>(known: Option<usize>, w: usize) -> Option<usize> {
    match known {
        Some(known) => known.checked_add(slot::<F, T>(w)),
        None => None,
    }
}

/// [`record_spans_into`] for a generated reader, one field at a time: the
/// same checks, in the same order, with the same errors, on constants of the
/// record instead of a table of layouts, and scalars instead of an array of
/// spans.
///
/// [`begin`](Self::begin) reads the header; [`field`](Self::field), once per
/// field in declaration order, gives where the field starts (or [`ABSENT`]);
/// [`finish`](Self::finish) walks the slots this reader does not know,
/// applies the rules of the whole frame and gives where the last variable
/// field ends. A variable field ends where the next present one starts.
pub struct RecordFrame {
    fixed_start: usize,
    fixed_size: usize,
    payload_size: usize,
    cursor: usize,
    previous_offset: usize,
    seen_variable: bool,
    missing: bool,
    unknown: bool,
    known: usize,
    w: usize,
    dwnc: bool,
    /// The last present field is an optional that starts here (the
    /// mandatory-pruning rule); [`ABSENT`] when it is not an optional.
    last_optional: usize,
    last_selector: bool,
}
impl RecordFrame {
    /// `known`: the fixed region of the reader's fields ([`add_slot`]);
    /// `invalid`: a field claims to be both an optional and fixed-size;
    /// `all_fixed`: every field is fixed-size.
    #[inline(always)]
    pub fn begin(
        w: usize,
        known: Option<usize>,
        invalid: bool,
        all_fixed: bool,
        dwnc: bool,
        bytes: &[u8],
    ) -> PsszResult<Self> {
        width(w)?;
        let known = match known {
            Some(known) if !invalid => known,
            _ => return Err(PsszError::new("pssz: invalid reader field layout")),
        };
        let mut fixed_start = 0usize;
        let mut fixed_size = known;
        if !dwnc {
            if bytes.len() < w {
                return Err(PsszError::new("pssz: record header truncated"));
            }
            fixed_size = read_offset_w(bytes, w)?;
            fixed_start = w;
        }
        let payload_size = bytes.len() - fixed_start;
        if fixed_size > payload_size {
            return Err(PsszError::new("pssz: record fixed region truncated"));
        }
        if fixed_size > known && (fixed_size - known) % w != 0 {
            return Err(PsszError::new("pssz: partial unknown optional slot"));
        }
        let unknown = fixed_size > known;
        if unknown || !(dwnc && all_fixed) {
            wire_span(payload_size)?;
        }
        Ok(Self {
            fixed_start,
            fixed_size,
            payload_size,
            cursor: 0,
            previous_offset: fixed_size,
            seen_variable: false,
            missing: false,
            unknown,
            known,
            w,
            dwnc,
            last_optional: ABSENT,
            last_selector: false,
        })
    }

    #[inline(always)]
    fn accept(&mut self, bytes: &[u8], at: usize) -> PsszResult<usize> {
        let offset = read_offset_w(bytes.get(at..).ok_or(FIELD_SPAN)?, self.w)?;
        if offset > self.payload_size
            || offset < self.previous_offset
            || (!self.seen_variable && offset != self.fixed_size)
        {
            return Err(PsszError::new("pssz: invalid record payload offset"));
        }
        self.previous_offset = offset;
        self.seen_variable = true;
        Ok(self.fixed_start + offset)
    }

    /// The next field: where its bytes start in `bytes` (a fixed-size
    /// field's slot, a variable field's payload), or [`ABSENT`] for a
    /// trailing optional the writer did not store.
    #[inline(always)]
    pub fn field<F: PsszFormat, T: PsszPack<F> + ?Sized>(&mut self, bytes: &[u8]) -> PsszResult<usize> {
        let slot = slot::<F, T>(self.w);
        if self.missing || slot > self.fixed_size - self.cursor {
            if self.cursor != self.fixed_size {
                return Err(PsszError::new("pssz: header cuts a known field"));
            }
            if !T::IS_OPTIONAL || self.dwnc {
                return Err(PsszError::new("pssz: required record field missing"));
            }
            self.missing = true;
            return Ok(ABSENT);
        }
        let at = self.fixed_start + self.cursor;
        self.cursor += slot;
        if T::IS_FIXED_SIZE {
            // (An optional is never fixed-size: `begin` refused that.)
            self.last_optional = ABSENT;
            Ok(at)
        } else {
            let start = self.accept(bytes, at)?;
            self.last_optional = if T::IS_OPTIONAL { start } else { ABSENT };
            self.last_selector = T::OPTIONAL_SELECTOR;
            Ok(start)
        }
    }

    /// After the last field: where the last present variable field ends.
    /// Reports unknown fields to the active session (an exact reader fails
    /// here).
    #[inline(always)]
    pub fn finish(&mut self, bytes: &[u8]) -> PsszResult<usize> {
        let mut tail = bytes.len();
        if self.unknown {
            tail = self.unknown_slots(bytes)?;
        }
        if !self.seen_variable && self.payload_size != self.fixed_size {
            return Err(PsszError::new("pssz: fixed record has trailing payload"));
        }
        // Mandatory pruning: see `record_spans_into`.
        if !self.dwnc && !self.unknown && self.last_optional != ABSENT {
            let start = self.last_optional;
            if start >= bytes.len() || (self.last_selector && bytes.get(start) == Some(&0)) {
                return Err(PsszError::new(
                    "pssz: trailing absent optional field was not pruned",
                ));
            }
        }
        if self.unknown {
            unknown_fields()?;
        }
        Ok(tail)
    }

    /// The slots a newer writer appended: each is an offset, and the first
    /// ends the last field this reader knows.
    #[cold]
    #[inline(never)]
    fn unknown_slots(&mut self, bytes: &[u8]) -> PsszResult<usize> {
        let mut tail = bytes.len();
        let mut at = self.known;
        while at < self.fixed_size {
            let start = self.accept(bytes, self.fixed_start + at)?;
            if at == self.known {
                tail = start;
            }
            at += self.w;
        }
        Ok(tail)
    }
}

/// Field of a record read through a [`RecordFrame`]: `start` is what
/// [`RecordFrame::field`] gave, `end` where a variable field ends (unused
/// for a fixed-size one); the field is node `depth`.
#[inline(always)]
pub fn decode_frame_field<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
    bytes: &[u8],
    start: usize,
    end: usize,
    depth: usize,
) -> PsszResult<T> {
    if start == ABSENT {
        return T::pssz_missing_optional();
    }
    let end = if T::IS_FIXED_SIZE { start.wrapping_add(T::FIXED_SIZE) } else { end };
    T::pssz_unpack_at_depth(bytes.get(start..end).ok_or(FIELD_SPAN)?, depth)
}

/// [`decode_frame_field`] for a validator.
#[inline(always)]
pub fn validate_frame_field<F: PsszFormat, T: PsszPack<F> + crate::pssz::PsszValidate<F> + ?Sized>(
    bytes: &[u8],
    start: usize,
    end: usize,
    depth: usize,
) -> PsszResult<()> {
    if start == ABSENT {
        return Ok(());
    }
    let end = if T::IS_FIXED_SIZE { start.wrapping_add(T::FIXED_SIZE) } else { end };
    T::pssz_validate_at_depth(bytes.get(start..end).ok_or(FIELD_SPAN)?, depth)
}

/// A record frame the caller vouches for (`from_pssz_trusted`): the header
/// is believed, so a field is where the header and the slots say and nothing
/// is compared with anything; every access to the bytes is still
/// bounds-checked, by the slicing (a frame that lies is a panic).
pub struct TrustedFrame {
    fixed_start: usize,
    fixed_size: usize,
    cursor: usize,
    w: usize,
}
impl TrustedFrame {
    #[inline(always)]
    pub fn begin(w: usize, known: usize, dwnc: bool, bytes: &[u8]) -> Self {
        if dwnc {
            Self { fixed_start: 0, fixed_size: known, cursor: 0, w }
        } else {
            Self { fixed_start: w, fixed_size: trusted_offset(bytes, 0, w), cursor: 0, w }
        }
    }
    /// Where the next field starts, or [`ABSENT`] when the writer's fixed
    /// region ends before it.
    #[inline(always)]
    pub fn field<F: PsszFormat, T: PsszPack<F> + ?Sized>(&mut self, bytes: &[u8]) -> usize {
        let slot = slot::<F, T>(self.w);
        if slot > self.fixed_size.saturating_sub(self.cursor) {
            self.cursor = self.fixed_size;
            return ABSENT;
        }
        let at = self.fixed_start.saturating_add(self.cursor);
        self.cursor += slot;
        if T::IS_FIXED_SIZE {
            at
        } else {
            self.fixed_start.saturating_add(trusted_offset(bytes, at, self.w))
        }
    }
    /// Where the last variable field the reader knows ends: at the payload
    /// of the first slot it does not know, else at the end of the frame.
    #[inline(always)]
    pub fn finish(&self, known: usize, bytes: &[u8]) -> usize {
        if self.fixed_size > known {
            self.fixed_start
                .saturating_add(trusted_offset(bytes, self.fixed_start.saturating_add(known), self.w))
        } else {
            bytes.len()
        }
    }
}

/// The offset `w` bytes wide at `at`. Panics when it is not in `bytes`.
#[inline(always)]
pub fn trusted_offset(bytes: &[u8], at: usize, w: usize) -> usize {
    let slot = &bytes[at..][..w];
    match w {
        1 => slot[0] as usize,
        2 => u16::from_le_bytes([slot[0], slot[1]]) as usize,
        4 => u32::from_le_bytes([slot[0], slot[1], slot[2], slot[3]]) as usize,
        _ => crate::pssz::trusted_bytes_invalid(PsszError::new("pssz: unsupported offset/header width")),
    }
}

/// Field of a record read through a [`TrustedFrame`]. Panics when the span
/// is not in `bytes`.
#[inline(always)]
pub fn trusted_frame_field<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(bytes: &[u8], start: usize, end: usize) -> T {
    if start == ABSENT {
        return match T::pssz_missing_optional() {
            Ok(value) => value,
            Err(error) => crate::pssz::trusted_bytes_invalid(error),
        };
    }
    let end = if T::IS_FIXED_SIZE { start.saturating_add(T::FIXED_SIZE) } else { end };
    T::pssz_unpack_trusted(&bytes[start..end])
}

/// Stack-array frame resolution for generated record readers. Reports
/// unknown fields to the active session (an exact reader fails here).
#[inline]
pub fn record_spans_n<const N: usize>(
    w: usize,
    layouts: [Layout; N],
    bytes: &[u8],
    dwnc: bool,
) -> PsszResult<[Option<Range<usize>>; N]> {
    let mut spans: [Option<Range<usize>>; N] = std::array::from_fn(|_| None);
    if record_spans_into(w, &layouts, bytes, dwnc, &mut spans)? {
        unknown_fields()?;
    }
    Ok(spans)
}

/// Heap variant for the schema-driven codec, at an explicit profile's width.
pub fn record_spans<F: PsszFormat>(
    layouts: &[Layout],
    bytes: &[u8],
    dwnc: bool,
) -> PsszResult<Vec<Option<Range<usize>>>> {
    format::<F>()?;
    record_spans_w(F::OFFSET_BYTES, layouts, bytes, dwnc)
}

/// Heap variant at the record's own width `w` (the schema-driven codec's
/// automatic profile). Reports unknown fields to the active session.
pub fn record_spans_w(
    w: usize,
    layouts: &[Layout],
    bytes: &[u8],
    dwnc: bool,
) -> PsszResult<Vec<Option<Range<usize>>>> {
    let mut spans = Vec::new();
    spans
        .try_reserve(layouts.len())
        .map_err(|_| PsszError::new("pssz: record allocation overflow"))?;
    spans.resize(layouts.len(), None);
    if record_spans_into(w, layouts, bytes, dwnc, &mut spans)? {
        unknown_fields()?;
    }
    Ok(spans)
}

pub fn decode_field<F: PsszFormat, T: PsszUnpack<F>>(
    bytes: &[u8],
    span: Option<Range<usize>>,
) -> PsszResult<T> {
    match span {
        Some(span) => T::pssz_unpack(
            bytes
                .get(span)
                .ok_or(PsszError::new("pssz: field span out of range"))?,
        ),
        None => T::pssz_missing_optional(),
    }
}

pub fn field<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> PsszResult<(Layout, Vec<u8>)> {
    let mut bytes = Vec::new();
    value.pssz_pack_checked(&mut bytes)?;
    if T::IS_FIXED_SIZE && bytes.len() != T::FIXED_SIZE {
        return Err(PsszError::new("pssz: fixed encoder size mismatch"));
    }
    let mut layout = Layout::of::<F, T>();
    layout.omit = T::IS_OPTIONAL && value.pssz_is_none();
    Ok((layout, bytes))
}

/// Where variable field `index` of a frame ends: where the next present
/// variable field starts (`starts` is what the frame gave for each field,
/// `variable` says which are not fixed-size), else at `tail`.
#[inline(always)]
pub fn field_end(starts: &[usize], variable: &[bool], index: usize, tail: usize) -> usize {
    let mut at = index + 1;
    while at < starts.len() && at < variable.len() {
        if variable[at] && starts[at] != ABSENT {
            return starts[at];
        }
        at += 1;
    }
    tail
}

/// Number of leading fields the encoder writes: everything up to and
/// including the last field that is not an absent optional (all, for DWNC).
#[inline]
pub fn record_kept_fields(omitted: &[bool], dwnc: bool) -> usize {
    if dwnc {
        omitted.len()
    } else {
        omitted.iter().rposition(|omit| !omit).map_or(0, |i| i + 1)
    }
}

/// A pruned trailing None is a synthesized default, not a traversed wire node.
/// Compute the kept prefix before attempting to encode any child values.
pub fn field_if_kept<F: PsszFormat, T: PsszPack<F> + ?Sized>(
    value: &T,
    kept: bool,
) -> PsszResult<(Layout, Vec<u8>)> {
    if kept {
        field::<F, T>(value)
    } else {
        if !T::IS_OPTIONAL || !value.pssz_is_none() {
            return Err(PsszError::new("pssz: cannot prune required or present field"));
        }
        let mut layout = Layout::of::<F, T>();
        layout.omit = true;
        Ok((layout, Vec::new()))
    }
}

/// Encode a record whose header and offset slots are `w` bytes wide. Trailing
/// absent optionals are always pruned (never for DWNC). Nothing is appended to
/// `out` on error.
pub fn encode_record_w(
    w: usize,
    fields: &[(Layout, Vec<u8>)],
    dwnc: bool,
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    width(w)?;
    let keep = if dwnc {
        fields.len()
    } else {
        fields
            .iter()
            .rposition(|(layout, _)| !layout.optional || !layout.omit)
            .map_or(0, |i| i + 1)
    };
    let fields = &fields[..keep];
    let fixed = fields.iter().try_fold(0usize, |n, (layout, _)| {
        n.checked_add(layout.slot(w))
            .ok_or(PsszError::new("pssz: fixed region overflow"))
    })?;
    let header = if dwnc { 0 } else { w };
    let size = header
        .checked_add(fixed)
        .ok_or(PsszError::new("pssz: record size overflow"))?;
    let mut result = Vec::new();
    result
        .try_reserve(size)
        .map_err(|_| PsszError::new("pssz: record allocation overflow"))?;
    result.resize(size, 0);
    if !dwnc {
        write_offset_w(&mut result[..header], w, fixed)?;
    }
    let mut cursor = header;
    for (layout, bytes) in fields {
        if layout.fixed {
            if layout.size != bytes.len() {
                return Err(PsszError::new("pssz: fixed field size mismatch"));
            }
            result[cursor..cursor + layout.size].copy_from_slice(bytes);
        } else {
            let offset = result.len() - header;
            write_offset_w(&mut result[cursor..cursor + w], w, offset)?;
            result
                .try_reserve(bytes.len())
                .map_err(|_| PsszError::new("pssz: record allocation overflow"))?;
            result.extend_from_slice(bytes);
        }
        cursor += layout.slot(w);
    }
    wire_span(result.len() - header)?;
    out.try_reserve(result.len())
        .map_err(|_| PsszError::new("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
}

pub fn encode_record<F: PsszFormat>(
    fields: &[(Layout, Vec<u8>)],
    dwnc: bool,
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    format::<F>()?;
    encode_record_w(F::OFFSET_BYTES, fields, dwnc, out)
}

/// Encode step shared by every generated record: frame at the record's own
/// width, then the `maxDynamicData` cap. The automatic profile enforces the
/// cap at every node; explicit profiles only at the top level (see
/// `pssz::try_encode`), as C++ does.
pub fn finish_record<F: PsszFormat>(
    w: usize,
    cap: Option<usize>,
    fields: &[(Layout, Vec<u8>)],
    dwnc: bool,
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    let mut result = Vec::new();
    encode_record_w(w, fields, dwnc, &mut result)?;
    if F::AUTO {
        check_cap(cap, result.len())?;
    }
    out.try_reserve(result.len())
        .map_err(|_| PsszError::new("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
}

/// What a field of type `T` takes in the fixed region of a record whose
/// slots are `w` wide: [`Layout::slot`], as an expression that folds to a
/// constant in a generated writer.
#[inline(always)]
pub const fn slot<F: PsszFormat, T: PsszPack<F> + ?Sized>(w: usize) -> usize {
    if T::IS_FIXED_SIZE {
        T::FIXED_SIZE
    } else {
        w
    }
}

/// What a field of a record whose slots are `w` wide adds to the record's
/// `pssz_size_hint`: its slot, and behind it the payload of a variable one.
#[inline]
pub fn field_size_hint<F: PsszFormat, T: PsszPack<F> + ?Sized>(w: usize, value: &T) -> usize {
    if T::IS_FIXED_SIZE {
        T::FIXED_SIZE
    } else {
        value.pssz_size_hint().saturating_add(w)
    }
}

/// Writes one record (or tuple) frame into the output, in place: the same
/// bytes and the same errors, in the same order, as encoding every field on
/// its own and assembling them with [`encode_record_w`] and
/// [`finish_record`].
///
/// [`begin`](Self::begin) appends the header and a zeroed fixed region for the
/// kept fields; each [`field`](Self::field), in declaration order, writes a
/// fixed-size value into its slot or appends a payload and stores its offset;
/// [`finish`](Self::finish) checks the offsets against the record's width,
/// the 32-bit span and the cap.
///
/// On error the output may be longer than it was (never shorter, and nothing
/// before the record is touched): the caller that owns the transaction
/// (`pssz_pack_checked`, `encode_into`) truncates.
pub struct RecordEncoder {
    /// Where the fixed region starts (behind the header).
    body: usize,
    /// Where the next field's slot starts.
    cursor: usize,
    w: usize,
    header: usize,
    fixed: usize,
    /// The largest offset stored: offsets only grow, so the last one.
    last: usize,
    child_depth: usize,
}
impl RecordEncoder {
    /// `fixed` is the sum of the [`slot`]s of the kept fields; `depth` the
    /// depth of the record's own node.
    #[inline(always)]
    pub fn begin(out: &mut Vec<u8>, w: usize, dwnc: bool, fixed: usize, depth: usize) -> PsszResult<Self> {
        let header = if dwnc { 0 } else { w };
        let base = out.len();
        append_zeros(out, header.saturating_add(fixed))?;
        store_offset(out, base, header, fixed);
        Ok(Self { body: base + header, cursor: base + header, w, header, fixed, last: 0, child_depth: depth + 1 })
    }

    /// The next kept field.
    #[inline(always)]
    pub fn field<F: PsszFormat, T: PsszPack<F> + ?Sized>(&mut self, out: &mut Vec<u8>, value: &T) -> PsszResult<()> {
        if T::IS_FIXED_SIZE {
            // (A range that wraps is not in the output: an error.)
            let end = self.cursor.wrapping_add(T::FIXED_SIZE);
            let slot = out.get_mut(self.cursor..end).ok_or(SHRUNK)?;
            value.pssz_pack_fixed(slot, self.child_depth)?;
            self.cursor = end;
        } else {
            let offset = out.len().checked_sub(self.body).ok_or(SHRUNK)?;
            store_offset(out, self.cursor, self.w, offset);
            self.last = offset;
            self.cursor += self.w;
            value.pssz_pack_at_depth(out, self.child_depth)?;
        }
        Ok(())
    }

    /// `cap` is the record's `maxDynamicData` (the automatic profile checks
    /// it at every record; explicit profiles only at the top level).
    #[inline(always)]
    pub fn finish<F: PsszFormat>(self, out: &mut Vec<u8>, cap: Option<usize>) -> PsszResult<()> {
        width(self.w)?;
        let payload = out.len().checked_sub(self.body).ok_or(SHRUNK)?;
        if payload < self.fixed {
            return Err(SHRUNK);
        }
        let limit = offset_limit(self.w);
        if (self.header != 0 && self.fixed > limit) || self.last > limit {
            return Err(OFFSET_OVERFLOW);
        }
        wire_span(payload)?;
        if F::AUTO {
            check_cap(cap, payload + self.header)?;
        }
        Ok(())
    }
}

// ── The size pass ───────────────────────────────────────────────────────────
//
// Every writer has a twin that runs the same checks in the same order and
// writes nothing: `PsszPack::pssz_size_at_depth` of `pssz_pack_at_depth`
// (it returns the number of bytes the writer appends) and
// `PsszPack::pssz_size_fixed` of `pssz_pack_fixed`. Each function below is
// the twin of one writer above and is kept in its shape: [`RecordSize`] of
// [`RecordEncoder`], [`size_sequence_with_at_depth`] of
// [`encode_sequence_with_at_depth`], [`size_fixed_by_size`] of
// [`pack_fixed_by_append`]. A sum
// that does not fit `usize` saturates; the writer could not have allocated
// it, and the checks that follow (an offset width, the 32-bit span) refuse
// it on a 64-bit target.

const FIXED_MISMATCH: PsszError = PsszError::new("pssz: fixed encoder size mismatch");

/// `pssz_size_at_depth` of a codec that only writes (a hand-written one):
/// the value is encoded and measured.
pub fn size_by_encoding<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T, depth: usize) -> PsszResult<usize> {
    let mut out = Vec::new();
    value.pssz_pack_at_depth(&mut out, depth)?;
    Ok(out.len())
}

/// The twin of [`pack_fixed_by_append`]: the value's own size pass, and its
/// size is that of its slot (`T::FIXED_SIZE` bytes, which is what every
/// writer in this crate hands a fixed-size value).
#[inline(always)]
pub fn size_fixed_by_size<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T, depth: usize) -> PsszResult<()> {
    if value.pssz_size_at_depth(depth)? != T::FIXED_SIZE {
        return Err(FIXED_MISMATCH);
    }
    Ok(())
}

/// The size pass of one record (or tuple) frame: [`RecordEncoder`] without
/// the output.
pub struct RecordSize {
    /// The fixed region and the payloads behind it, so far.
    payload: usize,
    w: usize,
    header: usize,
    fixed: usize,
    last: usize,
    child_depth: usize,
}
impl RecordSize {
    #[inline(always)]
    pub fn begin(w: usize, dwnc: bool, fixed: usize, depth: usize) -> Self {
        Self { payload: fixed, w, header: if dwnc { 0 } else { w }, fixed, last: 0, child_depth: depth + 1 }
    }

    /// The next kept field.
    #[inline(always)]
    pub fn field<F: PsszFormat, T: PsszPack<F> + ?Sized>(&mut self, value: &T) -> PsszResult<()> {
        if T::IS_FIXED_SIZE {
            value.pssz_size_fixed(self.child_depth)
        } else {
            self.last = self.payload;
            self.payload = self.payload.saturating_add(value.pssz_size_at_depth(self.child_depth)?);
            Ok(())
        }
    }

    #[inline(always)]
    pub fn finish<F: PsszFormat>(self, cap: Option<usize>) -> PsszResult<usize> {
        width(self.w)?;
        let limit = offset_limit(self.w);
        if (self.header != 0 && self.fixed > limit) || self.last > limit {
            return Err(OFFSET_OVERFLOW);
        }
        wire_span(self.payload)?;
        let size = self.payload.saturating_add(self.header);
        if F::AUTO {
            check_cap(cap, size)?;
        }
        Ok(size)
    }
}

/// The size pass of [`encode_sequence_at_depth`].
#[inline]
pub fn size_sequence_at_depth<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    values: &[T],
    known_count: bool,
    depth: usize,
) -> PsszResult<usize> {
    if T::IS_FIXED_SIZE {
        if let Some(bytes) = T::pssz_bulk_bytes(values) {
            width(w)?;
            if !values.is_empty() {
                encode_node(depth + 1)?;
            }
            return Ok(bytes.len());
        }
    }
    size_sequence_with_at_depth(
        w,
        T::IS_FIXED_SIZE.then_some(T::FIXED_SIZE),
        values.iter(),
        |value: &T, depth| value.pssz_size_at_depth(depth),
        |value: &T, depth| value.pssz_size_fixed(depth),
        known_count,
        depth,
    )
}

/// The size pass of [`encode_sequence_with_at_depth`]: `size` is the size
/// pass of one element (node `depth + 1`), `size_fixed` that of a fixed-size
/// one in its slot.
#[inline]
pub fn size_sequence_with_at_depth<T>(
    w: usize,
    fixed: Option<usize>,
    values: impl ExactSizeIterator<Item = T>,
    mut size: impl FnMut(T, usize) -> PsszResult<usize>,
    mut size_fixed: impl FnMut(T, usize) -> PsszResult<()>,
    known_count: bool,
    depth: usize,
) -> PsszResult<usize> {
    width(w)?;
    let count = values.len();
    if let Some(element) = fixed {
        if element == 0 && count != 0 && !known_count {
            return Err(PsszError::new(
                "pssz: nonempty zero-size vector is unsupported",
            ));
        }
        let total = count
            .checked_mul(element)
            .ok_or(PsszError::new("pssz: sequence size overflow"))?;
        if element == 0 {
            for value in values {
                size_fixed(value, depth + 1)?;
            }
            return Ok(0);
        }
        // As the writer: at most `count` elements, and exactly that many.
        let mut written = 0usize;
        for value in values.take(count) {
            size_fixed(value, depth + 1)?;
            written += 1;
        }
        if written != count {
            return Err(FIXED_MISMATCH);
        }
        Ok(total)
    } else {
        let table = count
            .checked_mul(w)
            .ok_or(PsszError::new("pssz: sequence table overflow"))?;
        let limit = offset_limit(w);
        let mut total = table;
        for (i, value) in values.enumerate() {
            if total > limit {
                return Err(OFFSET_OVERFLOW);
            }
            if i >= count {
                return Err(PsszError::new("pssz: sequence length mismatch"));
            }
            total = total.saturating_add(size(value, depth + 1)?);
        }
        wire_span(total)?;
        Ok(total)
    }
}

/// Reader-side counterpart of `finish_record`.
pub fn enter_record<F: PsszFormat>(cap: Option<usize>, bytes: &[u8]) -> PsszResult<()> {
    if F::AUTO {
        check_cap(cap, bytes.len())?;
    }
    Ok(())
}

// ── Borrowed field access ───────────────────────────────────────────────────

#[derive(Clone, Copy, Debug)]
pub struct FieldLocation {
    pub width: usize,
    pub slot: usize,
    pub slot_size: usize,
    pub known_fixed: usize,
    pub next_variable: Option<usize>,
    pub fixed: bool,
    pub optional: bool,
}
pub const fn record_location(w: usize, layouts: &[Layout], index: usize) -> FieldLocation {
    let mut slot = 0usize;
    let mut at = 0usize;
    let mut result = FieldLocation {
        width: w,
        slot: usize::MAX,
        slot_size: 0,
        known_fixed: 0,
        next_variable: None,
        fixed: false,
        optional: false,
    };
    while at < layouts.len() {
        let field = layouts[at];
        if at == index {
            result.slot = slot;
            result.slot_size = field.slot(w);
            result.fixed = field.fixed;
            result.optional = field.optional;
        }
        if at > index && !field.fixed && result.next_variable.is_none() {
            result.next_variable = Some(slot);
        }
        slot = slot.saturating_add(field.slot(w));
        at += 1;
    }
    result.known_fixed = slot;
    result
}

/// O(1) field access. Returns an error, never panics, on a malformed frame;
/// full validation (including pruning) is `PsszView::try_new`. A trailing
/// optional the writer pruned yields the static encoding of None.
#[inline(always)]
pub fn field_bytes<'a, F: PsszFormat, T: PsszPack<F> + ?Sized>(
    bytes: &'a [u8],
    location: FieldLocation,
    dwnc: bool,
) -> PsszResult<&'a [u8]> {
    let w = location.width;
    width(w)?;
    let header = if dwnc { 0 } else { w };
    let written_fixed = if dwnc {
        location.known_fixed
    } else {
        read_offset_w(bytes, w)?
    };
    let body = bytes
        .get(header..)
        .ok_or(PsszError::new("pssz view: header underrun"))?;
    if written_fixed > body.len() {
        return Err(PsszError::new("pssz view: fixed region underrun"));
    }
    let end_slot = location
        .slot
        .checked_add(location.slot_size)
        .ok_or(PsszError::new("pssz view: field location overflow"))?;
    if end_slot > written_fixed {
        if dwnc || location.slot < written_fixed || !location.optional {
            return Err(PsszError::new("pssz view: absent required field"));
        }
        return T::pssz_none_bytes().ok_or(PsszError::new("pssz view: optional default unavailable"));
    }
    if location.fixed {
        return body
            .get(location.slot..end_slot)
            .ok_or(PsszError::new("pssz view: field span mismatch"));
    }
    let begin = read_offset_w(&body[location.slot..], w)?;
    let end = match location.next_variable {
        Some(next) if next.checked_add(w).is_some_and(|end| end <= written_fixed) => {
            read_offset_w(&body[next..], w)?
        }
        _ if written_fixed > location.known_fixed => {
            read_offset_w(&body[location.known_fixed..], w)?
        }
        _ => body.len(),
    };
    if begin < written_fixed || begin > end {
        return Err(PsszError::new("pssz view: invalid field offset"));
    }
    body.get(begin..end)
        .ok_or(PsszError::new("pssz view: field offset outside buffer"))
}

// ── Sequences ───────────────────────────────────────────────────────────────

/// Validate a sequence's table (offset slots `w` wide) and return its element
/// count. `count` is `Some` for fixed-count arrays.
pub fn sequence_len<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    bytes: &[u8],
    count: Option<usize>,
) -> PsszResult<usize> {
    width(w)?;
    if T::IS_FIXED_SIZE {
        let size = T::FIXED_SIZE;
        let n = match count {
            Some(n) => n,
            None if bytes.is_empty() => 0,
            None if size == 0 => {
                return Err(PsszError::new(
                    "pssz: nonempty zero-size vector is unsupported",
                ))
            }
            None if bytes.len() % size == 0 => bytes.len() / size,
            None => return Err(PsszError::new("pssz: fixed sequence span mismatch")),
        };
        if n.checked_mul(size) != Some(bytes.len()) {
            return Err(PsszError::new(
                "pssz: fixed sequence span mismatch or overflow",
            ));
        }
        return Ok(n);
    }
    let n = match count {
        Some(n) => n,
        None if bytes.is_empty() => 0,
        None => {
            let first = read_offset_w(bytes, w)?;
            if first == 0 || first % w != 0 || first > bytes.len() {
                return Err(PsszError::new("pssz: invalid sequence offset table"));
            }
            first / w
        }
    };
    let table = n
        .checked_mul(w)
        .ok_or(PsszError::new("pssz: sequence table overflow"))?;
    if table > bytes.len() || (n == 0 && !bytes.is_empty()) {
        return Err(PsszError::new("pssz: sequence table mismatch"));
    }
    wire_span(bytes.len())?;
    let mut previous = table;
    for i in 0..n {
        let begin = read_offset_w(&bytes[i * w..], w)?;
        if (i == 0 && begin != table) || begin < previous || begin > bytes.len() {
            return Err(PsszError::new("pssz: sequence offset out of range"));
        }
        previous = begin;
    }
    Ok(n)
}

/// Element `index` of a sequence frame of `count` elements
/// ([`sequence_count`] or [`sequence_len`] of this same byte span): O(1),
/// and the span is inside `bytes`.
#[inline]
pub fn sequence_span<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    bytes: &[u8],
    count: usize,
    index: usize,
) -> PsszResult<Range<usize>> {
    if index >= count {
        return Err(PsszError::new("pssz: sequence index out of range"));
    }
    if T::IS_FIXED_SIZE {
        let begin = index
            .checked_mul(T::FIXED_SIZE)
            .ok_or(PsszError::new("pssz: element offset overflow"))?;
        let end = begin
            .checked_add(T::FIXED_SIZE)
            .ok_or(PsszError::new("pssz: element end overflow"))?;
        if end > bytes.len() {
            return Err(PsszError::new("pssz: element truncated"));
        }
        return Ok(begin..end);
    }
    width(w)?;
    let slot = index
        .checked_mul(w)
        .ok_or(PsszError::new("pssz: offset slot overflow"))?;
    let begin = read_offset_w(
        bytes
            .get(slot..)
            .ok_or(PsszError::new("pssz: offset slot truncated"))?,
        w,
    )?;
    let end = if index + 1 < count {
        let next = slot
            .checked_add(w)
            .ok_or(PsszError::new("pssz: next slot overflow"))?;
        read_offset_w(
            bytes
                .get(next..)
                .ok_or(PsszError::new("pssz: next slot truncated"))?,
            w,
        )?
    } else {
        bytes.len()
    };
    if begin > end || end > bytes.len() {
        return Err(PsszError::new("pssz: element span out of range"));
    }
    Ok(begin..end)
}

const ELEMENT_SPAN: PsszError = PsszError::new("pssz: element span out of range");

/// The element count of a sequence frame, from its length or its first
/// offset alone: what a view needs to index it. [`sequence_len`] checks the
/// whole table.
#[inline]
pub fn sequence_count<F: PsszFormat, T: PsszPack<F>>(w: usize, bytes: &[u8], count: Option<usize>) -> PsszResult<usize> {
    width(w)?;
    if T::IS_FIXED_SIZE {
        let size = T::FIXED_SIZE;
        let n = match count {
            Some(n) => n,
            None if bytes.is_empty() => 0,
            None if size == 0 => {
                return Err(PsszError::new(
                    "pssz: nonempty zero-size vector is unsupported",
                ))
            }
            None if bytes.len() % size == 0 => bytes.len() / size,
            None => return Err(PsszError::new("pssz: fixed sequence span mismatch")),
        };
        if n.checked_mul(size) != Some(bytes.len()) {
            return Err(PsszError::new(
                "pssz: fixed sequence span mismatch or overflow",
            ));
        }
        return Ok(n);
    }
    let n = match count {
        Some(n) => n,
        None if bytes.is_empty() => 0,
        None => {
            let first = read_offset_w(bytes, w)?;
            if first == 0 || first % w != 0 || first > bytes.len() {
                return Err(PsszError::new("pssz: invalid sequence offset table"));
            }
            first / w
        }
    };
    let table = n
        .checked_mul(w)
        .ok_or(PsszError::new("pssz: sequence table overflow"))?;
    if table > bytes.len() || (n == 0 && !bytes.is_empty()) {
        return Err(PsszError::new("pssz: sequence table mismatch"));
    }
    Ok(n)
}

/// Calls `element(index, bytes of the element)` for each element of a
/// sequence frame whose table [`sequence_len`] accepted (`n` is what it
/// returned): one pass over the offsets.
#[inline(always)]
pub fn for_each_element<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    bytes: &[u8],
    n: usize,
    mut element: impl FnMut(usize, &[u8]) -> PsszResult<()>,
) -> PsszResult<()> {
    if T::IS_FIXED_SIZE {
        let size = T::FIXED_SIZE;
        if size == 0 {
            for index in 0..n {
                element(index, &[])?;
            }
            return Ok(());
        }
        for (index, chunk) in bytes.chunks_exact(size).enumerate() {
            element(index, chunk)?;
        }
        return Ok(());
    }
    let mut begin = n.wrapping_mul(w);
    for index in 0..n {
        let end = if index + 1 < n {
            read_offset_w(bytes.get((index + 1) * w..).ok_or(ELEMENT_SPAN)?, w)?
        } else {
            bytes.len()
        };
        element(index, bytes.get(begin..end).ok_or(ELEMENT_SPAN)?)?;
        begin = end;
    }
    Ok(())
}

/// The elements of a sequence frame; they are nodes `depth`. A Vec owns each
/// initialized element throughout errors and unwinding.
#[inline]
pub fn decode_sequence_at_depth<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
    w: usize,
    bytes: &[u8],
    count: Option<usize>,
    depth: usize,
) -> PsszResult<Vec<T>> {
    let n = sequence_len::<F, T>(w, bytes, count)?;
    decode_elements_at_depth::<F, T>(w, bytes, n, depth)
}

/// The `n` elements of a sequence frame whose table [`sequence_len`]
/// accepted; they are nodes `depth`.
#[inline]
pub fn decode_elements_at_depth<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
    w: usize,
    bytes: &[u8],
    n: usize,
    depth: usize,
) -> PsszResult<Vec<T>> {
    let mut result = Vec::new();
    result
        .try_reserve_exact(n)
        .map_err(|_| PsszError::new("pssz: sequence allocation overflow"))?;
    if T::IS_FIXED_SIZE && T::FIXED_SIZE != 0 && n != 0 && T::BULK {
        // Leaves whose bytes are their values: what is left of each one's
        // own reader is its depth check.
        decode_node(depth)?;
        T::pssz_unpack_bulk(bytes, &mut result);
        if result.len() == n {
            return Ok(result);
        }
        result.clear();
    }
    for_each_element::<F, T>(w, bytes, n, |_, element| {
        result.push(T::pssz_unpack_at_depth(element, depth)?);
        Ok(())
    })?;
    Ok(result)
}

/// The elements of an array frame, without a heap allocation; they are
/// nodes `depth`.
#[inline]
pub fn decode_array_at_depth<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>, const N: usize>(
    w: usize,
    bytes: &[u8],
    depth: usize,
) -> PsszResult<[T; N]> {
    sequence_len::<F, T>(w, bytes, Some(N))?;
    if T::IS_FIXED_SIZE && T::FIXED_SIZE != 0 && N != 0 && T::BULK {
        decode_node(depth)?;
        if let Some(array) = T::pssz_unpack_bulk_array::<N>(bytes) {
            return Ok(array);
        }
    }
    // `array::from_fn` cannot stop: after the first error the rest are
    // skipped, and no element is `None` when there was none.
    let mut error: Option<PsszError> = None;
    let mut begin = N.wrapping_mul(w);
    let items: [Option<T>; N] = std::array::from_fn(|index| {
        if error.is_some() {
            return None;
        }
        let element = if T::IS_FIXED_SIZE {
            let size = T::FIXED_SIZE;
            bytes.get(index.wrapping_mul(size)..index.wrapping_mul(size).wrapping_add(size))
        } else {
            let end = if index + 1 < N {
                bytes.get((index + 1).wrapping_mul(w)..).and_then(|slot| read_offset_w(slot, w).ok())
            } else {
                Some(bytes.len())
            };
            let span = end.and_then(|end| bytes.get(begin..end).map(|element| (element, end)));
            span.map(|(element, end)| {
                begin = end;
                element
            })
        };
        match element.ok_or(ELEMENT_SPAN).and_then(|element| T::pssz_unpack_at_depth(element, depth)) {
            Ok(value) => Some(value),
            Err(failed) => {
                error = Some(failed);
                None
            }
        }
    });
    if let Some(error) = error {
        return Err(error);
    }
    Ok(items.map(|item| match item {
        Some(value) => value,
        None => unreachable!("every element was decoded"),
    }))
}

/// [`decode_sequence_at_depth`] for a reader that reads its depth from the
/// thread. A Vec owns each initialized element throughout errors and
/// unwinding.
pub fn decode_sequence<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
    w: usize,
    bytes: &[u8],
    count: Option<usize>,
) -> PsszResult<Vec<T>> {
    let n = sequence_len::<F, T>(w, bytes, count)?;
    let mut result = Vec::new();
    result
        .try_reserve(n)
        .map_err(|_| PsszError::new("pssz: sequence allocation overflow"))?;
    for i in 0..n {
        result.push(T::pssz_unpack(
            &bytes[sequence_span::<F, T>(w, bytes, n, i)?],
        )?);
    }
    Ok(result)
}

/// `known_count` is a fixed-count array: zero-size elements keep their
/// compile-time count. A variable-count list of zero-size elements cannot
/// represent a nonzero count and is rejected.
pub fn encode_sequence<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    values: &[T],
    out: &mut Vec<u8>,
    known_count: bool,
) -> PsszResult<()> {
    encode_sequence_with(
        w,
        T::IS_FIXED_SIZE.then_some(T::FIXED_SIZE),
        values.iter(),
        |value, out| value.pssz_pack_checked(out),
        out,
        known_count,
    )
}

/// [`encode_sequence`] over any elements: `fixed` is the elements' fixed
/// size when they have one (no offset table), `pack` appends one element.
pub fn encode_sequence_with<T>(
    w: usize,
    fixed: Option<usize>,
    values: impl ExactSizeIterator<Item = T>,
    mut pack: impl FnMut(T, &mut Vec<u8>) -> PsszResult<()>,
    out: &mut Vec<u8>,
    known_count: bool,
) -> PsszResult<()> {
    width(w)?;
    let count = values.len();
    let mut result = Vec::new();
    if let Some(size) = fixed {
        if size == 0 && count != 0 && !known_count {
            return Err(PsszError::new(
                "pssz: nonempty zero-size vector is unsupported",
            ));
        }
        count
            .checked_mul(size)
            .ok_or(PsszError::new("pssz: sequence size overflow"))?;
        for value in values {
            let before = result.len();
            pack(value, &mut result)?;
            if result.len() - before != size {
                return Err(PsszError::new("pssz: fixed encoder size mismatch"));
            }
        }
    } else {
        let table = count
            .checked_mul(w)
            .ok_or(PsszError::new("pssz: sequence table overflow"))?;
        result
            .try_reserve(table)
            .map_err(|_| PsszError::new("pssz: sequence allocation overflow"))?;
        result.resize(table, 0);
        for (i, value) in values.enumerate() {
            let offset = result.len();
            write_offset_w(&mut result[i * w..(i + 1) * w], w, offset)?;
            pack(value, &mut result)?;
        }
        wire_span(result.len())?;
    }
    out.try_reserve(result.len())
        .map_err(|_| PsszError::new("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
}

/// [`encode_sequence`], written into the output in place; the sequence is
/// node `depth`, whose guard the caller has passed. On error the output may
/// be longer than it was: the caller that owns the transaction truncates.
#[inline]
pub fn encode_sequence_at_depth<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    values: &[T],
    out: &mut Vec<u8>,
    known_count: bool,
    depth: usize,
) -> PsszResult<()> {
    if T::IS_FIXED_SIZE {
        if let Some(bytes) = T::pssz_bulk_bytes(values) {
            // The elements are leaves whose bytes are the encoding: all
            // that is left of each one's own writer is its depth check.
            width(w)?;
            if !values.is_empty() {
                encode_node(depth + 1)?;
            }
            return append(out, bytes);
        }
    }
    encode_sequence_with_at_depth(
        w,
        T::IS_FIXED_SIZE.then_some(T::FIXED_SIZE),
        values.iter(),
        |value: &T, out: &mut Vec<u8>, depth| value.pssz_pack_at_depth(out, depth),
        |value: &T, slot: &mut [u8], depth| value.pssz_pack_fixed(slot, depth),
        out,
        known_count,
        depth,
    )
}

/// [`encode_sequence_with`], in place: `pack` appends one element (node
/// `depth + 1`), `pack_fixed` writes a fixed-size one into its slot.
#[inline]
pub fn encode_sequence_with_at_depth<T>(
    w: usize,
    fixed: Option<usize>,
    values: impl ExactSizeIterator<Item = T>,
    mut pack: impl FnMut(T, &mut Vec<u8>, usize) -> PsszResult<()>,
    mut pack_fixed: impl FnMut(T, &mut [u8], usize) -> PsszResult<()>,
    out: &mut Vec<u8>,
    known_count: bool,
    depth: usize,
) -> PsszResult<()> {
    width(w)?;
    let count = values.len();
    let base = out.len();
    if let Some(size) = fixed {
        if size == 0 && count != 0 && !known_count {
            return Err(PsszError::new(
                "pssz: nonempty zero-size vector is unsupported",
            ));
        }
        let total = count
            .checked_mul(size)
            .ok_or(PsszError::new("pssz: sequence size overflow"))?;
        if size == 0 {
            for value in values {
                pack_fixed(value, &mut [], depth + 1)?;
            }
            return Ok(());
        }
        append_zeros(out, total)?;
        // `values` may hold fewer elements than it said (a safe trait): the
        // zip stops at the shorter and the length is compared.
        let mut written = 0usize;
        for (value, slot) in values.zip(out[base..].chunks_exact_mut(size)) {
            pack_fixed(value, slot, depth + 1)?;
            written += 1;
        }
        if written != count {
            return Err(PsszError::new("pssz: fixed encoder size mismatch"));
        }
    } else {
        let table = count
            .checked_mul(w)
            .ok_or(PsszError::new("pssz: sequence table overflow"))?;
        append_zeros(out, table)?;
        let limit = offset_limit(w);
        for (i, value) in values.enumerate() {
            let offset = out.len().checked_sub(base).ok_or(SHRUNK)?;
            if offset > limit {
                return Err(OFFSET_OVERFLOW);
            }
            if i >= count {
                // More elements than the iterator said: no slot for this one.
                return Err(PsszError::new("pssz: sequence length mismatch"));
            }
            store_offset(out, base + i * w, w, offset);
            pack(value, out, depth + 1)?;
        }
        wire_span(out.len().checked_sub(base).ok_or(SHRUNK)?)?;
    }
    Ok(())
}
