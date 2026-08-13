// SPDX-License-Identifier: GPL-2.0
/*
 * Case-insensitive pathname resolution for the NT personality.
 *
 * Windows is case-preserving and case-insensitive: "C:\Windows",
 * "C:\WINDOWS" and "C:\windows" name the same directory, and the name
 * stored on disk keeps whatever casing it was created with.  (The NT
 * kernel itself can be case-sensitive - OBJ_CASE_INSENSITIVE is a flag on
 * every object open, and NTFS stores an $UpCase table precisely so it can
 * be honoured - but every Win32 caller passes it, so case-insensitive is
 * the behaviour that matters.)
 *
 * There are two tiers here, and which one runs is decided per directory:
 *
 * Tier 1: the backing filesystem folds case itself.
 *
 *	ext4, f2fs and bcachefs support per-directory casefolding, and
 *	tmpfs picks it up too.  The dcache then does the work: the
 *	superblock's ->d_hash() hashes the folded name and ->d_compare()
 *	compares folded, so an ordinary VFS lookup of "WINDOWS" finds the
 *	dentry for "Windows" with no extra work, at full RCU-walk speed.
 *	The NT resolver contributes nothing and costs nothing.
 *
 *	This is the configuration the NT personality is meant to run on.
 *	Format the volume with casefolding enabled and set +F on its root
 *	so new directories inherit it.
 *
 * Tier 2: the backing filesystem is case-sensitive.
 *
 *	There is no index to consult, so somebody has to read the
 *	directory.  Doing that on every lookup would be exactly the
 *	pathological behaviour this subsystem must avoid, so instead we
 *	keep a fold-hint cache: a hash table keyed by (superblock, parent
 *	inode number, folded name) whose value is the real spelling of the
 *	name on disk.
 *
 *	A hit turns into one ordinary, exact, RCU-capable VFS lookup of the
 *	real name.  A miss costs one directory read, after which every
 *	subsequent lookup of any casing of that name is a hash lookup.
 *
 *	The cache is only ever a hint.  Its answer is verified by really
 *	looking the name up, so a stale entry cannot produce a wrong
 *	result - only a wasted lookup followed by a rescan.  That is what
 *	lets it avoid any coherency protocol with the dcache.
 *
 * The honest summary: tier 1 is the fast path and the intended
 * configuration; tier 2 exists so that the personality is usable on a
 * filesystem that was not prepared for it, and it is slower on the first
 * touch of each name.
 */

#include <linux/bits.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/jhash.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unicode.h>
#include <linux/vmalloc.h>
#include <linux/nt_personality.h>

#include "internal.h"

#include <trace/events/ntpers.h>

/*
 * Fold-hint cache sizing.  This is a cache of name spellings, not of
 * dentries, so entries are small and a modest table goes a long way.
 */
#define NT_CI_BUCKET_BITS	12
#define NT_CI_BUCKETS		BIT(NT_CI_BUCKET_BITS)
/* Cap so a hostile workload cannot grow the cache without bound. */
#define NT_CI_MAX_ENTRIES	65536

struct nt_ci_bucket {
	struct hlist_head	head;
	/* protects @head */
	spinlock_t		lock;
};

/**
 * struct nt_ci_entry - one remembered "folded name -> real name" mapping
 * @node:	link in its bucket
 * @rcu:	for RCU-deferred free
 * @sb:		superblock the parent lives on; compared, never dereferenced
 * @parent_ino:	parent directory's inode number
 * @fold_hash:	hash of the folded name
 * @fold_len:	length of the folded name
 * @real_len:	length of the real name
 * @names:	folded name followed by the real name, neither terminated
 *
 * @sb is stored as an opaque value for identity comparison only.  The
 * entry holds no reference to it, which is safe precisely because the
 * pointer is never followed: a superblock that has been freed and its
 * address reused can at worst produce a hint that fails to verify.
 */
struct nt_ci_entry {
	struct hlist_node	node;
	struct rcu_head		rcu;
	const void		*sb;
	unsigned long		parent_ino;
	u32			fold_hash;
	u8			fold_len;
	u8			real_len;
	char			names[];
};

static struct nt_ci_bucket *nt_ci_table;
static atomic_t nt_ci_entries = ATOMIC_INIT(0);
static atomic_long_t nt_ci_hits = ATOMIC_LONG_INIT(0);
static atomic_long_t nt_ci_misses = ATOMIC_LONG_INIT(0);
static atomic_long_t nt_ci_scans = ATOMIC_LONG_INIT(0);
static atomic_long_t nt_ci_stale = ATOMIC_LONG_INIT(0);

/* The Unicode map used for folding when the superblock has none. */
static struct unicode_map *nt_ci_encoding;

/**
 * nt_dir_is_native_ci - does the filesystem fold case for this directory?
 * @dir: a directory dentry
 *
 * When true, the dcache resolves names case-insensitively already and the
 * NT resolver must not interfere: passing the name through unchanged is
 * both correct and the fastest thing available.
 */
bool nt_dir_is_native_ci(const struct dentry *dir)
{
#if IS_ENABLED(CONFIG_UNICODE)
	struct inode *inode;

	if (!dir)
		return false;

	inode = d_inode_rcu(dir);
	if (!inode)
		return false;

	return dir->d_sb->s_encoding && IS_CASEFOLDED(inode);
#else
	return false;
#endif
}
EXPORT_SYMBOL_GPL(nt_dir_is_native_ci);

/*
 * Fold a name for cache lookup.
 *
 * Prefers the superblock's own Unicode map so that a filesystem which
 * folds case itself and this cache agree on what "the same name" means.
 * Falls back to the subsystem's map, and finally to ASCII folding if
 * Unicode is unavailable or the name is not valid UTF-8 - which is a
 * legitimate state for a Linux filesystem to be in, and must not make
 * lookups fail.
 */
static int nt_ci_fold(const struct super_block *sb, const char *name,
		      size_t len, char *out, size_t out_size)
{
#if IS_ENABLED(CONFIG_UNICODE)
	struct unicode_map *um = sb->s_encoding ?: nt_ci_encoding;

	if (um) {
		struct qstr q = QSTR_INIT(name, len);
		int folded = utf8_casefold(um, &q, out, out_size);

		if (folded > 0)
			return folded;
	}
#endif
	/*
	 * ASCII fallback.  Correct for the overwhelmingly common case and
	 * never wrong in a dangerous direction: a name that folds
	 * differently under Unicode rules simply will not be found by a
	 * differently-cased spelling, which is the same as no cache.
	 */
	if (len > out_size)
		return -ENAMETOOLONG;

	for (size_t i = 0; i < len; i++) {
		char c = name[i];

		out[i] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
	}
	return len;
}

static u32 nt_ci_hash(const void *sb, unsigned long ino, const char *fold,
		      size_t len)
{
	u32 h = jhash(fold, len, 0);

	h = jhash_2words((u32)(uintptr_t)sb, (u32)ino, h);
	return h;
}

static struct nt_ci_bucket *nt_ci_bucket(u32 hash)
{
	return &nt_ci_table[hash_32(hash, NT_CI_BUCKET_BITS)];
}

static void nt_ci_entry_free(struct rcu_head *head)
{
	kfree(container_of(head, struct nt_ci_entry, rcu));
}

/*
 * Look for a remembered spelling.  Copies the real name out under RCU so
 * the caller never holds a pointer into a freeable entry.
 */
static bool nt_ci_cache_get(const struct super_block *sb, unsigned long ino,
			    const char *fold, size_t fold_len,
			    char *real, size_t real_size, size_t *real_len)
{
	struct nt_ci_bucket *b;
	struct nt_ci_entry *e;
	u32 hash;
	bool found = false;

	if (!nt_ci_table || fold_len > NT_MAX_COMPONENT)
		return false;

	hash = nt_ci_hash(sb, ino, fold, fold_len);
	b = nt_ci_bucket(hash);

	rcu_read_lock();
	hlist_for_each_entry_rcu(e, &b->head, node) {
		if (e->fold_hash != hash || e->sb != sb ||
		    e->parent_ino != ino || e->fold_len != fold_len)
			continue;
		if (memcmp(e->names, fold, fold_len))
			continue;
		if (e->real_len > real_size)
			break;
		memcpy(real, e->names + e->fold_len, e->real_len);
		*real_len = e->real_len;
		found = true;
		break;
	}
	rcu_read_unlock();

	return found;
}

static void nt_ci_cache_put(const struct super_block *sb, unsigned long ino,
			    const char *fold, size_t fold_len,
			    const char *real, size_t real_len)
{
	struct nt_ci_bucket *b;
	struct nt_ci_entry *e, *old = NULL;
	u32 hash;

	if (!nt_ci_table || fold_len > NT_MAX_COMPONENT ||
	    real_len > NT_MAX_COMPONENT)
		return;
	if (atomic_read(&nt_ci_entries) >= NT_CI_MAX_ENTRIES)
		return;

	e = kmalloc(struct_size(e, names, fold_len + real_len), GFP_KERNEL);
	if (!e)
		return;

	e->sb = sb;
	e->parent_ino = ino;
	hash = nt_ci_hash(sb, ino, fold, fold_len);
	e->fold_hash = hash;
	e->fold_len = fold_len;
	e->real_len = real_len;
	memcpy(e->names, fold, fold_len);
	memcpy(e->names + fold_len, real, real_len);

	b = nt_ci_bucket(hash);

	spin_lock(&b->lock);
	hlist_for_each_entry(old, &b->head, node) {
		if (old->fold_hash == hash && old->sb == sb &&
		    old->parent_ino == ino && old->fold_len == fold_len &&
		    !memcmp(old->names, fold, fold_len)) {
			hlist_replace_rcu(&old->node, &e->node);
			spin_unlock(&b->lock);
			call_rcu(&old->rcu, nt_ci_entry_free);
			return;
		}
	}
	hlist_add_head_rcu(&e->node, &b->head);
	atomic_inc(&nt_ci_entries);
	spin_unlock(&b->lock);
}

/*
 * Drop every hint for one directory.  Called when we learn a cached
 * spelling was wrong, and available to callers that change a directory's
 * contents behind our back.
 */
static void nt_ci_drop_dir(const struct super_block *sb, unsigned long ino)
{
	struct hlist_node *tmp;
	struct nt_ci_entry *e;
	unsigned int i;

	if (!nt_ci_table)
		return;

	for (i = 0; i < NT_CI_BUCKETS; i++) {
		struct nt_ci_bucket *b = &nt_ci_table[i];

		if (hlist_empty(&b->head))
			continue;

		spin_lock(&b->lock);
		hlist_for_each_entry_safe(e, tmp, &b->head, node) {
			if (e->sb != sb || e->parent_ino != ino)
				continue;
			hlist_del_rcu(&e->node);
			atomic_dec(&nt_ci_entries);
			call_rcu(&e->rcu, nt_ci_entry_free);
		}
		spin_unlock(&b->lock);
	}
}

/**
 * nt_ci_invalidate_dir - forget cached name spellings for a directory
 * @dir: the directory inode
 *
 * Not required for correctness - the cache verifies every hint before
 * using it - but it turns a stale hint into a fresh scan immediately
 * rather than after one wasted lookup.
 */
void nt_ci_invalidate_dir(struct inode *dir)
{
	if (dir)
		nt_ci_drop_dir(dir->i_sb, dir->i_ino);
}
EXPORT_SYMBOL_GPL(nt_ci_invalidate_dir);

/*
 * Directory scan context.  Used only on a cache miss.
 */
struct nt_ci_scan {
	struct dir_context	ctx;
	const char		*fold;
	size_t			fold_len;
	const struct super_block *sb;
	char			real[NT_MAX_COMPONENT];
	size_t			real_len;
	bool			found;
};

static bool nt_ci_scan_actor(struct dir_context *ctx, const char *name,
			     int len, loff_t pos, u64 ino, unsigned int type)
{
	struct nt_ci_scan *s = container_of(ctx, struct nt_ci_scan, ctx);
	char fold[NT_MAX_COMPONENT];
	int folded;

	if (s->found)
		return false;
	if (len <= 0 || len > NT_MAX_COMPONENT)
		return true;

	/* An exact match would have been found by the plain lookup. */
	if ((size_t)len == s->fold_len && !memcmp(name, s->fold, s->fold_len))
		goto match;

	folded = nt_ci_fold(s->sb, name, len, fold, sizeof(fold));
	if (folded <= 0)
		return true;
	if ((size_t)folded != s->fold_len ||
	    memcmp(fold, s->fold, s->fold_len))
		return true;

match:
	memcpy(s->real, name, len);
	s->real_len = len;
	s->found = true;
	/* Stop the iteration; we have what we came for. */
	return false;
}

/*
 * Read @dir looking for a name that folds to @fold.
 *
 * This is the expensive path.  It runs once per (directory, unmatched
 * name) pair; the answer is remembered in the hint cache, so a repeat
 * lookup of any casing of that name never reaches here again.
 */
static int nt_ci_scan_dir(const struct path *dir, const char *fold,
			  size_t fold_len, char *real, size_t real_size,
			  size_t *real_len)
{
	struct nt_ci_scan scan = {
		.ctx.actor	= nt_ci_scan_actor,
		.fold		= fold,
		.fold_len	= fold_len,
		.sb		= dir->dentry->d_sb,
	};
	struct file *file;
	int err;

	file = dentry_open(dir, O_RDONLY | O_DIRECTORY, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);

	atomic_long_inc(&nt_ci_scans);

	err = iterate_dir(file, &scan.ctx);
	fput(file);

	if (err < 0)
		return err;
	if (!scan.found)
		return -ENOENT;
	if (scan.real_len > real_size)
		return -ENAMETOOLONG;

	memcpy(real, scan.real, scan.real_len);
	*real_len = scan.real_len;
	return 0;
}

/*
 * Do an ordinary, exact lookup of one component.
 */
static int nt_lookup_exact(const struct path *dir, const char *name,
			   size_t len, struct path *out)
{
	struct qstr q = QSTR_INIT(name, len);
	struct dentry *child;

	child = lookup_one_unlocked(mnt_idmap(dir->mnt), &q, dir->dentry);
	if (IS_ERR(child))
		return PTR_ERR(child);

	if (d_is_negative(child)) {
		dput(child);
		return -ENOENT;
	}

	out->mnt = mntget(dir->mnt);
	out->dentry = child;
	return 0;
}

/**
 * nt_ci_lookup - resolve one path component case-insensitively
 * @dir:  directory to look in
 * @name: component name
 * @len:  its length
 * @out:  filled in with a referenced path on success
 *
 * Tries, in order: an exact lookup (which also covers filesystems that
 * fold case themselves), a remembered spelling from the hint cache, and
 * finally a directory scan.
 *
 * Does not cross mounts or follow the result; the caller decides what to
 * do with what it gets back.
 *
 * Returns 0, -ENOENT if no name folds to @name, or another negative
 * errno.
 */
int nt_ci_lookup(const struct path *dir, const char *name, size_t len,
		 struct path *out)
{
	char fold[NT_MAX_COMPONENT];
	char real[NT_MAX_COMPONENT];
	struct super_block *sb;
	unsigned long ino;
	size_t real_len;
	int folded;
	int err;

	if (!dir || !dir->dentry || !name || !len || !out)
		return -EINVAL;
	if (len > NT_MAX_COMPONENT)
		return -ENAMETOOLONG;
	if (!d_is_dir(dir->dentry))
		return -ENOTDIR;

	/*
	 * Try the name as given first.  On a natively case-folding
	 * filesystem this is the whole algorithm, and on any filesystem it
	 * is the common case, because most lookups use the real spelling.
	 */
	err = nt_lookup_exact(dir, name, len, out);
	if (err != -ENOENT) {
		trace_ntpath_ci_lookup(name, len, "exact", err);
		return err;
	}

	/* A filesystem that folds for itself has already given its answer. */
	if (nt_dir_is_native_ci(dir->dentry)) {
		trace_ntpath_ci_lookup(name, len, "native", -ENOENT);
		return -ENOENT;
	}

	sb = dir->dentry->d_sb;
	ino = d_inode(dir->dentry)->i_ino;

	folded = nt_ci_fold(sb, name, len, fold, sizeof(fold));
	if (folded <= 0)
		return -ENOENT;

	if (nt_ci_cache_get(sb, ino, fold, folded, real, sizeof(real),
			    &real_len)) {
		err = nt_lookup_exact(dir, real, real_len, out);
		if (!err) {
			atomic_long_inc(&nt_ci_hits);
			trace_ntpath_ci_lookup(name, len, "hint", 0);
			return 0;
		}
		/*
		 * The hint did not verify, so the directory changed under
		 * us.  Drop everything we remember about it and fall
		 * through to a rescan.
		 */
		atomic_long_inc(&nt_ci_stale);
		nt_ci_drop_dir(sb, ino);
	}

	atomic_long_inc(&nt_ci_misses);

	err = nt_ci_scan_dir(dir, fold, folded, real, sizeof(real),
			     &real_len);
	if (err) {
		trace_ntpath_ci_lookup(name, len, "scan", err);
		return err;
	}

	err = nt_lookup_exact(dir, real, real_len, out);
	if (!err)
		nt_ci_cache_put(sb, ino, fold, folded, real, real_len);

	trace_ntpath_ci_lookup(name, len, "scan", err);
	return err;
}
EXPORT_SYMBOL_GPL(nt_ci_lookup);

/**
 * nt_ci_cache_stats - render fold-hint cache counters for debugfs
 * @m: the seq_file
 */
void nt_ci_cache_stats(struct seq_file *m)
{
	seq_printf(m, "entries %d\n", atomic_read(&nt_ci_entries));
	seq_printf(m, "hits %ld\n", atomic_long_read(&nt_ci_hits));
	seq_printf(m, "misses %ld\n", atomic_long_read(&nt_ci_misses));
	seq_printf(m, "scans %ld\n", atomic_long_read(&nt_ci_scans));
	seq_printf(m, "stale %ld\n", atomic_long_read(&nt_ci_stale));
	seq_printf(m, "buckets %lu\n", NT_CI_BUCKETS);
	seq_printf(m, "max_entries %u\n", NT_CI_MAX_ENTRIES);
}
EXPORT_SYMBOL_GPL(nt_ci_cache_stats);

int __init nt_casefold_init(void)
{
	unsigned int i;

	nt_ci_table = kvmalloc_array(NT_CI_BUCKETS, sizeof(*nt_ci_table),
				     GFP_KERNEL);
	if (!nt_ci_table)
		return -ENOMEM;

	for (i = 0; i < NT_CI_BUCKETS; i++) {
		INIT_HLIST_HEAD(&nt_ci_table[i].head);
		spin_lock_init(&nt_ci_table[i].lock);
	}

#if IS_ENABLED(CONFIG_UNICODE)
	/*
	 * Load a Unicode map of our own so that folding works on volumes
	 * whose superblock has none.  A failure here is not fatal: the
	 * ASCII fallback in nt_ci_fold() keeps the cache usable.
	 */
	nt_ci_encoding = utf8_load(UTF8_LATEST);
	if (IS_ERR(nt_ci_encoding)) {
		pr_warn("no Unicode tables, case folding limited to ASCII\n");
		nt_ci_encoding = NULL;
	}
#endif

	return 0;
}

void nt_casefold_exit(void)
{
	struct hlist_node *tmp;
	struct nt_ci_entry *e;
	unsigned int i;

	if (nt_ci_table) {
		for (i = 0; i < NT_CI_BUCKETS; i++) {
			hlist_for_each_entry_safe(e, tmp,
						  &nt_ci_table[i].head, node) {
				hlist_del(&e->node);
				kfree(e);
			}
		}
		kvfree(nt_ci_table);
		nt_ci_table = NULL;
	}

#if IS_ENABLED(CONFIG_UNICODE)
	if (nt_ci_encoding) {
		utf8_unload(nt_ci_encoding);
		nt_ci_encoding = NULL;
	}
#endif
}
