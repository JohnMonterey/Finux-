// SPDX-License-Identifier: GPL-2.0
/*
 * Alternate data streams.
 *
 * An NTFS file is not a single byte stream but a set of them.  The unnamed
 * "$DATA" stream is the file's ordinary contents; a named stream -
 * "file.txt:name" or "file.txt:name:$DATA" - is an independent byte stream
 * attached to the same file, with its own size and content.  The unnamed
 * stream stays the file itself and is handled by ordinary file I/O; this
 * file is only about the named ones.
 *
 * Each named stream is kept in an extended attribute, "user.nt.ads." plus
 * the stream name.  That is the "xattr fast path for small streams" the
 * design document describes: it is simple, it is atomic, and it covers the
 * named streams that actually occur in the wild - Zone.Identifier written
 * by a browser, and other small tags - which are a few hundred bytes at
 * most.  A stream is bounded by NT_STREAM_MAX_SIZE rather than by the file
 * size; a stream larger than an xattr can hold needs the hidden backing
 * store that is future work, and until then a write past the bound is
 * refused rather than silently truncated.
 *
 * Reads and writes carry an offset, so the interface looks like a byte
 * stream even though the storage is whole-value.  A partial write is a
 * read-modify-write of the whole stream; harmless at these sizes and the
 * only way to offer offsets over an atomic-value backing.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include <linux/nt_personality.h>

#include "internal.h"

#define NT_ADS_PREFIX		"user.nt.ads."
#define NT_ADS_PREFIX_LEN	(sizeof(NT_ADS_PREFIX) - 1)

/*
 * Longest stream name we can store.  The whole xattr name - prefix plus
 * stream name - must fit XATTR_NAME_MAX, which is what bounds this below
 * the 255 NTFS itself allows.
 */
#define NT_ADS_NAME_MAX		(XATTR_NAME_MAX - NT_ADS_PREFIX_LEN)

static struct mnt_idmap *nt_idmap(const struct path *path)
{
	return mnt_idmap(path->mnt);
}

/*
 * Is @name a usable stream name, and does it fit the xattr namespace?
 *
 * NTFS forbids the three path-structural characters in a stream name -
 * '\\', '/' and ':' - and a name cannot be empty or hold a NUL.  The upper
 * length bound is ours, imposed by the xattr backing rather than by NTFS.
 */
static bool nt_stream_name_ok(const char *name, size_t len)
{
	size_t i;

	if (len == 0 || len > NT_ADS_NAME_MAX)
		return false;

	for (i = 0; i < len; i++) {
		char c = name[i];

		if (c == '\0' || c == '/' || c == '\\' || c == ':')
			return false;
	}
	return true;
}

/* Build the backing xattr name for stream @name into @out (NUL terminated). */
static int nt_stream_xattr_name(const char *name, size_t len, char *out,
				size_t out_size)
{
	if (!nt_stream_name_ok(name, len))
		return -EINVAL;
	if (NT_ADS_PREFIX_LEN + len + 1 > out_size)
		return -ENAMETOOLONG;

	memcpy(out, NT_ADS_PREFIX, NT_ADS_PREFIX_LEN);
	memcpy(out + NT_ADS_PREFIX_LEN, name, len);
	out[NT_ADS_PREFIX_LEN + len] = '\0';
	return 0;
}

/**
 * nt_stream_size - the length of a named stream
 * @path: the file
 * @name: stream name
 * @name_len: its length
 *
 * Returns the size in bytes, -ENODATA if the stream does not exist, or
 * another negative errno.
 */
ssize_t nt_stream_size(const struct path *path, const char *name,
		       size_t name_len)
{
	char xname[XATTR_NAME_MAX + 1];
	int err;

	if (!path || !path->dentry)
		return -EINVAL;
	err = nt_stream_xattr_name(name, name_len, xname, sizeof(xname));
	if (err)
		return err;

	return vfs_getxattr(nt_idmap(path), path->dentry, xname, NULL, 0);
}
EXPORT_SYMBOL_GPL(nt_stream_size);

/**
 * nt_stream_set - replace a named stream's contents
 * @path: the file
 * @name: stream name
 * @name_len: its length
 * @buf:  the new contents (may be NULL when @size is 0)
 * @size: how many bytes
 *
 * Creates the stream if absent, replaces it if present.  Returns 0 or a
 * negative errno; -EFBIG if @size exceeds NT_STREAM_MAX_SIZE.
 */
int nt_stream_set(const struct path *path, const char *name, size_t name_len,
		  const void *buf, size_t size)
{
	char xname[XATTR_NAME_MAX + 1];
	int err;

	if (!path || !path->dentry)
		return -EINVAL;
	if (size > NT_STREAM_MAX_SIZE)
		return -EFBIG;
	err = nt_stream_xattr_name(name, name_len, xname, sizeof(xname));
	if (err)
		return err;

	err = mnt_want_write(path->mnt);
	if (err)
		return err;
	err = vfs_setxattr(nt_idmap(path), path->dentry, xname, buf, size, 0);
	mnt_drop_write(path->mnt);
	return err;
}
EXPORT_SYMBOL_GPL(nt_stream_set);

/*
 * Read a whole stream into a freshly allocated buffer.
 *
 * Returns the buffer and its length via @out/@out_len, or a negative
 * errno.  The caller frees @out with kvfree.
 */
static ssize_t nt_stream_read_all(const struct path *path, const char *xname,
				  void **out)
{
	ssize_t size;
	void *buf;

	size = vfs_getxattr(nt_idmap(path), path->dentry, xname, NULL, 0);
	if (size < 0)
		return size;
	if (size > NT_STREAM_MAX_SIZE)
		return -EFBIG;

	buf = kvmalloc(size ? size : 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (size) {
		size = vfs_getxattr(nt_idmap(path), path->dentry, xname, buf,
				    size);
		if (size < 0) {
			kvfree(buf);
			return size;
		}
	}
	*out = buf;
	return size;
}

/**
 * nt_stream_read - read from a named stream at an offset
 * @path: the file
 * @name: stream name
 * @name_len: its length
 * @offset: byte offset to start at
 * @buf:  where to put the bytes
 * @len:  how many to read
 *
 * Returns the number of bytes read (0 at or past end of stream), or a
 * negative errno.
 */
ssize_t nt_stream_read(const struct path *path, const char *name,
		       size_t name_len, loff_t offset, void *buf, size_t len)
{
	char xname[XATTR_NAME_MAX + 1];
	ssize_t size, ret;
	void *whole;
	int err;

	if (!path || !path->dentry || offset < 0)
		return -EINVAL;
	err = nt_stream_xattr_name(name, name_len, xname, sizeof(xname));
	if (err)
		return err;

	size = nt_stream_read_all(path, xname, &whole);
	if (size < 0)
		return size;

	if (offset >= size) {
		ret = 0;
	} else {
		ret = min_t(ssize_t, len, size - offset);
		memcpy(buf, whole + offset, ret);
	}
	kvfree(whole);
	return ret;
}
EXPORT_SYMBOL_GPL(nt_stream_read);

/**
 * nt_stream_write - write to a named stream at an offset
 * @path: the file
 * @name: stream name
 * @name_len: its length
 * @offset: byte offset to start at
 * @buf:  the bytes to write
 * @len:  how many
 *
 * Extends the stream if the write goes past its end, zero-filling any gap,
 * exactly as writing past the end of a file does.  Creates the stream if
 * it did not exist.  Returns the number of bytes written, or a negative
 * errno; -EFBIG if the result would exceed NT_STREAM_MAX_SIZE.
 */
ssize_t nt_stream_write(const struct path *path, const char *name,
			size_t name_len, loff_t offset, const void *buf,
			size_t len)
{
	char xname[XATTR_NAME_MAX + 1];
	ssize_t size, ret;
	size_t new_size;
	void *whole;
	int err;

	if (!path || !path->dentry || offset < 0)
		return -EINVAL;
	if (offset + len > NT_STREAM_MAX_SIZE)
		return -EFBIG;
	err = nt_stream_xattr_name(name, name_len, xname, sizeof(xname));
	if (err)
		return err;

	/* Read-modify-write: an xattr has no notion of a partial update. */
	size = nt_stream_read_all(path, xname, &whole);
	if (size == -ENODATA) {
		size = 0;
		whole = NULL;
	} else if (size < 0) {
		return size;
	}

	new_size = max_t(size_t, size, offset + len);
	if (new_size != (size_t)size) {
		void *grown = kvmalloc(new_size, GFP_KERNEL);

		if (!grown) {
			kvfree(whole);
			return -ENOMEM;
		}
		if (size)
			memcpy(grown, whole, size);
		/* Zero-fill a gap between the old end and @offset. */
		if (offset > size)
			memset(grown + size, 0, offset - size);
		kvfree(whole);
		whole = grown;
	}

	memcpy(whole + offset, buf, len);

	err = mnt_want_write(path->mnt);
	if (err) {
		kvfree(whole);
		return err;
	}
	err = vfs_setxattr(nt_idmap(path), path->dentry, xname, whole,
			   new_size, 0);
	mnt_drop_write(path->mnt);

	ret = err ? err : (ssize_t)len;
	kvfree(whole);
	return ret;
}
EXPORT_SYMBOL_GPL(nt_stream_write);

/**
 * nt_stream_remove - delete a named stream
 * @path: the file
 * @name: stream name
 * @name_len: its length
 *
 * Returns 0, -ENODATA if the stream did not exist, or another negative
 * errno.
 */
int nt_stream_remove(const struct path *path, const char *name,
		     size_t name_len)
{
	char xname[XATTR_NAME_MAX + 1];
	int err;

	if (!path || !path->dentry)
		return -EINVAL;
	err = nt_stream_xattr_name(name, name_len, xname, sizeof(xname));
	if (err)
		return err;

	err = mnt_want_write(path->mnt);
	if (err)
		return err;
	err = vfs_removexattr(nt_idmap(path), path->dentry, xname);
	mnt_drop_write(path->mnt);
	return err;
}
EXPORT_SYMBOL_GPL(nt_stream_remove);

/**
 * nt_stream_list - enumerate a file's named streams (FindFirstStreamW)
 * @path: the file
 * @fn:   called once per named stream
 * @ctx:  passed through to @fn
 *
 * The unnamed $DATA stream - the file's own contents - is not reported;
 * its size is the file size, which the caller already knows.  Returns 0
 * when the enumeration completed, whatever @fn returned if it stopped it
 * early, or a negative errno.
 */
int nt_stream_list(const struct path *path, nt_stream_iter_fn fn, void *ctx)
{
	struct dentry *dentry;
	char *list;
	ssize_t size, pos;
	int ret = 0;

	if (!path || !path->dentry || !fn)
		return -EINVAL;
	dentry = path->dentry;

	size = vfs_listxattr(dentry, NULL, 0);
	if (size <= 0)
		return size == -ENODATA ? 0 : size;

	list = kvmalloc(size, GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	size = vfs_listxattr(dentry, list, size);
	if (size < 0) {
		kvfree(list);
		return size;
	}

	/* listxattr returns NUL-separated full names; pick out our prefix. */
	for (pos = 0; pos < size; pos += strlen(list + pos) + 1) {
		const char *xname = list + pos;
		const char *sname;
		size_t slen;
		ssize_t ssize;

		if (strncmp(xname, NT_ADS_PREFIX, NT_ADS_PREFIX_LEN))
			continue;

		sname = xname + NT_ADS_PREFIX_LEN;
		slen = strlen(sname);
		if (slen == 0)
			continue;

		ssize = vfs_getxattr(nt_idmap(path), dentry, xname, NULL, 0);
		if (ssize < 0)
			continue;

		ret = fn(ctx, sname, slen, ssize);
		if (ret)
			break;
	}

	kvfree(list);
	return ret;
}
EXPORT_SYMBOL_GPL(nt_stream_list);
