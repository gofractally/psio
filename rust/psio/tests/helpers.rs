// The helper vocabulary of docs/helpers.md, one set of names per format:
// to_F / try_to_F / to_F_into / from_F / from_F_trusted / validate_F /
// F_size, the read-only and mutable views, and the owning and shared typed
// buffers.

use psio::{JsonText, Key, PsszPack, PsszUnpack, PsszView};
use psio::{PsszBuffer, PsszMutView, SharedPssz};

mod types {
    use super::*;
    #[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, JsonText, Key)]
    pub struct Account {
        pub balance: u64,
        pub owner: String,
        pub memo: Option<String>,
    }
}
// The accessor traits are generated next to the type.
mod psio_names {
    pub use super::types::{AccountPsszAccessors, AccountPsszMutAccessors};
}
use psio_names::*;
use types::Account;

fn account() -> Account {
    Account { balance: 7, owner: "alice".into(), memo: None }
}

#[test]
fn pssz_names() {
    let value = account();
    let bytes = psio::to_pssz(&value);
    assert_eq!(psio::try_to_pssz(&value).unwrap(), bytes);
    assert_eq!(psio::pssz_size(&value), bytes.len());
    let mut sink = vec![0xAA];
    psio::to_pssz_into(&value, &mut sink).unwrap();
    assert_eq!(&sink[1..], &bytes[..]);

    assert_eq!(psio::from_pssz::<Account>(&bytes).unwrap(), value);
    assert_eq!(psio::from_pssz_trusted::<Account>(&bytes), value);
    assert!(psio::validate_pssz::<Account>(&bytes).is_ok());

    let cut = &bytes[..bytes.len() - 1];
    assert!(psio::validate_pssz::<Account>(&cut[..3]).is_err());
    assert!(psio::from_pssz::<Account>(&cut[..3]).is_err());
    // The trusted decode of bytes that are not valid is a panic, not UB.
    let short = cut[..3].to_vec();
    assert!(std::panic::catch_unwind(|| psio::from_pssz_trusted::<Account>(&short)).is_err());

    // The tag-generic layer names the profile; `Pssz` is what the helpers use.
    use psio::pssz::{Pssz, Pssz32};
    assert_eq!(psio::pssz::encode::<Pssz, _>(&value), bytes);
    let wide = psio::pssz::encode::<Pssz32, _>(&value);
    assert_eq!(psio::pssz::decode::<Pssz32, Account>(&wide).unwrap(), value);
    assert_eq!(psio::pssz::size_of::<Pssz32, _>(&value), wide.len());
    assert_eq!(psio::encode::<Pssz32, _>(&value).unwrap(), wide);
    assert_eq!(psio::decode::<Pssz32, Account>(&wide).unwrap(), value);
    assert_eq!(psio::size_of::<Pssz32, _>(&value).unwrap(), wide.len());
    assert!(psio::validate::<Pssz, Account, _>(&bytes, &psio::DefaultSafe).is_ok());
    assert!(psio::validate::<Pssz, Account, _>(&bytes, &psio::StrictCanonical).is_ok());
}

#[test]
fn pssz_views_read_and_the_mutable_view_overwrites_fixed_size_values() {
    let value = account();
    let mut bytes = psio::to_pssz(&value);

    let view = PsszView::<Account>::new(&bytes).unwrap();
    assert_eq!(view.balance().get(), 7);
    assert_eq!(view.owner().view().unwrap(), "alice");
    assert!(!view.memo().has_value());
    assert!(PsszView::<Account>::new(&bytes[..3]).is_err());
    assert_eq!(PsszView::<Account>::trusted(&bytes).balance().get(), 7);

    let mut writer = PsszMutView::<Account>::new(&mut bytes).unwrap();
    writer.balance_mut().set(&42).unwrap();
    // Reading through the writer is explicit: as_view.
    assert_eq!(writer.as_view().balance().get(), 42);
    // A pruned trailing optional has no bytes in the frame to write.
    assert!(writer.try_memo_mut().is_err());
    assert_eq!(writer.into_view().owner().view().unwrap(), "alice");
    assert_eq!(psio::from_pssz::<Account>(&bytes).unwrap().balance, 42);
    assert!(PsszMutView::<Account>::new(&mut [0u8; 3]).is_err());

    // Elements of sequences and a present optional payload.
    let mut list = psio::to_pssz(&vec![1u32, 2, 3]);
    let mut w = PsszMutView::<Vec<u32>>::new(&mut list).unwrap();
    assert_eq!(w.len(), 3);
    w.get_mut(1).set(&20).unwrap();
    assert!(w.try_get_mut(3).is_err());
    assert_eq!(psio::from_pssz::<Vec<u32>>(&list).unwrap(), [1, 20, 3]);

    let mut some = psio::to_pssz(&Some(5u16));
    let mut w = PsszMutView::<Option<u16>>::new(&mut some).unwrap();
    w.get_mut().unwrap().set(&6).unwrap();
    assert_eq!(psio::from_pssz::<Option<u16>>(&some).unwrap(), Some(6));
    let mut none = psio::to_pssz(&None::<u16>);
    assert!(PsszMutView::<Option<u16>>::new(&mut none).unwrap().get_mut().is_none());

    let mut array = psio::to_pssz(&[1u8, 2, 3]);
    PsszMutView::<[u8; 3]>::new(&mut array).unwrap().set(&[9, 8, 7]).unwrap();
    assert_eq!(array, [9, 8, 7]);
}

#[test]
fn pssz_buffers_own_or_share_their_bytes() {
    let value = account();
    let mut owned = PsszBuffer::<Account>::new(psio::to_pssz(&value)).unwrap();
    assert_eq!(owned.bytes(), psio::to_pssz(&value));
    assert_eq!(owned.len(), psio::pssz_size(&value));
    assert_eq!(owned.as_view().balance().get(), 7);
    owned.as_mut_view().balance_mut().set(&8).unwrap();
    assert_eq!(owned.decode().unwrap().balance, 8);
    assert!(PsszBuffer::<Account>::new(vec![1, 2, 3]).is_err());
    assert_eq!(PsszBuffer::<Account>::encode(&value).unwrap().decode().unwrap(), value);

    let tx: SharedPssz<Account> = owned.into_shared();
    let other = tx.clone();
    assert!(tx.ptr_eq(&other));
    assert_eq!(other.as_view().balance().get(), 8);
    assert_eq!(other.decode().unwrap().balance, 8);
    assert_eq!(tx, other);
    let copy = other.to_buffer();
    assert_eq!(copy.bytes(), tx.bytes());
    assert!(SharedPssz::<Account>::new(vec![1, 2, 3]).is_err());
    assert!(SharedPssz::<Account>::new(psio::to_pssz(&value)).is_ok());
    let _trusted = SharedPssz::<Account>::trusted(&[1u8, 2, 3][..]);

    fn send_sync<T: Send + Sync>() {}
    send_sync::<SharedPssz<Account>>();
    send_sync::<PsszBuffer<Account>>();
}

#[test]
fn ssz_names() {
    let value: Vec<u32> = vec![1, 2, 3];
    let bytes = psio::to_ssz(&value);
    assert_eq!(psio::ssz_size(&value), bytes.len());
    let mut sink = vec![0xAA];
    psio::to_ssz_into(&value, &mut sink);
    assert_eq!(&sink[1..], &bytes[..]);
    assert_eq!(psio::from_ssz::<Vec<u32>>(&bytes).unwrap(), value);
    assert_eq!(psio::from_ssz_trusted::<Vec<u32>>(&bytes), value);
    assert!(psio::validate_ssz::<Vec<u32>>(&bytes).is_ok());
    assert!(psio::validate_ssz::<Vec<u32>>(&bytes[..5]).is_err());
    assert!(psio::from_ssz::<Vec<u32>>(&bytes[..5]).is_err());

    let view = psio::SszView::<Vec<u32>>::new(&bytes).unwrap();
    assert_eq!(view.get(2).get(), 3);
    assert!(psio::SszView::<Vec<u32>>::new(&bytes[..5]).is_err());
    assert_eq!(psio::SszView::<Vec<u32>>::trusted(&bytes).len(), 3);

    let owned = psio::SszBuffer::<Vec<u32>>::new(bytes.clone()).unwrap();
    assert_eq!(owned.as_view().len(), 3);
    let shared: psio::SharedSsz<Vec<u32>> = owned.into_shared();
    assert_eq!(shared.clone().decode().unwrap(), value);
    assert_eq!(psio::encode::<psio::Ssz, _>(&value).unwrap(), bytes);
}

#[test]
fn key_names() {
    let value = account();
    let bytes = psio::to_key(&value);
    assert_eq!(psio::try_to_key(&value).unwrap(), bytes);
    assert_eq!(psio::key_size(&value), bytes.len());
    let mut sink = vec![0xAA];
    psio::to_key_into(&value, &mut sink).unwrap();
    assert_eq!(&sink[1..], &bytes[..]);
    assert_eq!(psio::from_key::<Account>(&bytes).unwrap(), value);
    assert_eq!(psio::from_key_trusted::<Account>(&bytes), value);
    assert!(psio::validate_key::<Account>(&bytes).is_ok());
    assert!(psio::validate_key::<Account>(&bytes[..bytes.len() - 1]).is_err());

    use psio::key::{KeyBuffer, KeyFormat, SharedKey};
    let owned = KeyBuffer::<Account>::new(bytes.clone()).unwrap();
    let shared: SharedKey<Account> = owned.into_shared();
    assert_eq!(shared.decode().unwrap(), value);
    assert_eq!(psio::encode::<KeyFormat, _>(&value).unwrap(), bytes);
    assert_eq!(psio::size_of::<KeyFormat, _>(&value).unwrap(), bytes.len());
}

#[test]
fn json_and_ijson_names() {
    let value = Account { balance: 1 << 60, owner: "alice".into(), memo: Some("hi".into()) };
    let json = psio::to_json(&value);
    let ijson = psio::to_ijson(&value);
    assert_eq!(json, r#"{"balance":1152921504606846976,"owner":"alice","memo":"hi"}"#);
    assert_eq!(ijson, r#"{"balance":"1152921504606846976","owner":"alice","memo":"hi"}"#);
    assert_eq!(psio::try_to_json(&value).unwrap(), json);
    assert_eq!(psio::try_to_ijson(&value).unwrap(), ijson);
    assert_eq!(psio::json_size(&value), json.len());
    assert_eq!(psio::ijson_size(&value), ijson.len());
    let mut sink = String::from("x");
    psio::to_json_into(&value, &mut sink).unwrap();
    psio::to_ijson_into(&value, &mut sink).unwrap();
    assert_eq!(sink, format!("x{json}{ijson}"));

    // Text is a &str, String, &[u8] or Vec<u8>.
    assert_eq!(psio::from_json::<Account>(&json).unwrap(), value);
    assert_eq!(psio::from_json::<Account>(json.as_bytes()).unwrap(), value);
    assert_eq!(psio::from_ijson::<Account>(ijson.as_str()).unwrap(), value);
    assert_eq!(psio::from_json_trusted::<Account>(&json), value);
    assert_eq!(psio::from_ijson_trusted::<Account>(&ijson), value);
    assert!(psio::validate_json::<Account>(&json).is_ok());
    assert!(psio::validate_ijson::<Account>(&ijson).is_ok());
    // I-JSON does not take a bare number beyond 2^53 - 1.
    assert!(psio::validate_ijson::<Account>(&json).is_err());
    assert!(psio::from_ijson::<Account>(&json).is_err());
    assert!(std::panic::catch_unwind(|| psio::from_ijson_trusted::<Account>(r#"{"balance":"#)).is_err());

    // A missing trailing optional is None; a missing required member is an error.
    let short = psio::from_json::<Account>(r#"{"balance":1,"owner":"bob"}"#).unwrap();
    assert_eq!(short.memo, None);
    assert!(psio::from_json::<Account>(r#"{"balance":1}"#).is_err());

    // A value without text: the checked form reports it, the plain one panics.
    assert!(psio::try_to_json(&f64::NAN).is_err());
    assert!(std::panic::catch_unwind(|| psio::to_json(&f64::NAN)).is_err());

    use psio::json::{self, IJson, IJsonBuffer, Json, JsonBuffer, SharedJson};
    assert_eq!(json::encode::<Json, _>(&value), json);
    assert_eq!(json::try_encode::<IJson, _>(&value).unwrap(), ijson);
    assert_eq!(json::decode::<IJson, Account>(&ijson).unwrap(), value);
    assert!(json::validate::<Json, Account>(&json).is_ok());
    assert_eq!(json::size_of::<Json, _>(&value), json.len());
    let owned = JsonBuffer::<Account>::new(json.clone().into_bytes()).unwrap();
    let shared: SharedJson<Account> = owned.into_shared();
    assert_eq!(shared.decode().unwrap(), value);
    assert!(IJsonBuffer::<Account>::new(json.into_bytes()).is_err());
}

#[test]
fn capnp_flatbuf_and_wit_names() {
    #[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpUnpack, psio::FbPack, psio::FbUnpack, psio::WitPack, psio::WitUnpack)]
    struct Point {
        x: u32,
        name: String,
    }
    let value = Point { x: 9, name: "p".into() };

    let bytes = psio::to_capnp(&value);
    assert_eq!(psio::capnp_size(&value), bytes.len());
    assert_eq!(psio::from_capnp::<Point>(&bytes).unwrap(), value);
    assert_eq!(psio::from_capnp_trusted::<Point>(&bytes), value);
    assert!(psio::validate_capnp::<Point>(&bytes).is_ok());
    assert!(psio::validate_capnp::<Point>(&bytes[..5]).is_err());
    let mut sink = vec![1, 2, 3];
    psio::to_capnp_into(&value, &mut sink);
    assert_eq!(sink, bytes);
    assert!(psio::capnp::CapnpBuffer::<Point>::new(bytes).is_ok());

    let bytes = psio::to_flatbuf(&value);
    assert_eq!(psio::flatbuf_size(&value), bytes.len());
    assert_eq!(psio::from_flatbuf::<Point>(&bytes).unwrap(), value);
    assert_eq!(psio::from_flatbuf_trusted::<Point>(&bytes), value);
    assert!(psio::validate_flatbuf::<Point>(&bytes).is_ok());
    assert!(psio::validate_flatbuf::<Point>(&bytes[..2]).is_err());
    psio::to_flatbuf_into(&value, &mut sink);
    assert_eq!(sink, bytes);
    assert!(psio::flatbuf::SharedFlatbuf::<Point>::new(bytes).is_ok());

    let bytes = psio::to_wit(&value);
    assert_eq!(psio::wit_size(&value), bytes.len());
    assert_eq!(psio::from_wit::<Point>(&bytes).unwrap(), value);
    assert_eq!(psio::from_wit_trusted::<Point>(&bytes), value);
    assert!(psio::validate_wit::<Point>(&bytes).is_ok());
    assert!(psio::validate_wit::<Point>(&bytes[..2]).is_err());
    psio::to_wit_into(&value, &mut sink);
    assert_eq!(sink, bytes);
    assert!(psio::wit::WitBuffer::<Point>::new(bytes).is_ok());
}
