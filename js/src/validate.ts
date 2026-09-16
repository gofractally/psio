// validate.ts — Zero-copy validation of fracpack binary data
//
// Validates packed data against a type schema without unpacking.
// Returns 'valid', 'extended' (has unknown fields), or 'invalid'.

import type { FracType, SchemaNode } from './fracpack.js';

// ========================= Result type =========================

export type ValidationResult =
  | { status: 'valid' }
  | { status: 'extended'; message: string }
  | { status: 'invalid'; message: string };

// ========================= Validator =========================

class Validator {
  private data: Uint8Array;
  private dv: DataView;
  private end: number;
  extended = false;

  constructor(data: Uint8Array) {
    this.data = data;
    this.dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    this.end = data.length;
  }

  private check(pos: number, size: number): void {
    if (pos < 0 || pos + size > this.end) {
      throw new Error(`Read past end at ${pos}+${size} (buffer size ${this.end})`);
    }
  }

  /** Validate a top-level packed value. Returns position after consumed data. */
  validateTop(type: FracType<any>): void {
    const pos = this.validateAt(type.schema, type, 0);
    if (pos !== this.end) {
      throw new Error(`Extra data: ${this.end - pos} bytes after valid data`);
    }
  }

  /** Validate value at position, return position after consumed data. */
  private validateAt(schema: SchemaNode, type: FracType<any>, pos: number): number {
    switch (schema.kind) {
      case 'bool': {
        this.check(pos, 1);
        const v = this.dv.getUint8(pos);
        if (v > 1) throw new Error(`Invalid bool value: ${v}`);
        return pos + 1;
      }

      case 'int':
        this.check(pos, schema.bits / 8);
        return pos + schema.bits / 8;

      case 'float': {
        const size = schema.exp === 8 ? 4 : 8;
        this.check(pos, size);
        return pos + size;
      }

      case 'string': {
        this.check(pos, 4);
        const len = this.dv.getUint32(pos, true);
        this.check(pos + 4, len);
        // Validate UTF-8
        this.validateUtf8(pos + 4, len);
        return pos + 4 + len;
      }

      case 'bytes': {
        this.check(pos, 4);
        const len = this.dv.getUint32(pos, true);
        this.check(pos + 4, len);
        return pos + 4 + len;
      }

      case 'optional': {
        this.check(pos, 4);
        const offset = this.dv.getUint32(pos, true);
        if (offset === 1) return pos + 4; // None
        if (offset === 0) return pos + 4; // empty container
        const target = pos + offset;
        this.check(target, 0);
        return this.validateAt(schema.inner.schema, schema.inner, target);
      }

      case 'list': {
        this.check(pos, 4);
        const numBytes = this.dv.getUint32(pos, true);
        const elemSize = schema.element.fixedSize;
        if (numBytes % elemSize !== 0) {
          throw new Error(`List size ${numBytes} not divisible by element size ${elemSize}`);
        }
        const len = numBytes / elemSize;
        const fixedStart = pos + 4;
        this.check(fixedStart, numBytes);

        if (!schema.element.isVariableSize) {
          // Fixed-size elements: validate each inline
          for (let i = 0; i < len; i++) {
            this.validateAt(schema.element.schema, schema.element, fixedStart + i * elemSize);
          }
          return fixedStart + numBytes;
        }

        // Variable-size elements: follow offsets
        let endPos = fixedStart + numBytes;
        for (let i = 0; i < len; i++) {
          const fp = fixedStart + i * elemSize;
          const offset = this.dv.getUint32(fp, true);
          const target = fp + offset;
          if (target < endPos) throw new Error('Overlapping vector elements');
          endPos = this.validateAt(schema.element.schema, schema.element, target);
        }
        return endPos;
      }

      case 'array': {
        const elemSize = schema.element.fixedSize;
        const totalFixed = elemSize * schema.len;
        this.check(pos, totalFixed);

        if (!schema.element.isVariableSize) {
          for (let i = 0; i < schema.len; i++) {
            this.validateAt(schema.element.schema, schema.element, pos + i * elemSize);
          }
          return pos + totalFixed;
        }

        let endPos = pos + totalFixed;
        for (let i = 0; i < schema.len; i++) {
          endPos = this.validateEmbedded(schema.element.schema, schema.element, pos + i * elemSize, endPos);
        }
        return endPos;
      }

      case 'tuple': {
        this.check(pos, 2);
        const fixedSize = this.dv.getUint16(pos, true);
        const fixedStart = pos + 2;
        const heapPos = fixedStart + fixedSize;
        this.check(fixedStart, fixedSize);

        let endPos = heapPos;
        let fieldPos = fixedStart;
        for (let i = 0; i < schema.elements.length; i++) {
          const elem = schema.elements[i];
          if (fieldPos >= heapPos) {
            if (!elem.isOptional) throw new Error(`Non-optional tuple element ${i} missing`);
            continue;
          }
          endPos = this.validateEmbedded(elem.schema, elem, fieldPos, endPos);
          fieldPos += elem.fixedSize;
        }
        return endPos;
      }

      case 'variant': {
        this.check(pos, 5);
        const idx = this.dv.getUint8(pos);
        const size = this.dv.getUint32(pos + 1, true);
        const caseNames = Object.keys(schema.cases);
        if (idx >= caseNames.length) {
          throw new Error(`Variant index ${idx} out of range (${caseNames.length} cases)`);
        }
        const caseType = schema.cases[caseNames[idx]];
        this.check(pos + 5, size);
        this.validateAt(caseType.schema, caseType, pos + 5);
        return pos + 5 + size;
      }

      case 'struct': {
        this.check(pos, 2);
        const fixedSize = this.dv.getUint16(pos, true);
        const fixedStart = pos + 2;
        const heapPos = fixedStart + fixedSize;
        this.check(fixedStart, fixedSize);

        const fields = Object.values(schema.fields);
        let expectedFixed = 0;
        for (const f of fields) expectedFixed += f.fixedSize;

        if (fixedSize > expectedFixed) {
          this.extended = true; // Unknown fields present
        }

        let endPos = heapPos;
        let fieldPos = fixedStart;
        for (let i = 0; i < fields.length; i++) {
          const f = fields[i];
          if (fieldPos + f.fixedSize > heapPos) {
            if (!f.isOptional) break; // trailing optional
            continue;
          }
          endPos = this.validateEmbedded(f.schema, f, fieldPos, endPos);
          fieldPos += f.fixedSize;
        }
        return endPos;
      }

      case 'fixedStruct': {
        const fields = Object.values(schema.fields);
        let totalFixed = 0;
        for (const f of fields) totalFixed += f.fixedSize;
        this.check(pos, totalFixed);

        let endPos = pos + totalFixed;
        let fieldPos = pos;
        for (const f of fields) {
          endPos = this.validateEmbedded(f.schema, f, fieldPos, endPos);
          fieldPos += f.fixedSize;
        }
        return endPos;
      }

      case 'map': {
        // Map is encoded as vec(struct({ key: str, value: V }))
        // Validate as a list
        this.check(pos, 4);
        const numBytes = this.dv.getUint32(pos, true);
        const fixedStart = pos + 4;
        this.check(fixedStart, numBytes);
        // Defer to unpack for full validation (map entries are extensible structs)
        return fixedStart + numBytes;
      }
    }
  }

  /** Validate an embedded field at fixedPos, variable data at endPos. */
  private validateEmbedded(
    schema: SchemaNode, type: FracType<any>,
    fixedPos: number, endPos: number,
  ): number {
    if (!type.isVariableSize) {
      this.validateAt(schema, type, fixedPos);
      return endPos;
    }
    // Variable-size: read offset
    this.check(fixedPos, 4);
    const offset = this.dv.getUint32(fixedPos, true);
    if (offset === 0) return endPos; // empty container
    if (offset === 1 && type.isOptional) return endPos; // None
    const target = fixedPos + offset;
    if (target < endPos) throw new Error(`Offset ${offset} at ${fixedPos} points before heap`);
    // For optional types, the offset resolution is already done here;
    // validate the inner type directly to avoid double offset resolution.
    if (schema.kind === 'optional') {
      return this.validateAt(schema.inner.schema, schema.inner, target);
    }
    return this.validateAt(schema, type, target);
  }

  /** Validate UTF-8 encoding. */
  private validateUtf8(pos: number, len: number): void {
    let i = pos;
    const end = pos + len;
    while (i < end) {
      const b = this.data[i];
      if (b < 0x80) { i++; continue; }
      let need: number;
      if ((b & 0xe0) === 0xc0) need = 1;
      else if ((b & 0xf0) === 0xe0) need = 2;
      else if ((b & 0xf8) === 0xf0) need = 3;
      else throw new Error(`Invalid UTF-8 lead byte 0x${b.toString(16)} at ${i}`);
      if (i + 1 + need > end) throw new Error('Truncated UTF-8 sequence');
      for (let j = 1; j <= need; j++) {
        if ((this.data[i + j] & 0xc0) !== 0x80) {
          throw new Error(`Invalid UTF-8 continuation byte at ${i + j}`);
        }
      }
      i += 1 + need;
    }
  }
}

// ========================= Public API =========================

/**
 * Validate packed fracpack data against a type schema without unpacking.
 *
 * Returns:
 * - `{ status: 'valid' }` — data matches schema exactly
 * - `{ status: 'extended', message }` — valid but has unknown fields (forward-compatible)
 * - `{ status: 'invalid', message }` — data is malformed
 */
export function validate(type: FracType<any>, data: Uint8Array): ValidationResult {
  try {
    const v = new Validator(data);
    v.validateTop(type);
    if (v.extended) {
      return { status: 'extended', message: 'Unknown fields present' };
    }
    return { status: 'valid' };
  } catch (e) {
    return { status: 'invalid', message: (e as Error).message };
  }
}
