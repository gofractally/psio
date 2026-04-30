//! Schema import: parse .capnp, .fbs, and .wit IDL into DynamicSchema.
//!
//! Three parsers are provided:
//!
//! - [`capnp_parser`] -- Cap'n Proto `.capnp` IDL
//! - [`fbs_parser`] -- FlatBuffers `.fbs` IDL
//! - [`wit_parser`] -- WebAssembly Interface Types `.wit` IDL
//!
//! Each parser produces a format-specific AST (e.g. [`ParsedCapnpFile`]) and
//! can convert parsed types to [`DynamicSchema`](crate::dynamic_schema::DynamicSchema)
//! for format-agnostic runtime field access.

pub mod capnp_parser;
pub mod fbs_parser;
pub mod wit_parser;

// Re-exports for convenience
pub use capnp_parser::{parse_capnp, ParsedCapnpFile};
pub use fbs_parser::{parse_fbs, ParsedFbsFile};
pub use wit_parser::{parse_wit, ParsedWitFile};
