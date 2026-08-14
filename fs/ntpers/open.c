// SPDX-License-Identifier: GPL-2.0
/*
 * NT create / open.
 *
 * Win32 CreateFile and the NtCreateFile it lowers to fold several
 * behaviours into one call.  This file implements the one that decides
 * whether a name is created, opened, or truncated - the creation
 * disposition - and the case-collision rule that makes those decisions
 * safe on a filesystem Linux would otherwise let hold both "Foo" and
 * "foo".
 *
 * The order of operations is the whole design.  A create resolves the
 * path case-insensitively *first*: if any spelling of the name already
 * exists the resolver finds it, and the disposition is applied to that
 * one object.  Only a resolve that comes back -ENOENT - meaning no
 * spelling exists - leads to an actual create, and the create uses the
 * exact spelling the caller asked for.  So two files differing only in
 * case can never both come into being through this path, which is the
 * property NTFS has and a case-sensitive Linux filesystem does not.
 *
 * How complete that guarantee is depends on the volume:
 *
 *   - On a casefolding volume (the documented intended configuration)
 *     the guarantee is atomic.  start_creating() does its lookup under
 *     the parent's lock through the casefolding dcache, so even a
 *     create that races another create is resolved correctly: the loser
 *     sees a positive dentry and is handled as an open or a collision.
 *
 *   - On a case-sensitive volume the resolve-first check closes the
 *     ordinary case but leaves a window between the check and the
 *     create.  This is the same tier-2 fallback limitation the resolver
 *     already documents, and it is why casefolding is the recommended
 *     way to host this personality.
 *
 * What this file does NOT do yet, on purpose: it does not enforce
 * desired-access or share-access, and it does not consult a security
 * descriptor.  Those arrive with the stages that enforce them, so that
 * nothing here records an intention it does not honour.
 */

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/nt_personality.h>

#include "internal.h"

/* Pull the final path component out of a parsed, normalised path. */
static void nt_final_component(const struct nt_path_parse *p,
			       const char **name, unsigned int *len)
{
	const char *rel = p->rel;
	unsigned int i, start = 0;

	for (i = 0; i < p->rel_len; i++) {
		if (rel[i] == '/')
			start = i + 1;
	}

	*name = rel + start;
	*len = p->rel_len - start;
}

/*
 * Apply a disposition to a file that already exists.
 *
 * Consumes @existing: on success its ownership moves into @out, and on
 * failure it is released here, so the caller must not touch it again
 * either way.
 */
static int nt_open_existing(const struct nt_create_req *req,
			    struct nt_path *existing, struct nt_open_result *out)
{
	struct inode *inode = d_inode(existing->path.dentry);
	int err;

	switch (req->disposition) {
	case NT_DISPOSITION_CREATE_NEW:
		/* The caller demanded a brand new file; this is a collision. */
		nt_path_put(existing);
		return -EEXIST;

	case NT_DISPOSITION_OPEN_EXISTING:
	case NT_DISPOSITION_OPEN_ALWAYS:
		out->path = *existing;
		out->result = NT_RESULT_OPENED;
		return 0;

	case NT_DISPOSITION_CREATE_ALWAYS:
	case NT_DISPOSITION_TRUNCATE_EXISTING:
		/*
		 * Overwrite.  A directory has no contents to truncate and
		 * Win32 refuses CREATE_ALWAYS on one, so mirror that rather
		 * than let do_truncate() return a less specific error.
		 */
		if (S_ISDIR(inode->i_mode)) {
			nt_path_put(existing);
			return -EISDIR;
		}

		err = vfs_truncate(&existing->path, 0);
		if (err) {
			nt_path_put(existing);
			return err;
		}

		/*
		 * Overwriting is a modification, so ARCHIVE is set, together
		 * with any settable attributes the caller supplied - Win32
		 * applies dwFlagsAndAttributes on an overwriting create.
		 */
		nt_set_file_attributes(&existing->path,
				       (req->attributes &
					NT_FILE_ATTRIBUTE_SETTABLE) |
				       NT_FILE_ATTRIBUTE_ARCHIVE);

		out->path = *existing;
		out->result = NT_RESULT_OVERWRITTEN;
		return 0;

	default:
		nt_path_put(existing);
		return -EINVAL;
	}
}

/*
 * Stamp the attributes a freshly created object should carry.
 *
 * A new file gets ARCHIVE - that is what Windows sets on creation, and
 * it is what a backup tool keys off - plus whatever settable attributes
 * the caller asked for.  A new directory does not get ARCHIVE; DIRECTORY
 * is filesystem-owned and derived, so there is nothing to store for it
 * unless the caller set HIDDEN or SYSTEM.
 */
static void nt_stamp_created(const struct path *path,
			     const struct nt_create_req *req, bool is_dir)
{
	u32 attrs = req->attributes & NT_FILE_ATTRIBUTE_SETTABLE;

	if (!is_dir)
		attrs |= NT_FILE_ATTRIBUTE_ARCHIVE;

	if (attrs)
		nt_set_file_attributes(path, attrs);
}

/*
 * Create the final component inside an already-resolved parent.
 *
 * @parent is a resolved NT path for the directory; its volume reference
 * is transferred into @out on success.  On every return the caller still
 * owns @parent's path (mount + dentry) and must release it.
 */
static int nt_do_create(struct nt_path *parent, const char *fname,
			unsigned int flen, const struct nt_create_req *req,
			struct nt_open_result *out)
{
	struct dentry *parent_dentry = parent->path.dentry;
	struct mnt_idmap *idmap = mnt_idmap(parent->path.mnt);
	bool want_dir = req->options & NT_CREATE_DIRECTORY;
	struct qstr q = QSTR_INIT(fname, flen);
	struct dentry *child, *made;
	int err;

	child = start_creating(idmap, parent_dentry, &q);
	if (IS_ERR(child))
		return PTR_ERR(child);

	/*
	 * Positive means the name already exists.  On a casefolding volume
	 * this is where a case-variant, or a create that raced us, is
	 * caught atomically - the lookup ran under the parent lock through
	 * the folding dcache.  Hand it to the existing-file path, which
	 * turns CREATE_NEW into a collision and the rest into an open or an
	 * overwrite.  Drop the parent lock first: that path locks the child
	 * inode itself, and there is nothing left to protect here once the
	 * name is known to exist.
	 */
	if (d_is_positive(child)) {
		struct nt_path found = {
			.path	= { .mnt = mntget(parent->path.mnt),
				    .dentry = dget(child) },
			.volume	= nt_volume_get(parent->volume),
		};

		end_creating(child);
		return nt_open_existing(req, &found, out);
	}

	if (want_dir) {
		made = vfs_mkdir(idmap, d_inode(parent_dentry), child, 0777,
				 NULL);
		if (IS_ERR(made)) {
			err = PTR_ERR(made);
			end_creating(child);
			return err;
		}
		child = end_creating_keep(made);
	} else {
		err = vfs_create(idmap, child, 0666, NULL);
		if (err) {
			end_creating(child);
			return err;
		}
		child = end_creating_keep(child);
	}
	if (IS_ERR(child))
		return PTR_ERR(child);

	out->path.path.mnt = mntget(parent->path.mnt);
	out->path.path.dentry = child;			/* ref from _keep() */
	out->path.volume = nt_volume_get(parent->volume);
	out->path.stream = NULL;
	out->path.stream_len = 0;
	out->path.stream_type = 0;
	out->result = NT_RESULT_CREATED;

	nt_stamp_created(&out->path.path, req, want_dir);
	return 0;
}

/**
 * nt_create - create or open a file by NT path, applying NT disposition
 * @ctx:  the calling task's NT context
 * @name: NT or Win32 pathname
 * @req:  what to do; see struct nt_create_req
 * @out:  filled in on success; release with nt_path_put()
 *
 * Returns 0 with @out populated, or a negative errno.  The mapping of NT
 * status codes onto errno is the usual one: a collision is -EEXIST, a
 * missing file or path is -ENOENT.
 */
int nt_create(struct nt_task_ctx *ctx, const char *name,
	      const struct nt_create_req *req, struct nt_open_result *out)
{
	struct nt_path_parse parse;
	struct nt_path parent;
	const char *fname;
	unsigned int flen;
	u32 rflags, pflags;
	char *buf;
	int err;

	if (!name || !req || !out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	buf = kmalloc(NT_PATH_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	err = nt_path_parse(name, strlen(name), 0, buf, NT_PATH_BUF_SIZE,
			    &parse);
	if (err)
		goto out_free;

	/*
	 * A name that resolves to a Win32 reserved device (CON, NUL, ...)
	 * refers to a device, not a file on this volume.  This subsystem
	 * has no devices to hand back, and creating an on-disk file by that
	 * name is exactly what Windows does not do, so refuse it rather
	 * than quietly create something CreateFile would never have made.
	 */
	if (parse.flags & NT_PARSE_RESERVED_NAME) {
		err = -EPERM;
		goto out_free;
	}

	/* A stream create is a Stage 4 streams feature, not this one. */
	if (parse.flags & NT_PARSE_HAS_STREAM) {
		err = -EOPNOTSUPP;
		goto out_free;
	}

	nt_final_component(&parse, &fname, &flen);
	if (flen == 0) {
		/*
		 * The path names a volume root.  There is nothing to create;
		 * only an open of the root directory is meaningful.
		 */
		switch (req->disposition) {
		case NT_DISPOSITION_OPEN_EXISTING:
		case NT_DISPOSITION_OPEN_ALWAYS:
			err = nt_path_resolve(ctx, &parse,
					      (req->resolve_flags &
					       NT_RESOLVE_CASE_INSENSITIVE) |
					      NT_RESOLVE_DIRECTORY, &out->path);
			if (!err)
				out->result = NT_RESULT_OPENED;
			break;
		default:
			err = -EEXIST;
			break;
		}
		goto out_free;
	}

	rflags = req->resolve_flags & NT_RESOLVE_CASE_INSENSITIVE;
	if (req->options & NT_CREATE_DIRECTORY)
		rflags |= NT_RESOLVE_DIRECTORY;
	if (!(req->options & NT_CREATE_OPEN_REPARSE))
		rflags |= NT_RESOLVE_FOLLOW;

	/*
	 * Does any spelling of the name already exist?  This resolve is
	 * what makes the case-collision guarantee: if it succeeds we apply
	 * the disposition to the file that is there, and never create a
	 * second one differing only in case.
	 */
	{
		struct nt_path existing;

		err = nt_path_resolve(ctx, &parse, rflags, &existing);
		if (!err) {
			err = nt_open_existing(req, &existing, out);
			goto out_free;
		}
		if (err != -ENOENT)
			goto out_free;
	}

	/* Absent.  Only the creating dispositions go further. */
	switch (req->disposition) {
	case NT_DISPOSITION_CREATE_NEW:
	case NT_DISPOSITION_CREATE_ALWAYS:
	case NT_DISPOSITION_OPEN_ALWAYS:
		break;
	default:
		err = -ENOENT;
		goto out_free;
	}

	pflags = (req->resolve_flags & NT_RESOLVE_CASE_INSENSITIVE) |
		 NT_RESOLVE_PARENT | NT_RESOLVE_DIRECTORY;
	err = nt_path_resolve(ctx, &parse, pflags, &parent);
	if (err)
		goto out_free;

	err = nt_do_create(&parent, fname, flen, req, out);
	nt_path_put(&parent);

out_free:
	kfree(buf);
	return err;
}
EXPORT_SYMBOL_GPL(nt_create);
