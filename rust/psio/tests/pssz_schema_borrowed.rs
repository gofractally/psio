//! A record that borrows has the schema of its owned form: `&[u8]`, `&str`,
//! `&[T]` and `&T` are in a schema what `Vec<u8>`, `String`, `Vec<T>` and
//! `T` are (as they are on the wire), and a lifetime is not part of a type's
//! name.

use psio::pssz::schema_ir::{schema_for, Schema};
use psio::{PsszPack, ToPsszSchema};

#[derive(PsszPack, ToPsszSchema)]
struct S<'a> {
    b: &'a [u8],
}

#[derive(PsszPack, ToPsszSchema)]
struct Item {
    id: u32,
    label: String,
}

#[derive(PsszPack, ToPsszSchema)]
struct Row<'a, 'b> {
    key: u64,
    name: &'a str,
    bytes: &'a [u8],
    words: &'b [u32],
    items: &'b [Item],
    first: &'a Item,
    note: Option<&'a str>,
    names: Vec<&'b str>,
    hash: &'a [u8; 4],
}

mod owned {
    use psio::{PsszPack, ToPsszSchema};

    #[derive(PsszPack, ToPsszSchema)]
    pub struct S {
        pub b: Vec<u8>,
    }

    #[derive(PsszPack, ToPsszSchema)]
    pub struct Item {
        pub id: u32,
        pub label: String,
    }

    #[derive(PsszPack, ToPsszSchema)]
    pub struct Row {
        pub key: u64,
        pub name: String,
        pub bytes: Vec<u8>,
        pub words: Vec<u32>,
        pub items: Vec<Item>,
        pub first: Item,
        pub note: Option<String>,
        pub names: Vec<String>,
        pub hash: [u8; 4],
    }
}

fn json(schema: &(Schema, psio::pssz::schema_ir::AnyType)) -> String {
    format!("{:?} / {:?}", schema.0, schema.1)
}

#[test]
fn a_borrowing_record_has_the_schema_of_its_owned_form() {
    let borrowed = schema_for::<S>().unwrap();
    let owned = schema_for::<owned::S>().unwrap();
    assert_eq!(borrowed, owned);
    assert_eq!(json(&borrowed), json(&owned));
    // The type is named by its identifier, without the lifetime.
    assert_eq!(borrowed.0.types[0].name, "S");

    let borrowed = schema_for::<Row>().unwrap();
    let owned = schema_for::<owned::Row>().unwrap();
    assert_eq!(borrowed, owned);
    assert_eq!(json(&borrowed), json(&owned));
}

#[test]
fn references_alone_are_their_owned_types() {
    assert_eq!(schema_for::<&[u8]>().unwrap(), schema_for::<Vec<u8>>().unwrap());
    assert_eq!(schema_for::<&str>().unwrap(), schema_for::<String>().unwrap());
    assert_eq!(schema_for::<&[Item]>().unwrap(), schema_for::<Vec<Item>>().unwrap());
    assert_eq!(schema_for::<&Item>().unwrap(), schema_for::<Item>().unwrap());
    assert_eq!(schema_for::<&&[&str]>().unwrap(), schema_for::<Vec<String>>().unwrap());
    // `&[u8]` is bytes (the `hex` custom type), as `Vec<u8>` is.
    assert!(json(&schema_for::<&[u8]>().unwrap()).contains("hex"));
}

/// The schema does not need the borrow to be `'static`.
#[test]
fn the_lifetime_is_any() {
    fn schema_of<'a>(value: &S<'a>) -> (Schema, psio::pssz::schema_ir::AnyType) {
        let _ = value;
        schema_for::<S<'a>>().unwrap()
    }
    let local = vec![1u8, 2, 3];
    let record = S { b: &local };
    assert_eq!(schema_of(&record), schema_for::<owned::S>().unwrap());
    // And the bytes are those of the owned form.
    assert_eq!(psio::to_pssz(&record), psio::to_pssz(&owned::S { b: local.clone() }));
}

/// One builder, both forms: a borrowed member and its owned twin are one
/// named type in the schema, not two.
#[test]
fn borrowed_and_owned_members_share_a_type() {
    #[derive(PsszPack, ToPsszSchema)]
    struct Both<'a> {
        borrowed: &'a [Item],
        owned: Vec<Item>,
        text: &'a str,
        string: String,
    }
    let (schema, _) = schema_for::<Both>().unwrap();
    let debug = format!("{schema:?}");
    assert_eq!(schema.types.iter().filter(|member| member.name == "Item").count(), 1, "{debug}");
    let Some(both) = schema.types.iter().find(|member| member.name == "Both") else { panic!("{debug}") };
    let members = format!("{:?}", both.r#type);
    // The two list members refer to the same type, and so do the two texts.
    let psio::pssz::schema_ir::AnyValue::Object(object) = &both.r#type.value else { panic!("{members}") };
    assert_eq!(object.members[0].r#type, object.members[1].r#type);
    assert_eq!(object.members[2].r#type, object.members[3].r#type);
}
