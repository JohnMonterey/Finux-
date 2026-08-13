#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Generate and validate the performance config variants.
#
# Each variant isolates one change so it can be measured on its own, and
# the combined config applies everything that survived measurement.
#
# The important work here is not setting the options - scripts/config does
# that - but proving afterwards that each option actually took the value we
# asked for.  A symbol with a reverse dependency will silently come back
# after olddefconfig, and a variant that did not really change anything
# will otherwise be benchmarked as if it had, producing a confident
# measurement of nothing.
#
# Usage:
#   tools/testing/finux-perf/gen-variants.sh <baseline.config> [outdir]
#
# Builds nothing.  Writes only into the output directory.

set -u

BASE="${1:-}"
OUT="${2:-finux-configs}"
SRC="$(cd "$(dirname "$0")/../../.." && pwd)"

if [ -z "$BASE" ] || [ ! -r "$BASE" ]; then
	cat >&2 <<-EOF
	usage: $0 <baseline.config> [outdir]

	<baseline.config> must be the configuration the CURRENT working
	kernel was built from - /proc/config.gz, /boot/config-\$(uname -r),
	or the Finux build tree's .config.

	Do not pass arch/x86/configs/x86_64_defconfig.  It is not what any
	distribution ships and comparing against it measures the difference
	between a defconfig and a distro kernel, not the effect of a tuning
	change.
	EOF
	exit 1
fi

command -v "$SRC/scripts/config" >/dev/null 2>&1 ||
	[ -x "$SRC/scripts/config" ] || {
		echo "error: $SRC/scripts/config not found or not executable" >&2
		exit 1
	}

mkdir -p "$OUT" || exit 1
BASE_ABS="$(cd "$(dirname "$BASE")" && pwd)/$(basename "$BASE")"
OUT_ABS="$(cd "$OUT" && pwd)"

# kbuild refuses an O= build when the source tree contains in-tree build
# output, and the error names mrproper - which would be an alarming thing
# for this script to run unasked in a tree holding someone's work.  Detect
# it and let the user decide.
if [ -e "$SRC/.config" ] || [ -d "$SRC/include/config" ]; then
	cat >&2 <<-EOF
	error: the source tree contains in-tree build output
	         $SRC/.config and/or $SRC/include/config/

	kbuild will refuse the out-of-tree builds this script needs.  These
	are generated files, not source, but clearing them is your call:

	    cd $SRC && git status --short     # confirm nothing of yours
	    cd $SRC && make mrproper

	EOF
	exit 1
fi

echo "source tree : $SRC"
echo "baseline    : $BASE_ABS"
echo "output      : $OUT_ABS"
echo

# Variant definitions.  Each entry is "NAME|description|opt=val opt=val ..."
# where val is y, n, or a number.  Kept as data so the audit document and
# the script cannot drift apart.
VARIANTS=(
"baseline|unchanged reference|"
"A-schedcache|SCHED_CACHE off (single-LLC target)|SCHED_CACHE=n"
# LATENCYTOP selects SCHEDSTATS, so it has to go too or SCHEDSTATS=n
# silently fails.  Note this variant CANNOT reach SCHED_INFO=n while KVM
# is enabled - see PERFORMANCE_AUDIT.md section 3.1.
"B-schedstats|unconditional scheduler statistics off|TASK_DELAY_ACCT=n SCHEDSTATS=n LATENCYTOP=n"
"C-hz250|250Hz tick, lazy preemption|HZ_250=y HZ_1000=n HZ_300=n HZ_100=n NO_HZ_IDLE=y PREEMPT_LAZY=y"
"D-unused|unused scheduler/accounting features off|SCHED_CLASS_EXT=n CGROUP_CPUACCT=n"
# DEBUG_NET_SMALL_RTNL selects PROVE_LOCKING.  X86_DEBUG_FPU is
# "default y" and is on in x86_64_defconfig, so it is a live cost rather
# than a formality.
"E-nodebug|production diagnostics off|DEBUG_PREEMPT=n DEBUG_VM=n DEBUG_ENTRY=n X86_DEBUG_FPU=n PROVE_LOCKING=n LOCK_STAT=n KASAN=n KCSAN=n KCOV=n DEBUG_KMEMLEAK=n DEBUG_OBJECTS=n DEBUG_NET_SMALL_RTNL=n"
# X86_AMD_PSTATE, DEFAULT_MODE=3, CPU_IDLE and CPU_SUP_AMD are already
# forced or already the default on x86_64 (SCHED_MC_PRIO selects the
# first, which transitively forces CPU_IDLE; mode 3 is the Kconfig
# default; CPU_SUP_HYGON selects the last).  Setting them changes
# nothing, so this variant is really just -march=native.  They are still
# listed so the checker proves they hold rather than assuming it.
"F-native|build for this exact CPU (-march=native)|X86_NATIVE_CPU=y X86_AMD_PSTATE=y X86_AMD_PSTATE_DEFAULT_MODE=3 CPU_IDLE=y CPU_SUP_AMD=y"
"G-psi|PSI off by default (needs boot-param check first)|PSI_DEFAULT_DISABLED=y"
)

# Options that must never differ from the baseline, whatever a variant
# does.  Checked after every olddefconfig: a tuning pass that quietly turns
# off mitigations or 32-bit support is not a tuning pass.
declare -a INVARIANTS=(
	IA32_EMULATION
	COMPAT
	X86_X32_ABI
	CPU_IDLE
	CPU_FREQ
	SCHED_SMT
	CGROUPS
	CGROUP_SCHED
	FAIR_GROUP_SCHED
	SCHED_AUTOGROUP
	SCHED_MM_CID
	KVM
	MEMCG
	PERF_EVENTS
	RETPOLINE
	MITIGATION_RETPOLINE
	CPU_MITIGATIONS
	RANDOMIZE_BASE
	SECCOMP
	BPF_SYSCALL
)

value_of() {
	local sym="$1" cfg="$2" line
	line=$(grep -E "^(CONFIG_${sym}=|# CONFIG_${sym} is not set)" "$cfg" |
	       head -1)
	case "$line" in
		"") echo "absent" ;;
		"# CONFIG_${sym} is not set") echo "n" ;;
		*) echo "${line#CONFIG_${sym}=}" ;;
	esac
}

# Does the symbol exist in the tree at all?
#
# "Absent from the resolved config" and "does not exist" are different
# problems with different fixes, and reporting the second when it is really
# the first sends you looking for a symbol that is right there.  A symbol
# whose dependencies are unmet simply does not appear in .config.
symbol_in_tree() {
	local sym="$1"
	grep -rqE "^[[:space:]]*(menu)?config[[:space:]]+${sym}[[:space:]]*$" \
		"$SRC"/{init,kernel,mm,fs,lib,drivers,arch/x86,block,net,security} \
		--include='Kconfig*' 2>/dev/null
}

# Where a symbol is defined, and what it depends on - the information you
# actually need when an option refuses to apply.
explain_symbol() {
	local sym="$1" loc
	loc=$(grep -rnE "^[[:space:]]*(menu)?config[[:space:]]+${sym}[[:space:]]*$" \
		"$SRC"/{init,kernel,mm,fs,lib,drivers,arch/x86,block,net,security} \
		--include='Kconfig*' 2>/dev/null | head -1)
	[ -n "$loc" ] || return
	echo "               defined at ${loc%%:config*}" | sed 's|'"$SRC"'/||'
	local file="${loc%%:*}" line="${loc#*:}"
	line="${line%%:*}"
	sed -n "${line},$((line + 12))p" "$file" 2>/dev/null |
		grep -E "^[[:space:]]+(depends on|select|default)" |
		sed 's/^[[:space:]]*/               /'
}

overall_rc=0

for entry in "${VARIANTS[@]}"; do
	name="${entry%%|*}"
	rest="${entry#*|}"
	desc="${rest%%|*}"
	opts="${rest#*|}"

	cfg="$OUT_ABS/$name.config"
	cp "$BASE_ABS" "$cfg"

	echo "=============================================================="
	echo "variant: $name - $desc"
	echo "=============================================================="

	# Apply requested options.
	for kv in $opts; do
		sym="${kv%%=*}"
		val="${kv#*=}"
		case "$val" in
		y) "$SRC/scripts/config" --file "$cfg" --enable "$sym" ;;
		n) "$SRC/scripts/config" --file "$cfg" --disable "$sym" ;;
		*) "$SRC/scripts/config" --file "$cfg" --set-val "$sym" "$val" ;;
		esac
	done

	# Let Kconfig resolve dependencies.  This is where silent
	# re-selection happens, which is exactly what we check for next.
	#
	# Everything happens inside the build directory: the config goes in
	# as .config, kbuild resolves it there, and the result comes back
	# out.  Pointing KCONFIG_CONFIG at a path outside the build tree
	# makes kbuild scatter include/config/ and .config through the
	# source tree, after which it refuses every later O= build with
	# "the source tree is not clean" - so the second variant fails and
	# the first one silently was not resolved at all.
	bdir="$OUT_ABS/build-$name"
	mkdir -p "$bdir"
	cp "$cfg" "$bdir/.config"

	if ! make -C "$SRC" O="$bdir" olddefconfig \
	     > "$OUT_ABS/$name.olddefconfig.log" 2>&1; then
		echo "  ERROR: olddefconfig failed, see $name.olddefconfig.log"
		sed -n 's/^/    /p' "$OUT_ABS/$name.olddefconfig.log" | tail -6
		overall_rc=1
		continue
	fi
	cp "$bdir/.config" "$cfg"

	# Did every requested option actually stick?
	failed=0
	for kv in $opts; do
		sym="${kv%%=*}"
		want="${kv#*=}"
		got=$(value_of "$sym" "$cfg")
		if [ "$got" != "$want" ]; then
			printf '  NOT APPLIED  CONFIG_%-28s want=%-4s got=%s\n' \
				"$sym" "$want" "$got"
			if [ "$got" = "absent" ]; then
				if symbol_in_tree "$sym"; then
					echo "               symbol EXISTS but its dependencies are unmet,"
					echo "               so Kconfig dropped it from the resolved config."
					explain_symbol "$sym"
				else
					echo "               symbol DOES NOT EXIST in this tree"
				fi
			else
				echo "               a reverse dependency is forcing this value;"
				echo "               grep the tree for 'select $sym'"
			fi
			failed=1
			overall_rc=1
		else
			printf '  ok           CONFIG_%-28s = %s\n' "$sym" "$got"
		fi
	done
	[ "$failed" = 0 ] && [ -n "$opts" ] &&
		echo "  all requested options applied"

	# Did anything we promised not to touch move?
	for sym in "${INVARIANTS[@]}"; do
		b=$(value_of "$sym" "$BASE_ABS")
		v=$(value_of "$sym" "$cfg")
		if [ "$b" != "$v" ]; then
			printf '  INVARIANT MOVED  CONFIG_%-24s %s -> %s\n' \
				"$sym" "$b" "$v"
			overall_rc=1
		fi
	done

	# The real diff, including everything Kconfig pulled along.
	if [ "$name" != "baseline" ]; then
		"$SRC/scripts/diffconfig" "$OUT_ABS/baseline.config" "$cfg" \
			> "$OUT_ABS/$name.diff" 2>&1
		n=$(grep -c . "$OUT_ABS/$name.diff" 2>/dev/null || echo 0)
		echo "  resolved diff vs baseline: $n line(s) -> $name.diff"
		# Show it: an unexpectedly large diff means a dependency
		# cascade that has to be understood before benchmarking.
		[ "$n" -gt 0 ] && sed 's/^/    /' "$OUT_ABS/$name.diff"
	fi
	echo
done

# The combined configuration is deliberately NOT generated here.  It should
# contain only the variants that measurement actually justified, and that
# information does not exist until the benchmarks have run.
cat <<-EOF
==============================================================
Per-variant configs written to $OUT_ABS/

The combined configuration is intentionally not generated yet: it should
contain only the variants that measurement justified.  After benchmarking,
re-run with the surviving options merged into a single variant entry.

To turn a validated config into the target defconfig:
    make O=<build> KCONFIG_CONFIG=<validated.config> savedefconfig
    cp <build>/defconfig arch/x86/configs/finux_ryzen5500_defconfig
EOF

exit $overall_rc
