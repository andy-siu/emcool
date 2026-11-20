// @ts-check
// Daniel approved this map; keep its destinations together for feedback on the built screens.
export const SETTINGS_MAP = [
  { key: "console", title: "Bike console", detail: "Every setting · Status report · Restart · Commands" },
  { key: "phone", title: "This phone", detail: "Screen · Units" },
  { key: "bike", title: "Bike setup", detail: "Coolant probes · Radiator fan · Custom BMS" },
  { key: "charging", title: "Charging", detail: "Allow automatic current reduction" },
  { key: "connections", title: "Connections", detail: "Dashboard Bluetooth · Motorcycle · CAN" },
  { key: "network", title: "Network connections", detail: "Wi-Fi network · Wi-Fi password · SSH password · Telnet password" },
  { key: "recording", title: "Ride recording", detail: "Recording status · Public key · Backup help" },
  { key: "software", title: "emcool", detail: "Version · Firmware update" },
  { key: "advanced", title: "Advanced", detail: "Data location · Recording and service directories" },
];

/** @type {Record<string, string>} */
export const SETTINGS_PAGE_TITLES = {
  ...Object.fromEntries(SETTINGS_MAP.map(item => [item.key, item.title])),
  access: "Who can control my bike?",
};
