#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Capture everything needed to establish a real performance baseline on the
# target machine.
#
# This exists because a performance configuration cannot be designed away
# from the hardware it targets.  Run it on the Ryzen 5 5500 Fedora desktop
# and keep the resulting directory; every later decision refers back to it.
#
# Read-only: installs nothing, changes no setting, touches no boot entry.
#
# Usage: tools/testing/finux-perf/collect-baseline.sh [output-dir]

set -u

OUT="${1:-finux-baseline-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT" || exit 1

say() { printf '%s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Run a command into a file, recording clearly when a tool is absent rather
# than leaving an empty file that looks like a valid empty answer.
grab() {
	local name="$1"; shift
	if have "$1"; then
		{ "$@"; } > "$OUT/$name" 2>&1
	else
		printf 'MISSING TOOL: %s\n' "$1" > "$OUT/$name"
	fi
}

grab_file() {
	local name="$1" src="$2"
	if [ -r "$src" ]; then
		cat "$src" > "$OUT/$name" 2>&1
	else
		printf 'NOT PRESENT: %s\n' "$src" > "$OUT/$name"
	fi
}

say "Collecting baseline into $OUT/"

# --- identity ---------------------------------------------------------
{
	echo "collected: $(date -Is)"
	echo "hostname: $(hostname)"
	echo "uname: $(uname -a)"
	echo "kernel-release: $(uname -r)"
} > "$OUT/00-identity.txt"

grab_file 01-cmdline.txt /proc/cmdline
grab_file 02-cpuinfo.txt /proc/cpuinfo
grab_file 03-version.txt /proc/version
grab_file 04-meminfo.txt /proc/meminfo

# --- the baseline kernel configuration --------------------------------
#
# This is the single most important artifact.  Without the config the
# running kernel was actually built from, there is no baseline to compare
# against and no honest starting point for a tuned build.
if [ -r /proc/config.gz ]; then
	zcat /proc/config.gz > "$OUT/10-running-config" 2>/dev/null &&
		say "  running config: /proc/config.gz"
elif [ -r "/boot/config-$(uname -r)" ]; then
	cat "/boot/config-$(uname -r)" > "$OUT/10-running-config" &&
		say "  running config: /boot/config-$(uname -r)"
else
	printf 'NO RUNNING CONFIG FOUND\n' > "$OUT/10-running-config"
	say "  !! no running kernel config found - see notes at end"
fi
ls -la /boot/ > "$OUT/11-boot-dir.txt" 2>&1

# --- CPU topology -----------------------------------------------------
grab 20-lscpu.txt lscpu
grab 21-lscpu-extended.txt lscpu -e=CPU,CORE,SOCKET,NODE,CACHE,ONLINE,MAXMHZ,MINMHZ
grab 22-lscpu-cache.txt lscpu -C

# The LLC question decides Variant A.  Record every CPU's L3 sharing mask
# rather than sampling cpu0, so a heterogeneous or multi-CCX topology
# cannot hide behind a single lucky reading.
{
	echo "# cpu : index3(L3) shared_cpu_list : level : size"
	for c in /sys/devices/system/cpu/cpu[0-9]*; do
		n=${c##*/cpu}
		idx="$c/cache/index3"
		if [ -d "$idx" ]; then
			printf 'cpu%-3s: %-24s : L%-2s : %s\n' "$n" \
				"$(cat "$idx/shared_cpu_list" 2>/dev/null)" \
				"$(cat "$idx/level" 2>/dev/null)" \
				"$(cat "$idx/size" 2>/dev/null)"
		else
			printf 'cpu%-3s: NO index3 (no L3 exposed)\n' "$n"
		fi
	done
	echo
	echo "# distinct L3 sharing masks (one line == one LLC domain):"
	cat /sys/devices/system/cpu/cpu[0-9]*/cache/index3/shared_cpu_list \
		2>/dev/null | sort -u
	echo
	echo "# distinct count:"
	cat /sys/devices/system/cpu/cpu[0-9]*/cache/index3/shared_cpu_list \
		2>/dev/null | sort -u | wc -l
} > "$OUT/23-llc-topology.txt"

{
	echo "# thread siblings (SMT pairing):"
	for c in /sys/devices/system/cpu/cpu[0-9]*; do
		printf '%s: %s\n' "${c##*/}" \
			"$(cat "$c/topology/thread_siblings_list" 2>/dev/null)"
	done
	echo
	echo "smt/active: $(cat /sys/devices/system/cpu/smt/active 2>/dev/null)"
	echo "smt/control: $(cat /sys/devices/system/cpu/smt/control 2>/dev/null)"
} > "$OUT/24-smt.txt"

grab 25-numa.txt numactl --hardware
grab_file 26-node-online.txt /sys/devices/system/node/online

# --- frequency and power ----------------------------------------------
{
	for f in scaling_driver scaling_governor scaling_available_governors \
		 energy_performance_preference energy_performance_available_preferences \
		 scaling_min_freq scaling_max_freq cpuinfo_min_freq cpuinfo_max_freq; do
		printf '%-42s %s\n' "$f:" \
			"$(cat "/sys/devices/system/cpu/cpu0/cpufreq/$f" 2>/dev/null ||
			   echo '(absent)')"
	done
	echo
	echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null ||
			echo '(absent)')"
	echo "amd_pstate status: $(cat /sys/devices/system/cpu/amd_pstate/status \
			2>/dev/null || echo '(absent)')"
	echo "prefcore: $(cat /sys/devices/system/cpu/amd_pstate/prefcore \
			2>/dev/null || echo '(absent)')"
} > "$OUT/30-cpufreq.txt"

{
	echo "# cpuidle driver: $(cat /sys/devices/system/cpu/cpuidle/current_driver \
		2>/dev/null || echo '(absent)')"
	echo "# governor: $(cat /sys/devices/system/cpu/cpuidle/current_governor \
		2>/dev/null || echo '(absent)')"
	for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
		[ -d "$s" ] || continue
		printf '%s: name=%s latency=%sus disabled=%s\n' "${s##*/}" \
			"$(cat "$s/name" 2>/dev/null)" \
			"$(cat "$s/latency" 2>/dev/null)" \
			"$(cat "$s/disable" 2>/dev/null)"
	done
} > "$OUT/31-cpuidle.txt"

# --- microcode and firmware -------------------------------------------
{
	echo "microcode (cpuinfo): $(grep -m1 microcode /proc/cpuinfo)"
	echo "cpu family/model/stepping:"
	grep -m1 -E "^cpu family|^model\b|^stepping" /proc/cpuinfo
	grep -m1 "model name" /proc/cpuinfo
} > "$OUT/40-microcode.txt"

grab 41-dmidecode.txt dmidecode -t bios -t processor -t baseboard

# --- vulnerabilities (recorded, never changed) ------------------------
{
	for v in /sys/devices/system/cpu/vulnerabilities/*; do
		[ -r "$v" ] || continue
		printf '%-24s %s\n' "$(basename "$v"):" "$(cat "$v")"
	done
} > "$OUT/42-vulnerabilities.txt"

# --- cgroup / PSI / systemd -------------------------------------------
{
	echo "cgroup fs type: $(stat -fc %T /sys/fs/cgroup 2>/dev/null)"
	echo "  (cgroup2fs == unified/v2 only; tmpfs == v1 or hybrid)"
	echo
	echo "# cgroup v1 controllers still mounted (empty is good):"
	grep -E "cgroup " /proc/mounts | grep -v cgroup2 || echo "  none"
	echo
	echo "# /proc/cgroups:"
	cat /proc/cgroups 2>/dev/null
	echo
	echo "# cgroup.controllers at root:"
	cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null
} > "$OUT/50-cgroup.txt"

# PSI decides whether psi=0 is even a candidate.  A consumer that needs
# pressure data makes the whole variant a non-starter.
{
	echo "# /proc/pressure present?"
	ls -la /proc/pressure/ 2>&1
	echo
	echo "# cpu pressure sample:"
	cat /proc/pressure/cpu 2>/dev/null || echo "  (absent)"
	echo
	echo "# systemd-oomd (the main PSI consumer on Fedora):"
	systemctl is-enabled systemd-oomd 2>&1
	systemctl is-active systemd-oomd 2>&1
	echo
	echo "# units with oomd pressure settings configured:"
	systemctl show '*' -p ManagedOOMMemoryPressure --value 2>/dev/null |
		sort | uniq -c | sort -rn | head
	echo
	echo "# anything with /proc/pressure open right now:"
	if have lsof; then
		lsof /proc/pressure/* 2>/dev/null || echo "  (none found)"
	else
		for p in /proc/[0-9]*/fd/*; do
			tgt=$(readlink "$p" 2>/dev/null) || continue
			case "$tgt" in */pressure/*) echo "$p -> $tgt";; esac
		done 2>/dev/null | head -20
		echo "  (scanned /proc/*/fd; lsof not installed)"
	fi
} > "$OUT/51-psi.txt"

# --- sched_ext ---------------------------------------------------------
{
	echo "# sched_ext state (decides whether SCHED_CLASS_EXT=n is safe):"
	cat /sys/kernel/sched_ext/state 2>/dev/null || echo "  no /sys/kernel/sched_ext (not enabled or not loaded)"
	ls -la /sys/kernel/sched_ext/ 2>&1
	echo
	echo "# loaded BPF programs of type struct_ops:"
	if have bpftool; then
		bpftool struct_ops list 2>&1
	else
		echo "  MISSING TOOL: bpftool"
	fi
	echo
	echo "# scx services:"
	systemctl list-units --all 2>/dev/null | grep -i scx || echo "  none"
} > "$OUT/52-sched-ext.txt"

# --- toolchain ---------------------------------------------------------
{
	echo "gcc: $(gcc --version 2>/dev/null | head -1)"
	echo "clang: $(clang --version 2>/dev/null | head -1)"
	echo "ld: $(ld --version 2>/dev/null | head -1)"
	echo "make: $(make --version 2>/dev/null | head -1)"
	echo "rpmbuild: $(rpmbuild --version 2>/dev/null)"
	echo
	echo "# gcc -march=native resolution on this CPU:"
	gcc -march=native -Q --help=target 2>/dev/null |
		grep -E "^\s+-march=|^\s+-mtune=" | head -5
} > "$OUT/60-toolchain.txt"

# --- graphics / desktop ------------------------------------------------
{
	echo "session type: ${XDG_SESSION_TYPE:-unknown}"
	echo "desktop: ${XDG_CURRENT_DESKTOP:-unknown}"
	echo
	echo "# NVIDIA:"
	if have nvidia-smi; then
		nvidia-smi --query-gpu=name,driver_version,pstate --format=csv 2>&1
	else
		echo "  MISSING TOOL: nvidia-smi"
	fi
	echo "  loaded nvidia modules:"
	lsmod 2>/dev/null | grep -i nvidia || echo "    none"
	echo
	echo "# akmods/dkms packages present:"
	rpm -qa 2>/dev/null | grep -Ei "nvidia|akmod|dkms" | head -20
} > "$OUT/61-graphics.txt"

# --- 32-bit / Steam / Wine compatibility -------------------------------
{
	echo "# IA32 emulation available to userspace?"
	echo "  ia32_emulation in cmdline: $(grep -o 'ia32_emulation=[^ ]*' /proc/cmdline || echo '(not specified)')"
	echo "  /proc/sys/abi: $(ls /proc/sys/abi 2>/dev/null | tr '\n' ' ')"
	echo
	echo "# 32-bit runtime libraries installed:"
	rpm -qa 2>/dev/null | grep -c "\.i686" | sed 's/^/  i686 packages: /'
	echo
	echo "# Steam / Wine present:"
	rpm -qa 2>/dev/null | grep -Ei "^(steam|wine)" | head -10 || echo "  none via rpm"
	flatpak list 2>/dev/null | grep -iE "steam|wine" || true
} > "$OUT/62-32bit-compat.txt"

# --- benchmark tooling inventory ---------------------------------------
{
	echo "# Tools the benchmark needs.  Nothing is installed by this script."
	for t in perf turbostat hackbench schbench stress-ng sysbench \
		 bpftool numactl cpupower lsof dmidecode rpmbuild jq bc python3; do
		printf '%-12s %s\n' "$t" "$(command -v $t 2>/dev/null || echo MISSING)"
	done
	echo
	echo "# perf paranoid level (needs <= 1 for most counters, <= 0 or"
	echo "# CAP_PERFMON for some; -1 disables the restriction entirely):"
	cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null
	echo
	echo "# NMI watchdog (adds a per-CPU perf event; usually disabled for"
	echo "# benchmarking):"
	cat /proc/sys/kernel/nmi_watchdog 2>/dev/null
} > "$OUT/70-tooling.txt"

# --- background noise sources ------------------------------------------
{
	echo "# Services most likely to contaminate a desktop benchmark:"
	for u in tracker-miner-fs-3 tracker-extract-3 packagekit dnf-makecache \
		 fwupd-refresh abrtd rpm-ostreed flatpak-system-helper \
		 systemd-oomd thermald power-profiles-daemon tuned; do
		printf '%-28s %s\n' "$u" \
			"$(systemctl is-active "$u" 2>/dev/null || echo '-')"
	done
	echo
	echo "# active timers:"
	systemctl list-timers --all --no-pager 2>/dev/null | head -20
	echo
	echo "# load average right now: $(cat /proc/loadavg)"
} > "$OUT/71-background.txt"

# --- resolved config diagnostics ---------------------------------------
#
# Any of these being =y in the running kernel means the baseline is already
# paying for diagnostics, and a "faster" result might only be their removal.
if [ -s "$OUT/10-running-config" ] &&
   ! grep -q "NO RUNNING CONFIG" "$OUT/10-running-config"; then
	{
		echo "# Debug/diagnostic options in the RUNNING kernel."
		echo "# Any =y here inflates the baseline."
		for s in DEBUG_PREEMPT DEBUG_VM DEBUG_VM_PGFLAGS DEBUG_ENTRY \
			 X86_DEBUG_FPU PROVE_LOCKING LOCK_STAT LOCKDEP KASAN \
			 KCSAN KCOV UBSAN DEBUG_LIST DEBUG_OBJECTS \
			 DEBUG_KMEMLEAK SCHEDSTATS SCHED_DEBUG SCHED_INFO \
			 TASK_DELAY_ACCT TASKSTATS PSI PSI_DEFAULT_DISABLED \
			 SCHED_CLASS_EXT SCHED_CACHE CGROUP_CPUACCT \
			 FAIR_GROUP_SCHED SCHED_MM_CID FUNCTION_TRACER \
			 FTRACE DYNAMIC_FTRACE PREEMPT_LAZY PREEMPT_DYNAMIC \
			 HZ_250 HZ_300 HZ_1000 NO_HZ_IDLE NO_HZ_FULL \
			 X86_AMD_PSTATE X86_NATIVE_CPU IA32_EMULATION \
			 X86_X32_ABI RANDOMIZE_BASE RETPOLINE; do
			printf '%-28s %s\n' "CONFIG_$s" \
				"$(grep -E "^(# )?CONFIG_$s[= ]" \
				   "$OUT/10-running-config" | head -1 ||
				   echo 'not present')"
		done
	} > "$OUT/80-config-diagnostics.txt"
else
	echo "SKIPPED: no running config available" \
		> "$OUT/80-config-diagnostics.txt"
fi

# --- summary -----------------------------------------------------------
{
	echo "==================== BASELINE SUMMARY ===================="
	echo
	echo "kernel:    $(uname -r)"
	echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
	echo "cores:     $(nproc) online"
	echo "cgroup:    $(stat -fc %T /sys/fs/cgroup 2>/dev/null)"
	echo "cpufreq:   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo absent)"
	echo "governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo absent)"
	echo "epp:       $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null || echo absent)"
	echo "smt:       $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo absent)"
	echo -n "llc domains: "
	cat /sys/devices/system/cpu/cpu[0-9]*/cache/index3/shared_cpu_list \
		2>/dev/null | sort -u | wc -l
	echo "psi:       $([ -r /proc/pressure/cpu ] && echo present || echo absent)"
	echo "oomd:      $(systemctl is-active systemd-oomd 2>/dev/null || echo '-')"
	echo "sched_ext: $([ -d /sys/kernel/sched_ext ] && echo present || echo absent)"
	echo
	echo "config:    $(if grep -q 'NO RUNNING CONFIG' "$OUT/10-running-config" 2>/dev/null; then echo 'NOT FOUND -- BLOCKER'; else echo "captured ($(wc -l < "$OUT/10-running-config") lines)"; fi)"
	echo
	echo "missing tools:"
	grep MISSING "$OUT/70-tooling.txt" | sed 's/^/  /' || echo "  none"
	echo
	echo "=========================================================="
} | tee "$OUT/99-SUMMARY.txt"

say ""
say "Done.  Archive and share the whole directory:"
say "    tar czf ${OUT}.tar.gz $OUT"
say ""
if grep -q "NO RUNNING CONFIG" "$OUT/10-running-config" 2>/dev/null; then
	say "BLOCKER: no kernel config was found."
	say "  Enable CONFIG_IKCONFIG_PROC, or install the kernel-core package"
	say "  that provides /boot/config-\$(uname -r), or point at the Finux"
	say "  build tree's .config.  Without it there is no baseline."
fi
