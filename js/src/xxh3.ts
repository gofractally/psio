// xxh3.ts — XXH3-64 (seed 0, default secret), as pjson hashes object keys.
//
// A direct port of the reference algorithm in BigInt arithmetic. pjson keys
// are short, so no attempt is made at the speed of a native implementation.

const M64 = (1n << 64n) - 1n;
const P32_1 = 0x9e3779b1n;
const P32_2 = 0x85ebca77n;
const P32_3 = 0xc2b2ae3dn;
const P64_1 = 0x9e3779b185ebca87n;
const P64_2 = 0xc2b2ae3d27d4eb4fn;
const P64_3 = 0x165667b19e3779f9n;
const P64_4 = 0x85ebca77c2b2ae63n;
const P64_5 = 0x27d4eb2f165667c5n;
const MX1 = 0x165667919e3779f9n;
const MX2 = 0x9fb21c651e98df25n;

const SECRET_HEX =
  'b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671fcb79e64eccc0e578825ad07dccff7221' +
  'b8084674f743248ee03590e6813a264c3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8' +
  'a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364eac5ac8334d3ebc3c581a0fffa1363eb' +
  '170ddd51b7f0da49d316552629d4689e2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e';
const SECRET = new Uint8Array(192);
for (let i = 0; i < 192; ++i) SECRET[i] = parseInt(SECRET_HEX.slice(2 * i, 2 * i + 2), 16);

function le32(b: Uint8Array, at: number): bigint {
  return BigInt((b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0);
}
function le64(b: Uint8Array, at: number): bigint {
  return le32(b, at) | (le32(b, at + 4) << 32n);
}
function swap64(x: bigint): bigint {
  let out = 0n;
  for (let i = 0; i < 8; ++i) {
    out = (out << 8n) | (x & 0xffn);
    x >>= 8n;
  }
  return out;
}
const rotl = (x: bigint, n: bigint) => ((x << n) | (x >> (64n - n))) & M64;
function fold(a: bigint, b: bigint): bigint {
  const product = a * b;
  return (product & M64) ^ (product >> 64n);
}
function avalanche(h: bigint): bigint {
  h ^= h >> 37n;
  h = (h * MX1) & M64;
  return h ^ (h >> 32n);
}
function avalanche64(h: bigint): bigint {
  h ^= h >> 33n;
  h = (h * P64_2) & M64;
  h ^= h >> 29n;
  h = (h * P64_3) & M64;
  return h ^ (h >> 32n);
}
function mix16(input: Uint8Array, at: number, secretAt: number): bigint {
  return fold(le64(input, at) ^ le64(SECRET, secretAt), le64(input, at + 8) ^ le64(SECRET, secretAt + 8));
}

function accumulate(acc: bigint[], input: Uint8Array, at: number, secretAt: number): void {
  for (let i = 0; i < 8; ++i) {
    const value = le64(input, at + 8 * i);
    const key = value ^ le64(SECRET, secretAt + 8 * i);
    acc[i ^ 1] = (acc[i ^ 1] + value) & M64;
    acc[i] = (acc[i] + (key & 0xffffffffn) * (key >> 32n)) & M64;
  }
}

function long(input: Uint8Array): bigint {
  const len = input.length;
  const acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1];
  const stripesPerBlock = (192 - 64) / 8;
  const blockLength = 64 * stripesPerBlock;
  const blocks = Math.floor((len - 1) / blockLength);
  for (let n = 0; n < blocks; ++n) {
    for (let s = 0; s < stripesPerBlock; ++s) accumulate(acc, input, n * blockLength + s * 64, s * 8);
    for (let i = 0; i < 8; ++i) {
      let a = acc[i];
      a ^= a >> 47n;
      a ^= le64(SECRET, 192 - 64 + 8 * i);
      acc[i] = (a * P32_1) & M64;
    }
  }
  const stripes = Math.floor((len - 1 - blockLength * blocks) / 64);
  for (let s = 0; s < stripes; ++s) accumulate(acc, input, blocks * blockLength + s * 64, s * 8);
  accumulate(acc, input, len - 64, 192 - 64 - 7);
  let result = (BigInt(len) * P64_1) & M64;
  for (let i = 0; i < 4; ++i)
    result += fold(acc[2 * i] ^ le64(SECRET, 11 + 16 * i), acc[2 * i + 1] ^ le64(SECRET, 11 + 16 * i + 8));
  return avalanche(result & M64);
}

/** XXH3-64 of `input` with seed 0. */
export function xxh3(input: Uint8Array): bigint {
  const len = input.length;
  if (len === 0) return avalanche64(le64(SECRET, 56) ^ le64(SECRET, 64));
  if (len <= 3) {
    const combined = BigInt(((input[0] << 16) | (input[len >> 1] << 24) | input[len - 1] | (len << 8)) >>> 0);
    return avalanche64(combined ^ le32(SECRET, 0) ^ le32(SECRET, 4));
  }
  if (len <= 8) {
    const flip = le64(SECRET, 8) ^ le64(SECRET, 16);
    let h = (le32(input, len - 4) + (le32(input, 0) << 32n)) ^ flip;
    h ^= rotl(h, 49n) ^ rotl(h, 24n);
    h = (h * MX2) & M64;
    h ^= (h >> 35n) + BigInt(len);
    h = (h * MX2) & M64;
    return h ^ (h >> 28n);
  }
  if (len <= 16) {
    const low = le64(input, 0) ^ le64(SECRET, 24) ^ le64(SECRET, 32);
    const high = le64(input, len - 8) ^ le64(SECRET, 40) ^ le64(SECRET, 48);
    return avalanche((BigInt(len) + swap64(low) + high + fold(low, high)) & M64);
  }
  if (len <= 128) {
    let acc = BigInt(len) * P64_1;
    if (len > 32) {
      if (len > 64) {
        if (len > 96) acc += mix16(input, 48, 96) + mix16(input, len - 64, 112);
        acc += mix16(input, 32, 64) + mix16(input, len - 48, 80);
      }
      acc += mix16(input, 16, 32) + mix16(input, len - 32, 48);
    }
    acc += mix16(input, 0, 0) + mix16(input, len - 16, 16);
    return avalanche(acc & M64);
  }
  if (len <= 240) {
    let acc = BigInt(len) * P64_1;
    const rounds = Math.floor(len / 16);
    for (let i = 0; i < 8; ++i) acc += mix16(input, 16 * i, 16 * i);
    acc = avalanche(acc & M64);
    for (let i = 8; i < rounds; ++i) acc += mix16(input, 16 * i, 16 * (i - 8) + 3);
    acc += mix16(input, len - 16, 136 - 17);
    return avalanche(acc & M64);
  }
  return long(input);
}
