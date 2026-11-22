#!/usr/bin/env python3
"""Build unrelated consumers of both extracted Cargo archives.

One takes psio with its default features (pSSZ, keys, Json / IJson, the
Schema IR, time, names, interfaces) and one with every format.
"""
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
psio = {{ path = {json.dumps(str(work/'psio-0.1.0'))}, features = ["all-formats"] }}
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
    # A dependent of the published psio (default features) gets a serde_json
    # with no behaviour-changing feature: Cargo unifies features, so one
    # required here would apply to every crate of the dependent's program.
    tree = subprocess.run(['cargo','tree','--offline','--manifest-path',str(consumer/'Cargo.toml'),
                           '-e','features,normal,build','--prefix','none','--format','{p} [{f}]'],
                          check=True, capture_output=True, text=True).stdout
    serde_json = sorted({line for line in tree.splitlines() if line.startswith('serde_json v')})
    assert serde_json, tree
    for line in serde_json:
        for feature in ['arbitrary_precision', 'preserve_order', 'float_roundtrip', 'unbounded_depth']:
            assert feature not in line, f'psio imposes serde_json/{feature} on its dependents: {line}'
    # The default feature set: what a dependent that names no feature
    # compiles and can use.
    default = work/'default-consumer'; (default/'src').mkdir(parents=True)
    (default/'Cargo.toml').write_text(f'''[package]
name = "psio-default-consumer"
version = "0.0.0"
edition = "2021"
[dependencies]
psio = {{ path = {json.dumps(str(work/'psio-0.1.0'))} }}
[patch.crates-io]
psio-macros = {{ path = {json.dumps(str(work/'psio-macros-0.1.0'))} }}
''')
    (default/'src/main.rs').write_text('''
use psio::time::{Duration, Seconds, TimePoint};
use psio::{JsonText, Key, PsszPack, PsszUnpack, PsszView, ToPsszSchema};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, ToPsszSchema, JsonText, Key, psio::Serde)]
pub struct Record {
    pub id: u32,
    pub name: String,
    pub at: TimePoint<Seconds>,
}

#[psio::interface(name = "store")]
pub trait Store {
    fn put(&mut self, record: Record) -> u32;
    fn count(&self) -> u32;
}

struct Rows(Vec<Record>);
impl Store for Rows {
    fn put(&mut self, record: Record) -> u32 {
        self.0.push(record);
        self.0.len() as u32
    }
    fn count(&self) -> u32 {
        self.0.len() as u32
    }
}

fn main() {
    let at = "2026-11-22T10:15:30Z".parse::<TimePoint<Seconds>>().unwrap() + Duration::from_ticks(30);
    let value = Record { id: 42, name: "Alice".into(), at };
    let bytes = psio::to_pssz(&value);
    assert_eq!(psio::from_pssz::<Record>(&bytes).unwrap(), value);
    assert_eq!(psio::PsszView::<Record>::new(&bytes).unwrap().id().get(), 42);
    let text = psio::to_ijson(&value);
    assert_eq!(text, r#"{"id":42,"name":"Alice","at":"2026-11-22T10:16:00Z"}"#);
    assert_eq!(psio::from_ijson::<Record>(&text).unwrap(), value);
    assert_eq!(psio::from_key::<Record>(&psio::to_key(&value)).unwrap(), value);

    let (schema, root) = psio::pssz::schema_ir::schema_for::<Record>().unwrap();
    assert!(psio::pssz::dynamic::validate(&schema, &root, &bytes).is_ok());
    let mut builder = psio::pssz::schema_ir::SchemaBuilder::new();
    builder.insert_interface::<store::Tag>().unwrap();
    assert_eq!(builder.finish().interfaces[0].funcs.len(), 2);

    let mut rows = Rows(Vec::new());
    let reply = store::dispatch_name(&mut rows, "put", &psio::to_pssz(&store::args::put { record: value })).unwrap();
    assert_eq!(psio::from_pssz::<u32>(&reply).unwrap(), 1);
    assert_eq!(psio::compress_name::number_to_method(psio::compress_name::method_to_number("put")), "put");
}
''')
    built = subprocess.run(['cargo','run','--offline','--manifest-path',str(default/'Cargo.toml')],
                           capture_output=True, text=True)
    if built.returncode != 0:
        raise SystemExit(built.stderr)
    # A dependent sees no warning from psio's own code, and the formats it
    # did not ask for are not in its build.
    assert 'warning' not in built.stderr, built.stderr
    tree = subprocess.run(['cargo','tree','--offline','--manifest-path',str(default/'Cargo.toml'),
                           '-e','features,normal,build','--prefix','none','--format','{p} [{f}]'],
                          check=True, capture_output=True, text=True).stdout
    psio_line = next(line for line in tree.splitlines() if line.startswith('psio v'))
    for feature in ['ssz', 'pjson', 'capnp', 'flatbuf', 'wit', 'dynamic-view', 'beacon', 'experimental-frac']:
        assert feature not in psio_line, psio_line
    assert not any(line.startswith('xxhash-rust ') for line in tree.splitlines()), tree
print('Rust archive consumers passed')
