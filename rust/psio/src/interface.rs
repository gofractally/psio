//! Methods and interfaces: a trait's functions described once, for calls,
//! dispatch and the Schema IR.
//!
//! The Rust counterpart of C++ `PSIO_REFLECT(T, method(name, params...))`
//! and `PSIO_INTERFACE(T)`. `#[psio::interface]` on a trait declares the
//! interface; nothing else restates a function's name, its parameter names
//! or its types.
//!
//! ```
//! #[psio::interface(name = "token")]
//! pub trait Token {
//!     fn transfer(&mut self, to: String, amount: u64, memo: String);
//!     fn balance_of(&self, owner: String) -> u64;
//!     fn version() -> u32;
//! }
//!
//! // An implementation.
//! #[derive(Default)]
//! struct Ledger(std::collections::BTreeMap<String, u64>);
//! impl Token for Ledger {
//!     fn transfer(&mut self, to: String, amount: u64, _memo: String) {
//!         *self.0.entry(to).or_default() += amount;
//!     }
//!     fn balance_of(&self, owner: String) -> u64 {
//!         self.0.get(&owner).copied().unwrap_or(0)
//!     }
//!     fn version() -> u32 { 1 }
//! }
//!
//! use psio::interface::{Interface, Method};
//!
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! // Metadata: names, parameter names, the order of declaration.
//! assert_eq!(token::Tag::NAME, "token");
//! assert_eq!(token::Tag::METHODS[0].name, "transfer");
//! assert_eq!(token::Tag::METHODS[0].params, ["to", "amount", "memo"]);
//! assert_eq!(token::Tag::index_of_method("balance_of"), Some(1));
//!
//! // The arguments of a call are a record named after the method. Its pSSZ
//! // is the pSSZ of the tuple of the parameters.
//! let args = token::args::transfer { to: "bob".into(), amount: 5, memo: "hi".into() };
//! let bytes = psio::to_pssz(&args);
//! assert_eq!(bytes, psio::to_pssz(&("bob".to_string(), 5u64, "hi".to_string())));
//! assert_eq!(<token::args::transfer as Method>::NAME, "transfer");
//!
//! // Dispatch: by index or by name, from encoded arguments to an encoded
//! // result. Arguments are validated.
//! let mut ledger = Ledger::default();
//! token::dispatch(&mut ledger, 0, &bytes)?;
//! let reply = token::dispatch_name(&mut ledger, "balance_of", &psio::to_pssz(&("bob".to_string(),)))?;
//! assert_eq!(psio::from_pssz::<u64>(&reply)?, 5);
//!
//! // The Schema IR: the Interface and Func nodes C++ emits for the same
//! // declaration.
//! let mut builder = psio::pssz::schema_ir::SchemaBuilder::new();
//! builder.insert_interface::<token::Tag>()?;
//! let schema = builder.finish();
//! assert_eq!(schema.interfaces[0].funcs[1].name, "balance_of");
//! # Ok(())
//! # }
//! ```
//!
//! # What the attribute generates
//!
//! For `trait Token`, next to the unchanged trait, a module named after it
//! in snake case (`token`; `module = "name"` chooses another):
//!
//! | item | what it is | C++ |
//! |------|------------|-----|
//! | `token::Tag` | the interface as a type: [`Interface`] | the tag of `PSIO_INTERFACE` |
//! | `token::args::<method>` | the argument record of a method: [`Method`], `PsszPack`, `PsszUnpack`, `ToPsszSchema`, `JsonText` | `reflect<T>::method_arg_tuple<I>` |
//! | `token::dispatch`, `token::dispatch_name` | decode the arguments, call, encode the result | `reflect<T>::invoke_method<I>` over a decoded tuple |
//! | `token::Proxy<C>` | one function per method that hands the argument record to a [`Caller`] | `reflect<T>::proxy<P>` |
//!
//! Options: `name = "psi:db/store"` (the interface's name, default the
//! trait's identifier), `package = "..."` and `version = "..."` (the package
//! the schema reports), `types(A, B)` (the types the interface owns,
//! `Interface::type_names`), `module = "name"`, `derive(...)` (more derives
//! for the argument records, such as `Debug, Clone, PartialEq, psio::Serde`),
//! `crate = "path"`.
//!
//! The trait is declared in a module, not inside a function body: the
//! generated module names the trait and the parameter types through
//! `super`, which a function's local items are not reachable by. Parameter
//! and result types are named as the trait's module sees them (not through
//! `self::` or `super::`).
//!
//! # The pSSZ of a function's arguments
//!
//! One rule, the same in C++ and Rust: **the arguments of a call are the
//! tuple of the function's parameter types** (C++: the decayed types,
//! `std::tuple<std::remove_cvref_t<Args>...>`; Rust: the types as declared,
//! which are owned values), **in declaration order, with the tuple's
//! inferred offset width.** A function of no parameters has the empty
//! tuple. In pSSZ a tuple and a record with the same members have the same
//! bytes and the same width (extensible: a trailing `Option` that is `None`
//! is not written and one that is missing reads as `None`), which is why
//! the argument type here can be a record with named members:
//! `psio::to_pssz(&args::transfer { .. })` equals
//! `psio::to_pssz(&(to, amount, memo))`, and
//! `psio::pssz::auto_pssz_width::<args::transfer>()` is the tuple's.
//! Each argument record converts to and from its tuple (`From` both ways,
//! `<args::transfer as Method>::Tuple` names the tuple type).
//!
//! In text the two differ: the record is an object keyed by parameter name,
//! the tuple an array. C++ `method_arg_tuple<I>` is a tuple; use the tuple
//! type where C++'s text of the arguments is wanted.
//!
//! A method with no result (`()`) encodes its result as no bytes, and its
//! `Func` has no `result`.
//!
//! # What a trait may hold
//!
//! Functions only, each taking `&self`, `&mut self` or no `self`, with
//! named parameters of owned types and an owned result. The rest is a
//! compile error that says so:
//!
//! ```compile_fail
//! #[psio::interface]
//! trait Store {
//!     fn get(&self, key: &str) -> Vec<u8>; // a parameter is an owned value
//! }
//! # fn main() {}
//! ```
//!
//! ```compile_fail
//! #[psio::interface]
//! trait Store {
//!     fn put<V: Into<Vec<u8>>>(&mut self, key: String, value: V); // not generic
//! }
//! # fn main() {}
//! ```
//!
//! ```compile_fail
//! #[psio::interface]
//! trait Store {
//!     fn close(self); // `&self`, `&mut self` or no `self`
//! }
//! # fn main() {}
//! ```
//!
//! ```compile_fail
//! #[psio::interface]
//! trait Store {
//!     #[pssz(max_dynamic_data = 256)] // not yet: see Annotations
//!     fn put(&mut self, key: String, value: Vec<u8>);
//! }
//! # fn main() {}
//! ```
//!
//! ```compile_fail
//! #[psio::interface(nmae = "store")] // an unknown key
//! trait Store {
//!     fn get(&self, key: String) -> Vec<u8>;
//! }
//! # fn main() {}
//! ```
//!
//! # Annotations
//!
//! A parameter carries no annotation yet, and a method no argument cap:
//! `#[psio(...)]`, `#[pssz(...)]` and `#[json(...)]` on a method or a
//! parameter are compile errors until member bounds land in Rust. Their
//! place is fixed by this design, the same attributes a record takes: a
//! parameter's annotation is its member's in the argument record
//! (C++ `attr(param, max<N>)`), a method's is the record's
//! (C++ `maxDynamicData(N)`: `#[pssz(max_dynamic_data = N)]`).

use crate::pssz::schema_ir;
use crate::pssz::{Pssz, PsszError, PsszPack, PsszResult, PsszUnpack};


/// How a method takes the object it is called on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Receiver {
    /// No object: `fn name(args...)` (C++: a static member function).
    None,
    /// `&self` (C++: a const member function).
    Ref,
    /// `&mut self`.
    Mut,
}

/// A method of an interface, as declared.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MethodInfo {
    /// The function's identifier.
    pub name: &'static str,
    /// The parameter names, in order.
    pub params: &'static [&'static str],
    pub receiver: Receiver,
    /// Whether the function returns a value (anything but `()`).
    pub has_result: bool,
}

/// An interface: the functions of a trait annotated with
/// `#[psio::interface]`, implemented by the generated `Tag`.
pub trait Interface: 'static {
    /// The interface's name: `name = "..."`, else the trait's identifier.
    const NAME: &'static str;
    /// The package's name and version, when the declaration names one.
    const PACKAGE: Option<(&'static str, &'static str)>;
    /// The methods in declaration order; a method's index is its position.
    const METHODS: &'static [MethodInfo];

    /// The index of the method called `name`.
    fn index_of_method(name: &str) -> Option<usize> {
        Self::METHODS.iter().position(|method| method.name == name)
    }

    /// The interface as a Schema IR node; parameter, result and owned types
    /// are inserted into `builder`. Use
    /// [`SchemaBuilder::insert_interface`](schema_ir::SchemaBuilder::insert_interface).
    fn build_interface(builder: &mut schema_ir::SchemaBuilder) -> PsszResult<schema_ir::Interface>;
}

/// A method, implemented by its argument record.
pub trait Method: PsszPack<Pssz> + PsszUnpack<Pssz> + Sized {
    type Interface: Interface;
    /// The tuple of the parameter types: the type whose pSSZ the arguments
    /// have, and what C++ `method_arg_tuple<I>` is. The record converts to
    /// and from it.
    type Tuple: PsszPack<Pssz> + PsszUnpack<Pssz> + From<Self> + Into<Self>;
    /// The function's return type; `()` when it returns nothing.
    type Result;
    /// Position in [`Interface::METHODS`].
    const INDEX: usize;
    const NAME: &'static str;
    const PARAM_NAMES: &'static [&'static str];
    const HAS_RESULT: bool;

    /// The pSSZ of a result; no bytes for a method that returns nothing.
    fn result_to_pssz(result: &Self::Result) -> PsszResult<Vec<u8>>;
    /// Reads a result (validated); a method that returns nothing accepts
    /// only no bytes.
    fn result_from_pssz(bytes: &[u8]) -> PsszResult<Self::Result>;
}

/// What a proxy hands each call to (C++: the `P` of `reflect<T>::proxy<P>`).
///
/// `Output<R>` is what a call that returns `R` gives the caller: `R` itself
/// for a synchronous call, a `Result`, a future, an encoded action.
///
/// ```
/// use psio::interface::{Caller, Method};
///
/// #[psio::interface]
/// trait Counter {
///     fn add(&mut self, amount: u32) -> u32;
/// }
///
/// /// Encodes each call as (method name, argument bytes).
/// struct Encode;
/// impl Caller for Encode {
///     type Output<R> = (&'static str, Vec<u8>);
///     fn call<M: Method>(&self, args: M) -> Self::Output<M::Result> {
///         (M::NAME, psio::to_pssz(&args))
///     }
/// }
///
/// fn main() {
///     let proxy = counter::Proxy(Encode);
///     assert_eq!(proxy.add(7), ("add", psio::to_pssz(&(7u32,))));
/// }
/// ```
pub trait Caller {
    type Output<R>;
    fn call<M: Method>(&self, args: M) -> Self::Output<M::Result>;
}

/// Why a dispatched call did not happen or could not answer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DispatchError {
    /// No method has this index or name.
    UnknownMethod,
    /// The argument bytes are not a valid encoding of the method's arguments.
    Arguments(PsszError),
    /// The result is not representable (a bound or an invariant refuses it).
    Result(PsszError),
}

impl std::fmt::Display for DispatchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DispatchError::UnknownMethod => f.write_str("interface: unknown method"),
            DispatchError::Arguments(error) => write!(f, "interface: arguments: {error}"),
            DispatchError::Result(error) => write!(f, "interface: result: {error}"),
        }
    }
}
impl std::error::Error for DispatchError {}

/// A lookup from a caller-chosen key to a method index, for dispatch by
/// anything other than the name: a number, a hash of the name, a compressed
/// name (C++ `reflect<T>::index_of_method_hash<Hash>`). Two methods with one
/// key are refused when the table is built.
///
/// ```
/// #[psio::interface]
/// trait Counter {
///     fn add(&mut self, amount: u32) -> u32;
///     fn reset(&mut self);
/// }
/// use psio::interface::MethodTable;
///
/// fn main() {
///     // By compressed method name (C++ `psio::detail::method_to_number`).
///     use psio::compress_name::method_to_number;
///     let table = MethodTable::of::<counter::Tag, _>(|method| method_to_number(method.name)).unwrap();
///     assert_eq!(table.index_of(&method_to_number("reset")), Some(1));
///     assert_eq!(table.index_of(&0), None);
/// }
/// ```
#[derive(Debug, Clone)]
pub struct MethodTable<K> {
    /// Sorted by key.
    keys: Vec<(K, usize)>,
}

/// Two methods of an interface have the same key.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DuplicateMethodKey {
    pub first: &'static str,
    pub second: &'static str,
}

impl std::fmt::Display for DuplicateMethodKey {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "interface: methods {} and {} have the same key", self.first, self.second)
    }
}
impl std::error::Error for DuplicateMethodKey {}

impl<K: Ord> MethodTable<K> {
    pub fn of<I: Interface, F: Fn(&MethodInfo) -> K>(key: F) -> Result<Self, DuplicateMethodKey> {
        let mut keys: Vec<(K, usize)> =
            I::METHODS.iter().enumerate().map(|(index, method)| (key(method), index)).collect();
        keys.sort();
        for pair in keys.windows(2) {
            if pair[0].0 == pair[1].0 {
                return Err(DuplicateMethodKey {
                    first: I::METHODS[pair[0].1].name,
                    second: I::METHODS[pair[1].1].name,
                });
            }
        }
        Ok(Self { keys })
    }
    pub fn index_of(&self, key: &K) -> Option<usize> {
        self.keys.binary_search_by(|(k, _)| k.cmp(key)).ok().map(|at| self.keys[at].1)
    }
}

impl schema_ir::SchemaBuilder {
    /// Adds the interface `I` with its functions to the schema, inserting
    /// the types its functions use (C++ `SchemaBuilder::insert_interface`).
    /// The schema's package is the interface's when it names one.
    pub fn insert_interface<I: Interface>(&mut self) -> PsszResult<&mut Self> {
        let interface = I::build_interface(self)?;
        self.push_interface(interface, I::PACKAGE);
        Ok(self)
    }
}

/// The name a type has in `builder`'s schema, inserting it if needed
/// (C++ `SchemaBuilder::type_name_for`). Used by the generated
/// `Interface::build_interface`.
#[doc(hidden)]
pub fn type_name_for<T: schema_ir::ToPsszSchema>(builder: &mut schema_ir::SchemaBuilder) -> PsszResult<String> {
    match builder.insert::<T>()?.value {
        schema_ir::AnyValue::Type(reference) => Ok(reference.r#type),
        _ => Err(PsszError::new("pssz schema: an inserted type is a reference")),
    }
}

/// A parameter of a function: its type inserted in `builder`, with the
/// attributes a member of that type carries. Used by the generated
/// `Interface::build_interface`.
#[doc(hidden)]
pub fn param<T: schema_ir::ToPsszSchema>(
    builder: &mut schema_ir::SchemaBuilder,
    name: &str,
) -> PsszResult<schema_ir::Member> {
    Ok(schema_ir::Member {
        name: name.into(),
        r#type: Box::new(builder.insert::<T>()?),
        attributes: T::member_attributes(),
    })
}
