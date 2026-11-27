// Line-oriented pjson validator used by tools/check-pjson-validation.py.
// One hex frame per line in; per line out: `invalid` (not a well-formed
// frame), or `valid <c> <hex>|noform` — <c> is 1 when the frame is the
// canonical one of its value, then the canonical frame, or `noform` when
// the value has none (text that is not UTF-8).
import { createInterface } from 'node:readline';
import { pjsonCanonicalize, pjsonLazy, pjsonValidate } from '../dist/index.js';
for await (const line of createInterface({ input: process.stdin, crlfDelay: Infinity })) {
  const bytes = Buffer.from(line, 'hex');
  if (!pjsonLazy.validate(bytes)) { console.log('invalid'); continue; }
  let made = 'noform';
  try { made = Buffer.from(pjsonCanonicalize(bytes)).toString('hex'); } catch {}
  console.log(`valid ${Number(pjsonValidate(bytes))} ${made}`);
}
