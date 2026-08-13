// SPDX-License-Identifier: GPL-2.0
/*
 * NT file metadata: DOS attributes, Windows timestamps, security
 * descriptors and file identifiers.
 *
 * The job here is to answer NT's questions about a file using a Linux
 * inode that was never designed to answer them, without lying about the
 * parts that do not correspond.
 *
 * Where an attribute has a genuine Linux equivalent, that equivalent is
 * used and nothing is stored.  Where it does not, it goes in an extended
 * attribute.  Deciding which case a given attribute falls into is most of
 * the work, and getting it wrong in the "store it" direction produces
 * metadata that silently goes stale the moment anything touches the file
 * through a POSIX interface.
 *
 * Three sources, in order of preference:
 *
 *   1. The filesystem itself.  On ntfs3 the attributes and the security
 *	descriptor are real NTFS metadata, reachable through the xattrs
 *	fs/ntfs3 already exports.  We use those, so a volume backed by
 *	real NTFS and one backed by ext4 look identical from above.
 *
 *   2. The inode.  Directory-ness, compression, encryption, sparseness
 *	and writability are all things Linux already knows.  Deriving them
 *	every time is both cheaper and more correct than caching them.
 *
 *   3. Our own extended attribute, for the handful of DOS attributes that
 *	have no Linux meaning at all.
 *
 * On the choice of xattr namespace, which is a security question and not
 * a naming one:
 *
 *   - "system." is reserved for the filesystem.  A generic filesystem
 *     rejects names in it that it does not implement, so it works on
 *     ntfs3 and nowhere else.  We use it only for the ntfs3 passthrough.
 *
 *   - "trusted." requires CAP_SYS_ADMIN for every access, which would
 *     mean an ordinary program could not set the hidden bit on its own
 *     file.  That is not the semantics Windows has.
 *
 *   - "user." is writable by the file's owner, which is exactly right for
 *     DOS attributes: on Windows the owner can set them.  Its limitation
 *     is that the kernel does not allow user.* xattrs on symlinks or
 *     device nodes, so DOS attributes on those fall back to derivation.
 *
 *   - "security." is where Samba keeps NT ACLs, and is the right home for
 *     a security descriptor: it is mediated by the LSM rather than freely
 *     writable by the owner.  See the comment on nt_set_security_
 *     descriptor() for what that does and does not currently guarantee.
 */

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include <linux/nt_personality.h>

#include "internal.h"

#include <trace/events/ntpers.h>

/*
 * Native NTFS metadata, as exported by fs/ntfs3.  Trying these first
 * gives real interoperability on an NTFS volume for free: the values we
 * read and write are the ones in the $STANDARD_INFORMATION attribute.
 */
#define NT_XATTR_NTFS_ATTRIB	"system.ntfs_attrib"
#define NT_XATTR_NTFS_SECURITY	"system.ntfs_security"

/* Our own storage, for filesystems with nowhere native to put this. */
#define NT_XATTR_DOS_ATTRIB	"user.nt.dos_attrib"
#define NT_XATTR_CREATION_TIME	"user.nt.crtime"
#define NT_XATTR_SECURITY	"security.nt.sd"

/* A security descriptor larger than this is not one we want to store. */
#define NT_SD_MAX_SIZE		65536

/*
 * A self-relative SECURITY_DESCRIPTOR header, as NT lays it out on the
 * wire and on disk.  Mirrors fs/ntfs3's struct
 * SECURITY_DESCRIPTOR_RELATIVE; declared here so that meta.c does not
 * depend on the NTFS driver being built.
 */
struct nt_sd_relative {
	u8	revision;
	u8	sbz1;
	__le16	control;
	__le32	owner;
	__le32	group;
	__le32	sacl;
	__le32	dacl;
} __packed;

#define NT_SD_REVISION		1
#define NT_SE_SELF_RELATIVE	0x8000

static struct mnt_idmap *nt_idmap(const struct path *path)
{
	return mnt_idmap(path->mnt);
}

/*
 * Read a fixed-size little-endian integer xattr.
 *
 * Returns 0 and fills @out on success, or a negative errno.  -ENODATA
 * means the attribute is simply not there, which is the common case and
 * not an error for any caller here.
 */
static int nt_read_xattr_le(const struct path *path, const char *name,
			    void *out, size_t size)
{
	ssize_t got;

	got = vfs_getxattr(nt_idmap(path), path->dentry, name, out, size);
	if (got < 0)
		return got;
	if (got != size)
		return -EINVAL;
	return 0;
}

/*
 * DOS attributes as the filesystem itself holds them, if it does.
 *
 * fs/ntfs3 stores system.ntfs_attrib as a host-endian u32 holding the
 * $STANDARD_INFORMATION file attributes, which is exactly our format.
 */
static int nt_read_native_attrs(const struct path *path, u32 *attrs)
{
	u32 value;
	int err;

	err = nt_read_xattr_le(path, NT_XATTR_NTFS_ATTRIB, &value,
			       sizeof(value));
	if (err)
		return err;

	*attrs = value;
	return 0;
}

static int nt_write_native_attrs(const struct path *path, u32 attrs)
{
	return vfs_setxattr(nt_idmap(path), path->dentry,
			    NT_XATTR_NTFS_ATTRIB, &attrs, sizeof(attrs), 0);
}

/*
 * Derive the attributes the inode already knows about.
 *
 * Everything set here is recomputed on every query, because every one of
 * these can change through an ordinary POSIX operation that has no reason
 * to tell us about it.
 */
static u32 nt_derive_attributes(const struct path *path,
				const struct kstat *stat)
{
	const struct dentry *dentry = path->dentry;
	u32 attrs = 0;

	if (S_ISDIR(stat->mode))
		attrs |= NT_FILE_ATTRIBUTE_DIRECTORY;

	/*
	 * A symlink is the closest thing a generic filesystem has to a
	 * reparse point.  Stage 4 will add tags and payloads; until then
	 * reporting the bit is honest, because traversal really does
	 * redirect.
	 */
	if (S_ISLNK(stat->mode))
		attrs |= NT_FILE_ATTRIBUTE_REPARSE_POINT;

	if (S_ISCHR(stat->mode) || S_ISBLK(stat->mode) ||
	    S_ISFIFO(stat->mode) || S_ISSOCK(stat->mode))
		attrs |= NT_FILE_ATTRIBUTE_DEVICE;

	if (stat->attributes & STATX_ATTR_COMPRESSED)
		attrs |= NT_FILE_ATTRIBUTE_COMPRESSED;
	if (stat->attributes & STATX_ATTR_ENCRYPTED)
		attrs |= NT_FILE_ATTRIBUTE_ENCRYPTED;

	/*
	 * Sparseness is not reported by statx, so infer it the way du(1)
	 * does: fewer blocks allocated than the size accounts for.  Only
	 * meaningful for regular files, and only ever a hint - a file can
	 * be sparse in NT terms without Linux having allocated fewer
	 * blocks, and a compressed file trips this too, which is why the
	 * compressed case is excluded.
	 */
	if (S_ISREG(stat->mode) && stat->size > 0 &&
	    !(attrs & NT_FILE_ATTRIBUTE_COMPRESSED) &&
	    (u64)stat->blocks * 512 < (u64)stat->size)
		attrs |= NT_FILE_ATTRIBUTE_SPARSE_FILE;

	/*
	 * READONLY is backed by the mode rather than stored, so that
	 * chmod and SetFileAttributes cannot disagree about whether a file
	 * can be written.  Windows applies it per-file, not per-user, so
	 * the test is whether anybody may write.
	 */
	if (!(stat->mode & S_IWUGO))
		attrs |= NT_FILE_ATTRIBUTE_READONLY;

	/*
	 * A leading dot is the Unix way of saying hidden, and Samba has
	 * mapped it to FILE_ATTRIBUTE_HIDDEN for decades.  It is only a
	 * default: an explicitly stored value overrides it, so clearing
	 * the hidden bit on a dotfile sticks.
	 */
	if (dentry->d_name.len > 1 && dentry->d_name.name[0] == '.')
		attrs |= NT_FILE_ATTRIBUTE_HIDDEN;

	return attrs;
}

/*
 * Combine derived and stored attributes.
 *
 * The filesystem-owned bits always win, because they describe what the
 * file actually is.  The stored bits win over the derived defaults for
 * the attributes Linux has no opinion about.
 */
static u32 nt_merge_attributes(u32 derived, u32 stored, bool have_stored,
			       const struct kstat *stat)
{
	u32 attrs = derived & (NT_ATTR_DERIVED | NT_ATTR_MODE_BACKED);

	if (have_stored) {
		attrs |= stored & NT_ATTR_STORED;
	} else {
		/* Defaults for a file this subsystem has never seen. */
		attrs |= derived & NT_FILE_ATTRIBUTE_HIDDEN;
		if (S_ISREG(stat->mode))
			attrs |= NT_FILE_ATTRIBUTE_ARCHIVE;
	}

	/*
	 * NORMAL means "no other attribute is set" and is mutually
	 * exclusive with everything, so it can only be computed last.
	 */
	if (!attrs)
		attrs = NT_FILE_ATTRIBUTE_NORMAL;

	return attrs;
}

/**
 * nt_get_file_attributes - read a file's DOS attributes
 * @path:  the file
 * @attrs: filled in with a mask of NT_FILE_ATTRIBUTE_*
 *
 * Returns 0 or a negative errno.
 */
int nt_get_file_attributes(const struct path *path, u32 *attrs)
{
	struct kstat stat;
	u32 stored = 0, native;
	bool have_stored = false;
	int err;

	if (!path || !path->dentry || !attrs)
		return -EINVAL;

	err = vfs_getattr(path, &stat, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (err)
		return err;

	/*
	 * On a filesystem that keeps real NTFS metadata, that metadata is
	 * authoritative and there is nothing to merge.
	 */
	if (!nt_read_native_attrs(path, &native)) {
		*attrs = native & NT_FILE_ATTRIBUTE_VALID;
		if (S_ISDIR(stat.mode))
			*attrs |= NT_FILE_ATTRIBUTE_DIRECTORY;
		trace_ntmeta_attrs("get-native", stat.ino, *attrs, 0);
		return 0;
	}

	if (!nt_read_xattr_le(path, NT_XATTR_DOS_ATTRIB, &stored,
			      sizeof(stored)))
		have_stored = true;

	*attrs = nt_merge_attributes(nt_derive_attributes(path, &stat),
				     le32_to_cpu((__force __le32)stored),
				     have_stored, &stat);

	trace_ntmeta_attrs("get", stat.ino, *attrs, 0);
	return 0;
}
EXPORT_SYMBOL_GPL(nt_get_file_attributes);

/*
 * Apply the READONLY bit to the file mode.
 *
 * Setting it removes write permission from everyone, which is the whole
 * point of the attribute.  Clearing it restores write permission to the
 * owner only.
 *
 * The asymmetry is deliberate.  A DOS attribute carries no information
 * about *who* should be able to write, so the only safe reconstruction is
 * the least privileged one that satisfies the request.  Granting group or
 * other write here - for instance by mirroring the read bits, which would
 * turn a perfectly ordinary 0644 file into 0666 - would silently widen
 * access every time a Windows program cleared a read-only flag.
 */
static int nt_apply_readonly(const struct path *path, bool readonly)
{
	struct iattr attr = { .ia_valid = ATTR_MODE };
	struct inode *inode = d_inode(path->dentry);
	umode_t mode = inode->i_mode;
	umode_t want;
	int err;

	if (readonly)
		want = mode & ~S_IWUGO;
	else
		want = mode | S_IWUSR;

	if (want == mode)
		return 0;

	attr.ia_mode = want;

	inode_lock(inode);
	err = notify_change(nt_idmap(path), path->dentry, &attr, NULL);
	inode_unlock(inode);

	return err;
}

/**
 * nt_set_file_attributes - set a file's DOS attributes
 * @path:  the file
 * @attrs: mask of NT_FILE_ATTRIBUTE_*
 *
 * Only the attributes a caller is allowed to set are honoured; the rest
 * belong to the filesystem and are ignored rather than rejected, which is
 * what SetFileAttributes does with them.
 *
 * Returns 0 or a negative errno.  -EOPNOTSUPP means the backing
 * filesystem has nowhere to keep the attributes that need storing, which
 * happens for symlinks and device nodes because the kernel does not allow
 * user.* extended attributes on them.
 */
int nt_set_file_attributes(const struct path *path, u32 attrs)
{
	u32 stored, existing;
	__le32 value;
	int err;

	if (!path || !path->dentry)
		return -EINVAL;
	if (attrs & ~NT_FILE_ATTRIBUTE_VALID)
		return -EINVAL;

	err = mnt_want_write(path->mnt);
	if (err)
		return err;

	/* Native metadata: hand the whole thing to the filesystem. */
	if (!nt_read_native_attrs(path, &existing)) {
		u32 want = (existing & ~NT_FILE_ATTRIBUTE_SETTABLE) |
			   (attrs & NT_FILE_ATTRIBUTE_SETTABLE);

		err = nt_write_native_attrs(path, want);
		trace_ntmeta_attrs("set-native", d_inode(path->dentry)->i_ino,
				   want, err);
		goto out;
	}

	err = nt_apply_readonly(path, attrs & NT_FILE_ATTRIBUTE_READONLY);
	if (err)
		goto out;

	stored = attrs & NT_ATTR_STORED;
	value = cpu_to_le32(stored);

	if (stored) {
		err = vfs_setxattr(nt_idmap(path), path->dentry,
				   NT_XATTR_DOS_ATTRIB, &value, sizeof(value),
				   0);
	} else {
		/*
		 * Nothing left worth storing.  Remove the attribute rather
		 * than leaving a zero behind, so that the derived defaults
		 * apply again and the file looks untouched.
		 */
		err = vfs_removexattr(nt_idmap(path), path->dentry,
				      NT_XATTR_DOS_ATTRIB);
		if (err == -ENODATA)
			err = 0;
	}

	trace_ntmeta_attrs("set", d_inode(path->dentry)->i_ino, attrs, err);

out:
	mnt_drop_write(path->mnt);
	return err;
}
EXPORT_SYMBOL_GPL(nt_set_file_attributes);

/*
 * Work out CreationTime.
 *
 * Linux ctime is the inode change time and is emphatically not the
 * creation time; NT's ChangeTime is what ctime corresponds to.  The
 * sources for a real creation time, in order:
 *
 *   1. statx STATX_BTIME, where the filesystem records a birth time.
 *   2. A value stored here earlier, for filesystems that do not.
 *   3. Nothing, in which case we estimate and say so.
 */
static void nt_resolve_creation_time(const struct path *path,
				     const struct kstat *stat,
				     struct nt_file_info *info)
{
	__le64 stored;

	if (stat->result_mask & STATX_BTIME) {
		info->creation = stat->btime;
		info->time_flags |= NT_TIME_CREATION_EXACT;
		return;
	}

	if (!nt_read_xattr_le(path, NT_XATTR_CREATION_TIME, &stored,
			      sizeof(stored))) {
		nt_time_to_timespec(le64_to_cpu(stored), &info->creation);
		info->time_flags |= NT_TIME_CREATION_EXACT;
		return;
	}

	/*
	 * No birth time anywhere.  The oldest timestamp the inode has is
	 * the closest thing to a lower bound on when the file appeared.
	 * Flag it, so a caller that cares can tell the difference.
	 */
	info->creation = timespec64_compare(&stat->mtime, &stat->ctime) < 0 ?
			 stat->mtime : stat->ctime;
	info->time_flags |= NT_TIME_CREATION_ESTIMATED;
}

/**
 * nt_set_creation_time - record a creation time for a file
 * @path: the file
 * @ts:   the time
 *
 * Filesystems that keep a birth time do not let anything change it, so
 * this stores a value alongside instead.  A stored value is only
 * consulted when the filesystem has no birth time of its own, so this is
 * a no-op in observable terms on ext4 or btrfs - which is the correct
 * outcome, because the real birth time is the better answer.
 *
 * Returns 0 or a negative errno.
 */
int nt_set_creation_time(const struct path *path, const struct timespec64 *ts)
{
	__le64 value;
	int err;

	if (!path || !path->dentry || !ts)
		return -EINVAL;

	value = cpu_to_le64(nt_time_from_timespec(ts));

	err = mnt_want_write(path->mnt);
	if (err)
		return err;

	err = vfs_setxattr(nt_idmap(path), path->dentry,
			   NT_XATTR_CREATION_TIME, &value, sizeof(value), 0);

	mnt_drop_write(path->mnt);
	return err;
}
EXPORT_SYMBOL_GPL(nt_set_creation_time);

/*
 * Build the file identifiers NT hands out.
 *
 * The property that matters is that an id must not silently start
 * referring to a different file.  An inode number alone does not have
 * that property, because inode numbers are reused after deletion; this is
 * the same problem NFS has, and the same answer works: pair the inode
 * number with the inode generation, which changes on reuse.
 *
 * The 64-bit id is the inode number, because that is the width NT gives
 * us and there is nowhere to put the generation.  The 128-bit id carries
 * both, plus the volume serial, and is the one to prefer.
 */
static void nt_build_file_ids(const struct kstat *stat,
			      const struct inode *inode, u32 volume_serial,
			      struct nt_file_info *info)
{
	__le64 ino = cpu_to_le64(stat->ino);
	__le32 gen = cpu_to_le32(inode->i_generation);
	__le32 serial = cpu_to_le32(volume_serial);

	info->file_id = stat->ino;

	memcpy(info->file_id_128, &ino, sizeof(ino));
	memcpy(info->file_id_128 + 8, &gen, sizeof(gen));
	memcpy(info->file_id_128 + 12, &serial, sizeof(serial));
}

/**
 * nt_query_file_info - answer NT's questions about a file
 * @path: the file
 * @vol:  volume it was resolved through, or NULL
 * @info: filled in on success
 *
 * Returns 0 or a negative errno.
 */
int nt_query_file_info(const struct path *path, struct nt_volume *vol,
		       struct nt_file_info *info)
{
	struct inode *inode;
	struct kstat stat;
	u32 stored = 0;
	u32 native;
	bool have_stored = false;
	int err;

	if (!path || !path->dentry || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	/*
	 * Ask for the birth time explicitly.  Filesystems that have one
	 * report it in result_mask; the rest silently do not, which is
	 * what nt_resolve_creation_time() keys off.
	 */
	err = vfs_getattr(path, &stat, STATX_BASIC_STATS | STATX_BTIME,
			  AT_STATX_SYNC_AS_STAT);
	if (err)
		return err;

	inode = d_inode(path->dentry);

	if (!nt_read_native_attrs(path, &native)) {
		info->attributes = native & NT_FILE_ATTRIBUTE_VALID;
		if (S_ISDIR(stat.mode))
			info->attributes |= NT_FILE_ATTRIBUTE_DIRECTORY;
	} else {
		if (!nt_read_xattr_le(path, NT_XATTR_DOS_ATTRIB, &stored,
				      sizeof(stored)))
			have_stored = true;

		info->attributes =
			nt_merge_attributes(nt_derive_attributes(path, &stat),
					    le32_to_cpu((__force __le32)stored),
					    have_stored, &stat);
	}

	if (info->attributes & NT_FILE_ATTRIBUTE_REPARSE_POINT)
		info->reparse_tag = NT_IO_REPARSE_TAG_SYMLINK;

	/*
	 * The three timestamps that do correspond, mapped to the NT
	 * concept each one actually matches.
	 */
	info->last_access = stat.atime;
	info->last_write = stat.mtime;
	info->change = stat.ctime;
	nt_resolve_creation_time(path, &stat, info);

	info->volume_serial = vol ? vol->serial : 0;
	nt_build_file_ids(&stat, inode, info->volume_serial, info);

	info->size = stat.size;
	info->alloc_size = (u64)stat.blocks * 512;
	info->nlink = stat.nlink;

	trace_ntmeta_query(stat.ino, info->attributes, info->time_flags);
	return 0;
}
EXPORT_SYMBOL_GPL(nt_query_file_info);

/*
 * Check that a blob really is a self-relative security descriptor before
 * storing it.  Refusing garbage here means anything that later reads one
 * back can rely on the header, and keeps a malformed descriptor from
 * being persisted where a future access check would trip over it.
 */
static bool nt_sd_is_valid(const void *buf, size_t size)
{
	const struct nt_sd_relative *sd = buf;
	u32 off;

	if (size < sizeof(*sd) || size > NT_SD_MAX_SIZE)
		return false;
	if (sd->revision != NT_SD_REVISION)
		return false;
	if (!(le16_to_cpu(sd->control) & NT_SE_SELF_RELATIVE))
		return false;

	/*
	 * Every offset is either zero, meaning absent, or must land inside
	 * the buffer with room for at least a minimal structure.
	 */
	off = le32_to_cpu(sd->owner);
	if (off && (off < sizeof(*sd) || off >= size))
		return false;
	off = le32_to_cpu(sd->group);
	if (off && (off < sizeof(*sd) || off >= size))
		return false;
	off = le32_to_cpu(sd->dacl);
	if (off && (off < sizeof(*sd) || off >= size))
		return false;
	off = le32_to_cpu(sd->sacl);
	if (off && (off < sizeof(*sd) || off >= size))
		return false;

	return true;
}

/**
 * nt_get_security_descriptor - read a file's NT security descriptor
 * @path: the file
 * @buf:  where to put it, or NULL to query the size
 * @size: size of @buf
 *
 * Returns the size of the descriptor, or a negative errno.  -ENODATA
 * means the file has no NT security descriptor, which is the normal state
 * for a file created through a POSIX interface; the caller should
 * synthesise one from the Linux credentials rather than treat it as an
 * error.
 */
ssize_t nt_get_security_descriptor(const struct path *path, void *buf,
				   size_t size)
{
	ssize_t got;

	if (!path || !path->dentry)
		return -EINVAL;

	/* Real NTFS metadata first, so an NTFS volume round-trips. */
	got = vfs_getxattr(nt_idmap(path), path->dentry,
			   NT_XATTR_NTFS_SECURITY, buf, size);
	if (got >= 0 || (got != -EOPNOTSUPP && got != -ENODATA))
		return got;

	return vfs_getxattr(nt_idmap(path), path->dentry, NT_XATTR_SECURITY,
			    buf, size);
}
EXPORT_SYMBOL_GPL(nt_get_security_descriptor);

/**
 * nt_set_security_descriptor - store an NT security descriptor
 * @path: the file
 * @buf:  a self-relative SECURITY_DESCRIPTOR
 * @size: its size
 *
 * The descriptor is stored alongside the Linux credentials, not instead
 * of them.  Linux uid, gid, mode and POSIX ACLs remain the only thing
 * that governs access; this is metadata that a future Win32 subsystem can
 * hand back to a caller that asks for an owner SID or a DACL.
 *
 * That is why "security." is the right namespace and why it is safe
 * today: the descriptor is inert, so a forged one grants nothing.  Before
 * anything starts making access decisions from it, this needs revisiting
 * - at that point a descriptor an unprivileged owner can rewrite becomes
 * an access-control bypass, and the write path will need to enforce that
 * the new descriptor is no more permissive than the caller could already
 * achieve through chmod.  See Documentation/filesystems/nt-personality.rst.
 *
 * Returns 0 or a negative errno.
 */
int nt_set_security_descriptor(const struct path *path, const void *buf,
			       size_t size)
{
	int err;

	if (!path || !path->dentry || !buf)
		return -EINVAL;
	if (!nt_sd_is_valid(buf, size))
		return -EINVAL;

	err = mnt_want_write(path->mnt);
	if (err)
		return err;

	/* Prefer the filesystem's own security metadata where it has any. */
	err = vfs_setxattr(nt_idmap(path), path->dentry,
			   NT_XATTR_NTFS_SECURITY, buf, size, 0);
	if (err == -EOPNOTSUPP || err == -ENODATA)
		err = vfs_setxattr(nt_idmap(path), path->dentry,
				   NT_XATTR_SECURITY, buf, size, 0);

	mnt_drop_write(path->mnt);
	return err;
}
EXPORT_SYMBOL_GPL(nt_set_security_descriptor);
