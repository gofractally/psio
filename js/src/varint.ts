// varint.ts — Variable-length integer encoding (VLQ and ZigZag)

import { FracpackError, Writer, Reader } from './fracpack.js';

/** Encode a u32 as variable-length quantity (base-128, little-endian). */
export function encodeVaruint32(value: number, w: Writer): void {
  value = value >>> 0; // ensure unsigned
  while (value >= 0x80) {
    w.writeU8((value & 0x7F) | 0x80);
    value >>>= 7;
  }
  w.writeU8(value);
}

/** Decode a varuint32 from the reader. */
export function decodeVaruint32(r: Reader): number {
  let result = 0;
  let shift = 0;
  for (;;) {
    const b = r.readU8();
    result |= (b & 0x7F) << shift;
    if ((b & 0x80) === 0) break;
    shift += 7;
    if (shift > 28) throw new FracpackError('Varuint32 too long');
  }
  return result >>> 0;
}

/** Encode an i32 using ZigZag + VLQ. */
export function encodeVarint32(value: number, w: Writer): void {
  // ZigZag transform: (value << 1) ^ (value >> 31)
  const zigzag = ((value << 1) ^ (value >> 31)) >>> 0;
  encodeVaruint32(zigzag, w);
}

/** Decode a varint32 (ZigZag + VLQ). */
export function decodeVarint32(r: Reader): number {
  const zigzag = decodeVaruint32(r);
  return (zigzag >>> 1) ^ -(zigzag & 1);
}

/** Pack a varuint32 to bytes. */
export function packVaruint32(value: number): Uint8Array {
  const w = new Writer(5);
  encodeVaruint32(value, w);
  return w.finish();
}

/** Unpack a varuint32 from bytes. */
export function unpackVaruint32(data: Uint8Array): number {
  const r = new Reader(data);
  return decodeVaruint32(r);
}

/** Pack a varint32 to bytes. */
export function packVarint32(value: number): Uint8Array {
  const w = new Writer(5);
  encodeVarint32(value, w);
  return w.finish();
}

/** Unpack a varint32 from bytes. */
export function unpackVarint32(data: Uint8Array): number {
  const r = new Reader(data);
  return decodeVarint32(r);
}
