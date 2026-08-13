// SPDX-License-Identifier: GPL-2.0
/*
 * Turning a parsed NT path into a Linux struct path.
 *
 * This is the join between the NT namespace and the Linux VFS.  Two
 * decisions shape it:
 *
 * 1. The starting point comes from the NT volume layer, not from
 *    current->fs->root.  "C:\Windows" starts at the root of whatever
 *    volume holds drive letter C, which is a real object with a real
 *    mount behind it.  This is what makes C: a first-class concept
 *    rather than an alias for /.
 *
 * 2. The walk itself is the ordinary VFS walk.  Once we know where to
 *    start and have a canonical relative path, vfs_path_lookup() does
 *    everything: mount crossing, symlinks, permission checks, RCU
 *    pathwalk, the lot.  Reimplementing any of that would be both a
 *    large amount of duplicated code and a security liability.
 *
 * The case-insensitive walk is the exception.  When the backing
 * filesystem folds case itself, case insensitivity is invisible here and
 * the fast path above is used unchanged.  When it does not, we have to
 * walk component by component so that each one can go through
 * nt_ci_lookup().  That path is slower and gives up RCU pathwalk; see the
 * comment on nt_walk_ci() and the discussion in
 * Documentation/filesystems/nt-personality.rst.
 */

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/nt_personality.h>

#include "internal.h"

#include <trace/events/ntpers.h>

/**
 * nt_path_put - release a resolved NT path
 * @ntp: the path, may be all zeroes
 */
void nt_path_put(struct nt_path *ntp)
{
	if (!ntp)
		return;
	if (ntp->path.dentry)
		path_put(&ntp->path);
	nt_volume_put(ntp->volume);
	memset(ntp, 0, sizeof(*ntp));
}
EXPORT_SYMBOL_GPL(nt_path_put);

/*
 * Work out where a parsed path starts.
 *
 * Fills @start with a referenced path and, when the path is anchored to a
 * volume, @volp with a referenced volume.
 */
static int nt_resolve_start(struct nt_task_ctx *ctx,
			    const struct nt_path_parse *p,
			    struct path *start, struct nt_volume **volp)
{
	struct nt_namespace *ns = ctx ? ctx->ns : &init_nt_ns;
	struct nt_volume *vol = NULL;
	u8 letter;

	*volp = NULL;

	switch (p->type) {
	case NT_PATH_DRIVE_ABSOLUTE:
		vol = nt_volume_lookup_letter(ns, p->drive);
		if (!vol)
			return -ENODEV;
		*start = vol->root;
		path_get(start);
		*volp = vol;
		return 0;

	case NT_PATH_DRIVE_RELATIVE:
		/*
		 * "C:foo" is relative to the current directory on C:.  If
		 * the process has not established one, Windows uses the
		 * root of the drive, which is what a fresh process sees.
		 */
		vol = nt_volume_lookup_letter(ns, p->drive);
		if (!vol)
			return -ENODEV;

		if (ctx && nt_ctx_get_drive_cwd(ctx, p->drive, start)) {
			*volp = vol;
			return 0;
		}

		/*
		 * A process sitting on this drive uses its ordinary
		 * current directory, which is the same object Linux
		 * already tracks.
		 */
		if (ctx && nt_drive_upper(p->drive) == ctx->cur_drive) {
			get_fs_pwd(current->fs, start);
			*volp = vol;
			return 0;
		}

		*start = vol->root;
		path_get(start);
		*volp = vol;
		return 0;

	case NT_PATH_ROOTED:
		/* "\Windows" is rooted on the caller's current drive. */
		letter = ctx ? ctx->cur_drive : 'C';
		vol = nt_volume_lookup_letter(ns, letter);
		if (!vol)
			return -ENODEV;
		*start = vol->root;
		path_get(start);
		*volp = vol;
		return 0;

	case NT_PATH_RELATIVE:
		if (!current->fs)
			return -ENOENT;
		get_fs_pwd(current->fs, start);
		return 0;

	case NT_PATH_NT_OBJECT:
	case NT_PATH_DEVICE:
		/*
		 * "\Device\HarddiskVolume1\..." and "\??\Volume{...}\..."
		 * name a volume without going through a drive letter.
		 * This is the form the Object Manager uses internally and
		 * the one a driver would see.
		 */
		if (p->drive) {
			vol = nt_volume_lookup_letter(ns, p->drive);
		} else if (p->device_len) {
			vol = nt_volume_lookup_device(ns, p->device,
						      p->device_len);
		}
		if (!vol)
			return -ENODEV;
		*start = vol->root;
		path_get(start);
		*volp = vol;
		return 0;

	case NT_PATH_UNC:
		/*
		 * A UNC path names a resource on another machine.  Serving
		 * one means going through a network filesystem, which is a
		 * redirector's job, not this layer's.  The parser
		 * understands the syntax so that a future SMB redirector
		 * can be plugged in here.
		 */
		return -EOPNOTSUPP;

	case NT_PATH_INVALID:
	default:
		return -EINVAL;
	}
}

/*
 * Walk a canonical relative path one component at a time, folding case
 * at each step.
 *
 * Used only when the backing filesystem cannot fold case itself.  It is
 * slower than vfs_path_lookup() in two ways that are worth naming
 * plainly:
 *
 *   - It cannot use RCU pathwalk, because nt_ci_lookup() may have to read
 *     a directory, which can sleep.  Every component takes a reference.
 *
 *   - It does not follow symlinks in the middle of a path.  A component
 *     that turns out to be a symlink stops the walk with -ELOOP rather
 *     than silently resolving to something the caller did not ask for.
 *
 * Both are limitations of this fallback, not of the architecture: on a
 * volume with casefolding enabled, nt_path_resolve() never calls this and
 * gets the full VFS walk.
 */
static int nt_walk_ci(const struct path *start, const char *rel,
		      unsigned int rel_len, u32 flags, struct path *out)
{
	struct path cur = *start;
	const char *pos = rel, *end = rel + rel_len;
	bool want_dir = flags & NT_RESOLVE_DIRECTORY;
	int err = 0;

	path_get(&cur);

	while (pos < end) {
		const char *comp = pos;
		struct path next;
		size_t clen;
		bool last;

		while (pos < end && *pos != '/')
			pos++;
		clen = pos - comp;
		if (pos < end)
			pos++;
		last = pos >= end;

		if (!clen)
			continue;

		if (!d_is_dir(cur.dentry)) {
			err = -ENOTDIR;
			break;
		}

		err = inode_permission(mnt_idmap(cur.mnt),
				       d_inode(cur.dentry), MAY_EXEC);
		if (err)
			break;

		err = nt_ci_lookup(&cur, comp, clen, &next);
		if (err)
			break;

		/*
		 * nt_ci_lookup() deliberately does not cross mounts, so
		 * that the component it found is the one the directory
		 * actually contains.  Cross here, where we know we are
		 * continuing the walk.
		 */
		if (d_mountpoint(next.dentry)) {
			err = follow_down(&next, 0);
			if (err < 0) {
				path_put(&next);
				break;
			}
		}

		if (!last || (flags & NT_RESOLVE_FOLLOW)) {
			if (d_is_symlink(next.dentry)) {
				path_put(&next);
				err = -ELOOP;
				break;
			}
		}

		path_put(&cur);
		cur = next;
	}

	if (!err && want_dir && !d_is_dir(cur.dentry))
		err = -ENOTDIR;

	if (err) {
		path_put(&cur);
		return err;
	}

	*out = cur;
	return 0;
}

/**
 * nt_path_resolve - resolve a parsed NT path to a Linux path
 * @ctx:   the caller's NT context, or NULL to use machine defaults
 * @p:     a successful nt_path_parse() result
 * @flags: NT_RESOLVE_* flags
 * @out:   filled in on success; release with nt_path_put()
 *
 * Returns 0, or a negative errno.  -ENODEV means the path names a drive
 * letter or device with no volume behind it; -EOPNOTSUPP that it names
 * something this layer does not serve, such as a UNC share.
 */
int nt_path_resolve(struct nt_task_ctx *ctx, const struct nt_path_parse *p,
		    u32 flags, struct nt_path *out)
{
	struct nt_volume *vol = NULL;
	struct path start, result;
	unsigned int lookup_flags = 0;
	const char *rel;
	unsigned int rel_len;
	char *parent_rel = NULL;
	int err;

	if (!p || !out || p->type == NT_PATH_INVALID)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	if ((p->flags & NT_PARSE_HAS_STREAM) &&
	    !(flags & NT_RESOLVE_ALLOW_STREAM)) {
		/*
		 * Named streams are a Stage 4 feature.  Failing loudly is
		 * better than silently opening the default stream, which
		 * would give a caller the wrong bytes.
		 */
		return -EOPNOTSUPP;
	}

	err = nt_resolve_start(ctx, p, &start, &vol);
	if (err) {
		trace_ntpath_resolve(p, p->drive, NULL, err);
		return err;
	}

	rel = p->rel;
	rel_len = p->rel_len;

	if (flags & NT_RESOLVE_PARENT) {
		const char *slash = NULL;
		unsigned int i;

		for (i = rel_len; i > 0; i--) {
			if (rel[i - 1] == '/') {
				slash = rel + i - 1;
				break;
			}
		}

		if (!slash) {
			/* The parent is the starting point itself. */
			rel_len = 0;
		} else {
			parent_rel = kmemdup_nul(rel, slash - rel, GFP_KERNEL);
			if (!parent_rel) {
				err = -ENOMEM;
				goto out_put;
			}
			rel = parent_rel;
			rel_len = slash - p->rel;
		}
	}

	if (!rel_len) {
		/* The path names the root of its volume. */
		result = start;
		path_get(&result);
		if ((flags & NT_RESOLVE_DIRECTORY) && !d_is_dir(result.dentry)) {
			path_put(&result);
			err = -ENOTDIR;
			goto out_put;
		}
		goto done;
	}

	if (flags & NT_RESOLVE_FOLLOW)
		lookup_flags |= LOOKUP_FOLLOW;
	if (flags & NT_RESOLVE_DIRECTORY)
		lookup_flags |= LOOKUP_DIRECTORY;

	/*
	 * Fast path: either the caller wants case-sensitive resolution, or
	 * the volume's backing filesystem folds case by itself.  Either
	 * way the ordinary VFS walk produces the right answer, so use it
	 * and get RCU pathwalk, symlinks and mount crossing for free.
	 */
	if (!(flags & NT_RESOLVE_CASE_INSENSITIVE) ||
	    (vol && (vol->flags & NT_VOL_NATIVE_CI)) ||
	    nt_dir_is_native_ci(start.dentry)) {
		err = vfs_path_lookup(start.dentry, start.mnt, rel,
				      lookup_flags, &result);
		if (err)
			goto out_put;
		goto done;
	}

	err = nt_walk_ci(&start, rel, rel_len, flags, &result);
	if (err)
		goto out_put;

done:
	out->path = result;
	out->volume = vol;
	out->stream = p->stream;
	out->stream_len = p->stream_len;
	out->stream_type = p->stream_type;
	vol = NULL;
	err = 0;

	trace_ntpath_resolve(p, out->volume ? out->volume->letter : 0,
			     result.dentry, 0);

out_put:
	if (err)
		trace_ntpath_resolve(p, vol ? vol->letter : 0, NULL, err);
	kfree(parent_rel);
	path_put(&start);
	nt_volume_put(vol);
	return err;
}
EXPORT_SYMBOL_GPL(nt_path_resolve);

/**
 * nt_kern_path - parse and resolve an NT path from kernel memory
 * @name:          NUL-terminated NT or Win32 pathname
 * @resolve_flags: NT_RESOLVE_* flags
 * @out:           filled in on success; release with nt_path_put()
 *
 * The convenience entry point for in-kernel callers.  Uses the calling
 * process's NT context if it has one.
 *
 * Note that @out->stream is NULL after this call even when @name named a
 * stream: the parse buffer is freed before returning, so there is nothing
 * for it to point at.  Callers that care about streams should parse and
 * resolve in two steps and keep the buffer alive.
 */
int nt_kern_path(const char *name, u32 resolve_flags, struct nt_path *out)
{
	struct nt_path_parse parse;
	char *buf;
	int err;

	if (!name || !out)
		return -EINVAL;

	buf = __getname();
	if (!buf)
		return -ENOMEM;

	err = nt_path_parse(name, strlen(name), 0, buf, PATH_MAX, &parse);
	trace_ntpath_parse(name, &parse, err);
	if (err)
		goto out;

	if (parse.flags & NT_PARSE_HAS_STREAM) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = nt_path_resolve(nt_ctx_current(), &parse, resolve_flags, out);

out:
	__putname(buf);
	return err;
}
EXPORT_SYMBOL_GPL(nt_kern_path);
