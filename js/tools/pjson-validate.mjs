import { createInterface } from 'node:readline';
import { pjsonValidate } from '../dist/index.js';
for await (const line of createInterface({ input: process.stdin, crlfDelay: Infinity }))
  console.log(Number(pjsonValidate(Buffer.from(line, 'hex'))));
