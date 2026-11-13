#!/usr/bin/env bash
# Every host-side check:
#   - the generated registry is current with the reference implementation
#   - the C++ decoders match the TypeScript reference value for value on the golden corpus, and
#     warn exactly as often; no location signal reaches the ride log
#   - the signal-store rules hold
#   - the clock gate matches the reference verdict for verdict
#   - the pack-temperature routing matches the reference value for value, and warns as often
#   - the diagnostic tables are current with the reference; the OBD side — the PID poller, trouble-code
#     reads over ISO-TP, the poller's hold, the E-LOCK read — driven against a scripted ECU on a
#     fake clock, sends, records, logs and serves /stored-dtcs exactly as the reference does, to the
#     millisecond
#   - numbers are written exactly as JavaScript writes them; the event loop keeps Node's order
#   - freeze frames, the lifetime statistics and the hub's active fault list decode as the reference's do
#   - the VCU parameter tables are current with the reference; service mode's pure parts (codecs, the
#     service gate, rows, the TABLE_TYPE report) match the reference's on random input; and service
#     mode itself — sweeps, probes, the read runner beside the OBD poller — driven against
#     the reference's simulated VCU micros sends, logs and answers exactly as the reference does
#   - the VCU WRITE path's pure parts (the security key, the write/routine/clear reply decoders,
#     the service stamp and clock verdict, the RTC frame, the allowlist and table gate, the
#     /vcu-write parser, the charge-command frames and the charge-ack judge) match the reference's on
#     random input; and the write session on the wire — the reference's own writeParameter/
#     readServiceStamp/resetVcu/clearStoredDtcs and the charge-current/stop runner, driven against a
#     scripted bus — transmits the same frames and resolves to the same outcome in the C++ port
#   - the automatic charge controller's decision and its support match the reference bit-for-bit, and
#     the reference's own controller driven through scripted charges commands and reports the same
#   - the settings and the serial console's commands behave, and the dashboard's Bike console runs
#     them with the same checks, refuses a stale page's save, and guards its restart
#   - the card commands (dir, cd, type, rm, cp, mv, …) work on a folder standing in for the card,
#     refuse the ride being recorded, RIDES and vcu-params, let one console at a time change it, and
#     leave nothing half-done when the card goes or the phone gives up
#   - an over-the-air update writes exactly the image it's given, and refuses another chip's, another
#     project's, a bootloader, and one cut short or running long, never choosing a part-written slot
#   - the ride log's rules hold, and the decoder corpus pushed through the whole C++ pipeline
#     onto disk reads back exactly on the laptop, survives simulated power cuts and card
#     damage, and imports into rides.db
#   - on a full card (simulated, then a real FAT image on macOS) the oldest parts are deleted
#     first and exactly the newest readings survive
#   - the Bluetooth wire protocol's pieces hold (crypto vectors, deflate against zlib, JSON,
#     framing, the handshake), and the C++ server, driven by the reference's own phone-side
#     ble-wire.js, handshakes, streams the live state exactly, and answers requests with
#     the reference's bodies; /dl sends the ride log the corpus run left on disk, which the laptop
#     reads and imports exactly as it does the card's files, while the live stream keeps coming
#
# Needs a checkout of the TypeScript reference implementation (COOL_EVA_DIR, default ../cool-eva),
# Node 22.13+ (for its built-in SQLite), CMake and Ninja.
# Does not need ESP-IDF.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
reference="${COOL_EVA_DIR:-$root/../cool-eva}"
build="$root/host/build"
golden="$build/golden-decode.txt"
node_flags=(--disable-warning=ExperimentalWarning)

node "${node_flags[@]}" "$root/tools/generate-registry.mts" "$reference" --check
node "${node_flags[@]}" "$root/tools/generate-diagnostic-tables.mts" "$reference" --check
node "${node_flags[@]}" "$root/tools/generate-vcu-tables.mts" "$reference" --check

cmake -S "$root/host" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$build" | grep -vE '^\[[0-9]+/[0-9]+\]' || true

node "${node_flags[@]}" "$root/tools/generate-golden.mts" "$reference" "$golden"

"$build/check_signals" 2>"$build/check_signals.stderr" || {
  cat "$build/check_signals.stderr" >&2
  exit 1
}

status=0
"$build/check_decoders" "$golden" 2>"$build/check_decoders.stderr" || status=$?
grep -vE '^W (gps|attitude): ' "$build/check_decoders.stderr" >&2 || true
typescript_warnings="$(cat "$golden.warnings")"
cpp_warnings="$(grep -cE '^W (gps|attitude): ' "$build/check_decoders.stderr" || true)"
if [[ "$cpp_warnings" != "$typescript_warnings" ]]; then
  echo "check_decoders: the C++ decoders warned $cpp_warnings times, the reference $typescript_warnings" >&2
  status=1
else
  echo "check_decoders: both warned $cpp_warnings times (expected: the corpus feeds bad angles and half-fixes)"
fi
[[ "$status" == 0 ]] || exit "$status"

node "${node_flags[@]}" "$root/tools/generate-golden-clock.mts" "$reference" "$build/golden-clock.txt"
"$build/check_clock" "$build/golden-clock.txt" 2>"$build/check_clock.stderr" || {
  cat "$build/check_clock.stderr" >&2
  exit 1
}

node "${node_flags[@]}" "$root/tools/generate-golden-pack-temperature.mts" "$reference" \
  "$build/golden-pack-temperature.txt"
"$build/check_pack_temperature" "$build/golden-pack-temperature.txt" 2>"$build/check_pack_temperature.stderr" || {
  grep -vE '^[IWE] bms: ' "$build/check_pack_temperature.stderr" >&2
  exit 1
}

node "${node_flags[@]}" "$root/tools/generate-golden-obd.mts" "$reference" "$build/golden-obd.txt"
"$build/check_obd" "$build/golden-obd.txt" 2>"$build/check_obd.stderr" || {
  grep -vE '^[IWE] (obd|obd-dtc|elock): ' "$build/check_obd.stderr" >&2
  exit 1
}

node "${node_flags[@]}" "$root/tools/generate-golden-numbers.mts" >"$build/golden-numbers.txt"
"$build/check_text" "$build/golden-numbers.txt"
"$build/check_async"

node "${node_flags[@]}" "$root/tools/generate-golden-vcu.mts" "$reference" "$build/golden-vcu.txt"
"$build/check_vcu" "$build/golden-vcu.txt" 2>"$build/check_vcu.stderr" || {
  grep -vE '^[IWE] (vcu|vcu-table|vcu-snapshot): ' "$build/check_vcu.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/generate-golden-diagnostics.mts" "$reference" "$build/golden-diagnostics.txt"
"$build/check_diagnostics" "$build/golden-diagnostics.txt" 2>"$build/check_diagnostics.stderr" || {
  grep -vE '^[IWE] (diagnostics|vcu-snapshot): ' "$build/check_diagnostics.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/generate-golden-service.mts" "$reference" "$build/golden-service.txt"
"$build/check_service" "$build/golden-service.txt" 2>"$build/check_service.stderr" || {
  grep -vE '^[IWE] (obd|obd-dtc|vcu|vcu-table|vcu-sweep|vcu-read|vcu-probe|vcu-snapshot|lifetime|freeze-frame|expected-faults): ' "$build/check_service.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/generate-golden-write.mts" "$reference" "$build/golden-write.txt"
"$build/check_write" "$build/golden-write.txt" 2>"$build/check_write.stderr" || {
  cat "$build/check_write.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/generate-golden-write-session.mts" "$reference" "$build/golden-write-session.txt"
"$build/check_write_session" "$build/golden-write-session.txt" 2>"$build/check_write_session.stderr" || {
  cat "$build/check_write_session.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/generate-golden-charge-auto.mts" "$reference" "$build/golden-charge-auto.txt"
node "${node_flags[@]}" "$root/tools/generate-golden-charge-auto-loop.mts" "$reference" "$build/golden-charge-auto-loop.txt"
"$build/check_charge_auto" "$build/golden-charge-auto.txt" "$build/golden-charge-auto-loop.txt" \
  2>"$build/check_charge_auto.stderr" || {
  grep -vE '^W charge-auto: ' "$build/check_charge_auto.stderr" >&2
  exit 1
}

"$build/check_settings" 2>"$build/check_settings.stderr" || {
  cat "$build/check_settings.stderr" >&2
  exit 1
}
"$build/check_console" 2>"$build/check_console.stderr" || {
  grep -vE '^[IW] console: ' "$build/check_console.stderr" >&2
  exit 1
}
"$build/check_card_shell" 2>"$build/check_card_shell.stderr" || {
  grep -vE '^[IW] card: ' "$build/check_card_shell.stderr" >&2
  exit 1
}
# The real firmware image when it has been built (idf.py build), else one laid out the same way.
update_image=()
[[ -f "$root/build/emcool.bin" ]] && update_image=("$root/build/emcool.bin")
"$build/check_update" "${update_image[@]}" 2>"$build/check_update.stderr" || {
  grep -vE '^[IW] update: ' "$build/check_update.stderr" >&2
  exit 1
}

"$build/check_ride_log" 2>"$build/check_ride_log.stderr" || {
  cat "$build/check_ride_log.stderr" >&2
  exit 1
}
"$build/check_ride_log" --corpus "$golden" "$build/ride-log-corpus" "$build/ride-log-expected.jsonl" \
  2>"$build/check_ride_log_corpus.stderr" || {
  grep -vE '^W (gps|attitude): ' "$build/check_ride_log_corpus.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/check-ride-log.mts" "$build/ride-log-corpus" "$build/ride-log-expected.jsonl" \
  "$reference"

"$build/check_ble_wire" 2>"$build/check_ble_wire.stderr" || {
  cat "$build/check_ble_wire.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/check-ble.mts" "$build/ble_server_host" "$golden" "$reference" \
  "$build/ride-log-corpus/RIDES" 2>"$build/check_ble.stderr" || {
  grep -vE '^[IW] (gps|attitude|ble-server|ride-log): ' "$build/check_ble.stderr" >&2
  exit 1
}
# And with no card configured, /dl says the ride log is off.
node "${node_flags[@]}" "$root/tools/check-ble.mts" "$build/ble_server_host" "$golden" "$reference" \
  2>"$build/check_ble_no_card.stderr" || {
  grep -vE '^[IW] (gps|attitude|ble-server|ride-log): ' "$build/check_ble_no_card.stderr" >&2
  exit 1
}

# A full card: the same corpus on a simulated 6 MB card, then on a real 8 MB FAT image where
# writes genuinely run out of room. Both must keep exactly the newest readings.
"$build/check_ride_log" --corpus "$golden" "$build/ride-log-rotated" "$build/ride-log-rotated.jsonl" \
  --card-bytes 6000000 2>"$build/check_ride_log_rotated.stderr" || {
  grep -vE '^W (gps|attitude): ' "$build/check_ride_log_rotated.stderr" >&2
  exit 1
}
node "${node_flags[@]}" "$root/tools/check-ride-log.mts" "$build/ride-log-rotated" "$build/ride-log-rotated.jsonl" \
  "$reference" --rotated

if command -v hdiutil >/dev/null; then
  image_directory="$(mktemp -d /tmp/emcool-fat.XXXXXX)"
  hdiutil create -size 8m -fs "MS-DOS" -volname EMCTEST -layout NONE "$image_directory/card.dmg" >/dev/null
  attach_output="$(hdiutil attach -nobrowse -noautoopen -mountrandom "$image_directory" "$image_directory/card.dmg")"
  device="$(echo "$attach_output" | awk 'NR==1 {print $1}')"
  mount_point="$(echo "$attach_output" | awk -F'\t' 'NF>1 {print $NF}' | tail -1 | sed 's/[[:space:]]*$//')"
  trap 'hdiutil detach "$device" >/dev/null 2>&1 || hdiutil detach -force "$device" >/dev/null; rm -rf "$image_directory"' EXIT
  "$build/check_ride_log" --corpus "$golden" "$mount_point" "$build/ride-log-fat.jsonl" --statvfs \
    2>"$build/check_ride_log_fat.stderr" || {
    grep -vE '^W (gps|attitude): ' "$build/check_ride_log_fat.stderr" >&2
    exit 1
  }
  node "${node_flags[@]}" "$root/tools/check-ride-log.mts" "$mount_point" "$build/ride-log-fat.jsonl" "$reference" --rotated
else
  echo "check.sh: no hdiutil, so the real-FAT full-card run is skipped (the simulated one ran)"
fi
