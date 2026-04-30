//! WIT (WebAssembly Interface Types) `.wit` IDL parser.
//!
//! Parses WIT IDL into a [`ParsedWitFile`] with computed canonical ABI layout
//! (natural alignment, sizes).
//!
//! Supports: record, variant, enum, flags, type aliases, func, resource,
//! interface, world, use, and package declarations.

use crate::dynamic_schema::{
    AltDesc, DynamicSchema, DynamicType, FieldDesc,
};

use std::collections::HashMap;
use std::fmt;

// =========================================================================
// Public types
// =========================================================================

/// Error returned by the WIT parser.
#[derive(Debug, Clone)]
pub struct ParseError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "wit:{}:{}: {}", self.line, self.column, self.message)
    }
}

impl std::error::Error for ParseError {}

/// WIT built-in type tags.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WitTypeTag {
    Bool,
    U8,
    U16,
    U32,
    U64,
    S8,
    S16,
    S32,
    S64,
    F32,
    F64,
    Char,
    String,
    List,
    Option,
    Result,
    Tuple,
    Record,
    Variant,
    Enum,
    Flags,
    Own,
    Borrow,
    Named,
}

/// A WIT type reference.
#[derive(Debug, Clone)]
pub struct WitTypeRef {
    pub tag: WitTypeTag,
    /// For List/Option: the element type.
    pub inner: Option<Box<WitTypeRef>>,
    /// For Result: the ok type (inner = ok, inner2 = err).
    pub inner2: Option<Box<WitTypeRef>>,
    /// For Tuple: element types.
    pub tuple_types: Vec<WitTypeRef>,
    /// For Named: the type name.
    pub name: String,
}

impl Default for WitTypeRef {
    fn default() -> Self {
        Self {
            tag: WitTypeTag::Bool,
            inner: None,
            inner2: None,
            tuple_types: Vec::new(),
            name: String::new(),
        }
    }
}

impl WitTypeRef {
    fn simple(tag: WitTypeTag) -> Self {
        Self { tag, ..Default::default() }
    }

    fn named(name: &str) -> Self {
        Self { tag: WitTypeTag::Named, name: name.to_string(), ..Default::default() }
    }
}

/// A field in a record.
#[derive(Debug, Clone)]
pub struct WitRecordField {
    pub name: String,
    pub ty: WitTypeRef,
}

/// A case in a variant.
#[derive(Debug, Clone)]
pub struct WitVariantCase {
    pub name: String,
    /// None = no payload (like Void).
    pub ty: Option<WitTypeRef>,
}

/// A parsed enum definition.
#[derive(Debug, Clone)]
pub struct WitEnumDef {
    pub name: String,
    pub cases: Vec<String>,
}

/// A parsed flags definition.
#[derive(Debug, Clone)]
pub struct WitFlagsDef {
    pub name: String,
    pub flags: Vec<String>,
}

/// A parsed record definition.
#[derive(Debug, Clone)]
pub struct WitRecordDef {
    pub name: String,
    pub fields: Vec<WitRecordField>,
}

/// A parsed variant definition.
#[derive(Debug, Clone)]
pub struct WitVariantDef {
    pub name: String,
    pub cases: Vec<WitVariantCase>,
}

/// A parsed type alias.
#[derive(Debug, Clone)]
pub struct WitTypeDef {
    pub name: String,
    pub ty: WitTypeRef,
}

/// A function parameter.
#[derive(Debug, Clone)]
pub struct WitParam {
    pub name: String,
    pub ty: WitTypeRef,
}

/// A parsed function definition.
#[derive(Debug, Clone)]
pub struct WitFuncDef {
    pub name: String,
    pub params: Vec<WitParam>,
    pub results: Vec<WitTypeRef>,
    /// Named results (e.g., `-> (a: u32, b: string)`).
    pub named_results: Vec<WitParam>,
}

/// A parsed resource definition.
#[derive(Debug, Clone)]
pub struct WitResourceDef {
    pub name: String,
    pub methods: Vec<WitFuncDef>,
}

/// A parsed interface.
#[derive(Debug, Clone)]
pub struct WitInterfaceDef {
    pub name: String,
    pub records: Vec<WitRecordDef>,
    pub variants: Vec<WitVariantDef>,
    pub enums: Vec<WitEnumDef>,
    pub flags: Vec<WitFlagsDef>,
    pub type_defs: Vec<WitTypeDef>,
    pub functions: Vec<WitFuncDef>,
    pub resources: Vec<WitResourceDef>,
}

/// A complete parsed .wit file.
#[derive(Debug, Clone)]
pub struct ParsedWitFile {
    pub package: String,
    pub records: Vec<WitRecordDef>,
    pub variants: Vec<WitVariantDef>,
    pub enums: Vec<WitEnumDef>,
    pub flags: Vec<WitFlagsDef>,
    pub type_defs: Vec<WitTypeDef>,
    pub functions: Vec<WitFuncDef>,
    pub resources: Vec<WitResourceDef>,
    pub interfaces: Vec<WitInterfaceDef>,
    pub type_map: HashMap<String, WitTypeRef>,
}

impl ParsedWitFile {
    /// Find a record by name.
    pub fn find_record(&self, name: &str) -> Option<&WitRecordDef> {
        self.records.iter().find(|r| r.name == name)
    }

    /// Find a variant by name.
    pub fn find_variant(&self, name: &str) -> Option<&WitVariantDef> {
        self.variants.iter().find(|v| v.name == name)
    }

    /// Find an enum by name.
    pub fn find_enum(&self, name: &str) -> Option<&WitEnumDef> {
        self.enums.iter().find(|e| e.name == name)
    }

    /// Convert a record to a `DynamicSchema`.
    pub fn to_dynamic_schema(&self, record_name: &str) -> Option<DynamicSchema> {
        let r = self.find_record(record_name)?;
        Some(wit_record_to_dynamic(r, self))
    }
}

// =========================================================================
// WIT Canonical ABI layout computation
// =========================================================================

/// Canonical ABI size and alignment for a WIT type.
#[derive(Debug, Clone, Copy)]
struct AbiLayout {
    size: u32,
    align: u32,
}

fn wit_type_abi(ty: &WitTypeRef, file: &ParsedWitFile) -> AbiLayout {
    match ty.tag {
        WitTypeTag::Bool | WitTypeTag::U8 | WitTypeTag::S8 => AbiLayout { size: 1, align: 1 },
        WitTypeTag::U16 | WitTypeTag::S16 => AbiLayout { size: 2, align: 2 },
        WitTypeTag::U32 | WitTypeTag::S32 | WitTypeTag::F32 | WitTypeTag::Char => {
            AbiLayout { size: 4, align: 4 }
        }
        WitTypeTag::U64 | WitTypeTag::S64 | WitTypeTag::F64 => {
            AbiLayout { size: 8, align: 8 }
        }
        WitTypeTag::String => {
            // Canonical ABI: (i32 ptr, i32 len)
            AbiLayout { size: 8, align: 4 }
        }
        WitTypeTag::List => {
            // Canonical ABI: (i32 ptr, i32 len)
            AbiLayout { size: 8, align: 4 }
        }
        WitTypeTag::Record => {
            // Inline -- but we'd need the record def. For Named refs, resolve.
            AbiLayout { size: 0, align: 1 }
        }
        WitTypeTag::Named => {
            // Resolve through type_map
            if let Some(r) = file.find_record(&ty.name) {
                wit_record_abi(r, file)
            } else {
                // Could be a type alias
                if let Some(resolved) = file.type_map.get(&ty.name) {
                    wit_type_abi(resolved, file)
                } else {
                    AbiLayout { size: 4, align: 4 } // fallback
                }
            }
        }
        WitTypeTag::Option => {
            // discriminant (1 byte) + payload, aligned
            if let Some(inner) = &ty.inner {
                let inner_abi = wit_type_abi(inner, file);
                let align = inner_abi.align.max(1);
                let disc_size = 1u32;
                let payload_offset = align_to(disc_size, inner_abi.align);
                let size = align_to(payload_offset + inner_abi.size, align);
                AbiLayout { size, align }
            } else {
                AbiLayout { size: 1, align: 1 }
            }
        }
        WitTypeTag::Result => {
            // discriminant (1 byte) + max(ok_size, err_size)
            let ok_abi = ty.inner.as_ref().map(|t| wit_type_abi(t, file)).unwrap_or(AbiLayout { size: 0, align: 1 });
            let err_abi = ty.inner2.as_ref().map(|t| wit_type_abi(t, file)).unwrap_or(AbiLayout { size: 0, align: 1 });
            let align = ok_abi.align.max(err_abi.align).max(1);
            let payload_size = ok_abi.size.max(err_abi.size);
            let payload_offset = align_to(1, align);
            let size = align_to(payload_offset + payload_size, align);
            AbiLayout { size, align }
        }
        WitTypeTag::Tuple => {
            // Like a struct with unnamed fields
            let mut offset = 0u32;
            let mut max_align = 1u32;
            for elem in &ty.tuple_types {
                let elem_abi = wit_type_abi(elem, file);
                max_align = max_align.max(elem_abi.align);
                offset = align_to(offset, elem_abi.align);
                offset += elem_abi.size;
            }
            AbiLayout { size: align_to(offset, max_align), align: max_align }
        }
        WitTypeTag::Variant => AbiLayout { size: 4, align: 4 },
        WitTypeTag::Enum => AbiLayout { size: 4, align: 4 }, // u32 discriminant
        WitTypeTag::Flags => AbiLayout { size: 4, align: 4 }, // u32 bitfield
        WitTypeTag::Own | WitTypeTag::Borrow => AbiLayout { size: 4, align: 4 }, // handle = i32
    }
}

fn wit_record_abi(r: &WitRecordDef, file: &ParsedWitFile) -> AbiLayout {
    let mut offset = 0u32;
    let mut max_align = 1u32;
    for field in &r.fields {
        let field_abi = wit_type_abi(&field.ty, file);
        max_align = max_align.max(field_abi.align);
        offset = align_to(offset, field_abi.align);
        offset += field_abi.size;
    }
    AbiLayout {
        size: align_to(offset, max_align),
        align: max_align,
    }
}

fn align_to(offset: u32, align: u32) -> u32 {
    if align == 0 { return offset; }
    (offset + align - 1) & !(align - 1)
}

// =========================================================================
// Convert to DynamicSchema
// =========================================================================

fn wit_type_to_dynamic(tag: WitTypeTag) -> DynamicType {
    match tag {
        WitTypeTag::Bool => DynamicType::Bool,
        WitTypeTag::U8 => DynamicType::U8,
        WitTypeTag::U16 => DynamicType::U16,
        WitTypeTag::U32 => DynamicType::U32,
        WitTypeTag::U64 => DynamicType::U64,
        WitTypeTag::S8 => DynamicType::I8,
        WitTypeTag::S16 => DynamicType::I16,
        WitTypeTag::S32 => DynamicType::I32,
        WitTypeTag::S64 => DynamicType::I64,
        WitTypeTag::F32 => DynamicType::F32,
        WitTypeTag::F64 => DynamicType::F64,
        WitTypeTag::Char => DynamicType::U32,
        WitTypeTag::String => DynamicType::Text,
        WitTypeTag::List => DynamicType::Vector,
        WitTypeTag::Option => DynamicType::Variant,
        WitTypeTag::Result => DynamicType::Variant,
        WitTypeTag::Tuple => DynamicType::Struct,
        WitTypeTag::Record | WitTypeTag::Named => DynamicType::Struct,
        WitTypeTag::Variant => DynamicType::Variant,
        WitTypeTag::Enum => DynamicType::U32,
        WitTypeTag::Flags => DynamicType::U32,
        WitTypeTag::Own | WitTypeTag::Borrow => DynamicType::U32,
    }
}

fn wit_record_to_dynamic(r: &WitRecordDef, file: &ParsedWitFile) -> DynamicSchema {
    let mut fields = Vec::new();
    let mut offset = 0u32;
    let mut max_align = 1u32;
    let mut total_ptrs = 0u16;

    for field in &r.fields {
        let field_abi = wit_type_abi(&field.ty, file);
        max_align = max_align.max(field_abi.align);
        offset = align_to(offset, field_abi.align);

        let ty = wit_type_to_dynamic(field.ty.tag);
        let is_ptr = matches!(
            field.ty.tag,
            WitTypeTag::String | WitTypeTag::List
        );

        let nested = if field.ty.tag == WitTypeTag::Record || field.ty.tag == WitTypeTag::Named {
            if let Some(rec) = file.find_record(&field.ty.name) {
                Some(Box::new(wit_record_to_dynamic(rec, file)))
            } else {
                None
            }
        } else {
            None
        };

        let mut fd = FieldDesc {
            name_hash: crate::xxh64::hash_str(&field.name),
            name: field.name.clone(),
            ty,
            is_ptr,
            offset,
            bit_index: 0,
            byte_size: ty.byte_size(),
            nested,
            alternatives: Vec::new(),
            disc_offset: 0,
        };

        // For variant types, create alternatives
        if field.ty.tag == WitTypeTag::Variant || field.ty.tag == WitTypeTag::Named {
            if let Some(v) = file.find_variant(&field.ty.name) {
                let mut alts = Vec::new();
                for case in &v.cases {
                    let alt_ty = if let Some(ref ty_ref) = case.ty {
                        wit_type_to_dynamic(ty_ref.tag)
                    } else {
                        DynamicType::Void
                    };
                    alts.push(AltDesc {
                        ty: alt_ty,
                        is_ptr: false,
                        offset: 0,
                        bit_index: 0,
                        byte_size: alt_ty.byte_size(),
                        nested: None,
                    });
                }
                if !alts.is_empty() {
                    fd.ty = DynamicType::Variant;
                    fd.alternatives = alts;
                    fd.disc_offset = offset;
                }
            }
        }

        if is_ptr {
            total_ptrs += 1;
        }

        offset += field_abi.size;
        fields.push(fd);
    }

    let data_words = ((align_to(offset, max_align) + 7) / 8) as u16;
    DynamicSchema::new(fields, data_words, total_ptrs)
}

// =========================================================================
// Lexer
// =========================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Tok {
    Eof,
    Ident,
    Number,
    StringLit,
    LBrace,
    RBrace,
    LParen,
    RParen,
    LAngle,
    RAngle,
    Colon,
    Semicolon,
    Comma,
    Equal,
    Dot,
    Arrow, // ->
    Star,  // *
    Slash,
    At,
    // keywords
    KwRecord,
    KwVariant,
    KwEnum,
    KwFlags,
    KwType,
    KwFunc,
    KwResource,
    KwInterface,
    KwWorld,
    KwUse,
    KwPackage,
    KwImport,
    KwExport,
    KwInclude,
    KwConstructor,
    KwStatic,
    // built-in types
    KwBool,
    KwU8,
    KwU16,
    KwU32,
    KwU64,
    KwS8,
    KwS16,
    KwS32,
    KwS64,
    KwF32,
    KwF64,
    KwChar,
    KwString,
    KwList,
    KwOption,
    KwResult,
    KwTuple,
    KwOwn,
    KwBorrow,
}

#[derive(Debug, Clone)]
struct Token {
    kind: Tok,
    text: String,
    line: u32,
    col: u32,
}

struct Lexer {
    src: Vec<char>,
    pos: usize,
    line: u32,
    col: u32,
}

impl Lexer {
    fn new(input: &str) -> Self {
        Self {
            src: input.chars().collect(),
            pos: 0,
            line: 1,
            col: 1,
        }
    }

    fn advance(&mut self, n: usize) {
        for _ in 0..n {
            if self.pos >= self.src.len() {
                break;
            }
            if self.src[self.pos] == '\n' {
                self.line += 1;
                self.col = 1;
            } else {
                self.col += 1;
            }
            self.pos += 1;
        }
    }

    fn skip_ws_and_comments(&mut self) {
        while self.pos < self.src.len() {
            let c = self.src[self.pos];
            if c == ' ' || c == '\t' || c == '\r' || c == '\n' {
                self.advance(1);
                continue;
            }
            // Line comments: //
            if c == '/' && self.pos + 1 < self.src.len() && self.src[self.pos + 1] == '/' {
                while self.pos < self.src.len() && self.src[self.pos] != '\n' {
                    self.advance(1);
                }
                continue;
            }
            // Block comments: /* ... */
            if c == '/' && self.pos + 1 < self.src.len() && self.src[self.pos + 1] == '*' {
                self.advance(2);
                while self.pos + 1 < self.src.len()
                    && !(self.src[self.pos] == '*' && self.src[self.pos + 1] == '/')
                {
                    self.advance(1);
                }
                if self.pos + 1 < self.src.len() {
                    self.advance(2);
                }
                continue;
            }
            break;
        }
    }

    fn next(&mut self) -> Token {
        self.skip_ws_and_comments();
        if self.pos >= self.src.len() {
            return Token { kind: Tok::Eof, text: String::new(), line: self.line, col: self.col };
        }

        let start_line = self.line;
        let start_col = self.col;
        let c = self.src[self.pos];

        let single = |s: &mut Self, k: Tok| -> Token {
            let ch = s.src[s.pos].to_string();
            s.advance(1);
            Token { kind: k, text: ch, line: start_line, col: start_col }
        };

        // Arrow: ->
        if c == '-' && self.pos + 1 < self.src.len() && self.src[self.pos + 1] == '>' {
            self.advance(2);
            return Token { kind: Tok::Arrow, text: "->".to_string(), line: start_line, col: start_col };
        }

        match c {
            '{' => return single(self, Tok::LBrace),
            '}' => return single(self, Tok::RBrace),
            '(' => return single(self, Tok::LParen),
            ')' => return single(self, Tok::RParen),
            '<' => return single(self, Tok::LAngle),
            '>' => return single(self, Tok::RAngle),
            ':' => return single(self, Tok::Colon),
            ';' => return single(self, Tok::Semicolon),
            ',' => return single(self, Tok::Comma),
            '=' => return single(self, Tok::Equal),
            '.' => return single(self, Tok::Dot),
            '*' => return single(self, Tok::Star),
            '/' => return single(self, Tok::Slash),
            '@' => return single(self, Tok::At),
            _ => {}
        }

        // String literal
        if c == '"' {
            self.advance(1);
            let start = self.pos;
            while self.pos < self.src.len() && self.src[self.pos] != '"' {
                if self.src[self.pos] == '\\' && self.pos + 1 < self.src.len() {
                    self.advance(1);
                }
                self.advance(1);
            }
            let text: String = self.src[start..self.pos].iter().collect();
            if self.pos < self.src.len() {
                self.advance(1);
            }
            return Token { kind: Tok::StringLit, text, line: start_line, col: start_col };
        }

        // Numbers
        if c.is_ascii_digit() || (c == '-' && self.pos + 1 < self.src.len() && self.src[self.pos + 1].is_ascii_digit()) {
            let start = self.pos;
            if c == '-' {
                self.advance(1);
            }
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
                self.advance(1);
            }
            let text: String = self.src[start..self.pos].iter().collect();
            return Token { kind: Tok::Number, text, line: start_line, col: start_col };
        }

        // Identifiers, keywords, and kebab-case identifiers
        if is_ident_start(c) {
            let start = self.pos;
            self.advance(1);
            // WIT allows kebab-case identifiers (e.g., `my-type`)
            while self.pos < self.src.len() && is_wit_ident_cont(self.src[self.pos]) {
                self.advance(1);
            }
            let text: String = self.src[start..self.pos].iter().collect();
            let kind = classify_wit_keyword(&text);
            return Token { kind, text, line: start_line, col: start_col };
        }

        // Percent-prefixed identifiers (WIT uses %name for reserved word escaping)
        if c == '%' {
            self.advance(1);
            let start = self.pos;
            while self.pos < self.src.len() && is_wit_ident_cont(self.src[self.pos]) {
                self.advance(1);
            }
            let text: String = self.src[start..self.pos].iter().collect();
            return Token { kind: Tok::Ident, text, line: start_line, col: start_col };
        }

        Token { kind: Tok::Eof, text: c.to_string(), line: start_line, col: start_col }
    }

    fn peek(&mut self) -> Token {
        let saved_pos = self.pos;
        let saved_line = self.line;
        let saved_col = self.col;
        let t = self.next();
        self.pos = saved_pos;
        self.line = saved_line;
        self.col = saved_col;
        t
    }
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_wit_ident_cont(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_' || c == '-'
}

fn classify_wit_keyword(text: &str) -> Tok {
    match text {
        "record" => Tok::KwRecord,
        "variant" => Tok::KwVariant,
        "enum" => Tok::KwEnum,
        "flags" => Tok::KwFlags,
        "type" => Tok::KwType,
        "func" => Tok::KwFunc,
        "resource" => Tok::KwResource,
        "interface" => Tok::KwInterface,
        "world" => Tok::KwWorld,
        "use" => Tok::KwUse,
        "package" => Tok::KwPackage,
        "import" => Tok::KwImport,
        "export" => Tok::KwExport,
        "include" => Tok::KwInclude,
        "constructor" => Tok::KwConstructor,
        "static" => Tok::KwStatic,
        // Built-in types
        "bool" => Tok::KwBool,
        "u8" => Tok::KwU8,
        "u16" => Tok::KwU16,
        "u32" => Tok::KwU32,
        "u64" => Tok::KwU64,
        "s8" => Tok::KwS8,
        "s16" => Tok::KwS16,
        "s32" => Tok::KwS32,
        "s64" => Tok::KwS64,
        "f32" | "float32" => Tok::KwF32,
        "f64" | "float64" => Tok::KwF64,
        "char" => Tok::KwChar,
        "string" => Tok::KwString,
        "list" => Tok::KwList,
        "option" => Tok::KwOption,
        "result" => Tok::KwResult,
        "tuple" => Tok::KwTuple,
        "own" => Tok::KwOwn,
        "borrow" => Tok::KwBorrow,
        _ => Tok::Ident,
    }
}

// =========================================================================
// Parser
// =========================================================================

struct Parser {
    lex: Lexer,
}

impl Parser {
    fn new(input: &str) -> Self {
        Self { lex: Lexer::new(input) }
    }

    fn peek_is(&mut self, k: Tok) -> bool {
        self.lex.peek().kind == k
    }

    fn expect(&mut self, k: Tok) -> Result<Token, ParseError> {
        let t = self.lex.next();
        if t.kind != k {
            return Err(ParseError {
                message: format!("expected {:?}, got {:?} '{}'", k, t.kind, t.text),
                line: t.line,
                column: t.col,
            });
        }
        Ok(t)
    }

    #[allow(dead_code)]
    fn expect_ident(&mut self) -> Result<Token, ParseError> {
        let t = self.lex.next();
        if t.kind != Tok::Ident {
            return Err(ParseError {
                message: format!("expected identifier, got {:?} '{}'", t.kind, t.text),
                line: t.line,
                column: t.col,
            });
        }
        Ok(t)
    }

    fn expect_ident_or_keyword(&mut self) -> Result<Token, ParseError> {
        let t = self.lex.next();
        // In WIT, keywords can sometimes appear in identifier positions
        // (e.g., field names like "type" or "use")
        if t.kind == Tok::Ident || is_keyword_tok(t.kind) {
            Ok(t)
        } else {
            Err(ParseError {
                message: format!("expected identifier, got {:?} '{}'", t.kind, t.text),
                line: t.line,
                column: t.col,
            })
        }
    }

    fn error<T>(&self, t: &Token, msg: &str) -> Result<T, ParseError> {
        Err(ParseError {
            message: msg.to_string(),
            line: t.line,
            column: t.col,
        })
    }

    fn parse(&mut self) -> Result<ParsedWitFile, ParseError> {
        let mut file = ParsedWitFile {
            package: String::new(),
            records: Vec::new(),
            variants: Vec::new(),
            enums: Vec::new(),
            flags: Vec::new(),
            type_defs: Vec::new(),
            functions: Vec::new(),
            resources: Vec::new(),
            interfaces: Vec::new(),
            type_map: HashMap::new(),
        };

        while !self.peek_is(Tok::Eof) {
            let t = self.lex.peek();
            match t.kind {
                Tok::KwPackage => self.parse_package(&mut file)?,
                Tok::KwRecord => {
                    let r = self.parse_record()?;
                    file.type_map.insert(r.name.clone(), WitTypeRef { tag: WitTypeTag::Record, name: r.name.clone(), ..Default::default() });
                    file.records.push(r);
                }
                Tok::KwVariant => {
                    let v = self.parse_variant()?;
                    file.type_map.insert(v.name.clone(), WitTypeRef { tag: WitTypeTag::Variant, name: v.name.clone(), ..Default::default() });
                    file.variants.push(v);
                }
                Tok::KwEnum => {
                    let e = self.parse_enum()?;
                    file.type_map.insert(e.name.clone(), WitTypeRef { tag: WitTypeTag::Enum, name: e.name.clone(), ..Default::default() });
                    file.enums.push(e);
                }
                Tok::KwFlags => {
                    let f = self.parse_flags()?;
                    file.type_map.insert(f.name.clone(), WitTypeRef { tag: WitTypeTag::Flags, name: f.name.clone(), ..Default::default() });
                    file.flags.push(f);
                }
                Tok::KwType => {
                    let td = self.parse_type_def()?;
                    file.type_map.insert(td.name.clone(), td.ty.clone());
                    file.type_defs.push(td);
                }
                Tok::KwResource => {
                    let r = self.parse_resource()?;
                    file.resources.push(r);
                }
                Tok::KwInterface => {
                    let iface = self.parse_interface()?;
                    file.interfaces.push(iface);
                }
                Tok::KwWorld => {
                    self.skip_world()?;
                }
                Tok::KwUse => {
                    self.skip_use()?;
                }
                Tok::Ident => {
                    // Could be a function: name: func(...)
                    let func = self.parse_func_item()?;
                    file.functions.push(func);
                }
                _ => {
                    // Skip unknown tokens
                    self.lex.next();
                }
            }
        }

        Ok(file)
    }

    fn parse_package(&mut self, file: &mut ParsedWitFile) -> Result<(), ParseError> {
        self.expect(Tok::KwPackage)?;
        let mut pkg = String::new();
        // Package: namespace:name@version or namespace:name/path
        loop {
            let t = self.lex.next();
            if t.kind == Tok::Semicolon || t.kind == Tok::Eof {
                break;
            }
            pkg.push_str(&t.text);
        }
        file.package = pkg;
        Ok(())
    }

    fn parse_record(&mut self) -> Result<WitRecordDef, ParseError> {
        self.expect(Tok::KwRecord)?;
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::LBrace)?;

        let mut fields = Vec::new();
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let field_name = self.expect_ident_or_keyword()?;
            self.expect(Tok::Colon)?;
            let ty = self.parse_type_ref()?;
            fields.push(WitRecordField {
                name: field_name.text,
                ty,
            });
            // Optional comma or semicolon
            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RBrace)?;

        Ok(WitRecordDef { name: name.text, fields })
    }

    fn parse_variant(&mut self) -> Result<WitVariantDef, ParseError> {
        self.expect(Tok::KwVariant)?;
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::LBrace)?;

        let mut cases = Vec::new();
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let case_name = self.expect_ident_or_keyword()?;
            let ty = if self.peek_is(Tok::LParen) {
                self.lex.next();
                let ty = self.parse_type_ref()?;
                self.expect(Tok::RParen)?;
                Some(ty)
            } else {
                None
            };
            cases.push(WitVariantCase {
                name: case_name.text,
                ty,
            });
            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RBrace)?;

        Ok(WitVariantDef { name: name.text, cases })
    }

    fn parse_enum(&mut self) -> Result<WitEnumDef, ParseError> {
        self.expect(Tok::KwEnum)?;
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::LBrace)?;

        let mut cases = Vec::new();
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let case_name = self.expect_ident_or_keyword()?;
            cases.push(case_name.text);
            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RBrace)?;

        Ok(WitEnumDef { name: name.text, cases })
    }

    fn parse_flags(&mut self) -> Result<WitFlagsDef, ParseError> {
        self.expect(Tok::KwFlags)?;
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::LBrace)?;

        let mut flags = Vec::new();
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let flag_name = self.expect_ident_or_keyword()?;
            flags.push(flag_name.text);
            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RBrace)?;

        Ok(WitFlagsDef { name: name.text, flags })
    }

    fn parse_type_def(&mut self) -> Result<WitTypeDef, ParseError> {
        self.expect(Tok::KwType)?;
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::Equal)?;
        let ty = self.parse_type_ref()?;
        // Optional semicolon
        if self.peek_is(Tok::Semicolon) {
            self.lex.next();
        }
        Ok(WitTypeDef { name: name.text, ty })
    }

    fn parse_resource(&mut self) -> Result<WitResourceDef, ParseError> {
        self.expect(Tok::KwResource)?;
        let name = self.expect_ident_or_keyword()?;

        let mut methods = Vec::new();

        if self.peek_is(Tok::LBrace) {
            self.lex.next();
            while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
                let t = self.lex.peek();
                if t.kind == Tok::KwConstructor {
                    self.lex.next();
                    // constructor(params)
                    let params = self.parse_param_list()?;
                    if self.peek_is(Tok::Semicolon) {
                        self.lex.next();
                    }
                    methods.push(WitFuncDef {
                        name: "constructor".to_string(),
                        params,
                        results: Vec::new(),
                        named_results: Vec::new(),
                    });
                } else if t.kind == Tok::Ident || is_keyword_tok(t.kind) {
                    let func = self.parse_func_item()?;
                    methods.push(func);
                } else if t.kind == Tok::KwStatic {
                    self.lex.next();
                    let func = self.parse_func_item()?;
                    methods.push(func);
                } else {
                    self.lex.next(); // skip
                }
            }
            self.expect(Tok::RBrace)?;
        } else if self.peek_is(Tok::Semicolon) {
            self.lex.next();
        }

        Ok(WitResourceDef { name: name.text, methods })
    }

    fn parse_interface(&mut self) -> Result<WitInterfaceDef, ParseError> {
        self.expect(Tok::KwInterface)?;
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::LBrace)?;

        let mut iface = WitInterfaceDef {
            name: name.text,
            records: Vec::new(),
            variants: Vec::new(),
            enums: Vec::new(),
            flags: Vec::new(),
            type_defs: Vec::new(),
            functions: Vec::new(),
            resources: Vec::new(),
        };

        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let t = self.lex.peek();
            match t.kind {
                Tok::KwRecord => {
                    iface.records.push(self.parse_record()?);
                }
                Tok::KwVariant => {
                    iface.variants.push(self.parse_variant()?);
                }
                Tok::KwEnum => {
                    iface.enums.push(self.parse_enum()?);
                }
                Tok::KwFlags => {
                    iface.flags.push(self.parse_flags()?);
                }
                Tok::KwType => {
                    iface.type_defs.push(self.parse_type_def()?);
                }
                Tok::KwResource => {
                    iface.resources.push(self.parse_resource()?);
                }
                Tok::KwUse => {
                    self.skip_use()?;
                }
                Tok::Ident => {
                    iface.functions.push(self.parse_func_item()?);
                }
                _ => {
                    self.lex.next(); // skip
                }
            }
        }
        self.expect(Tok::RBrace)?;

        Ok(iface)
    }

    fn parse_func_item(&mut self) -> Result<WitFuncDef, ParseError> {
        let name = self.expect_ident_or_keyword()?;
        self.expect(Tok::Colon)?;

        // May be prefixed with "func" keyword
        if self.peek_is(Tok::KwFunc) {
            self.lex.next();
        }

        let params = self.parse_param_list()?;

        let mut results = Vec::new();
        let mut named_results = Vec::new();

        if self.peek_is(Tok::Arrow) {
            self.lex.next();
            // Results can be a single type, or (name: type, ...)
            if self.peek_is(Tok::LParen) {
                // Named results
                self.lex.next();
                while !self.peek_is(Tok::RParen) && !self.peek_is(Tok::Eof) {
                    let rn = self.expect_ident_or_keyword()?;
                    self.expect(Tok::Colon)?;
                    let rt = self.parse_type_ref()?;
                    named_results.push(WitParam { name: rn.text, ty: rt });
                    if self.peek_is(Tok::Comma) {
                        self.lex.next();
                    }
                }
                self.expect(Tok::RParen)?;
            } else {
                // Single result type
                let ty = self.parse_type_ref()?;
                results.push(ty);
            }
        }

        // Optional semicolon
        if self.peek_is(Tok::Semicolon) {
            self.lex.next();
        }

        Ok(WitFuncDef { name: name.text, params, results, named_results })
    }

    fn parse_param_list(&mut self) -> Result<Vec<WitParam>, ParseError> {
        self.expect(Tok::LParen)?;
        let mut params = Vec::new();
        while !self.peek_is(Tok::RParen) && !self.peek_is(Tok::Eof) {
            let pname = self.expect_ident_or_keyword()?;
            self.expect(Tok::Colon)?;
            let pty = self.parse_type_ref()?;
            params.push(WitParam { name: pname.text, ty: pty });
            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RParen)?;
        Ok(params)
    }

    fn parse_type_ref(&mut self) -> Result<WitTypeRef, ParseError> {
        let t = self.lex.peek();

        match t.kind {
            Tok::KwBool => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::Bool)) }
            Tok::KwU8 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::U8)) }
            Tok::KwU16 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::U16)) }
            Tok::KwU32 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::U32)) }
            Tok::KwU64 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::U64)) }
            Tok::KwS8 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::S8)) }
            Tok::KwS16 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::S16)) }
            Tok::KwS32 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::S32)) }
            Tok::KwS64 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::S64)) }
            Tok::KwF32 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::F32)) }
            Tok::KwF64 => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::F64)) }
            Tok::KwChar => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::Char)) }
            Tok::KwString => { self.lex.next(); Ok(WitTypeRef::simple(WitTypeTag::String)) }

            Tok::KwList => {
                self.lex.next();
                self.expect(Tok::LAngle)?;
                let inner = self.parse_type_ref()?;
                self.expect(Tok::RAngle)?;
                Ok(WitTypeRef {
                    tag: WitTypeTag::List,
                    inner: Some(Box::new(inner)),
                    ..Default::default()
                })
            }
            Tok::KwOption => {
                self.lex.next();
                self.expect(Tok::LAngle)?;
                let inner = self.parse_type_ref()?;
                self.expect(Tok::RAngle)?;
                Ok(WitTypeRef {
                    tag: WitTypeTag::Option,
                    inner: Some(Box::new(inner)),
                    ..Default::default()
                })
            }
            Tok::KwResult => {
                self.lex.next();
                // result or result<ok, err> or result<_, err> or result<ok>
                if self.peek_is(Tok::LAngle) {
                    self.lex.next();
                    let ok_ty = if self.peek_is(Tok::Ident) && self.lex.peek().text == "_" {
                        self.lex.next();
                        None
                    } else if self.peek_is(Tok::RAngle) {
                        None
                    } else {
                        Some(Box::new(self.parse_type_ref()?))
                    };
                    let err_ty = if self.peek_is(Tok::Comma) {
                        self.lex.next();
                        if self.peek_is(Tok::Ident) && self.lex.peek().text == "_" {
                            self.lex.next();
                            None
                        } else {
                            Some(Box::new(self.parse_type_ref()?))
                        }
                    } else {
                        None
                    };
                    self.expect(Tok::RAngle)?;
                    Ok(WitTypeRef {
                        tag: WitTypeTag::Result,
                        inner: ok_ty,
                        inner2: err_ty,
                        ..Default::default()
                    })
                } else {
                    Ok(WitTypeRef {
                        tag: WitTypeTag::Result,
                        ..Default::default()
                    })
                }
            }
            Tok::KwTuple => {
                self.lex.next();
                self.expect(Tok::LAngle)?;
                let mut types = Vec::new();
                while !self.peek_is(Tok::RAngle) && !self.peek_is(Tok::Eof) {
                    types.push(self.parse_type_ref()?);
                    if self.peek_is(Tok::Comma) {
                        self.lex.next();
                    }
                }
                self.expect(Tok::RAngle)?;
                Ok(WitTypeRef {
                    tag: WitTypeTag::Tuple,
                    tuple_types: types,
                    ..Default::default()
                })
            }
            Tok::KwOwn => {
                self.lex.next();
                self.expect(Tok::LAngle)?;
                let inner = self.parse_type_ref()?;
                self.expect(Tok::RAngle)?;
                Ok(WitTypeRef {
                    tag: WitTypeTag::Own,
                    inner: Some(Box::new(inner)),
                    ..Default::default()
                })
            }
            Tok::KwBorrow => {
                self.lex.next();
                self.expect(Tok::LAngle)?;
                let inner = self.parse_type_ref()?;
                self.expect(Tok::RAngle)?;
                Ok(WitTypeRef {
                    tag: WitTypeTag::Borrow,
                    inner: Some(Box::new(inner)),
                    ..Default::default()
                })
            }

            Tok::Ident => {
                let name_tok = self.lex.next();
                Ok(WitTypeRef::named(&name_tok.text))
            }

            _ => self.error(&t, "expected type"),
        }
    }

    fn skip_world(&mut self) -> Result<(), ParseError> {
        self.expect(Tok::KwWorld)?;
        self.expect_ident_or_keyword()?;
        self.expect(Tok::LBrace)?;
        let mut depth = 1;
        while depth > 0 && !self.peek_is(Tok::Eof) {
            let t = self.lex.next();
            if t.kind == Tok::LBrace {
                depth += 1;
            } else if t.kind == Tok::RBrace {
                depth -= 1;
            }
        }
        Ok(())
    }

    fn skip_use(&mut self) -> Result<(), ParseError> {
        self.lex.next(); // consume 'use'
        while !self.peek_is(Tok::Semicolon) && !self.peek_is(Tok::Eof) {
            self.lex.next();
        }
        if self.peek_is(Tok::Semicolon) {
            self.lex.next();
        }
        Ok(())
    }
}

fn is_keyword_tok(k: Tok) -> bool {
    matches!(
        k,
        Tok::KwRecord
            | Tok::KwVariant
            | Tok::KwEnum
            | Tok::KwFlags
            | Tok::KwType
            | Tok::KwFunc
            | Tok::KwResource
            | Tok::KwInterface
            | Tok::KwWorld
            | Tok::KwUse
            | Tok::KwPackage
            | Tok::KwImport
            | Tok::KwExport
            | Tok::KwInclude
            | Tok::KwConstructor
            | Tok::KwStatic
            | Tok::KwBool
            | Tok::KwU8
            | Tok::KwU16
            | Tok::KwU32
            | Tok::KwU64
            | Tok::KwS8
            | Tok::KwS16
            | Tok::KwS32
            | Tok::KwS64
            | Tok::KwF32
            | Tok::KwF64
            | Tok::KwChar
            | Tok::KwString
            | Tok::KwList
            | Tok::KwOption
            | Tok::KwResult
            | Tok::KwTuple
            | Tok::KwOwn
            | Tok::KwBorrow
    )
}

// =========================================================================
// Public API
// =========================================================================

/// Parse a `.wit` schema source string into a [`ParsedWitFile`].
pub fn parse_wit(input: &str) -> Result<ParsedWitFile, ParseError> {
    let mut parser = Parser::new(input);
    parser.parse()
}

// =========================================================================
// Tests
// =========================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_record() {
        let input = r#"
            record point {
                x: f64,
                y: f64,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.records.len(), 1);

        let point = &file.records[0];
        assert_eq!(point.name, "point");
        assert_eq!(point.fields.len(), 2);
        assert_eq!(point.fields[0].name, "x");
        assert_eq!(point.fields[0].ty.tag, WitTypeTag::F64);
        assert_eq!(point.fields[1].name, "y");
        assert_eq!(point.fields[1].ty.tag, WitTypeTag::F64);
    }

    #[test]
    fn test_record_dynamic_schema() {
        let input = r#"
            record point {
                x: f64,
                y: f64,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let schema = file.to_dynamic_schema("point").unwrap();
        assert_eq!(schema.field_count(), 2);

        let x = schema.find_by_name("x").unwrap();
        assert_eq!(x.ty, DynamicType::F64);

        let y = schema.find_by_name("y").unwrap();
        assert_eq!(y.ty, DynamicType::F64);
    }

    #[test]
    fn test_variant() {
        let input = r#"
            variant shape {
                circle(f64),
                rectangle(f64),
                none,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.variants.len(), 1);

        let shape = &file.variants[0];
        assert_eq!(shape.name, "shape");
        assert_eq!(shape.cases.len(), 3);
        assert_eq!(shape.cases[0].name, "circle");
        assert!(shape.cases[0].ty.is_some());
        assert_eq!(shape.cases[1].name, "rectangle");
        assert_eq!(shape.cases[2].name, "none");
        assert!(shape.cases[2].ty.is_none());
    }

    #[test]
    fn test_enum() {
        let input = r#"
            enum color {
                red,
                green,
                blue,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.enums.len(), 1);
        assert_eq!(file.enums[0].name, "color");
        assert_eq!(file.enums[0].cases, vec!["red", "green", "blue"]);
    }

    #[test]
    fn test_flags() {
        let input = r#"
            flags permissions {
                read,
                write,
                exec,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.flags.len(), 1);
        assert_eq!(file.flags[0].name, "permissions");
        assert_eq!(file.flags[0].flags, vec!["read", "write", "exec"]);
    }

    #[test]
    fn test_type_alias() {
        let input = r#"
            type my-id = u64;
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.type_defs.len(), 1);
        assert_eq!(file.type_defs[0].name, "my-id");
        assert_eq!(file.type_defs[0].ty.tag, WitTypeTag::U64);
    }

    #[test]
    fn test_list_type() {
        let input = r#"
            record user-profile {
                id: u64,
                name: string,
                tags: list<string>,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let profile = &file.records[0];
        assert_eq!(profile.fields.len(), 3);
        assert_eq!(profile.fields[2].name, "tags");
        assert_eq!(profile.fields[2].ty.tag, WitTypeTag::List);
        assert!(profile.fields[2].ty.inner.is_some());
        assert_eq!(profile.fields[2].ty.inner.as_ref().unwrap().tag, WitTypeTag::String);
    }

    #[test]
    fn test_option_type() {
        let input = r#"
            record config {
                timeout: option<u32>,
                name: string,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let cfg = &file.records[0];
        assert_eq!(cfg.fields[0].ty.tag, WitTypeTag::Option);
    }

    #[test]
    fn test_result_type() {
        let input = r#"
            record response {
                data: result<string, u32>,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let resp = &file.records[0];
        assert_eq!(resp.fields[0].ty.tag, WitTypeTag::Result);
        assert!(resp.fields[0].ty.inner.is_some());
        assert!(resp.fields[0].ty.inner2.is_some());
    }

    #[test]
    fn test_tuple_type() {
        let input = r#"
            record pair {
                value: tuple<u32, string>,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let pair = &file.records[0];
        assert_eq!(pair.fields[0].ty.tag, WitTypeTag::Tuple);
        assert_eq!(pair.fields[0].ty.tuple_types.len(), 2);
    }

    #[test]
    fn test_func() {
        let input = r#"
            interface api {
                add: func(a: u32, b: u32) -> u32;
                greet: func(name: string);
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.interfaces.len(), 1);
        let api = &file.interfaces[0];
        assert_eq!(api.name, "api");
        assert_eq!(api.functions.len(), 2);

        let add = &api.functions[0];
        assert_eq!(add.name, "add");
        assert_eq!(add.params.len(), 2);
        assert_eq!(add.results.len(), 1);
        assert_eq!(add.results[0].tag, WitTypeTag::U32);

        let greet = &api.functions[1];
        assert_eq!(greet.name, "greet");
        assert_eq!(greet.params.len(), 1);
        assert!(greet.results.is_empty());
    }

    #[test]
    fn test_comments() {
        let input = r#"
            // This is a comment
            record point {
                x: f64, // x coordinate
                /* block comment */
                y: f64,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.records.len(), 1);
        assert_eq!(file.records[0].fields.len(), 2);
    }

    #[test]
    fn test_interface() {
        let input = r#"
            interface http-handler {
                record request {
                    method: string,
                    path: string,
                    body: list<u8>,
                }
                record response {
                    status: u16,
                    body: list<u8>,
                }
                handle: func(req: request) -> response;
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.interfaces.len(), 1);
        let iface = &file.interfaces[0];
        assert_eq!(iface.name, "http-handler");
        assert_eq!(iface.records.len(), 2);
        assert_eq!(iface.functions.len(), 1);
    }

    #[test]
    fn test_resource() {
        let input = r#"
            interface streams {
                resource input-stream {
                    read: func(len: u64) -> list<u8>;
                }
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.interfaces.len(), 1);
        let iface = &file.interfaces[0];
        assert_eq!(iface.resources.len(), 1);
        assert_eq!(iface.resources[0].name, "input-stream");
        assert_eq!(iface.resources[0].methods.len(), 1);
    }

    #[test]
    fn test_complex_record_dynamic_schema() {
        let input = r#"
            record sensor-reading {
                timestamp: u64,
                device-id: string,
                temp: f64,
                humidity: f64,
                pressure: f64,
                accel-x: f64,
                accel-y: f64,
                accel-z: f64,
                gyro-x: f64,
                gyro-y: f64,
                gyro-z: f64,
                mag-x: f64,
                mag-y: f64,
                mag-z: f64,
                battery: f32,
                signal-dbm: s16,
                error-code: u32,
                firmware: string,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let schema = file.to_dynamic_schema("sensor-reading").unwrap();
        assert_eq!(schema.field_count(), 18);

        let ts = schema.find_by_name("timestamp").unwrap();
        assert_eq!(ts.ty, DynamicType::U64);

        let dev = schema.find_by_name("device-id").unwrap();
        assert_eq!(dev.ty, DynamicType::Text);

        let temp = schema.find_by_name("temp").unwrap();
        assert_eq!(temp.ty, DynamicType::F64);

        let bat = schema.find_by_name("battery").unwrap();
        assert_eq!(bat.ty, DynamicType::F32);

        let sig = schema.find_by_name("signal-dbm").unwrap();
        assert_eq!(sig.ty, DynamicType::I16);
    }

    #[test]
    fn test_nested_record() {
        let input = r#"
            record inner {
                x: s32,
                y: s32,
            }
            record outer {
                pos: inner,
                id: u64,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.records.len(), 2);

        let schema = file.to_dynamic_schema("outer").unwrap();
        assert_eq!(schema.field_count(), 2);

        let pos = schema.find_by_name("pos").unwrap();
        assert_eq!(pos.ty, DynamicType::Struct);
        assert!(pos.nested.is_some());
        let inner = pos.nested.as_ref().unwrap();
        assert_eq!(inner.field_count(), 2);
    }

    #[test]
    fn test_abi_layout() {
        // Verify canonical ABI layout computation
        let input = r#"
            record mixed {
                a: u8,
                b: u32,
                c: u8,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let schema = file.to_dynamic_schema("mixed").unwrap();

        // Canonical ABI: a at 0 (1 byte), padding to 4, b at 4 (4 bytes), c at 8 (1 byte)
        // Total padded to 4 = 12 bytes = 2 data words (ceiling of 12/8)
        let a = schema.find_by_name("a").unwrap();
        assert_eq!(a.offset, 0);

        let b = schema.find_by_name("b").unwrap();
        assert_eq!(b.offset, 4);

        let c = schema.find_by_name("c").unwrap();
        assert_eq!(c.offset, 8);
    }

    #[test]
    fn test_kebab_case_identifiers() {
        let input = r#"
            record my-record {
                my-field: u32,
                another-field: string,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.records[0].name, "my-record");
        assert_eq!(file.records[0].fields[0].name, "my-field");
    }

    #[test]
    fn test_package_declaration() {
        let input = r#"
            package wasi:http@0.2.0;
            record request {
                method: string,
            }
        "#;
        let file = parse_wit(input).unwrap();
        assert_eq!(file.package, "wasi:http@0.2.0");
        assert_eq!(file.records.len(), 1);
    }

    #[test]
    fn test_own_and_borrow() {
        let input = r#"
            record handle-holder {
                owned: own<u32>,
                borrowed: borrow<u32>,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let r = &file.records[0];
        assert_eq!(r.fields[0].ty.tag, WitTypeTag::Own);
        assert_eq!(r.fields[1].ty.tag, WitTypeTag::Borrow);
    }

    #[test]
    fn test_all_scalar_types() {
        let input = r#"
            record all-scalars {
                a: bool,
                b: u8,
                c: u16,
                d: u32,
                e: u64,
                f: s8,
                g: s16,
                h: s32,
                i: s64,
                j: f32,
                k: f64,
                l: char,
                m: string,
            }
        "#;
        let file = parse_wit(input).unwrap();
        let r = &file.records[0];
        assert_eq!(r.fields.len(), 13);
        assert_eq!(r.fields[0].ty.tag, WitTypeTag::Bool);
        assert_eq!(r.fields[1].ty.tag, WitTypeTag::U8);
        assert_eq!(r.fields[2].ty.tag, WitTypeTag::U16);
        assert_eq!(r.fields[3].ty.tag, WitTypeTag::U32);
        assert_eq!(r.fields[4].ty.tag, WitTypeTag::U64);
        assert_eq!(r.fields[5].ty.tag, WitTypeTag::S8);
        assert_eq!(r.fields[6].ty.tag, WitTypeTag::S16);
        assert_eq!(r.fields[7].ty.tag, WitTypeTag::S32);
        assert_eq!(r.fields[8].ty.tag, WitTypeTag::S64);
        assert_eq!(r.fields[9].ty.tag, WitTypeTag::F32);
        assert_eq!(r.fields[10].ty.tag, WitTypeTag::F64);
        assert_eq!(r.fields[11].ty.tag, WitTypeTag::Char);
        assert_eq!(r.fields[12].ty.tag, WitTypeTag::String);

        let schema = file.to_dynamic_schema("all-scalars").unwrap();
        assert_eq!(schema.field_count(), 13);
    }
}
