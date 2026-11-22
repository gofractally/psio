//! Shared actual inputs/bytes for the approved optional-suffix contract.
use psio::pssz::*;
use serde::Serialize;
use serde_json::json;

#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct Base {
    name: String,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct Extended {
    name: String,
    extra: Option<String>,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct Twice {
    name: String,
    middle: Option<u32>,
    last: Option<String>,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct OddBase {
    flag: bool,
    name: String,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct OddExtended {
    flag: bool,
    name: String,
    extra: Option<String>,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct NewOuter {
    inner: Extended,
    values: Vec<Extended>,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct OldOuter {
    inner: Base,
    values: Vec<Base>,
}
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
#[pssz(definition_will_not_change)]
struct Unit;
#[derive(Serialize, PsszPack, PsszUnpack, PsszView, ToPsszSchema)]
struct DepthNode {
    value: u8,
    next: Option<Box<DepthNode>>,
}

fn observe<F: PsszFormat>(case: &str, bytes: &[u8]) -> serde_json::Value {
    use psio::pssz_view::PsszView;
    match case {
        "base" | "extended_some" | "extended_none" => {
            let old = PsszView::<Base, F>::new_checked(bytes).unwrap();
            let new = PsszView::<Extended, F>::new_strict(bytes).unwrap();
            json!({"old_name":old.value.name().view().unwrap(),"old_has_unknown":old.has_unknown,"new_extra_some":new.extra().has_value(),"new_extra_raw_hex":hex::encode(new.extra().bytes())})
        }
        "twice_empty_middle" | "twice_pruned_last" => {
            let old = PsszView::<Base, F>::new_checked(bytes).unwrap();
            let new = PsszView::<Twice, F>::new_strict(bytes).unwrap();
            json!({"old_name":old.value.name().view().unwrap(),"old_has_unknown":old.has_unknown,"middle_some":new.middle().has_value(),"last_some":new.last().has_value()})
        }
        "odd_base" | "odd_extended_some" | "odd_extended_none" => {
            let old = PsszView::<OddBase, F>::new_checked(bytes).unwrap();
            let new = PsszView::<OddExtended, F>::new_strict(bytes).unwrap();
            json!({"old_name":old.value.name().view().unwrap(),"old_flag":old.value.flag().get(),"old_has_unknown":old.has_unknown,"new_extra_some":new.extra().has_value()})
        }
        "option_record_none" | "option_record_some" => {
            let view = PsszView::<Option<Base>, F>::new_strict(bytes).unwrap();
            json!({"some":view.has_value(),"name":if view.has_value(){Some(view.unwrap().name().view().unwrap())}else{None}})
        }
        "option_unit_none" | "option_unit_some" => {
            json!({"some":PsszView::<Option<Unit>,F>::new_strict(bytes).unwrap().has_value()})
        }
        "nested_option_none" | "nested_option_some_none" | "nested_option_some_empty" => {
            let view = PsszView::<Option<Option<String>>, F>::new_strict(bytes).unwrap();
            json!({"outer_some":view.has_value(),"inner_some":if view.has_value(){Some(view.unwrap().has_value())}else{None}})
        }
        "zero_argument_tuple" => json!({"accepted":PsszView::<(),F>::new_strict(bytes).is_ok()}),
        "positional_tuple" => {
            let view = PsszView::<(u32, String), F>::new_strict(bytes).unwrap();
            json!({"first":view.field::<0>().get(),"second":view.field::<1>().view().unwrap()})
        }
        "nested_record_vector" => {
            let old = PsszView::<OldOuter, F>::new_checked(bytes).unwrap();
            json!({"old_has_unknown":old.has_unknown,"inner_name":old.value.inner().name().view().unwrap(),"first_name":old.value.values().get(0).name().view().unwrap()})
        }
        "node_depth_63" | "node_depth_65_reject" => {
            json!({"accepted":PsszView::<DepthNode,F>::new_strict(bytes).is_ok()})
        }
        "node_depth_64" => json!({"accepted":PsszView::<Option<DepthNode>,F>::new_strict(bytes).is_ok()}),
        _ => panic!("unknown observation case"),
    }
}

fn emit<F: PsszFormat, T: PsszPack<F> + Serialize + schema_ir::ToPsszSchema>(
    case: &str,
    ty: &str,
    value: T,
) {
    let bytes = if case == "node_depth_65_reject" {
        assert!(try_encode::<F, _>(&value).is_err());
        invalid_depth_fixture_bytes::<F>(32)
    } else {
        try_encode::<F, _>(&value).unwrap()
    };
    let (schema, root_schema) = schema_ir::schema_for::<T>().unwrap();
    let observations = observe::<F>(case, &bytes);
    let mut value = serde_json::to_value(value).unwrap();
    if case == "option_unit_some" {
        value = json!({});
    }
    if case == "zero_argument_tuple" {
        value = json!([]);
    }
    let option_value = match case {
        "option_record_none" | "option_unit_none" | "nested_option_none" => json!({"None":null}),
        "option_record_some" | "option_unit_some" => json!({"Some":value}),
        "nested_option_some_none" => json!({"Some":{"None":null}}),
        "nested_option_some_empty" => json!({"Some":{"Some":""}}),
        _ => serde_json::Value::Null,
    };
    println!(
        "{}",
        json!({"contract":"optional-suffix","profile":format!("pssz{}",F::OFFSET_BYTES*8),"name":case,"type":ty,"value":value,"schema":schema,"root_schema":root_schema,"views":observations,"option_value":option_value,"expected_accept":case!="node_depth_65_reject","invalid_fixture_path":if case=="node_depth_65_reject" {json!("independent-byte-builder")} else {serde_json::Value::Null},"hex":hex::encode(bytes)})
    );
}
// Deliberately bypass the supported checked encoder only to construct a
// resource-invalid decoder fixture. This is not a production encoding API.
fn invalid_depth_fixture_bytes<F: PsszFormat>(links: usize) -> Vec<u8> {
    let width = F::OFFSET_BYTES;
    let mut bytes = vec![0; width + 1];
    psio::pssz_checked::write_offset::<F>(&mut bytes[..width], 1).unwrap();
    bytes[width] = 7;
    for _ in 0..links {
        let fixed = 1 + width;
        let mut parent = vec![0; width + fixed];
        psio::pssz_checked::write_offset::<F>(&mut parent[..width], fixed).unwrap();
        parent[width] = 1;
        psio::pssz_checked::write_offset::<F>(&mut parent[width + 1..], fixed).unwrap();
        parent.extend(bytes);
        bytes = parent;
    }
    bytes
}
fn node(links: usize) -> DepthNode {
    let mut value = DepthNode {
        value: 7,
        next: None,
    };
    for _ in 0..links {
        value = DepthNode {
            value: 1,
            next: Some(Box::new(value)),
        };
    }
    value
}
fn profile<F: PsszFormat>() {
    emit::<F, _>("base", "Base", Base { name: "a".into() });
    emit::<F, _>(
        "extended_some",
        "Extended",
        Extended {
            name: "a".into(),
            extra: Some("b".into()),
        },
    );
    emit::<F, _>(
        "extended_none",
        "Extended",
        Extended {
            name: "a".into(),
            extra: None,
        },
    );
    emit::<F, _>(
        "twice_empty_middle",
        "Twice",
        Twice {
            name: "a".into(),
            middle: None,
            last: Some("b".into()),
        },
    );
    emit::<F, _>(
        "twice_pruned_last",
        "Twice",
        Twice {
            name: "a".into(),
            middle: Some(9),
            last: None,
        },
    );
    emit::<F, _>(
        "odd_base",
        "OddBase",
        OddBase {
            flag: true,
            name: "a".into(),
        },
    );
    emit::<F, _>(
        "odd_extended_some",
        "OddExtended",
        OddExtended {
            flag: true,
            name: "a".into(),
            extra: Some("b".into()),
        },
    );
    emit::<F, _>(
        "odd_extended_none",
        "OddExtended",
        OddExtended {
            flag: true,
            name: "a".into(),
            extra: None,
        },
    );
    emit::<F, _>("option_record_none", "Option<Base>", Option::<Base>::None);
    emit::<F, _>(
        "option_record_some",
        "Option<Base>",
        Some(Base { name: "a".into() }),
    );
    emit::<F, _>("option_unit_none", "Option<Unit>", Option::<Unit>::None);
    emit::<F, _>("option_unit_some", "Option<Unit>", Some(Unit));
    emit::<F, _>(
        "nested_option_none",
        "Option<Option<String>>",
        Option::<Option<String>>::None,
    );
    emit::<F, _>(
        "nested_option_some_none",
        "Option<Option<String>>",
        Some(Option::<String>::None),
    );
    emit::<F, _>(
        "nested_option_some_empty",
        "Option<Option<String>>",
        Some(Some(String::new())),
    );
    emit::<F, _>("zero_argument_tuple", "()", ());
    emit::<F, _>("positional_tuple", "(u32,String)", (7u32, "hi".to_string()));
    emit::<F, _>(
        "nested_record_vector",
        "NewOuter",
        NewOuter {
            inner: Extended {
                name: "a".into(),
                extra: Some("b".into()),
            },
            values: vec![Extended {
                name: "c".into(),
                extra: Some("d".into()),
            }],
        },
    );
    let bytes = try_encode::<F, _>(&node(31)).unwrap();
    assert!(decode_strict::<F, DepthNode>(&bytes).is_ok());
    emit::<F, _>("node_depth_63", "DepthNode", node(31));
    let bytes = try_encode::<F, _>(&Some(node(31))).unwrap();
    assert!(decode_strict::<F, Option<DepthNode>>(&bytes).is_ok());
    emit::<F, _>("node_depth_64", "Option<DepthNode>", Some(node(31)));
    let bytes = invalid_depth_fixture_bytes::<F>(32);
    assert!(decode_strict::<F, DepthNode>(&bytes).is_err());
    emit::<F, _>("node_depth_65_reject", "DepthNode", node(32));
}
fn main() {
    profile::<Pssz8>();
    profile::<Pssz16>();
    profile::<Pssz32>();
}
