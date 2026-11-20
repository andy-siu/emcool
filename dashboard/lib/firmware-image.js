// @ts-check

// What the phone checks in an emcool.bin before sending a byte of it, and how it sends it. The bike
// checks the same again (components/emcool_update/src/firmware_update.cpp checkHeader), so this is
// for a quick, clear answer — "that's the bootloader", "that's the ESP32-S3 build" — not a guard.
// Plain ES module with no DOM, so tools/check-ble.mts runs it against the C++ server.

const IMAGE_MAGIC = 0xe9;
const APP_DESC_MAGIC = 0xabcd5432;
// The image header (24 bytes), the first segment's header (8), then esp_app_desc_t.
const DESCRIPTION = 24 + 8;
export const HEADER_BYTES = DESCRIPTION + 128;
/** The app slot's size (partitions.csv's ota_0 and ota_1). */
export const SLOT_BYTES = 0x3e0000;

/** @param {number} chip */
export function chipName(chip) {
  return chip === 0 ? "ESP32" : chip === 9 ? "ESP32-S3" : `chip id ${chip}`;
}

/** @param {Uint8Array} bytes @param {number} at @param {number} length */
function field(bytes, at, length) {
  let text = "";
  for (let index = 0; index < length; index++) {
    const byte = bytes[at + index];
    if (byte === 0) break;
    text += byte >= 0x20 && byte < 0x7f ? String.fromCharCode(byte) : "?";
  }
  return text;
}

/**
 * @typedef {{ version: string, project: string, built: string, idfVersion: string, chip: number }} ImageInfo
 * @param {Uint8Array} image the whole file
 * @param {number | null} chip the bike's chip, or null if it isn't known yet
 * @returns {{ info: ImageInfo | null, problem: string }}
 */
export function checkImage(image, chip) {
  if (image.length < HEADER_BYTES) return { info: null, problem: `Too short to be firmware (${image.length} bytes).` };
  if (image[0] !== IMAGE_MAGIC) {
    return { info: null, problem: "Not an ESP32 firmware image. Choose the build's emcool.bin." };
  }
  const imageChip = image[12] | (image[13] << 8);
  if (chip !== null && imageChip !== chip) {
    return {
      info: null,
      problem: `This is firmware for the ${chipName(imageChip)}, but the bike is an ${chipName(chip)}. Choose the build for this board (build/${chip === 9 ? "esp32s3" : "esp32"}/emcool.bin).`,
    };
  }
  const view = new DataView(image.buffer, image.byteOffset, image.byteLength);
  if (view.getUint32(DESCRIPTION, true) !== APP_DESC_MAGIC) {
    return {
      info: null,
      problem: "An ESP32 image, but not an app: choose emcool.bin, not the bootloader or a merged image.",
    };
  }
  const info = {
    version: field(image, DESCRIPTION + 16, 32),
    project: field(image, DESCRIPTION + 48, 32),
    built: `${field(image, DESCRIPTION + 96, 16)} ${field(image, DESCRIPTION + 80, 16)}`,
    idfVersion: field(image, DESCRIPTION + 112, 32),
    chip: imageChip,
  };
  if (info.project !== "emcool") return { info: null, problem: `This is "${info.project}" firmware, not emcool's.` };
  if (image.length > SLOT_BYTES) {
    return { info: null, problem: `${image.length} bytes won't fit the ${SLOT_BYTES}-byte app slot.` };
  }
  return { info, problem: "" };
}

/** @param {ImageInfo} info */
export function describeImage(info) {
  return `emcool ${info.version}, built ${info.built}, ESP-IDF ${info.idfVersion}, for the ${chipName(info.chip)}`;
}

/**
 * @typedef {object} FirmwareReply
 * @property {boolean} ok
 * @property {string} message
 * @property {number} [received]
 * @property {number} [expected]
 * @property {string} [incoming]
 */
/**
 * @typedef {(path: string, body?: Uint8Array) => Promise<{ status: number, reply: FirmwareReply }>} FirmwarePost
 *   POSTs to the bike with X-Emcool: firmware, and reads its JSON reply.
 */

/** Pieces in flight at once: enough to keep the radio busy, few enough to spare the bike's heap. */
export const PIECES_IN_FLIGHT = 2;

/**
 * Sends `image` to the bike: begin, every piece in order, finish. Resolves with the bike's final
 * reply; ok:false says why it stopped, and the running firmware is unchanged then.
 * @param {Uint8Array} image
 * @param {number} chunkBytes the bike's piece size (GET /firmware's chunkBytes)
 * @param {FirmwarePost} post
 * @param {(sent: number, total: number) => void} onProgress
 * @param {() => boolean} [cancelled]
 * @returns {Promise<FirmwareReply>}
 */
export async function sendImage(image, chunkBytes, post, onProgress, cancelled = () => false) {
  const begun = await post(`/firmware/begin?bytes=${image.length}`);
  if (!begun.reply.ok) return begun.reply;
  let next = 0;
  let confirmed = 0;
  /** @type {FirmwareReply | null} */
  let failure = null;
  // Pieces go in order; the bike answers them in order, so a window of them just hides the
  // round trip. A piece refused stops the rest, which the bike refuses too (wrong offset).
  /** @type {Promise<void>[]} */
  const inFlight = [];
  // Never rejects: a piece waiting in the window has no handler yet, so a lost link is recorded instead.
  const sendPiece = async (/** @type {number} */ offset) => {
    let reply;
    try {
      ({ reply } = await post(`/firmware/chunk?offset=${offset}`, image.subarray(offset, offset + chunkBytes)));
    } catch (error) {
      failure ??= {
        ok: false,
        message: `The link went away at ${confirmed} of ${image.length} bytes (${/** @type {Error} */ (error).message}). The bike stops the update when the phone goes; the running firmware is unchanged.`,
      };
      return;
    }
    if (!reply.ok) {
      failure ??= reply;
      return;
    }
    confirmed = Math.max(confirmed, reply.received ?? offset + chunkBytes);
    onProgress(confirmed, image.length);
  };
  while (next < image.length && failure === null) {
    if (cancelled()) {
      await Promise.allSettled(inFlight);
      const stopped = await post("/firmware/cancel");
      return { ok: false, message: `Cancelled. ${stopped.reply.message}` };
    }
    inFlight.push(sendPiece(next));
    next += chunkBytes;
    if (inFlight.length >= PIECES_IN_FLIGHT) await inFlight.shift();
  }
  await Promise.all(inFlight);
  if (failure !== null) return failure;
  return (await post("/firmware/finish")).reply;
}
