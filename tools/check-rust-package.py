#!/usr/bin/env python3
"""Build an unrelated consumer of both extracted Cargo archives."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('package_directory', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='psio-rust-consumer-') as temp:
    work = Path(temp)
    for name in ['psio', 'psio-macros']:
        with tarfile.open(args.package_directory / f'{name}-0.1.0.crate') as archive:
            archive.extractall(work, filter='data')
    consumer = work/'consumer'; (consumer/'src').mkdir(parents=True)
    (consumer/'Cargo.toml').write_text(f'''[package]
name = "psio-package-consumer"
version = "0.0.0"
edition = "2021"
[dependencies]
psio = {{ path = {json.dumps(str(work/'psio-0.1.0'))} }}
[patch.crates-io]
psio-macros = {{ path = {json.dumps(str(work/'psio-macros-0.1.0'))} }}
''')
    (consumer/'src/main.rs').write_text('''
use psio::{JsonText, Key, PsszPack, PsszUnpack, PsszView, ToPsszSchema};
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, ToPsszSchema, JsonText, Key)]
// The serde impls come through psio: the consumer has no serde dependency.
#[derive(psio::Serde)]
#[derive(psio::CapnpPack, psio::CapnpUnpack, psio::FbPack, psio::FbUnpack, psio::WitPack, psio::WitUnpack)]
#[pssz(max_dynamic_data = 4096)]
struct Record {
    #[json(json_string)]
    id: u32,
    name: String,
}
psio::pjson_struct!(Record { id: u32, name: String });
psio::ssz_struct!(Record { id: u32, name: String });

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText, Key)]
#[psio(transparent)]
struct RecordId(u32);

fn main() {
    let value = Record { id: 42, name: "Alice".into() };

    let bytes = psio::to_pssz(&value);
    assert_eq!(psio::pssz_size(&value), bytes.len());
    psio::validate_pssz::<Record>(&bytes).unwrap();
    assert_eq!(psio::from_pssz::<Record>(&bytes).unwrap(), value);
    assert_eq!(psio::PsszView::<Record>::new(&bytes).unwrap().id().get(), 42);
    assert_eq!(psio::to_pssz(&RecordId(42)), psio::to_pssz(&42u32));

    let text = psio::to_json(&value);
    assert_eq!(text, r#"{"id":"42","name":"Alice"}"#);
    psio::validate_json::<Record>(&text).unwrap();
    assert_eq!(psio::from_json::<Record>(&text).unwrap(), value);
    assert_eq!(psio::from_ijson::<Record>(&psio::to_ijson(&value)).unwrap(), value);

    let key = psio::to_key(&value);
    assert_eq!(psio::from_key::<Record>(&key).unwrap(), value);
    assert!(key < psio::to_key(&Record { id: 43, name: String::new() }));

    assert_eq!(psio::from_ssz::<Record>(&psio::to_ssz(&value)).unwrap(), value);
    assert_eq!(psio::from_capnp::<Record>(&psio::to_capnp(&value)).unwrap(), value);
    assert_eq!(psio::from_flatbuf::<Record>(&psio::to_flatbuf(&value)).unwrap(), value);
    assert_eq!(psio::from_wit::<Record>(&psio::to_wit(&value)).unwrap(), value);

    assert_eq!(psio::pjson::WIRE_REVISION, 2);
    assert_eq!(psio::pjson::to_pjson(&5i64), vec![0x25]);
    assert_eq!(psio::pjson::to_pjson(&-5i64), vec![0x35]);
    let bytes = psio::pjson::to_pjson(&value);
    assert_eq!(psio::pjson::from_pjson::<Record>(&bytes).unwrap(), value);
    let rows = vec![value]; let bytes = psio::pjson::to_pjson(&rows);
    assert_eq!(psio::pjson::from_pjson::<Vec<Record>>(&bytes).unwrap(), rows);
}
''')
    subprocess.run(['cargo','run','--offline','--manifest-path',str(consumer/'Cargo.toml')], check=True)
print('Rust archive consumer passed')
