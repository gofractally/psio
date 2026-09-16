#!/usr/bin/env python3
"""Install the packed npm artifact into an unrelated directory and run an ESM consumer."""
import json
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='psio-npm-consumer-') as temp:
    work = Path(temp)
    packed = subprocess.run(['npm','pack','--json','--pack-destination',temp],
                            cwd=root/'js', check=True, capture_output=True, text=True)
    archive = json.loads(packed.stdout)[0]
    assert not any('/test/' in f['path'] for f in archive['files'])
    (work/'package.json').write_text('{"private":true,"type":"module"}\n')
    subprocess.run(['npm','install','--ignore-scripts','--no-audit','--no-fund',
                    str(work/archive['filename'])], cwd=work, check=True)
    (work/'consumer.mjs').write_text('''
import assert from 'node:assert/strict';
import { struct, u32, str, createPjson, PJSON_WIRE_REVISION } from 'psio';
const record = struct({id:u32, name:str});
const value = {id:42, name:'Alice'};
assert.deepEqual(record.unpack(record.pack(value)), value);
const codec = await createPjson();
assert.equal(PJSON_WIRE_REVISION, 2);
assert.equal(Buffer.from(codec.encode(-5n)).toString('hex'),'35');
assert.equal(Buffer.from(codec.encode(5n)).toString('hex'),'25');
assert.equal(codec.decode(codec.encode(-1234567890123456789n)), -1234567890123456789n);
''')
    subprocess.run(['node','consumer.mjs'],cwd=work,check=True)
print('npm archive consumer passed')
