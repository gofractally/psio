use fracpack::{Pack, Unpack};
use psio::{PsszPack, PsszUnpack};
use psio::pssz::{try_encode, decode_strict, Pssz32};
use serde_json::json;

#[derive(Debug, Pack, Unpack, PsszPack, PsszUnpack)]
#[fracpack(fracpack_mod = "fracpack")]
struct V1 { name: String }
#[derive(Debug, Pack, Unpack, PsszPack, PsszUnpack)]
#[fracpack(fracpack_mod = "fracpack")]
struct V2String { name: String, extra: String }
#[derive(Debug, Pack, Unpack, PsszPack, PsszUnpack)]
#[fracpack(fracpack_mod = "fracpack")]
struct V2Fixed { name: String, extra: u32 }
#[derive(Debug, Pack, Unpack, PsszPack, PsszUnpack)]
#[fracpack(fracpack_mod = "fracpack")]
struct V2Optional { name: String, extra: Option<String> }

fn hex(bytes: &[u8]) -> String { bytes.iter().map(|b| format!("{b:02x}")).collect() }
fn result<T: std::fmt::Debug, E: std::fmt::Debug>(read: impl FnOnce() -> Result<T,E> + std::panic::UnwindSafe) -> serde_json::Value {
    match std::panic::catch_unwind(read) {
        Ok(Ok(v)) => json!({"ok":format!("{v:?}")}),
        Ok(Err(e)) => json!({"error":format!("{e:?}")}),
        Err(_) => json!({"panic":"decoder panicked; see captured stderr"}),
    }
}
fn emit<T: Pack + psio::pssz::PsszPack<Pssz32>>(name: &str, value: T) {
    let frac = value.packed(); let pssz = try_encode::<Pssz32,_>(&value).unwrap();
    println!("{}",json!({"case":name,"format":"old-psibase-fracpack","hex":hex(&frac),"size":frac.len(),
        "read_v1":result(||V1::unpacked(&frac)),"read_v2_string":result(||V2String::unpacked(&frac)),
        "read_v2_fixed":result(||V2Fixed::unpacked(&frac)),"read_v2_optional":result(||V2Optional::unpacked(&frac))}));
    println!("{}",json!({"case":name,"format":"maintained-rust-pssz32-exact","hex":hex(&pssz),"size":pssz.len(),
        "read_v1":result(||decode_strict::<Pssz32,V1>(&pssz)),"read_v2_string":result(||decode_strict::<Pssz32,V2String>(&pssz)),
        "read_v2_fixed":result(||decode_strict::<Pssz32,V2Fixed>(&pssz)),"read_v2_optional":result(||decode_strict::<Pssz32,V2Optional>(&pssz))}));
}
fn main() {
    emit("v1",V1{name:"a".into()});
    emit("v2_string",V2String{name:"a".into(),extra:"b".into()});
    emit("v2_string_default",V2String{name:"a".into(),extra:"".into()});
    emit("v2_fixed",V2Fixed{name:"a".into(),extra:7});
    emit("v2_fixed_ambiguous",V2Fixed{name:"ab".into(),extra:9});
    emit("v2_fixed_default",V2Fixed{name:"a".into(),extra:0});
    emit("v2_optional_some",V2Optional{name:"a".into(),extra:Some("b".into())});
    emit("v2_optional_none",V2Optional{name:"a".into(),extra:None});
}
