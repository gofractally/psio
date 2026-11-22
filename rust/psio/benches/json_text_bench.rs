//! Typed JSON / I-JSON text: decode and encode timings on the shapes of
//! `cpp/benchmarks/bench_json_text.cpp`, and the raw parse speed of
//! `serde_json` and `simd-json` on the same inputs (candidate syntax layers).
//!
//!     cargo bench --bench json_text_bench [-- <filter>]
//!
//! Minimum of repeated short trials, ns per document and MB/s.

use psio::json::{self, IJson, Json, JsonFormat, JsonText};
use std::hint::black_box;
use std::time::Instant;

#[derive(psio::JsonText, Clone, Default)]
struct Small {
    id: u32,
    name: String,
    active: bool,
    score: f64,
    count: u64,
}

#[derive(psio::JsonText, Clone, Default)]
struct Address {
    street: String,
    city: String,
    region: Option<String>,
    zip: u32,
}

#[derive(psio::JsonText, Clone, Default)]
struct LineItem {
    sku: u32,
    title: String,
    quantity: u32,
    price: f64,
    discount: Option<u32>,
}

#[derive(psio::JsonText, Clone, Default)]
struct Card {
    last4: String,
    expiry: u32,
}

#[derive(psio::JsonText, Clone)]
enum Payment {
    #[psio(name = "uint32")]
    Account(u32),
    #[psio(name = "string")]
    Token(String),
    Card(Card),
}

#[derive(psio::JsonText, Clone)]
struct Medium {
    order_id: u64,
    customer: String,
    email: String,
    shipping: Address,
    billing: Option<Address>,
    payment: Payment,
    items: Vec<LineItem>,
    note: Option<String>,
    tags: Vec<String>,
    gift: bool,
}

#[derive(psio::JsonText, Clone, Default)]
struct Wide200 {
    field_000: u32,
    field_001_b: u32,
    field_002_cc: u32,
    field_003_ddd: u32,
    field_004_eeee: u32,
    field_005_fffff: u32,
    field_006_gggggg: u32,
    field_007: u32,
    field_008_i: u32,
    field_009_jj: u32,
    field_010_aaa: u32,
    field_011_bbbb: u32,
    field_012_ccccc: u32,
    field_013_dddddd: u32,
    field_014: u32,
    field_015_f: u32,
    field_016_gg: u32,
    field_017_hhh: u32,
    field_018_iiii: u32,
    field_019_jjjjj: u32,
    field_020_aaaaaa: u32,
    field_021: u32,
    field_022_c: u32,
    field_023_dd: u32,
    field_024_eee: u32,
    field_025_ffff: u32,
    field_026_ggggg: u32,
    field_027_hhhhhh: u32,
    field_028: u32,
    field_029_j: u32,
    field_030_aa: u32,
    field_031_bbb: u32,
    field_032_cccc: u32,
    field_033_ddddd: u32,
    field_034_eeeeee: u32,
    field_035: u32,
    field_036_g: u32,
    field_037_hh: u32,
    field_038_iii: u32,
    field_039_jjjj: u32,
    field_040_aaaaa: u32,
    field_041_bbbbbb: u32,
    field_042: u32,
    field_043_d: u32,
    field_044_ee: u32,
    field_045_fff: u32,
    field_046_gggg: u32,
    field_047_hhhhh: u32,
    field_048_iiiiii: u32,
    field_049: u32,
    field_050_a: u32,
    field_051_bb: u32,
    field_052_ccc: u32,
    field_053_dddd: u32,
    field_054_eeeee: u32,
    field_055_ffffff: u32,
    field_056: u32,
    field_057_h: u32,
    field_058_ii: u32,
    field_059_jjj: u32,
    field_060_aaaa: u32,
    field_061_bbbbb: u32,
    field_062_cccccc: u32,
    field_063: u32,
    field_064_e: u32,
    field_065_ff: u32,
    field_066_ggg: u32,
    field_067_hhhh: u32,
    field_068_iiiii: u32,
    field_069_jjjjjj: u32,
    field_070: u32,
    field_071_b: u32,
    field_072_cc: u32,
    field_073_ddd: u32,
    field_074_eeee: u32,
    field_075_fffff: u32,
    field_076_gggggg: u32,
    field_077: u32,
    field_078_i: u32,
    field_079_jj: u32,
    field_080_aaa: u32,
    field_081_bbbb: u32,
    field_082_ccccc: u32,
    field_083_dddddd: u32,
    field_084: u32,
    field_085_f: u32,
    field_086_gg: u32,
    field_087_hhh: u32,
    field_088_iiii: u32,
    field_089_jjjjj: u32,
    field_090_aaaaaa: u32,
    field_091: u32,
    field_092_c: u32,
    field_093_dd: u32,
    field_094_eee: u32,
    field_095_ffff: u32,
    field_096_ggggg: u32,
    field_097_hhhhhh: u32,
    field_098: u32,
    field_099_j: u32,
    field_100_aa: u32,
    field_101_bbb: u32,
    field_102_cccc: u32,
    field_103_ddddd: u32,
    field_104_eeeeee: u32,
    field_105: u32,
    field_106_g: u32,
    field_107_hh: u32,
    field_108_iii: u32,
    field_109_jjjj: u32,
    field_110_aaaaa: u32,
    field_111_bbbbbb: u32,
    field_112: u32,
    field_113_d: u32,
    field_114_ee: u32,
    field_115_fff: u32,
    field_116_gggg: u32,
    field_117_hhhhh: u32,
    field_118_iiiiii: u32,
    field_119: u32,
    field_120_a: u32,
    field_121_bb: u32,
    field_122_ccc: u32,
    field_123_dddd: u32,
    field_124_eeeee: u32,
    field_125_ffffff: u32,
    field_126: u32,
    field_127_h: u32,
    field_128_ii: u32,
    field_129_jjj: u32,
    field_130_aaaa: u32,
    field_131_bbbbb: u32,
    field_132_cccccc: u32,
    field_133: u32,
    field_134_e: u32,
    field_135_ff: u32,
    field_136_ggg: u32,
    field_137_hhhh: u32,
    field_138_iiiii: u32,
    field_139_jjjjjj: u32,
    field_140: u32,
    field_141_b: u32,
    field_142_cc: u32,
    field_143_ddd: u32,
    field_144_eeee: u32,
    field_145_fffff: u32,
    field_146_gggggg: u32,
    field_147: u32,
    field_148_i: u32,
    field_149_jj: u32,
    field_150_aaa: u32,
    field_151_bbbb: u32,
    field_152_ccccc: u32,
    field_153_dddddd: u32,
    field_154: u32,
    field_155_f: u32,
    field_156_gg: u32,
    field_157_hhh: u32,
    field_158_iiii: u32,
    field_159_jjjjj: u32,
    field_160_aaaaaa: u32,
    field_161: u32,
    field_162_c: u32,
    field_163_dd: u32,
    field_164_eee: u32,
    field_165_ffff: u32,
    field_166_ggggg: u32,
    field_167_hhhhhh: u32,
    field_168: u32,
    field_169_j: u32,
    field_170_aa: u32,
    field_171_bbb: u32,
    field_172_cccc: u32,
    field_173_ddddd: u32,
    field_174_eeeeee: u32,
    field_175: u32,
    field_176_g: u32,
    field_177_hh: u32,
    field_178_iii: u32,
    field_179_jjjj: u32,
    field_180_aaaaa: u32,
    field_181_bbbbbb: u32,
    field_182: u32,
    field_183_d: u32,
    field_184_ee: u32,
    field_185_fff: u32,
    field_186_gggg: u32,
    field_187_hhhhh: u32,
    field_188_iiiiii: u32,
    field_189: u32,
    field_190_a: u32,
    field_191_bb: u32,
    field_192_ccc: u32,
    field_193_dddd: u32,
    field_194_eeeee: u32,
    field_195_ffffff: u32,
    field_196: u32,
    field_197_h: u32,
    field_198_ii: u32,
    field_199_jjj: u32,
}

fn small() -> Small {
    Small {
        id: 1234567,
        name: "a small record".into(),
        active: true,
        score: 0.8125,
        count: 9007199254740991,
    }
}

fn medium() -> Medium {
    Medium {
        order_id: 9007199254740993,
        customer: "Ada Lovelace".into(),
        email: "ada@example.org".into(),
        shipping: Address {
            street: "12 Analytical Engine Way".into(),
            city: "London".into(),
            region: Some("Greater London".into()),
            zip: 10100,
        },
        billing: Some(Address {
            street: "1 Difference Street".into(),
            city: "Cambridge".into(),
            region: None,
            zip: 20200,
        }),
        payment: Payment::Card(Card { last4: "4242".into(), expiry: 1230 }),
        items: (0..20u32)
            .map(|i| LineItem {
                sku: 100000 + i * 7,
                title: format!("item number {i} of the order"),
                quantity: 1 + i % 4,
                price: 9.99 + i as f64 * 1.25,
                discount: if i % 3 == 0 { Some(5 + i) } else { None },
            })
            .collect(),
        note: Some("leave with the \"porter\" \\ ring twice".into()),
        tags: vec!["priority".into(), "fragile".into(), "gift-wrap".into()],
        gift: true,
    }
}

fn large() -> Vec<Small> {
    (0..10000u32)
        .map(|i| Small {
            id: i.wrapping_mul(2654435761),
            name: format!("record {i}"),
            active: i & 1 != 0,
            score: i as f64 * 0.37,
            count: i as u64 * 1000003,
        })
        .collect()
}

/// The members of a one-level object text, in reverse order.
fn reversed(text: &str) -> String {
    let inner = &text[1..text.len() - 1];
    let mut members: Vec<&str> = inner.split(',').collect();
    members.reverse();
    format!("{{{}}}", members.join(","))
}

fn measure(mut body: impl FnMut()) -> f64 {
    let mut iters = 1u64;
    loop {
        let start = Instant::now();
        for _ in 0..iters {
            body();
        }
        let ns = start.elapsed().as_nanos() as f64;
        if ns >= 4_000_000.0 || iters >= 1 << 30 {
            break;
        }
        iters *= if ns < 400_000.0 { 8 } else { 2 };
    }
    let mut best = f64::MAX;
    for _ in 0..60 {
        let start = Instant::now();
        for _ in 0..iters {
            body();
        }
        best = best.min(start.elapsed().as_nanos() as f64 / iters as f64);
    }
    best
}

fn report(shape: &str, format: &str, op: &str, bytes: usize, ns: f64) {
    println!(
        "{shape:<9} {format:<6} {op:<16} {bytes:>8} {ns:>12.1} {:>9.1}",
        bytes as f64 / ns * 1000.0
    );
}

fn wanted(filter: &Option<String>, shape: &str) -> bool {
    filter.as_ref().map_or(true, |f| shape.contains(f.as_str()))
}

fn typed<F: JsonFormat, T: JsonText>(shape: &str, format: &str, text: &str) {
    let ns = measure(|| {
        black_box(json::decode::<F, T>(black_box(text)).unwrap());
    });
    report(shape, format, "decode", text.len(), ns);
}

fn shape<T: JsonText>(filter: &Option<String>, name: &str, value: &T) {
    if !wanted(filter, name) {
        return;
    }
    let text = psio::to_json(value);
    let itext = psio::to_ijson(value);
    typed::<Json, T>(name, "json", &text);
    typed::<IJson, T>(name, "ijson", &itext);
    let ns = measure(|| {
        black_box(psio::to_json(black_box(value)));
    });
    report(name, "json", "encode", text.len(), ns);
    let ns = measure(|| {
        black_box(psio::to_ijson(black_box(value)));
    });
    report(name, "ijson", "encode", itext.len(), ns);
    syntax_layers(name, &text);
}

/// Raw parse speed of the candidate syntax layers: no typed value is built.
fn syntax_layers(name: &str, text: &str) {
    let ns = measure(|| {
        black_box(
            serde_json::from_str::<serde::de::IgnoredAny>(black_box(text)).unwrap(),
        );
    });
    report(name, "json", "serde_json skip", text.len(), ns);
    let ns = measure(|| {
        black_box(serde_json::from_str::<serde_json::Value>(black_box(text)).unwrap());
    });
    report(name, "json", "serde_json Value", text.len(), ns);
    let mut buffer = text.as_bytes().to_vec();
    let ns = measure(|| {
        buffer.copy_from_slice(text.as_bytes());
        black_box(simd_json::to_tape(black_box(&mut buffer)).unwrap());
    });
    report(name, "json", "simd-json tape", text.len(), ns);
}

fn main() {
    let filter = std::env::args().skip(1).find(|a| !a.starts_with('-'));
    println!(
        "{:<9} {:<6} {:<16} {:>8} {:>12} {:>9}",
        "shape", "format", "op", "bytes", "ns/op", "MB/s"
    );
    shape(&filter, "small", &small());
    shape(&filter, "medium", &medium());
    shape(&filter, "large", &large());
    let mut wide = Wide200::default();
    wide.field_007 = 7;
    wide.field_199_jjj = 199;
    shape(&filter, "wide200", &wide);
    if wanted(&filter, "wide200r") {
        let text = reversed(&psio::to_json(&wide));
        typed::<Json, Wide200>("wide200r", "json", &text);
        typed::<IJson, Wide200>("wide200r", "ijson", &text);
    }
}
