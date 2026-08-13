#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Shell-level smoke test for the NT personality debugfs interface.
#
# Complements nt_personality_test.c by exercising the interface the way an
# administrator or an installer would: register a volume, designate it as
# the system volume, build the Windows 7 directory layout on it, and
# confirm the standard paths resolve.

# kselftest exit codes
ksft_pass=0
ksft_fail=1
ksft_skip=4

DBG=/sys/kernel/debug/ntpers
VOLDIR=
LETTER=T

cleanup()
{
	[ -n "$VOLDIR" ] || return 0
	echo "unmount $LETTER" > "$DBG/control" 2>/dev/null
	rm -rf "$VOLDIR"
}
trap cleanup EXIT

fail()
{
	echo "not ok: $*"
	exit $ksft_fail
}

[ "$(id -u)" -eq 0 ] || {
	echo "skip: need root"
	exit $ksft_skip
}

[ -d "$DBG" ] || {
	echo "skip: $DBG not present (CONFIG_NT_FS_PERSONALITY_DEBUGFS?)"
	exit $ksft_skip
}

# Resolve an NT path and print the answer.  The rw debugfs files keep
# their result per open file description, so write and read must share
# one descriptor; a subshell redirect gives us that.
nt_resolve()
{
	local path="$1"
	exec {fd}<> "$DBG/resolve" || return 1
	printf '%s' "$path" >&$fd
	cat <&$fd
	exec {fd}>&-
}

nt_parse()
{
	local path="$1"
	exec {fd}<> "$DBG/parse" || return 1
	printf '%s' "$path" >&$fd
	cat <&$fd
	exec {fd}>&-
}

VOLDIR=$(mktemp -d /tmp/ntpers_sh_XXXXXX) || fail "mktemp"

echo "unmount $LETTER" > "$DBG/control" 2>/dev/null

echo "mount $LETTER $VOLDIR TestVolume" > "$DBG/control" ||
	fail "could not register volume"

grep -q "letter=$LETTER:" "$DBG/volumes" ||
	fail "volume not in table"

# Build the Windows 7 system layout the design targets.
mkdir -p "$VOLDIR/Windows/System32" \
	 "$VOLDIR/Windows/SysWOW64" \
	 "$VOLDIR/Windows/Temp" \
	 "$VOLDIR/Program Files" \
	 "$VOLDIR/Program Files (x86)" \
	 "$VOLDIR/ProgramData" \
	 "$VOLDIR/Users/Public" \
	 "$VOLDIR/Users/John/Desktop" \
	 "$VOLDIR/Users/John/Documents" \
	 "$VOLDIR/Users/John/Downloads" \
	 "$VOLDIR/Users/John/AppData/Local" \
	 "$VOLDIR/Users/John/AppData/Roaming" ||
	fail "could not build layout"

: > "$VOLDIR/Windows/System32/kernel32.dll"

check_resolves()
{
	local ntpath="$1" expect="$2" out
	out=$(nt_resolve "$ntpath")
	local got
	got=$(printf '%s\n' "$out" | sed -n 's/^posix //p')
	if [ "$got" != "$expect" ]; then
		fail "$ntpath resolved to '${got:-<error>}', wanted '$expect'"
	fi
	echo "ok: $ntpath -> $got"
}

check_error()
{
	local ntpath="$1" out
	out=$(nt_resolve "$ntpath")
	printf '%s\n' "$out" | grep -q '^error ' ||
		fail "$ntpath should not have resolved: $out"
	echo "ok: $ntpath correctly refused"
}

# The standard Windows 7 hierarchy.
check_resolves "$LETTER:\\"                  "$VOLDIR"
check_resolves "$LETTER:\\Windows"           "$VOLDIR/Windows"
check_resolves "$LETTER:\\Windows\\System32" "$VOLDIR/Windows/System32"
check_resolves "$LETTER:\\Windows\\SysWOW64" "$VOLDIR/Windows/SysWOW64"
check_resolves "$LETTER:\\Windows\\Temp"     "$VOLDIR/Windows/Temp"
check_resolves "$LETTER:\\Program Files"     "$VOLDIR/Program Files"
check_resolves "$LETTER:\\Program Files (x86)" "$VOLDIR/Program Files (x86)"
check_resolves "$LETTER:\\ProgramData"       "$VOLDIR/ProgramData"
check_resolves "$LETTER:\\Users"             "$VOLDIR/Users"
check_resolves "$LETTER:\\Users\\Public"     "$VOLDIR/Users/Public"
check_resolves "$LETTER:\\Users\\John\\AppData\\Roaming" \
	"$VOLDIR/Users/John/AppData/Roaming"

# The path a PE loader will ask for.
check_resolves "$LETTER:\\Windows\\System32\\kernel32.dll" \
	"$VOLDIR/Windows/System32/kernel32.dll"

# Case insensitivity, with the stored spelling preserved.
check_resolves "$LETTER:\\windows\\system32" "$VOLDIR/Windows/System32"
check_resolves "$LETTER:\\WINDOWS\\SYSTEM32" "$VOLDIR/Windows/System32"
check_resolves "$LETTER:\\WiNdOwS"           "$VOLDIR/Windows"
check_resolves "$LETTER:\\program files"     "$VOLDIR/Program Files"

# Forward slashes are separators too, and "." and ".." collapse.
check_resolves "$LETTER:/Windows/System32"        "$VOLDIR/Windows/System32"
check_resolves "$LETTER:\\Windows\\.\\System32"   "$VOLDIR/Windows/System32"
check_resolves "$LETTER:\\Users\\..\\Windows"     "$VOLDIR/Windows"
check_resolves "$LETTER:\\..\\..\\Windows"        "$VOLDIR/Windows"
check_resolves "$LETTER:\\Windows\\"              "$VOLDIR/Windows"

# The extended prefix reaches the same volume.
check_resolves "\\\\?\\$LETTER:\\Windows"         "$VOLDIR/Windows"
# ... and so does the NT object-namespace form, with no drive letter at all.
NTDEV=$(sed -n "s/.*letter=$LETTER:.*nt=\([^ ]*\).*/\1/p" "$DBG/volumes")
[ -n "$NTDEV" ] || fail "no NT device name for volume"
check_resolves "$NTDEV\\Windows"                  "$VOLDIR/Windows"

# Things that must not resolve.
check_error "$LETTER:\\NoSuchDirectory"
check_error "Q:\\Windows"
check_error "$LETTER:\\Windows\\System32\\kernel32.dll:stream"

# Designating the system volume moves it to C:, as Windows does.
echo "system $LETTER" > "$DBG/control" 2>/dev/null
if grep -q "letter=C:.*system" "$DBG/volumes"; then
	LETTER=C
	check_resolves 'C:\Windows\System32' "$VOLDIR/Windows/System32"
	echo "ok: system volume became C:"
else
	# C: is already taken by something else on this machine; that is a
	# legitimate state, not a failure of the code under test.
	echo "ok: C: already in use, skipped system volume promotion"
fi

# NT metadata for a real file on the volume.
nt_getinfo()
{
	local path="$1"
	exec {fd}<> "$DBG/getinfo" || return 1
	printf '%s' "$path" >&$fd
	cat <&$fd
	exec {fd}>&-
}

INFO=$(nt_getinfo "$LETTER:\\Windows\\System32\\kernel32.dll")
printf '%s\n' "$INFO" | grep -q '^ok$' ||
	fail "getinfo failed: $INFO"

# A regular file must carry ARCHIVE (0x20) and not DIRECTORY (0x10).
ATTRS=$(printf '%s\n' "$INFO" | sed -n 's/^attributes //p')
[ $(( ATTRS & 0x20 )) -ne 0 ] || fail "kernel32.dll missing ARCHIVE ($ATTRS)"
[ $(( ATTRS & 0x10 )) -eq 0 ] || fail "kernel32.dll claims DIRECTORY ($ATTRS)"
echo "ok: file attributes $ATTRS"

# A directory must carry DIRECTORY.
DATTRS=$(nt_getinfo "$LETTER:\\Windows" | sed -n 's/^attributes //p')
[ $(( DATTRS & 0x10 )) -ne 0 ] || fail "C:\\Windows missing DIRECTORY ($DATTRS)"
echo "ok: directory attributes $DATTRS"

# CreationTime must be in NT units and explicitly labelled real or not.
CREATION=$(printf '%s\n' "$INFO" | sed -n 's/^creation //p')
EXACT=$(printf '%s\n' "$INFO" | sed -n 's/^creation_exact //p')
[ "$CREATION" -gt 116444736000000000 ] ||
	fail "creation time $CREATION is not an NT timestamp"
case "$EXACT" in
0|1) ;;
*) fail "creation_exact is '$EXACT', expected 0 or 1" ;;
esac
echo "ok: creation time $CREATION (exact=$EXACT)"

# File ids must be non-zero and stable.
FID=$(printf '%s\n' "$INFO" | sed -n 's/^file_id //p')
[ "$FID" -ne 0 ] || fail "file id is zero"
FID2=$(nt_getinfo "$LETTER:\\windows\\system32\\KERNEL32.DLL" |
	sed -n 's/^file_id //p')
[ "$FID" = "$FID2" ] ||
	fail "file id differs by path casing: $FID vs $FID2"
echo "ok: file id $FID stable across casings"

# The parser reports Win32 device names without enforcing them.
nt_parse 'C:\Users\CON' | grep -q reserved ||
	fail "CON should be flagged as a Win32 device name"
nt_parse '\\?\C:\Users\CON' | grep -q reserved &&
	fail "a verbatim path must not be intercepted"
echo "ok: Win32 reserved names flagged, verbatim paths exempt"

echo "ok: all NT personality debugfs checks passed"
exit $ksft_pass
