import { createInterface } from 'node:readline';
import { createPjson } from '../dist/index.js';
const codec = await createPjson();
for await (const line of createInterface({ input: process.stdin, crlfDelay: Infinity }))
  console.log(Number(codec.validate(Buffer.from(line, 'hex'))));
