//! Bounds-checked layout helpers shared by the PSSZ codecs, the declarative
//! record macros, the proc-macro derives, the borrowed views and the
//! schema-driven codec.
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
use crate::pssz::{PsszError, PsszFormat, PsszPack, PsszResult, PsszUnpack};
use std::ops::Range;

/// Deepest accepted node. The root is node 0; every present record field,
/// sequence element, `Some` payload and variant payload is one deeper. Matches
/// C++ `kMaxValidationDepth`.
pub const MAX_DEPTH: usize = 64;

thread_local! { static DECODE_DEPTH: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
thread_local! { static ENCODE_DEPTH: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
thread_local! {
    // Outside a session (a direct trait call) unknown optional suffix fields
    // are accepted, as C++ `decode` / `validate` do.
    static ALLOW_UNKNOWN: std::cell::Cell<bool> = const { std::cell::Cell::new(true) };
    static SAW_UNKNOWN: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// Scope of one top-level decode or validation. `allow == false` is the exact
/// reader: an unknown optional suffix field anywhere in the tree is an error.
pub struct DecodeSession {
    previous_allow: bool,
    previous_unknown: bool,
}
impl DecodeSession {
    pub fn enter(allow: bool) -> Self {
        Self {
            previous_allow: ALLOW_UNKNOWN.with(|v| v.replace(allow)),
            previous_unknown: SAW_UNKNOWN.with(|v| v.replace(false)),
        }
    }
    pub fn has_unknown(&self) -> bool {
        SAW_UNKNOWN.with(|v| v.get())
    }
}
impl Drop for DecodeSession {
    fn drop(&mut self) {
        ALLOW_UNKNOWN.with(|v| v.set(self.previous_allow));
        SAW_UNKNOWN.with(|v| v.set(self.previous_unknown || v.get()));
    }
}
fn unknown_fields() -> PsszResult<()> {
    SAW_UNKNOWN.with(|v| v.set(true));
    if ALLOW_UNKNOWN.with(|v| v.get()) {
        Ok(())
    } else {
        Err(PsszError("pssz: unknown fields rejected by exact reader"))
    }
}

/// Exact (strict) readers require raw strings to be UTF-8, as C++
/// `validate_strict` does. Every other reader passes raw bytes through.
pub fn strict_strings() -> bool {
    !ALLOW_UNKNOWN.with(|v| v.get())
}

pub struct DecodeDepth;
impl DecodeDepth {
    pub fn enter() -> PsszResult<Self> {
        DECODE_DEPTH.with(|depth| {
            // Root is node 0. Node 64 is allowed; a child at 65 is rejected.
            if depth.get() > MAX_DEPTH {
                return Err(PsszError("pssz: max depth exceeded"));
            }
            depth.set(depth.get() + 1);
            Ok(Self)
        })
    }
}
impl Drop for DecodeDepth {
    fn drop(&mut self) {
        DECODE_DEPTH.with(|depth| depth.set(depth.get().saturating_sub(1)));
    }
}

/// Checked encoding uses the same node budget as the checked readers. Wire-
/// transparent wrappers must forward without creating another guard.
pub struct EncodeDepth;
impl EncodeDepth {
    pub fn enter() -> PsszResult<Self> {
        ENCODE_DEPTH.with(|depth| {
            if depth.get() > MAX_DEPTH {
                return Err(PsszError("pssz: max depth exceeded"));
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
    !WRITE_UNCHECKED.with(|flag| flag.get())
}

// ── Formats and offsets ─────────────────────────────────────────────────────

pub fn format<F: PsszFormat>() -> PsszResult<()> {
    if !matches!(F::OFFSET_BYTES, 1 | 2 | 4) || F::HEADER_BYTES != F::OFFSET_BYTES {
        return Err(PsszError("pssz: unsupported offset/header width"));
    }
    Ok(())
}

fn width(w: usize) -> PsszResult<()> {
    if matches!(w, 1 | 2 | 4) {
        Ok(())
    } else {
        Err(PsszError("pssz: unsupported offset/header width"))
    }
}

/// C++ `wire_span_fits`: a variable payload is addressed by at most 32 bits.
pub fn wire_span(len: usize) -> PsszResult<()> {
    if len as u128 > u32::MAX as u128 {
        return Err(PsszError(
            "pssz: variable payload exceeds 32-bit address space",
        ));
    }
    Ok(())
}

pub fn read_offset_w(bytes: &[u8], w: usize) -> PsszResult<usize> {
    let b = bytes.get(..w).ok_or(PsszError("pssz: offset underrun"))?;
    Ok(match w {
        1 => b[0] as usize,
        2 => u16::from_le_bytes([b[0], b[1]]) as usize,
        4 => u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize,
        _ => return Err(PsszError("pssz: unsupported offset/header width")),
    })
}

pub fn write_offset_w(bytes: &mut [u8], w: usize, value: usize) -> PsszResult<()> {
    if bytes.len() != w {
        return Err(PsszError("pssz: offset width overflow"));
    }
    const OVERFLOW: PsszError = PsszError("pssz: value exceeds the static offset width");
    match w {
        1 => bytes.copy_from_slice(&u8::try_from(value).map_err(|_| OVERFLOW)?.to_le_bytes()),
        2 => bytes.copy_from_slice(&u16::try_from(value).map_err(|_| OVERFLOW)?.to_le_bytes()),
        4 => bytes.copy_from_slice(&u32::try_from(value).map_err(|_| OVERFLOW)?.to_le_bytes()),
        _ => return Err(PsszError("pssz: unsupported offset/header width")),
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
pub fn check_cap(cap: Option<usize>, len: usize) -> PsszResult<()> {
    match cap {
        Some(cap) if len > cap => Err(PsszError(
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

/// C++ `max_encoded_size` for a record: header (unless DWNC) plus, per field,
/// its bound and a four-byte slot when it is not stored inline. Bookkeeping is
/// always four bytes wide, whatever width is finally selected.
pub const fn record_bound(fields: &[(Option<usize>, bool)], dwnc: bool) -> Option<usize> {
    let mut total: usize = if dwnc { 0 } else { 4 };
    let mut at = 0;
    while at < fields.len() {
        let (bound, fixed) = fields[at];
        let bound = match bound {
            Some(b) => b,
            None => return None,
        };
        let slot = if fixed { 0 } else { 4 };
        total = match total.checked_add(bound) {
            Some(t) => match t.checked_add(slot) {
                Some(t) => t,
                None => return None,
            },
            None => return None,
        };
        at += 1;
    }
    Some(total)
}

/// C++ `max_encoded_size` for `count` elements (array, bounded list).
pub const fn sequence_bound(count: usize, element: Option<usize>, fixed: bool) -> Option<usize> {
    if count == 0 {
        return Some(0);
    }
    let element = match element {
        Some(e) => e,
        None => return None,
    };
    let each = match element.checked_add(if fixed { 0 } else { 4 }) {
        Some(e) => e,
        None => return None,
    };
    count.checked_mul(each)
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
pub fn record_spans_into(
    w: usize,
    layouts: &[Layout],
    bytes: &[u8],
    dwnc: bool,
    spans: &mut [Option<Range<usize>>],
) -> PsszResult<bool> {
    width(w)?;
    if layouts.len() != spans.len() {
        return Err(PsszError("pssz: invalid record span"));
    }
    for span in spans.iter_mut() {
        *span = None;
    }
    let mut known = 0usize;
    for layout in layouts {
        if layout.optional && layout.fixed {
            return Err(PsszError("pssz: invalid reader field layout"));
        }
        known = known
            .checked_add(layout.slot(w))
            .ok_or(PsszError("pssz: invalid reader field layout"))?;
    }
    let mut fixed_start = 0usize;
    let mut fixed_size = known;
    if !dwnc {
        if bytes.len() < w {
            return Err(PsszError("pssz: record header truncated"));
        }
        fixed_size = read_offset_w(bytes, w)?;
        fixed_start = w;
    }
    let payload_size = bytes.len() - fixed_start;
    if fixed_size > payload_size {
        return Err(PsszError("pssz: record fixed region truncated"));
    }
    if fixed_size > known && (fixed_size - known) % w != 0 {
        return Err(PsszError("pssz: partial unknown optional slot"));
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
            return Err(PsszError("pssz: invalid record payload offset"));
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
                return Err(PsszError("pssz: header cuts a known field"));
            }
            if !layout.optional || dwnc {
                return Err(PsszError("pssz: required record field missing"));
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
        return Err(PsszError("pssz: fixed record has trailing payload"));
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
                        return Err(PsszError(
                            "pssz: trailing absent optional field was not pruned",
                        ));
                    }
                }
            }
        }
    }
    Ok(unknown)
}

/// Stack-array frame resolution for generated record readers. Reports
/// unknown fields to the active session (an exact reader fails here).
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
        .map_err(|_| PsszError("pssz: record allocation overflow"))?;
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
                .ok_or(PsszError("pssz: field span out of range"))?,
        ),
        None => T::pssz_missing_optional(),
    }
}

pub fn field<F: PsszFormat, T: PsszPack<F> + ?Sized>(value: &T) -> PsszResult<(Layout, Vec<u8>)> {
    let mut bytes = Vec::new();
    value.pssz_pack_checked(&mut bytes)?;
    if T::IS_FIXED_SIZE && bytes.len() != T::FIXED_SIZE {
        return Err(PsszError("pssz: fixed encoder size mismatch"));
    }
    let mut layout = Layout::of::<F, T>();
    layout.omit = T::IS_OPTIONAL && value.pssz_is_none();
    Ok((layout, bytes))
}

/// Number of leading fields the encoder writes: everything up to and
/// including the last field that is not an absent optional (all, for DWNC).
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
            return Err(PsszError("pssz: cannot prune required or present field"));
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
            .ok_or(PsszError("pssz: fixed region overflow"))
    })?;
    let header = if dwnc { 0 } else { w };
    let size = header
        .checked_add(fixed)
        .ok_or(PsszError("pssz: record size overflow"))?;
    let mut result = Vec::new();
    result
        .try_reserve(size)
        .map_err(|_| PsszError("pssz: record allocation overflow"))?;
    result.resize(size, 0);
    if !dwnc {
        write_offset_w(&mut result[..header], w, fixed)?;
    }
    let mut cursor = header;
    for (layout, bytes) in fields {
        if layout.fixed {
            if layout.size != bytes.len() {
                return Err(PsszError("pssz: fixed field size mismatch"));
            }
            result[cursor..cursor + layout.size].copy_from_slice(bytes);
        } else {
            let offset = result.len() - header;
            write_offset_w(&mut result[cursor..cursor + w], w, offset)?;
            result
                .try_reserve(bytes.len())
                .map_err(|_| PsszError("pssz: record allocation overflow"))?;
            result.extend_from_slice(bytes);
        }
        cursor += layout.slot(w);
    }
    wire_span(result.len() - header)?;
    out.try_reserve(result.len())
        .map_err(|_| PsszError("pssz: output allocation overflow"))?;
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
        .map_err(|_| PsszError("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
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
        .ok_or(PsszError("pssz view: header underrun"))?;
    if written_fixed > body.len() {
        return Err(PsszError("pssz view: fixed region underrun"));
    }
    let end_slot = location
        .slot
        .checked_add(location.slot_size)
        .ok_or(PsszError("pssz view: field location overflow"))?;
    if end_slot > written_fixed {
        if dwnc || location.slot < written_fixed || !location.optional {
            return Err(PsszError("pssz view: absent required field"));
        }
        return T::pssz_none_bytes().ok_or(PsszError("pssz view: optional default unavailable"));
    }
    if location.fixed {
        return body
            .get(location.slot..end_slot)
            .ok_or(PsszError("pssz view: field span mismatch"));
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
        return Err(PsszError("pssz view: invalid field offset"));
    }
    body.get(begin..end)
        .ok_or(PsszError("pssz view: field offset outside buffer"))
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
                return Err(PsszError(
                    "pssz: nonempty zero-size vector is unsupported",
                ))
            }
            None if bytes.len() % size == 0 => bytes.len() / size,
            None => return Err(PsszError("pssz: fixed sequence span mismatch")),
        };
        if n.checked_mul(size) != Some(bytes.len()) {
            return Err(PsszError(
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
                return Err(PsszError("pssz: invalid sequence offset table"));
            }
            first / w
        }
    };
    let table = n
        .checked_mul(w)
        .ok_or(PsszError("pssz: sequence table overflow"))?;
    if table > bytes.len() || (n == 0 && !bytes.is_empty()) {
        return Err(PsszError("pssz: sequence table mismatch"));
    }
    wire_span(bytes.len())?;
    let mut previous = table;
    for i in 0..n {
        let begin = read_offset_w(&bytes[i * w..], w)?;
        if (i == 0 && begin != table) || begin < previous || begin > bytes.len() {
            return Err(PsszError("pssz: sequence offset out of range"));
        }
        previous = begin;
    }
    Ok(n)
}

/// Callers first check `sequence_len` on this same byte span and count.
pub fn sequence_span<F: PsszFormat, T: PsszPack<F>>(
    w: usize,
    bytes: &[u8],
    count: usize,
    index: usize,
) -> PsszResult<Range<usize>> {
    if index >= count {
        return Err(PsszError("pssz: sequence index out of range"));
    }
    if T::IS_FIXED_SIZE {
        let begin = index
            .checked_mul(T::FIXED_SIZE)
            .ok_or(PsszError("pssz: element offset overflow"))?;
        let end = begin
            .checked_add(T::FIXED_SIZE)
            .ok_or(PsszError("pssz: element end overflow"))?;
        if end > bytes.len() {
            return Err(PsszError("pssz: element truncated"));
        }
        return Ok(begin..end);
    }
    width(w)?;
    let slot = index
        .checked_mul(w)
        .ok_or(PsszError("pssz: offset slot overflow"))?;
    let begin = read_offset_w(
        bytes
            .get(slot..)
            .ok_or(PsszError("pssz: offset slot truncated"))?,
        w,
    )?;
    let end = if index + 1 < count {
        let next = slot
            .checked_add(w)
            .ok_or(PsszError("pssz: next slot overflow"))?;
        read_offset_w(
            bytes
                .get(next..)
                .ok_or(PsszError("pssz: next slot truncated"))?,
            w,
        )?
    } else {
        bytes.len()
    };
    if begin > end || end > bytes.len() {
        return Err(PsszError("pssz: element span out of range"));
    }
    Ok(begin..end)
}

/// A Vec owns each initialized element throughout errors and unwinding.
pub fn decode_sequence<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
    w: usize,
    bytes: &[u8],
    count: Option<usize>,
) -> PsszResult<Vec<T>> {
    let n = sequence_len::<F, T>(w, bytes, count)?;
    let mut result = Vec::new();
    result
        .try_reserve(n)
        .map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
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
    width(w)?;
    let mut result = Vec::new();
    if T::IS_FIXED_SIZE {
        if T::FIXED_SIZE == 0 && !values.is_empty() && !known_count {
            return Err(PsszError(
                "pssz: nonempty zero-size vector is unsupported",
            ));
        }
        values
            .len()
            .checked_mul(T::FIXED_SIZE)
            .ok_or(PsszError("pssz: sequence size overflow"))?;
        for value in values {
            let before = result.len();
            value.pssz_pack_checked(&mut result)?;
            if result.len() - before != T::FIXED_SIZE {
                return Err(PsszError("pssz: fixed encoder size mismatch"));
            }
        }
    } else {
        let table = values
            .len()
            .checked_mul(w)
            .ok_or(PsszError("pssz: sequence table overflow"))?;
        result
            .try_reserve(table)
            .map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
        result.resize(table, 0);
        for (i, value) in values.iter().enumerate() {
            let offset = result.len();
            write_offset_w(&mut result[i * w..(i + 1) * w], w, offset)?;
            value.pssz_pack_checked(&mut result)?;
        }
        wire_span(result.len())?;
    }
    out.try_reserve(result.len())
        .map_err(|_| PsszError("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
}
