//! `conformance/evolution-vectors.json` (described in
//! `conformance/evolution-vectors.md`): pairs of types and of interfaces of
//! two Schema IRs, with the verdict of the run-time schema comparison for
//! each (`evolves` / `equivalent`, each under `wire` and `contract`).
//!
//! Every row is run through `psio::pssz::dynamic::{evolves_from, equivalent}`
//! (types) and `{evolves_from_interface, equivalent_interface}` (interfaces)
//! and compared on `ok`, `code` and `path`. Nothing is skipped.

use psio::pssz::dynamic::{self, Compare, Compatibility};
use psio::pssz::schema_ir::{AnyType, Schema};
use std::collections::BTreeMap;

#[derive(serde::Deserialize, Debug, PartialEq)]
struct Verdict {
    ok: bool,
    code: String,
    path: String,
}
#[derive(serde::Deserialize)]
struct Modes {
    wire: Verdict,
    contract: Verdict,
}
#[derive(serde::Deserialize)]
struct SchemaRow {
    id: String,
    ir_hex: String,
}
#[derive(serde::Deserialize)]
struct TypeRow {
    name: String,
    old_schema: String,
    old_type: String,
    new_schema: String,
    new_type: String,
    evolves: Modes,
    equivalent: Modes,
}
#[derive(serde::Deserialize)]
struct InterfaceRow {
    name: String,
    old_schema: String,
    old_interface: String,
    new_schema: String,
    new_interface: String,
    evolves: Modes,
    equivalent: Modes,
}
#[derive(serde::Deserialize)]
struct Fixture {
    version: u32,
    schemas: Vec<SchemaRow>,
    types: Vec<TypeRow>,
    interfaces: Vec<InterfaceRow>,
}

fn fixture() -> (Fixture, BTreeMap<String, Schema>) {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/evolution-vectors.json");
    let fixture: Fixture = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    assert_eq!(fixture.version, 1);
    let schemas = fixture
        .schemas
        .iter()
        .map(|row| {
            let bytes = hex::decode(&row.ir_hex).unwrap();
            let schema: Schema = psio::from_pssz(&bytes).unwrap_or_else(|e| panic!("{}: {e:?}", row.id));
            (row.id.clone(), schema)
        })
        .collect();
    (fixture, schemas)
}

fn verdict(result: &Compatibility) -> Verdict {
    Verdict { ok: result.ok, code: result.code.to_string(), path: result.path.clone() }
}

/// The four verdicts of one row against the fixture's; returns the number
/// compared.
fn check(name: &str, evolves: &Modes, equivalent: &Modes, run: impl Fn(bool, Compare) -> Compatibility) -> usize {
    let mut compared = 0;
    for (relation, exact, expected) in [("evolves", false, evolves), ("equivalent", true, equivalent)] {
        for (mode, what, expected) in
            [("wire", Compare::Wire, &expected.wire), ("contract", Compare::Contract, &expected.contract)]
        {
            let got = run(exact, what);
            assert_eq!(&verdict(&got), expected, "{name}: {relation} / {mode}: {got}");
            // A verdict is a rule and a place, or neither.
            assert_eq!(got.ok, got.code.is_empty(), "{name}");
            assert_eq!(got.ok, got.path.is_empty(), "{name}");
            assert_eq!(got.ok, got.reason.is_empty(), "{name}");
            compared += 1;
        }
    }
    compared
}

#[test]
fn every_type_pair_has_the_fixture_verdict() {
    let (fixture, schemas) = fixture();
    // 50 pairs, each in both directions.
    assert_eq!(fixture.types.len(), 100);
    let mut compared = 0;
    for row in &fixture.types {
        let (old, new) = (&schemas[&row.old_schema], &schemas[&row.new_schema]);
        let (old_type, new_type) = (AnyType::named(&row.old_type), AnyType::named(&row.new_type));
        compared += check(&row.name, &row.evolves, &row.equivalent, |exact, what| {
            if exact {
                dynamic::equivalent(old, &old_type, new, &new_type, what)
            } else {
                dynamic::evolves_from(old, &old_type, new, &new_type, what)
            }
        });
    }
    assert_eq!(compared, 400);
}

#[test]
fn every_interface_pair_has_the_fixture_verdict() {
    let (fixture, schemas) = fixture();
    // 6 pairs, each in both directions.
    assert_eq!(fixture.interfaces.len(), 12);
    let mut compared = 0;
    for row in &fixture.interfaces {
        let (old, new) = (&schemas[&row.old_schema], &schemas[&row.new_schema]);
        let find = |schema: &'_ Schema, name: &str| {
            schema.interfaces.iter().position(|i| i.name == name).unwrap_or_else(|| panic!("{}: {name}", row.name))
        };
        let old_interface = &old.interfaces[find(old, &row.old_interface)];
        let new_interface = &new.interfaces[find(new, &row.new_interface)];
        compared += check(&row.name, &row.evolves, &row.equivalent, |exact, what| {
            if exact {
                dynamic::equivalent_interface(old, old_interface, new, new_interface, what)
            } else {
                dynamic::evolves_from_interface(old, old_interface, new, new_interface, what)
            }
        });
        // A schema holding that one interface under one name on both sides
        // compares as its interface does, the path led by the name and `/`.
        if row.old_interface == row.new_interface && old.interfaces.len() == 1 && new.interfaces.len() == 1 {
            for (exact, what, expected) in [
                (false, Compare::Wire, &row.evolves.wire),
                (false, Compare::Contract, &row.evolves.contract),
                (true, Compare::Wire, &row.equivalent.wire),
                (true, Compare::Contract, &row.equivalent.contract),
            ] {
                let got = if exact {
                    dynamic::equivalent_schema(old, new, what)
                } else {
                    dynamic::evolves_from_schema(old, new, what)
                };
                assert_eq!(got.ok, expected.ok, "{}", row.name);
                assert_eq!(got.code, expected.code, "{}", row.name);
                if !got.ok {
                    assert_eq!(got.path, format!("{}/{}", row.old_interface, expected.path), "{}", row.name);
                }
            }
        }
    }
    assert_eq!(compared, 48);
}

#[test]
fn every_schema_of_the_fixture_is_used_and_well_formed() {
    let (fixture, schemas) = fixture();
    assert_eq!(schemas.len(), fixture.schemas.len());
    let mut used = std::collections::BTreeSet::new();
    for row in &fixture.types {
        used.extend([&row.old_schema, &row.new_schema]);
    }
    for row in &fixture.interfaces {
        used.extend([&row.old_schema, &row.new_schema]);
    }
    assert_eq!(used.len(), schemas.len());
    for (id, schema) in &schemas {
        dynamic::validate_schema(schema).unwrap_or_else(|e| panic!("{id}: {e:?}"));
    }
}

#[test]
fn a_type_the_schema_does_not_have_is_a_malformed_schema() {
    let (_, schemas) = fixture();
    let schema = schemas.values().next().unwrap();
    let missing = AnyType::named("no-such-type");
    let root = AnyType::named("Root");
    let got = dynamic::evolves_from(schema, &missing, schema, &root, Compare::Wire);
    assert_eq!((got.ok, got.code, got.path.as_str()), (false, "malformed-schema", "(root)"));
    let got = dynamic::equivalent(schema, &root, schema, &missing, Compare::Contract);
    assert_eq!((got.ok, got.code, got.path.as_str()), (false, "malformed-schema", "(root)"));
    assert!(got.clone().into_result().is_err());
    assert!(got.to_string().contains("malformed-schema"));
    // The default is the contract comparison, as in C++.
    assert_eq!(Compare::default(), Compare::Contract);
}
