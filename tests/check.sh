#!/usr/bin/env bash
# One command for all checks, with short output: only failures and a summary.
#   tests/check.sh          unit tests + example configs + end-to-end tests (~8 min)
#   tests/check.sh --quick  unit tests + example configs only (~1 min)
# ESPHOME=/path/to/esphome selects the esphome binary (default: from PATH).
set -u
cd "$(dirname "$0")/.."
ESPHOME="${ESPHOME:-esphome}"
LOG="$(mktemp -d)"
fail=0

step() {  # step <name> <command...>
  local name="$1"; shift
  if "$@" >"$LOG/$name.log" 2>&1; then
    echo "ok    $name"
  else
    echo "FAIL  $name (last lines of $LOG/$name.log):"
    tail -n 25 "$LOG/$name.log" | sed 's/^/      /'
    fail=1
  fi
}

# Example configs need examples/secrets.yaml; create a throw-away one if it does not exist.
if [ ! -f examples/secrets.yaml ]; then
  printf 'wifi_ssid: "x"\nwifi_password: "xxxxxxxx"\nfingerprint_grow_plus_key: "q83vEjRWeJCrze8SNFZ4kKvN7xI0VniQq83vEjRWeJA="\nfingerprint_delete_all_code: "CHECK_ONLY"\n' >examples/secrets.yaml
  trap 'rm -f examples/secrets.yaml' EXIT
fi

step unit tests/run_unit_tests.sh
for ex in esp32-arduino esp32-idf esp8266 standalone; do
  step "config-$ex" bash -c "cd examples && '$ESPHOME' config $ex.yaml"
done
if [ "${1:-}" != "--quick" ]; then
  if python3 tests/e2e/run_e2e.py --esphome "$ESPHOME" >"$LOG/e2e.log" 2>&1; then
    echo "ok    e2e ($(grep -o '[0-9]*/[0-9]* checks passed' "$LOG/e2e.log"))"
  else
    echo "FAIL  e2e:"
    grep -E "FAIL|compile of|checks passed|Traceback|Error" "$LOG/e2e.log" | head -n 30 | sed 's/^/      /'
    fail=1
  fi
fi
[ $fail -eq 0 ] && echo "ALL OK" || echo "SOME CHECKS FAILED (logs in $LOG)"
exit $fail
