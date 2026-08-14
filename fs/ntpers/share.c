// SPDX-License-Identifier: GPL-2.0
/*
 * Windows share modes and delete-on-close.
 *
 * POSIX has no equivalent of the Windows sharing model.  On Linux any
 * number of processes can hold a file open for writing at once; on
 * Windows an open declares both what it wants to do (its access) and what
 * it will tolerate others doing (its share mode), and an open that
 * conflicts with what is already granted fails with a sharing violation.
 * That difference is why a Windows program can rename a DLL that is in
 * use and a Linux one cannot, and why a build tool on Windows fails with
 * "the file is in use by another process" - behaviour that software
 * genuinely depends on, so the personality has to reproduce it rather
 * than inherit Linux's permissiveness.
 *
 * The model here is the NT one (IoCheckShareAccess / IoRemoveShareAccess),
 * kept per file rather than per name so that a hard link or a differently
 * cased path to the same inode shares one view.  Each inode that has an
 * NT handle open gets a control block holding the running totals needed
 * to answer the next open, and the block - with its pin on the inode -
 * goes away when the last handle closes.
 *
 * The check, stated plainly for one new open against everything already
 * open on the inode:
 *
 *   - Every access the new open wants, every existing open must already
 *     be sharing.  Wanting to write means every existing handle was
 *     opened with FILE_SHARE_WRITE.
 *   - Every access an existing open holds, the new open must be willing
 *     to share.  If any existing handle can write, the new open must
 *     grant FILE_SHARE_WRITE.
 *
 * With per-inode totals that reduces to six comparisons; see
 * nt_share_conflict().
 */

#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/nt_personality.h>

#include "internal.h"

/**
 * struct nt_share - per-inode share-control block
 * @node:	link in its hash bucket
 * @inode:	the inode this describes; pinned with a reference while open
 * @opens:	the handles currently open on @inode
 * @open_count:	number of handles (length of @opens)
 * @readers:	how many handles were granted read access
 * @writers:	how many were granted write access
 * @deleters:	how many were granted delete access
 * @share_read:	how many handles grant others read access
 * @share_write: how many grant others write access
 * @share_delete: how many grant others delete access
 * @delete_pending: a delete-on-close handle is open; new opens are refused
 *
 * All fields are protected by the bucket lock the block lives in.
 */
struct nt_share {
	struct hlist_node	node;
	struct inode		*inode;
	struct list_head	opens;
	unsigned int		open_count;
	unsigned int		readers;
	unsigned int		writers;
	unsigned int		deleters;
	unsigned int		share_read;
	unsigned int		share_write;
	unsigned int		share_delete;
	bool			delete_pending;
};

#define NT_SHARE_BUCKET_BITS	10
#define NT_SHARE_BUCKETS	(1U << NT_SHARE_BUCKET_BITS)

struct nt_share_bucket {
	struct hlist_head	head;
	spinlock_t		lock;
};

static struct nt_share_bucket *nt_share_table;

static struct nt_share_bucket *nt_share_bucket(const struct inode *inode)
{
	return &nt_share_table[hash_ptr((void *)inode, NT_SHARE_BUCKET_BITS)];
}

/*
 * Reduce an NT access mask to the three things sharing turns on.
 *
 * GENERIC_READ/WRITE/EXECUTE and GENERIC_ALL are expanded to the specific
 * rights they stand for, so a caller that passes either the generic or the
 * specific form gets the same answer.
 */
static void nt_access_wants(u32 access, bool *read, bool *write, bool *del)
{
	if (access & NT_ACCESS_GENERIC_ALL)
		access |= NT_ACCESS_GENERIC_READ | NT_ACCESS_GENERIC_WRITE |
			  NT_ACCESS_DELETE;

	*read = access & (NT_ACCESS_GENERIC_READ | NT_ACCESS_FILE_READ_DATA);
	*write = access & (NT_ACCESS_GENERIC_WRITE | NT_ACCESS_FILE_WRITE_DATA |
			   NT_ACCESS_FILE_APPEND_DATA);
	*del = access & NT_ACCESS_DELETE;
}

/*
 * Would registering @open in @share violate sharing?
 *
 * The two-part rule of the file comment, expressed against the running
 * totals.  "Every existing open shares read" is "share_read == open_count";
 * "no existing open holds read" is "readers == 0".
 */
static bool nt_share_conflict(const struct nt_share *share,
			      const struct nt_open *open)
{
	bool want_read, want_write, want_delete;

	nt_access_wants(open->access, &want_read, &want_write, &want_delete);

	/* What this open wants, everyone already open must be sharing. */
	if (want_read && share->share_read != share->open_count)
		return true;
	if (want_write && share->share_write != share->open_count)
		return true;
	if (want_delete && share->share_delete != share->open_count)
		return true;

	/* What is already held, this open must be willing to share. */
	if (!(open->share_mode & NT_SHARE_READ) && share->readers)
		return true;
	if (!(open->share_mode & NT_SHARE_WRITE) && share->writers)
		return true;
	if (!(open->share_mode & NT_SHARE_DELETE) && share->deleters)
		return true;

	return false;
}

/* Fold @open's access and share mode into the running totals. */
static void nt_share_add(struct nt_share *share, struct nt_open *open)
{
	bool want_read, want_write, want_delete;

	nt_access_wants(open->access, &want_read, &want_write, &want_delete);

	share->open_count++;
	share->readers += want_read;
	share->writers += want_write;
	share->deleters += want_delete;
	share->share_read += !!(open->share_mode & NT_SHARE_READ);
	share->share_write += !!(open->share_mode & NT_SHARE_WRITE);
	share->share_delete += !!(open->share_mode & NT_SHARE_DELETE);
	list_add(&open->node, &share->opens);
}

/* Undo nt_share_add(). */
static void nt_share_sub(struct nt_share *share, struct nt_open *open)
{
	bool want_read, want_write, want_delete;

	nt_access_wants(open->access, &want_read, &want_write, &want_delete);

	share->open_count--;
	share->readers -= want_read;
	share->writers -= want_write;
	share->deleters -= want_delete;
	share->share_read -= !!(open->share_mode & NT_SHARE_READ);
	share->share_write -= !!(open->share_mode & NT_SHARE_WRITE);
	share->share_delete -= !!(open->share_mode & NT_SHARE_DELETE);
	list_del(&open->node);
}

static struct nt_share *nt_share_find(struct nt_share_bucket *b,
				      const struct inode *inode)
{
	struct nt_share *s;

	hlist_for_each_entry(s, &b->head, node)
		if (s->inode == inode)
			return s;
	return NULL;
}

/**
 * nt_share_open - register an open, enforcing the share rules
 * @open: the handle; its path, access and share_mode must be set
 *
 * Returns 0 on success with @open registered, -EBUSY on a sharing
 * violation, -ENOENT if a delete is pending on the file, or -ENOMEM.
 */
int nt_share_open(struct nt_open *open)
{
	struct inode *inode = d_inode(open->path.dentry);
	struct nt_share_bucket *b = nt_share_bucket(inode);
	struct nt_share *share, *fresh = NULL;
	int err;

	/*
	 * Allocate before taking the lock so the common path does not
	 * allocate under it; free the spare if another CPU created the
	 * block first, or if there is already one.
	 */
retry:
	spin_lock(&b->lock);
	share = nt_share_find(b, inode);
	if (!share) {
		if (!fresh) {
			spin_unlock(&b->lock);
			fresh = kzalloc_obj(struct nt_share);
			if (!fresh)
				return -ENOMEM;
			INIT_LIST_HEAD(&fresh->opens);
			goto retry;
		}
		/*
		 * The dentry in @open pins this inode, so it cannot be
		 * going away; ihold() rather than igrab() is correct.  The
		 * reference keeps the inode - and therefore the identity
		 * this block is keyed on - stable until the last handle
		 * closes.
		 */
		ihold(inode);
		fresh->inode = inode;
		hlist_add_head(&fresh->node, &b->head);
		share = fresh;
		fresh = NULL;
	}

	if (share->delete_pending) {
		err = -ENOENT;
		goto out;
	}

	if (nt_share_conflict(share, open)) {
		err = -EBUSY;
		goto out;
	}

	nt_share_add(share, open);
	if (open->delete_on_close)
		share->delete_pending = true;
	open->share = share;
	err = 0;
out:
	spin_unlock(&b->lock);
	kfree(fresh);		/* NULL if it became the registered block */
	return err;
}
EXPORT_SYMBOL_GPL(nt_share_open);

/**
 * nt_share_close - deregister an open
 * @open: a handle previously registered by nt_share_open()
 *
 * Drops @open from its inode's share block.  Returns true when this was
 * the last handle and a delete was pending on the file, meaning the caller
 * should now unlink it - which the caller must do itself, because the
 * unlink needs the parent directory and cannot happen under the bucket
 * lock, and because whether it is an unlink or an rmdir is its business.
 */
bool nt_share_close(struct nt_open *open)
{
	struct nt_share *share = open->share;
	struct nt_share_bucket *b;
	bool freed = false, unlink = false;

	if (!share)
		return false;

	b = nt_share_bucket(share->inode);
	spin_lock(&b->lock);
	nt_share_sub(share, open);
	if (share->open_count == 0) {
		unlink = share->delete_pending;
		hlist_del(&share->node);
		freed = true;
	}
	spin_unlock(&b->lock);

	if (freed) {
		iput(share->inode);
		kfree(share);
	}
	open->share = NULL;
	return unlink;
}
EXPORT_SYMBOL_GPL(nt_share_close);

int __init nt_share_subsystem_init(void)
{
	unsigned int i;

	nt_share_table = kvmalloc_array(NT_SHARE_BUCKETS,
					sizeof(*nt_share_table), GFP_KERNEL);
	if (!nt_share_table)
		return -ENOMEM;

	for (i = 0; i < NT_SHARE_BUCKETS; i++) {
		INIT_HLIST_HEAD(&nt_share_table[i].head);
		spin_lock_init(&nt_share_table[i].lock);
	}
	return 0;
}

void nt_share_subsystem_exit(void)
{
	kvfree(nt_share_table);
	nt_share_table = NULL;
}
