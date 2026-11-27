//! One generic function, every format (gofractally/psio#18; C++
//! `cpp/tests/format_concept_tests.cpp`).
//!
//! `exercise::<F, T>` is written once against the traits of
//! `psio::format` — `Format`, `Codec<F>` (= `Encode<F> + Decode<F> +
//! Validate<F>`) — and is instantiated for each format through a type
//! alias: the write, the validated write, the size, validation, the
//! validating read, a buffer, and bytes that are not a frame. Views are
//! exercised through `ViewFormat<T>` for the formats that implement it.
//!
//! What a format does not have is written down: `EXPECTED` states each
//! format's capabilities and is held against the trait constants and
//! against which generic functions are instantiated for it. A row changes
//! when the format does.

use psio::{Buffer, Codec, DefaultSafe, Format, TrustedRead};
use std::fmt::Debug;

#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key, psio::JsonText)]
#[cfg_attr(feature = "capnp", derive(psio::CapnpPack, psio::CapnpUnpack))]
#[cfg_attr(feature = "flatbuf", derive(psio::FbPack, psio::FbUnpack))]
#[cfg_attr(feature = "wit", derive(psio::WitPack, psio::WitUnpack))]
#[cfg_attr(feature = "protobuf", derive(psio::Protobuf))]
struct Record {
    id: u32,
    name: String,
    values: Vec<u32>,
    active: bool,
}
#[cfg(feature = "ssz")]
psio::ssz_struct!(Record { id: u32, name: String, values: Vec<u32>, active: bool });
#[cfg(feature = "pjson")]
psio::pjson_struct!(Record { id: u32, name: String, values: Vec<u32>, active: bool });

fn sample() -> Record {
    Record { id: 42, name: "Alice".into(), values: vec![1, 256, 65536], active: true }
}

/// What a format has today.
#[derive(Debug, PartialEq)]
struct Capabilities {
    name: &'static str,
    text: bool,
    trusted_read: TrustedRead,
    /// `Buffer::as_view` (`ViewFormat<T>` is implemented).
    view: bool,
    /// The write appends to `out` (the `Encode` contract).
    appends: bool,
}

fn exercise<F: Format, T: Codec<F> + PartialEq + Debug>(value: &T, expected: &Capabilities)
where
    F::Error: Debug,
{
    assert_eq!((F::NAME, F::TEXT, F::TRUSTED_READ), (expected.name, expected.text, expected.trusted_read));

    // The write; the size; validation; the validating read.
    let bytes = psio::encode::<F, T>(value).unwrap();
    assert_eq!(psio::size_of::<F, T>(value).unwrap(), bytes.len(), "{}", F::NAME);
    psio::validate::<F, T, _>(&bytes, &DefaultSafe).unwrap();
    assert_eq!(&psio::decode::<F, T>(&bytes).unwrap(), value, "{}", F::NAME);
    if F::TEXT {
        std::str::from_utf8(&bytes).unwrap();
    }

    // The validated write gives the same bytes.
    assert_eq!(psio::try_encode::<F, T>(value).unwrap(), bytes, "{}", F::NAME);

    // `encode_into` appends.
    let mut out = vec![b'x'];
    let written = value.encode_into(&mut out).unwrap();
    assert_eq!(expected.appends, out.len() == bytes.len() + 1 && out[0] == b'x' && written == bytes.len(), "{}", F::NAME);

    // A buffer of the type in the format.
    let buffer = Buffer::<T, F>::encode(value).unwrap();
    assert_eq!(buffer.bytes(), &bytes[..]);
    assert_eq!(&buffer.decode().unwrap(), value);
    assert!(Buffer::<T, F>::new(bytes.clone()).is_ok());

    // Bytes that are not a frame.
    if bytes.len() > 8 {
        assert!(psio::validate::<F, T, _>(&bytes[..3], &DefaultSafe).is_err(), "{}", F::NAME);
        assert!(psio::decode::<F, T>(&bytes[..3]).is_err(), "{}", F::NAME);
        assert!(Buffer::<T, F>::new(bytes[..3].to_vec()).is_err(), "{}", F::NAME);
    }
}

/// For the formats with views: `F: ViewFormat<T>`.
fn exercise_view<F: psio::ViewFormat<T> + Format, T: Codec<F>>(value: &T)
where
    F::Error: Debug,
{
    let buffer = Buffer::<T, F>::encode(value).unwrap();
    let _view = buffer.as_view();
}

macro_rules! format_case {
    ($test:ident, $tag:ty, $expected:expr $(, view = $view:tt)?) => {
        #[test]
        fn $test() {
            // The alias is the only thing that changes.
            type F = $tag;
            let expected = $expected;
            exercise::<F, Record>(&sample(), &expected);
            exercise::<F, Record>(&Record::default(), &expected);
            $( let _: bool = $view; exercise_view::<F, Record>(&sample()); )?
            assert_eq!(expected.view, false $( || $view )?, "the view column and the instantiation disagree");
        }
    };
}

const fn caps(name: &'static str, text: bool, trusted_read: TrustedRead, view: bool) -> Capabilities {
    Capabilities { name, text, trusted_read, view, appends: true }
}

format_case!(pssz, psio::Pssz, caps("pssz", false, TrustedRead::RefusesNonFrames, true), view = true);
format_case!(key, psio::key::KeyFormat, caps("key", false, TrustedRead::Parses, false));
format_case!(json, psio::Json, caps("json", true, TrustedRead::Parses, false));
format_case!(ijson, psio::IJson, caps("ijson", true, TrustedRead::Parses, false));
#[cfg(feature = "ssz")]
format_case!(ssz, psio::Ssz, caps("ssz", false, TrustedRead::Parses, true), view = true);
#[cfg(feature = "flatbuf")]
format_case!(flatbuf, psio::flatbuf::Flatbuf, caps("flatbuf", false, TrustedRead::ValidatedBytesOnly, true), view = true);
// Gaps against the concept (psio#18, B7): no generic view (`ViewFormat`) for
// capnp, wit and pjson; no trusted read for protobuf and pjson. pjson is in
// the traits as it stands (gofractally/psio#17): its tag is
// `pjson::format::Pjson`, not at the crate root, its types are declared by
// `pjson_struct!` rather than a derive, and it is behind a cargo feature.
#[cfg(feature = "capnp")]
format_case!(capnp, psio::capnp::Capnp, caps("capnp", false, TrustedRead::Parses, false));
#[cfg(feature = "wit")]
format_case!(wit, psio::wit::Wit, caps("wit", false, TrustedRead::Parses, false));
#[cfg(feature = "protobuf")]
format_case!(protobuf, psio::protobuf::ProtobufFormat, caps("protobuf", false, TrustedRead::Absent, false));
#[cfg(feature = "pjson")]
format_case!(pjson, psio::pjson::format::Pjson, caps("pjson", false, TrustedRead::Absent, false));
