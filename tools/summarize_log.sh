#!/usr/bin/env bash
# Summarise a gpsmap QEMU log: device access counts, GIC/SDHCI trace events.
#   tools/summarize_log.sh <logfile>
set -u
L="${1:?logfile}"
echo "lines: $(wc -l <"$L")"
echo "=== device / event counts"
sed -E 's/^[0-9@.]+ //' "$L" | cut -d: -f1 | cut -d'(' -f1 | sort | uniq -c | sort -rn | head -30
echo "=== GIC enable/disable/dist writes"
grep -E "gic_enable_irq|gic_disable_irq|gic_dist_write|gic_cpu_write" "$L" | sed -E 's/^[0-9@.]+ //' | sort | uniq -c | sort -rn | head -40
echo "=== GIC set/ack"
grep -E "gic_set_irq|gic_acknowledge" "$L" | sed -E 's/^[0-9@.]+ //' | cut -c1-90 | sort | uniq -c | sort -rn | head -20
echo "=== SDHCI / SD card"
grep -E "sdhci|sdcard|emmc" "$L" | sed -E 's/^[0-9@.]+ //' | cut -c1-110 | sort | uniq -c | sort -rn | head -40
echo "=== exceptions"
grep -E "Taking exception" "$L" | sort | uniq -c | sort -rn | head
echo "=== tail"
tail -25 "$L" | cut -c1-120
