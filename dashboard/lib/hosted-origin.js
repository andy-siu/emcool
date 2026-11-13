// @ts-check

// The exact HTTPS origins that may open the Bluetooth link. The Bluetooth key is kept in the
// origin's localStorage, so any page served from the same origin could read it: the origin must be
// this dashboard's alone. That rules out a shared host (andy-siu.github.io is every GitHub Pages
// site on the account), paths, wildcards and previews. lib/ble-origin.js reads its allow-list from here.
export const HOSTED_DASHBOARD_ORIGINS = ["https://emcool.athegen.com"];
