// psio — JavaScript/TypeScript serialization library
//
// Schema-first fracpack binary format with full TypeScript type inference,
// zero-copy views, and cross-language compatibility with C++ and Rust.

export {
  // Core types
  FracType,
  StructType,
  RawFieldResult,
  Infer,
  SchemaNode,
  FracpackError,

  // Scalar types
  bool,
  u8, u16, u32, u64,
  i8, i16, i32, i64,
  f32, f64,

  // Variable-size types
  str,
  bytes,

  // Container constructors
  optional,
  vec,
  array,
  tuple,
  variant,
  map,

  // Struct constructors
  struct,
  fixedStruct,
} from './fracpack.js';

export {
  // Varint encoding
  encodeVaruint32,
  decodeVaruint32,
  encodeVarint32,
  decodeVarint32,
  packVaruint32,
  unpackVaruint32,
  packVarint32,
  unpackVarint32,
} from './varint.js';

export {
  // Name compression
  nameToNumber,
  numberToName,
  hashName,
  isHashName,
  isCompressedName,
} from './compress-name.js';

export {
  // Schema generation (AnyType format, compatible with Rust)
  type AnyType,
  type Schema,
  typeToAnyType,
  generateSchema,
} from './schema.js';

export {
  // WIT generation
  type WitFunc,
  type WitOptions,
  generateWit,
  typeToWitExpr,
  toKebabCase,
} from './wit.js';

export {
  // JSON conversion
  fracToJson,
  jsonToFrac,
  valueToJson,
  jsonToValue,
  toHex,
  fromHex,
} from './json.js';

export {
  // Validation
  type ValidationResult,
  validate,
} from './validate.js';

export {
  // In-place mutation
  spliceBuffer,
  patchOffset,
  MutView,
  type MutViewOptions,
} from './mutation.js';

export { PJSON_WIRE_REVISION, createPjson, PjsonError, PjsonDecimal, PjsonFloat, PjsonString,
         PjsonTypedArray, PjsonRows, PjsonObject, type PjsonValue } from './pjson.js';
