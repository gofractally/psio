//! Bounds-checked layout helpers shared by PSSZ codecs and explicit derives.
use crate::pssz::{PsszError, PsszFormat, PsszPack, PsszResult, PsszUnpack};

thread_local! { static DECODE_DEPTH: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
thread_local! { static ENCODE_DEPTH: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
thread_local! {
    static ALLOW_UNKNOWN: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static SAW_UNKNOWN: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}
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
        Err(PsszError("pssz: unknown fields rejected by exact decoder"))
    }
}
pub struct DecodeDepth;
impl DecodeDepth {
    pub fn enter() -> PsszResult<Self> {
        DECODE_DEPTH.with(|depth| {
            // Root is node 0. Node 64 is allowed; a child at 65 is rejected.
            if depth.get() > 64 {
                return Err(PsszError("pssz: decode depth limit exceeded"));
            }
            depth.set(depth.get() + 1);
            Ok(Self)
        })
    }
}
impl Drop for DecodeDepth {
    fn drop(&mut self) {
        DECODE_DEPTH.with(|depth| depth.set(depth.get() - 1));
    }
}

/// Checked encoding uses the same node budget as the checked readers. Wire-
/// transparent wrappers must forward without creating another guard.
pub struct EncodeDepth;
impl EncodeDepth {
    pub fn enter() -> PsszResult<Self> {
        ENCODE_DEPTH.with(|depth| {
            if depth.get() > 64 {
                return Err(PsszError("pssz: encode depth limit exceeded"));
            }
            depth.set(depth.get() + 1);
            Ok(Self)
        })
    }
}
impl Drop for EncodeDepth {
    fn drop(&mut self) {
        ENCODE_DEPTH.with(|depth| depth.set(depth.get() - 1));
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Layout {
    pub fixed: bool,
    pub size: usize,
    pub optional: bool,
    pub omit: bool,
}

impl Layout {
    pub const fn of<F: PsszFormat, T: PsszPack<F>>() -> Self {
        Self {
            fixed: T::IS_FIXED_SIZE,
            size: T::FIXED_SIZE,
            optional: T::IS_OPTIONAL,
            omit: false,
        }
    }
    pub const fn slot<F: PsszFormat>(self) -> usize {
        if self.fixed {
            self.size
        } else {
            F::OFFSET_BYTES
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct FieldLocation {
    pub slot: usize,
    pub slot_size: usize,
    pub known_fixed: usize,
    pub next_variable: Option<usize>,
    pub fixed: bool,
    pub optional: bool,
}
pub const fn record_location<F: PsszFormat>(layouts: &[Layout], index: usize) -> FieldLocation {
    let mut slot = 0usize;
    let mut at = 0usize;
    let mut result = FieldLocation {
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
            result.slot_size = field.slot::<F>();
            result.fixed = field.fixed;
            result.optional = field.optional;
        }
        if at > index && !field.fixed && result.next_variable.is_none() {
            result.next_variable = Some(slot);
        }
        slot = slot.saturating_add(field.slot::<F>());
        at += 1;
    }
    result.known_fixed = slot;
    result
}

/// O(1) access after full validation. Omitted optional fields use static None
/// bytes rather than pretending that an empty span encodes every optional type.
pub fn field_bytes<'a, F: PsszFormat, T: PsszPack<F>>(
    bytes: &'a [u8],
    location: FieldLocation,
    dwnc: bool,
) -> PsszResult<&'a [u8]> {
    let header = if dwnc { 0 } else { F::HEADER_BYTES };
    let written_fixed = if dwnc {
        location.known_fixed
    } else {
        read_offset::<F>(bytes)?
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
    let begin = read_offset::<F>(&body[location.slot..])?;
    let end = match location.next_variable {
        Some(next)
            if next
                .checked_add(F::OFFSET_BYTES)
                .is_some_and(|end| end <= written_fixed) =>
        {
            read_offset::<F>(&body[next..])?
        }
        _ if written_fixed > location.known_fixed => {
            read_offset::<F>(&body[location.known_fixed..])?
        }
        _ => body.len(),
    };
    if begin < written_fixed || begin > end {
        return Err(PsszError("pssz view: invalid field offset"));
    }
    body.get(begin..end)
        .ok_or(PsszError("pssz view: field offset outside buffer"))
}

pub fn field<F: PsszFormat, T: PsszPack<F>>(value: &T) -> PsszResult<(Layout, Vec<u8>)> {
    let mut bytes = Vec::new();
    value.pssz_pack_checked(&mut bytes)?;
    if T::IS_FIXED_SIZE && bytes.len() != T::FIXED_SIZE {
        return Err(PsszError("pssz: fixed encoder size mismatch"));
    }
    let mut layout = Layout::of::<F, T>();
    layout.omit = T::IS_OPTIONAL && value.pssz_is_none();
    Ok((layout, bytes))
}

pub fn record_kept_fields(omitted: &[bool], dwnc: bool) -> usize {
    if dwnc {
        omitted.len()
    } else {
        omitted.iter().rposition(|omit| !omit).map_or(0, |i| i + 1)
    }
}

/// A pruned trailing None is a synthesized default, not a traversed wire node.
/// Compute the kept prefix before attempting to encode any child values.
pub fn field_if_kept<F: PsszFormat, T: PsszPack<F>>(
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

pub fn encode_record<F: PsszFormat>(
    fields: &[(Layout, Vec<u8>)],
    dwnc: bool,
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    format::<F>()?;
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
        n.checked_add(layout.slot::<F>())
            .ok_or(PsszError("pssz: fixed region overflow"))
    })?;
    let header = if dwnc { 0 } else { F::HEADER_BYTES };
    let size = header
        .checked_add(fixed)
        .ok_or(PsszError("pssz: record size overflow"))?;
    let mut result = Vec::new();
    result
        .try_reserve(size)
        .map_err(|_| PsszError("pssz: record allocation overflow"))?;
    result.resize(size, 0);
    if !dwnc {
        write_offset::<F>(&mut result[..header], fixed)?;
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
            write_offset::<F>(&mut result[cursor..cursor + F::OFFSET_BYTES], offset)?;
            result
                .try_reserve(bytes.len())
                .map_err(|_| PsszError("pssz: record allocation overflow"))?;
            result.extend_from_slice(bytes);
        }
        cursor += layout.slot::<F>();
    }
    out.try_reserve(result.len())
        .map_err(|_| PsszError("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
}

pub fn record_spans<F: PsszFormat>(
    layouts: &[Layout],
    bytes: &[u8],
    dwnc: bool,
) -> PsszResult<Vec<Option<std::ops::Range<usize>>>> {
    format::<F>()?;
    let header = if dwnc { 0 } else { F::HEADER_BYTES };
    let fixed = layouts.iter().try_fold(0usize, |n, layout| {
        n.checked_add(layout.slot::<F>())
            .ok_or(PsszError("pssz: fixed region overflow"))
    })?;
    let written_fixed = if dwnc {
        fixed
    } else {
        read_offset::<F>(bytes)?
    };
    let data = bytes
        .get(header..)
        .ok_or(PsszError("pssz: header underrun"))?;
    if data.len() < written_fixed {
        return Err(PsszError("pssz: fixed region underrun"));
    }
    if written_fixed > fixed && (written_fixed - fixed) % F::OFFSET_BYTES != 0 {
        return Err(PsszError("pssz: unaligned unknown optional suffix"));
    }
    let mut spans = vec![None; layouts.len()];
    let mut variables: Vec<(Option<usize>, usize)> = Vec::new();
    let mut cursor = 0usize;
    let mut missing = false;
    for (index, layout) in layouts.iter().enumerate() {
        let end = cursor
            .checked_add(layout.slot::<F>())
            .ok_or(PsszError("pssz: field boundary overflow"))?;
        if missing || end > written_fixed {
            if cursor != written_fixed || !layout.optional || dwnc {
                return Err(PsszError(
                    "pssz: missing required field or header inside field",
                ));
            }
            missing = true;
            continue;
        }
        if layout.fixed {
            spans[index] = Some(header + cursor..header + end);
        } else {
            variables.push((Some(index), read_offset::<F>(&data[cursor..])?));
        }
        cursor = end;
    }
    if written_fixed > fixed {
        for at in (fixed..written_fixed).step_by(F::OFFSET_BYTES) {
            variables.push((None, read_offset::<F>(&data[at..])?));
        }
    }
    let mut previous = written_fixed;
    for (position, &(index, begin)) in variables.iter().enumerate() {
        let end = variables
            .get(position + 1)
            .map(|(_, offset)| *offset)
            .unwrap_or(data.len());
        if (position == 0 && begin != written_fixed)
            || begin < previous
            || begin > end
            || end > data.len()
        {
            return Err(PsszError("pssz: noncanonical record offset"));
        }
        if let Some(index) = index {
            spans[index] = Some(header + begin..header + end);
        }
        previous = begin;
    }
    if variables.is_empty() && data.len() != written_fixed {
        return Err(PsszError("pssz: trailing record bytes"));
    }
    if written_fixed > fixed {
        unknown_fields()?;
    }
    Ok(spans)
}

pub fn decode_field<F: PsszFormat, T: PsszUnpack<F>>(
    bytes: &[u8],
    span: Option<std::ops::Range<usize>>,
) -> PsszResult<T> {
    match span {
        Some(span) => T::pssz_unpack(&bytes[span]),
        None => T::pssz_missing_optional(),
    }
}

pub fn format<F: PsszFormat>() -> PsszResult<()> {
    if !matches!(F::OFFSET_BYTES, 1 | 2 | 4) || F::HEADER_BYTES != F::OFFSET_BYTES {
        return Err(PsszError("pssz: unsupported explicit-width profile"));
    }
    Ok(())
}

pub fn read_offset<F: PsszFormat>(bytes: &[u8]) -> PsszResult<usize> {
    format::<F>()?;
    let b = bytes
        .get(..F::OFFSET_BYTES)
        .ok_or(PsszError("pssz: offset underrun"))?;
    Ok(match F::OFFSET_BYTES {
        1 => b[0] as usize,
        2 => u16::from_le_bytes([b[0], b[1]]) as usize,
        4 => u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize,
        _ => return Err(PsszError("pssz: invalid offset width")),
    })
}

pub fn write_offset<F: PsszFormat>(bytes: &mut [u8], value: usize) -> PsszResult<()> {
    format::<F>()?;
    if bytes.len() != F::OFFSET_BYTES || value as u128 > F::MAX_TOTAL as u128 {
        return Err(PsszError("pssz: offset width overflow"));
    }
    match F::OFFSET_BYTES {
        1 => bytes.copy_from_slice(
            &u8::try_from(value)
                .map_err(|_| PsszError("pssz: offset width overflow"))?
                .to_le_bytes(),
        ),
        2 => bytes.copy_from_slice(
            &u16::try_from(value)
                .map_err(|_| PsszError("pssz: offset width overflow"))?
                .to_le_bytes(),
        ),
        4 => bytes.copy_from_slice(
            &u32::try_from(value)
                .map_err(|_| PsszError("pssz: offset width overflow"))?
                .to_le_bytes(),
        ),
        _ => return Err(PsszError("pssz: invalid offset width")),
    }
    Ok(())
}

pub fn decode_sequence<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F>>(
    bytes: &[u8],
    count: Option<usize>,
) -> PsszResult<Vec<T>> {
    format::<F>()?;
    if T::IS_FIXED_SIZE {
        let size = T::FIXED_SIZE;
        let n = match count {
            Some(n) => n,
            None if bytes.is_empty() => 0,
            None if size == 0 => return Err(PsszError("pssz: zero-size list element")),
            None if bytes.len() % size == 0 => bytes.len() / size,
            None => return Err(PsszError("pssz: fixed sequence span mismatch")),
        };
        if n.checked_mul(size) != Some(bytes.len()) {
            return Err(PsszError("pssz: fixed sequence span mismatch"));
        }
        let mut out = Vec::new();
        out.try_reserve(n)
            .map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
        for i in 0..n {
            out.push(T::pssz_unpack(&bytes[i * size..(i + 1) * size])?);
        }
        return Ok(out);
    }
    let width = F::OFFSET_BYTES;
    let n = match count {
        Some(n) => n,
        None if bytes.is_empty() => 0,
        None => {
            let first = read_offset::<F>(bytes)?;
            if first == 0 || first % width != 0 || first > bytes.len() {
                return Err(PsszError("pssz: invalid sequence offset table"));
            }
            first / width
        }
    };
    let table = n
        .checked_mul(width)
        .ok_or(PsszError("pssz: sequence table overflow"))?;
    if table > bytes.len() || (n == 0 && !bytes.is_empty()) {
        return Err(PsszError("pssz: sequence table mismatch"));
    }
    let mut offsets = Vec::new();
    offsets
        .try_reserve(n)
        .map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
    let mut previous = table;
    for i in 0..n {
        let offset = read_offset::<F>(&bytes[i * width..])?;
        if (i == 0 && offset != table) || offset < previous || offset > bytes.len() {
            return Err(PsszError("pssz: noncanonical sequence offset"));
        }
        offsets.push(offset);
        previous = offset;
    }
    let mut out = Vec::new();
    out.try_reserve(n)
        .map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
    for i in 0..n {
        let end = offsets.get(i + 1).copied().unwrap_or(bytes.len());
        out.push(T::pssz_unpack(&bytes[offsets[i]..end])?);
    }
    Ok(out)
}

pub fn encode_sequence<F: PsszFormat, T: PsszPack<F>>(
    values: &[T],
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    encode_sequence_impl::<F, T>(values, out, false)
}

pub fn encode_array<F: PsszFormat, T: PsszPack<F>>(
    values: &[T],
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    encode_sequence_impl::<F, T>(values, out, true)
}

fn encode_sequence_impl<F: PsszFormat, T: PsszPack<F>>(
    values: &[T],
    out: &mut Vec<u8>,
    known_count: bool,
) -> PsszResult<()> {
    format::<F>()?;
    let mut result = Vec::new();
    if T::IS_FIXED_SIZE {
        if T::FIXED_SIZE == 0 && !values.is_empty() && !known_count {
            return Err(PsszError("pssz: nonempty zero-size sequence is ambiguous"));
        }
        values
            .len()
            .checked_mul(T::FIXED_SIZE)
            .ok_or(PsszError("pssz: sequence size overflow"))?;
        for value in values {
            value.pssz_pack_checked(&mut result)?;
        }
    } else {
        let table = values
            .len()
            .checked_mul(F::OFFSET_BYTES)
            .ok_or(PsszError("pssz: sequence table overflow"))?;
        if table as u128 > F::MAX_TOTAL as u128 {
            return Err(PsszError("pssz: offset width overflow"));
        }
        result
            .try_reserve(table)
            .map_err(|_| PsszError("pssz: sequence allocation overflow"))?;
        result.resize(table, 0);
        for (i, value) in values.iter().enumerate() {
            let offset = result.len();
            write_offset::<F>(
                &mut result[i * F::OFFSET_BYTES..(i + 1) * F::OFFSET_BYTES],
                offset,
            )?;
            value.pssz_pack_checked(&mut result)?;
        }
    }
    out.try_reserve(result.len())
        .map_err(|_| PsszError("pssz: output allocation overflow"))?;
    out.extend_from_slice(&result);
    Ok(())
}
