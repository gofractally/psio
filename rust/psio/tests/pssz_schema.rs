use psio::pssz::{schema_ir::*, *};
use psio::{PsszPack, PsszUnpack, ToPsszSchema};

#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
struct Example<T> {
    id: u32,
    text: T,
    next: Option<Box<Example<T>>>,
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
#[pssz(definition_will_not_change)]
struct Fixed {
    flag: bool,
    id: u32,
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
enum Choice {
    Unit,
    Single(u32),
    Pair(u16, String),
    Named { flag: bool },
}
trait Source {
    type Value;
}
struct Provider;
impl Source for Provider {
    type Value = u32;
}
#[derive(PsszPack, PsszUnpack, ToPsszSchema)]
struct Associated<T: Source> {
    value: T::Value,
}

#[test]
fn derives_populate_actual_cpp_ir_with_references_bounds_and_layout_attributes() {
    let (schema, root) = schema_for::<Example<psio::pssz::BoundedString<8>>>().unwrap();
    let AnyValue::Object(object) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Object IR")
    };
    assert_eq!(
        object
            .members
            .iter()
            .map(|m| m.name.as_str())
            .collect::<Vec<_>>(),
        ["id", "text", "next"]
    );
    let AnyValue::Custom(text) = &schema.resolve(&object.members[1].r#type).unwrap().value else {
        panic!("expected string adapter")
    };
    assert_eq!(text.id, "string");
    let AnyValue::BoundedList(bound) = &text.r#type.value else {
        panic!("expected bounded bytes")
    };
    assert_eq!(bound.maxCount, 8);
    let (fixed_schema, fixed_root) = schema_for::<Fixed>().unwrap();
    let AnyValue::Object(fixed) = &fixed_schema.resolve(&fixed_root).unwrap().value else {
        panic!("expected Object")
    };
    assert_eq!(
        fixed.attributes,
        [Attribute {
            name: "definitionWillNotChange".into(),
            value: Some("".into())
        }]
    );
    let bytes = try_to_pssz::<Pssz32, _>(&schema).unwrap();
    assert_eq!(from_pssz::<Pssz32, Schema>(&bytes).unwrap(), schema);
}

#[test]
fn plain_tuples_use_tuple_ir_and_enum_payloads_preserve_extensibility() {
    let (schema, root) = schema_for::<(u32, String)>().unwrap();
    let AnyValue::Tuple(tuple) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Tuple IR")
    };
    assert_eq!(tuple.members.len(), 2);
    let (schema, root) = schema_for::<Choice>().unwrap();
    let AnyValue::Variant(choice) = &schema.resolve(&root).unwrap().value else {
        panic!("expected Variant IR")
    };
    assert_eq!(
        choice
            .members
            .iter()
            .map(|m| m.name.as_str())
            .collect::<Vec<_>>(),
        ["Unit", "Single", "Pair", "Named"]
    );
    assert!(matches!(
        choice.members[0].r#type.value,
        AnyValue::Struct(_)
    ));
    assert!(matches!(choice.members[2].r#type.value, AnyValue::Tuple(_)));
    assert!(matches!(
        choice.members[3].r#type.value,
        AnyValue::Object(_)
    ));
}

#[test]
fn generic_bounds_follow_serialized_associated_fields() {
    let value = Associated::<Provider> { value: 7 };
    let bytes = try_to_pssz::<Pssz32, _>(&value).unwrap();
    assert_eq!(
        from_pssz::<Pssz32, Associated<Provider>>(&bytes)
            .unwrap()
            .value,
        7
    );
    schema_for::<Associated<Provider>>().unwrap();
}

#[test]
fn schema_alias_resolution_rejects_cycles_and_unknown_names() {
    let mut schema = Schema::default();
    schema.types.push(Member {
        name: "cycle".into(),
        r#type: Box::new(AnyType::named("cycle")),
        attributes: vec![],
    });
    assert!(schema.resolve(&schema.types[0].r#type).is_err());
    assert!(schema.resolve(&AnyType::named("missing")).is_err());
}
