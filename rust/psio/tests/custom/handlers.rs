// The handlers of the fixtures' custom types (conformance/custom-types.md),
// written by hand against the PSSZ bytes of their wire types (automatic
// profile), as a consumer holding only the schema writes them. The typed
// tests register the mirror types' own `TextForm` / invariants instead.

use super::text::*;
use psio::pssz::dynamic::CustomHandlers;
use psio::pssz::PsszError;

/// The ids of the custom nodes these handlers cover.
pub const CUSTOM_IDS: [&str; 8] =
    ["Blob", "Endpoint", "Ratio", "Ref", "Name", "DerKey", "Range", "Level"];

const NOT_WIRE: PsszError = PsszError("handler: bytes are not the wire type");
const NOT_TEXT: PsszError = PsszError("handler: text is not the presentation");

/// variant<V4, V6>: the index byte, the address bytes, the port (u16 LE).
fn endpoint_of(bytes: &[u8]) -> Result<EndpointParts, PsszError> {
    let port = |at: usize| u16::from_le_bytes([bytes[at], bytes[at + 1]]);
    match (bytes.first(), bytes.len()) {
        (Some(0), 7) => Ok(EndpointParts::V4(bytes[1..5].try_into().unwrap(), port(5))),
        (Some(1), 19) => Ok(EndpointParts::V6(bytes[1..17].try_into().unwrap(), port(17))),
        _ => Err(NOT_WIRE),
    }
}
/// variant<u64, raw_string>: the index byte, then 8 LE bytes or the name.
fn ref_of(bytes: &[u8]) -> Result<RefParts, PsszError> {
    match bytes.split_first() {
        Some((0, rest)) => {
            Ok(RefParts::Number(u64::from_le_bytes(rest.try_into().map_err(|_| NOT_WIRE)?)))
        }
        Some((1, rest)) => Ok(RefParts::Name(rest.to_vec())),
        _ => Err(NOT_WIRE),
    }
}

pub fn fixture_custom_handlers(handlers: &mut CustomHandlers) {
    // Blob: a byte list (its bytes) as base64.
    handlers.add(
        "Blob",
        |bytes: &[u8]| Ok(base64_encode(bytes)),
        |text: &str| base64_decode(text).ok_or(NOT_TEXT),
    );
    handlers.add(
        "Endpoint",
        |bytes: &[u8]| Ok(endpoint_to_text(&endpoint_of(bytes)?)),
        |text: &str| {
            Ok(match endpoint_from_text(text).ok_or(NOT_TEXT)? {
                EndpointParts::V4(address, port) => {
                    [&[0u8][..], &address[..], &port.to_le_bytes()[..]].concat()
                }
                EndpointParts::V6(address, port) => {
                    [&[1u8][..], &address[..], &port.to_le_bytes()[..]].concat()
                }
            })
        },
    );
    // Ratio: tuple<i16, u16>, an unbounded tuple: a 4-byte header holding
    // the fixed region's size (4), then the two members.
    handlers.add(
        "Ratio",
        |bytes: &[u8]| match bytes {
            [4, 0, 0, 0, a, b, c, d] => {
                Ok(ratio_to_text(i16::from_le_bytes([*a, *b]), u16::from_le_bytes([*c, *d])))
            }
            _ => Err(NOT_WIRE),
        },
        |text: &str| {
            let (n, d) = ratio_from_text(text).ok_or(NOT_TEXT)?;
            Ok([&[4u8, 0, 0, 0][..], &n.to_le_bytes()[..], &d.to_le_bytes()[..]].concat())
        },
    );
    handlers.add(
        "Ref",
        |bytes: &[u8]| ref_to_text(&ref_of(bytes)?).ok_or(NOT_WIRE),
        |text: &str| {
            Ok(match ref_from_text(text).ok_or(NOT_TEXT)? {
                RefParts::Number(n) => [&[0u8][..], &n.to_le_bytes()[..]].concat(),
                RefParts::Name(name) => [&[1u8][..], &name[..]].concat(),
            })
        },
    );
    // Invariants.
    handlers.add_check("Ref", |bytes: &[u8]| match ref_of(bytes)? {
        RefParts::Name(name) if name.is_empty() => Err(PsszError(REF)),
        _ => Ok(()),
    });
    // Name: the fixed-layout record {value: u64}: 8 LE bytes.
    handlers.add_check("Name", |bytes: &[u8]| {
        let value = u64::from_le_bytes(bytes.try_into().map_err(|_| NOT_WIRE)?);
        if value >> 60 == 0 { Ok(()) } else { Err(PsszError(NAME)) }
    });
    handlers.add_check("DerKey", |bytes: &[u8]| {
        if bytes.first() == Some(&0x30) { Ok(()) } else { Err(PsszError(DER_KEY)) }
    });
    // Range: the record {lo: u32, hi: u32} at one-byte offsets: the header
    // (8), then the two members.
    handlers.add_check("Range", |bytes: &[u8]| match bytes {
        [8, rest @ ..] if rest.len() == 8 => {
            let lo = u32::from_le_bytes(rest[..4].try_into().unwrap());
            let hi = u32::from_le_bytes(rest[4..].try_into().unwrap());
            if lo <= hi { Ok(()) } else { Err(PsszError(RANGE)) }
        }
        _ => Err(NOT_WIRE),
    });
    handlers.add_check("Level", |bytes: &[u8]| match bytes {
        [level] if *level <= 3 => Ok(()),
        [_] => Err(PsszError(LEVEL)),
        _ => Err(NOT_WIRE),
    });
}
