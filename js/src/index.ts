// psio — PSIO serialization for JavaScript and TypeScript.
//
// PSSZ (binary), key (order-preserving binary), JSON / I-JSON (text) and
// pjson, from a Schema IR fetched at run time or from types declared in
// TypeScript. See README.md.

export {
  // Schema-driven codec: PSSZ, key, JSON / I-JSON and views from a Schema IR
  PsszSchema,
  PsszType,
  PsszError,
  PsszNaN,
  PsszView,
  RecordView,
  TupleView,
  ListView,
  VariantView,
  IR_VERSION,
  jsonToPssz,
  psszToJson,
  validatePssz,
  keyPrefixUpperBound,
  type PsszKeyValidation,
  type PsszErrorKind,
  type PsszWidth,
  type PsszTypeOptions,
  type PsszSchemaOptions,
  type PsszCustomHandler,
  type PsszCustomHandlers,
  type PsszDecodeOptions,
  type PsszValidation,
  type PsszSchemaInput,
  type PsszTypeInput,
  type JsonFormat,
  type IrSchema,
  type IrAnyType,
  type IrMember,
  type IrAttribute,
  type IrTypeValue,
  type IrEnum,
  type IrEnumerator,
  type IrFunc,
  type IrResource,
  type IrInterface,
  type IrWorld,
  type IrUse,
  type CompactEnumerator,
  type CompactType,
  type Some,
  type VariantValue,
  type BitsValue,
} from './pssz.js';

export {
  // Typed definitions: declare a type, get its Schema IR and a typed codec
  TypeDef,
  Field,
  Alt,
  generateSchema,
  bool, char,
  u8, u16, u32, u64, u128, u256,
  i8, i16, i32, i64, i128,
  f32, f64,
  utf8, boundedUtf8, rawString,
  bytes, boundedBytes, fixedBytes,
  bitvector, bitlist,
  optional, list, boundedList, array, tuple,
  record, field, variant, alt,
  enumeration, timePoint, duration, monotonic, custom, box, lazy,
  type Infer,
  type InferView,
  type CustomHandler,
  type FieldOptions,
  type AltOptions,
  type RecordOptions,
  type LengthBoundOption,
  type IntText,
  type TypeDefOptions,
} from './types.js';

export {
  // Time values (docs/time.md): bigint ticks, exact Date / RFC 3339 conversions
  timePointToIso,
  timePointFromIso,
  timePointToDate,
  timePointFromDate,
  timeSchemaId,
  parseTimeSchemaId,
  reservedTimeId,
  timeFractionDigits,
  type TimeKind,
  type TimeUnit,
  type TickType,
} from './time.js';

export { toHex, fromHex } from './hex.js';

// WIT text of a schema (the port of C++ psio::emit_wit)
export { emitWit } from './wit.js';

export {
  // Account-name compression (a 64-bit number <-> a short name)
  nameToNumber,
  numberToName,
  hashName,
  isHashName,
  isCompressedName,
} from './compress-name.js';

export { PJSON_WIRE_REVISION, pjsonEncode, pjsonDecode, pjsonValidate, PjsonError, PjsonDecimal, PjsonFloat, PjsonString,
         PjsonTypedArray, PjsonRows, PjsonObject, type PjsonValue } from './pjson.js';
