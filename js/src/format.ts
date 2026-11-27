// Formats as values (gofractally/psio#18).
//
// A type object has one method family per format (`T.encode`, `T.encodeKey`,
// `T.valueToJson`, ...). Code that is generic over the format takes one of
// these values instead and calls the same members whatever the format:
//
//     function store(format: Format, T: PsszType, value: unknown) {
//       return format.encodeChecked(T, value);
//     }
//     store(formats.pssz, Account, a);     // or formats.key, formats.json, ...
//
// The members are the operations every format has (C++ `psio::format`,
// `psio/format_concept.hpp`); what a format may lack is a capability:
// `view` is present only where the format reads in place.

/** What a format needs of a type: a `PsszType` (schema-driven) and a typed
 *  definition (`record(...)`, `u32`, ...) both have these members. */
export interface FormatType {
  encode(value: any): Uint8Array;
  encodeChecked(value: any): Uint8Array;
  decode(bytes: Uint8Array): unknown;
  validate(bytes: Uint8Array): { ok: boolean; error?: unknown };
  view(bytes: Uint8Array): unknown;
  encodeKey(value: any): Uint8Array;
  encodeKeyChecked(value: any): Uint8Array;
  decodeKey(bytes: Uint8Array): unknown;
  validateKey(bytes: Uint8Array): { ok: boolean; error?: unknown };
  jsonToValue(text: string | Uint8Array, format?: 'json' | 'ijson'): unknown;
  valueToJson(value: any, format?: 'json' | 'ijson'): string;
  valueToJsonChecked(value: any, format?: 'json' | 'ijson'): string;
}

/** The outcome of `Format.validate`. */
export interface FormatValidation {
  ok: boolean;
  error?: unknown;
}

export interface Format {
  /** The format's name, as C++ spells the tag (`psio::pssz` is `"pssz"`). */
  readonly name: 'pssz' | 'key' | 'json' | 'ijson';
  /** The encoding is UTF-8 text. */
  readonly text: boolean;
  /** The fast write. */
  encode(type: FormatType, value: unknown): Uint8Array;
  /** The validated write: the value is held against what the type declares. */
  encodeChecked(type: FormatType, value: unknown): Uint8Array;
  /** The validating read. */
  decode(type: FormatType, bytes: Uint8Array): unknown;
  /** Checks bytes against the format and the type. Never throws. */
  validate(type: FormatType, bytes: Uint8Array): FormatValidation;
  /** Reads in place. Present only for a format with views (pssz). */
  view?(type: FormatType, bytes: Uint8Array): unknown;
}

const utf8 = new TextEncoder();
const fromText = new TextDecoder('utf-8', { fatal: true });

function textFormat(name: 'json' | 'ijson'): Format {
  return {
    name,
    text: true,
    encode: (type, value) => utf8.encode(type.valueToJson(value, name)),
    encodeChecked: (type, value) => utf8.encode(type.valueToJsonChecked(value, name)),
    decode: (type, bytes) => type.jsonToValue(bytes, name),
    validate: (type, bytes) => {
      try {
        fromText.decode(bytes);
        type.jsonToValue(bytes, name);
        return { ok: true };
      } catch (error) {
        return { ok: false, error };
      }
    },
  };
}

const pssz: Format = {
  name: 'pssz',
  text: false,
  encode: (type, value) => type.encode(value),
  encodeChecked: (type, value) => type.encodeChecked(value),
  decode: (type, bytes) => type.decode(bytes),
  validate: (type, bytes) => {
    try {
      const st = type.validate(bytes);
      return st.ok ? { ok: true } : { ok: false, error: st.error };
    } catch (error) {
      return { ok: false, error };
    }
  },
  view: (type, bytes) => type.view(bytes),
};

const key: Format = {
  name: 'key',
  text: false,
  encode: (type, value) => type.encodeKey(value),
  encodeChecked: (type, value) => type.encodeKeyChecked(value),
  decode: (type, bytes) => type.decodeKey(bytes),
  validate: (type, bytes) => {
    try {
      const st = type.validateKey(bytes);
      return st.ok ? { ok: true } : { ok: false, error: st.error };
    } catch (error) {
      return { ok: false, error };
    }
  },
};

/** Every format of this package, as a value. */
export const formats: { readonly pssz: Format; readonly key: Format; readonly json: Format; readonly ijson: Format } = {
  pssz,
  key,
  json: textFormat('json'),
  ijson: textFormat('ijson'),
};
