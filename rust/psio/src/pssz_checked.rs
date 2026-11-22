//! Bounds-checked layout helpers shared by PSSZ codecs and explicit derives.
use crate::pssz::{PsszError, PsszFormat, PsszPack, PsszResult, PsszUnpack};

#[derive(Clone, Copy, Debug)]
pub struct Layout {
    pub fixed: bool,
    pub size: usize,
}

impl Layout {
    pub const fn of<F: PsszFormat, T: PsszPack<F>>() -> Self {
        Self {
            fixed: T::IS_FIXED_SIZE,
            size: T::FIXED_SIZE,
        }
    }
    pub fn slot<F: PsszFormat>(self) -> usize {
        if self.fixed {
            self.size
        } else {
            F::OFFSET_BYTES
        }
    }
}

pub fn field<F: PsszFormat, T: PsszPack<F>>(value: &T) -> PsszResult<(Layout, Vec<u8>)> {
    let mut bytes = Vec::new();
    value.pssz_pack_checked(&mut bytes)?;
    if T::IS_FIXED_SIZE && bytes.len() != T::FIXED_SIZE {
        return Err(PsszError("pssz: fixed encoder size mismatch"));
    }
    Ok((Layout::of::<F, T>(), bytes))
}

pub fn encode_record<F: PsszFormat>(
    fields: &[(Layout, Vec<u8>)],
    dwnc: bool,
    out: &mut Vec<u8>,
) -> PsszResult<()> {
    format::<F>()?;
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
) -> PsszResult<Vec<std::ops::Range<usize>>> {
    format::<F>()?;
    let header = if dwnc { 0 } else { F::HEADER_BYTES };
    let fixed = layouts.iter().try_fold(0usize, |n, layout| {
        n.checked_add(layout.slot::<F>())
            .ok_or(PsszError("pssz: fixed region overflow"))
    })?;
    if !dwnc && read_offset::<F>(bytes)? != fixed {
        return Err(PsszError(
            "pssz: unsupported record schema or noncanonical header",
        ));
    }
    let data = bytes
        .get(header..)
        .ok_or(PsszError("pssz: header underrun"))?;
    if data.len() < fixed {
        return Err(PsszError("pssz: fixed region underrun"));
    }
    let mut spans = Vec::with_capacity(layouts.len());
    let mut variable = Vec::new();
    let mut cursor = 0usize;
    for (i, layout) in layouts.iter().enumerate() {
        if layout.fixed {
            spans.push(header + cursor..header + cursor + layout.size);
        } else {
            let offset = read_offset::<F>(&data[cursor..])?;
            variable.push((i, offset));
            spans.push(0..0);
        }
        cursor += layout.slot::<F>();
    }
    let mut previous = fixed;
    for (n, &(index, begin)) in variable.iter().enumerate() {
        let end = variable
            .get(n + 1)
            .map(|(_, start)| *start)
            .unwrap_or(data.len());
        if (n == 0 && begin != fixed) || begin < previous || begin > end || end > data.len() {
            return Err(PsszError("pssz: noncanonical record offset"));
        }
        spans[index] = header + begin..header + end;
        previous = begin;
    }
    if variable.is_empty() && data.len() != fixed {
        return Err(PsszError("pssz: trailing record bytes"));
    }
    Ok(spans)
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
    format::<F>()?;
    let mut result = Vec::new();
    if T::IS_FIXED_SIZE {
        if T::FIXED_SIZE == 0 && !values.is_empty() {
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
