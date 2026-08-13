// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for case-insensitive pathname resolution.
 *
 * This is the headline behaviour of the personality - "C:\Windows",
 * "C:\WINDOWS" and "C:\windows" naming the same directory while the
 * stored spelling is preserved - so it is tested against a real
 * directory tree rather than a mock.
 *
 * A private tmpfs mount is used, so the tests never touch the running
 * system's files and do not depend on what it has mounted.  tmpfs is
 * case-sensitive, which means these tests exercise tier 2 of
 * casefold.c: the fold-hint cache and the directory scan behind it.
 * That is the interesting tier; tier 1 is the dcache doing its job and
 * has its own coverage in fs/unicode.
 */

#include <linux/cred.h>
#include <linux/nt_personality.h>

#include "ntpers_kunit.h"

static int nt_cf_test_init(struct kunit *test)
{
	struct nt_test_fs *fs;

	fs = kunit_kzalloc(test, sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;
	test->priv = fs;

	return nt_test_fs_init(test, fs);
}

static void nt_cf_test_exit(struct kunit *test)
{
	if (test->priv)
		nt_test_fs_exit(test->priv);
}

#define CTX(test) ((struct nt_test_fs *)(test)->priv)

/* Create in the fixture root; the fixture owns the reference. */
static struct dentry *nt_cf_create(struct kunit *test, struct dentry *parent,
				   const char *name, bool dir)
{
	return nt_test_create(test, CTX(test), parent, name, dir);
}

/* Look @name up case-insensitively and assert the spelling we get back. */
static void expect_ci_finds(struct kunit *test, const struct path *dir,
			    const char *lookup, const char *stored)
{
	struct path found;
	int err;

	err = nt_ci_lookup(dir, lookup, strlen(lookup), &found);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "looking up \"%s\"", lookup);

	/*
	 * Case-preserving means the dentry we get back carries the name as
	 * it was created, not as it was asked for.
	 */
	KUNIT_EXPECT_STREQ_MSG(test, found.dentry->d_name.name, stored,
			       "\"%s\" should resolve to stored name \"%s\"",
			       lookup, stored);
	path_put(&found);
}

static void expect_ci_misses(struct kunit *test, const struct path *dir,
			     const char *lookup)
{
	struct path found;
	int err;

	err = nt_ci_lookup(dir, lookup, strlen(lookup), &found);
	KUNIT_EXPECT_EQ_MSG(test, err, -ENOENT, "looking up \"%s\"", lookup);
	if (!err)
		path_put(&found);
}

/*
 * The core promise: create TestFile.txt, find it through any casing,
 * and always get the stored spelling back.
 */
static void test_ci_lookup_preserves_case(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *file;

	file = nt_cf_create(test, ctx->root.dentry, "TestFile.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));

	expect_ci_finds(test, &ctx->root, "TestFile.txt", "TestFile.txt");
	expect_ci_finds(test, &ctx->root, "testfile.txt", "TestFile.txt");
	expect_ci_finds(test, &ctx->root, "TESTFILE.TXT", "TestFile.txt");
	expect_ci_finds(test, &ctx->root, "TeStFiLe.TxT", "TestFile.txt");
	expect_ci_finds(test, &ctx->root, "testFILE.txt", "TestFile.txt");

	/* A name that differs by more than case must still not be found. */
	expect_ci_misses(test, &ctx->root, "TestFile2.txt");
	expect_ci_misses(test, &ctx->root, "TestFil.txt");
}

/*
 * The second and later lookups of a given casing come from the fold-hint
 * cache rather than a directory scan.  The observable result must be
 * identical, which is what makes the cache safe.
 */
static void test_ci_lookup_is_cached(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *file;
	int i;

	file = nt_cf_create(test, ctx->root.dentry, "CachedName.dat", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));

	/* First pass populates, the rest hit. */
	for (i = 0; i < 5; i++)
		expect_ci_finds(test, &ctx->root, "cachedname.dat",
				"CachedName.dat");
}

/*
 * A cached hint that no longer matches reality must not produce a wrong
 * answer.  Delete the file the hint points at, recreate it with a
 * different spelling, and check the next lookup follows the new one.
 */
static void test_ci_stale_hint_is_reverified(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *file, *victim;
	struct qstr q;
	int err;

	file = nt_cf_create(test, ctx->root.dentry, "Renamed.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));

	/* Populate the hint cache with "Renamed.txt". */
	expect_ci_finds(test, &ctx->root, "RENAMED.TXT", "Renamed.txt");

	/* Remove it behind the cache's back. */
	q = (struct qstr)QSTR_INIT("Renamed.txt", 11);
	victim = start_removing_noperm(ctx->root.dentry, &q);
	KUNIT_ASSERT_FALSE(test, IS_ERR(victim));
	err = vfs_unlink(&nop_mnt_idmap, d_inode(ctx->root.dentry), victim,
			 NULL);
	end_removing_path(&ctx->root, victim);
	KUNIT_ASSERT_EQ(test, err, 0);

	/* The stale hint must not resurrect it. */
	expect_ci_misses(test, &ctx->root, "RENAMED.TXT");

	/* Recreate with different casing; the cache must follow. */
	file = nt_cf_create(test, ctx->root.dentry, "RENAMED.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	expect_ci_finds(test, &ctx->root, "renamed.txt", "RENAMED.txt");
	expect_ci_finds(test, &ctx->root, "Renamed.Txt", "RENAMED.txt");
}

/* Two names that differ only by case can both exist on a case-sensitive
 * filesystem.  An exact match must win over any folded one, because that
 * is the only answer that can be right.
 */
static void test_ci_exact_match_wins(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *lower, *upper;

	lower = nt_cf_create(test, ctx->root.dentry, "clash.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lower));
	upper = nt_cf_create(test, ctx->root.dentry, "CLASH.TXT", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(upper));

	expect_ci_finds(test, &ctx->root, "clash.txt", "clash.txt");
	expect_ci_finds(test, &ctx->root, "CLASH.TXT", "CLASH.TXT");
}

static void test_ci_lookup_directories(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *dir, *inner;
	struct path dirpath;

	dir = nt_cf_create(test, ctx->root.dentry, "Windows", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));

	inner = nt_cf_create(test, dir, "System32", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(inner));

	expect_ci_finds(test, &ctx->root, "windows", "Windows");
	expect_ci_finds(test, &ctx->root, "WINDOWS", "Windows");

	/* And a second level, through the differently-cased first one. */
	dirpath.mnt = ctx->mnt;
	dirpath.dentry = dir;
	expect_ci_finds(test, &dirpath, "system32", "System32");
	expect_ci_finds(test, &dirpath, "SYSTEM32", "System32");
}

/* A whole Windows 7 style path, walked case-insensitively end to end. */
static void test_ci_resolve_windows_layout(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *windows, *system32, *dll;
	struct nt_namespace *ns;
	struct nt_path_parse parse;
	struct nt_task_ctx *tc;
	struct nt_volume *vol;
	struct nt_path out;
	char *buf;
	int err;

	buf = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	windows = nt_cf_create(test, ctx->root.dentry, "Windows", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(windows));
	system32 = nt_cf_create(test, windows, "System32", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(system32));
	dll = nt_cf_create(test, system32, "kernel32.dll", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dll));

	ns = nt_ns_create();
	KUNIT_ASSERT_NOT_NULL(test, ns);

	vol = nt_volume_create(ns, &ctx->root, 'C', NT_VOL_SYSTEM, "System");
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	tc = nt_ctx_alloc(ns);
	KUNIT_ASSERT_NOT_NULL(test, tc);

	/*
	 * The path a PE loader will ask for, in the casing Windows
	 * software actually uses, and in two others.
	 */
	{
		static const char * const spellings[] = {
			"C:\\Windows\\System32\\kernel32.dll",
			"c:\\windows\\system32\\kernel32.dll",
			"C:\\WINDOWS\\SYSTEM32\\KERNEL32.DLL",
			"C:/Windows/System32/kernel32.dll",
			"C:\\Windows\\.\\System32\\..\\System32\\kernel32.dll",
		};
		size_t i;

		for (i = 0; i < ARRAY_SIZE(spellings); i++) {
			err = nt_path_parse(spellings[i],
					    strlen(spellings[i]), 0, buf,
					    NT_PATH_BUF_SIZE, &parse);
			KUNIT_ASSERT_EQ_MSG(test, err, 0, "parsing %s",
					    spellings[i]);

			err = nt_path_resolve(tc, &parse,
					      NT_RESOLVE_CASE_INSENSITIVE,
					      &out);
			KUNIT_ASSERT_EQ_MSG(test, err, 0, "resolving %s",
					    spellings[i]);
			KUNIT_EXPECT_PTR_EQ_MSG(test, out.path.dentry, dll,
						"%s resolved elsewhere",
						spellings[i]);
			KUNIT_EXPECT_PTR_EQ(test, out.volume, vol);
			nt_path_put(&out);
		}
	}

	/* Without the flag, the wrong casing must not resolve. */
	err = nt_path_parse("C:\\WINDOWS\\SYSTEM32", 19, 0, buf,
			    NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, nt_path_resolve(tc, &parse, 0, &out), -ENOENT);

	/* A directory that does not exist in any casing stays absent. */
	err = nt_path_parse("C:\\Windows\\NoSuchDir", 20, 0, buf,
			    NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, nt_path_resolve(tc, &parse,
					      NT_RESOLVE_CASE_INSENSITIVE,
					      &out), -ENOENT);

	/* NT_RESOLVE_DIRECTORY must reject a file. */
	err = nt_path_parse("C:\\windows\\system32\\kernel32.dll", 32, 0, buf,
			    NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, nt_path_resolve(tc, &parse,
					      NT_RESOLVE_CASE_INSENSITIVE |
					      NT_RESOLVE_DIRECTORY, &out),
			-ENOTDIR);

	/* Resolving the parent gives the containing directory. */
	err = nt_path_parse("C:\\windows\\system32\\kernel32.dll", 32, 0, buf,
			    NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	err = nt_path_resolve(tc, &parse,
			      NT_RESOLVE_CASE_INSENSITIVE |
			      NT_RESOLVE_PARENT, &out);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, system32);
	nt_path_put(&out);

	nt_ctx_put(tc);
	nt_volume_destroy(ns, vol);
	nt_volume_put(vol);
	nt_ns_put(ns);
}

/* Unicode names must survive folding rather than becoming unreachable. */
static void test_ci_unicode_names(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *file;

	file = nt_cf_create(test, ctx->root.dentry, "Документы", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));

	/* Exact match always works, whatever the folding rules say. */
	expect_ci_finds(test, &ctx->root, "Документы", "Документы");

	/*
	 * A name whose case folding is pure ASCII must fold even when the
	 * rest of it is not.
	 */
	file = nt_cf_create(test, ctx->root.dentry, "Ünïcøde-DIR", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	expect_ci_finds(test, &ctx->root, "Ünïcøde-dir", "Ünïcøde-DIR");
}

static void test_ci_argument_checking(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	struct dentry *file;
	struct path found;
	struct path filepath;
	char toolong[NT_MAX_COMPONENT + 8];

	KUNIT_EXPECT_EQ(test, nt_ci_lookup(NULL, "x", 1, &found), -EINVAL);
	KUNIT_EXPECT_EQ(test, nt_ci_lookup(&ctx->root, NULL, 1, &found),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, nt_ci_lookup(&ctx->root, "x", 0, &found),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, nt_ci_lookup(&ctx->root, "x", 1, NULL),
			-EINVAL);

	memset(toolong, 'a', sizeof(toolong));
	KUNIT_EXPECT_EQ(test, nt_ci_lookup(&ctx->root, toolong,
					   sizeof(toolong), &found),
			-ENAMETOOLONG);

	/* Looking inside a non-directory is -ENOTDIR, not -ENOENT. */
	file = nt_cf_create(test, ctx->root.dentry, "notadir", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	filepath.mnt = ctx->mnt;
	filepath.dentry = file;
	KUNIT_EXPECT_EQ(test, nt_ci_lookup(&filepath, "x", 1, &found),
			-ENOTDIR);
}

/*
 * Matching a name by a casing other than the stored one requires reading
 * the directory, so it requires read permission on it - unlike an exact
 * lookup, which only needs search permission.  Check that a mode 0711
 * directory therefore serves the exact name and refuses the folded one,
 * rather than quietly enumerating a directory the owner made unreadable.
 *
 * KUnit runs as root, which bypasses DAC entirely, so the test installs
 * an unprivileged credential for the duration of the two lookups.  That
 * is the only way to observe the behaviour it is asserting.
 */
static void test_ci_scan_needs_read_permission(struct kunit *test)
{
	struct nt_test_fs *ctx = CTX(test);
	const struct cred *old_cred;
	struct dentry *dir, *file;
	struct path dirpath, found;
	struct cred *unpriv;
	struct iattr attr = {
		.ia_valid = ATTR_MODE,
		.ia_mode = S_IFDIR | 0311,
	};
	int err;

	dir = nt_cf_create(test, ctx->root.dentry, "Locked", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));

	file = nt_cf_create(test, dir, "Secret.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));

	/* Search permission but no read permission: the interesting case. */
	inode_lock(d_inode(dir));
	err = notify_change(&nop_mnt_idmap, dir, &attr, NULL);
	inode_unlock(d_inode(dir));
	if (err)
		kunit_skip(test, "could not drop read permission (%d)", err);

	unpriv = prepare_creds();
	if (!unpriv)
		kunit_skip(test, "could not prepare credentials");

	/*
	 * Drop every capability that would let the caller ignore the mode,
	 * and stop being uid 0, which owns the directory we just locked.
	 */
	cap_clear(unpriv->cap_effective);
	cap_clear(unpriv->cap_permitted);
	cap_clear(unpriv->cap_bset);
	unpriv->fsuid = make_kuid(unpriv->user_ns, 65534);
	unpriv->fsgid = make_kgid(unpriv->user_ns, 65534);

	dirpath.mnt = ctx->mnt;
	dirpath.dentry = dir;

	old_cred = override_creds(unpriv);

	/* The exact name still works: search permission is enough. */
	err = nt_ci_lookup(&dirpath, "Secret.txt", 10, &found);
	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "exact lookup needs only search permission");
	if (!err)
		path_put(&found);

	/*
	 * A different casing has to read the directory, and must be
	 * refused rather than enumerating it one guess at a time.
	 */
	err = nt_ci_lookup(&dirpath, "secret.txt", 10, &found);
	KUNIT_EXPECT_EQ_MSG(test, err, -EACCES,
			    "folded lookup must not read an unreadable dir");
	if (!err)
		path_put(&found);

	revert_creds(old_cred);
	put_cred(unpriv);
}

static struct kunit_case nt_casefold_test_cases[] = {
	KUNIT_CASE(test_ci_lookup_preserves_case),
	KUNIT_CASE(test_ci_lookup_is_cached),
	KUNIT_CASE(test_ci_stale_hint_is_reverified),
	KUNIT_CASE(test_ci_exact_match_wins),
	KUNIT_CASE(test_ci_lookup_directories),
	KUNIT_CASE(test_ci_resolve_windows_layout),
	KUNIT_CASE(test_ci_unicode_names),
	KUNIT_CASE(test_ci_scan_needs_read_permission),
	KUNIT_CASE(test_ci_argument_checking),
	{}
};

static struct kunit_suite nt_casefold_test_suite = {
	.name = "ntpers-casefold",
	.init = nt_cf_test_init,
	.exit = nt_cf_test_exit,
	.test_cases = nt_casefold_test_cases,
};
kunit_test_suite(nt_casefold_test_suite);

MODULE_DESCRIPTION("KUnit tests for NT case-insensitive resolution");
MODULE_LICENSE("GPL");
