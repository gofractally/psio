// pssz-view.ts — read views over PSSZ bytes.
//
// A view reads a value in place: a record view finds one field from the
// record's header and slots, a list view one element from its offset table,
// and nothing else is touched or allocated. Scalars and text are returned as
// values; bytes and raw strings as subarrays of the input (no copy); records,
// tuples, lists and variants as further views.
//
// A view checks what it reads and nothing more: every frame on the path to
// an accessed value (header, slot, offset, selector, span) and the value
// itself, with the same errors as `decode`. It does not see the fields it is
// not asked for, so rules that need the whole value (trailing optionals
// pruned, member length bounds, unknown-field policy) are those of
// `validate` / `decode`; `toValue()` applies them to the subtree it decodes.
//
// PSSZ has no in-place mutation: an encoding is canonical, offset widths are
// derived from the type, and trailing absent optionals are pruned, so
// changing a value means decode -> modify -> encode.
//
// This file is internal. The public API is `pssz.ts`.

import { Codec, Node, PsszError, wrapSome, type DecodeContext } from './pssz-core.js';

function bytesError(message: string): never {
  throw new PsszError('bytes', message);
}
function valueError(message: string): never {
  throw new PsszError('value', message);
}

/** A composite value read in place. `T` is the type of the decoded value. */
export abstract class PsszView<T = unknown> {
  /** @internal */
  constructor(
    /** @internal */ protected readonly codec: Codec,
    /** @internal */ protected readonly node: Node,
    /** @internal */ protected readonly input: Uint8Array,
    /** @internal */ protected readonly start: number,
    /** @internal */ protected readonly end: number,
    /** @internal */ protected readonly depth: number,
  ) {}

  /** The encoding of this value: a subarray of the input, not a copy. */
  get bytes(): Uint8Array {
    return this.input.subarray(this.start, this.end);
  }

  /**
   * Decodes this value in full, with everything `decode` checks. Unknown
   * appended optional fields are dropped unless `rejectUnknown`.
   */
  toValue(options?: { rejectUnknown?: boolean }): T {
    const ctx: DecodeContext = { allowUnknown: !options?.rejectUnknown, hasUnknown: false };
    return guarded(() => this.codec.decodeAt(this.node, this.input, this.start, this.end, this.depth, ctx)) as T;
  }
}

function guarded<R>(run: () => R): R {
  try {
    return run();
  } catch (e) {
    if (e instanceof PsszError) throw e;
    throw new PsszError('internal', e instanceof Error ? `${e.name}: ${e.message}` : String(e));
  }
}

type Spans = ReturnType<Codec['recordSpans']>;

abstract class MembersView<T> extends PsszView<T> {
  private spans?: Spans;

  /** @internal The record frame, parsed once. */
  protected frame(): Spans {
    if (this.spans) return this.spans;
    return guarded(() => {
      const { codec, node } = this;
      const record = node.kind === 'record';
      if (record && codec.overCap(node, this.end - this.start))
        bytesError('pssz IR: encoded size exceeds maxDynamicData cap');
      const w = codec.widthOf(node);
      return (this.spans = codec.recordSpans(
        this.input, this.start, this.end, w, record && node.stable, codec.fieldLayouts(node, w),
      ));
    });
  }

  /** @internal Member `i`: null when the writer did not store it. */
  protected member(i: number): unknown {
    const spans = this.frame();
    if (!spans.present[i]) return null;
    return viewAt(this.codec, this.node.members[i].node, this.input, spans.begins[i], spans.ends[i], this.depth + 1);
  }
}

/**
 * A record read in place. `F` maps each field name to what `get` returns for
 * it: a value for a scalar or text, a view for a composite.
 */
export class RecordView<T = unknown, F = Record<string, unknown>> extends MembersView<T> {
  /** One field. Only the record's header and this field are read. */
  get<K extends keyof F & string>(name: K): F[K] {
    const index = this.node.byName!.get(name);
    if (index === undefined) return valueError(`pssz view: no field "${name}"`);
    return this.member(index) as F[K];
  }

  /** The field names of the type, in declaration order. */
  get fields(): string[] {
    return this.node.members.map((m) => m.name);
  }

  /** The writer appended optional fields this type does not know. */
  get hasUnknown(): boolean {
    return this.frame().unknown;
  }
}

/** A tuple read in place. `E` is the tuple of what `at` returns per position. */
export class TupleView<T = unknown, E extends readonly unknown[] = unknown[]> extends MembersView<T> {
  /** The number of elements of the type. */
  get length(): number {
    return this.node.members.length;
  }

  /** One element. */
  at<I extends number>(index: I): E[I] {
    if (!Number.isInteger(index) || index < 0 || index >= this.node.members.length)
      return valueError('pssz view: tuple index out of range');
    return this.member(index) as E[I];
  }
}

/** A list or array read in place. `E` is what `at` returns for an element. */
export class ListView<T = unknown, E = unknown> extends PsszView<T> {
  private count = -1;
  private elementSize = -1; // fixed element size; -1: offset table
  private w = 0;

  private measure(): number {
    if (this.count >= 0) return this.count;
    return guarded(() => {
      const { codec, node } = this;
      const size = this.end - this.start;
      const w = codec.widthOf(node);
      const info = codec.layout(node.child);
      let count: number;
      if (node.exact) {
        if (!Number.isFinite(node.countNum)) bytesError('pssz IR: array count overflow');
        count = node.countNum;
      } else if (size === 0) count = 0;
      else if (info.fixed) {
        if (!info.size || size % info.size) bytesError('pssz IR: fixed list span mismatch');
        count = size / info.size;
      } else {
        if (w > size) bytesError('pssz IR: offset underrun');
        const first = codec.readOffset(this.input, this.start, w);
        if (!first || first % w || first > size) bytesError('pssz IR: invalid list offset table');
        count = first / w;
      }
      if (node.bounded && (node.exact ? count !== node.countNum : count > node.countNum))
        bytesError('pssz IR: sequence bound exceeded');
      if (info.fixed) {
        if (count * info.size !== size) bytesError('pssz IR: fixed sequence span mismatch');
      } else if (count > Math.floor(size / w) || (!count && size)) bytesError('pssz IR: list table span mismatch');
      this.w = w;
      this.elementSize = info.fixed ? info.size : -1;
      return (this.count = count);
    });
  }

  /** The number of elements, from the frame (no element is read). */
  get length(): number {
    return this.measure();
  }

  /** One element. Only its offset-table entries and the element are read. */
  at(index: number): E {
    const count = this.measure();
    if (!Number.isInteger(index) || index < 0 || index >= count)
      return valueError('pssz view: list index out of range');
    return guarded(() => {
      const { codec, input, start, w } = this;
      const element = this.node.child;
      if (this.elementSize >= 0) {
        const at = start + index * this.elementSize;
        return viewAt(codec, element, input, at, at + this.elementSize, this.depth + 1) as E;
      }
      const size = this.end - start;
      const begin = codec.readOffset(input, start + index * w, w);
      const stop = index + 1 < count ? codec.readOffset(input, start + (index + 1) * w, w) : size;
      if (begin < count * w || begin > stop || stop > size) bytesError('pssz IR: invalid list offsets');
      return viewAt(codec, element, input, start + begin, start + stop, this.depth + 1) as E;
    });
  }

  *[Symbol.iterator](): IterableIterator<E> {
    const count = this.measure();
    for (let i = 0; i < count; ++i) yield this.at(i);
  }
}

/**
 * A variant read in place: `type` is the name of the stored alternative,
 * `value` what it holds (a value or a view).
 */
export class VariantView<T = unknown, K extends string = string, V = unknown> extends PsszView<T> {
  private selector(): number {
    if (this.end === this.start) bytesError('pssz IR: variant selector missing');
    const selector = this.input[this.start];
    if (selector >= this.node.members.length) bytesError('pssz IR: variant selector out of range');
    return selector;
  }

  /** The index of the stored alternative. */
  get index(): number {
    return this.selector();
  }

  get type(): K {
    return this.node.members[this.selector()].name as K;
  }

  get value(): V {
    const member = this.node.members[this.selector()];
    return viewAt(this.codec, member.node, this.input, this.start + 1, this.end, this.depth + 1) as V;
  }
}

/** What a view returns for the value of `node` at `[start, end)`. */
export function viewAt(codec: Codec, node: Node, bytes: Uint8Array, start: number, end: number, n: number): unknown {
  return guarded(() => {
    codec.checkDepth(n, bytesError);
    switch (node.kind) {
      case 'record':
        return new RecordView(codec, node, bytes, start, end, n);
      case 'tuple':
        return new TupleView(codec, node, bytes, start, end, n);
      case 'list':
      case 'array':
        return new ListView(codec, node, bytes, start, end, n);
      case 'variant':
        return new VariantView(codec, node, bytes, start, end, n);
      case 'option': {
        const size = end - start;
        if (codec.canBeEmpty(node.child)) {
          if (size === 0) bytesError('pssz IR: optional selector missing');
          if (bytes[start] === 0) {
            if (size !== 1) bytesError('pssz IR: trailing None bytes');
            return null;
          }
          if (bytes[start] !== 1) bytesError('pssz IR: invalid optional selector');
          ++start;
        } else if (size === 0) return null;
        return wrapSome(node, viewAt(codec, node.child, bytes, start, end, n + 1));
      }
      case 'hex':
        codec.boundBytes(node.child, end - start, bytesError);
        if (end > start) codec.checkDepth(n + 1, bytesError);
        return bytes.subarray(start, end);
      case 'string':
        if (node.raw) {
          codec.boundBytes(node.child, end - start, bytesError);
          return bytes.subarray(start, end);
        }
        break;
      case 'custom':
        return viewAt(codec, codec.wireOf(node).node, bytes, start, end, n);
      default:
        break;
    }
    // Scalars, text and bit types: the value itself.
    return codec.decodeAt(node, bytes, start, end, n, { allowUnknown: true, hasUnknown: false });
  });
}
