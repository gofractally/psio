//! Formats kept to measure design trade-offs.
//!
//! Nothing here is a supported wire format. An experimental format has no
//! stability guarantee, no helper names at the crate root (`to_F`, `from_F`,
//! `validate_F`, ...) and no `Format` tag; it can change or disappear in any
//! release. Each one is compiled only when its cargo feature is enabled.
//!
//! | module | feature | what it measures |
//! |--------|---------|------------------|
//! | `frac` | `experimental-frac` | a size per field against sizes inferred from offset pointers |

#[cfg(feature = "experimental-frac")]
pub mod frac;
