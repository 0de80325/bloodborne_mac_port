#!/usr/bin/env bash
# M2 Pro perf work (docs/M2PRO_PERF_PLAN.md): logs memory state every 5 s while the
# game runs, side by side with the game log. Usage: bash tools/m2perf/sample_mem.sh OUT.tsv
set -u
out=${1:?usage: sample_mem.sh OUT.tsv}
printf 'time\tswap_used_MB\twired_MB\tcompressor_MB\tbb_probe_MB\tbbgpu_host_MB\n' > "$out"
rss() { # RSS of every process whose command matches $1, in MiB
    ps -axo rss=,comm= | awk -v p="$1" '$0 ~ p && !/awk/ {s += $1} END {printf "%d", s / 1024}'
}
while :; do
    swap=$(sysctl -n vm.swapusage | awk '{gsub(/M/,"",$6); print $6}')
    read -r wired comp < <(vm_stat | awk '
        /Pages wired down/            {gsub(/\./,"",$4); w = $4}
        /Pages occupied by compressor/{gsub(/\./,"",$5); c = $5}
        END {print w + 0, c + 0}')
    # 16 KiB pages on this machine: pages / 64 = MiB
    printf '%s\t%s\t%d\t%d\t%s\t%s\n' "$(date +%T)" "$swap" "$((wired / 64))" "$((comp / 64))" \
        "$(rss bb-probe)" "$(rss bbgpu-host)" >> "$out"
    sleep 5
done
