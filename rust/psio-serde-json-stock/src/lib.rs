//! Test only. A program that links `psio` with its default features has the
//! `serde_json` it would have without `psio`: Cargo unifies features across
//! the dependency graph, so a `serde_json` feature required by a library is
//! imposed on every crate of the program. `tests/stock.rs` asserts that
//! `psio` imposes none that changes behaviour.

/// A PSIO type, so the crate links and uses `psio`.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Serde)]
pub struct Transfer {
    pub to: String,
    pub amount: u64,
    pub ratio: f64,
    pub memo: Option<String>,
}
