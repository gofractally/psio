//! `#[psio(crate = "path")]` names the psio crate for every derive. Inside
//! this crate `::psio` does not resolve, so each derive below compiles only
//! if it takes the path from the attribute.

use crate::dynamic_view::ToDynamicSchema as _;
use crate::{
    CapnpPack, CapnpUnpack, FbPack, FbUnpack, JsonText, Key, PsszPack, PsszUnpack, PsszView,
    ToDynamicSchema, ToPsszSchema, WitPack, WitUnpack,
};

#[derive(
    Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, ToPsszSchema, JsonText, Key,
    CapnpPack, CapnpUnpack, FbPack, FbUnpack, WitPack, WitUnpack, ToDynamicSchema,
)]
#[psio(crate = "crate")]
struct Point {
    x: u32,
    y: u64,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, Key)]
#[psio(crate = "crate", transparent, invariant = "nonzero")]
struct Id(u32);

fn nonzero(id: &Id) -> Result<(), &'static str> {
    if id.0 == 0 {
        Err("an id is not zero")
    } else {
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText, Key)]
#[psio(crate = "crate")]
#[repr(u8)]
enum Colour {
    #[psio(name = "red")]
    Red = 1,
    #[psio(name = "green")]
    Green = 2,
    #[psio(other)]
    Unknown(u8),
}

#[test]
fn every_derive_resolves_the_crate_from_psio_crate() {
    let value = Point { x: 7, y: 9 };
    assert_eq!(crate::from_pssz::<Point>(&crate::to_pssz(&value)).unwrap(), value);
    assert_eq!(crate::from_json::<Point>(&crate::to_json(&value)).unwrap(), value);
    assert_eq!(crate::from_key::<Point>(&crate::to_key(&value)).unwrap(), value);
    assert_eq!(crate::from_capnp::<Point>(&crate::to_capnp(&value)).unwrap(), value);
    assert_eq!(crate::from_flatbuf::<Point>(&crate::to_flatbuf(&value)).unwrap(), value);
    assert_eq!(crate::from_wit::<Point>(&crate::to_wit(&value)).unwrap(), value);
    let schema = Point::dynamic_schema();
    assert_eq!(schema.field_count(), 2);
    assert_eq!(schema.find_by_name("y").unwrap().offset, 8);
}

#[test]
fn type_level_keys_apply_to_every_format() {
    // transparent: the wrapper is its member in pSSZ, JSON and key.
    let id = Id(5);
    assert_eq!(crate::to_pssz(&id), crate::to_pssz(&5u32));
    assert_eq!(crate::to_json(&id), crate::to_json(&5u32));
    assert_eq!(crate::to_key(&id), crate::to_key(&5u32));
    // invariant: every reader rejects a value that violates it.
    assert!(crate::from_pssz::<Id>(&crate::to_pssz(&0u32)).is_err());
    assert!(crate::from_json::<Id>("0").is_err());
    assert!(crate::from_key::<Id>(&crate::to_key(&0u32)).is_err());
    // name and other: text uses the name, the catch-all keeps the number.
    assert_eq!(crate::to_json(&Colour::Green), "\"green\"");
    assert_eq!(crate::from_json::<Colour>("\"red\"").unwrap(), Colour::Red);
    assert_eq!(crate::from_pssz::<Colour>(&[9]).unwrap(), Colour::Unknown(9));
    assert_eq!(crate::from_key::<Colour>(&crate::to_key(&Colour::Unknown(9))).unwrap(), Colour::Unknown(9));
}
