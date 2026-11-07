// Copies cool-eva's phone dashboard (public/) into dashboard/, applying emcool's renames, so a sync
// is one command rather than a hand-merge of a few thousand lines.
//
//   node tools/sync-dashboard.mts <cool-eva checkout> [--check]
//
// What it renames is what the two ends must agree on and what a rider sees as the name:
//   - wire protocol, in lockstep with the firmware: the X-Cool-Eva request header and the
//     cool-eva/hello and cool-eva/session handshake salts (dashboard/README.md);
//   - the brand: "Cool Eva" in titles, the header and the manifest;
//   - phone-side storage keys and file names (cool-eva.* → emcool.*), which touch nothing on the bike.
// emcool's own files (README.md, CNAME, lib/hosted-origin.js, views/bike-console.js) are kept as they are.
// It keeps cool-eva's wording ("the Pi") and its native-app names (cooleva:, coolEvaNative), as the
// first copy did. Then it re-applies emcool's own differences, each a fixed replacement that must
// match exactly once, so a cool-eva change that moves one fails loudly here instead of shipping
// half-applied. --check fails if dashboard/ is not what this would write.

import { cp, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const emcoolRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const dashboardDirectory = join(emcoolRoot, "dashboard");

const [coolEvaDirectory, ...flags] = process.argv.slice(2);
if (!coolEvaDirectory) {
  console.error("usage: node tools/sync-dashboard.mts <cool-eva checkout> [--check]");
  process.exit(2);
}
const checkOnly = flags.includes("--check");
const publicDirectory = join(coolEvaDirectory, "public");

// Everywhere, in order.
const RENAMES: [string | RegExp, string][] = [
  [/"X-Cool-Eva"/g, '"X-Emcool"'],
  [/X-Cool-Eva/g, "X-Emcool"],
  [/"cool-eva\/hello"/g, '"emcool/hello"'],
  [/"cool-eva\/session"/g, '"emcool/session"'],
  [/cool-eva\.(?=[a-zA-Z-]+[:"`])/g, "emcool."],
  [/cool-eva-bluetooth-key\.txt/g, "emcool-bluetooth-key.txt"],
  [/Cool Eva/g, "emcool"],
];

// File-specific, after the renames; each `from` must occur exactly once.
interface Patch {
  file: string;
  from: string;
  to: string;
}

const PATCHES: Patch[] = [
  // The hosted origin: emcool's own exclusive one, never cool-eva's (docs/hosting.md).
  {
    file: "lib/ble-origin.js",
    from: `export const HOSTED_BLE_ORIGINS = [
  "https://cool-eva.athegen.com", // GitHub Pages custom domain, .github/workflows/pages.yml
];`,
    to: "export const HOSTED_BLE_ORIGINS = HOSTED_DASHBOARD_ORIGINS;",
  },
  {
    file: "lib/ble-origin.js",
    from: "// @ts-check\n",
    to: '// @ts-check\nimport { HOSTED_DASHBOARD_ORIGINS } from "./hosted-origin.js";\n',
  },
  // The ride log: emcool's card is plain text with no sealing step, and Bluetooth is slow, so the
  // download is the newest parts, and there is no /recording/flush to call first (docs/ble.md).
  {
    file: "views/sheet.js",
    from: `async function downloadCurrentRide(onProgress) {
  const response = await request("/recording/flush", { method: "POST", headers: { "X-Emcool": "recording" } });
  if (!response.ok) {
    throw new Error("The current recording could not be sealed for download.");
  }
  return download("/dl", "emcool.celog", onProgress);
}`,
    to: `async function downloadCurrentRide(onProgress) {
  return download(\`/dl?mb=\${DOWNLOAD_MB}\`, "emcool.log", onProgress);
}`,
  },
  {
    file: "views/sheet.js",
    from: "/** BLE-only: over wifi the browser's download manager shows progress, so this stays empty. */\nconst downloadProgress = van.state(\"\");",
    to: `/** BLE-only: over wifi the browser's download manager shows progress, so this stays empty. */
const downloadProgress = van.state("");

/**
 * How much of the newest ride log one download asks for. The bike sends its newest parts until
 * about this many megabytes (always at least one whole part), each cut where its records end:
 * Bluetooth 4.2 moves tens of kB/s, so this is a recent ride; the card is how to take everything.
 */
const DOWNLOAD_MB = 16;`,
  },
  {
    file: "views/sheet.js",
    from: "/** Downloads the sealed ride log, with its size known before you commit to it. */",
    to: "/** Downloads the newest part of the ride log, for tools/import-ride-log.mts. */",
  },
  {
    file: "views/sheet.js",
    from: `            return "⬇  Download ride log";`,
    to: `            return "⬇  Download recent ride log";`,
  },
  {
    file: "views/sheet.js",
    from: `            return "Ride recording unavailable — Settings";`,
    to: `            return "⚠  No ride log — no card in the bike";`,
  },
  {
    file: "views/sheet.js",
    from: "          return `⬇  Download ride log (${bytes(current.log.bytes)})`;",
    to:
      "          // `log.bytes` counts every part at its full size, zero-filled tails and all.\n" +
      "          return `⬇  Download recent ride log (up to ${DOWNLOAD_MB} MB of ${bytes(current.log.bytes)})`;",
  },
  {
    file: "views/sheet.js",
    from: `    // No caption under the button. The file count that used to sit here is still
    // in the status payload (\`log.files\`) and still correct — it was dropped for
    // screen space, not because it was wrong. The two facts it carried are worth
    // knowing and live in the code that owns them: a \`.celog\` holds hundreds or
    // thousands of segments, so the count moves a few times a day rather than as you
    // ride — a day file plus whatever was sealed before the clock could be believed
    // (src/http/status.ts, docs/ride-log-clock.md); and the log is unreadable without the laptop's
    // private key, but /dl authenticates nobody, so the ciphertext is pullable by
    // anyone on that wifi (src/http/download.ts, and docs/ride-log.md "What this does and
    // doesn't hide").`,
    to: `    // No caption under the button. The log on the card is plain text, not encrypted, and /dl
    // answers only inside the Bluetooth session, so only a phone holding the key can pull it
    // (docs/ble.md, docs/ride-log-format.md).`,
  },
  // Settings change through the bike's console commands: on the serial console, or here in Bike
  // console (views/bike-console.js, emcool's own), which runs them over Bluetooth (docs/setup.md).
  // These pages' own Save buttons are refused.
  {
    file: "views/settings-page.js",
    from: `export function SettingsPage() {
  return div(
    { class: "settings-page" },`,
    to: `export function SettingsPage() {
  return div(
    { class: "settings-page" },
    () =>
      destinationSection.val === "console"
        ? div()
        : SettingsCard(
            "Changed with the bike's console",
            "emcool's settings change through its console commands: in Bike console here, over Bluetooth, or on the serial console (idf.py monitor, or nc <bike> 23 over Wi-Fi). Save buttons on the other pages are refused.",
            div(
              { class: "settings-links" },
              button(
                { class: "settings-link", onclick: () => openDestination("settings", "console") },
                "Open Bike console →"
              ),
              a(
                {
                  href: "https://github.com/andy-siu/emcool/blob/main/docs/setup.md",
                  target: "_blank",
                  rel: "noopener",
                  class: "settings-link",
                },
                "Every setting and command →"
              )
            )
          ),`,
  },
  {
    file: "views/settings-page.js",
    from: `import { ScreenGeometry } from "./screen-geometry.js";
`,
    to: `import { ScreenGeometry } from "./screen-geometry.js";
import { BikeConsole } from "./bike-console.js";
`,
  },
  {
    file: "views/settings-page.js",
    from: `    case "access":
      return div(h2("Who can control my bike?"), AccessExplanation());`,
    to: `    case "access":
      return div(h2("Who can control my bike?"), AccessExplanation());
    case "console":
      return div(h2("Bike console"), BikeConsole());`,
  },
  // Bike console has its own Restart, which works; the page-wide one posts /settings/apply, refused.
  {
    file: "views/settings-page.js",
    from: `SettingsRestart(!["phone", "access"].includes(destinationSection.val))`,
    to: `SettingsRestart(!["phone", "access", "console"].includes(destinationSection.val))`,
  },
  {
    file: "lib/settings-map.js",
    from: `export const SETTINGS_MAP = [
`,
    to: `export const SETTINGS_MAP = [
  { key: "console", title: "Bike console", detail: "Every setting · Status report · Restart · Commands" },
`,
  },
];

const staging = await mkdtemp(join(tmpdir(), "emcool-dashboard-"));
try {
  await cp(publicDirectory, staging, { recursive: true });
  for (const path of await listFiles(staging)) {
    const name = relative(staging, path);
    if (name.startsWith("vendor/")) continue;
    if (!/\.(js|html|css|webmanifest)$/.test(name)) continue;
    let text = await readFile(path, "utf-8");
    for (const [pattern, replacement] of RENAMES) {
      text = text.replace(pattern, replacement);
    }
    await writeFile(path, text);
  }
  for (const patch of PATCHES) {
    const path = join(staging, patch.file);
    const text = await readFile(path, "utf-8");
    const occurrences = text.split(patch.from).length - 1;
    if (occurrences !== 1) {
      throw new Error(`${patch.file}: expected the patched text once, found it ${occurrences} times:\n${patch.from}`);
    }
    await writeFile(path, text.replace(patch.from, patch.to));
  }
  // emcool's own files, kept across a sync.
  for (const own of ["CNAME", "README.md", "lib/hosted-origin.js", "views/bike-console.js"]) {
    await cp(join(dashboardDirectory, own), join(staging, own));
  }

  const leftover = [];
  for (const path of await listFiles(staging)) {
    const name = relative(staging, path);
    if (name.startsWith("vendor/") || !name.endsWith(".js")) continue;
    const text = await readFile(path, "utf-8");
    if (/X-Cool-Eva|cool-eva\/(hello|session)|cool-eva-dashboard|cool-eva\.athegen\.com/.test(text)) leftover.push(name);
  }
  if (leftover.length > 0) {
    throw new Error(`cool-eva wire names survived the renames in: ${leftover.join(", ")}`);
  }

  const differences = await compareTrees(staging, dashboardDirectory);
  if (checkOnly) {
    if (differences.length > 0) {
      console.error(`dashboard: out of date with cool-eva public/ (${differences.join(", ")}); run tools/sync-dashboard.mts`);
      process.exit(1);
    }
    console.log("dashboard: up to date with cool-eva public/");
  } else {
    await rm(dashboardDirectory, { recursive: true, force: true });
    await cp(staging, dashboardDirectory, { recursive: true });
    console.log(`dashboard: ${differences.length} files changed from cool-eva public/`);
  }
} finally {
  await rm(staging, { recursive: true, force: true });
}

async function listFiles(directory: string): Promise<string[]> {
  const entries = await readdir(directory, { withFileTypes: true, recursive: true });
  return entries.filter(entry => entry.isFile()).map(entry => join(entry.parentPath, entry.name));
}

async function compareTrees(left: string, right: string): Promise<string[]> {
  const leftFiles = new Set((await listFiles(left)).map(path => relative(left, path)));
  const rightFiles = new Set(
    (await listFiles(right).catch((error: NodeJS.ErrnoException) => {
      if (error.code === "ENOENT") return [];
      throw error;
    })).map(path => relative(right, path))
  );
  const differences: string[] = [];
  for (const name of new Set([...leftFiles, ...rightFiles])) {
    if (!leftFiles.has(name) || !rightFiles.has(name)) {
      differences.push(name);
      continue;
    }
    const [leftText, rightText] = await Promise.all([readFile(join(left, name)), readFile(join(right, name))]);
    if (!leftText.equals(rightText)) differences.push(name);
  }
  return differences.sort();
}
