// Drives the reference's own automatic-charge CONTROLLER — src/charge/auto.ts's startChargeAutomatic,
// the stateful shell around decideChargeCurrent — through a scripted sequence of signal changes and
// 60 s ticks on a fake clock, and records the charge currents it commanded and the signals it
// emitted (charge_auto_mode / reason / target_a). host/tests/check_charge_auto.cpp (loop section)
// replays the identical script through the C++ ChargeAutomatic and must match.
//
//   node tools/generate-golden-charge-auto-loop.mts <reference checkout> <output file>
//
// Lines:
//   scenario <n> <enabled 0|1>
//   rec <ms> <key> <value>            a signal the scenario records (an input to the controller)
//   mode <ms> <automatic|off>         a setMode call
//   note <ms> <amps> <manual 0|1>     a noteChargeCurrentOutgoing call
//   tick <ms>                         the 60 s tick fires (we advance the clock to <ms> and tick)
//   command <ms> <amps>               the controller called the sink with <amps> (an output)
//   emit <ms> <key> <value>           the controller record()'d a signal (an output)
//   end

import { registerHooks } from "node:module";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [referenceDirectory, outputPath] = process.argv.slice(2);
if (!referenceDirectory || !outputPath) {
  process.stderr.write("usage: node tools/generate-golden-charge-auto-loop.mts <reference checkout> <output file>\n");
  process.exit(2);
}
const referenceRoot = pathToFileURL(resolve(referenceDirectory)).href + "/";
const srcRoot = referenceRoot + "src/";

const fakeDirectory = await mkdtemp(join(tmpdir(), "emcool-auto-"));
const fakeMonotonicPath = join(fakeDirectory, "fake-monotonic.mjs");
await writeFile(
  fakeMonotonicPath,
  `export function monotonicNow() { return (globalThis).__now; }
export function since(mark) { return (globalThis).__now - mark; }
`
);
const fakeMonotonicUrl = pathToFileURL(fakeMonotonicPath).href;
registerHooks({
  resolve(specifier, context, nextResolve) {
    const result = nextResolve(specifier, context);
    const parent = context.parentURL ?? "";
    if (parent.startsWith(srcRoot) && result.url.startsWith(srcRoot + "monotonic.ts")) {
      return { url: fakeMonotonicUrl, shortCircuit: true, format: "module" };
    }
    if (parent.startsWith(srcRoot) && result.url.startsWith(srcRoot) && !result.url.includes("?")) {
      const tag = new URL(parent).searchParams.get("gen");
      if (tag !== null) return { ...result, url: `${result.url}?gen=${tag}` };
    }
    return result;
  },
});

const lines: string[] = [];
let now = 0;
const emit = (line: string) => void lines.push(line);
(globalThis as any).__now = 0;
Object.defineProperty(performance, "now", { value: () => now, configurable: true, writable: true });
Date.now = () => 1790000000000 + now;
console.log = console.warn = console.error = () => {};

// A single timer queue on the fake clock (the controller's setInterval).
interface Timer { at: number; seq: number; cb: () => void; cancelled: boolean; interval: number; }
let timers: Timer[] = [];
let seq = 0;
const coerce = (d: unknown) => { const a = Number(d); return a >= 1 && a <= 2_147_483_647 ? a : 1; };
globalThis.setInterval = ((cb: (...a: unknown[]) => void, delay?: number, ...a: unknown[]) => {
  const t: Timer & { unref(): unknown } = { at: now + coerce(delay), seq: seq++, cb: () => cb(...a), cancelled: false, interval: coerce(delay), unref: () => t };
  timers.push(t);
  return t as unknown as NodeJS.Timeout;
}) as unknown as typeof setInterval;
globalThis.setTimeout = ((cb: (...a: unknown[]) => void, delay?: number, ...a: unknown[]) => {
  const t: Timer & { unref(): unknown } = { at: now + coerce(delay), seq: seq++, cb: () => cb(...a), cancelled: false, interval: 0, unref: () => t };
  timers.push(t);
  return t as unknown as NodeJS.Timeout;
}) as unknown as typeof setTimeout;
globalThis.clearInterval = ((t: Timer | undefined) => { if (t) t.cancelled = true; }) as typeof clearInterval;
globalThis.clearTimeout = globalThis.clearInterval as unknown as typeof clearTimeout;

const realSetImmediate = globalThis.setImmediate;
const drain = async () => { for (let t = 0; t < 6; t++) await new Promise<void>(done => realSetImmediate(done)); };

// Fire due timers up to `toMs`, re-arming intervals, as the loop would.
async function advanceTo(toMs: number): Promise<void> {
  while (true) {
    while (timers[0]?.cancelled) timers.shift();
    timers.sort((a, b) => a.at - b.at || a.seq - b.seq);
    const due = timers[0];
    if (!due || due.at > toMs) break;
    now = due.at;
    (globalThis as any).__now = now;
    timers.shift();
    due.cb();
    if (due.interval > 0 && !due.cancelled) {
      due.at = now + due.interval;
      due.seq = seq++;
      timers.push(due);
    }
    await drain();
  }
  now = toMs;
  (globalThis as any).__now = now;
}

let generation = 0;
const importFresh = (path: string) => import(`${referenceRoot}${path}?gen=${generation}`);

interface Step {
  at: number;
  run: (ctx: { signals: any; automatic: any }) => void | Promise<void>;
  tick?: boolean;
}

async function runScenario(index: number, enabled: boolean, steps: Step[]): Promise<void> {
  generation++;
  timers = [];
  seq = 0;
  now = 0;
  (globalThis as any).__now = 0;
  const signals = await importFresh("src/can/signals.ts");
  const auto = await importFresh("src/charge/auto.ts");

  emit(`scenario ${index} ${enabled ? 1 : 0}`);
  const automatic = auto.startChargeAutomatic(
    { commandChargeCurrent: async (amps: number) => { emit(`command ${now} ${amps}`); return { succeeded: true, message: "" }; } },
    { enabled }
  );
  await drain();
  const emitState = () => {
    const s = automatic.state();
    emit(`state ${now} ${s.mode === "automatic" ? 1 : 0} ${s.reason} ${s.commandedAmps ?? "null"}`);
  };
  emitState();

  steps.sort((a, b) => a.at - b.at);
  for (const step of steps) {
    await advanceTo(step.at);
    if (step.tick) {
      // The 60 s interval fires on its own as advanceTo passes its due time; a tickStep only
      // extends the scenario's clock. No input line and no state line — the C++ replays the
      // interval the same way, and the next real step's state line captures the result.
      continue;
    }
    await step.run({ signals, automatic });
    await drain();
    emitState();
  }
  automatic.stop();
  emit("end");
}

// --- scenarios --------------------------------------------------------------------------------
// Helpers to build a step that records a signal / sets mode / notes an outgoing command.
const recStep = (at: number, key: string, value: number): Step => ({
  at,
  run: ({ signals }) => {
    emit(`rec ${at} ${key} ${value}`);
    signals.record(key, value);
  },
});
const modeStep = (at: number, mode: "automatic" | "off"): Step => ({
  at,
  run: ({ automatic }) => {
    emit(`mode ${at} ${mode}`);
    automatic.setMode(mode);
  },
});
const noteStep = (at: number, amps: number, manual: boolean): Step => ({
  at,
  run: ({ automatic }) => {
    emit(`note ${at} ${amps} ${manual ? 1 : 0}`);
    automatic.noteChargeCurrentOutgoing(amps, manual ? "manual" : "automatic");
  },
});
const tickStep = (at: number): Step => ({ at, tick: true, run: () => {} });

async function main(): Promise<void> {
  // A continuous DC charge keeps the session/ceiling/temperature fresh: record them every 30 s so
  // they are inside the 5 s staleness window when the 60 s tick fires. `warmingCharge` builds that.
  const warmingCharge = (celsius: (at: number) => number, untilMs: number): Step[] => {
    const steps: Step[] = [];
    for (let at = 1000; at <= untilMs; at += 4000) {
      steps.push(recStep(at, "charge_manager_state", 0x23));
      steps.push(recStep(at, "fast_dc_limit_max_a", 75));
      steps.push(recStep(at, "batt_temp_hi", celsius(at)));
    }
    return steps;
  };

  // 1. A warming DC charge: history builds; ticks command the current down as it nears 54 °C.
  await runScenario(0, true, [
    ...warmingCharge(at => 48 + (at / 60000) * 1.0, 720000),
    tickStep(660000),
    tickStep(720000),
  ]);

  // 2. Rider sets a different current mid-charge → standdown (reason RIDER thereafter).
  await runScenario(1, true, [
    ...warmingCharge(at => 45 + (at / 60000) * 0.2, 660000),
    recStep(120000, "dc_charge_limit_selected_a", 62),  // not the ceiling, not our last-sent → rider
    tickStep(660000),
  ]);

  // 3. Disabled: setMode ignored, no commands, reason stays DISABLED.
  await runScenario(2, false, [
    recStep(1000, "charge_manager_state", 0x23),
    recStep(1000, "fast_dc_limit_max_a", 75),
    recStep(1000, "batt_temp_hi", 56),
    modeStep(2000, "automatic"),
    tickStep(60000),
  ]);

  // 4. Session ends mid-charge → forget (samples cleared, reason falls back).
  await runScenario(3, true, [
    ...warmingCharge(at => 53 + (at / 60000) * 0.3, 240000),
    recStep(244000, "charge_manager_state", 0),  // session drops → forgetSession
    recStep(248000, "batt_temp_hi", 40),
    tickStep(300000),
  ]);

  // 5. A manual note mid-charge stands down.
  await runScenario(4, true, [
    ...warmingCharge(at => 45 + (at / 60000) * 0.2, 660000),
    noteStep(120000, 50, true),
    tickStep(660000),
  ]);

  // 6. Hot arrival, no history → blind descent at the first tick once above target.
  await runScenario(5, true, [
    ...warmingCharge(() => 56, 120000),
    tickStep(120000),
  ]);
  await writeFile(outputPath, lines.join("\n") + "\n");
  process.stderr.write(`golden-charge-auto-loop: ${lines.filter(l => l.startsWith("scenario")).length} scenarios, ${lines.filter(l => l.startsWith("command ")).length} commands\n`);
}

await main();
