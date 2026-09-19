#!/usr/bin/env bash
# Print log lines after the first match of a pattern, filtered of noise.
#   tools/log_after.sh <logfile> <grep-pattern> [max-lines]
set -u
LOG="${1:?log}"; PAT="${2:?pattern}"; MAX="${3:-80}"
N=$(grep -n -- "$PAT" "$LOG" | head -1 | cut -d: -f1)
if [ -z "$N" ]; then echo "pattern not found"; exit 0; fi
echo "=== first match at line $N of $(wc -l <"$LOG")"
tail -n +"$N" "$LOG" | grep -vE "sdcard_read_data|i2c[1-4]: |twl|slave-|repeating|ctrl_core_pad|gpt3|gpt10|wdt2|mode switch|EL1|ESR|Exception return|exception 5" \
  | sed -E 's/^[0-9@.]+ //' | cut -c1-125 | head -n "$MAX"
