use psio::pssz::{schema_ir::*, upgrade::validate_optional_upgrade};
use psio::ToPsszSchema;

#[derive(ToPsszSchema)]
struct Old {
    name: String,
}
#[derive(ToPsszSchema)]
struct New {
    name: String,
    extra: Option<String>,
}
#[derive(ToPsszSchema)]
struct Required {
    name: String,
    extra: String,
}
#[derive(ToPsszSchema)]
struct Changed {
    name: u32,
}
#[derive(ToPsszSchema)]
struct Renamed {
    other: String,
}
#[derive(ToPsszSchema)]
#[psio(definition_will_not_change)]
struct Frozen {
    name: String,
}
#[derive(ToPsszSchema)]
#[psio(definition_will_not_change)]
struct FrozenNew {
    name: String,
    extra: Option<String>,
}
#[derive(ToPsszSchema)]
struct OldOuter {
    inner: Old,
    values: Vec<Old>,
}
#[derive(ToPsszSchema)]
struct NewOuter {
    inner: New,
    values: Vec<New>,
}
fn admits<A: ToPsszSchema, B: ToPsszSchema>(
) -> psio::pssz::PsszResult<psio::pssz::upgrade::UpgradeReport> {
    let (a, at) = schema_for::<A>().unwrap();
    let (b, bt) = schema_for::<B>().unwrap();
    validate_optional_upgrade(&a, &at, &b, &bt)
}
#[test]
fn accepts_only_optional_suffixes_and_preserves_prefixes_and_frozen_layouts() {
    assert_eq!(
        admits::<Old, New>().unwrap().added_optional_fields,
        ["$.extra"]
    );
    assert!(admits::<Old, Required>().is_err());
    assert!(admits::<Old, Changed>().is_err());
    assert!(admits::<Old, Renamed>().is_err());
    assert!(admits::<New, Old>().is_err());
    assert!(admits::<Frozen, FrozenNew>().is_err());
    assert!(admits::<Old, Frozen>().is_err());
    assert!(admits::<Old, Old>().is_ok());
    assert_eq!(
        admits::<(u32,), (u32, Option<String>)>()
            .unwrap()
            .added_optional_fields,
        ["$.1"]
    );
    assert!(admits::<(u32,), (u32, String)>().is_err());
}
#[test]
fn nested_optional_upgrades_and_reference_graphs_are_checked() {
    assert_eq!(
        admits::<OldOuter, NewOuter>()
            .unwrap()
            .added_optional_fields,
        ["$.inner.extra"]
    );
    let (a, at) = schema_for::<Old>().unwrap();
    let (mut b, bt) = schema_for::<New>().unwrap();
    let AnyValue::Type(reference) = &bt.value else {
        panic!()
    };
    let member = b
        .types
        .iter_mut()
        .find(|m| m.name == reference.r#type)
        .unwrap();
    let AnyValue::Object(object) = &mut member.r#type.value else {
        panic!()
    };
    object.members[1].r#type = Box::new(AnyType::new(AnyValue::Option(Optional {
        r#type: Box::new(AnyType::named("missing")),
    })));
    assert!(validate_optional_upgrade(&a, &at, &b, &bt).is_err());
}
