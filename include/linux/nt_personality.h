/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NT filesystem personality - in-kernel interfaces.
 *
 * This subsystem gives the kernel a first-class notion of an NT-style
 * namespace (volumes, drive letters, NT device names) and an NT/Win32
 * pathname resolver that sits on top of the ordinary Linux VFS.  It does
 * not replace POSIX pathname resolution and it does not require any
 * particular backing filesystem.
 *
 * Layering, from the bottom up:
 *
 *   backing filesystem   ext4, bcachefs, ntfs3, ...
 *   Linux VFS            dentries, inodes, mounts, struct path
 *   NT volume layer      struct nt_volume, struct nt_namespace
 *   NT pathname layer    struct nt_path_parse, nt_path_resolve()
 *   Win32 rules          reserved names, MAX_PATH, normalization
 *   Win32/NT API         (future) NtCreateFile, CreateFileW
 *
 * Everything in this header belongs to the middle three layers.
 */
#ifndef _LINUX_NT_PERSONALITY_H
#define _LINUX_NT_PERSONALITY_H

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/path.h>
#include <linux/refcount.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/uuid.h>
#include <uapi/linux/nt_personality.h>

struct dentry;
struct fs_struct;
struct inode;
struct seq_file;
struct super_block;
struct nt_share;

/* Longest "\Device\HarddiskVolume4294967295" style name we generate. */
#define NT_VOL_DEVNAME_MAX	48
#define NT_MAX_COMPUTER_NAME	15	/* NetBIOS limit Win32 reports */

/* Number of assignable DOS drive letters, A: through Z:. */
#define NT_NR_DRIVES		26

/*
 * Pathname buffer size used by the resolver.
 *
 * The NT kernel accepts pathnames up to NT_MAX_NT_PATH UTF-16 code units,
 * which does not fit Linux's PATH_MAX.  Until the VFS grows a longer
 * pathname limit we bound NT paths by PATH_MAX like everything else, and
 * the resolver returns -ENAMETOOLONG beyond it.  This is a documented
 * limitation rather than an architectural one: nothing in the parser
 * depends on the bound.
 */
#define NT_PATH_BUF_SIZE	PATH_MAX

/* struct nt_volume::flags */
#define NT_VOL_SYSTEM		BIT(0)	/* the system volume, normally C: */
#define NT_VOL_READONLY		BIT(1)
#define NT_VOL_REMOVABLE	BIT(2)
/*
 * The backing filesystem resolves names case-insensitively on its own
 * (ext4/f2fs casefolding, or a natively case-insensitive filesystem).
 * When set, the NT resolver hands names straight to the VFS and the
 * dcache does the work.  When clear, the resolver falls back to its own
 * fold-hint cache; see fs/ntpers/casefold.c.
 */
#define NT_VOL_NATIVE_CI	BIT(3)
/* Volume is being torn down; no new references. */
#define NT_VOL_DYING		BIT(4)

/**
 * struct nt_volume - an NT-visible volume
 * @list:	link in nt_namespace::volumes
 * @rcu:	for RCU-deferred free
 * @count:	reference count
 * @root:	backing Linux path this volume is rooted at; pins the mount
 * @id:		small dense identifier, unique within the namespace
 * @guid:	volume GUID, as reported by \\?\Volume{...}\
 * @serial:	Win32 volume serial number (GetVolumeInformation)
 * @letter:	assigned drive letter as an uppercase ASCII char, or 0
 * @flags:	NT_VOL_* above
 * @label:	volume label
 * @fs_name:	filesystem name reported to Win32 ("NTFS", "FAT32", ...)
 * @nt_device:	NT device name, e.g. "\Device\HarddiskVolume1"
 * @fs_flags:	NT_FS_* capabilities, as GetVolumeInformation reports them.
 *		Answered from what this volume can actually do, which is
 *		not the same question as what @fs_name calls it.
 *
 * A volume is the binding between an NT drive letter and a Linux mount.
 * It deliberately does not duplicate the mount: @root holds a reference
 * to a (vfsmount, dentry) pair and everything else is identity metadata
 * that the Linux VFS has no place to store.
 */
struct nt_volume {
	struct list_head	list;
	struct rcu_head		rcu;
	refcount_t		count;

	struct path		root;

	u32			id;
	guid_t			guid;
	u32			serial;
	u8			letter;
	u32			flags;

	char			label[NT_MAX_LABEL + 1];
	char			fs_name[NT_MAX_FS_NAME + 1];
	char			nt_device[NT_VOL_DEVNAME_MAX];
	u32			fs_flags;
};

/**
 * struct nt_namespace - a set of volumes and their drive-letter bindings
 * @count:	reference count
 * @rcu:	for RCU-deferred free
 * @lock:	serialises all mutation below
 * @volumes:	all volumes, in creation order
 * @drives:	drive letter table; index 0 is A:, index 25 is Z:
 * @system:	the volume marked NT_VOL_SYSTEM, or NULL
 * @next_id:	allocator for nt_volume::id
 * @nr_volumes:	number of entries on @volumes
 * @computer_name: machine name a Win32 personality reports
 *
 * There is one namespace per machine by default (init_nt_ns).  The object
 * is refcounted so that a future container-visible NT namespace can exist
 * without changing any of its users.
 *
 * @drives and @volumes are read under RCU and mutated under @lock.
 */
struct nt_namespace {
	refcount_t		count;
	struct rcu_head		rcu;

	/* protects @volumes, @drives, @system, @next_id, @nr_volumes */
	spinlock_t		lock;
	struct list_head	volumes;
	struct nt_volume __rcu	*drives[NT_NR_DRIVES];
	struct nt_volume __rcu	*system;

	u32			next_id;
	u32			nr_volumes;

	char			computer_name[NT_MAX_COMPUTER_NAME + 1];
};

extern struct nt_namespace init_nt_ns;

/**
 * struct nt_drive_cwd - remembered current directory for one drive
 * @path:	the directory; holds a reference
 * @letter:	uppercase drive letter this entry describes
 *
 * Win32 keeps a current directory per drive so that "C:foo" means
 * something other than "C:\foo".  In real Windows this lives in the
 * process environment block, not in the NT kernel; we keep it in the
 * kernel because our Win32 personality has no PEB of its own yet.
 */
struct nt_drive_cwd {
	struct path	path;
	u8		letter;
};

/**
 * struct nt_task_ctx - per-process NT personality state
 * @count:	reference count; shared through CLONE_FS
 * @ns:		namespace this process resolves against; holds a reference
 * @flags:	NT_PERSONALITY_* from the UAPI header
 * @cur_drive:	current drive letter, uppercase ASCII
 * @lock:	protects @cwd and @nr_cwd
 * @nr_cwd:	valid entries in @cwd
 * @max_cwd:	allocated entries in @cwd
 * @cwd:	lazily grown per-drive current directory cache
 *
 * Hangs off struct fs_struct, so it follows the same sharing rules as the
 * root and current directory: threads of a process share it, and CLONE_FS
 * shares it across processes.  That matches Windows, where the current
 * directory is process-wide.
 */
struct nt_task_ctx {
	refcount_t		count;
	struct nt_namespace	*ns;
	u32			flags;
	u8			cur_drive;

	/* protects @cwd, @nr_cwd, @max_cwd */
	spinlock_t		lock;
	u8			nr_cwd;
	u8			max_cwd;
	struct nt_drive_cwd	*cwd;
};

/**
 * struct nt_path_parse - result of parsing an NT/Win32 pathname
 * @type:	classification; see enum nt_path_type
 * @flags:	NT_PARSE_* result flags
 * @drive:	uppercase drive letter, or 0 when the path names no drive
 * @buf:	caller-owned scratch buffer the fields below point into
 * @buf_size:	size of @buf
 * @rel:	canonical '/'-separated relative path, NUL terminated; an
 *		empty string means "the root of the resolved volume"
 * @rel_len:	length of @rel, excluding the NUL
 * @nr_components: number of components in @rel
 * @unc_server:	UNC server name (NT_PATH_UNC only)
 * @unc_share:	UNC share name (NT_PATH_UNC only)
 * @device:	device name for NT_PATH_DEVICE / NT_PATH_NT_OBJECT
 * @stream:	named stream on the final component, or NULL
 * @stream_type: NT_STREAM_TYPE_* for @stream
 *
 * The parser never allocates.  Everything it produces lives in @buf,
 * which the caller supplies (normally from __getname()).  @rel is built
 * with '/' separators specifically so that it can be handed to the
 * ordinary VFS resolver without a second pass: '/' cannot appear inside
 * an NT filename, so the transformation is lossless.
 */
struct nt_path_parse {
	enum nt_path_type	type;
	u32			flags;
	u8			drive;

	char			*buf;
	size_t			buf_size;

	char			*rel;
	u16			rel_len;
	u16			nr_components;

	const char		*unc_server;
	u16			unc_server_len;
	const char		*unc_share;
	u16			unc_share_len;

	const char		*device;
	u16			device_len;

	const char		*stream;
	u16			stream_len;
	u32			stream_type;
};

/**
 * struct nt_path - a resolved NT path
 * @path:	the Linux path; holds references to mount and dentry
 * @volume:	volume the path was resolved through; holds a reference
 * @stream:	named stream requested, or NULL; points into the caller's
 *		parse buffer and is only valid while that buffer lives
 * @stream_len:	length of @stream
 * @stream_type: NT_STREAM_TYPE_* for @stream
 *
 * Release with nt_path_put().
 */
struct nt_path {
	struct path		path;
	struct nt_volume	*volume;
	const char		*stream;
	u16			stream_len;
	u32			stream_type;
};

/* Resolution flags for nt_path_resolve(). */
/* Resolve the parent directory rather than the final component. */
#define NT_RESOLVE_PARENT	BIT(0)
/* Follow a reparse point / symlink on the final component. */
#define NT_RESOLVE_FOLLOW	BIT(1)
/* Require the result to be a directory. */
#define NT_RESOLVE_DIRECTORY	BIT(2)
/* Resolve case-insensitively (the Windows default). */
#define NT_RESOLVE_CASE_INSENSITIVE BIT(3)
/* Caller understands named streams; otherwise a stream request fails. */
#define NT_RESOLVE_ALLOW_STREAM	BIT(4)

/*
 * NT timestamps count 100ns intervals since 1601-01-01.  These match
 * fs/ntfs3's kernel2nt()/nt2kernel(), which are the reference conversion,
 * and are here so that callers outside a filesystem can use them.
 */
#define NT_TIME_UNITS_PER_SEC	10000000LL
#define NT_TIME_EPOCH_DELTA	11644473600LL	/* 1601-01-01 to 1970-01-01 */

static inline u64 nt_time_from_timespec(const struct timespec64 *ts)
{
	return (u64)((ts->tv_sec + NT_TIME_EPOCH_DELTA) * NT_TIME_UNITS_PER_SEC +
		     ts->tv_nsec / 100);
}

static inline void nt_time_to_timespec(u64 nt, struct timespec64 *ts)
{
	s64 t = (s64)nt - NT_TIME_UNITS_PER_SEC * NT_TIME_EPOCH_DELTA;
	s32 rem;

	ts->tv_sec = div_s64_rem(t, NT_TIME_UNITS_PER_SEC, &rem);
	ts->tv_nsec = rem * 100;
}

/* struct nt_file_info::time_flags */
/*
 * CreationTime came from the filesystem's own birth time, or from a value
 * this subsystem stored earlier.  It means what NT means by it.
 */
#define NT_TIME_CREATION_EXACT		BIT(0)
/*
 * The backing filesystem has no birth time and none was ever stored, so
 * CreationTime is a best-effort estimate.  It is *not* the Linux ctime,
 * which is the inode change time and a different concept entirely; the
 * estimate is the oldest timestamp the inode does have.  A caller that
 * needs to know the difference must check this flag.
 */
#define NT_TIME_CREATION_ESTIMATED	BIT(1)

/**
 * struct nt_file_info - what NT wants to know about a file
 * @attributes:		NT_FILE_ATTRIBUTE_* mask
 * @reparse_tag:	reparse tag, or 0 when not a reparse point
 * @creation:		NT CreationTime
 * @last_access:	NT LastAccessTime (Linux atime)
 * @last_write:		NT LastWriteTime (Linux mtime)
 * @change:		NT ChangeTime (Linux ctime - the correct match)
 * @time_flags:		NT_TIME_* describing how reliable @creation is
 * @file_id:		64-bit file id, as returned by GetFileInformationByHandle
 * @file_id_128:	128-bit file id, as in FILE_ID_INFO
 * @volume_serial:	serial of the volume the file lives on
 * @size:		end-of-file position
 * @alloc_size:		space actually allocated
 * @nlink:		hard link count
 *
 * Assembled by nt_query_file_info() from a statx, the inode, and this
 * subsystem's own stored metadata.
 */
struct nt_file_info {
	u32			attributes;
	u32			reparse_tag;
	struct timespec64	creation;
	struct timespec64	last_access;
	struct timespec64	last_write;
	struct timespec64	change;
	u32			time_flags;
	u64			file_id;
	u8			file_id_128[16];
	u32			volume_serial;
	u64			size;
	u64			alloc_size;
	u32			nlink;
};

/*
 * Which attributes come from where.  See fs/ntpers/meta.c and
 * Documentation/filesystems/nt-personality.rst for the reasoning.
 */
/* Derived from the inode every time; storing them would only go stale. */
#define NT_ATTR_DERIVED							\
	(NT_FILE_ATTRIBUTE_DIRECTORY | NT_FILE_ATTRIBUTE_REPARSE_POINT | \
	 NT_FILE_ATTRIBUTE_COMPRESSED | NT_FILE_ATTRIBUTE_ENCRYPTED |	\
	 NT_FILE_ATTRIBUTE_SPARSE_FILE | NT_FILE_ATTRIBUTE_DEVICE)
/*
 * Backed by the file mode rather than by stored metadata, so that the
 * Linux and NT views of "can I write this?" cannot disagree.
 */
#define NT_ATTR_MODE_BACKED	NT_FILE_ATTRIBUTE_READONLY
/* Kept in an extended attribute; nothing in Linux represents them. */
#define NT_ATTR_STORED							\
	(NT_FILE_ATTRIBUTE_HIDDEN | NT_FILE_ATTRIBUTE_SYSTEM |		\
	 NT_FILE_ATTRIBUTE_ARCHIVE | NT_FILE_ATTRIBUTE_TEMPORARY |	\
	 NT_FILE_ATTRIBUTE_OFFLINE |					\
	 NT_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)

#ifdef CONFIG_NT_FS_PERSONALITY

/* --- pathname parser (fs/ntpers/path.c) ------------------------------ */

int nt_path_parse(const char *name, size_t len, u32 flags,
		  char *buf, size_t buf_size, struct nt_path_parse *out);
const char *nt_path_type_name(enum nt_path_type type);

/* --- Win32 reserved device names (fs/ntpers/reserved.c) -------------- */

bool nt_name_is_reserved_device(const char *name, size_t len);
bool nt_name_is_valid_win32(const char *name, size_t len);

/* --- volumes (fs/ntpers/volume.c) ------------------------------------ */

struct nt_volume *nt_volume_create(struct nt_namespace *ns,
				   const struct path *root, u8 letter,
				   u32 flags, const char *label);
void nt_volume_destroy(struct nt_namespace *ns, struct nt_volume *vol);
struct nt_volume *nt_volume_get(struct nt_volume *vol);
void nt_volume_put(struct nt_volume *vol);
struct nt_volume *nt_volume_lookup_letter(struct nt_namespace *ns, u8 letter);
struct nt_volume *nt_volume_lookup_id(struct nt_namespace *ns, u32 id);
struct nt_volume *nt_volume_lookup_device(struct nt_namespace *ns,
					  const char *name, size_t len);
struct nt_volume *nt_volume_get_system(struct nt_namespace *ns);
int nt_volume_assign_letter(struct nt_namespace *ns, struct nt_volume *vol,
			    u8 letter);
int nt_volume_set_system(struct nt_namespace *ns, struct nt_volume *vol);
u8 nt_volume_first_free_letter(struct nt_namespace *ns);
void nt_volume_seq_show(struct seq_file *m, struct nt_volume *vol);

/* --- namespaces and per-task state (fs/ntpers/namespace.c) ----------- */

struct nt_namespace *nt_ns_create(void);
struct nt_namespace *nt_ns_get(struct nt_namespace *ns);
void nt_ns_put(struct nt_namespace *ns);

struct nt_task_ctx *nt_ctx_get(struct nt_task_ctx *ctx);
void nt_ctx_put(struct nt_task_ctx *ctx);
struct nt_task_ctx *nt_ctx_alloc(struct nt_namespace *ns);
/* Returns the current task's context, or NULL if it has none. */
struct nt_task_ctx *nt_ctx_current(void);
/* Returns the current task's context, creating it if necessary. */
struct nt_task_ctx *nt_ctx_current_or_create(void);
struct nt_namespace *nt_ns_current(void);

int nt_personality_set(u32 flags);
u32 nt_personality_get(void);
int nt_ctx_set_current_drive(struct nt_task_ctx *ctx, u8 letter);
int nt_ctx_set_drive_cwd(struct nt_task_ctx *ctx, u8 letter,
			 const struct path *path);
bool nt_ctx_get_drive_cwd(struct nt_task_ctx *ctx, u8 letter,
			  struct path *out);

/* fs_struct integration; see fs/fs_struct.c. */
int nt_fs_struct_copy(struct fs_struct *new_fs, struct fs_struct *old_fs);
void nt_fs_struct_free(struct fs_struct *fs);

/* --- resolver (fs/ntpers/resolve.c) ---------------------------------- */

int nt_path_resolve(struct nt_task_ctx *ctx, const struct nt_path_parse *p,
		    u32 flags, struct nt_path *out);
int nt_kern_path(const char *name, u32 resolve_flags, struct nt_path *out);
void nt_path_put(struct nt_path *ntp);

/* --- case-insensitive lookup (fs/ntpers/casefold.c) ------------------ */

bool nt_dir_is_native_ci(const struct dentry *dir);
int nt_ci_lookup(const struct path *dir, const char *name, size_t len,
		 struct path *out);
void nt_ci_invalidate_dir(struct inode *dir);
void nt_ci_cache_stats(struct seq_file *m);

/* --- create / open (fs/ntpers/open.c) -------------------------------- */

/**
 * struct nt_create_req - what a caller is asking nt_create() to do
 * @disposition:  one of NT_DISPOSITION_*; decides create vs open vs truncate
 * @access:       NT_ACCESS_* the handle wants; the share check turns on the
 *                read/write/delete distinction within it
 * @share:        NT_SHARE_* this open grants other openers; 0 is exclusive
 * @options:      NT_CREATE_* flags
 * @attributes:   NT_FILE_ATTRIBUTE_* to stamp on a file this call creates.
 *                Ignored when an existing file is opened, exactly as
 *                CreateFile ignores dwFlagsAndAttributes on an open.
 * @resolve_flags: NT_RESOLVE_* passed through to path resolution; the
 *                case-sensitivity of the lookup is a caller decision.
 */
struct nt_create_req {
	u32	disposition;
	u32	access;
	u32	share;
	u32	options;
	u32	attributes;
	u32	resolve_flags;
};

/**
 * struct nt_open - one open instance: the file object a create/open returns
 * @path:    the object; holds references to mount and dentry
 * @volume:  volume the path was resolved through; holds a reference
 * @share:   per-inode share-control block this open is registered in
 * @node:    link in nt_share::opens
 * @stream:  named data stream this handle refers to, or NULL for the file
 *           itself; owned by the handle
 * @stream_len: length of @stream
 * @access:  NT_ACCESS_* granted to this handle
 * @share_mode: NT_SHARE_* this handle grants others
 * @delete_on_close: on the last close, delete the file - or, for a stream
 *           handle, just that stream
 *
 * This is the NT file object.  It is created by nt_create() and released
 * by nt_close(); a caller must call nt_close() exactly once, never
 * nt_path_put() on the embedded path.
 */
struct nt_open {
	struct path		path;
	struct nt_volume	*volume;
	struct nt_share		*share;
	struct list_head	node;
	char			*stream;
	u16			stream_len;
	u32			access;
	u32			share_mode;
	bool			delete_on_close;
};

/**
 * struct nt_open_result - what nt_create() did
 * @handle: the open file object; release with nt_close()
 * @result: one of NT_RESULT_*, the IoStatusBlock.Information equivalent
 */
struct nt_open_result {
	struct nt_open	*handle;
	u32		result;
};

int nt_create(struct nt_task_ctx *ctx, const char *name,
	      const struct nt_create_req *req, struct nt_open_result *out);
void nt_close(struct nt_open *handle);

/* --- share / delete semantics (fs/ntpers/share.c) -------------------- */

int nt_share_open(struct nt_open *open);
bool nt_share_close(struct nt_open *open);
int nt_share_subsystem_init(void);
void nt_share_subsystem_exit(void);

/* --- file metadata (fs/ntpers/meta.c) -------------------------------- */

int nt_query_file_info(const struct path *path, struct nt_volume *vol,
		       struct nt_file_info *info);
int nt_get_file_attributes(const struct path *path, u32 *attrs);
int nt_set_file_attributes(const struct path *path, u32 attrs);
int nt_set_creation_time(const struct path *path,
			 const struct timespec64 *ts);
ssize_t nt_get_security_descriptor(const struct path *path, void *buf,
				   size_t size);
int nt_set_security_descriptor(const struct path *path, const void *buf,
			       size_t size);
int nt_set_reparse_point(const struct path *path, const void *buf,
			 size_t size);
ssize_t nt_get_reparse_point(const struct path *path, void *buf, size_t size);
int nt_delete_reparse_point(const struct path *path);

/* --- alternate data streams (fs/ntpers/stream.c) --------------------- */

/*
 * Callback for nt_stream_list().  @name is the stream name (not NUL
 * terminated), @size its length in bytes.  A non-zero return stops the
 * enumeration and is returned by nt_stream_list().
 */
typedef int (*nt_stream_iter_fn)(void *ctx, const char *name,
				 size_t name_len, loff_t size);

int nt_stream_set(const struct path *path, const char *name, size_t name_len,
		  const void *buf, size_t size);
ssize_t nt_stream_read(const struct path *path, const char *name,
		       size_t name_len, loff_t offset, void *buf, size_t len);
ssize_t nt_stream_write(const struct path *path, const char *name,
			size_t name_len, loff_t offset, const void *buf,
			size_t len);
int nt_stream_remove(const struct path *path, const char *name,
		     size_t name_len);
ssize_t nt_stream_size(const struct path *path, const char *name,
		       size_t name_len);
int nt_stream_list(const struct path *path, nt_stream_iter_fn fn, void *ctx);

/* --- subsystem init (fs/ntpers/main.c) ------------------------------- */

bool nt_personality_ready(void);

static inline bool nt_task_is_nt(void)
{
	struct nt_task_ctx *ctx = nt_ctx_current();

	return ctx && (ctx->flags & NT_PERSONALITY_ENABLED);
}

#else /* !CONFIG_NT_FS_PERSONALITY */

static inline bool nt_personality_ready(void) { return false; }
static inline bool nt_task_is_nt(void) { return false; }
static inline u32 nt_personality_get(void) { return 0; }
static inline int nt_personality_set(u32 flags) { return -EINVAL; }
static inline void nt_ci_invalidate_dir(struct inode *dir) { }
static inline int nt_fs_struct_copy(struct fs_struct *new_fs,
				    struct fs_struct *old_fs) { return 0; }
static inline void nt_fs_struct_free(struct fs_struct *fs) { }

#endif /* CONFIG_NT_FS_PERSONALITY */

/**
 * nt_drive_index - convert a drive letter to a table index
 * @letter: ASCII letter, either case
 *
 * Returns 0..NT_NR_DRIVES-1, or -1 if @letter is not a drive letter.
 * Drive letters are case-insensitive everywhere in NT and Win32.
 */
static inline int nt_drive_index(u8 letter)
{
	if (letter >= 'a' && letter <= 'z')
		return letter - 'a';
	if (letter >= 'A' && letter <= 'Z')
		return letter - 'A';
	return -1;
}

static inline u8 nt_drive_upper(u8 letter)
{
	if (letter >= 'a' && letter <= 'z')
		return letter - 'a' + 'A';
	return letter;
}

/**
 * nt_is_sep - is @c an NT/Win32 pathname separator?
 *
 * Win32 accepts both separators everywhere and the NT kernel normalises
 * '/' to '\' on the way in, so both must be recognised.  Note that this
 * means '/' can never appear inside an NT filename, which is what lets
 * the parser emit a POSIX-shaped relative path losslessly.
 */
static inline bool nt_is_sep(char c)
{
	return c == '\\' || c == '/';
}

#endif /* _LINUX_NT_PERSONALITY_H */
