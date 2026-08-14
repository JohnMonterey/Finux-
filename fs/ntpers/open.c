// SPDX-License-Identifier: GPL-2.0
/*
 * NT create / open.
 *
 * Win32 CreateFile and the NtCreateFile it lowers to fold several
 * behaviours into one call.  This file implements them as one flow:
 *
 *   - the creation disposition, which decides whether a name is created,
 *     opened, or truncated, with the case-collision rule that makes those
 *     decisions safe on a filesystem Linux would otherwise let hold both
 *     "Foo" and "foo";
 *   - the share check, so an open declares what it will tolerate others
 *     doing and conflicts fail the way Windows software expects;
 *   - delete-on-close, so a handle can mark its file to vanish when the
 *     last handle to it goes.
 *
 * The order of operations is the design.  A create resolves the path
 * case-insensitively *first*: if any spelling already exists the resolver
 * finds it and the disposition is applied to that one object.  Only a
 * resolve that comes back -ENOENT - meaning no spelling exists - leads to
 * an actual create, using the caller's exact spelling.  So two files
 * differing only in case can never both be created through this path,
 * which is the property NTFS has and a case-sensitive Linux filesystem
 * does not.
 *
 * How complete the collision guarantee is depends on the volume: on a
 * casefolding volume it is atomic (start_creating() looks up under the
 * parent lock through the folding dcache); on a case-sensitive volume the
 * resolve-first check closes the ordinary case and leaves the documented
 * tier-2 window.
 *
 * The share check runs after the object exists but before any truncation,
 * so a CREATE_ALWAYS that loses a sharing conflict does not first destroy
 * the contents it was refused permission to replace.
 *
 * nt_create() returns a handle - the NT file object - not a bare path.
 * The handle carries the path and the open's registration in the share
 * table; nt_close() releases both and performs a pending delete.
 */

#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/file.h>
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
 * Stamp the attributes a freshly created object should carry.
 *
 * A new file gets ARCHIVE - what Windows sets on creation and what a
 * backup tool keys off - plus whatever settable attributes the caller
 * asked for.  A new directory does not get ARCHIVE.
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
 * On success @out holds a referenced path and volume (the volume ref is a
 * fresh get; the caller still owns @parent).  Returns 0, or a negative
 * errno; -EEXIST if start_creating() finds the name already there, which
 * the caller turns into the right disposition outcome.
 */
static int nt_do_create(struct nt_path *parent, const char *fname,
			unsigned int flen, const struct nt_create_req *req,
			struct nt_path *out, bool *created)
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
	 * caught atomically under the parent lock.  Report it as existing so
	 * the caller can apply the disposition (a collision, or an open).
	 */
	if (d_is_positive(child)) {
		out->path.mnt = mntget(parent->path.mnt);
		out->path.dentry = dget(child);
		out->volume = nt_volume_get(parent->volume);
		out->stream = NULL;
		out->stream_len = 0;
		out->stream_type = 0;
		end_creating(child);
		*created = false;
		return 0;
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

	out->path.mnt = mntget(parent->path.mnt);
	out->path.dentry = child;			/* ref from _keep() */
	out->volume = nt_volume_get(parent->volume);
	out->stream = NULL;
	out->stream_len = 0;
	out->stream_type = 0;

	nt_stamp_created(&out->path, req, want_dir);
	*created = true;
	return 0;
}

/*
 * Resolve the target, or create it, per the disposition.
 *
 * Fills @final with a referenced path+volume, sets @result to the
 * NT_RESULT_* to report, and sets @needs_trunc when the caller must
 * truncate the file after the share check has passed.  Does not itself
 * truncate: that has to wait until sharing is known to allow the open.
 */
static int nt_resolve_or_create(struct nt_task_ctx *ctx,
				struct nt_path_parse *parse,
				const struct nt_create_req *req,
				const char *fname, unsigned int flen,
				struct nt_path *final, u32 *result,
				bool *needs_trunc)
{
	u32 keep = NT_RESOLVE_CASE_INSENSITIVE | NT_RESOLVE_ALLOW_STREAM;
	struct nt_path parent;
	u32 rflags, pflags;
	bool created;
	int err;

	*needs_trunc = false;

	rflags = req->resolve_flags & keep;
	if (req->options & NT_CREATE_DIRECTORY)
		rflags |= NT_RESOLVE_DIRECTORY;
	if (!(req->options & NT_CREATE_OPEN_REPARSE))
		rflags |= NT_RESOLVE_FOLLOW;

	/* Does any spelling of the name already exist? */
	err = nt_path_resolve(ctx, parse, rflags, final);
	if (!err) {
		struct inode *inode = d_inode(final->path.dentry);

		switch (req->disposition) {
		case NT_DISPOSITION_CREATE_NEW:
			nt_path_put(final);
			return -EEXIST;
		case NT_DISPOSITION_OPEN_EXISTING:
		case NT_DISPOSITION_OPEN_ALWAYS:
			*result = NT_RESULT_OPENED;
			return 0;
		case NT_DISPOSITION_CREATE_ALWAYS:
		case NT_DISPOSITION_TRUNCATE_EXISTING:
			if (S_ISDIR(inode->i_mode)) {
				nt_path_put(final);
				return -EISDIR;
			}
			*result = NT_RESULT_OVERWRITTEN;
			*needs_trunc = true;
			return 0;
		default:
			nt_path_put(final);
			return -EINVAL;
		}
	}
	if (err != -ENOENT)
		return err;

	/* Absent.  Only the creating dispositions go further. */
	switch (req->disposition) {
	case NT_DISPOSITION_CREATE_NEW:
	case NT_DISPOSITION_CREATE_ALWAYS:
	case NT_DISPOSITION_OPEN_ALWAYS:
		break;
	default:
		return -ENOENT;
	}

	pflags = (req->resolve_flags & keep) |
		 NT_RESOLVE_PARENT | NT_RESOLVE_DIRECTORY;
	err = nt_path_resolve(ctx, parse, pflags, &parent);
	if (err)
		return err;

	err = nt_do_create(&parent, fname, flen, req, final, &created);
	nt_path_put(&parent);
	if (err)
		return err;

	if (!created) {
		/*
		 * The name appeared between the resolve and the locked
		 * create - a casefolding volume catches a case-variant here.
		 * Re-apply the disposition to what is really there.
		 */
		struct inode *inode = d_inode(final->path.dentry);

		switch (req->disposition) {
		case NT_DISPOSITION_CREATE_NEW:
			nt_path_put(final);
			return -EEXIST;
		case NT_DISPOSITION_CREATE_ALWAYS:
			if (S_ISDIR(inode->i_mode)) {
				nt_path_put(final);
				return -EISDIR;
			}
			*result = NT_RESULT_OVERWRITTEN;
			*needs_trunc = true;
			return 0;
		default:	/* OPEN_ALWAYS */
			*result = NT_RESULT_OPENED;
			return 0;
		}
	}

	*result = NT_RESULT_CREATED;
	return 0;
}

/*
 * Check that the caller may take the access it is asking for.
 *
 * This is the point where, once an NT access token exists to match SIDs
 * against, DACL evaluation will plug in.  Until then the stored security
 * descriptor does not govern access - the design document says so plainly
 * - and the enforcement is POSIX: the desired NT access is reduced to
 * MAY_READ / MAY_WRITE and checked against the inode the ordinary Linux
 * way.  That is honest about what protects the file today (uid, gid,
 * mode, POSIX ACLs) while giving the open a real access gate rather than
 * granting every handle unconditionally.
 *
 * Deliberately narrow: only read and write are mapped.  A Windows data
 * open uses GENERIC_READ or GENERIC_WRITE; mapping FILE_EXECUTE onto the
 * POSIX execute bit would wrongly refuse an ordinary read of a file that
 * merely lacks +x.  Delete access is not gated here - it is enforced by
 * the share check (FILE_SHARE_DELETE) and, at the actual unlink, by the
 * VFS permission check on the parent directory.
 */
static int nt_check_access(const struct path *path, u32 access)
{
	int mask = 0;

	if (access & NT_ACCESS_GENERIC_ALL)
		access |= NT_ACCESS_GENERIC_READ | NT_ACCESS_GENERIC_WRITE;

	if (access & (NT_ACCESS_GENERIC_READ | NT_ACCESS_FILE_READ_DATA))
		mask |= MAY_READ;
	if (access & (NT_ACCESS_GENERIC_WRITE | NT_ACCESS_FILE_WRITE_DATA |
		      NT_ACCESS_FILE_APPEND_DATA))
		mask |= MAY_WRITE;

	if (!mask)
		return 0;

	return inode_permission(mnt_idmap(path->mnt), d_inode(path->dentry),
				mask);
}

/*
 * Open the struct file the NT read and write calls act on.
 *
 * nt_create() returns a file object, and NtReadFile / NtWriteFile need a real
 * open struct file to move bytes through, so it is opened once here at create
 * time rather than lazily on first I/O - an open that will fail (a directory
 * asked for write access, say) should fail the create, not a later read.  The
 * flags follow the granted access: a directory opens read-only with
 * O_DIRECTORY, as there is no data stream to write; a regular file opens
 * O_RDWR when any write right was granted and O_RDONLY otherwise.
 *
 * An object that is neither a regular file nor a directory - a device node, a
 * fifo, or a reparse point opened without following it - carries no readable
 * byte stream, so no file is opened (handle->file stays NULL) and the read
 * and write calls refuse it.  This is deliberate: blindly opening a fifo
 * would block waiting for a peer.
 *
 * Returns 0, having set handle->file (possibly to NULL), or a negative errno.
 */
static int nt_open_file_object(struct nt_open *handle)
{
	struct inode *inode = d_inode(handle->path.dentry);
	struct file *file;
	int flags;

	if (S_ISDIR(inode->i_mode)) {
		flags = O_RDONLY | O_DIRECTORY | O_LARGEFILE;
	} else if (S_ISREG(inode->i_mode)) {
		bool write = handle->access & (NT_ACCESS_GENERIC_ALL |
					       NT_ACCESS_GENERIC_WRITE |
					       NT_ACCESS_FILE_WRITE_DATA |
					       NT_ACCESS_FILE_APPEND_DATA);

		flags = (write ? O_RDWR : O_RDONLY) | O_LARGEFILE;
	} else {
		handle->file = NULL;
		return 0;
	}

	file = dentry_open(&handle->path, flags, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);

	handle->file = file;
	return 0;
}

/* Unlink (or rmdir) the object a delete-on-close handle held. */
static void nt_unlink_handle(struct nt_open *open)
{
	struct dentry *dentry = open->path.dentry;
	struct mnt_idmap *idmap = mnt_idmap(open->path.mnt);
	struct dentry *parent;
	struct inode *dir;

	/* A volume root has itself as parent; there is nothing to unlink. */
	if (IS_ROOT(dentry))
		return;

	parent = dget_parent(dentry);
	dir = d_inode(parent);

	inode_lock_nested(dir, I_MUTEX_PARENT);
	/* Someone may have unlinked it already; both errors are fine to drop. */
	if (d_is_dir(dentry))
		vfs_rmdir(idmap, dir, dentry, NULL);
	else
		vfs_unlink(idmap, dir, dentry, NULL);
	inode_unlock(dir);

	dput(parent);
}

/**
 * nt_close - close a handle from nt_create(), performing a pending delete
 * @handle: the handle, or NULL
 */
void nt_close(struct nt_open *handle)
{
	bool unlink;

	if (!handle)
		return;

	unlink = nt_share_close(handle);

	/*
	 * Drop this handle's own open file object before any pending delete.
	 * The file is an internal reference taken by nt_create(); releasing it
	 * first means the delete-on-close unlink below is not operating on a
	 * file still held open from within this same handle.  fput() is safe
	 * here even from a kthread - it defers the final close - and unlinking
	 * an open file is well defined, so the ordering is a tidiness choice,
	 * not a correctness one.
	 */
	if (handle->file)
		fput(handle->file);

	if (handle->stream) {
		/*
		 * A delete-on-close stream handle removes just its stream,
		 * not the file it hangs off.  Stream handles are not in the
		 * share table, so there is no last-close count here - the
		 * stream goes when this handle closes.
		 */
		if (handle->delete_on_close)
			nt_stream_remove(&handle->path, handle->stream,
					 handle->stream_len);
	} else if (unlink) {
		nt_unlink_handle(handle);
	}

	kfree(handle->stream);
	path_put(&handle->path);
	nt_volume_put(handle->volume);
	kfree(handle);
}
EXPORT_SYMBOL_GPL(nt_close);

/*
 * Apply a creation disposition to a named stream.
 *
 * The base file already exists by the time this runs; the disposition
 * here decides the fate of the stream, exactly as it would for a file.
 * Sets *@result and returns 0, or a negative errno.
 */
static int nt_stream_disposition(const struct path *base, const char *name,
				 size_t name_len, u32 disposition, u32 *result)
{
	bool exists;
	ssize_t sz;
	int err;

	sz = nt_stream_size(base, name, name_len);
	if (sz < 0 && sz != -ENODATA)
		return sz;
	exists = sz >= 0;

	switch (disposition) {
	case NT_DISPOSITION_CREATE_NEW:
		if (exists)
			return -EEXIST;
		err = nt_stream_set(base, name, name_len, NULL, 0);
		*result = NT_RESULT_CREATED;
		return err;
	case NT_DISPOSITION_OPEN_EXISTING:
		if (!exists)
			return -ENOENT;
		*result = NT_RESULT_OPENED;
		return 0;
	case NT_DISPOSITION_OPEN_ALWAYS:
		if (exists) {
			*result = NT_RESULT_OPENED;
			return 0;
		}
		err = nt_stream_set(base, name, name_len, NULL, 0);
		*result = NT_RESULT_CREATED;
		return err;
	case NT_DISPOSITION_CREATE_ALWAYS:
		err = nt_stream_set(base, name, name_len, NULL, 0);
		*result = exists ? NT_RESULT_OVERWRITTEN : NT_RESULT_CREATED;
		return err;
	case NT_DISPOSITION_TRUNCATE_EXISTING:
		if (!exists)
			return -ENOENT;
		err = nt_stream_set(base, name, name_len, NULL, 0);
		*result = NT_RESULT_OVERWRITTEN;
		return err;
	default:
		return -EINVAL;
	}
}

/*
 * Open or create a named data stream on a file.
 *
 * The base file is resolved (and created if the disposition creates and it
 * is absent) without its unnamed $DATA stream ever being touched; the
 * disposition then applies to the named stream.  The handle records the
 * stream so reads and writes, and a delete-on-close, act on it rather than
 * on the file.
 */
static int nt_create_stream(struct nt_task_ctx *ctx,
			    struct nt_path_parse *parse,
			    const struct nt_create_req *req,
			    const char *fname, unsigned int flen,
			    struct nt_open_result *out)
{
	const char *sname = parse->stream;
	u16 slen = parse->stream_len;
	struct nt_create_req base_req;
	struct nt_open *handle;
	struct nt_path base;
	bool needs_trunc;
	u32 bresult, result = 0;
	bool creating;
	int err;

	/* Only real data streams are storable; $INDEX/$BITMAP are internal. */
	if (parse->stream_type != NT_STREAM_TYPE_DATA)
		return -EOPNOTSUPP;

	creating = req->disposition == NT_DISPOSITION_CREATE_NEW ||
		   req->disposition == NT_DISPOSITION_CREATE_ALWAYS ||
		   req->disposition == NT_DISPOSITION_OPEN_ALWAYS;

	/*
	 * Resolve the base file.  A creating disposition ensures the base
	 * exists (OPEN_ALWAYS - never truncating its data); a non-creating
	 * one requires it.  Either way the base's own contents are left
	 * alone; the stream is what the caller's disposition governs.
	 */
	base_req = *req;
	base_req.disposition = creating ? NT_DISPOSITION_OPEN_ALWAYS :
					  NT_DISPOSITION_OPEN_EXISTING;
	base_req.options &= ~NT_CREATE_DIRECTORY;
	base_req.resolve_flags |= NT_RESOLVE_ALLOW_STREAM;

	err = nt_resolve_or_create(ctx, parse, &base_req, fname, flen, &base,
				   &bresult, &needs_trunc);
	if (err)
		return err;

	err = nt_check_access(&base.path, req->access);
	if (err) {
		nt_path_put(&base);
		return err;
	}

	err = nt_stream_disposition(&base.path, sname, slen, req->disposition,
				    &result);
	if (err) {
		nt_path_put(&base);
		return err;
	}

	handle = kzalloc_obj(struct nt_open);
	if (!handle) {
		nt_path_put(&base);
		return -ENOMEM;
	}
	handle->stream = kmemdup(sname, slen, GFP_KERNEL);
	if (!handle->stream) {
		kfree(handle);
		nt_path_put(&base);
		return -ENOMEM;
	}
	handle->stream_len = slen;
	handle->path = base.path;
	handle->volume = base.volume;
	handle->access = req->access;
	handle->share_mode = req->share;
	handle->delete_on_close = req->options & NT_CREATE_DELETE_ON_CLOSE;
	INIT_LIST_HEAD(&handle->node);
	/* Stream handles are not registered in the share table; see nt_close. */

	out->handle = handle;
	out->result = result;
	return 0;
}

/**
 * nt_create - create or open a file by NT path
 * @ctx:  the calling task's NT context
 * @name: NT or Win32 pathname
 * @req:  what to do; see struct nt_create_req
 * @out:  filled in on success; release out->handle with nt_close()
 *
 * Returns 0 with @out populated, or a negative errno: -EEXIST for a
 * collision, -ENOENT for a missing file or path, -EBUSY for a sharing
 * violation.
 */
int nt_create(struct nt_task_ctx *ctx, const char *name,
	      const struct nt_create_req *req, struct nt_open_result *out)
{
	struct nt_path_parse parse;
	struct nt_open *handle;
	struct nt_path final;
	const char *fname;
	unsigned int flen;
	bool needs_trunc;
	u32 result = 0;
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
	 * A Win32 reserved device name (CON, NUL, ...) refers to a device,
	 * not a file on this volume; refuse it rather than create an on-disk
	 * file CreateFile would never have made.
	 */
	if (parse.flags & NT_PARSE_RESERVED_NAME) {
		err = -EPERM;
		goto out_free;
	}

	nt_final_component(&parse, &fname, &flen);

	/* "file:stream" opens a named data stream on the base file. */
	if (parse.flags & NT_PARSE_HAS_STREAM) {
		if (flen == 0)
			err = -EINVAL;	/* a volume root has no streams */
		else
			err = nt_create_stream(ctx, &parse, req, fname, flen,
					       out);
		goto out_free;
	}

	if (flen == 0) {
		/* The path names a volume root: only an open is meaningful. */
		switch (req->disposition) {
		case NT_DISPOSITION_OPEN_EXISTING:
		case NT_DISPOSITION_OPEN_ALWAYS:
			err = nt_path_resolve(ctx, &parse,
					      (req->resolve_flags &
					       NT_RESOLVE_CASE_INSENSITIVE) |
					      NT_RESOLVE_DIRECTORY, &final);
			if (err)
				goto out_free;
			result = NT_RESULT_OPENED;
			needs_trunc = false;
			break;
		default:
			err = -EEXIST;
			goto out_free;
		}
	} else {
		err = nt_resolve_or_create(ctx, &parse, req, fname, flen,
					   &final, &result, &needs_trunc);
		if (err)
			goto out_free;
	}

	/*
	 * The access gate, before the share check, so an open that the
	 * caller has no permission for fails with -EACCES rather than
	 * -EBUSY.  A file this call just created is owned by the caller and
	 * passes; an existing file enforces its mode.
	 */
	err = nt_check_access(&final.path, req->access);
	if (err) {
		nt_path_put(&final);
		goto out_free;
	}

	/* Build the handle around the resolved-or-created object. */
	handle = kzalloc_obj(struct nt_open);
	if (!handle) {
		nt_path_put(&final);
		err = -ENOMEM;
		goto out_free;
	}
	handle->path = final.path;
	handle->volume = final.volume;
	handle->access = req->access;
	handle->share_mode = req->share;
	handle->delete_on_close = (req->options & NT_CREATE_DELETE_ON_CLOSE) &&
				  !IS_ROOT(final.path.dentry);
	INIT_LIST_HEAD(&handle->node);

	/*
	 * Register the open before truncating, so a CREATE_ALWAYS that loses
	 * a sharing conflict does not first destroy the file's contents.
	 */
	err = nt_share_open(handle);
	if (err) {
		/*
		 * The share block took no ownership; release the handle's
		 * path and volume here.  A file this call just created is
		 * left in place - a create that fails on sharing is a rare
		 * race, and leaving an empty file is milder than unlinking
		 * one another opener may now hold.
		 */
		path_put(&handle->path);
		nt_volume_put(handle->volume);
		kfree(handle);
		goto out_free;
	}

	if (needs_trunc) {
		err = vfs_truncate(&handle->path, 0);
		if (err) {
			nt_close(handle);
			goto out_free;
		}
		nt_set_file_attributes(&handle->path,
				       (req->attributes &
					NT_FILE_ATTRIBUTE_SETTABLE) |
				       NT_FILE_ATTRIBUTE_ARCHIVE);
	}

	/*
	 * Open the file object last, once the create, the share check and any
	 * truncation have all succeeded, so it exists only for an open that is
	 * fully granted - the order IoCreateFile uses.
	 */
	err = nt_open_file_object(handle);
	if (err) {
		/*
		 * The open failed after the handle was registered in the share
		 * table.  Deregister it, but do not let delete-on-close fire:
		 * like the sharing-conflict path above, a create that fails at
		 * this last step leaves the file as it found it rather than
		 * unlinking one a racing opener may now hold.  handle->file is
		 * NULL on this path, so there is nothing to fput.
		 */
		nt_share_close(handle);
		path_put(&handle->path);
		nt_volume_put(handle->volume);
		kfree(handle);
		goto out_free;
	}

	out->handle = handle;
	out->result = result;
	err = 0;

out_free:
	kfree(buf);
	return err;
}
EXPORT_SYMBOL_GPL(nt_create);
