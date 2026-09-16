// compress-name.ts — Arithmetic coding name compression
//
// Ported from C++ psio1::detail::compress_name.hpp
// Compresses short lowercase names into 64-bit values using context-aware
// frequency tables. Falls back to seahash for names that don't compress.

// prettier-ignore
const SYMBOL_TO_CHAR = [
  0, 101, 97, 105, 111, 116, 110, 114, 115,  // \0 e a i o t n r s
  108, 99, 117, 104, 100, 112, 109, 121, 103, // l c u h d p m y g
  98, 102, 119, 118, 107, 122, 120, 113, 106  // b f w v k z x q j
];

// prettier-ignore
const CHAR_TO_SYMBOL = new Uint8Array([
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,0,0,0,0,0,0,
  0,2,18,10,13,1,19,17,12,3,26,22,9,15,6,4,14,25,7,8,5,11,21,20,24,16,23,
  0,0,0,0,25,0,2,18,10,13,1,19,17,12,3,26,22,9,15,6,4,14,25,7,8,5,11,21,20,24,16,23,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
]);

// prettier-ignore
const MODEL_CF: readonly (readonly number[])[] = [
  [0,0,1956,4146,6063,7423,9305,10276,11226,13687,14907,16131,17056,18369,19922,21644,23210,24604,25065,26398,28475,30408,31395,31725,32367,32402,32623,32767],
  [0,4880,5243,6669,7714,8472,9634,11619,15233,16738,18031,18650,18993,20913,22653,25217,25802,26404,26589,28244,29312,31398,31845,32080,32183,32478,32695,32767],
  [0,1737,2901,3020,3666,3734,8543,12634,15523,17208,21367,22949,23407,24602,25562,26470,28001,28323,29045,30727,31040,31251,32084,32387,32496,32598,32745,32767],
  [0,1420,3454,4922,5132,6751,9959,13993,14547,18653,20482,23055,23598,23956,25067,25566,27966,27973,28483,29334,29844,30620,31685,31882,32375,32425,32757,32767],
  [0,4209,5209,5503,5994,6779,9809,13734,16394,17728,20734,21636,23733,24071,24785,26194,28482,28566,29403,30186,30894,31383,32100,32294,32382,32531,32744,32767],
  [0,6157,9213,10380,13294,15931,16600,16829,17987,18142,18574,18667,19055,21787,22406,24449,24772,25604,25663,29173,30781,31791,31995,32219,32350,32610,32748,32767],
  [0,4891,8913,10772,13035,15218,19118,19893,20080,21202,22114,23568,23895,24392,26546,26760,27744,28021,30534,31037,31376,31650,32250,32490,32548,32565,32709,32767],
  [0,4689,9532,12839,16144,19335,21427,21880,22583,23728,24811,25404,25957,26777,27443,28035,29213,30179,30481,31447,31666,31782,32303,32484,32510,32511,32751,32767],
  [0,5750,9495,10402,12127,13082,19468,19684,19710,21681,23775,24600,25576,26982,27084,27953,30214,30492,30515,30947,31019,31446,31709,31847,31884,31884,32761,32767],
  [0,1827,5418,7243,11598,13806,14484,14972,15432,15532,16785,16863,17441,18465,19298,23066,24626,26449,26523,27067,29601,31460,31683,32257,32266,32312,32318,32767],
  [0,3085,6902,11386,13498,18692,21202,21246,22818,22950,23980,24484,25962,30168,30176,30181,30391,31287,31288,31293,31294,31297,31592,32703,32712,32712,32767,32767],
  [0,1460,2488,3382,4190,4382,7997,13589,16710,21209,25118,26178,26204,26432,27181,28387,30216,30257,30864,31867,32177,32219,32374,32455,32551,32617,32754,32767],
  [0,3497,9469,11581,13649,15750,19606,19858,20362,20409,23728,23738,24110,26020,26170,26284,28241,29240,29247,29296,29442,29819,30442,30559,30604,30604,32763,32767],
  [0,7725,13656,15552,18212,19436,22322,22566,23251,23443,26247,26267,26704,26809,27460,27622,29567,30839,30969,31225,31379,31448,32114,32127,32143,32143,32739,32767],
  [0,3654,6677,7904,9941,12843,16232,16550,19915,20300,22842,22898,23406,25127,25246,25931,27743,28101,28153,28321,29252,31975,32243,32396,32480,32494,32721,32767],
  [0,2259,4242,6358,8676,10453,10663,11361,12343,12503,13022,13049,13568,15061,16673,21204,22114,23775,23801,26743,28089,29382,31518,31992,32482,32634,32711,32767],
  [0,8569,10926,11269,11458,12219,15419,15811,16168,19632,21864,22233,22272,23584,23897,24563,27086,27677,27838,30347,30897,31720,32305,32316,32374,32410,32766,32767],
  [0,5421,10228,13261,16435,18453,18538,19596,22477,22749,25180,25189,26615,28325,28370,28391,28704,29610,30232,30341,30376,30446,32750,32760,32764,32764,32765,32767],
  [0,3536,8233,9248,10667,11916,16198,16366,17262,17620,22341,22384,23103,23432,24178,24653,27955,28446,28462,29156,29914,30860,31440,31618,31718,31718,32711,32767],
  [0,6957,8777,9282,11114,12083,14679,14746,15172,16789,19423,19431,20076,20456,21353,21603,22831,23934,24047,24077,26986,30996,32258,32271,32421,32421,32764,32767],
  [0,4391,7270,8148,9018,9499,14426,14654,14752,18571,23671,23681,23696,24591,24714,24801,27775,27825,27830,30016,30215,30862,31659,31682,31823,31824,32740,32767],
  [0,508,5145,6952,9251,10376,10985,11140,11725,11743,12502,12513,12662,15602,17187,22455,22627,23556,23562,26519,30666,32564,32574,32641,32646,32708,32730,32767],
  [0,2561,9484,10526,12712,13144,24077,24752,24913,25311,29557,29584,29814,30171,30199,30250,31654,31986,32003,32134,32255,32412,32625,32686,32687,32687,32751,32767],
  [0,1902,10556,12815,13501,14918,18520,18600,18633,21631,26209,26218,26391,26794,26889,27833,30807,31246,31254,31774,32060,32183,32247,32252,32655,32655,32756,32767],
  [0,3478,6497,8308,12600,13818,17038,17122,17152,17528,20748,21906,22255,22599,22677,24481,25735,28777,28815,28891,29073,31075,31424,31433,31485,31485,32764,32767],
  [0,17596,18281,18455,19256,19875,20521,20599,20949,21038,21413,21416,23479,24932,25804,26610,28270,29555,29557,30312,31584,32451,32516,32525,32623,32701,32703,32767],
  [0,392,8308,10995,11504,13348,22180,22194,22231,22248,23981,23981,26646,26978,27000,27014,30409,30435,30435,30457,30474,30503,30697,30704,30818,30818,31861,32767],
];

const MODEL_WIDTH = 27;
const CODE_VALUE_BITS = 17; // (32 + 3) / 2 = 17
const MAX_CODE = (1 << CODE_VALUE_BITS) - 1;       // 0x1FFFF
const ONE_FOURTH = 1 << (CODE_VALUE_BITS - 2);     // 0x08000
const ONE_HALF = 2 * ONE_FOURTH;                    // 0x10000
const THREE_FOURTHS = 3 * ONE_FOURTH;               // 0x18000

// SeaHash — deterministic hash for fallback
function seahash(input: string): bigint {
  const p = 0x6eed0e9da4d94a4fn;

  const g = (x: bigint): bigint => {
    x = BigInt.asUintN(64, x * p);
    x = x ^ ((x >> 32n) >> (x >> 60n));
    x = BigInt.asUintN(64, x * p);
    return x;
  };

  const state = [
    0x16f11fe89b0d677cn,
    0xb480a793d8e6c86cn,
    0x6fe2e5aaf078ebc9n,
    0x14f994a4c5259381n,
  ];

  for (let i = 0; i < input.length; i += 8) {
    let n = 0n;
    const bytes = Math.min(input.length - i, 8);
    for (let j = 0; j < bytes; j++) {
      n += BigInt(input.charCodeAt(i + j)) << BigInt(j * 8);
    }
    const idx = (i >> 3) & 3;
    state[idx] = g(state[idx] ^ n);
  }

  return g(state[0] ^ state[1] ^ state[2] ^ state[3] ^ BigInt(input.length));
}

/** Compress a name string to a 64-bit value.
 *  Short lowercase names are arithmetically coded.
 *  Others fall back to seahash with a marker bit. */
export function nameToNumber(input: string): bigint {
  if (input.length === 0) return 0n;

  // Hash name decode: "#" + 16 chars
  if (input[0] === '#') {
    if (input.length !== 17) return 0xFFFFFFFFFFFFFFFFn; // -1 as u64
    let output = 0n;
    for (let i = 1; i < 17; i++) {
      const sym = BigInt(CHAR_TO_SYMBOL[input.charCodeAt(i)] - 1);
      output |= (sym & 0xFn) << BigInt(4 * (i - 1));
    }
    return output;
  }

  // Check all chars are compressible
  for (let i = 0; i < input.length; i++) {
    if (!CHAR_TO_SYMBOL[input.charCodeAt(i)]) return 0n;
  }

  // Arithmetic encoding
  let lastByte = CHAR_TO_SYMBOL[0]; // context state
  let inputIdx = 0;

  let bit = 0;
  let nextByte = 0;
  let mask = 0x80;
  let output = 0n;
  let bitc = 0;

  const putBit = (b: boolean) => {
    if (b && bitc === 63) {
      output = 0n;
      bit = 64;
    } else {
      bitc++;
      if (b) nextByte |= mask;
      mask >>= 1;
      if (!mask && bit < 64) {
        output |= BigInt(nextByte) << BigInt(bit);
        bit += 8;
        mask = 0x80;
        nextByte = 0;
      }
    }
  };

  const getByte = (): number => {
    if (inputIdx >= input.length) return 0;
    const c = CHAR_TO_SYMBOL[input.charCodeAt(inputIdx)];
    inputIdx++;
    return c;
  };

  let pendingBits = 0;
  let low = 0;
  let high = MAX_CODE;

  const putBitPlusPending = (b: boolean) => {
    putBit(b);
    while (pendingBits) { putBit(!b); pendingBits--; }
  };

  let c = 1;
  while (c !== 0 && bit < 64) {
    c = getByte();

    const cf = MODEL_CF[lastByte];
    const pLow = cf[c];
    const pHigh = cf[c + 1];
    const pCount = cf[MODEL_WIDTH];
    lastByte = c;

    if (pCount === 0) return 0n;

    const range = high - low + 1;
    high = low + Math.floor(range * pHigh / pCount) - 1;
    low  = low + Math.floor(range * pLow / pCount);

    while (bit < 64) {
      if (high < ONE_HALF) {
        putBitPlusPending(false);
      } else if (low >= ONE_HALF) {
        putBitPlusPending(true);
      } else if (low >= ONE_FOURTH && high < THREE_FOURTHS) {
        pendingBits++;
        low -= ONE_FOURTH;
        high -= ONE_FOURTH;
      } else {
        break;
      }
      high = ((high << 1) + 1) & MAX_CODE;
      low  = (low << 1) & MAX_CODE;
    }
  }

  pendingBits++;
  if (low < ONE_FOURTH) putBitPlusPending(false);
  else putBitPlusPending(true);

  if (mask !== 0x80 && bit < 64) {
    output |= BigInt(nextByte) << BigInt(bit);
  }

  if (inputIdx < input.length || c !== 0) output = 0n;

  // Fall back to hash if compression failed
  if (output === 0n) {
    output = seahash(input);
    output |= 1n << 56n;
  }

  return output;
}

/** Decompress a 64-bit value back to a name string. */
export function numberToName(input: bigint): string {
  if (input === 0n) return '';

  // Hash name: marker bit set at bit 56
  if ((input & (1n << 56n)) !== 0n) {
    let s = '#';
    let r = input;
    for (let i = 0; i < 16; i++) {
      s += String.fromCharCode(SYMBOL_TO_CHAR[(Number(r & 0xFn)) + 1]);
      r >>= 4n;
    }
    return s;
  }

  // Arithmetic decoding
  let lastByte = CHAR_TO_SYMBOL[0]; // context state
  let currentByte = 0;
  let lastMask = 1;
  let notEof = 64 + 16;

  const getBit = (): boolean => {
    if (lastMask === 1) {
      currentByte = Number(input & 0xFFn);
      input >>= 8n;
      lastMask = 0x80;
    } else {
      lastMask >>= 1;
    }
    notEof--;
    return (currentByte & lastMask) !== 0;
  };

  let out = '';
  let high = MAX_CODE;
  let low = 0;
  let value = 0;

  for (let i = 0; i < CODE_VALUE_BITS && notEof; i++) {
    value = (value << 1) + (getBit() ? 1 : 0);
  }

  while (notEof) {
    const range = high - low + 1;
    if (range === 0) return out;

    const cf = MODEL_CF[lastByte];
    const count = cf[MODEL_WIDTH];
    const scaledValue = Math.floor(((value - low + 1) * count - 1) / range);

    // Find symbol
    let c = 0;
    for (let i = 0; i < MODEL_WIDTH; i++) {
      if (scaledValue < cf[i + 1]) {
        c = i;
        break;
      }
    }

    const pLow = cf[c];
    const pHigh = cf[c + 1];
    const pCount = cf[MODEL_WIDTH];
    lastByte = c;

    if (c === 0) break; // null terminator
    out += String.fromCharCode(SYMBOL_TO_CHAR[c]);

    if (pCount === 0) return out;

    high = low + Math.floor(range * pHigh / pCount) - 1;
    low  = low + Math.floor(range * pLow / pCount);

    while (notEof) {
      if (high < ONE_HALF) {
        // do nothing
      } else if (low >= ONE_HALF) {
        value -= ONE_HALF;
        low -= ONE_HALF;
        high -= ONE_HALF;
      } else if (low >= ONE_FOURTH && high < THREE_FOURTHS) {
        value -= ONE_FOURTH;
        low -= ONE_FOURTH;
        high -= ONE_FOURTH;
      } else {
        break;
      }
      low = (low << 1);
      high = (high << 1) + 1;
      value = (value << 1) + (getBit() ? 1 : 0);
    }
  }

  return out;
}

/** Check if a 64-bit name value uses hash encoding (not compressed). */
export function isHashName(h: bigint): boolean {
  return (h & (1n << 56n)) !== 0n;
}

/** Check if a 64-bit name value was successfully compressed. */
export function isCompressedName(h: bigint): boolean {
  return !isHashName(h);
}

/** Public alias: compress a name string to a 64-bit value. */
export const hashName = nameToNumber;
