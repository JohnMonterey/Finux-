#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Apply the Finux performance configuration to an existing kernel config.
#
# The Kconfig defaults Finux changes only affect configurations generated
# from scratch.  An existing .config - a distribution config, or one you
# have been carrying for a while - keeps whatever value it already has for
# every symbol it mentions, so the defaults never reach it.  This script
# is how those systems get the same treatment.
#
# It applies kernel/configs/finux-performance.config and, on request,
# kernel/configs/finux-desktop.config, then proves each option actually
# took the value it was given.  That last part is the reason this exists
# rather than a line in a README: Kconfig silently re-selects symbols
# through reverse dependencies, and an option that quietly came back is
# worse than one that was never set, because you will benchmark as though
# it had applied.
#
# Usage:
#   tools/testing/finux-perf/optimize-config.sh [options] <config>
#
#   --desktop   also apply finux-desktop.config (trades features - read it)
#   --native    add CONFIG_X86_NATIVE_CPU=y (kernel runs only on this CPU)
#   --output F  write to F instead of <config>.finux
#   --in-place  overwrite <config> (a .bak copy is kept)
#
# Builds nothing.  Never modifies a config in place unless asked.

set -u

SRC="$(cd "$(dirname "$0")/../../.." && pwd)"

DESKTOP=0
NATIVE=0
INPLACE=0
OUTPUT=""
CFG=""

usage() {
	sed -n '3,28p' "$0" | sed 's/^# \?//'
	exit "${1:-1}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--desktop)  DESKTOP=1 ;;
	--native)   NATIVE=1 ;;
	--in-place) INPLACE=1 ;;
	--output)   shift; OUTPUT="${1:-}" ;;
	-h|--help)  usage 0 ;;
	-*)         echo "unknown option: $1" >&2; usage ;;
	*)          CFG="$1" ;;
	esac
	shift
done

if [ -z "$CFG" ]; then
	cat >&2 <<-EOF
	error: no configuration given

	Pass the config your working kernel was built from:

	    /proc/config.gz          (zcat it out first)
	    /boot/config-\$(uname -r)
	    <build>/.config

	Do not pass arch/x86/configs/x86_64_defconfig.  It is not what any
	distribution ships, and a kernel built from it will be missing
	drivers and filesystems your system needs to boot.
	EOF
	exit 1
fi

[ -r "$CFG" ] || { echo "error: cannot read $CFG" >&2; exit 1; }

case "$CFG" in
*.gz) echo "error: decompress it first: zcat $CFG > my.config" >&2; exit 1 ;;
esac

grep -q '^CONFIG_' "$CFG" || {
	echo "error: $CFG does not look like a kernel configuration" >&2
	exit 1
}

FRAGMENTS=("$SRC/kernel/configs/finux-performance.config")
[ "$DESKTOP" = 1 ] &&
	FRAGMENTS+=("$SRC/kernel/configs/finux-desktop.config")

for f in "${FRAGMENTS[@]}"; do
	[ -r "$f" ] || { echo "error: missing fragment $f" >&2; exit 1; }
done

CFG_ABS="$(cd "$(dirname "$CFG")" && pwd)/$(basename "$CFG")"
if [ "$INPLACE" = 1 ]; then
	OUT_ABS="$CFG_ABS"
elif [ -n "$OUTPUT" ]; then
	mkdir -p "$(dirname "$OUTPUT")" 2>/dev/null
	OUT_ABS="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
else
	OUT_ABS="$CFG_ABS.finux"
fi

# kbuild refuses an O= build while the source tree holds in-tree build
# output, and the error it prints names mrproper - not something this
# script should run unasked in a tree holding someone's work.
if [ -e "$SRC/.config" ] || [ -d "$SRC/include/config" ]; then
	cat >&2 <<-EOF
	error: the source tree contains in-tree build output
	         $SRC/.config and/or $SRC/include/config/

	This script resolves the config out of tree and kbuild will refuse.
	Clearing those is your call:

	    cd $SRC && git status --short     # confirm nothing of yours
	    cd $SRC && make mrproper
	EOF
	exit 1
fi

WORK="$(mktemp -d)" || exit 1
trap 'rm -rf "$WORK"' EXIT

echo "source tree : $SRC"
echo "input       : $CFG_ABS"
echo "output      : $OUT_ABS"
echo "fragments   : $(printf '%s ' "${FRAGMENTS[@]##*/}")"
echo

# ----------------------------------------------------------------------
# Does the hardware match what the desktop fragment assumes?
# ----------------------------------------------------------------------
#
# One entry in finux-desktop.config is only correct on a machine with a
# single last-level cache.  Getting that wrong costs real performance on a
# multi-CCD or multi-socket box, so check rather than assume - but only
# when the check is meaningful, which is when this script is running on
# the machine the kernel is for.

llc_count() {
	local d out
	out=$(for d in /sys/devices/system/cpu/cpu*/cache/index*/; do
		[ -r "$d/level" ] || continue
		[ -r "$d/shared_cpu_list" ] || continue
		# The LLC is the highest-level unified/data cache present.
		echo "$(cat "$d/level") $(cat "$d/shared_cpu_list")"
	done 2>/dev/null)
	[ -n "$out" ] || return 1
	local top
	top=$(echo "$out" | awk '{print $1}' | sort -rn | head -1)
	echo "$out" | awk -v t="$top" '$1 == t {print $2}' | sort -u | wc -l
}

if [ "$DESKTOP" = 1 ]; then
	if n=$(llc_count) && [ "$n" -ge 1 ] 2>/dev/null; then
		if [ "$n" -eq 1 ]; then
			echo "topology    : 1 last-level cache - SCHED_CACHE=n is correct here"
		else
			cat <<-EOF
			topology    : $n last-level caches detected

			  finux-desktop.config disables CONFIG_SCHED_CACHE, which is
			  only free on a single-LLC machine.  On this one the
			  cache-aware balancer has somewhere to move tasks to and
			  turning it off will cost you.

			  Re-enable it after this script finishes:
			      scripts/config --file $OUT_ABS --enable SCHED_CACHE

			EOF
		fi
	else
		echo "topology    : cannot read cache topology here"
		echo "              (this is not the target machine, or sysfs is not mounted)"
		echo "              Verify SCHED_CACHE by hand - see finux-desktop.config."
	fi
	echo
fi

# ----------------------------------------------------------------------
# Merge and resolve
# ----------------------------------------------------------------------
#
# Everything happens inside the throwaway build directory.  Pointing
# KCONFIG_CONFIG at a path outside a build tree makes kbuild scatter
# include/config/ and .config through the source tree, after which every
# later O= build fails.

BDIR="$WORK/build"
mkdir -p "$BDIR"
cp "$CFG_ABS" "$BDIR/.config"

# merge_config.sh creates its scratch file in the current directory, so
# run it from the throwaway one rather than leaving .tmp.config.* litter
# in the source tree if it is interrupted.
if ! (cd "$WORK" && "$SRC/scripts/kconfig/merge_config.sh" -m -O "$BDIR" \
      "$BDIR/.config" "${FRAGMENTS[@]}") > "$WORK/merge.log" 2>&1; then
	echo "error: merge_config.sh failed:" >&2
	sed 's/^/  /' "$WORK/merge.log" >&2
	exit 1
fi

# merge_config.sh reports every symbol whose value differs from the base
# config as "redefined", which is the normal case here and says nothing.
# The applied/blocked table below is the real report.

[ "$NATIVE" = 1 ] &&
	"$SRC/scripts/config" --file "$BDIR/.config" --enable X86_NATIVE_CPU

if ! make -C "$SRC" O="$BDIR" olddefconfig > "$WORK/olddefconfig.log" 2>&1; then
	echo "error: olddefconfig failed:" >&2
	tail -20 "$WORK/olddefconfig.log" | sed 's/^/  /' >&2
	exit 1
fi

# ----------------------------------------------------------------------
# Prove it
# ----------------------------------------------------------------------

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

# Every CONFIG_ line the fragments asked for, as "SYM want".
#
# A symbol may appear in more than one fragment.  Report it once, with the
# value the last fragment gave it, which is the one merge_config.sh kept.
requested() {
	local f
	{
		for f in "${FRAGMENTS[@]}"; do
			sed -nE 's/^CONFIG_([A-Z0-9_]+)=(.*)$/\1 \2/p;
			         s/^# CONFIG_([A-Z0-9_]+) is not set$/\1 n/p' "$f"
		done
		[ "$NATIVE" = 1 ] && echo "X86_NATIVE_CPU y"
	} | awk '{ if (!($1 in v)) order[++n] = $1; v[$1] = $2 }
	         END { for (i = 1; i <= n; i++) print order[i], v[order[i]] }'
}

applied=0
skipped=0
blocked=0

echo "applied:"
while read -r sym want; do
	[ -n "$sym" ] || continue
	got=$(value_of "$sym" "$BDIR/.config")
	was=$(value_of "$sym" "$CFG_ABS")

	if [ "$got" = "$want" ]; then
		if [ "$was" = "$want" ]; then
			skipped=$((skipped + 1))
		else
			printf '  %-42s %s -> %s\n' "CONFIG_$sym" "$was" "$got"
			applied=$((applied + 1))
		fi
	elif [ "$got" = "absent" ] && [ "$want" = "n" ]; then
		# Not in the resolved config and we wanted it off.  Its
		# dependencies are unmet, which is the same outcome.
		skipped=$((skipped + 1))
	else
		printf '  BLOCKED  %-33s want=%-4s got=%s\n' \
			"CONFIG_$sym" "$want" "$got"
		if [ "$got" = "absent" ]; then
			echo "           dependencies unmet, so Kconfig dropped it"
		else
			echo "           a reverse dependency forces this value;"
			echo "           grep the tree for 'select $sym'"
		fi
		blocked=$((blocked + 1))
	fi
done < <(requested)

[ "$applied" = 0 ] && echo "  (nothing changed - this config was already tuned)"
echo
echo "  $applied changed, $skipped already correct, $blocked blocked"
echo

# ----------------------------------------------------------------------
# Did anything we promised not to touch move?
# ----------------------------------------------------------------------

declare -a INVARIANTS=(
	IA32_EMULATION COMPAT X86_X32_ABI
	CPU_IDLE CPU_FREQ SCHED_SMT
	CGROUPS CGROUP_SCHED FAIR_GROUP_SCHED CFS_BANDWIDTH SCHED_AUTOGROUP
	SCHED_MM_CID MEMCG PSI
	KVM PERF_EVENTS BPF_SYSCALL SECCOMP
	RETPOLINE MITIGATION_RETPOLINE MITIGATION_RETHUNK CPU_MITIGATIONS
	MITIGATION_SRSO MITIGATION_SPECTRE_BHI MITIGATION_IBPB_ENTRY
	RANDOMIZE_BASE STRICT_KERNEL_RWX MODULE_SIG SECURITY_SELINUX
	MODULES BLK_DEV_INITRD EFI EFI_STUB
)

moved=0
for sym in "${INVARIANTS[@]}"; do
	b=$(value_of "$sym" "$CFG_ABS")
	v=$(value_of "$sym" "$BDIR/.config")
	if [ "$b" != "$v" ]; then
		[ "$moved" = 0 ] && echo "INVARIANT MOVED - this is a bug, do not use this config:"
		printf '  CONFIG_%-34s %s -> %s\n' "$sym" "$b" "$v"
		moved=1
	fi
done

if [ "$moved" = 1 ]; then
	echo
	echo "Refusing to write the output.  Report this with the two configs."
	exit 1
fi
echo "invariants  : 32-bit support, mitigations, cgroups, PSI, KVM, perf,"
echo "              SELinux, module signing and Secure Boot all unchanged"
echo

# ----------------------------------------------------------------------
# Write it out
# ----------------------------------------------------------------------

if [ "$INPLACE" = 1 ] && [ -e "$OUT_ABS" ]; then
	cp "$OUT_ABS" "$OUT_ABS.bak" || exit 1
	echo "backup      : $OUT_ABS.bak"
fi
cp "$BDIR/.config" "$OUT_ABS" || exit 1

if [ -x "$SRC/scripts/diffconfig" ]; then
	"$SRC/scripts/diffconfig" "$CFG_ABS" "$OUT_ABS" > "$WORK/diff" 2>&1
	n=$(grep -c . "$WORK/diff" 2>/dev/null || echo 0)
	echo "full diff   : $n line(s), including everything Kconfig pulled along"
	[ "$n" -gt 0 ] && [ "$n" -le 60 ] && sed 's/^/  /' "$WORK/diff"
fi
echo
echo "wrote $OUT_ABS"

# ----------------------------------------------------------------------
# What a config cannot express
# ----------------------------------------------------------------------

cat <<-EOF

	==============================================================
	Not everything worth changing is a build option.  These are
	runtime, they are reversible, and several are larger than
	anything above.  Test them one at a time.

	  Boot parameters
	    audit=0                Once auditd has run, every fork
	                           allocates an audit context and marks
	                           the child for syscall auditing, for
	                           the life of that task.  Costly under
	                           Wine and Proton.  You lose audit
	                           records; SELinux denials still reach
	                           dmesg.
	    cpuidle.governor=teo   If you kept the menu governor.
	    init_on_alloc=0        Stops zeroing every allocation.  This
	                           is a hardening feature - it turns
	                           uninitialised-heap reads into zeros -
	                           so weigh it rather than just taking it.

	  Live, no reboot
	    echo performance > /sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference
	    echo NO_HRTICK  > /sys/kernel/debug/sched/features

	  BIOS - check this before any of the above
	    cat /sys/devices/system/cpu/cpufreq/policy0/scaling_driver
	    Anything other than amd-pstate-epp on a Zen CPU usually means
	    CPPC is disabled in firmware, and every frequency decision is
	    going through a 1000 us shared-memory path.  Enabling CPPC in
	    the BIOS is likely worth more than this whole script.

	==============================================================
	Then measure.  Build it, boot it, and run:

	    tools/testing/finux-perf/bench-kernel.sh
	    tools/testing/finux-perf/analyze.py <baseline> <this>

	Run the same kernel twice under two tags first.  The largest
	difference that shows up in that A/A run is this machine's real
	noise floor, and no claim below it means anything.
EOF
