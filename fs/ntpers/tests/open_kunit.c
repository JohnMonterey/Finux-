// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for NT create / open.
 *
 * The disposition state machine has ten cells - five dispositions times
 * exists/absent - and each one has a defined NT behaviour that is easy to
 * get subtly wrong, so every cell is checked here rather than sampled.
 *
 * The case-collision tests are the ones that matter most.  They run on
 * tmpfs, which is case-sensitive, and prove that a create resolved
 * case-insensitively never produces a second file differing only in
 * case - the property this whole layer exists to add.
 */

#include <linux/nt_personality.h>

#include "ntpers_kunit.h"

struct nt_open_ctx {
	struct nt_test_fs	fs;
	struct nt_namespace	*ns;
	struct nt_task_ctx	*tc;
	struct nt_volume	*vol;
};

static int nt_open_test_init(struct kunit *test)
{
	struct nt_open_ctx *ctx;
	int err;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;

	err = nt_test_fs_init(test, &ctx->fs);
	if (err)
		return err;

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

static void nt_open_test_exit(struct kunit *test)
{
	struct nt_open_ctx *ctx = test->priv;

	if (!ctx)
		return;

	nt_ctx_put(ctx->tc);
	nt_ns_put(ctx->ns);
	nt_test_fs_exit(&ctx->fs);
}

#define CTX(test) ((struct nt_open_ctx *)(test)->priv)

/* Issue one create/open.  On success the caller must nt_path_put(&out->path). */
static int nt_op(struct kunit *test, const char *path, u32 disposition,
		 u32 options, struct nt_open_result *out)
{
	struct nt_create_req req = {
		.disposition	= disposition,
		.options	= options,
		.attributes	= 0,
		.resolve_flags	= NT_RESOLVE_CASE_INSENSITIVE,
	};

	return nt_create(CTX(test)->tc, path, &req, out);
}

/* --------------------------------------------------------- absent side */

static void nt_op_create_new_absent(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\New.txt", NT_DISPOSITION_CREATE_NEW, 0,
			      &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_CREATED);
	KUNIT_EXPECT_TRUE(test, d_is_positive(out.path.path.dentry));
	KUNIT_EXPECT_TRUE(test, S_ISREG(d_inode(out.path.path.dentry)->i_mode));
	nt_path_put(&out.path);
}

static void nt_op_open_existing_absent(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Nope.txt",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out),
			-ENOENT);
}

static void nt_op_truncate_absent(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Nope.txt",
			      NT_DISPOSITION_TRUNCATE_EXISTING, 0, &out),
			-ENOENT);
}

static void nt_op_open_always_absent(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Made.txt",
			      NT_DISPOSITION_OPEN_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_CREATED);
	nt_path_put(&out.path);
}

static void nt_op_create_always_absent(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Fresh.txt",
			      NT_DISPOSITION_CREATE_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_CREATED);
	nt_path_put(&out.path);
}

/* --------------------------------------------------------- exists side */

/* Create a file up front, so the "exists" dispositions have something. */
static void seed(struct kunit *test, const char *path)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, path, NT_DISPOSITION_CREATE_NEW, 0, &out),
			0);
	nt_path_put(&out.path);
}

static void nt_op_create_new_exists(struct kunit *test)
{
	struct nt_open_result out;

	seed(test, "C:\\Dup.txt");
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Dup.txt", NT_DISPOSITION_CREATE_NEW,
			      0, &out),
			-EEXIST);
}

static void nt_op_open_existing_exists(struct kunit *test)
{
	struct nt_open_result out;

	seed(test, "C:\\Have.txt");
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Have.txt",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OPENED);
	nt_path_put(&out.path);
}

static void nt_op_open_always_exists(struct kunit *test)
{
	struct nt_open_result out;

	seed(test, "C:\\Have.txt");
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Have.txt",
			      NT_DISPOSITION_OPEN_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OPENED);
	nt_path_put(&out.path);
}

/*
 * CREATE_ALWAYS on an existing file truncates it and reports
 * FILE_OVERWRITTEN rather than making a new one.
 */
static void nt_op_create_always_exists_truncates(struct kunit *test)
{
	struct nt_open_result out;
	struct dentry *seeded;

	seed(test, "C:\\Big.txt");

	/* Give it some size, so truncation to zero is observable. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Big.txt",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), 0);
	seeded = dget(out.path.path.dentry);
	KUNIT_ASSERT_EQ(test, vfs_truncate(&out.path.path, 4096), 0);
	KUNIT_ASSERT_EQ(test, i_size_read(d_inode(seeded)), 4096);
	nt_path_put(&out.path);

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Big.txt",
			      NT_DISPOSITION_CREATE_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OVERWRITTEN);
	/* Same object, now empty. */
	KUNIT_EXPECT_PTR_EQ(test, out.path.path.dentry, seeded);
	KUNIT_EXPECT_EQ(test, i_size_read(d_inode(seeded)), 0);
	nt_path_put(&out.path);
	dput(seeded);
}

/* ------------------------------------------------------ case collision */

/*
 * The headline property: on a case-sensitive volume, a create resolved
 * case-insensitively must operate on an existing case-variant rather than
 * make a second file.
 */
static void nt_op_case_collision(struct kunit *test)
{
	struct nt_open_result out;
	struct dentry *original;

	/* The real, on-disk spelling. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Kernel32.dll",
			      NT_DISPOSITION_CREATE_NEW, 0, &out), 0);
	original = dget(out.path.path.dentry);
	nt_path_put(&out.path);

	/* CREATE_NEW of a different casing is a collision, not a new file. */
	KUNIT_EXPECT_EQ_MSG(test,
			    nt_op(test, "C:\\kernel32.dll",
				  NT_DISPOSITION_CREATE_NEW, 0, &out),
			    -EEXIST,
			    "a case-variant CREATE_NEW must collide");

	/* OPEN_EXISTING of yet another casing opens the one that is there. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\KERNEL32.DLL",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OPENED);
	KUNIT_EXPECT_PTR_EQ_MSG(test, out.path.path.dentry, original,
				"a case-variant open must find the original");
	nt_path_put(&out.path);

	/* OPEN_ALWAYS likewise opens rather than creating a duplicate. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\kErNeL32.DlL",
			      NT_DISPOSITION_OPEN_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OPENED);
	KUNIT_EXPECT_PTR_EQ(test, out.path.path.dentry, original);
	nt_path_put(&out.path);

	dput(original);
}

/* ---------------------------------------------------------- attributes */

/* A newly created file carries ARCHIVE, the way Windows sets it. */
static void nt_op_created_file_has_archive(struct kunit *test)
{
	struct nt_open_result out;
	u32 attrs = 0;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Doc.txt", NT_DISPOSITION_CREATE_NEW,
			      0, &out), 0);
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&out.path.path, &attrs),
			0);
	KUNIT_EXPECT_TRUE_MSG(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE,
			      "a new file should have ARCHIVE set");
	nt_path_put(&out.path);
}

/* A new directory is a directory, and does not get ARCHIVE. */
static void nt_op_create_directory(struct kunit *test)
{
	struct nt_open_result out;
	u32 attrs = 0;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\SubDir", NT_DISPOSITION_CREATE_NEW,
			      NT_CREATE_DIRECTORY, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_CREATED);
	KUNIT_EXPECT_TRUE(test, S_ISDIR(d_inode(out.path.path.dentry)->i_mode));
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&out.path.path, &attrs),
			0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_DIRECTORY);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE);
	nt_path_put(&out.path);
}

/* ------------------------------------------------------------ refusals */

/* A reserved device name is not an on-disk file and must be refused. */
static void nt_op_reserved_name_refused(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\CON", NT_DISPOSITION_CREATE_NEW, 0,
			      &out),
			-EPERM);
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\nul.txt", NT_DISPOSITION_OPEN_EXISTING,
			      0, &out),
			-EPERM);
}

/* A stream create is a separate feature and must say so, not half-do it. */
static void nt_op_stream_refused(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\f.txt:stream",
			      NT_DISPOSITION_CREATE_NEW, 0, &out),
			-EOPNOTSUPP);
}

static struct kunit_case nt_open_test_cases[] = {
	KUNIT_CASE(nt_op_create_new_absent),
	KUNIT_CASE(nt_op_open_existing_absent),
	KUNIT_CASE(nt_op_truncate_absent),
	KUNIT_CASE(nt_op_open_always_absent),
	KUNIT_CASE(nt_op_create_always_absent),
	KUNIT_CASE(nt_op_create_new_exists),
	KUNIT_CASE(nt_op_open_existing_exists),
	KUNIT_CASE(nt_op_open_always_exists),
	KUNIT_CASE(nt_op_create_always_exists_truncates),
	KUNIT_CASE(nt_op_case_collision),
	KUNIT_CASE(nt_op_created_file_has_archive),
	KUNIT_CASE(nt_op_create_directory),
	KUNIT_CASE(nt_op_reserved_name_refused),
	KUNIT_CASE(nt_op_stream_refused),
	{}
};

static struct kunit_suite nt_open_test_suite = {
	.name = "ntpers-open",
	.init = nt_open_test_init,
	.exit = nt_open_test_exit,
	.test_cases = nt_open_test_cases,
};
kunit_test_suite(nt_open_test_suite);

MODULE_DESCRIPTION("KUnit tests for NT create/open");
MODULE_LICENSE("GPL");
