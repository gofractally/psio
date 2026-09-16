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
use psio::{Pack, Unpack};
#[derive(Debug, PartialEq, psio::Pack, psio::Unpack)]
#[fracpack(fracpack_mod = "psio")]
struct Record { id: u32, name: String }
psio::pjson_struct!(Record { id: u32, name: String });
psio::ssz_struct!(Record { id: u32, name: String });
psio::pssz_struct!(Record { id: u32, name: String });
fn main() {
    assert_eq!(psio::pjson::WIRE_REVISION, 2);
    assert_eq!(psio::pjson::to_pjson(&5i64), vec![0x25]);
    assert_eq!(psio::pjson::to_pjson(&-5i64), vec![0x35]);
    let value = Record { id: 42, name: "Alice".into() };
    assert_eq!(Record::unpacked(&value.packed()).unwrap(), value);
    let bytes = psio::pjson::to_pjson(&value);
    assert_eq!(psio::pjson::from_pjson::<Record>(&bytes).unwrap(), value);
    let rows = vec![value]; let bytes = psio::pjson::to_pjson(&rows);
    assert_eq!(psio::pjson::from_pjson::<Vec<Record>>(&bytes).unwrap(), rows);
}
''')
    subprocess.run(['cargo','run','--offline','--manifest-path',str(consumer/'Cargo.toml')], check=True)
print('Rust archive consumer passed')
