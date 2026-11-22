// The text forms of conformance/custom-types.md, written against plain
// values: shared by the typed mirror types (types.rs: `TextForm`) and by the
// handlers a schema-only consumer registers (handlers.rs).

/// `0`, or a non-zero digit followed by digits: no leading zero, no sign.
pub fn canonical_u64(text: &str) -> Option<u64> {
    let canonical = !text.is_empty()
        && text.bytes().all(|b| b.is_ascii_digit())
        && (text == "0" || !text.starts_with('0'));
    if canonical { text.parse().ok() } else { None }
}

const BASE64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// RFC 4648 section 4, with `=` padding.
pub fn base64_encode(bytes: &[u8]) -> String {
    let mut out = String::new();
    for chunk in bytes.chunks(3) {
        let n = u32::from(chunk[0]) << 16
            | u32::from(*chunk.get(1).unwrap_or(&0)) << 8
            | u32::from(*chunk.get(2).unwrap_or(&0));
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(BASE64[((n >> (18 - 6 * i)) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}
/// Exactly the text `base64_encode` writes: a multiple of 4 characters,
/// padding only at the end of the last group, unused bits zero.
pub fn base64_decode(text: &str) -> Option<Vec<u8>> {
    let bytes = text.as_bytes();
    if bytes.len() % 4 != 0 {
        return None;
    }
    let mut out = Vec::new();
    for (index, group) in bytes.chunks(4).enumerate() {
        let last = index + 1 == bytes.len() / 4;
        let padding = group.iter().rev().take_while(|c| **c == b'=').count();
        if padding > 2 || (padding > 0 && !last) {
            return None;
        }
        let mut n = 0u32;
        for &c in &group[..4 - padding] {
            n = (n << 6) | BASE64.iter().position(|a| *a == c)? as u32;
        }
        n <<= 6 * padding as u32;
        let data = 3 - padding;
        // The bits a padded group does not use are zero.
        if n & ((1u32 << (8 * padding as u32)) - 1) != 0 {
            return None;
        }
        out.extend_from_slice(&n.to_be_bytes()[1..1 + data]);
    }
    Some(out)
}

#[derive(Debug, Clone, PartialEq)]
pub enum EndpointParts {
    V4([u8; 4], u16),
    V6([u8; 16], u16),
}
fn port(text: &str) -> Option<u16> {
    canonical_u64(text).and_then(|port| u16::try_from(port).ok())
}
/// `a.b.c.d:port`, or `[g:g:g:g:g:g:g:g]:port` with eight groups of exactly
/// four lowercase hex digits.
pub fn endpoint_to_text(endpoint: &EndpointParts) -> String {
    match endpoint {
        EndpointParts::V4(a, port) => format!("{}.{}.{}.{}:{port}", a[0], a[1], a[2], a[3]),
        EndpointParts::V6(a, port) => {
            let groups: Vec<String> =
                a.chunks(2).map(|pair| format!("{:02x}{:02x}", pair[0], pair[1])).collect();
            format!("[{}]:{port}", groups.join(":"))
        }
    }
}
pub fn endpoint_from_text(text: &str) -> Option<EndpointParts> {
    if let Some(rest) = text.strip_prefix('[') {
        let (groups, rest) = rest.split_once(']')?;
        let port = port(rest.strip_prefix(':')?)?;
        let groups: Vec<&str> = groups.split(':').collect();
        if groups.len() != 8 {
            return None;
        }
        let mut address = [0u8; 16];
        for (i, group) in groups.iter().enumerate() {
            let lower = group.len() == 4
                && group.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c));
            if !lower {
                return None;
            }
            let value = u16::from_str_radix(group, 16).ok()?;
            address[2 * i..2 * i + 2].copy_from_slice(&value.to_be_bytes());
        }
        return Some(EndpointParts::V6(address, port));
    }
    let (address, rest) = text.split_once(':')?;
    let port = port(rest)?;
    let octets: Vec<&str> = address.split('.').collect();
    if octets.len() != 4 {
        return None;
    }
    let mut out = [0u8; 4];
    for (slot, octet) in out.iter_mut().zip(octets) {
        *slot = u8::try_from(canonical_u64(octet)?).ok()?;
    }
    Some(EndpointParts::V4(out, port))
}

/// `n/d`: `n` is `0 | -?[1-9][0-9]*` within i16, `d` a canonical u16.
pub fn ratio_to_text(n: i16, d: u16) -> String {
    format!("{n}/{d}")
}
pub fn ratio_from_text(text: &str) -> Option<(i16, u16)> {
    let (n, d) = text.split_once('/')?;
    let magnitude = canonical_u64(n.strip_prefix('-').unwrap_or(n))?;
    if n.starts_with('-') && magnitude == 0 {
        return None;
    }
    let n = if n.starts_with('-') { -(magnitude as i64) } else { magnitude as i64 };
    Some((i16::try_from(n).ok()?, u16::try_from(canonical_u64(d)?).ok()?))
}

#[derive(Debug, Clone, PartialEq)]
pub enum RefParts {
    Number(u64),
    Name(Vec<u8>),
}
/// `#` and the canonical decimal, or `@` and the name verbatim. A name that
/// is not UTF-8 has no text.
pub fn ref_to_text(target: &RefParts) -> Option<String> {
    match target {
        RefParts::Number(n) => Some(format!("#{n}")),
        RefParts::Name(name) => Some(format!("@{}", std::str::from_utf8(name).ok()?)),
    }
}
pub fn ref_from_text(text: &str) -> Option<RefParts> {
    if let Some(digits) = text.strip_prefix('#') {
        return canonical_u64(digits).map(RefParts::Number);
    }
    text.strip_prefix('@').map(|name| RefParts::Name(name.as_bytes().to_vec()))
}

// The invariants' messages.
pub const NAME: &str = "Name: value is not a canonical name";
pub const DER_KEY: &str = "DerKey: not a DER sequence";
pub const RANGE: &str = "Range: lo is greater than hi";
pub const LEVEL: &str = "Level: not a listed level";
pub const REF: &str = "Ref: a name is not empty";
