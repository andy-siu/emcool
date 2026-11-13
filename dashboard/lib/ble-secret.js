// @ts-check

/** Generates the stable shared key that onboarding persists once, never at every boot. */
export function generateBleSecret() {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
}

/**
 * A generated key (64 lower-case hex), or a typed one: 8-64 printable ASCII characters with at
 * least one letter and one digit, and no spaces, quotes or backslashes. The firmware's
 * BleSession::secretIsUsable() and its BLE_SECRET setting check the same.
 * @param {unknown} secret
 */
export function validBleSecret(secret) {
  if (typeof secret !== "string") return false;
  if (/^[0-9a-f]{64}$/.test(secret)) return true;
  return /^[\x21\x23-\x5b\x5d-\x7e]{8,64}$/.test(secret) && /[A-Za-z]/.test(secret) && /[0-9]/.test(secret);
}

/** The key as the bike keeps it: trimmed, and a generated key in lower case. @param {string} secret */
export function canonicalBleSecret(secret) {
  const trimmed = secret.trim();
  const lowered = trimmed.toLowerCase();
  return /^[0-9a-f]{64}$/.test(lowered) ? lowered : trimmed;
}

/** @param {string} secret */
export function normalizeBleSecret(secret) {
  const normalized = canonicalBleSecret(secret);
  if (!validBleSecret(normalized)) {
    throw new Error(
      "Paste the bike’s Bluetooth key: the 64 characters new-ble-key printed, or the 8-64 you set, with a letter and a digit and no spaces."
    );
  }
  return normalized;
}
