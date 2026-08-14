// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the parts of nt_path_resolve() that are easy to get
 * subtly wrong and hard to notice.
 *
 * Two behaviours are covered here, both of which were shipped broken and
 * both of which the existing suites missed for the same reason: they only
 * ever resolved ordinary files on an ordinary tree, where the wrong answer
 * and the right answer look identical.
 *
 *   - Symlinks as the final component.  NT_RESOLVE_FOLLOW must resolve
 *     one, and its absence must return the link itself, the way a create
 *     carrying FILE_OPEN_REPARSE_POINT expects.  These are opposites, so a
 *     test that only checks "does it return an error" passes either way.
 *
 *   - The case-folding fast path.  A volume whose root folds case is not a
 *     volume where every directory does, and a lookup that misses in the
 *     fast path must be retried before the answer is believed.  Tested by
 *     telling the resolver a case-sensitive volume folds case, which is
 *     exactly the state a mixed tree puts it in.
 *
 * A private tmpfs mount is used throughout, so nothing here touches the
 * running system.
 */

#include <linux/nt_personality.h>

#include "ntpers_kunit.h"
#include "../internal.h"

struct nt_rs_ctx {
	struct nt_test_fs	fs;
	struct nt_namespace	*ns;
	struct nt_task_ctx	*tc;
	struct nt_volume	*vol;
	char			*buf;
};

static int nt_rs_test_init(struct kunit *test)
{
	struct nt_rs_ctx *ctx;
	int err;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;

	err = nt_test_fs_init(test, &ctx->fs);
	if (err)
		return err;

	ctx->buf = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	if (!ctx->buf)
		return -ENOMEM;

	ctx->ns = nt_ns_create();
	if (!ctx->ns)
		return -ENOMEM;

	ctx->vol = nt_volume_create(ctx->ns, &ctx->fs.root, 'C',
				    NT_VOL_SYSTEM, "System");
	if (IS_ERR(ctx->vol))
		return PTR_ERR(ctx->vol);

	ctx->tc = nt_ctx_alloc(ctx->ns);
	if (!ctx->tc)
		return -ENOMEM;

	return 0;
}

static void nt_rs_test_exit(struct kunit *test)
{
	struct nt_rs_ctx *ctx = test->priv;

	if (!ctx)
		return;

	nt_ctx_put(ctx->tc);
	nt_ns_put(ctx->ns);
	nt_test_fs_exit(&ctx->fs);
}

#define CTX(test) ((struct nt_rs_ctx *)(test)->priv)

/* Parse @path and resolve it with @flags, asserting the parse succeeded. */
static int nt_rs_resolve(struct kunit *test, const char *path, u32 flags,
			 struct nt_path *out)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct nt_path_parse parse;
	int err;

	err = nt_path_parse(path, strlen(path), 0, ctx->buf,
			    NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "parsing %s", path);

	return nt_path_resolve(ctx->tc, &parse, flags, out);
}

/*
 * A symlink as the final component.
 *
 * NT_RESOLVE_FOLLOW previously selected the final component for
 * rejection rather than resolution, so the flag meant the opposite of its
 * name and every follow returned -ELOOP.
 */
static void nt_rs_final_symlink(struct kunit *test)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct dentry *target, *link;
	struct nt_path out;

	target = nt_test_create(test, &ctx->fs, ctx->fs.root.dentry,
				"Real.dll", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(target));
	link = nt_test_symlink(test, &ctx->fs, ctx->fs.root.dentry,
			       "Link.dll", "Real.dll");
	KUNIT_ASSERT_FALSE(test, IS_ERR(link));

	/* With FOLLOW: the target, not the link, and not an error. */
	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\Link.dll",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_FOLLOW, &out), 0);
	KUNIT_EXPECT_PTR_EQ_MSG(test, out.path.dentry, target,
				"NT_RESOLVE_FOLLOW did not follow the link");
	nt_path_put(&out);

	/* Without it: the link itself, still not an error. */
	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\Link.dll",
				      NT_RESOLVE_CASE_INSENSITIVE, &out), 0);
	KUNIT_EXPECT_PTR_EQ_MSG(test, out.path.dentry, link,
				"no FOLLOW should return the link itself");
	nt_path_put(&out);

	/* Case folding still applies to the link's own name. */
	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\LINK.DLL",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_FOLLOW, &out), 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, target);
	nt_path_put(&out);
}

/* A symlink to a directory, followed, satisfies NT_RESOLVE_DIRECTORY. */
static void nt_rs_symlink_to_dir(struct kunit *test)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct dentry *dir, *link;
	struct nt_path out;

	dir = nt_test_create(test, &ctx->fs, ctx->fs.root.dentry, "System32",
			     true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));
	link = nt_test_symlink(test, &ctx->fs, ctx->fs.root.dentry, "sys32",
			       "System32");
	KUNIT_ASSERT_FALSE(test, IS_ERR(link));

	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\SYS32",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_FOLLOW |
				      NT_RESOLVE_DIRECTORY, &out), 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, dir);
	nt_path_put(&out);

	/*
	 * Unfollowed, the link is not a directory, so the same request
	 * without FOLLOW has to fail rather than quietly return the link.
	 */
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\SYS32",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_DIRECTORY, &out),
			-ENOTDIR);
}

/* A broken link is -ENOENT when followed, and resolvable when not. */
static void nt_rs_dangling_symlink(struct kunit *test)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct dentry *link;
	struct nt_path out;

	link = nt_test_symlink(test, &ctx->fs, ctx->fs.root.dentry, "Gone.dll",
			       "NoSuchFile.dll");
	KUNIT_ASSERT_FALSE(test, IS_ERR(link));

	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\Gone.dll",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_FOLLOW, &out),
			-ENOENT);

	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\gone.dll",
				      NT_RESOLVE_CASE_INSENSITIVE, &out), 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, link);
	nt_path_put(&out);
}

/*
 * A symlink in the middle of a path is still refused.
 *
 * This is the documented limitation of the folding walk, and it has to
 * stay a deliberate -ELOOP rather than become a silent resolution once
 * the final-component case learned to follow.
 */
static void nt_rs_intermediate_symlink(struct kunit *test)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct dentry *dir, *file, *link;
	struct nt_path out;

	dir = nt_test_create(test, &ctx->fs, ctx->fs.root.dentry, "Windows",
			     true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));
	file = nt_test_create(test, &ctx->fs, dir, "win.ini", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	link = nt_test_symlink(test, &ctx->fs, ctx->fs.root.dentry, "WinDir",
			       "Windows");
	KUNIT_ASSERT_FALSE(test, IS_ERR(link));

	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\WinDir\\win.ini",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_FOLLOW, &out),
			-ELOOP);

	/* The same path without the link resolves, so the tree is fine. */
	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\windows\\WIN.INI",
				      NT_RESOLVE_CASE_INSENSITIVE, &out), 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, file);
	nt_path_put(&out);
}

/*
 * Casefolding is per-directory, so NT_VOL_NATIVE_CI can be wrong.
 *
 * The flag is decided once, from the volume root, and a tree can be
 * mixed - a casefolded root can contain a directory that predates the +F
 * flag.  The fast path then hands the whole path to the ordinary VFS
 * walk, which matches case-sensitively inside that directory and returns
 * -ENOENT for a name that is really there.
 *
 * tmpfs cannot casefold, so this sets the flag by hand.  That produces
 * precisely the state a mixed tree produces: a resolver that believes the
 * filesystem will fold for it, on a directory where it will not.
 */
static void nt_rs_mixed_casefold(struct kunit *test)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct dentry *dir, *file;
	struct nt_path out;

	dir = nt_test_create(test, &ctx->fs, ctx->fs.root.dentry, "Program",
			     true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));
	file = nt_test_create(test, &ctx->fs, dir, "Config.ini", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));

	/* Exact spelling: resolves either way, and proves the tree is sane. */
	KUNIT_ASSERT_EQ(test,
			nt_rs_resolve(test, "C:\\Program\\Config.ini",
				      NT_RESOLVE_CASE_INSENSITIVE, &out), 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, file);
	nt_path_put(&out);

	ctx->vol->flags |= NT_VOL_NATIVE_CI;

	/*
	 * The fast path is now taken and will miss.  A correct resolver
	 * retries; the one that trusted the flag reported -ENOENT for a
	 * file the caller can see in a directory listing.
	 */
	KUNIT_ASSERT_EQ_MSG(test,
			    nt_rs_resolve(test, "C:\\PROGRAM\\CONFIG.INI",
					  NT_RESOLVE_CASE_INSENSITIVE, &out),
			    0,
			    "a mis-set NATIVE_CI must not turn into -ENOENT");
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, file);
	nt_path_put(&out);

	/*
	 * A name that really is absent must still be absent - the retry
	 * must not invent one - and a case-sensitive caller must not get
	 * the folded answer just because the retry exists.
	 */
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\PROGRAM\\Missing.ini",
				      NT_RESOLVE_CASE_INSENSITIVE, &out),
			-ENOENT);
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\PROGRAM\\CONFIG.INI", 0, &out),
			-ENOENT);

	ctx->vol->flags &= ~NT_VOL_NATIVE_CI;
}

/*
 * The retry is not paid on every miss.
 *
 * Correctness alone would be satisfied by re-walking each time the fast
 * path returns -ENOENT, and that would be expensive in the worst place:
 * a PE loader resolving an import misses most of the DLL search order,
 * and a folding walk reads a directory per component.  The guard trusts
 * the miss when the final component's parent resolves and folds.
 *
 * tmpfs never folds, so every miss here is genuinely unresolvable by the
 * fast path and every one must retry - which is what makes the *other*
 * direction testable: a miss whose parent does not even exist has
 * nothing above it that could fold, and must not be retried either.
 */
static void nt_rs_retry_is_bounded(struct kunit *test)
{
	struct nt_rs_ctx *ctx = CTX(test);
	struct dentry *dir;
	struct nt_path out;
	long before;

	dir = nt_test_create(test, &ctx->fs, ctx->fs.root.dentry, "Windows",
			     true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));

	ctx->vol->flags |= NT_VOL_NATIVE_CI;

	/*
	 * A missing name in a directory that exists: the parent resolves
	 * but does not fold, so this one has to be re-walked.
	 */
	before = nt_ci_retry_count();
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\Windows\\Nothing.dll",
				      NT_RESOLVE_CASE_INSENSITIVE, &out),
			-ENOENT);
	KUNIT_EXPECT_GT_MSG(test, nt_ci_retry_count(), before,
			    "a non-folding parent must be re-walked");

	/*
	 * A missing name in a directory that does not exist either.  The
	 * parent lookup fails, so there is no folding directory to trust
	 * and the walk still has to run - it is the only thing that can
	 * tell "absent" from "spelled differently" for the parent itself.
	 */
	before = nt_ci_retry_count();
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\NoSuchDir\\Nothing.dll",
				      NT_RESOLVE_CASE_INSENSITIVE, &out),
			-ENOENT);
	KUNIT_EXPECT_GT(test, nt_ci_retry_count(), before);

	/*
	 * A case-sensitive caller never took the folding path to begin
	 * with, so it must never trigger a retry however badly the volume
	 * flag is set.
	 */
	before = nt_ci_retry_count();
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\Windows\\Nothing.dll", 0,
				      &out),
			-ENOENT);
	KUNIT_EXPECT_EQ_MSG(test, nt_ci_retry_count(), before,
			    "a case-sensitive miss must not be re-walked");

	/*
	 * Nor should an error that is not -ENOENT.  A file where a
	 * directory was demanded is absent in no sense at all.
	 */
	KUNIT_ASSERT_FALSE(test, IS_ERR(nt_test_create(test, &ctx->fs, dir,
						       "win.ini", false)));
	before = nt_ci_retry_count();
	KUNIT_EXPECT_EQ(test,
			nt_rs_resolve(test, "C:\\Windows\\win.ini",
				      NT_RESOLVE_CASE_INSENSITIVE |
				      NT_RESOLVE_DIRECTORY, &out),
			-ENOTDIR);
	KUNIT_EXPECT_EQ_MSG(test, nt_ci_retry_count(), before,
			    "-ENOTDIR is not a case-folding miss");

	ctx->vol->flags &= ~NT_VOL_NATIVE_CI;
}

static struct kunit_case nt_resolve_test_cases[] = {
	KUNIT_CASE(nt_rs_final_symlink),
	KUNIT_CASE(nt_rs_symlink_to_dir),
	KUNIT_CASE(nt_rs_dangling_symlink),
	KUNIT_CASE(nt_rs_intermediate_symlink),
	KUNIT_CASE(nt_rs_mixed_casefold),
	KUNIT_CASE(nt_rs_retry_is_bounded),
	{}
};

static struct kunit_suite nt_resolve_test_suite = {
	.name = "ntpers-resolve",
	.init = nt_rs_test_init,
	.exit = nt_rs_test_exit,
	.test_cases = nt_resolve_test_cases,
};
kunit_test_suite(nt_resolve_test_suite);

MODULE_DESCRIPTION("KUnit tests for NT personality path resolution");
MODULE_LICENSE("GPL");
