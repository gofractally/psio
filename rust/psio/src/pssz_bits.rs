//! Packed-bit invariants shared by typed and actual-IR PSSZ codecs.
use crate::pssz::{PsszError, PsszResult};

pub const fn byte_count(count: usize) -> usize {
    count / 8 + if count % 8 == 0 { 0 } else { 1 }
}

pub fn validate_data(bytes: &[u8], count: usize) -> PsszResult<()> {
    if bytes.len() != byte_count(count) {
        return Err(PsszError::new("pssz: packed bit byte count mismatch"));
    }
    if count % 8 != 0
        && bytes
            .last()
            .is_some_and(|b| b & !((1u8 << (count % 8)) - 1) != 0)
    {
        return Err(PsszError::new("pssz: packed bits above logical count"));
    }
    Ok(())
}

pub fn list_count(wire: &[u8], bound: usize) -> PsszResult<usize> {
    let last = *wire
        .last()
        .ok_or(PsszError::new("pssz: bitlist delimiter missing"))?;
    if last == 0 {
        return Err(PsszError::new("pssz: noncanonical bitlist delimiter"));
    }
    let highest = 7 - last.leading_zeros() as usize;
    let count = (wire.len() - 1)
        .checked_mul(8)
        .and_then(|n| n.checked_add(highest))
        .ok_or(PsszError::new("pssz: bitlist count overflow"))?;
    if count > bound && crate::pssz_checked::declared() {
        return Err(PsszError::new("pssz: bitlist count exceeds bound"));
    }
    Ok(count)
}

pub fn encode_list(data: &[u8], count: usize, bound: usize) -> PsszResult<Vec<u8>> {
    if count > bound && crate::pssz_checked::declared() {
        return Err(PsszError::new("pssz: bitlist count exceeds bound"));
    }
    validate_data(data, count)?;
    let mut result = Vec::new();
    result
        .try_reserve(count / 8 + 1)
        .map_err(|_| PsszError::new("pssz: bitlist allocation overflow"))?;
    result.extend_from_slice(data);
    if count % 8 == 0 {
        result.push(1);
    } else {
        *result
            .last_mut()
            .ok_or(PsszError::new("pssz: bitlist data missing"))? |= 1u8 << (count % 8);
    }
    Ok(result)
}

pub fn decode_list(wire: &[u8], bound: usize) -> PsszResult<(Vec<u8>, usize)> {
    let count = list_count(wire, bound)?;
    let mut data = Vec::new();
    data.try_reserve(byte_count(count))
        .map_err(|_| PsszError::new("pssz: bitlist allocation overflow"))?;
    data.extend_from_slice(&wire[..byte_count(count)]);
    if count % 8 != 0 {
        *data
            .last_mut()
            .ok_or(PsszError::new("pssz: bitlist data missing"))? &= (1u8 << (count % 8)) - 1;
    }
    Ok((data, count))
}
