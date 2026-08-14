/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared scaffolding for the NT personality KUnit suites that need a real
 * filesystem to work against.
 *
 * Both the case-folding and the metadata tests need the same thing: a
 * private, writable, case-sensitive mount, and files on it with exactly
 * the names they ask for.
 *
 * Every dentry handed out is tracked and released by nt_test_fs_exit(),
 * rather than by the test that created it.  That matters because
 * KUNIT_ASSERT_* aborts the test case on the spot: a test that released
 * its own references would leak them on every assertion failure, and the
 * leak surfaces as a "Busy inodes after unmount" splat that buries the
 * assertion message that actually explains the failure.  KUnit's deferred
 * actions cannot be used for this either, as they run after the fixture's
 * exit() has already unmounted.
 */
#ifndef _FS_NTPERS_TESTS_KUNIT_H
#define _FS_NTPERS_TESTS_KUNIT_H

#include <kunit/test.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/slab.h>

/* Enough for any single test case here; asserted rather than grown. */
#define NT_TEST_MAX_DENTRIES	16

struct nt_test_fs {
	struct file_system_type	*fstype;
	struct vfsmount		*mnt;
	struct path		root;
	struct dentry		*tracked[NT_TEST_MAX_DENTRIES];
	unsigned int		nr_tracked;
};

/**
 * nt_test_fs_init - mount a private filesystem for a test case
 * @test: the test
 * @fs:   fixture state to initialise
 *
 * Skips the test if no suitable filesystem is available.  Returns 0 or a
 * negative errno.
 */
static inline int nt_test_fs_init(struct kunit *test, struct nt_test_fs *fs)
{
	/*
	 * tmpfs first: it supports extended attributes, which the metadata
	 * tests need.  ramfs is a fallback that at least exercises the
	 * pathname and case-folding paths.
	 */
	fs->fstype = get_fs_type("tmpfs");
	if (!fs->fstype)
		fs->fstype = get_fs_type("ramfs");
	if (!fs->fstype)
		kunit_skip(test, "neither tmpfs nor ramfs available");

	fs->mnt = kern_mount(fs->fstype);
	if (IS_ERR(fs->mnt)) {
		int err = PTR_ERR(fs->mnt);

		fs->mnt = NULL;
		put_filesystem(fs->fstype);
		fs->fstype = NULL;
		return err;
	}

	fs->root.mnt = fs->mnt;
	fs->root.dentry = fs->mnt->mnt_root;
	path_get(&fs->root);
	fs->nr_tracked = 0;

	return 0;
}

/**
 * nt_test_fs_exit - release everything nt_test_fs_init() and
 *		     nt_test_create() allocated
 * @fs: fixture state
 *
 * Safe to call on a partially initialised fixture, which is what happens
 * when init() skipped or failed.
 */
static inline void nt_test_fs_exit(struct nt_test_fs *fs)
{
	unsigned int i;

	/* Before the unmount, or the mount will still be busy. */
	for (i = 0; i < fs->nr_tracked; i++)
		dput(fs->tracked[i]);
	fs->nr_tracked = 0;

	if (fs->root.dentry)
		path_put(&fs->root);

	if (fs->mnt) {
		/*
		 * A KUnit test case runs in a kthread, and fput() from a
		 * kthread hands the final close to a workqueue.  The
		 * case-insensitive directory scan opens a file, so by the
		 * time we get here that file may still be holding a
		 * reference to this mount.  kern_unmount() assumes its
		 * mntput() is the last one, so the deferred close has to
		 * be forced through first - otherwise the workqueue tears
		 * the mount down later and walks freed memory.
		 */
		flush_delayed_fput();
		kern_unmount(fs->mnt);
	}

	if (fs->fstype)
		put_filesystem(fs->fstype);
}

/**
 * nt_test_untrack - stop tracking a dentry the test is about to destroy
 * @fs:     fixture state
 * @dentry: the dentry
 *
 * For the few tests that unlink a file they created and must not have
 * its reference dropped a second time at exit.
 */
static inline void nt_test_untrack(struct nt_test_fs *fs,
				   struct dentry *dentry)
{
	unsigned int i;

	for (i = 0; i < fs->nr_tracked; i++) {
		if (fs->tracked[i] != dentry)
			continue;
		fs->tracked[i] = fs->tracked[--fs->nr_tracked];
		return;
	}
}

/**
 * nt_test_create - create a file or directory with an exact name
 * @test:   the test
 * @fs:     fixture state
 * @parent: directory to create in
 * @name:   the name, stored exactly as given
 * @dir:    true for a directory
 *
 * Returns a dentry owned by the fixture, or an ERR_PTR.  Do not dput it.
 */
static inline struct dentry *nt_test_create(struct kunit *test,
					    struct nt_test_fs *fs,
					    struct dentry *parent,
					    const char *name, bool dir)
{
	struct qstr q = QSTR_INIT(name, strlen(name));
	struct dentry *child, *made;
	int err;

	KUNIT_ASSERT_LT_MSG(test, fs->nr_tracked,
			    (unsigned int)NT_TEST_MAX_DENTRIES,
			    "raise NT_TEST_MAX_DENTRIES");

	child = start_creating_noperm(parent, &q);
	if (IS_ERR(child))
		return child;

	if (dir) {
		made = vfs_mkdir(&nop_mnt_idmap, d_inode(parent), child, 0755,
				 NULL);
		if (IS_ERR(made)) {
			end_creating(child);
			return made;
		}
		child = end_creating_keep(made);
	} else {
		err = vfs_create(&nop_mnt_idmap, child, 0644, NULL);
		if (err) {
			end_creating(child);
			return ERR_PTR(err);
		}
		child = end_creating_keep(child);
	}

	fs->tracked[fs->nr_tracked++] = child;
	return child;
}

/**
 * nt_test_symlink - create a symlink with an exact name and target
 * @test:   the test
 * @fs:     fixture state
 * @parent: directory to create in
 * @name:   the link's name
 * @target: what it points at, as stored
 *
 * Returns a dentry owned by the fixture, or an ERR_PTR.  Do not dput it.
 */
static inline struct dentry *nt_test_symlink(struct kunit *test,
					     struct nt_test_fs *fs,
					     struct dentry *parent,
					     const char *name,
					     const char *target)
{
	struct qstr q = QSTR_INIT(name, strlen(name));
	struct dentry *child;
	int err;

	KUNIT_ASSERT_LT_MSG(test, fs->nr_tracked,
			    (unsigned int)NT_TEST_MAX_DENTRIES,
			    "raise NT_TEST_MAX_DENTRIES");

	child = start_creating_noperm(parent, &q);
	if (IS_ERR(child))
		return child;

	err = vfs_symlink(&nop_mnt_idmap, d_inode(parent), child, target,
			  NULL);
	if (err) {
		end_creating(child);
		return ERR_PTR(err);
	}
	child = end_creating_keep(child);

	fs->tracked[fs->nr_tracked++] = child;
	return child;
}

/* Build a struct path for a dentry on the fixture's mount. */
static inline void nt_test_path(struct nt_test_fs *fs, struct dentry *dentry,
				struct path *out)
{
	out->mnt = fs->mnt;
	out->dentry = dentry;
}

#endif /* _FS_NTPERS_TESTS_KUNIT_H */
