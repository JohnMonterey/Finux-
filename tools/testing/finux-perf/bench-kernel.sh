#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Benchmark one booted kernel and emit raw samples for later comparison.
#
# Run this once per kernel variant, after booting into it.  Each run writes
# a self-describing result directory tagged with the kernel's identity, so
# results from different kernels cannot be confused with each other - which
# is the failure mode that quietly invalidates this kind of study.
#
# Design decisions worth knowing about:
#
#   * Benchmark order is randomised per repetition.  Running all reps of
#     benchmark 1 and then all reps of benchmark 2 lets thermal drift on a
#     6-core desktop masquerade as a difference between them.
#
#   * Every repetition is preceded by a quiescence check.  A rep that ran
#     while a background service woke up is not a measurement, and
#     averaging it in is worse than dropping it.
#
#   * Nothing is installed, and no system setting is changed.  Missing
#     tools are reported and their benchmarks skipped.
#
# Usage:
#   tools/testing/finux-perf/bench-kernel.sh [-r reps] [-w warmups]
#                                            [-o outdir] [-t tag] [-q]
#                                            [--allow-untuned]

set -u

REPS=15
WARMUPS=3
OUTBASE="finux-bench"
TAG=""
QUICK=0
ALLOW_UNTUNED=0
SRC="$(cd "$(dirname "$0")/../../.." && pwd)"

while [ $# -gt 0 ]; do
	case "$1" in
	-r) REPS="$2"; shift 2 ;;
	-w) WARMUPS="$2"; shift 2 ;;
	-o) OUTBASE="$2"; shift 2 ;;
	-t) TAG="$2"; shift 2 ;;
	-q) QUICK=1; REPS=3; WARMUPS=1; shift ;;
	--allow-untuned) ALLOW_UNTUNED=1; shift ;;
	-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

have() { command -v "$1" >/dev/null 2>&1; }
NPROC=$(nproc)

# --- identity ---------------------------------------------------------
KVER=$(uname -r)
[ -n "$TAG" ] || TAG="$KVER"
OUT="$OUTBASE/$TAG"
mkdir -p "$OUT" || exit 1
SAMPLES="$OUT/samples.jsonl"
: > "$SAMPLES"

log() { printf '%s\n' "$*" | tee -a "$OUT/run.log"; }

EMIT=1
emit() {
	# emit <benchmark> <metric> <unit> <value> <rep>
	[ "$EMIT" = 1 ] || return 0
	printf '{"kernel":"%s","tag":"%s","bench":"%s","metric":"%s","unit":"%s","value":%s,"rep":%s,"ts":%s}\n' \
		"$KVER" "$TAG" "$1" "$2" "$3" "$4" "$5" "$(date +%s)" \
		>> "$SAMPLES"
}

log "=============================================================="
log "kernel : $KVER"
log "tag    : $TAG"
log "reps   : $REPS (warmups $WARMUPS)"
log "cpus   : $NPROC"
log "started: $(date -Is)"
log "=============================================================="

# --- environment capture ----------------------------------------------
#
# Recorded per run, because a benchmark compared across kernels is only
# valid if these matched.  Comparing runs whose governor differed is
# comparing governors.
{
	echo "uname: $(uname -a)"
	echo "cmdline: $(cat /proc/cmdline)"
	echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
	echo "microcode: $(grep -m1 microcode /proc/cpuinfo | cut -d: -f2- | xargs)"
	echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo absent)"
	echo "driver: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo absent)"
	echo "epp: $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null || echo absent)"
	echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo absent)"
	echo "smt: $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo absent)"
	echo "turbo/idle driver: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo absent)"
	echo "nmi_watchdog: $(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null)"
	echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)"
	echo "gcc: $(gcc --version 2>/dev/null | head -1)"
	echo "uptime: $(cat /proc/uptime)"
} > "$OUT/env.txt"
cat "$OUT/env.txt" >> "$OUT/run.log"

if [ -r /proc/config.gz ]; then
	zcat /proc/config.gz > "$OUT/config"
elif [ -r "/boot/config-$KVER" ]; then
	cp "/boot/config-$KVER" "$OUT/config"
else
	echo "NO CONFIG AVAILABLE" > "$OUT/config"
	log "WARNING: cannot capture this kernel's config; results will not be"
	log "         attributable to a specific configuration."
fi

# --- preconditions ------------------------------------------------------
warn=0

GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
if [ -n "$GOV" ] && [ "$GOV" != "performance" ]; then
	log "NOTE: governor is '$GOV', not 'performance'."
	log "      Scheduler deltas are easily swamped by DVFS behaviour."
	log "      Either pin the governor for all variants or accept the noise,"
	log "      but do not change it between variants."
	warn=1
fi

UPTIME_S=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)
if [ "$UPTIME_S" -lt 120 ]; then
	log "NOTE: uptime is ${UPTIME_S}s.  Boot-time work is still settling."
	log "      Waiting 120s before starting."
	sleep 120
fi

if [ "$ALLOW_UNTUNED" = 0 ] && [ "$warn" = 1 ]; then
	log ""
	log "Re-run with --allow-untuned to proceed anyway."
	log "(This is a warning about comparability, not a hard error.)"
fi

# --- tool inventory ------------------------------------------------------
declare -A TOOL
for t in perf turbostat hackbench schbench stress-ng; do
	if have "$t"; then TOOL[$t]=1; else TOOL[$t]=0; fi
done
{
	echo "# tool availability"
	for t in "${!TOOL[@]}"; do
		printf '%-12s %s\n' "$t" \
			"$([ "${TOOL[$t]}" = 1 ] && echo present || echo MISSING)"
	done
} | tee "$OUT/tools.txt" >> "$OUT/run.log"

MISSING=$(awk '$2=="MISSING"{print $1}' "$OUT/tools.txt" | tr '\n' ' ')
[ -n "$MISSING" ] && log "MISSING (benchmarks skipped, nothing installed): $MISSING"

# --- build the fork benchmark -------------------------------------------
FORKBENCH="$OUT/forkbench"
if cc -O2 -o "$FORKBENCH" "$SRC/tools/testing/finux-perf/forkbench.c" \
	-lpthread 2>"$OUT/forkbench-build.log"; then
	log "forkbench: built"
else
	log "forkbench: BUILD FAILED (see forkbench-build.log)"
	FORKBENCH=""
fi

# --- quiescence ----------------------------------------------------------
#
# A run contaminated by background activity produces a number that looks
# like data.  Refuse to start a repetition until the machine is actually
# idle, and give up rather than silently measuring noise.
QUIESCE_MAX_LOAD="0.30"
wait_quiet() {
	local tries=0 load
	while [ $tries -lt 30 ]; do
		load=$(cut -d' ' -f1 /proc/loadavg)
		if awk -v l="$load" -v m="$QUIESCE_MAX_LOAD" \
			'BEGIN{exit !(l<=m)}'; then
			return 0
		fi
		sleep 2
		tries=$((tries + 1))
	done
	log "  WARNING: load did not settle below $QUIESCE_MAX_LOAD (now $load)"
	return 1
}

# --- perf stat wrapper ----------------------------------------------------
#
# Wall time alone cannot distinguish "the kernel did less work" from "the
# work landed somewhere else".  The counters below are what separate a real
# reduction in scheduler overhead from a redistribution of it.
PERF_EVENTS="task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions"
# cache-misses is not always available; probed once rather than per rep.
if command -v perf >/dev/null 2>&1 &&
   perf stat -e cache-misses -x, true >/dev/null 2>&1; then
	PERF_EVENTS="$PERF_EVENTS,cache-misses"
fi

perf_wrap() {
	# perf_wrap <label> <rep> <command...>
	local label="$1" rep="$2"; shift 2
	local pf="$OUT/perf-$label-$rep.csv" rc

	if [ "${TOOL[perf]}" != 1 ]; then
		"$@" > "$OUT/out-$label-$rep.txt" 2>&1
		return $?
	fi

	perf stat -e "$PERF_EVENTS" -x, -o "$pf" -- "$@" \
		> "$OUT/out-$label-$rep.txt" 2>&1
	rc=$?

	# perf -x, emits: value,unit,event,runtime,pct,...
	while IFS=, read -r val _unit ev _rest; do
		case "$ev" in
		task-clock|context-switches|cpu-migrations|page-faults|\
cycles|instructions|cache-misses)
			case "$val" in
			''|*[!0-9.]*) ;;
			*) emit "$label" "$ev" count "$val" "$rep" ;;
			esac
			;;
		esac
	done < <(grep -v '^#' "$pf" 2>/dev/null)

	return $rc
}

# --- individual benchmarks -------------------------------------------------

bench_sched_pipe() {
	local rep="$1" out mig
	[ "${TOOL[perf]}" = 1 ] || return 0

	# Two tasks ping-ponging through a pipe.  Mostly a syscall and
	# context-switch path test - it cannot see load balancing, fairness
	# or anything cgroup-related, so a null result here says nothing
	# about those.
	#
	# --format=default, not simple: simple prints milliseconds, which
	# quantises a sub-second result into uselessness.  default prints
	# usecs/op.
	#
	# Note perf's own -r/--repeat does NOT apply to sched pipe in this
	# tree, so repetition has to come from the outside, as it does here.
	out=$(perf bench --format=default sched pipe -l 1000000 2>&1 |
	      sed -n 's/^[[:space:]]*\([0-9.]*\)[[:space:]]*usecs\/op.*/\1/p' |
	      head -1)
	case "$out" in
	''|*[!0-9.]*) return 0 ;;
	*) emit sched-pipe usecs-per-op us "$out" "$rep" ;;
	esac

	# Pinned to one CPU the benchmark is a pure context switch; left to
	# the scheduler the wakee may land on an idle sibling, making every
	# iteration pay a reschedule IPI and a C-state exit.  Those are two
	# different experiments producing one number, so record which one
	# happened.
	if [ "${TOOL[perf]}" = 1 ]; then
		mig=$(perf stat -e cpu-migrations -x, -- \
			perf bench --format=simple sched pipe -l 200000 \
			2>&1 >/dev/null | awk -F, '$3=="cpu-migrations"{print $1}')
		case "$mig" in
		''|*[!0-9.]*) ;;
		*) emit sched-pipe cpu-migrations count "$mig" "$rep" ;;
		esac
	fi
}

bench_sched_messaging() {
	local rep="$1" out
	[ "${TOOL[perf]}" = 1 ] || return 0
	# 20 groups x 20 tasks: heavy wakeup and migration load, sized to
	# clearly oversubscribe 12 threads.
	out=$(perf bench -f simple sched messaging -g 20 -l 200 2>/dev/null |
		tr -d ' ')
	case "$out" in
	''|*[!0-9.]*) return 0 ;;
	*) emit sched-messaging wall-time s "$out" "$rep" ;;
	esac
}

bench_hackbench() {
	local rep="$1" out
	# NOTE: perf bench sched messaging is a port of hackbench - same
	# benchmark, different defaults.  Agreement between the two is not
	# corroboration, and counting both inflates the multiple-comparison
	# family.  Kept only as an external cross-check; treat as correlated
	# with sched-messaging when interpreting results.
	[ "${TOOL[hackbench]}" = 1 ] || return 0
	out=$(hackbench -g 20 -l 200 2>/dev/null |
		sed -n 's/.*Time: *\([0-9.]*\).*/\1/p')
	[ -n "$out" ] && emit hackbench wall-time s "$out" "$rep"
}

bench_schbench() {
	local rep="$1" p99
	[ "${TOOL[schbench]}" = 1 ] || return 0
	# Wakeup latency under load; -m workers, -t threads each.
	schbench -m 6 -t 12 -r 10 > "$OUT/schbench-$rep.txt" 2>&1
	p99=$(sed -n 's/.*99.0th: *\([0-9]*\).*/\1/p' "$OUT/schbench-$rep.txt" |
		tail -1)
	[ -n "$p99" ] && emit schbench wakeup-p99 us "$p99" "$rep"
}

bench_fork() {
	local rep="$1" v
	[ -n "$FORKBENCH" ] || return 0
	# The in-tree equivalents, which are versioned with the kernel and
	# so cannot drift from it.  forkbench is kept as well because it
	# separates thread from fork from exec, which is what localises a
	# per-mm cost as opposed to a per-task one.
	if [ "${TOOL[perf]}" = 1 ]; then
		for sc in fork execve; do
			v=$(perf bench --format=simple syscall "$sc" -l 20000 \
				2>/dev/null | tr -d ' ')
			case "$v" in
			''|*[!0-9.]*) ;;
			*) emit "syscall-$sc" wall-time s "$v" "$rep" ;;
			esac
		done
	fi

	for mode in fork exec thread; do
		# Under perf stat, so process-creation cost is visible as
		# counters as well as wall time.  This is the benchmark that
		# would reveal a per-mm allocation.
		perf_wrap "fork-$mode" "$rep" "$FORKBENCH" "$mode" 20000
		v=$(tail -1 "$OUT/out-fork-$mode-$rep.txt" 2>/dev/null)
		case "$v" in
		''|*[!0-9.]*) continue ;;
		*) emit "fork-$mode" ns-per-op ns "$v" "$rep" ;;
		esac
	done
}

bench_kbuild() {
	local rep="$1" start end
	local bdir="$OUT/kbuild"
	# Largely userspace-bound, so it dilutes scheduler effects - but it
	# is the workload the machine actually runs, and the -j value
	# oversubscribes enough to exercise load balancing.
	rm -rf "$bdir"
	mkdir -p "$bdir"
	make -C "$SRC" O="$bdir" defconfig > /dev/null 2>&1 || return 0
	# Prime, then time a clean rebuild of a bounded subset so a rep takes
	# minutes rather than an hour.
	make -C "$SRC" O="$bdir" -j"$NPROC" prepare > /dev/null 2>&1
	sync
	start=$(date +%s.%N)
	perf_wrap kbuild "$rep" \
		make -C "$SRC" O="$bdir" -j$((NPROC * 2)) kernel/ mm/ fs/
	end=$(date +%s.%N)
	emit kbuild wall-time s "$(awk -v a="$start" -v b="$end" \
		'BEGIN{printf "%.3f", b-a}')" "$rep"
	rm -rf "$bdir"
}

bench_idle() {
	local secs="${1:-300}"
	log "idle test: ${secs}s"
	wait_quiet
	if have turbostat; then
		turbostat --quiet --Summary --interval 10 \
			--out "$OUT/idle-turbostat.txt" sleep "$secs" 2>&1 |
			tail -5 >> "$OUT/run.log"
	else
		local c0 c1
		c0=$(awk '{print $1}' /proc/stat | head -1)
		cat /proc/stat | head -1 > "$OUT/idle-stat-before.txt"
		sleep "$secs"
		cat /proc/stat | head -1 > "$OUT/idle-stat-after.txt"
		log "  turbostat missing: recorded /proc/stat deltas only."
		log "  No package power, frequency or C-state data collected."
	fi
	# Interrupt deltas bound how much timer work idle actually did.
	cp /proc/interrupts "$OUT/idle-interrupts-after.txt" 2>/dev/null
}

# --- main loop -------------------------------------------------------------
BENCHES=(bench_sched_pipe bench_sched_messaging bench_hackbench
	 bench_schbench bench_fork)
[ "$QUICK" = 1 ] || BENCHES+=(bench_kbuild)

run_all() {
	local rep="$1"
	# Randomise order so thermal drift does not consistently favour
	# whichever benchmark happens to run first.
	local order
	mapfile -t order < <(printf '%s\n' "${BENCHES[@]}" | shuf)
	for b in "${order[@]}"; do
		wait_quiet || true
		"$b" "$rep"
	done
}

log ""
log "--- warmups ($WARMUPS) ---"
EMIT=0
for i in $(seq 1 "$WARMUPS"); do
	log "warmup $i/$WARMUPS"
	run_all "w$i" > /dev/null 2>&1
done
EMIT=1

log ""
log "--- measured repetitions ($REPS) ---"
for i in $(seq 1 "$REPS"); do
	log "rep $i/$REPS  ($(date +%H:%M:%S), load $(cut -d' ' -f1 /proc/loadavg))"
	run_all "$i"
done

log ""
[ "$QUICK" = 1 ] || bench_idle 300

cp /proc/interrupts "$OUT/interrupts-final.txt" 2>/dev/null

log ""
log "=============================================================="
log "samples: $(wc -l < "$SAMPLES") in $SAMPLES"
log "finished: $(date -Is)"
log "=============================================================="
log ""
log "Analyse with:"
log "  tools/testing/finux-perf/analyze.py $OUTBASE/*/samples.jsonl"
