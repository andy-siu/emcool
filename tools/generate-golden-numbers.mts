// Doubles of every shape, each with JavaScript's String() of it, for host/tests/check_text.cpp: emcool's
// JSON bodies must write numbers exactly as JSON.stringify does.
//
//   node tools/generate-golden-numbers.mts > <output file>
const lines: string[] = [];
const buffer = new DataView(new ArrayBuffer(8));
let state = 12345;
const next = () => { state = (state * 1103515245 + 12345) >>> 0; return state; };
const push = (value: number) => { if (Number.isFinite(value)) lines.push(`${Buffer.from(new Float64Array([value]).buffer).toString("hex")} ${String(value)}`); };
for (const value of [0, -0, 1, -1, 0.1, 1e21, 1e20, 123456789012345680000, 1e-7, 1e-6, 0.000001234, 5e-324, 1.7976931348623157e308, 2 ** 53, 2 ** 56, 2 ** 72, 72057594037927936, 39.21568627450981, 4294967295, -2147483648, 100, 1.5, 0.5, 1234.5678]) push(value);
for (let i = 0; i < 200000; i++) {
  buffer.setUint32(0, next()); buffer.setUint32(4, next());
  const shape = i % 4;
  if (shape === 0) push(buffer.getFloat64(0));
  else if (shape === 1) push((next() % 1000000) / (1 + (next() % 1000)));
  else if (shape === 2) push(Number(BigInt(next()) * BigInt(next()) * BigInt(next() % 100000)));
  else push((next() - 2147483648) * 10 ** ((next() % 60) - 30));
}
process.stdout.write(lines.join("\n") + "\n");
