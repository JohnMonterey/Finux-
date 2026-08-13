// SPDX-License-Identifier: GPL-2.0
/*
 * Win32 reserved device names.
 *
 * This is a Win32 rule and nothing else, so it lives in its own file to
 * keep the layering honest:
 *
 *   - NTFS can store a file called "CON".  Nothing in the on-disk format
 *     objects, and fs/ntfs3 will read and write it happily.
 *
 *   - The NT Object Manager has device objects called \Device\Null and
 *     friends, reachable as \??\NUL.  They are objects in a namespace,
 *     not filenames, and the Object Manager does not go looking for them
 *     when parsing a filesystem path.
 *
 *   - It is kernel32's CreateFileW that inspects the last component of a
 *     Win32 path, notices "CON", and opens a device instead of touching
 *     the disk.  A "\\?\" path skips that inspection entirely, which is
 *     precisely how you create a file called CON on Windows.
 *
 * So the parser only ever *flags* these names.  Turning the flag into
 * behaviour is the job of a Win32 personality, which is the only layer
 * entitled to make that decision.
 */

#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/nt_personality.h>

#include "internal.h"

/*
 * The Windows 7 SP1 set, as documented in "Naming Files, Paths, and
 * Namespaces".  Later Windows releases added COM0/LPT1 superscript
 * variants and treat COM0/LPT0 as reserved; Windows 7 does not, and this
 * subsystem targets Windows 7 behaviour, so they are deliberately absent.
 *
 * CLOCK$ was reserved on MS-DOS and Windows 9x and is still a device
 * object on NT, but Win32 on Windows 7 does not intercept it as a
 * filename, so it is not listed here either.
 */
static const char * const nt_reserved_names[] = {
	"CON", "PRN", "AUX", "NUL",
};

/*
 * Devices that are a fixed prefix followed by a single digit 1-9.
 */
static const char * const nt_reserved_numbered[] = {
	"COM", "LPT",
};

/**
 * nt_name_is_reserved_device - does @name name a Win32 device?
 * @name: the final path component
 * @len:  its length in bytes
 *
 * Applies the Win32 matching rules, which are looser than simple string
 * equality in two ways that matter:
 *
 *   - Everything from the first '.' onwards is ignored, so "CON.txt",
 *     "NUL.log" and "COM1.anything" all name devices.
 *   - Trailing spaces are ignored, so "CON   " names a device.
 *
 * Matching is case-insensitive.  Only ASCII is considered, which is
 * correct: these names are compared by Win32 against ASCII literals.
 *
 * Returns true if a Win32 personality would open a device rather than a
 * file for this name.
 */
bool nt_name_is_reserved_device(const char *name, size_t len)
{
	const char *dot;
	size_t i;

	if (!name || !len)
		return false;

	/* Win32 looks only at the stem before the first '.'. */
	dot = memchr(name, '.', len);
	if (dot)
		len = dot - name;

	/* ... and ignores trailing spaces on that stem. */
	while (len > 0 && name[len - 1] == ' ')
		len--;

	if (len == 3) {
		for (i = 0; i < ARRAY_SIZE(nt_reserved_names); i++) {
			if (!strncasecmp(name, nt_reserved_names[i], 3))
				return true;
		}
		return false;
	}

	if (len == 4 && name[3] >= '1' && name[3] <= '9') {
		for (i = 0; i < ARRAY_SIZE(nt_reserved_numbered); i++) {
			if (!strncasecmp(name, nt_reserved_numbered[i], 3))
				return true;
		}
	}

	return false;
}
EXPORT_SYMBOL_GPL(nt_name_is_reserved_device);

/**
 * nt_name_is_valid_win32 - can Win32 create a file with this name?
 * @name: a single path component
 * @len:  its length in bytes
 *
 * Checks the character and length rules Win32 applies to a filename.
 * This is stricter than what the filesystem below can store, which is
 * intentional: it describes what a Win32 caller is allowed to ask for,
 * not what exists.
 *
 * Reserved device names are not rejected here; use
 * nt_name_is_reserved_device() for that, because whether a device name
 * is an error or a redirection depends on the operation.
 */
bool nt_name_is_valid_win32(const char *name, size_t len)
{
	size_t i;

	if (!name || !len || len > NT_MAX_COMPONENT)
		return false;

	/* "." and ".." are relative names, not filenames. */
	if (len == 1 && name[0] == '.')
		return false;
	if (len == 2 && name[0] == '.' && name[1] == '.')
		return false;

	for (i = 0; i < len; i++) {
		unsigned char c = name[i];

		if (c < 0x20)
			return false;
		switch (c) {
		case '<':
		case '>':
		case ':':
		case '"':
		case '/':
		case '\\':
		case '|':
		case '?':
		case '*':
			return false;
		}
	}

	/*
	 * Win32 silently trims trailing dots and spaces, so a name made
	 * entirely of them would normalise away to nothing.
	 */
	for (i = 0; i < len; i++) {
		if (name[i] != '.' && name[i] != ' ')
			return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(nt_name_is_valid_win32);
