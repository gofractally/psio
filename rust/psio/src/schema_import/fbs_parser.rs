//! FlatBuffers `.fbs` IDL parser.
//!
//! Parses FlatBuffers schema text into a [`ParsedFbsFile`] with computed
//! wire layout (vtable slots, struct byte offsets).
//!
//! Port of the C++ `fbs_parser.hpp` to Rust.

use crate::dynamic_schema::{
    AltDesc, DynamicSchema, DynamicType, FieldDesc,
};

use std::collections::HashMap;
use std::fmt;

// =========================================================================
// Public types
// =========================================================================

/// Error returned by the FlatBuffers parser.
#[derive(Debug, Clone)]
pub struct ParseError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "fbs:{}:{}: {}", self.line, self.column, self.message)
    }
}

impl std::error::Error for ParseError {}

/// FlatBuffers base/scalar type classification.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FbsBaseType {
    None,
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
    Vector,
    Table,
    Struct,
    Enum,
    Union,
}

/// What kind of top-level definition.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FbsTypeKind {
    Table,
    Struct,
    Enum,
    Union,
}

/// Field metadata (from parenthesized attributes).
#[derive(Debug, Clone, Default)]
pub struct FbsFieldMetadata {
    pub deprecated: bool,
    pub id: i32,
    pub required: bool,
}

/// An enum value (name + integer value).
#[derive(Debug, Clone)]
pub struct FbsEnumVal {
    pub name: String,
    pub value: i64,
}

/// A union member.
#[derive(Debug, Clone)]
pub struct FbsUnionMember {
    pub name: String,
    pub type_idx: i32,
}

/// A field within a table or struct.
#[derive(Debug, Clone)]
pub struct FbsFieldDef {
    pub name: String,
    pub field_type: FbsBaseType,
    pub type_idx: i32,
    pub elem_type: FbsBaseType,
    pub elem_type_idx: i32,
    pub unresolved_type_name: String,
    pub unresolved_elem_name: String,
    pub metadata: FbsFieldMetadata,
    // Layout info
    pub vtable_slot: i32,
    pub struct_offset: u32,
    pub wire_size: u8,
    pub wire_align: u8,
    pub is_offset_type: bool,
    // Default values
    pub default_int: i64,
    pub default_float: f64,
    pub default_string: String,
    pub has_default: bool,
}

impl Default for FbsFieldDef {
    fn default() -> Self {
        Self {
            name: String::new(),
            field_type: FbsBaseType::None,
            type_idx: -1,
            elem_type: FbsBaseType::None,
            elem_type_idx: -1,
            unresolved_type_name: String::new(),
            unresolved_elem_name: String::new(),
            metadata: FbsFieldMetadata { deprecated: false, id: -1, required: false },
            vtable_slot: -1,
            struct_offset: 0,
            wire_size: 0,
            wire_align: 0,
            is_offset_type: false,
            default_int: 0,
            default_float: 0.0,
            default_string: String::new(),
            has_default: false,
        }
    }
}

/// A top-level type definition (table, struct, enum, or union).
#[derive(Debug, Clone)]
pub struct FbsTypeDef {
    pub name: String,
    pub full_name: String,
    pub kind: FbsTypeKind,
    // For tables and structs
    pub fields: Vec<FbsFieldDef>,
    // For enums
    pub underlying_type: FbsBaseType,
    pub enum_values: Vec<FbsEnumVal>,
    // For unions
    pub union_members: Vec<FbsUnionMember>,
    // Layout info
    pub vtable_slot_count: u16,
    pub struct_size: u32,
    pub struct_align: u32,
}

impl Default for FbsTypeDef {
    fn default() -> Self {
        Self {
            name: String::new(),
            full_name: String::new(),
            kind: FbsTypeKind::Table,
            fields: Vec::new(),
            underlying_type: FbsBaseType::Int32,
            enum_values: Vec::new(),
            union_members: Vec::new(),
            vtable_slot_count: 0,
            struct_size: 0,
            struct_align: 0,
        }
    }
}

/// Top-level schema container.
#[derive(Debug, Clone)]
pub struct ParsedFbsFile {
    pub ns: String,
    pub root_type: String,
    pub file_identifier: String,
    pub file_extension: String,
    pub types: Vec<FbsTypeDef>,
    pub includes: Vec<String>,
    pub attributes: Vec<String>,
    pub type_map: HashMap<String, u32>,
}

impl ParsedFbsFile {
    /// Find a type by name (tries both bare name and namespace-qualified).
    pub fn find_type(&self, name: &str) -> Option<&FbsTypeDef> {
        if let Some(&idx) = self.type_map.get(name) {
            return Some(&self.types[idx as usize]);
        }
        if !self.ns.is_empty() {
            let qualified = format!("{}.{}", self.ns, name);
            if let Some(&idx) = self.type_map.get(&qualified) {
                return Some(&self.types[idx as usize]);
            }
        }
        None
    }

    /// Convert a parsed table/struct to a `DynamicSchema`.
    pub fn to_dynamic_schema(&self, type_name: &str) -> Option<DynamicSchema> {
        let td = self.find_type(type_name)?;
        if td.kind != FbsTypeKind::Table && td.kind != FbsTypeKind::Struct {
            return None;
        }
        Some(fbs_type_to_dynamic(td, self))
    }
}

// =========================================================================
// Lexer
// =========================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Tok {
    Eof,
    Ident,
    Integer,
    FloatLit,
    StringLit,
    LBrace,
    RBrace,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Colon,
    Semicolon,
    Equal,
    Comma,
    Dot,
    // keywords
    KwTable,
    KwStruct,
    KwEnum,
    KwUnion,
    KwRootType,
    KwNamespace,
    KwAttribute,
    KwInclude,
    KwFileIdentifier,
    KwFileExtension,
    KwRpcService,
    // built-in types
    KwBool,
    KwInt8,
    KwUInt8,
    KwInt16,
    KwUInt16,
    KwInt32,
    KwUInt32,
    KwInt64,
    KwUInt64,
    KwFloat32,
    KwFloat64,
    KwString,
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
            if c == '/' && self.pos + 1 < self.src.len() {
                if self.src[self.pos + 1] == '/' {
                    while self.pos < self.src.len() && self.src[self.pos] != '\n' {
                        self.advance(1);
                    }
                    continue;
                }
                if self.src[self.pos + 1] == '*' {
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

        match c {
            '{' => return single(self, Tok::LBrace),
            '}' => return single(self, Tok::RBrace),
            '(' => return single(self, Tok::LParen),
            ')' => return single(self, Tok::RParen),
            '[' => return single(self, Tok::LBracket),
            ']' => return single(self, Tok::RBracket),
            ':' => return single(self, Tok::Colon),
            ';' => return single(self, Tok::Semicolon),
            '=' => return single(self, Tok::Equal),
            ',' => return single(self, Tok::Comma),
            '.' => return single(self, Tok::Dot),
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
        if c.is_ascii_digit() || c == '-' || c == '+' {
            return self.lex_number(start_line, start_col);
        }

        // Identifiers and keywords
        if is_ident_start(c) {
            let start = self.pos;
            self.advance(1);
            while self.pos < self.src.len() && is_ident_cont(self.src[self.pos]) {
                self.advance(1);
            }
            let text: String = self.src[start..self.pos].iter().collect();
            let kind = classify_keyword(&text);
            return Token { kind, text, line: start_line, col: start_col };
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

    fn lex_number(&mut self, start_line: u32, start_col: u32) -> Token {
        let start = self.pos;
        let mut is_float = false;

        if self.src[self.pos] == '-' || self.src[self.pos] == '+' {
            self.advance(1);
        }

        // Hex
        if self.pos + 1 < self.src.len()
            && self.src[self.pos] == '0'
            && (self.src[self.pos + 1] == 'x' || self.src[self.pos + 1] == 'X')
        {
            self.advance(2);
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_hexdigit() {
                self.advance(1);
            }
            let text: String = self.src[start..self.pos].iter().collect();
            return Token { kind: Tok::Integer, text, line: start_line, col: start_col };
        }

        while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
            self.advance(1);
        }

        if self.pos < self.src.len() && self.src[self.pos] == '.' {
            is_float = true;
            self.advance(1);
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
                self.advance(1);
            }
        }

        if self.pos < self.src.len()
            && (self.src[self.pos] == 'e' || self.src[self.pos] == 'E')
        {
            is_float = true;
            self.advance(1);
            if self.pos < self.src.len()
                && (self.src[self.pos] == '+' || self.src[self.pos] == '-')
            {
                self.advance(1);
            }
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
                self.advance(1);
            }
        }

        let text: String = self.src[start..self.pos].iter().collect();
        Token {
            kind: if is_float { Tok::FloatLit } else { Tok::Integer },
            text,
            line: start_line,
            col: start_col,
        }
    }
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident_cont(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

fn classify_keyword(text: &str) -> Tok {
    match text {
        "table" => Tok::KwTable,
        "struct" => Tok::KwStruct,
        "enum" => Tok::KwEnum,
        "union" => Tok::KwUnion,
        "root_type" => Tok::KwRootType,
        "namespace" => Tok::KwNamespace,
        "attribute" => Tok::KwAttribute,
        "include" => Tok::KwInclude,
        "file_identifier" => Tok::KwFileIdentifier,
        "file_extension" => Tok::KwFileExtension,
        "rpc_service" => Tok::KwRpcService,
        "bool" => Tok::KwBool,
        "byte" | "int8" => Tok::KwInt8,
        "ubyte" | "uint8" => Tok::KwUInt8,
        "short" | "int16" => Tok::KwInt16,
        "ushort" | "uint16" => Tok::KwUInt16,
        "int" | "int32" => Tok::KwInt32,
        "uint" | "uint32" => Tok::KwUInt32,
        "long" | "int64" => Tok::KwInt64,
        "ulong" | "uint64" => Tok::KwUInt64,
        "float" | "float32" => Tok::KwFloat32,
        "double" | "float64" => Tok::KwFloat64,
        "string" => Tok::KwString,
        _ => Tok::Ident,
    }
}

// =========================================================================
// Layout helpers
// =========================================================================

fn base_type_size(bt: FbsBaseType) -> u8 {
    match bt {
        FbsBaseType::Bool | FbsBaseType::Int8 | FbsBaseType::UInt8 => 1,
        FbsBaseType::Int16 | FbsBaseType::UInt16 => 2,
        FbsBaseType::Int32 | FbsBaseType::UInt32 | FbsBaseType::Float32 => 4,
        FbsBaseType::Int64 | FbsBaseType::UInt64 | FbsBaseType::Float64 => 8,
        _ => 4,
    }
}

fn fbs_to_dynamic_type(bt: FbsBaseType) -> DynamicType {
    match bt {
        FbsBaseType::Bool => DynamicType::Bool,
        FbsBaseType::Int8 => DynamicType::I8,
        FbsBaseType::UInt8 => DynamicType::U8,
        FbsBaseType::Int16 => DynamicType::I16,
        FbsBaseType::UInt16 => DynamicType::U16,
        FbsBaseType::Int32 => DynamicType::I32,
        FbsBaseType::UInt32 => DynamicType::U32,
        FbsBaseType::Int64 => DynamicType::I64,
        FbsBaseType::UInt64 => DynamicType::U64,
        FbsBaseType::Float32 => DynamicType::F32,
        FbsBaseType::Float64 => DynamicType::F64,
        FbsBaseType::String => DynamicType::Text,
        FbsBaseType::Vector => DynamicType::Vector,
        FbsBaseType::Table | FbsBaseType::Struct => DynamicType::Struct,
        FbsBaseType::Enum => DynamicType::I32,
        FbsBaseType::Union => DynamicType::Variant,
        FbsBaseType::None => DynamicType::Void,
    }
}

fn fbs_type_to_dynamic(td: &FbsTypeDef, file: &ParsedFbsFile) -> DynamicSchema {
    let mut fields = Vec::new();

    for sf in &td.fields {
        let mut ty = fbs_to_dynamic_type(sf.field_type);
        let mut byte_size = sf.wire_size;

        // For enum fields, use the underlying type
        if sf.field_type == FbsBaseType::Enum && sf.type_idx >= 0 {
            let idx = sf.type_idx as usize;
            if idx < file.types.len() {
                let enum_def = &file.types[idx];
                ty = fbs_to_dynamic_type(enum_def.underlying_type);
                byte_size = base_type_size(enum_def.underlying_type);
            }
        }

        let offset = if td.kind == FbsTypeKind::Table {
            (4 + 2 * sf.vtable_slot) as u32
        } else {
            sf.struct_offset
        };

        let is_ptr = if td.kind == FbsTypeKind::Table {
            sf.is_offset_type
        } else {
            false
        };

        let nested = if (sf.field_type == FbsBaseType::Table || sf.field_type == FbsBaseType::Struct)
            && sf.type_idx >= 0
        {
            let idx = sf.type_idx as usize;
            if idx < file.types.len() {
                Some(Box::new(fbs_type_to_dynamic(&file.types[idx], file)))
            } else {
                None
            }
        } else {
            None
        };

        let mut fd = FieldDesc {
            name_hash: crate::xxh64::hash_str(&sf.name),
            name: sf.name.clone(),
            ty,
            is_ptr,
            offset,
            bit_index: 0,
            byte_size,
            nested,
            alternatives: Vec::new(),
            disc_offset: 0,
        };

        // Union/variant
        if sf.field_type == FbsBaseType::Union && sf.type_idx >= 0 {
            let idx = sf.type_idx as usize;
            if idx < file.types.len() {
                let union_def = &file.types[idx];
                let mut alts = Vec::new();
                for um in &union_def.union_members {
                    let alt_ty = if um.type_idx >= 0 {
                        DynamicType::Struct
                    } else {
                        DynamicType::Void
                    };
                    let alt_nested = if um.type_idx >= 0 {
                        let ui = um.type_idx as usize;
                        if ui < file.types.len() {
                            Some(Box::new(fbs_type_to_dynamic(&file.types[ui], file)))
                        } else {
                            None
                        }
                    } else {
                        None
                    };
                    alts.push(AltDesc {
                        ty: alt_ty,
                        is_ptr: true,
                        offset: 0,
                        bit_index: 0,
                        byte_size: 0,
                        nested: alt_nested,
                    });
                }
                fd.alternatives = alts;
                fd.disc_offset = (4 + 2 * sf.vtable_slot) as u32;
                fd.offset = (4 + 2 * (sf.vtable_slot + 1)) as u32;
            }
        }

        // Vector element nested schema
        if sf.field_type == FbsBaseType::Vector
            && (sf.elem_type == FbsBaseType::Table || sf.elem_type == FbsBaseType::Struct)
            && sf.elem_type_idx >= 0
        {
            let idx = sf.elem_type_idx as usize;
            if idx < file.types.len() {
                fd.nested = Some(Box::new(fbs_type_to_dynamic(&file.types[idx], file)));
            }
        }

        fields.push(fd);
    }

    let data_words = 0u16; // FlatBuffers don't use capnp data_words
    let ptr_count = 0u16;
    DynamicSchema::new(fields, data_words, ptr_count)
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
                message: format!("expected {:?}, got '{}'", k, t.text),
                line: t.line,
                column: t.col,
            });
        }
        Ok(t)
    }

    fn expect_ident(&mut self) -> Result<Token, ParseError> {
        let t = self.lex.next();
        if t.kind != Tok::Ident {
            return Err(ParseError {
                message: format!("expected identifier, got '{}'", t.text),
                line: t.line,
                column: t.col,
            });
        }
        Ok(t)
    }

    fn error<T>(&self, t: &Token, msg: &str) -> Result<T, ParseError> {
        Err(ParseError {
            message: msg.to_string(),
            line: t.line,
            column: t.col,
        })
    }

    fn parse(&mut self) -> Result<ParsedFbsFile, ParseError> {
        let mut schema = ParsedFbsFile {
            ns: String::new(),
            root_type: String::new(),
            file_identifier: String::new(),
            file_extension: String::new(),
            types: Vec::new(),
            includes: Vec::new(),
            attributes: Vec::new(),
            type_map: HashMap::new(),
        };

        while !self.peek_is(Tok::Eof) {
            let t = self.lex.peek();
            match t.kind {
                Tok::KwNamespace => self.parse_namespace(&mut schema)?,
                Tok::KwTable => self.parse_table(&mut schema)?,
                Tok::KwStruct => self.parse_struct(&mut schema)?,
                Tok::KwEnum => self.parse_enum(&mut schema)?,
                Tok::KwUnion => self.parse_union(&mut schema)?,
                Tok::KwRootType => self.parse_root_type(&mut schema)?,
                Tok::KwFileIdentifier => self.parse_file_identifier(&mut schema)?,
                Tok::KwFileExtension => self.parse_file_extension(&mut schema)?,
                Tok::KwInclude => self.parse_include(&mut schema)?,
                Tok::KwAttribute => self.parse_attribute(&mut schema)?,
                Tok::KwRpcService => self.skip_rpc_service()?,
                _ => return self.error(&t, "expected top-level declaration"),
            }
        }

        self.resolve_types(&mut schema);
        self.compute_layouts(&mut schema);

        Ok(schema)
    }

    fn parse_namespace(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwNamespace)?;
        let name = self.expect_ident()?;
        let mut ns = name.text.clone();
        while self.peek_is(Tok::Dot) {
            self.lex.next();
            let part = self.expect_ident()?;
            ns.push('.');
            ns.push_str(&part.text);
        }
        self.expect(Tok::Semicolon)?;
        schema.ns = ns;
        Ok(())
    }

    fn parse_table(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwTable)?;
        let name = self.expect_ident()?;

        let mut td = FbsTypeDef {
            name: name.text.clone(),
            kind: FbsTypeKind::Table,
            ..Default::default()
        };
        td.full_name = if !schema.ns.is_empty() {
            format!("{}.{}", schema.ns, td.name)
        } else {
            td.name.clone()
        };

        if self.peek_is(Tok::LParen) {
            self.skip_metadata();
        }

        self.expect(Tok::LBrace)?;
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            self.parse_field(&mut td)?;
        }
        self.expect(Tok::RBrace)?;

        let idx = schema.types.len() as u32;
        schema.type_map.insert(td.name.clone(), idx);
        schema.type_map.insert(td.full_name.clone(), idx);
        schema.types.push(td);
        Ok(())
    }

    fn parse_struct(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwStruct)?;
        let name = self.expect_ident()?;

        let mut td = FbsTypeDef {
            name: name.text.clone(),
            kind: FbsTypeKind::Struct,
            ..Default::default()
        };
        td.full_name = if !schema.ns.is_empty() {
            format!("{}.{}", schema.ns, td.name)
        } else {
            td.name.clone()
        };

        if self.peek_is(Tok::LParen) {
            self.skip_metadata();
        }

        self.expect(Tok::LBrace)?;
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            self.parse_field(&mut td)?;
        }
        self.expect(Tok::RBrace)?;

        let idx = schema.types.len() as u32;
        schema.type_map.insert(td.name.clone(), idx);
        schema.type_map.insert(td.full_name.clone(), idx);
        schema.types.push(td);
        Ok(())
    }

    fn parse_enum(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwEnum)?;
        let name = self.expect_ident()?;

        let mut td = FbsTypeDef {
            name: name.text.clone(),
            kind: FbsTypeKind::Enum,
            ..Default::default()
        };
        td.full_name = if !schema.ns.is_empty() {
            format!("{}.{}", schema.ns, td.name)
        } else {
            td.name.clone()
        };

        self.expect(Tok::Colon)?;
        td.underlying_type = self.parse_scalar_base_type()?;

        if self.peek_is(Tok::LParen) {
            self.skip_metadata();
        }

        self.expect(Tok::LBrace)?;
        let mut next_value: i64 = 0;
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let val_name = self.expect_ident()?;
            let mut ev = FbsEnumVal {
                name: val_name.text.clone(),
                value: next_value,
            };

            if self.peek_is(Tok::Equal) {
                self.lex.next();
                ev.value = self.parse_integer_value()?;
                next_value = ev.value + 1;
            } else {
                next_value = ev.value + 1;
            }

            td.enum_values.push(ev);

            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RBrace)?;

        let idx = schema.types.len() as u32;
        schema.type_map.insert(td.name.clone(), idx);
        schema.type_map.insert(td.full_name.clone(), idx);
        schema.types.push(td);
        Ok(())
    }

    fn parse_union(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwUnion)?;
        let name = self.expect_ident()?;

        let mut td = FbsTypeDef {
            name: name.text.clone(),
            kind: FbsTypeKind::Union,
            ..Default::default()
        };
        td.full_name = if !schema.ns.is_empty() {
            format!("{}.{}", schema.ns, td.name)
        } else {
            td.name.clone()
        };

        if self.peek_is(Tok::LParen) {
            self.skip_metadata();
        }

        self.expect(Tok::LBrace)?;
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let first = self.expect_ident()?;
            let mut um = FbsUnionMember {
                name: first.text.clone(),
                type_idx: -1,
            };

            if self.peek_is(Tok::Colon) {
                self.lex.next();
                let type_name = self.expect_ident()?;
                um.name = type_name.text.clone();
            }

            td.union_members.push(um);

            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RBrace)?;

        let idx = schema.types.len() as u32;
        schema.type_map.insert(td.name.clone(), idx);
        schema.type_map.insert(td.full_name.clone(), idx);
        schema.types.push(td);
        Ok(())
    }

    fn parse_field(&mut self, td: &mut FbsTypeDef) -> Result<(), ParseError> {
        let field_name_tok = self.expect_ident()?;

        let mut field = FbsFieldDef {
            name: field_name_tok.text.clone(),
            ..Default::default()
        };

        self.expect(Tok::Colon)?;
        self.parse_field_type(&mut field)?;

        if self.peek_is(Tok::Equal) {
            self.lex.next();
            self.parse_default_value(&mut field);
        }

        if self.peek_is(Tok::LParen) {
            self.parse_field_metadata(&mut field)?;
        }

        self.expect(Tok::Semicolon)?;

        td.fields.push(field);
        Ok(())
    }

    fn parse_field_type(&mut self, field: &mut FbsFieldDef) -> Result<(), ParseError> {
        let t = self.lex.peek();

        // Vector type: [element_type]
        if t.kind == Tok::LBracket {
            self.lex.next();
            field.field_type = FbsBaseType::Vector;
            let elem_tok = self.lex.peek();
            if is_scalar_type_tok(elem_tok.kind) {
                self.lex.next();
                field.elem_type = scalar_tok_to_base_type(elem_tok.kind);
            } else if elem_tok.kind == Tok::KwString {
                self.lex.next();
                field.elem_type = FbsBaseType::String;
            } else if elem_tok.kind == Tok::Ident {
                self.lex.next();
                field.elem_type = FbsBaseType::Table;
                field.elem_type_idx = -1;
                field.unresolved_elem_name = elem_tok.text.clone();
            } else {
                return self.error(&elem_tok, "expected type in vector declaration");
            }
            self.expect(Tok::RBracket)?;
            return Ok(());
        }

        // Scalar types
        if is_scalar_type_tok(t.kind) {
            self.lex.next();
            field.field_type = scalar_tok_to_base_type(t.kind);
            return Ok(());
        }

        // String
        if t.kind == Tok::KwString {
            self.lex.next();
            field.field_type = FbsBaseType::String;
            return Ok(());
        }

        // Type reference
        if t.kind == Tok::Ident {
            self.lex.next();
            let mut type_name = t.text.clone();
            while self.peek_is(Tok::Dot) {
                self.lex.next();
                let part = self.expect_ident()?;
                type_name.push('.');
                type_name.push_str(&part.text);
            }
            field.field_type = FbsBaseType::Table;
            field.type_idx = -1;
            field.unresolved_type_name = type_name;
            return Ok(());
        }

        self.error(&t, "expected field type")
    }

    fn parse_default_value(&mut self, field: &mut FbsFieldDef) {
        let t = self.lex.peek();

        match t.kind {
            Tok::Integer => {
                self.lex.next();
                field.default_int = Self::parse_integer_literal(&t.text);
                field.has_default = true;
            }
            Tok::FloatLit => {
                self.lex.next();
                field.default_float = t.text.parse::<f64>().unwrap_or(0.0);
                field.has_default = true;
            }
            Tok::Ident => {
                self.lex.next();
                match t.text.as_str() {
                    "true" => {
                        field.default_int = 1;
                        field.has_default = true;
                    }
                    "false" => {
                        field.default_int = 0;
                        field.has_default = true;
                    }
                    "nan" => {
                        field.default_float = f64::NAN;
                        field.has_default = true;
                    }
                    "inf" | "infinity" => {
                        field.default_float = f64::INFINITY;
                        field.has_default = true;
                    }
                    _ => {
                        field.default_string = t.text.clone();
                        field.has_default = true;
                    }
                }
            }
            Tok::StringLit => {
                self.lex.next();
                field.default_string = t.text.clone();
                field.has_default = true;
            }
            _ => {}
        }
    }

    fn parse_field_metadata(&mut self, field: &mut FbsFieldDef) -> Result<(), ParseError> {
        self.expect(Tok::LParen)?;
        while !self.peek_is(Tok::RParen) && !self.peek_is(Tok::Eof) {
            let attr = self.expect_ident()?;
            match attr.text.as_str() {
                "deprecated" => field.metadata.deprecated = true,
                "id" => {
                    self.expect(Tok::Colon)?;
                    let val = self.expect(Tok::Integer)?;
                    field.metadata.id = Self::parse_integer_literal(&val.text) as i32;
                }
                "required" => field.metadata.required = true,
                _ => {
                    if self.peek_is(Tok::Colon) {
                        self.lex.next();
                        self.lex.next(); // skip value
                    }
                }
            }
            if self.peek_is(Tok::Comma) {
                self.lex.next();
            }
        }
        self.expect(Tok::RParen)?;
        Ok(())
    }

    fn skip_metadata(&mut self) {
        let _ = self.lex.next(); // consume '('
        let mut depth = 1;
        while depth > 0 && !self.peek_is(Tok::Eof) {
            let t = self.lex.next();
            if t.kind == Tok::LParen {
                depth += 1;
            } else if t.kind == Tok::RParen {
                depth -= 1;
            }
        }
    }

    fn parse_root_type(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwRootType)?;
        let name = self.expect_ident()?;
        schema.root_type = name.text.clone();
        while self.peek_is(Tok::Dot) {
            self.lex.next();
            let part = self.expect_ident()?;
            schema.root_type.push('.');
            schema.root_type.push_str(&part.text);
        }
        self.expect(Tok::Semicolon)?;
        Ok(())
    }

    fn parse_file_identifier(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwFileIdentifier)?;
        let s = self.expect(Tok::StringLit)?;
        schema.file_identifier = s.text;
        self.expect(Tok::Semicolon)?;
        Ok(())
    }

    fn parse_file_extension(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwFileExtension)?;
        let s = self.expect(Tok::StringLit)?;
        schema.file_extension = s.text;
        self.expect(Tok::Semicolon)?;
        Ok(())
    }

    fn parse_include(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwInclude)?;
        let s = self.expect(Tok::StringLit)?;
        schema.includes.push(s.text);
        self.expect(Tok::Semicolon)?;
        Ok(())
    }

    fn parse_attribute(&mut self, schema: &mut ParsedFbsFile) -> Result<(), ParseError> {
        self.expect(Tok::KwAttribute)?;
        if self.peek_is(Tok::StringLit) {
            let s = self.lex.next();
            schema.attributes.push(s.text);
        } else {
            let name = self.expect_ident()?;
            schema.attributes.push(name.text);
        }
        self.expect(Tok::Semicolon)?;
        Ok(())
    }

    fn skip_rpc_service(&mut self) -> Result<(), ParseError> {
        self.expect(Tok::KwRpcService)?;
        self.expect_ident()?;
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

    fn parse_scalar_base_type(&mut self) -> Result<FbsBaseType, ParseError> {
        let t = self.lex.next();
        if is_scalar_type_tok(t.kind) {
            Ok(scalar_tok_to_base_type(t.kind))
        } else {
            Err(ParseError {
                message: "expected scalar type".to_string(),
                line: t.line,
                column: t.col,
            })
        }
    }

    fn parse_integer_literal(text: &str) -> i64 {
        if text.len() >= 2
            && text.as_bytes()[0] == b'0'
            && (text.as_bytes()[1] == b'x' || text.as_bytes()[1] == b'X')
        {
            i64::from_str_radix(&text[2..], 16).unwrap_or(0)
        } else {
            text.parse::<i64>().unwrap_or(0)
        }
    }

    fn parse_integer_value(&mut self) -> Result<i64, ParseError> {
        let t = self.lex.next();
        if t.kind == Tok::Integer {
            Ok(Self::parse_integer_literal(&t.text))
        } else {
            Err(ParseError {
                message: "expected integer value".to_string(),
                line: t.line,
                column: t.col,
            })
        }
    }

    // Post-parse: type resolution
    fn resolve_types(&self, schema: &mut ParsedFbsFile) {
        let type_map = schema.type_map.clone();
        // Snapshot kinds so we can look up type kinds without borrowing schema.types immutably
        let type_kinds: Vec<(FbsTypeKind, FbsBaseType)> = schema
            .types
            .iter()
            .map(|t| (t.kind, t.underlying_type))
            .collect();

        for td in &mut schema.types {
            match td.kind {
                FbsTypeKind::Table | FbsTypeKind::Struct => {
                    for field in &mut td.fields {
                        Self::resolve_field_type(&type_map, &type_kinds, field);
                    }
                }
                FbsTypeKind::Union => {
                    for um in &mut td.union_members {
                        if let Some(&idx) = type_map.get(&um.name) {
                            um.type_idx = idx as i32;
                        }
                    }
                }
                _ => {}
            }
        }
    }

    fn resolve_field_type(
        type_map: &HashMap<String, u32>,
        type_kinds: &[(FbsTypeKind, FbsBaseType)],
        field: &mut FbsFieldDef,
    ) {
        if field.field_type == FbsBaseType::Table
            && field.type_idx == -1
            && !field.unresolved_type_name.is_empty()
        {
            if let Some(&idx) = type_map.get(&field.unresolved_type_name) {
                let (kind, _) = type_kinds[idx as usize];
                match kind {
                    FbsTypeKind::Table => {
                        field.field_type = FbsBaseType::Table;
                        field.type_idx = idx as i32;
                    }
                    FbsTypeKind::Struct => {
                        field.field_type = FbsBaseType::Struct;
                        field.type_idx = idx as i32;
                    }
                    FbsTypeKind::Enum => {
                        field.field_type = FbsBaseType::Enum;
                        field.type_idx = idx as i32;
                    }
                    FbsTypeKind::Union => {
                        field.field_type = FbsBaseType::Union;
                        field.type_idx = idx as i32;
                    }
                }
                field.unresolved_type_name.clear();
            }
        }

        if field.field_type == FbsBaseType::Vector
            && field.elem_type == FbsBaseType::Table
            && field.elem_type_idx == -1
            && !field.unresolved_elem_name.is_empty()
        {
            if let Some(&idx) = type_map.get(&field.unresolved_elem_name) {
                let (kind, _) = type_kinds[idx as usize];
                match kind {
                    FbsTypeKind::Table => {
                        field.elem_type = FbsBaseType::Table;
                        field.elem_type_idx = idx as i32;
                    }
                    FbsTypeKind::Struct => {
                        field.elem_type = FbsBaseType::Struct;
                        field.elem_type_idx = idx as i32;
                    }
                    FbsTypeKind::Enum => {
                        field.elem_type = FbsBaseType::Enum;
                        field.elem_type_idx = idx as i32;
                    }
                    _ => {}
                }
                field.unresolved_elem_name.clear();
            }
        }
    }

    // Post-parse: layout computation
    fn compute_layouts(&self, schema: &mut ParsedFbsFile) {
        // Need to process structs first (so their sizes are known for table layout)
        let types_snapshot: Vec<FbsTypeDef> = schema.types.clone();
        for td in &mut schema.types {
            if td.kind == FbsTypeKind::Struct {
                Self::compute_struct_layout(&types_snapshot, td);
            }
        }
        // Re-snapshot after struct layouts are computed
        let types_snapshot: Vec<FbsTypeDef> = schema.types.clone();
        for td in &mut schema.types {
            if td.kind == FbsTypeKind::Table {
                Self::compute_table_layout(&types_snapshot, td);
            }
        }
    }

    fn compute_table_layout(types: &[FbsTypeDef], td: &mut FbsTypeDef) {
        let has_explicit_ids = td.fields.iter().any(|f| f.metadata.id >= 0);

        if has_explicit_ids {
            for f in &mut td.fields {
                if f.metadata.id >= 0 {
                    f.vtable_slot = f.metadata.id;
                } else {
                    f.vtable_slot = 0;
                }
            }
        } else {
            let mut slot = 0i32;
            for f in &mut td.fields {
                f.vtable_slot = slot;
                if f.field_type == FbsBaseType::Union {
                    slot += 2;
                } else {
                    slot += 1;
                }
            }
        }

        let mut max_slot = 0i32;
        for f in &td.fields {
            let end_slot = if f.field_type == FbsBaseType::Union {
                f.vtable_slot + 2
            } else {
                f.vtable_slot + 1
            };
            max_slot = max_slot.max(end_slot);
        }
        td.vtable_slot_count = max_slot as u16;

        for f in &mut td.fields {
            Self::compute_field_wire_info(types, f);
        }
    }

    fn compute_struct_layout(types: &[FbsTypeDef], td: &mut FbsTypeDef) {
        let mut offset: u32 = 0;
        let mut max_align: u32 = 1;

        for f in &mut td.fields {
            Self::compute_field_wire_info(types, f);

            let align = if f.wire_align == 0 { 1u32 } else { f.wire_align as u32 };
            max_align = max_align.max(align);

            offset = (offset + align - 1) & !(align - 1);
            f.struct_offset = offset;
            offset += f.wire_size as u32;
        }

        td.struct_align = max_align;
        td.struct_size = (offset + max_align - 1) & !(max_align - 1);
    }

    fn compute_field_wire_info(types: &[FbsTypeDef], f: &mut FbsFieldDef) {
        match f.field_type {
            FbsBaseType::Bool => {
                f.wire_size = 1;
                f.wire_align = 1;
                f.is_offset_type = false;
            }
            FbsBaseType::Int8 | FbsBaseType::UInt8 => {
                f.wire_size = 1;
                f.wire_align = 1;
                f.is_offset_type = false;
            }
            FbsBaseType::Int16 | FbsBaseType::UInt16 => {
                f.wire_size = 2;
                f.wire_align = 2;
                f.is_offset_type = false;
            }
            FbsBaseType::Int32 | FbsBaseType::UInt32 | FbsBaseType::Float32 => {
                f.wire_size = 4;
                f.wire_align = 4;
                f.is_offset_type = false;
            }
            FbsBaseType::Int64 | FbsBaseType::UInt64 | FbsBaseType::Float64 => {
                f.wire_size = 8;
                f.wire_align = 8;
                f.is_offset_type = false;
            }
            FbsBaseType::String => {
                f.wire_size = 4;
                f.wire_align = 4;
                f.is_offset_type = true;
            }
            FbsBaseType::Vector => {
                f.wire_size = 4;
                f.wire_align = 4;
                f.is_offset_type = true;
            }
            FbsBaseType::Table => {
                f.wire_size = 4;
                f.wire_align = 4;
                f.is_offset_type = true;
            }
            FbsBaseType::Struct => {
                if f.type_idx >= 0 && (f.type_idx as usize) < types.len() {
                    let ref_type = &types[f.type_idx as usize];
                    f.wire_size = ref_type.struct_size as u8;
                    f.wire_align = ref_type.struct_align as u8;
                }
                f.is_offset_type = false;
            }
            FbsBaseType::Enum => {
                if f.type_idx >= 0 && (f.type_idx as usize) < types.len() {
                    let ref_type = &types[f.type_idx as usize];
                    let sz = base_type_size(ref_type.underlying_type);
                    f.wire_size = sz;
                    f.wire_align = sz;
                } else {
                    f.wire_size = 4;
                    f.wire_align = 4;
                }
                f.is_offset_type = false;
            }
            FbsBaseType::Union => {
                f.wire_size = 4;
                f.wire_align = 4;
                f.is_offset_type = true;
            }
            _ => {}
        }
    }
}

fn is_scalar_type_tok(k: Tok) -> bool {
    matches!(
        k,
        Tok::KwBool
            | Tok::KwInt8
            | Tok::KwUInt8
            | Tok::KwInt16
            | Tok::KwUInt16
            | Tok::KwInt32
            | Tok::KwUInt32
            | Tok::KwInt64
            | Tok::KwUInt64
            | Tok::KwFloat32
            | Tok::KwFloat64
    )
}

fn scalar_tok_to_base_type(k: Tok) -> FbsBaseType {
    match k {
        Tok::KwBool => FbsBaseType::Bool,
        Tok::KwInt8 => FbsBaseType::Int8,
        Tok::KwUInt8 => FbsBaseType::UInt8,
        Tok::KwInt16 => FbsBaseType::Int16,
        Tok::KwUInt16 => FbsBaseType::UInt16,
        Tok::KwInt32 => FbsBaseType::Int32,
        Tok::KwUInt32 => FbsBaseType::UInt32,
        Tok::KwInt64 => FbsBaseType::Int64,
        Tok::KwUInt64 => FbsBaseType::UInt64,
        Tok::KwFloat32 => FbsBaseType::Float32,
        Tok::KwFloat64 => FbsBaseType::Float64,
        _ => FbsBaseType::None,
    }
}

// =========================================================================
// Public API
// =========================================================================

/// Parse a `.fbs` schema source string into a [`ParsedFbsFile`].
pub fn parse_fbs(input: &str) -> Result<ParsedFbsFile, ParseError> {
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
    fn test_simple_table() {
        let input = r#"
            table Point {
                x: double;
                y: double;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        assert_eq!(file.types.len(), 1);

        let point = &file.types[0];
        assert_eq!(point.name, "Point");
        assert_eq!(point.kind, FbsTypeKind::Table);
        assert_eq!(point.fields.len(), 2);
        assert_eq!(point.fields[0].name, "x");
        assert_eq!(point.fields[0].field_type, FbsBaseType::Float64);
        assert_eq!(point.fields[1].name, "y");
        assert_eq!(point.fields[1].field_type, FbsBaseType::Float64);
        assert_eq!(point.fields[0].vtable_slot, 0);
        assert_eq!(point.fields[1].vtable_slot, 1);
    }

    #[test]
    fn test_namespace() {
        let input = r#"
            namespace game.entities;
            table Monster {
                hp: int16;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        assert_eq!(file.ns, "game.entities");
        assert_eq!(file.types[0].full_name, "game.entities.Monster");
    }

    #[test]
    fn test_struct_layout() {
        let input = r#"
            struct Vec3 {
                x: float;
                y: float;
                z: float;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let v = &file.types[0];
        assert_eq!(v.kind, FbsTypeKind::Struct);
        assert_eq!(v.struct_size, 12); // 3 x 4 bytes
        assert_eq!(v.struct_align, 4);
        assert_eq!(v.fields[0].struct_offset, 0);
        assert_eq!(v.fields[1].struct_offset, 4);
        assert_eq!(v.fields[2].struct_offset, 8);
    }

    #[test]
    fn test_enum() {
        let input = r#"
            enum Color: byte {
                Red = 0,
                Green,
                Blue = 5,
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let e = &file.types[0];
        assert_eq!(e.kind, FbsTypeKind::Enum);
        assert_eq!(e.enum_values.len(), 3);
        assert_eq!(e.enum_values[0].name, "Red");
        assert_eq!(e.enum_values[0].value, 0);
        assert_eq!(e.enum_values[1].name, "Green");
        assert_eq!(e.enum_values[1].value, 1);
        assert_eq!(e.enum_values[2].name, "Blue");
        assert_eq!(e.enum_values[2].value, 5);
    }

    #[test]
    fn test_union() {
        let input = r#"
            table Sword { damage: int32; }
            table Shield { defense: int32; }
            union Equipment { Sword, Shield }
        "#;
        let file = parse_fbs(input).unwrap();
        let u = file.find_type("Equipment").unwrap();
        assert_eq!(u.kind, FbsTypeKind::Union);
        assert_eq!(u.union_members.len(), 2);
        assert_eq!(u.union_members[0].name, "Sword");
        assert_eq!(u.union_members[1].name, "Shield");
    }

    #[test]
    fn test_vector_type() {
        let input = r#"
            table Inventory {
                items: [string];
                counts: [uint32];
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let inv = &file.types[0];
        assert_eq!(inv.fields[0].field_type, FbsBaseType::Vector);
        assert_eq!(inv.fields[0].elem_type, FbsBaseType::String);
        assert_eq!(inv.fields[1].field_type, FbsBaseType::Vector);
        assert_eq!(inv.fields[1].elem_type, FbsBaseType::UInt32);
    }

    #[test]
    fn test_table_ref() {
        let input = r#"
            table Inner { x: int32; }
            table Outer {
                inner: Inner;
                value: uint64;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let outer = file.find_type("Outer").unwrap();
        assert_eq!(outer.fields[0].field_type, FbsBaseType::Table);
        assert_eq!(outer.fields[0].type_idx, 0);
    }

    #[test]
    fn test_dynamic_schema_conversion() {
        let input = r#"
            table Point {
                x: double;
                y: double;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let schema = file.to_dynamic_schema("Point").unwrap();
        assert_eq!(schema.field_count(), 2);

        let x = schema.find_by_name("x").unwrap();
        assert_eq!(x.ty, DynamicType::F64);

        let y = schema.find_by_name("y").unwrap();
        assert_eq!(y.ty, DynamicType::F64);
    }

    #[test]
    fn test_root_type() {
        let input = r#"
            table Monster { hp: int32; }
            root_type Monster;
        "#;
        let file = parse_fbs(input).unwrap();
        assert_eq!(file.root_type, "Monster");
    }

    #[test]
    fn test_default_values() {
        let input = r#"
            table Config {
                timeout: uint32 = 30;
                name: string;
                enabled: bool = true;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let c = &file.types[0];
        assert_eq!(c.fields[0].default_int, 30);
        assert!(c.fields[0].has_default);
        assert_eq!(c.fields[2].default_int, 1); // true
    }

    #[test]
    fn test_comments() {
        let input = r#"
            // This is a line comment
            table Point {
                x: double; // x coord
                /* block comment */
                y: double;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        assert_eq!(file.types.len(), 1);
        assert_eq!(file.types[0].fields.len(), 2);
    }

    #[test]
    fn test_bench_schemas() {
        let input = include_str!("../../../../cpp/benchmarks/bench_schemas.fbs");
        let file = parse_fbs(input).unwrap();

        // Should have: Point, Token, UserProfile, LineItem, Order, SensorReading
        assert_eq!(file.types.len(), 6);
        assert_eq!(file.ns, "fb");

        let point = file.find_type("Point").unwrap();
        assert_eq!(point.fields.len(), 2);
        assert_eq!(point.kind, FbsTypeKind::Table);

        let token = file.find_type("Token").unwrap();
        assert_eq!(token.fields.len(), 4);

        let profile = file.find_type("UserProfile").unwrap();
        assert_eq!(profile.fields.len(), 8);

        let sensor = file.find_type("SensorReading").unwrap();
        assert_eq!(sensor.fields.len(), 18);

        let order = file.find_type("Order").unwrap();
        assert_eq!(order.fields.len(), 5);

        // Verify DynamicSchema for SensorReading
        let schema = file.to_dynamic_schema("SensorReading").unwrap();
        assert_eq!(schema.field_count(), 18);
        let ts = schema.find_by_name("timestamp").unwrap();
        assert_eq!(ts.ty, DynamicType::U64);
        let dev = schema.find_by_name("device_id").unwrap();
        assert_eq!(dev.ty, DynamicType::Text);
        assert!(dev.is_ptr);
    }

    #[test]
    fn test_type_aliases() {
        let input = r#"
            table T {
                a: byte;
                b: ubyte;
                c: short;
                d: ushort;
                e: int;
                f: uint;
                g: long;
                h: ulong;
                i: float;
                j: double;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let t = &file.types[0];
        assert_eq!(t.fields[0].field_type, FbsBaseType::Int8);
        assert_eq!(t.fields[1].field_type, FbsBaseType::UInt8);
        assert_eq!(t.fields[2].field_type, FbsBaseType::Int16);
        assert_eq!(t.fields[3].field_type, FbsBaseType::UInt16);
        assert_eq!(t.fields[4].field_type, FbsBaseType::Int32);
        assert_eq!(t.fields[5].field_type, FbsBaseType::UInt32);
        assert_eq!(t.fields[6].field_type, FbsBaseType::Int64);
        assert_eq!(t.fields[7].field_type, FbsBaseType::UInt64);
        assert_eq!(t.fields[8].field_type, FbsBaseType::Float32);
        assert_eq!(t.fields[9].field_type, FbsBaseType::Float64);
    }

    #[test]
    fn test_struct_with_alignment() {
        let input = r#"
            struct Mixed {
                a: byte;
                b: int32;
                c: byte;
            }
        "#;
        let file = parse_fbs(input).unwrap();
        let s = &file.types[0];
        assert_eq!(s.kind, FbsTypeKind::Struct);
        // a at 0 (1 byte), padding to align 4, b at 4 (4 bytes), c at 8 (1 byte)
        // Total padded to align 4 = 12
        assert_eq!(s.fields[0].struct_offset, 0);
        assert_eq!(s.fields[1].struct_offset, 4);
        assert_eq!(s.fields[2].struct_offset, 8);
        assert_eq!(s.struct_align, 4);
        assert_eq!(s.struct_size, 12);
    }

    #[test]
    fn test_vector_of_tables() {
        let input = r#"
            table Item { name: string; }
            table Container { items: [Item]; }
        "#;
        let file = parse_fbs(input).unwrap();
        let c = file.find_type("Container").unwrap();
        assert_eq!(c.fields[0].field_type, FbsBaseType::Vector);
        assert_eq!(c.fields[0].elem_type, FbsBaseType::Table);
        assert_eq!(c.fields[0].elem_type_idx, 0);

        // DynamicSchema should have nested schema for vector element
        let schema = file.to_dynamic_schema("Container").unwrap();
        let items = schema.find_by_name("items").unwrap();
        assert_eq!(items.ty, DynamicType::Vector);
        assert!(items.nested.is_some());
    }
}
