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
 * Largest named data stream this subsystem stores.  A named stream lives
 * in an extended attribute (the "xattr fast path" for small streams), so
 * it is bounded by what an xattr can hold rather than by the file size.
 * Streams larger than this need the backing store that is future work;
 * the overwhelmingly common named streams - Zone.Identifier and other
 * small tags - fit with room to spare.
 */
#define NT_STREAM_MAX_SIZE		65536

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
 * NT Object Manager - handles and status codes  (layer: NT kernel)
 * ---------------------------------------------------------------------
 *
 * Every object an NT process holds open - a file or directory today, an
 * event or a section later - is named by a handle: an opaque integer,
 * private to the process, that indexes the process's handle table.  The
 * full NtCreateFile ABI (OBJECT_ATTRIBUTES, IO_STATUS_BLOCK) is not here
 * yet; this is only what the handle table itself needs.
 *
 * Representation.  On the kernel/userspace boundary a handle is an opaque
 * __u32.  The NT kernel guarantees a handle's significant content fits in
 * 32 bits even on 64-bit Windows, and it hands out values that are
 * multiples of four, leaving the low two bits for the tag bits Win32 keeps
 * in them.  It is therefore carried as a plain __u32 rather than a typedef
 * - matching how the kernel represents other opaque handles in its UAPI,
 * e.g. DRM and io_uring - which also keeps the arithmetic explicit: a
 * handle is its table index times four.
 *
 * Zero is the null handle: it names no object and is never allocated, so a
 * zeroed field holds no accidental handle.  The top of the range - where a
 * value read as signed is negative - is left for the pseudo-handles Windows
 * adds on top (GetCurrentProcess() is (HANDLE)-1, GetCurrentThread() is
 * (HANDLE)-2, ...); the object-handle allocator caps its indices far below
 * that range and never collides with it.
 */
#define NT_NULL_HANDLE			0

/*
 * NTSTATUS is the NT kernel's result type: a 32-bit code whose high bits
 * are a severity (0x0 success, 0xC error).  Windows types it as a signed
 * LONG, but every code here is used only by exact-value comparison, so it
 * is carried as a __u32 to keep the 0xC000xxxx error constants clear of
 * implementation-defined sign conversions.  Only the codes the handle
 * table returns are defined; the rest of the space arrives with the
 * operations that need it.
 */
#define STATUS_SUCCESS			0x00000000
#define STATUS_INVALID_HANDLE		0xC0000008
#define STATUS_INVALID_PARAMETER	0xC000000D
#define STATUS_NO_MEMORY		0xC0000017
#define STATUS_TOO_MANY_OPENED_FILES	0xC000011F
/*
 * Reserved: the close path will return this once a handle can be marked
 * protected from closing (OBJ_PROTECT_CLOSE) and once pseudo-handles exist.
 * Defined now so the numeric ABI is fixed ahead of the code - as the NT_FS_*
 * capability bits above already are - but no path returns it yet.
 */
#define STATUS_HANDLE_NOT_CLOSABLE	0xC0000235

/*
 * Codes the file calls (create, open, read, write, query) return, at their
 * real Windows NTSTATUS values so a personality can compare against them
 * directly.  STATUS_END_OF_FILE is a warning-severity code (0x8...) rather
 * than an error, exactly as NtReadFile returns it at end of file.
 */
#define STATUS_UNSUCCESSFUL		0xC0000001
#define STATUS_NOT_IMPLEMENTED		0xC0000002
#define STATUS_INFO_LENGTH_MISMATCH	0xC0000004
#define STATUS_ACCESS_VIOLATION		0xC0000005
#define STATUS_INVALID_DEVICE_REQUEST	0xC0000010
#define STATUS_END_OF_FILE		0xC0000011
#define STATUS_ACCESS_DENIED		0xC0000022
#define STATUS_BUFFER_TOO_SMALL		0xC0000023
#define STATUS_OBJECT_NAME_INVALID	0xC0000033
#define STATUS_OBJECT_NAME_NOT_FOUND	0xC0000034
#define STATUS_OBJECT_NAME_COLLISION	0xC0000035
#define STATUS_OBJECT_PATH_NOT_FOUND	0xC000003A
#define STATUS_SHARING_VIOLATION	0xC0000043
#define STATUS_EAS_NOT_SUPPORTED	0xC000004F
#define STATUS_DISK_FULL		0xC000007F
#define STATUS_DELETE_PENDING		0xC0000056
#define STATUS_FILE_IS_A_DIRECTORY	0xC00000BA
#define STATUS_NOT_SUPPORTED		0xC00000BB
#define STATUS_NOT_A_DIRECTORY		0xC0000103
#define STATUS_NAME_TOO_LONG		0xC0000106

/*
 * ---------------------------------------------------------------------
 * NtCreateFile / NtReadFile / ... marshalling ABI  (layer: NT kernel)
 * ---------------------------------------------------------------------
 *
 * The structures a native NT caller passes NtCreateFile and its kin.  They
 * mirror the 64-bit Windows layout exactly - the byte offsets and total
 * sizes are asserted at build time in fs/ntpers/syscall.c - but use
 * kernel-idiomatic lower-case field names, as the other NT-mirroring
 * structures in this subsystem do (see struct nt_sd_relative in
 * fs/ntpers/meta.c).  Each field carries its Windows name in a comment.
 *
 * Pointers are carried as __u64 so the layout is identical whatever the
 * caller's word size; a native AMD64 PE is LP64, so they are genuine 64-bit
 * pointers there.
 */

/* UNICODE_STRING - 16 bytes. */
struct nt_unicode_string {
	__u16	length;			/* Length: bytes in use, sans any NUL */
	__u16	maximum_length;		/* MaximumLength: bytes of buffer */
	__u32	__pad;
	__u64	buffer;			/* Buffer: pointer to UTF-16LE units */
};

/* OBJECT_ATTRIBUTES - 48 bytes. */
struct nt_object_attributes {
	__u32	length;			/* Length: sizeof(this) */
	__u32	__pad0;
	__u64	root_directory;		/* RootDirectory: a HANDLE, or 0 */
	__u64	object_name;		/* ObjectName: nt_unicode_string * */
	__u32	attributes;		/* Attributes: OBJ_* below */
	__u32	__pad1;
	__u64	security_descriptor;	/* SecurityDescriptor */
	__u64	security_qos;		/* SecurityQualityOfService */
};

/* IO_STATUS_BLOCK - 16 bytes. */
struct nt_io_status_block {
	union {
		__u32	status;		/* Status: the operation's NTSTATUS */
		__u64	pointer;	/* Pointer: keeps the union 8 bytes */
	};
	__u64	information;		/* Information: op result / byte count */
};

/*
 * OBJECT_ATTRIBUTES.Attributes bits.  Only the one that changes name
 * resolution here is honoured; the rest are accepted and ignored.
 */
#define NT_OBJ_INHERIT			0x00000002
#define NT_OBJ_CASE_INSENSITIVE		0x00000040

/*
 * CreateDisposition (NtCreateFile) values.  These are the NT kernel numbers,
 * which differ from the Win32 CreateFile dwCreationDisposition numbering that
 * NT_DISPOSITION_* above carries; fs/ntpers/syscall.c maps between the two.
 * FILE_SUPERSEDE has no NT_DISPOSITION_* equivalent - superseding resets a
 * file's attributes and reports FILE_SUPERSEDED, which nt_create() does not
 * do - so the wrapper returns STATUS_NOT_IMPLEMENTED for it.
 */
#define NT_FILE_SUPERSEDE		0
#define NT_FILE_OPEN			1
#define NT_FILE_CREATE			2
#define NT_FILE_OPEN_IF			3
#define NT_FILE_OVERWRITE		4
#define NT_FILE_OVERWRITE_IF		5

/*
 * CreateOptions (NtCreateFile) / OpenOptions (NtOpenFile) bits this layer
 * honours.  Everything else a caller passes is accepted and ignored, because
 * it selects a caching or completion behaviour this scaffold does not change
 * (FILE_WRITE_THROUGH, FILE_SYNCHRONOUS_IO_NONALERT, ...) rather than what
 * object is opened.
 */
#define NT_FILE_DIRECTORY_FILE		0x00000001
#define NT_FILE_NON_DIRECTORY_FILE	0x00000040
#define NT_FILE_DELETE_ON_CLOSE		0x00001000
#define NT_FILE_OPEN_REPARSE_POINT	0x00200000

/*
 * IoStatusBlock.Information after a create/open: what actually happened.
 * These are the real Windows FILE_* values, which again differ from the
 * NT_RESULT_* numbering used internally; the wrapper maps NT_RESULT_* to
 * these on the way out.
 */
#define NT_FILE_SUPERSEDED		0
#define NT_FILE_OPENED			1
#define NT_FILE_CREATED			2
#define NT_FILE_OVERWRITTEN		3
#define NT_FILE_EXISTS			4
#define NT_FILE_DOES_NOT_EXIST		5

/*
 * FILE_INFORMATION_CLASS subset for NtQueryInformationFile.  The values are
 * the NT enumerators (FileBasicInformation == 4, FileStandardInformation ==
 * 5); any other class returns STATUS_NOT_IMPLEMENTED.
 */
#define NT_FILEINFO_BASIC		4
#define NT_FILEINFO_STANDARD		5

/* FILE_BASIC_INFORMATION - 40 bytes. Times are NT 100ns ticks since 1601. */
struct nt_file_basic_information {
	__s64	creation_time;		/* CreationTime */
	__s64	last_access_time;	/* LastAccessTime */
	__s64	last_write_time;	/* LastWriteTime */
	__s64	change_time;		/* ChangeTime */
	__u32	file_attributes;	/* FileAttributes: NT_FILE_ATTRIBUTE_* */
	__u32	__pad;
};

/* FILE_STANDARD_INFORMATION - 24 bytes. */
struct nt_file_standard_information {
	__s64	allocation_size;	/* AllocationSize */
	__s64	end_of_file;		/* EndOfFile: the file size */
	__u32	number_of_links;	/* NumberOfLinks */
	__u8	delete_pending;		/* DeletePending */
	__u8	directory;		/* Directory */
	__u16	__pad;
};

/*
 * NtReadFile/NtWriteFile ByteOffset sentinels.  A LARGE_INTEGER whose value
 * is FILE_USE_FILE_POINTER_POSITION means "use the handle's current
 * position" rather than an explicit offset; FILE_WRITE_TO_END_OF_FILE means
 * "append", and is only meaningful to a write.
 */
#define NT_FILE_USE_FILE_POINTER_POSITION	0xfffffffffffffffeULL
#define NT_FILE_WRITE_TO_END_OF_FILE		0xffffffffffffffffULL

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
