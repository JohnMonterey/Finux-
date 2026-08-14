/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * NT filesystem personality - userspace ABI.
 *
 * Constants here mirror the on-the-wire/on-disk values that Windows 7 SP1
 * era software expects.  They are deliberately grouped by the layer that
 * actually defines them, because conflating those layers is the single
 * most common source of bugs in this area:
 *
 *   NTFS      - values stored in NTFS metadata on disk.
 *   NT        - values defined by the NT kernel / Object Manager.
 *   Win32     - values defined by kernel32/Win32 API rules only.
 *
 * A value marked Win32 must never be enforced by a filesystem or by the
 * NT namespace layer; it is the job of a Win32 personality to apply it.
 */
#ifndef _UAPI_LINUX_NT_PERSONALITY_H
#define _UAPI_LINUX_NT_PERSONALITY_H

#include <linux/types.h>

/*
 * ---------------------------------------------------------------------
 * DOS/NTFS file attributes  (layer: NTFS on-disk, $STANDARD_INFORMATION)
 * ---------------------------------------------------------------------
 *
 * These match fs/ntfs3's enum FILE_ATTRIBUTE exactly, in host byte order.
 * FILE_ATTRIBUTE_DIRECTORY is reported by NT from the directory bit in the
 * index entry rather than from $STANDARD_INFORMATION, but it is part of the
 * value returned to Win32 callers, so it lives here too.
 */
#define NT_FILE_ATTRIBUTE_READONLY		0x00000001
#define NT_FILE_ATTRIBUTE_HIDDEN		0x00000002
#define NT_FILE_ATTRIBUTE_SYSTEM		0x00000004
#define NT_FILE_ATTRIBUTE_DIRECTORY		0x00000010
#define NT_FILE_ATTRIBUTE_ARCHIVE		0x00000020
#define NT_FILE_ATTRIBUTE_DEVICE		0x00000040
#define NT_FILE_ATTRIBUTE_NORMAL		0x00000080
#define NT_FILE_ATTRIBUTE_TEMPORARY		0x00000100
#define NT_FILE_ATTRIBUTE_SPARSE_FILE		0x00000200
#define NT_FILE_ATTRIBUTE_REPARSE_POINT		0x00000400
#define NT_FILE_ATTRIBUTE_COMPRESSED		0x00000800
#define NT_FILE_ATTRIBUTE_OFFLINE		0x00001000
#define NT_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED	0x00002000
#define NT_FILE_ATTRIBUTE_ENCRYPTED		0x00004000

/*
 * Attributes a caller may set directly.  Win32 SetFileAttributes() rejects
 * everything else; DIRECTORY, SPARSE_FILE, REPARSE_POINT, COMPRESSED and
 * ENCRYPTED are owned by the filesystem and are only ever reported.
 */
#define NT_FILE_ATTRIBUTE_SETTABLE					\
	(NT_FILE_ATTRIBUTE_READONLY | NT_FILE_ATTRIBUTE_HIDDEN |	\
	 NT_FILE_ATTRIBUTE_SYSTEM | NT_FILE_ATTRIBUTE_ARCHIVE |		\
	 NT_FILE_ATTRIBUTE_NORMAL | NT_FILE_ATTRIBUTE_TEMPORARY |	\
	 NT_FILE_ATTRIBUTE_OFFLINE | NT_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)

/* Attributes the filesystem owns and a caller may only read. */
#define NT_FILE_ATTRIBUTE_FS_OWNED					\
	(NT_FILE_ATTRIBUTE_DIRECTORY | NT_FILE_ATTRIBUTE_DEVICE |	\
	 NT_FILE_ATTRIBUTE_SPARSE_FILE | NT_FILE_ATTRIBUTE_REPARSE_POINT | \
	 NT_FILE_ATTRIBUTE_COMPRESSED | NT_FILE_ATTRIBUTE_ENCRYPTED)

#define NT_FILE_ATTRIBUTE_VALID						\
	(NT_FILE_ATTRIBUTE_SETTABLE | NT_FILE_ATTRIBUTE_FS_OWNED)

/*
 * ---------------------------------------------------------------------
 * Reparse tags  (layer: NT kernel; storage: NTFS $REPARSE_POINT)
 * ---------------------------------------------------------------------
 */
#define NT_IO_REPARSE_TAG_RESERVED_ZERO		0x00000000
#define NT_IO_REPARSE_TAG_MOUNT_POINT		0xA0000003
#define NT_IO_REPARSE_TAG_HSM			0xC0000004
#define NT_IO_REPARSE_TAG_SIS			0x80000007
#define NT_IO_REPARSE_TAG_DFS			0x8000000A
#define NT_IO_REPARSE_TAG_FILTER_MANAGER	0x8000000B
#define NT_IO_REPARSE_TAG_SYMLINK		0xA000000C
#define NT_IO_REPARSE_TAG_DFSR			0x80000012
#define NT_IO_REPARSE_TAG_DEDUP			0x80000013

#define NT_IO_REPARSE_TAG_MICROSOFT		0x80000000
#define NT_IO_REPARSE_TAG_NAME_SURROGATE	0x20000000
#define NT_IO_REPARSE_TAG_DIRECTORY_BIT		0x10000000

/* SYMLINK reparse buffer flags. */
#define NT_SYMLINK_FLAG_RELATIVE		0x00000001

/* Largest reparse payload NT accepts (MAXIMUM_REPARSE_DATA_BUFFER_SIZE). */
#define NT_MAXIMUM_REPARSE_DATA_BUFFER_SIZE	16384

/*
 * ---------------------------------------------------------------------
 * Pathname limits
 * ---------------------------------------------------------------------
 *
 * NT_MAX_COMPONENT is an NTFS limit (255 UTF-16 code units per name).
 * NT_MAX_PATH is purely a Win32 limit; the NT kernel itself accepts up to
 * 32767 code units and \\?\ paths bypass MAX_PATH entirely.  Do not
 * enforce NT_MAX_PATH below the Win32 layer.
 */
#define NT_MAX_COMPONENT	255
#define NT_MAX_PATH		260
#define NT_MAX_NT_PATH		32767

/* Volume identity, mirroring what a Win32 caller can observe. */
#define NT_MAX_LABEL		32	/* NTFS volume label, UTF-16 units */
#define NT_MAX_FS_NAME		16

/*
 * ---------------------------------------------------------------------
 * Parsed pathname classification  (layer: Win32 + NT Object Manager)
 * ---------------------------------------------------------------------
 */
enum nt_path_type {
	/* Not a syntactically valid path. */
	NT_PATH_INVALID = 0,
	/* "C:\Windows" - rooted on an explicit drive. */
	NT_PATH_DRIVE_ABSOLUTE,
	/* "C:Windows" - relative to the caller's current directory on C:. */
	NT_PATH_DRIVE_RELATIVE,
	/* "\Windows" - rooted on the caller's current drive. */
	NT_PATH_ROOTED,
	/* "Windows" - relative to the caller's current directory. */
	NT_PATH_RELATIVE,
	/* "\\server\share\path" (also "\\?\UNC\server\share\path"). */
	NT_PATH_UNC,
	/* "\\.\PhysicalDrive0" - Win32 device namespace. */
	NT_PATH_DEVICE,
	/* "\??\C:\x" or "\Device\HarddiskVolume1\x" - NT Object Manager. */
	NT_PATH_NT_OBJECT,
};

/*
 * Flags describing what the parser found.  Returned to callers so a Win32
 * personality can apply its own rules without re-parsing.
 */
/* Path used a \\?\ or \??\ prefix: normalization must be suppressed. */
#define NT_PARSE_VERBATIM	(1U << 0)
/* Path ended in a separator, so the caller meant a directory. */
#define NT_PARSE_TRAILING_SEP	(1U << 1)
/* Final component carried a ":stream" suffix. */
#define NT_PARSE_HAS_STREAM	(1U << 2)
/* Final component names a Win32 reserved device (CON, NUL, COM1, ...). */
#define NT_PARSE_RESERVED_NAME	(1U << 3)
/* Path resolved to the root of its volume; there are no components. */
#define NT_PARSE_VOLUME_ROOT	(1U << 4)
/* Fully qualified form exceeds MAX_PATH; only meaningful to Win32. */
#define NT_PARSE_LONG_PATH	(1U << 5)
/* Normalization removed trailing dots/spaces from at least one component. */
#define NT_PARSE_TRIMMED	(1U << 6)

/*
 * Volume capability flags, as GetVolumeInformation() reports them in
 * lpFileSystemFlags.  Values are the Win32 FILE_* constants.
 *
 * These exist so that the filesystem *name* and the filesystem
 * *capabilities* can be answered separately, and honestly.  Reporting
 * "NTFS" is a compatibility decision - there is no Win32 vocabulary for
 * ext4, and Samba has answered this way for decades - but a caller that
 * asks whether the volume supports named streams or persistent ACLs is
 * asking a question with a real answer, and it must get the real one.
 * An application told it has reparse points will use them.
 */
#define NT_FS_CASE_SENSITIVE_SEARCH	0x00000001
#define NT_FS_CASE_PRESERVED_NAMES	0x00000002
#define NT_FS_UNICODE_ON_DISK		0x00000004
#define NT_FS_PERSISTENT_ACLS		0x00000008
#define NT_FS_FILE_COMPRESSION		0x00000010
#define NT_FS_VOLUME_QUOTAS		0x00000020
#define NT_FS_SUPPORTS_SPARSE_FILES	0x00000040
#define NT_FS_SUPPORTS_REPARSE_POINTS	0x00000080
#define NT_FS_SUPPORTS_OBJECT_IDS	0x00010000
#define NT_FS_SUPPORTS_ENCRYPTION	0x00020000
#define NT_FS_NAMED_STREAMS		0x00040000
#define NT_FS_READ_ONLY_VOLUME		0x00080000
#define NT_FS_SUPPORTS_TRANSACTIONS	0x00200000
#define NT_FS_SUPPORTS_HARD_LINKS	0x00400000
#define NT_FS_SUPPORTS_EXTENDED_ATTRIBUTES 0x00800000
#define NT_FS_SUPPORTS_OPEN_BY_FILE_ID	0x01000000
#define NT_FS_SUPPORTS_USN_JOURNAL	0x02000000

/*
 * Input flags for the parser.
 */
/* Treat the input as already verbatim (as if it had a \\?\ prefix). */
#define NT_PARSE_F_VERBATIM	(1U << 0)
/* Do not split a ":stream" suffix off the final component. */
#define NT_PARSE_F_NO_STREAM	(1U << 1)
/* Apply the Win32 MAX_PATH limit and reject longer paths. */
#define NT_PARSE_F_WIN32_LIMITS	(1U << 2)
/* Reject Win32 reserved device names instead of just flagging them. */
#define NT_PARSE_F_REJECT_DEVICE (1U << 3)

/*
 * ---------------------------------------------------------------------
 * Named streams  (layer: NTFS attribute types)
 * ---------------------------------------------------------------------
 *
 * A Win32 path may name a stream as "file:stream:$TYPE".  The type suffix
 * selects an NTFS attribute type; "$DATA" is the only one that can hold
 * caller-supplied bytes.  "file::$DATA" names the unnamed default stream,
 * which is the same object as "file".
 */
#define NT_STREAM_TYPE_DATA		0x00000080	/* $DATA */
#define NT_STREAM_TYPE_INDEX_ALLOCATION	0x000000A0	/* $INDEX_ALLOCATION */
#define NT_STREAM_TYPE_BITMAP		0x000000B0	/* $BITMAP */

/*
 * ---------------------------------------------------------------------
 * Create / open  (layer: NT kernel - NtCreateFile / Win32 CreateFile)
 * ---------------------------------------------------------------------
 *
 * Win32 CreateFile packs five behaviours into one call.  The one that
 * decides whether a file is created, opened, or truncated is the
 * "creation disposition"; these are the CreateFile dwCreationDisposition
 * values, kept at their Win32 numbering so a personality shim can pass
 * them straight through.
 */
#define NT_DISPOSITION_CREATE_NEW	1	/* create; fail if it exists */
#define NT_DISPOSITION_CREATE_ALWAYS	2	/* create, or truncate if it exists */
#define NT_DISPOSITION_OPEN_EXISTING	3	/* open; fail if it is absent */
#define NT_DISPOSITION_OPEN_ALWAYS	4	/* open, or create if it is absent */
#define NT_DISPOSITION_TRUNCATE_EXISTING 5	/* open and truncate; fail if absent */

/*
 * What actually happened, reported the way NtCreateFile reports it in its
 * IoStatusBlock.Information field.  A caller uses this to tell CREATE_ALWAYS
 * that made a new file from CREATE_ALWAYS that replaced one.
 */
#define NT_RESULT_CREATED		1	/* FILE_CREATED */
#define NT_RESULT_OPENED		2	/* FILE_OPENED */
#define NT_RESULT_OVERWRITTEN		3	/* FILE_OVERWRITTEN */

/*
 * Create options.  A subset of NtCreateFile's CreateOptions and the
 * FILE_FLAG_* bits, limited to what changes name resolution or what is
 * created rather than how bytes are later read.
 */
/* The object being created or opened must be a directory. */
#define NT_CREATE_DIRECTORY		(1U << 0)
/* Open a reparse point itself rather than following it (FILE_OPEN_REPARSE_POINT). */
#define NT_CREATE_OPEN_REPARSE		(1U << 1)
/* Delete the file when the last handle to it closes (FILE_DELETE_ON_CLOSE). */
#define NT_CREATE_DELETE_ON_CLOSE	(1U << 2)

/*
 * Access rights, at their Win32 numbering.  A handle records the access
 * it was granted; the sharing rules below are enforced against it.  Only
 * the rights that bear on sharing and on the operations this layer
 * performs are called out - the read/write/delete distinction is what the
 * share check turns on.  GENERIC_* are the caller-facing masks that map
 * onto the specific rights.
 */
#define NT_ACCESS_FILE_READ_DATA	0x00000001
#define NT_ACCESS_FILE_WRITE_DATA	0x00000002
#define NT_ACCESS_FILE_APPEND_DATA	0x00000004
#define NT_ACCESS_FILE_READ_EA		0x00000008
#define NT_ACCESS_FILE_WRITE_EA		0x00000010
#define NT_ACCESS_FILE_EXECUTE		0x00000020
#define NT_ACCESS_FILE_READ_ATTRIBUTES	0x00000080
#define NT_ACCESS_FILE_WRITE_ATTRIBUTES	0x00000100
#define NT_ACCESS_DELETE		0x00010000
#define NT_ACCESS_READ_CONTROL		0x00020000
#define NT_ACCESS_WRITE_DAC		0x00040000
#define NT_ACCESS_WRITE_OWNER		0x00080000
#define NT_ACCESS_SYNCHRONIZE		0x00100000
#define NT_ACCESS_GENERIC_ALL		0x10000000
#define NT_ACCESS_GENERIC_EXECUTE	0x20000000
#define NT_ACCESS_GENERIC_WRITE		0x40000000
#define NT_ACCESS_GENERIC_READ		0x80000000

/*
 * Share modes (CreateFile dwShareMode).  A handle grants these to other
 * openers: FILE_SHARE_READ says "others may open me for reading too".  A
 * share mode of 0 is exclusive access, the CreateFile default.
 */
#define NT_SHARE_READ			0x00000001
#define NT_SHARE_WRITE			0x00000002
#define NT_SHARE_DELETE			0x00000004

/*
 * ---------------------------------------------------------------------
 * prctl() personality control
 * ---------------------------------------------------------------------
 * See include/uapi/linux/prctl.h for the PR_* numbers.
 */
/* Interpret pathnames handed to NT-aware interfaces using NT rules. */
#define NT_PERSONALITY_ENABLED		(1U << 0)
/* Case-insensitive pathname resolution (Windows default behaviour). */
#define NT_PERSONALITY_CASE_INSENSITIVE	(1U << 1)
/* Apply Win32 rules: reserved names, MAX_PATH, trailing dot/space trim. */
#define NT_PERSONALITY_WIN32_RULES	(1U << 2)

#define NT_PERSONALITY_ALL_FLAGS					\
	(NT_PERSONALITY_ENABLED | NT_PERSONALITY_CASE_INSENSITIVE |	\
	 NT_PERSONALITY_WIN32_RULES)

#endif /* _UAPI_LINUX_NT_PERSONALITY_H */
