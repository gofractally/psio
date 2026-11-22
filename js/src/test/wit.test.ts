// emitWit: the cases of cpp/tests/emit_wit_tests.cpp on the same IR, and
// the typed layer's schema as WIT.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, PsszSchema, array, bool, boundedList, bytes, emitWit, enumeration, f32, f64, generateSchema, i32, list,
  optional, record, tuple, u32, u64, u8, utf8, variant, rawString, type IrSchema, type PsszTypeInput,
} from '../index.js';

const U8 = { Int: { bits: 8, isSigned: false } };
const full = (parts: Omit<Partial<IrSchema>, 'types'> & { types?: { name: string; type: PsszTypeInput }[] }): PsszSchema =>
  new PsszSchema({
    package: { name: '', version: '', attributes: [] }, interfaces: [], worlds: [], uses: [], types: [],
    ir_version: 1, ...parts,
  } as never);

test('emitWit: package + bare interface + funcs', () => {
  const text = emitWit(full({
    package: { name: 'wasi:clocks', version: '0.2.3', attributes: [] },
    interfaces: [{
      name: 'wall_clock', type_names: [], attributes: [],
      funcs: [{ name: 'now', params: [], result: { value: { type: 'Int', value: { bits: 64, isSigned: false } } }, attributes: [] }],
    }],
  }));
  assert.equal(text, 'package wasi:clocks@0.2.3;\n\ninterface wall-clock {\n  now: func() -> u64;\n}\n\n');
});

test('emitWit: record with primitive + list + option fields', () => {
  const text = emitWit(full({
    types: [{
      name: 'bag',
      type: {
        Object: {
          name: { Custom: { type: { List: U8 }, id: 'string' } },
          items: { List: { Int: { bits: 32, isSigned: false } } },
          count: { Option: { Int: { bits: 32, isSigned: true } } },
        },
      },
    }],
    interfaces: [{ name: 'bags', type_names: ['bag'], funcs: [], attributes: [] }],
  }));
  assert.equal(text, 'interface bags {\n  record bag {\n    name: string,\n    items: list<u32>,\n    count: option<s32>\n  }\n}\n\n');
});

test('emitWit: variant + resource', () => {
  const boolIr = { value: { type: 'Custom', value: { type: { value: { type: 'Int', value: { bits: 1, isSigned: false } } }, id: 'bool' } } };
  const text = emitWit(full({
    types: [
      { name: 'status', type: { Variant: { ok: { Tuple: [] }, err: { Int: { bits: 32, isSigned: false } } } } },
      {
        name: 'pollable',
        type: {
          value: {
            type: 'Resource',
            value: {
              name: 'pollable', attributes: [],
              methods: [
                { name: 'ready', params: [], result: boolIr, attributes: [] },
                { name: 'block', params: [], result: null, attributes: [] },
              ],
            },
          },
        } as never,
      },
    ],
    interfaces: [{ name: 'io', type_names: ['status', 'pollable'], funcs: [], attributes: [] }],
  }));
  assert.equal(text, [
    'interface io {', '  variant status {', '    ok,', '    err(u32)', '  }', '  resource pollable {',
    '    ready: func() -> bool;', '    block: func();', '  }', '}', '', '',
  ].join('\n'));
});

test('emitWit: world with imports and exports', () => {
  const text = emitWit(full({
    package: { name: 'wasi:cli', version: '0.2.3', attributes: [] },
    uses: [{ package: 'wasi:io', interface_name: 'poll', version: '0.2.3', items: [] }],
    worlds: [{ name: 'command', imports: [{ package: 'wasi:io', interface_name: 'poll' }], exports: ['environment'], attributes: [] }],
    interfaces: [{ name: 'environment', type_names: [], funcs: [], attributes: [] }],
  }));
  assert.equal(text, [
    'package wasi:cli@0.2.3;', '', 'use wasi:io/poll@0.2.3;', '', 'interface environment {', '}', '',
    'world command {', '  import wasi:io/poll;', '  export environment;', '}', '', '',
  ].join('\n'));
});

test('emitWit: the schema of typed definitions', () => {
  const Point = record('Point', { x: i32, y: i32 }, { fixed: true });
  const Shape = variant({ none: tuple(), circle: f64, polygon: list(Point) });
  const Drawing = record('DrawingFile', {
    title: utf8, raw_name: rawString, shapes: list(Shape), origin: optional(Point), tags: boundedList(utf8, 4),
    color: enumeration(u8, { red: 0, green: 1 }), scale: f32, blob: bytes, corners: array(Point, 2), ok: bool,
    pair: tuple(u32, u64),
  }, { maxDynamicData: 4096 });
  const text = emitWit(generateSchema({ Drawing, Shape }));
  assert.equal(text, [
    '@maxDynamicData(4096)',
    'record drawing-file {',
    '  title: string,',
    '  raw-name: string,',
    '  shapes: list</* TODO: inline variant not supported */>,',
    '  origin: option<point>,',
    '  tags: list<string>/* @psio:max=4 */,',
    '  color: u8,',
    '  scale: f32,',
    '  blob: list<u8>,',
    '  corners: /* TODO: fixed-size array */,',
    '  ok: bool,',
    '  pair: tuple<u32, u64>',
    '}',
    '@definitionWillNotChange',
    'record point {',
    '  x: s32,',
    '  y: s32',
    '}',
    'type drawing = drawing-file;',
    'type shape = /* TODO: inline variant not supported */;',
    '',
  ].join('\n'));
  // A schema that travelled gives the same text.
  assert.equal(emitWit(PsszSchema.decode(generateSchema({ Drawing, Shape }).encode())), text);
});

test('emitWit: a malformed envelope is a typed error', () => {
  assert.throws(() => emitWit({} as never), PsszError);
  assert.throws(() => emitWit(full({ interfaces: [{ name: 5 }] as never })), PsszError);
  assert.throws(() => emitWit(full({ worlds: ['x'] as never })), PsszError);
  // A cycle of aliases does not hang.
  const cyclic = full({ types: [{ name: '@0', type: { List: '@0' } }, { name: 'a', type: '@0' }] });
  assert.throws(() => emitWit(cyclic), (e) => e instanceof PsszError && e.kind === 'schema');
});
