//! Cap'n Proto `.capnp` IDL parser.
//!
//! Parses Cap'n Proto schema language into a [`ParsedCapnpFile`] with computed
//! wire layout (data_words, ptr_count, per-field locations).
//!
//! Port of the C++ `capnp_parser.hpp` to Rust.

use crate::dynamic_schema::{
    AltDesc, DynamicSchema, DynamicType, FieldDesc,
};

use std::collections::HashMap;
use std::fmt;

// =========================================================================
// Public types
// =========================================================================

/// Error returned by the capnp parser.
#[derive(Debug, Clone)]
pub struct ParseError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "capnp:{}:{}: {}", self.line, self.column, self.message)
    }
}

impl std::error::Error for ParseError {}

/// Cap'n Proto primitive/builtin type tags.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CapnpTypeTag {
    Void,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float32,
    Float64,
    Text,
    Data,
    List,
    Struct,
    Enum,
    AnyPointer,
}

/// A type reference -- either a builtin or a reference to a compound type.
#[derive(Debug, Clone)]
pub struct CapnpTypeRef {
    pub tag: CapnpTypeTag,
    /// For List: index into file types (encoded) or tag of element.
    pub element_type_idx: i32,
    /// For Struct/Enum: index into file.structs / file.enums.
    pub referenced_type_idx: i32,
}

impl Default for CapnpTypeRef {
    fn default() -> Self {
        Self {
            tag: CapnpTypeTag::Void,
            element_type_idx: -1,
            referenced_type_idx: -1,
        }
    }
}

/// Location of a field within the capnp wire format.
#[derive(Debug, Clone, Default)]
pub struct CapnpFieldLoc {
    pub is_ptr: bool,
    pub offset: u32,
    pub bit_index: u8,
}

/// A parsed field within a struct.
#[derive(Debug, Clone)]
pub struct CapnpParsedField {
    pub name: String,
    pub ordinal: u32,
    pub type_ref: CapnpTypeRef,
    pub default_value: String,
    pub loc: CapnpFieldLoc,
}

/// A parsed union within a struct.
#[derive(Debug, Clone)]
pub struct CapnpParsedUnion {
    pub alternatives: Vec<CapnpParsedField>,
    pub discriminant_loc: CapnpFieldLoc,
}

/// A parsed enum definition.
#[derive(Debug, Clone)]
pub struct CapnpParsedEnum {
    pub name: String,
    pub id: u64,
    pub enumerants: Vec<String>,
}

/// A parsed struct definition.
#[derive(Debug, Clone)]
pub struct CapnpParsedStruct {
    pub name: String,
    pub id: u64,
    pub fields: Vec<CapnpParsedField>,
    pub unions: Vec<CapnpParsedUnion>,
    pub data_words: u16,
    pub ptr_count: u16,
}

/// A type alias (using Name = Type).
#[derive(Debug, Clone)]
pub struct CapnpTypeAlias {
    pub name: String,
    pub type_ref: CapnpTypeRef,
}

/// A complete parsed .capnp file.
#[derive(Debug, Clone)]
pub struct ParsedCapnpFile {
    pub file_id: u64,
    pub structs: Vec<CapnpParsedStruct>,
    pub enums: Vec<CapnpParsedEnum>,
    pub aliases: Vec<CapnpTypeAlias>,
    pub type_map: HashMap<String, CapnpTypeRef>,
}

impl ParsedCapnpFile {
    /// Find a struct by name.
    pub fn find_struct(&self, name: &str) -> Option<&CapnpParsedStruct> {
        self.structs.iter().find(|s| s.name == name)
    }

    /// Find an enum by name.
    pub fn find_enum(&self, name: &str) -> Option<&CapnpParsedEnum> {
        self.enums.iter().find(|e| e.name == name)
    }

    /// Convert a parsed struct to a `DynamicSchema`.
    pub fn to_dynamic_schema(&self, struct_name: &str) -> Option<DynamicSchema> {
        let s = self.find_struct(struct_name)?;
        Some(capnp_struct_to_dynamic(s, self))
    }
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
    At,
    Colon,
    Semicolon,
    Equal,
    Dot,
    Comma,
    // keywords
    KwStruct,
    KwUnion,
    KwEnum,
    KwUsing,
    KwConst,
    KwAnnotation,
    KwInterface,
    KwImport,
    // type keywords
    KwVoid,
    KwBool,
    KwInt8,
    KwInt16,
    KwInt32,
    KwInt64,
    KwUInt8,
    KwUInt16,
    KwUInt32,
    KwUInt64,
    KwFloat32,
    KwFloat64,
    KwText,
    KwData,
    KwList,
    KwAnyPointer,
    // literals
    KwTrue,
    KwFalse,
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
            if c == '#' {
                while self.pos < self.src.len() && self.src[self.pos] != '\n' {
                    self.advance(1);
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

        // Single-char punctuation
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
            '@' => return single(self, Tok::At),
            ':' => return single(self, Tok::Colon),
            ';' => return single(self, Tok::Semicolon),
            '=' => return single(self, Tok::Equal),
            '.' => return single(self, Tok::Dot),
            ',' => return single(self, Tok::Comma),
            _ => {}
        }

        // String literals
        if c == '"' {
            return self.lex_string(start_line, start_col);
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

        Token {
            kind: Tok::Eof,
            text: c.to_string(),
            line: start_line,
            col: start_col,
        }
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

    fn lex_string(&mut self, start_line: u32, start_col: u32) -> Token {
        let start = self.pos;
        self.advance(1); // skip opening quote
        while self.pos < self.src.len() && self.src[self.pos] != '"' {
            if self.src[self.pos] == '\\' {
                self.advance(1);
            }
            self.advance(1);
        }
        if self.pos < self.src.len() {
            self.advance(1); // skip closing quote
        }
        let text: String = self.src[start..self.pos].iter().collect();
        Token { kind: Tok::StringLit, text, line: start_line, col: start_col }
    }

    fn lex_number(&mut self, start_line: u32, start_col: u32) -> Token {
        let start = self.pos;
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
            return Token { kind: Tok::Number, text, line: start_line, col: start_col };
        }
        // Decimal digits
        while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
            self.advance(1);
        }
        // Float
        if self.pos < self.src.len() && self.src[self.pos] == '.' {
            self.advance(1);
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
                self.advance(1);
            }
        }
        if self.pos < self.src.len() && (self.src[self.pos] == 'e' || self.src[self.pos] == 'E') {
            self.advance(1);
            if self.pos < self.src.len() && (self.src[self.pos] == '+' || self.src[self.pos] == '-') {
                self.advance(1);
            }
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
                self.advance(1);
            }
        }
        let text: String = self.src[start..self.pos].iter().collect();
        Token { kind: Tok::Number, text, line: start_line, col: start_col }
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
        "struct" => Tok::KwStruct,
        "union" => Tok::KwUnion,
        "enum" => Tok::KwEnum,
        "using" => Tok::KwUsing,
        "const" => Tok::KwConst,
        "annotation" => Tok::KwAnnotation,
        "interface" => Tok::KwInterface,
        "import" => Tok::KwImport,
        "Void" => Tok::KwVoid,
        "Bool" => Tok::KwBool,
        "Int8" => Tok::KwInt8,
        "Int16" => Tok::KwInt16,
        "Int32" => Tok::KwInt32,
        "Int64" => Tok::KwInt64,
        "UInt8" => Tok::KwUInt8,
        "UInt16" => Tok::KwUInt16,
        "UInt32" => Tok::KwUInt32,
        "UInt64" => Tok::KwUInt64,
        "Float32" => Tok::KwFloat32,
        "Float64" => Tok::KwFloat64,
        "Text" => Tok::KwText,
        "Data" => Tok::KwData,
        "List" => Tok::KwList,
        "AnyPointer" => Tok::KwAnyPointer,
        "true" => Tok::KwTrue,
        "false" => Tok::KwFalse,
        "inf" | "nan" => Tok::Number,
        _ => Tok::Ident,
    }
}

// =========================================================================
// Layout computation
// =========================================================================

fn type_byte_size(tag: CapnpTypeTag) -> u32 {
    match tag {
        CapnpTypeTag::Bool => 0,
        CapnpTypeTag::Int8 | CapnpTypeTag::UInt8 => 1,
        CapnpTypeTag::Int16 | CapnpTypeTag::UInt16 | CapnpTypeTag::Enum => 2,
        CapnpTypeTag::Int32 | CapnpTypeTag::UInt32 | CapnpTypeTag::Float32 => 4,
        CapnpTypeTag::Int64 | CapnpTypeTag::UInt64 | CapnpTypeTag::Float64 => 8,
        _ => 0,
    }
}

#[allow(dead_code)]
fn is_data_type(tag: CapnpTypeTag) -> bool {
    matches!(tag,
        CapnpTypeTag::Bool
        | CapnpTypeTag::Int8 | CapnpTypeTag::Int16 | CapnpTypeTag::Int32 | CapnpTypeTag::Int64
        | CapnpTypeTag::UInt8 | CapnpTypeTag::UInt16 | CapnpTypeTag::UInt32 | CapnpTypeTag::UInt64
        | CapnpTypeTag::Float32 | CapnpTypeTag::Float64
        | CapnpTypeTag::Enum
    )
}

fn is_ptr_type(tag: CapnpTypeTag) -> bool {
    matches!(tag,
        CapnpTypeTag::Text | CapnpTypeTag::Data | CapnpTypeTag::List
        | CapnpTypeTag::Struct | CapnpTypeTag::AnyPointer
    )
}

/// Compute the capnp wire layout for a parsed struct.
fn compute_layout(s: &mut CapnpParsedStruct) {
    let mut occupied = [false; 2048];
    let mut data_words: u16 = 0;
    let mut ptr_count: u16 = 0;

    let alloc_bits = |bit_count: u32, bit_align: u32, occupied: &mut [bool; 2048], data_words: &mut u16| -> u32 {
        let mut bit: u32 = 0;
        loop {
            let end_bit = bit + bit_count;
            let words_needed = ((end_bit + 63) / 64) as u16;
            if words_needed > *data_words {
                *data_words = words_needed;
            }

            let mut ok = true;
            for b in bit..end_bit {
                if (b as usize) < 2048 && occupied[b as usize] {
                    ok = false;
                    break;
                }
            }
            if ok {
                for b in bit..end_bit {
                    if (b as usize) < 2048 {
                        occupied[b as usize] = true;
                    }
                }
                return bit;
            }
            bit += bit_align;
        }
    };

    // Sort regular fields by ordinal
    s.fields.sort_by_key(|f| f.ordinal);

    // Sort union alternatives by ordinal
    for u in &mut s.unions {
        u.alternatives.sort_by_key(|a| a.ordinal);
    }

    // Build merged ordinal list
    #[derive(Clone)]
    struct OrdinalEntry {
        ordinal: u32,
        is_union_alt: bool,
        union_idx: usize,
        alt_idx: usize,
        field_idx: usize,
    }

    let mut entries = Vec::new();

    for (i, f) in s.fields.iter().enumerate() {
        entries.push(OrdinalEntry {
            ordinal: f.ordinal,
            is_union_alt: false,
            field_idx: i,
            union_idx: 0,
            alt_idx: 0,
        });
    }

    for (ui, u) in s.unions.iter().enumerate() {
        for (ai, alt) in u.alternatives.iter().enumerate() {
            entries.push(OrdinalEntry {
                ordinal: alt.ordinal,
                is_union_alt: true,
                union_idx: ui,
                alt_idx: ai,
                field_idx: 0,
            });
        }
    }

    entries.sort_by_key(|e| e.ordinal);

    // Allocate slots in ordinal order
    for e in &entries {
        let tag = if e.is_union_alt {
            s.unions[e.union_idx].alternatives[e.alt_idx].type_ref.tag
        } else {
            s.fields[e.field_idx].type_ref.tag
        };

        let loc = if tag == CapnpTypeTag::Void {
            CapnpFieldLoc::default()
        } else if is_ptr_type(tag) {
            let loc = CapnpFieldLoc { is_ptr: true, offset: ptr_count as u32, bit_index: 0 };
            ptr_count += 1;
            loc
        } else if tag == CapnpTypeTag::Bool {
            let bit = alloc_bits(1, 1, &mut occupied, &mut data_words);
            CapnpFieldLoc { is_ptr: false, offset: bit / 8, bit_index: (bit % 8) as u8 }
        } else {
            let sz = type_byte_size(tag);
            let bit = alloc_bits(sz * 8, sz * 8, &mut occupied, &mut data_words);
            CapnpFieldLoc { is_ptr: false, offset: bit / 8, bit_index: 0 }
        };

        if e.is_union_alt {
            s.unions[e.union_idx].alternatives[e.alt_idx].loc = loc;
        } else {
            s.fields[e.field_idx].loc = loc;
        }
    }

    // Allocate discriminants for unions
    for u in &mut s.unions {
        let bit = alloc_bits(16, 16, &mut occupied, &mut data_words);
        u.discriminant_loc = CapnpFieldLoc { is_ptr: false, offset: bit / 8, bit_index: 0 };
    }

    s.data_words = data_words;
    s.ptr_count = ptr_count;
}

// =========================================================================
// Convert to DynamicSchema
// =========================================================================

fn capnp_tag_to_dynamic_type(tag: CapnpTypeTag) -> DynamicType {
    match tag {
        CapnpTypeTag::Void => DynamicType::Void,
        CapnpTypeTag::Bool => DynamicType::Bool,
        CapnpTypeTag::Int8 => DynamicType::I8,
        CapnpTypeTag::Int16 => DynamicType::I16,
        CapnpTypeTag::Int32 => DynamicType::I32,
        CapnpTypeTag::Int64 => DynamicType::I64,
        CapnpTypeTag::UInt8 => DynamicType::U8,
        CapnpTypeTag::UInt16 => DynamicType::U16,
        CapnpTypeTag::UInt32 => DynamicType::U32,
        CapnpTypeTag::UInt64 => DynamicType::U64,
        CapnpTypeTag::Float32 => DynamicType::F32,
        CapnpTypeTag::Float64 => DynamicType::F64,
        CapnpTypeTag::Text => DynamicType::Text,
        CapnpTypeTag::Data => DynamicType::Data,
        CapnpTypeTag::List => DynamicType::Vector,
        CapnpTypeTag::Struct => DynamicType::Struct,
        CapnpTypeTag::Enum => DynamicType::U16,
        CapnpTypeTag::AnyPointer => DynamicType::Data,
    }
}

fn capnp_field_to_field_desc(
    field: &CapnpParsedField,
    file: &ParsedCapnpFile,
) -> FieldDesc {
    let ty = capnp_tag_to_dynamic_type(field.type_ref.tag);
    let nested = if field.type_ref.tag == CapnpTypeTag::Struct
        && field.type_ref.referenced_type_idx >= 0
    {
        let idx = field.type_ref.referenced_type_idx as usize;
        if idx < file.structs.len() {
            Some(Box::new(capnp_struct_to_dynamic(
                &file.structs[idx],
                file,
            )))
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
        is_ptr: field.loc.is_ptr,
        offset: field.loc.offset,
        bit_index: field.loc.bit_index,
        byte_size: ty.byte_size(),
        nested,
        alternatives: Vec::new(),
        disc_offset: 0,
    };

    // For list fields containing structs, set nested schema on element
    if field.type_ref.tag == CapnpTypeTag::List {
        let elem_idx = field.type_ref.element_type_idx;
        // Positive values = struct index
        if elem_idx >= 0 && (elem_idx as usize) < file.structs.len() {
            fd.nested = Some(Box::new(capnp_struct_to_dynamic(
                &file.structs[elem_idx as usize],
                file,
            )));
        }
    }

    fd
}

fn capnp_struct_to_dynamic(
    s: &CapnpParsedStruct,
    file: &ParsedCapnpFile,
) -> DynamicSchema {
    let mut fields = Vec::new();

    for f in &s.fields {
        fields.push(capnp_field_to_field_desc(f, file));
    }

    // Convert unions to variant fields
    for u in &s.unions {
        if u.alternatives.is_empty() {
            continue;
        }
        // Use first alternative name as variant name, or "union"
        let name = format!("union_{}", fields.len());
        let mut alts = Vec::new();
        for alt in &u.alternatives {
            let ty = capnp_tag_to_dynamic_type(alt.type_ref.tag);
            alts.push(AltDesc {
                ty,
                is_ptr: alt.loc.is_ptr,
                offset: alt.loc.offset,
                bit_index: alt.loc.bit_index,
                byte_size: ty.byte_size(),
                nested: if alt.type_ref.tag == CapnpTypeTag::Struct
                    && alt.type_ref.referenced_type_idx >= 0
                {
                    let idx = alt.type_ref.referenced_type_idx as usize;
                    if idx < file.structs.len() {
                        Some(Box::new(capnp_struct_to_dynamic(
                            &file.structs[idx],
                            file,
                        )))
                    } else {
                        None
                    }
                } else {
                    None
                },
            });
        }
        fields.push(FieldDesc::variant(&name, u.discriminant_loc.offset, alts));
    }

    DynamicSchema::new(fields, s.data_words, s.ptr_count)
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

    fn error<T>(&self, t: &Token, msg: &str) -> Result<T, ParseError> {
        Err(ParseError {
            message: msg.to_string(),
            line: t.line,
            column: t.col,
        })
    }

    fn parse_uint64(text: &str) -> u64 {
        if text.len() > 2 && text.starts_with("0x") || text.starts_with("0X") {
            let hex_part = &text[2..];
            u64::from_str_radix(hex_part, 16).unwrap_or(0)
        } else {
            text.parse::<u64>().unwrap_or(0)
        }
    }

    fn parse_uint32(text: &str) -> u32 {
        Self::parse_uint64(text) as u32
    }

    fn parse(&mut self) -> Result<ParsedCapnpFile, ParseError> {
        let mut file = ParsedCapnpFile {
            file_id: 0,
            structs: Vec::new(),
            enums: Vec::new(),
            aliases: Vec::new(),
            type_map: HashMap::new(),
        };

        // Parse optional file ID: @0xNNNN;
        if self.peek_is(Tok::At) {
            self.lex.next();
            let num_tok = self.expect(Tok::Number)?;
            file.file_id = Self::parse_uint64(&num_tok.text);
            self.expect(Tok::Semicolon)?;
        }

        // Parse top-level items
        while !self.peek_is(Tok::Eof) {
            let t = self.lex.peek();
            match t.kind {
                Tok::KwStruct => self.parse_struct(&mut file)?,
                Tok::KwEnum => self.parse_enum(&mut file)?,
                Tok::KwUsing => self.parse_using(&mut file)?,
                Tok::KwConst => self.skip_const()?,
                Tok::KwAnnotation => self.skip_annotation_decl(),
                Tok::KwInterface => self.skip_interface()?,
                Tok::KwImport => self.skip_import()?,
                _ => return self.error(&t, "expected struct, enum, using, const, annotation, or interface"),
            }
        }

        // Compute layouts for all structs
        for s in &mut file.structs {
            compute_layout(s);
        }

        Ok(file)
    }

    fn parse_struct(&mut self, file: &mut ParsedCapnpFile) -> Result<(), ParseError> {
        self.expect(Tok::KwStruct)?;
        let name_tok = self.expect_ident()?;
        let name = name_tok.text.clone();

        let mut s = CapnpParsedStruct {
            name: name.clone(),
            id: 0,
            fields: Vec::new(),
            unions: Vec::new(),
            data_words: 0,
            ptr_count: 0,
        };

        if self.peek_is(Tok::At) {
            self.lex.next();
            let id_tok = self.expect(Tok::Number)?;
            s.id = Self::parse_uint64(&id_tok.text);
        }

        if self.peek_is(Tok::LParen) {
            self.skip_generic_params();
        }

        self.expect(Tok::LBrace)?;
        self.parse_struct_body(file, &mut s)?;
        self.expect(Tok::RBrace)?;

        let type_ref = CapnpTypeRef {
            tag: CapnpTypeTag::Struct,
            element_type_idx: -1,
            referenced_type_idx: file.structs.len() as i32,
        };
        file.type_map.insert(name, type_ref);
        file.structs.push(s);
        Ok(())
    }

    fn parse_struct_body(
        &mut self,
        file: &mut ParsedCapnpFile,
        s: &mut CapnpParsedStruct,
    ) -> Result<(), ParseError> {
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let t = self.lex.peek();
            match t.kind {
                Tok::KwStruct => self.parse_struct(file)?,
                Tok::KwEnum => self.parse_enum(file)?,
                Tok::KwUsing => self.parse_using(file)?,
                Tok::KwUnion => self.parse_union(file, s)?,
                Tok::KwConst => self.skip_const()?,
                Tok::KwAnnotation => self.skip_annotation_decl(),
                Tok::Ident => self.parse_field(file, s)?,
                _ => return self.error(&t, "unexpected token in struct body"),
            }
        }
        Ok(())
    }

    fn parse_field(
        &mut self,
        file: &mut ParsedCapnpFile,
        s: &mut CapnpParsedStruct,
    ) -> Result<(), ParseError> {
        let name_tok = self.expect_ident()?;

        let mut field = CapnpParsedField {
            name: name_tok.text.clone(),
            ordinal: 0,
            type_ref: CapnpTypeRef::default(),
            default_value: String::new(),
            loc: CapnpFieldLoc::default(),
        };

        self.expect(Tok::At)?;
        let ord_tok = self.expect(Tok::Number)?;
        field.ordinal = Self::parse_uint32(&ord_tok.text);

        self.expect(Tok::Colon)?;
        field.type_ref = self.parse_type_ref(file)?;

        if self.peek_is(Tok::Equal) {
            self.lex.next();
            field.default_value = self.skip_default_value();
        }

        self.skip_field_annotations();
        self.expect(Tok::Semicolon)?;

        s.fields.push(field);
        Ok(())
    }

    fn parse_union(
        &mut self,
        file: &mut ParsedCapnpFile,
        s: &mut CapnpParsedStruct,
    ) -> Result<(), ParseError> {
        self.expect(Tok::KwUnion)?;

        let mut u = CapnpParsedUnion {
            alternatives: Vec::new(),
            discriminant_loc: CapnpFieldLoc::default(),
        };

        self.expect(Tok::LBrace)?;
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let t = self.lex.peek();
            if t.kind == Tok::Ident {
                let alt_name = self.expect_ident()?;
                let mut alt = CapnpParsedField {
                    name: alt_name.text.clone(),
                    ordinal: 0,
                    type_ref: CapnpTypeRef::default(),
                    default_value: String::new(),
                    loc: CapnpFieldLoc::default(),
                };

                self.expect(Tok::At)?;
                let ord_tok = self.expect(Tok::Number)?;
                alt.ordinal = Self::parse_uint32(&ord_tok.text);

                self.expect(Tok::Colon)?;
                alt.type_ref = self.parse_type_ref(file)?;

                if self.peek_is(Tok::Equal) {
                    self.lex.next();
                    alt.default_value = self.skip_default_value();
                }

                self.skip_field_annotations();
                self.expect(Tok::Semicolon)?;

                u.alternatives.push(alt);
            } else {
                return self.error(&t, "expected field in union");
            }
        }
        self.expect(Tok::RBrace)?;

        s.unions.push(u);
        Ok(())
    }

    fn parse_enum(&mut self, file: &mut ParsedCapnpFile) -> Result<(), ParseError> {
        self.expect(Tok::KwEnum)?;
        let name_tok = self.expect_ident()?;
        let name = name_tok.text.clone();

        let mut e = CapnpParsedEnum {
            name: name.clone(),
            id: 0,
            enumerants: Vec::new(),
        };

        if self.peek_is(Tok::At) {
            self.lex.next();
            let id_tok = self.expect(Tok::Number)?;
            e.id = Self::parse_uint64(&id_tok.text);
        }

        self.expect(Tok::LBrace)?;
        while !self.peek_is(Tok::RBrace) && !self.peek_is(Tok::Eof) {
            let label = self.expect_ident()?;
            self.expect(Tok::At)?;
            let _ord_tok = self.expect(Tok::Number)?;
            self.expect(Tok::Semicolon)?;
            e.enumerants.push(label.text.clone());
        }
        self.expect(Tok::RBrace)?;

        let type_ref = CapnpTypeRef {
            tag: CapnpTypeTag::Enum,
            element_type_idx: -1,
            referenced_type_idx: file.enums.len() as i32,
        };
        file.type_map.insert(name, type_ref);
        file.enums.push(e);
        Ok(())
    }

    fn parse_using(&mut self, file: &mut ParsedCapnpFile) -> Result<(), ParseError> {
        self.expect(Tok::KwUsing)?;
        let name_tok = self.expect_ident()?;
        let name = name_tok.text.clone();

        self.expect(Tok::Equal)?;
        let type_ref = self.parse_type_ref(file)?;
        self.expect(Tok::Semicolon)?;

        let alias = CapnpTypeAlias {
            name: name.clone(),
            type_ref: type_ref.clone(),
        };
        file.aliases.push(alias);
        file.type_map.insert(name, type_ref);
        Ok(())
    }

    fn parse_type_ref(
        &mut self,
        file: &ParsedCapnpFile,
    ) -> Result<CapnpTypeRef, ParseError> {
        let t = self.lex.peek();

        match t.kind {
            Tok::KwVoid => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Void, ..Default::default() }); }
            Tok::KwBool => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Bool, ..Default::default() }); }
            Tok::KwInt8 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Int8, ..Default::default() }); }
            Tok::KwInt16 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Int16, ..Default::default() }); }
            Tok::KwInt32 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Int32, ..Default::default() }); }
            Tok::KwInt64 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Int64, ..Default::default() }); }
            Tok::KwUInt8 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::UInt8, ..Default::default() }); }
            Tok::KwUInt16 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::UInt16, ..Default::default() }); }
            Tok::KwUInt32 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::UInt32, ..Default::default() }); }
            Tok::KwUInt64 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::UInt64, ..Default::default() }); }
            Tok::KwFloat32 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Float32, ..Default::default() }); }
            Tok::KwFloat64 => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Float64, ..Default::default() }); }
            Tok::KwText => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Text, ..Default::default() }); }
            Tok::KwData => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::Data, ..Default::default() }); }
            Tok::KwAnyPointer => { self.lex.next(); return Ok(CapnpTypeRef { tag: CapnpTypeTag::AnyPointer, ..Default::default() }); }
            _ => {}
        }

        // List(ElementType)
        if t.kind == Tok::KwList {
            self.lex.next();
            self.expect(Tok::LParen)?;
            let elem_type = self.parse_type_ref(file)?;
            self.expect(Tok::RParen)?;
            let encoded = Self::encode_type_ref(&elem_type);
            return Ok(CapnpTypeRef {
                tag: CapnpTypeTag::List,
                element_type_idx: encoded,
                referenced_type_idx: -1,
            });
        }

        // Named type reference
        if t.kind == Tok::Ident {
            let name = self.lex.next();
            let mut type_name = name.text.clone();
            while self.peek_is(Tok::Dot) {
                self.lex.next();
                let part = self.expect_ident()?;
                type_name.push('.');
                type_name.push_str(&part.text);
            }
            if self.peek_is(Tok::LParen) {
                self.skip_generic_params();
            }
            if let Some(r) = file.type_map.get(&type_name) {
                return Ok(r.clone());
            }
            // Forward reference: assume struct
            return Ok(CapnpTypeRef {
                tag: CapnpTypeTag::Struct,
                element_type_idx: -1,
                referenced_type_idx: -1,
            });
        }

        self.error(&t, "expected type")
    }

    fn encode_type_ref(r: &CapnpTypeRef) -> i32 {
        match r.tag {
            CapnpTypeTag::Struct | CapnpTypeTag::Enum => r.referenced_type_idx,
            _ => -(r.tag as i32 + 1),
        }
    }

    fn skip_default_value(&mut self) -> String {
        let t = self.lex.peek();
        if t.kind == Tok::LParen {
            let mut val = String::from("(");
            self.lex.next();
            let mut depth = 1;
            while depth > 0 && !self.peek_is(Tok::Eof) {
                let inner = self.lex.next();
                if inner.kind == Tok::LParen {
                    depth += 1;
                } else if inner.kind == Tok::RParen {
                    depth -= 1;
                }
                if depth > 0 {
                    val.push_str(&inner.text);
                }
            }
            val.push(')');
            return val;
        }
        let v = self.lex.next();
        v.text.clone()
    }

    fn skip_field_annotations(&mut self) {
        // Cap'n Proto field annotations look like $annotation(args)
        // Our lexer doesn't handle '$' so this is a no-op
    }

    fn skip_const(&mut self) -> Result<(), ParseError> {
        self.expect(Tok::KwConst)?;
        self.expect_ident()?;
        self.expect(Tok::At)?;
        self.expect(Tok::Number)?;
        self.expect(Tok::Colon)?;
        self.skip_type();
        self.expect(Tok::Equal)?;
        self.skip_default_value();
        self.expect(Tok::Semicolon)?;
        Ok(())
    }

    fn skip_annotation_decl(&mut self) {
        self.lex.next(); // consume 'annotation'
        while !self.peek_is(Tok::Semicolon) && !self.peek_is(Tok::Eof) {
            self.lex.next();
        }
        if self.peek_is(Tok::Semicolon) {
            self.lex.next();
        }
    }

    fn skip_interface(&mut self) -> Result<(), ParseError> {
        self.expect(Tok::KwInterface)?;
        self.expect_ident()?;
        if self.peek_is(Tok::At) {
            self.lex.next();
            self.expect(Tok::Number)?;
        }
        if self.peek_is(Tok::LBrace) {
            self.skip_braced_block();
        }
        Ok(())
    }

    fn skip_import(&mut self) -> Result<(), ParseError> {
        self.lex.next(); // consume 'import'
        // Skip to semicolon
        while !self.peek_is(Tok::Semicolon) && !self.peek_is(Tok::Eof) {
            self.lex.next();
        }
        if self.peek_is(Tok::Semicolon) {
            self.lex.next();
        }
        Ok(())
    }

    fn skip_type(&mut self) {
        self.lex.next();
        if self.peek_is(Tok::LParen) {
            self.skip_generic_params();
        }
        while self.peek_is(Tok::Dot) {
            self.lex.next();
            let _ = self.lex.next();
        }
    }

    fn skip_generic_params(&mut self) {
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

    fn skip_braced_block(&mut self) {
        let _ = self.lex.next(); // consume '{'
        let mut depth = 1;
        while depth > 0 && !self.peek_is(Tok::Eof) {
            let t = self.lex.next();
            if t.kind == Tok::LBrace {
                depth += 1;
            } else if t.kind == Tok::RBrace {
                depth -= 1;
            }
        }
    }
}

// =========================================================================
// Public API
// =========================================================================

/// Parse a `.capnp` schema source string into a [`ParsedCapnpFile`].
pub fn parse_capnp(input: &str) -> Result<ParsedCapnpFile, ParseError> {
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
    fn test_simple_struct() {
        let input = r#"
            @0xdeadbeef;
            struct Point {
                x @0 :Float64;
                y @1 :Float64;
            }
        "#;
        let file = parse_capnp(input).unwrap();
        assert_eq!(file.file_id, 0xdeadbeef);
        assert_eq!(file.structs.len(), 1);

        let point = &file.structs[0];
        assert_eq!(point.name, "Point");
        assert_eq!(point.fields.len(), 2);
        assert_eq!(point.fields[0].name, "x");
        assert_eq!(point.fields[0].type_ref.tag, CapnpTypeTag::Float64);
        assert_eq!(point.fields[1].name, "y");
        assert_eq!(point.fields[1].type_ref.tag, CapnpTypeTag::Float64);

        // Layout: two Float64 fields => 2 data words, 0 ptrs
        assert_eq!(point.data_words, 2);
        assert_eq!(point.ptr_count, 0);
        // x at offset 0, y at offset 8
        assert_eq!(point.fields[0].loc.offset, 0);
        assert_eq!(point.fields[1].loc.offset, 8);
    }

    #[test]
    fn test_struct_with_text() {
        let input = r#"
            @0x1;
            struct Token {
                kind   @0 :UInt16;
                offset @1 :UInt32;
                length @2 :UInt32;
                text   @3 :Text;
            }
        "#;
        let file = parse_capnp(input).unwrap();
        let token = &file.structs[0];
        assert_eq!(token.fields.len(), 4);
        assert_eq!(token.fields[0].name, "kind");
        assert_eq!(token.fields[0].type_ref.tag, CapnpTypeTag::UInt16);
        assert_eq!(token.fields[3].name, "text");
        assert_eq!(token.fields[3].type_ref.tag, CapnpTypeTag::Text);
        assert!(token.fields[3].loc.is_ptr);
        assert_eq!(token.ptr_count, 1);
    }

    #[test]
    fn test_enum() {
        let input = r#"
            @0x1;
            enum Color {
                red @0;
                green @1;
                blue @2;
            }
        "#;
        let file = parse_capnp(input).unwrap();
        assert_eq!(file.enums.len(), 1);
        assert_eq!(file.enums[0].name, "Color");
        assert_eq!(file.enums[0].enumerants, vec!["red", "green", "blue"]);
    }

    #[test]
    fn test_list_type() {
        let input = r#"
            @0x1;
            struct UserProfile {
                id    @0 :UInt64;
                name  @1 :Text;
                tags  @2 :List(Text);
            }
        "#;
        let file = parse_capnp(input).unwrap();
        let s = &file.structs[0];
        assert_eq!(s.fields.len(), 3);
        assert_eq!(s.fields[2].name, "tags");
        assert_eq!(s.fields[2].type_ref.tag, CapnpTypeTag::List);
    }

    #[test]
    fn test_nested_struct_ref() {
        let input = r#"
            @0x1;
            struct Inner {
                x @0 :Int32;
            }
            struct Outer {
                inner @0 :Inner;
                value @1 :UInt64;
            }
        "#;
        let file = parse_capnp(input).unwrap();
        assert_eq!(file.structs.len(), 2);

        let outer = &file.structs[1];
        assert_eq!(outer.fields[0].name, "inner");
        assert_eq!(outer.fields[0].type_ref.tag, CapnpTypeTag::Struct);
        assert_eq!(outer.fields[0].type_ref.referenced_type_idx, 0);
    }

    #[test]
    fn test_dynamic_schema_conversion() {
        let input = r#"
            @0x1;
            struct Point {
                x @0 :Float64;
                y @1 :Float64;
            }
        "#;
        let file = parse_capnp(input).unwrap();
        let schema = file.to_dynamic_schema("Point").unwrap();
        assert_eq!(schema.field_count(), 2);
        assert_eq!(schema.data_words, 2);
        assert_eq!(schema.ptr_count, 0);

        let x = schema.find_by_name("x").unwrap();
        assert_eq!(x.ty, DynamicType::F64);
        assert_eq!(x.offset, 0);
        assert!(!x.is_ptr);

        let y = schema.find_by_name("y").unwrap();
        assert_eq!(y.ty, DynamicType::F64);
        assert_eq!(y.offset, 8);
    }

    #[test]
    fn test_bench_schemas() {
        let input = include_str!("../../../../cpp/benchmarks/bench_schemas.capnp");
        let file = parse_capnp(input).unwrap();

        // Should have: Point, Token, UserProfile, LineItem, Order, SensorReading
        assert_eq!(file.structs.len(), 6);

        let point = file.find_struct("Point").unwrap();
        assert_eq!(point.fields.len(), 2);
        assert_eq!(point.data_words, 2);
        assert_eq!(point.ptr_count, 0);

        let token = file.find_struct("Token").unwrap();
        assert_eq!(token.fields.len(), 4);
        assert_eq!(token.ptr_count, 1); // text

        let profile = file.find_struct("UserProfile").unwrap();
        assert_eq!(profile.fields.len(), 8);
        // id(u64), name(text), email(text), bio(text), age(u32), score(f64),
        // tags(List(Text)), verified(bool)
        // Ptrs: name, email, bio, tags = 4
        assert_eq!(profile.ptr_count, 4);

        let sensor = file.find_struct("SensorReading").unwrap();
        assert_eq!(sensor.fields.len(), 18);

        let order = file.find_struct("Order").unwrap();
        assert_eq!(order.fields.len(), 5);

        // Verify DynamicSchema conversion for sensor reading
        let schema = file.to_dynamic_schema("SensorReading").unwrap();
        assert_eq!(schema.field_count(), 18);
        let ts = schema.find_by_name("timestamp").unwrap();
        assert_eq!(ts.ty, DynamicType::U64);
        let dev = schema.find_by_name("deviceId").unwrap();
        assert_eq!(dev.ty, DynamicType::Text);
        assert!(dev.is_ptr);
    }

    #[test]
    fn test_bool_layout() {
        let input = r#"
            @0x1;
            struct Flags {
                a @0 :Bool;
                b @1 :Bool;
                c @2 :Bool;
            }
        "#;
        let file = parse_capnp(input).unwrap();
        let s = &file.structs[0];
        assert_eq!(s.data_words, 1);
        assert_eq!(s.ptr_count, 0);
        // Bools should be packed into bits
        assert_eq!(s.fields[0].loc.offset, 0);
        assert_eq!(s.fields[0].loc.bit_index, 0);
        assert_eq!(s.fields[1].loc.offset, 0);
        assert_eq!(s.fields[1].loc.bit_index, 1);
        assert_eq!(s.fields[2].loc.offset, 0);
        assert_eq!(s.fields[2].loc.bit_index, 2);
    }

    #[test]
    fn test_using_alias() {
        let input = r#"
            @0x1;
            using MyInt = UInt32;
        "#;
        let file = parse_capnp(input).unwrap();
        assert_eq!(file.aliases.len(), 1);
        assert_eq!(file.aliases[0].name, "MyInt");
        assert_eq!(file.aliases[0].type_ref.tag, CapnpTypeTag::UInt32);
    }

    #[test]
    fn test_comments() {
        let input = r#"
            @0x1;
            # This is a comment
            struct Point {
                x @0 :Float64;  # x coordinate
                y @1 :Float64;  # y coordinate
            }
        "#;
        let file = parse_capnp(input).unwrap();
        assert_eq!(file.structs.len(), 1);
        assert_eq!(file.structs[0].fields.len(), 2);
    }

    #[test]
    fn test_union() {
        let input = r#"
            @0x1;
            struct Message {
                id @0 :UInt64;
                union {
                    text @1 :Text;
                    data @2 :Data;
                    empty @3 :Void;
                }
            }
        "#;
        let file = parse_capnp(input).unwrap();
        let msg = &file.structs[0];
        assert_eq!(msg.fields.len(), 1); // id
        assert_eq!(msg.unions.len(), 1);
        assert_eq!(msg.unions[0].alternatives.len(), 3);
        assert_eq!(msg.unions[0].alternatives[0].name, "text");
        assert_eq!(msg.unions[0].alternatives[1].name, "data");
        assert_eq!(msg.unions[0].alternatives[2].name, "empty");

        // Union DynamicSchema
        let schema = file.to_dynamic_schema("Message").unwrap();
        // Should have id + a variant field
        assert_eq!(schema.field_count(), 2);
    }

    #[test]
    fn test_default_values() {
        let input = r#"
            @0x1;
            struct Config {
                timeout @0 :UInt32 = 30;
                name @1 :Text = "default";
            }
        "#;
        let file = parse_capnp(input).unwrap();
        let s = &file.structs[0];
        assert_eq!(s.fields[0].default_value, "30");
        assert_eq!(s.fields[1].default_value, "\"default\"");
    }
}
