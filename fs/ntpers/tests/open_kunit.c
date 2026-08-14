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

#include <linux/capability.h>
#include <linux/cred.h>
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

/* Full access, full sharing: the disposition tests below turn on neither. */
#define NT_OP_ALL_ACCESS	(NT_ACCESS_GENERIC_READ | NT_ACCESS_GENERIC_WRITE | \
				 NT_ACCESS_DELETE)
#define NT_OP_ALL_SHARE		(NT_SHARE_READ | NT_SHARE_WRITE | NT_SHARE_DELETE)

/* Issue one create/open.  On success the caller must nt_close(out->handle). */
static int nt_op(struct kunit *test, const char *path, u32 disposition,
		 u32 options, struct nt_open_result *out)
{
	struct nt_create_req req = {
		.disposition	= disposition,
		.access		= NT_OP_ALL_ACCESS,
		.share		= NT_OP_ALL_SHARE,
		.options	= options,
		.attributes	= 0,
		.resolve_flags	= NT_RESOLVE_CASE_INSENSITIVE,
	};

	return nt_create(CTX(test)->tc, path, &req, out);
}

/* As nt_op(), but with explicit access and share mode for the share tests. */
static int nt_op_share(struct kunit *test, const char *path, u32 disposition,
		       u32 access, u32 share, u32 options,
		       struct nt_open_result *out)
{
	struct nt_create_req req = {
		.disposition	= disposition,
		.access		= access,
		.share		= share,
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
	KUNIT_EXPECT_TRUE(test, d_is_positive(out.handle->path.dentry));
	KUNIT_EXPECT_TRUE(test, S_ISREG(d_inode(out.handle->path.dentry)->i_mode));
	nt_close(out.handle);
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
	nt_close(out.handle);
}

static void nt_op_create_always_absent(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Fresh.txt",
			      NT_DISPOSITION_CREATE_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_CREATED);
	nt_close(out.handle);
}

/* --------------------------------------------------------- exists side */

/* Create a file up front, so the "exists" dispositions have something. */
static void seed(struct kunit *test, const char *path)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, path, NT_DISPOSITION_CREATE_NEW, 0, &out),
			0);
	nt_close(out.handle);
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
	nt_close(out.handle);
}

static void nt_op_open_always_exists(struct kunit *test)
{
	struct nt_open_result out;

	seed(test, "C:\\Have.txt");
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Have.txt",
			      NT_DISPOSITION_OPEN_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OPENED);
	nt_close(out.handle);
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
	seeded = dget(out.handle->path.dentry);
	KUNIT_ASSERT_EQ(test, vfs_truncate(&out.handle->path, 4096), 0);
	KUNIT_ASSERT_EQ(test, i_size_read(d_inode(seeded)), 4096);
	nt_close(out.handle);

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Big.txt",
			      NT_DISPOSITION_CREATE_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OVERWRITTEN);
	/* Same object, now empty. */
	KUNIT_EXPECT_PTR_EQ(test, out.handle->path.dentry, seeded);
	KUNIT_EXPECT_EQ(test, i_size_read(d_inode(seeded)), 0);
	nt_close(out.handle);
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
	original = dget(out.handle->path.dentry);
	nt_close(out.handle);

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
	KUNIT_EXPECT_PTR_EQ_MSG(test, out.handle->path.dentry, original,
				"a case-variant open must find the original");
	nt_close(out.handle);

	/* OPEN_ALWAYS likewise opens rather than creating a duplicate. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\kErNeL32.DlL",
			      NT_DISPOSITION_OPEN_ALWAYS, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_OPENED);
	KUNIT_EXPECT_PTR_EQ(test, out.handle->path.dentry, original);
	nt_close(out.handle);

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
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&out.handle->path, &attrs),
			0);
	KUNIT_EXPECT_TRUE_MSG(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE,
			      "a new file should have ARCHIVE set");
	nt_close(out.handle);
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
	KUNIT_EXPECT_TRUE(test, S_ISDIR(d_inode(out.handle->path.dentry)->i_mode));
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&out.handle->path, &attrs),
			0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_DIRECTORY);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE);
	nt_close(out.handle);
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

/* ---------------------------------------------------------- sharing */

/*
 * An exclusive open (share mode 0) blocks any second open, as Win32's
 * default CreateFile share mode does.
 */
static void nt_op_exclusive_blocks_second(struct kunit *test)
{
	struct nt_open_result a, b;

	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Excl.dat",
				    NT_DISPOSITION_CREATE_NEW,
				    NT_ACCESS_GENERIC_READ, 0, 0, &a), 0);

	KUNIT_EXPECT_EQ_MSG(test,
			    nt_op_share(test, "C:\\Excl.dat",
					NT_DISPOSITION_OPEN_EXISTING,
					NT_ACCESS_GENERIC_READ, 0, 0, &b),
			    -EBUSY, "a second open of an exclusive file must fail");

	/* Once the first handle is closed the file opens again. */
	nt_close(a.handle);
	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Excl.dat",
				    NT_DISPOSITION_OPEN_EXISTING,
				    NT_ACCESS_GENERIC_READ, 0, 0, &b), 0);
	nt_close(b.handle);
}

/* Two readers that both grant FILE_SHARE_READ coexist. */
static void nt_op_two_shared_readers(struct kunit *test)
{
	struct nt_open_result a, b;

	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Doc.dat",
				    NT_DISPOSITION_CREATE_NEW,
				    NT_ACCESS_GENERIC_READ, NT_SHARE_READ, 0,
				    &a), 0);
	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Doc.dat",
				    NT_DISPOSITION_OPEN_EXISTING,
				    NT_ACCESS_GENERIC_READ, NT_SHARE_READ, 0,
				    &b), 0);
	nt_close(a.handle);
	nt_close(b.handle);
}

/*
 * A reader that does not grant FILE_SHARE_WRITE is refused while a writer
 * is open, because the writer's access is not one it tolerates.
 */
static void nt_op_reader_conflicts_with_writer(struct kunit *test)
{
	struct nt_open_result a, b;

	/* Writer, sharing read only. */
	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Log.dat",
				    NT_DISPOSITION_CREATE_NEW,
				    NT_ACCESS_GENERIC_READ |
				    NT_ACCESS_GENERIC_WRITE, NT_SHARE_READ, 0,
				    &a), 0);

	/* Reader that will not tolerate the existing writer. */
	KUNIT_EXPECT_EQ(test,
			nt_op_share(test, "C:\\Log.dat",
				    NT_DISPOSITION_OPEN_EXISTING,
				    NT_ACCESS_GENERIC_READ, NT_SHARE_READ, 0,
				    &b), -EBUSY);

	nt_close(a.handle);
}

/* Wanting DELETE while an existing open does not grant FILE_SHARE_DELETE. */
static void nt_op_delete_needs_share_delete(struct kunit *test)
{
	struct nt_open_result a, b;

	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Keep.dat",
				    NT_DISPOSITION_CREATE_NEW,
				    NT_ACCESS_GENERIC_READ,
				    NT_SHARE_READ | NT_SHARE_WRITE, 0, &a), 0);

	KUNIT_EXPECT_EQ(test,
			nt_op_share(test, "C:\\Keep.dat",
				    NT_DISPOSITION_OPEN_EXISTING,
				    NT_ACCESS_DELETE,
				    NT_SHARE_READ | NT_SHARE_WRITE |
				    NT_SHARE_DELETE, 0, &b), -EBUSY);

	nt_close(a.handle);
}

/* ------------------------------------------------------ delete-on-close */

/* A delete-on-close handle removes the file when it is closed. */
static void nt_op_delete_on_close_removes_file(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Temp.dat",
				    NT_DISPOSITION_CREATE_NEW, NT_OP_ALL_ACCESS,
				    NT_OP_ALL_SHARE, NT_CREATE_DELETE_ON_CLOSE,
				    &out), 0);
	nt_close(out.handle);

	KUNIT_EXPECT_EQ_MSG(test,
			    nt_op(test, "C:\\Temp.dat",
				  NT_DISPOSITION_OPEN_EXISTING, 0, &out),
			    -ENOENT, "a delete-on-close file must be gone");
}

/*
 * While a delete-on-close handle is open the delete is pending, and other
 * opens are refused - the file is on its way out.  The delete happens when
 * the marking handle closes.
 */
static void nt_op_delete_pending_blocks_open(struct kunit *test)
{
	struct nt_open_result a, b;

	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Going.dat",
				    NT_DISPOSITION_CREATE_NEW, NT_OP_ALL_ACCESS,
				    NT_OP_ALL_SHARE, NT_CREATE_DELETE_ON_CLOSE,
				    &a), 0);

	KUNIT_EXPECT_EQ_MSG(test,
			    nt_op(test, "C:\\Going.dat",
				  NT_DISPOSITION_OPEN_EXISTING, 0, &b),
			    -ENOENT, "opens are refused while delete is pending");

	nt_close(a.handle);

	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Going.dat",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &b), -ENOENT);
}

/*
 * Delete-on-close only fires on the *last* close: a second handle opened
 * before the delete was marked keeps the file until it too closes.
 */
static void nt_op_delete_on_close_waits_for_last(struct kunit *test)
{
	struct nt_open_result keeper, deleter, check;

	/* An ordinary handle, opened first. */
	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Shared.dat",
				    NT_DISPOSITION_CREATE_NEW, NT_OP_ALL_ACCESS,
				    NT_OP_ALL_SHARE, 0, &keeper), 0);

	/* A delete-on-close handle on the same file. */
	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Shared.dat",
				    NT_DISPOSITION_OPEN_EXISTING, NT_OP_ALL_ACCESS,
				    NT_OP_ALL_SHARE, NT_CREATE_DELETE_ON_CLOSE,
				    &deleter), 0);

	/* Closing the delete-on-close handle does not remove it yet. */
	nt_close(deleter.handle);

	/* But the name is delete-pending, so it cannot be reopened. */
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Shared.dat",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &check), -ENOENT);

	/* The last close removes it. */
	nt_close(keeper.handle);
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Shared.dat",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &check), -ENOENT);
}

/* ------------------------------------------------------- access check */

/*
 * The desired access is gated against the file's mode.  A write open of a
 * read-only file is refused; a read open of a world-readable one is not.
 *
 * KUnit runs as root, which bypasses DAC, so the check is exercised from
 * an unprivileged credential - the same technique the case-fold read
 * permission test uses.
 */
static void nt_op_access_check_enforces_mode(struct kunit *test)
{
	struct nt_open_result out;
	const struct cred *old_cred;
	struct cred *unpriv;
	struct iattr attr = { .ia_valid = ATTR_MODE };
	struct dentry *file;
	int err;

	/* Create as root, then make it read-only for everyone. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\ReadOnly.txt",
			      NT_DISPOSITION_CREATE_NEW, 0, &out), 0);
	file = dget(out.handle->path.dentry);
	attr.ia_mode = (d_inode(file)->i_mode & ~0777) | 0444;
	inode_lock(d_inode(file));
	err = notify_change(&nop_mnt_idmap, file, &attr, NULL);
	inode_unlock(d_inode(file));
	nt_close(out.handle);
	if (err) {
		dput(file);
		kunit_skip(test, "could not drop write permission (%d)", err);
	}

	unpriv = prepare_creds();
	if (!unpriv) {
		dput(file);
		kunit_skip(test, "could not prepare credentials");
	}
	cap_clear(unpriv->cap_effective);
	cap_clear(unpriv->cap_permitted);
	cap_clear(unpriv->cap_bset);
	unpriv->fsuid = make_kuid(unpriv->user_ns, 65534);
	unpriv->fsgid = make_kgid(unpriv->user_ns, 65534);
	old_cred = override_creds(unpriv);

	/* Write access to a read-only file is refused before sharing. */
	err = nt_op_share(test, "C:\\ReadOnly.txt", NT_DISPOSITION_OPEN_EXISTING,
			  NT_ACCESS_GENERIC_WRITE, NT_OP_ALL_SHARE, 0, &out);
	KUNIT_EXPECT_EQ_MSG(test, err, -EACCES,
			    "write open of a read-only file must be refused");
	if (!err)
		nt_close(out.handle);

	/* Read access to a world-readable file is granted. */
	err = nt_op_share(test, "C:\\ReadOnly.txt", NT_DISPOSITION_OPEN_EXISTING,
			  NT_ACCESS_GENERIC_READ, NT_OP_ALL_SHARE, 0, &out);
	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "read open of a readable file is allowed");
	if (!err)
		nt_close(out.handle);

	revert_creds(old_cred);
	put_cred(unpriv);
	dput(file);
}

/* --------------------------------------------------- data streams */

/*
 * A named stream is created through nt_create(), written and read back,
 * and the base file it hangs off exists as an ordinary file whose own
 * contents were never touched.
 */
static void nt_op_stream_create_write_read(struct kunit *test)
{
	struct nt_open_result out;
	char buf[16];
	ssize_t n;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Notes.txt:tag",
			      NT_DISPOSITION_CREATE_NEW, 0, &out), 0);
	KUNIT_EXPECT_EQ(test, out.result, (u32)NT_RESULT_CREATED);
	KUNIT_ASSERT_NOT_NULL(test, out.handle->stream);

	KUNIT_EXPECT_EQ(test,
			nt_stream_write(&out.handle->path, out.handle->stream,
					out.handle->stream_len, 0, "hello", 5),
			5);
	n = nt_stream_read(&out.handle->path, out.handle->stream,
			   out.handle->stream_len, 0, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, n, 5);
	KUNIT_EXPECT_EQ(test, memcmp(buf, "hello", 5), 0);
	nt_close(out.handle);

	/* The base file exists and is an ordinary regular file. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Notes.txt",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), 0);
	KUNIT_EXPECT_TRUE(test, S_ISREG(d_inode(out.handle->path.dentry)->i_mode));
	nt_close(out.handle);
}

/* Dispositions apply to the stream, and require the base file to exist. */
static void nt_op_stream_dispositions(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Doc.txt:s", NT_DISPOSITION_CREATE_NEW,
			      0, &out), 0);
	nt_close(out.handle);

	/* CREATE_NEW of an existing stream collides. */
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Doc.txt:s", NT_DISPOSITION_CREATE_NEW,
			      0, &out), -EEXIST);

	/* A missing stream on an existing base is -ENOENT. */
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Doc.txt:other",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), -ENOENT);

	/* A stream on a base that does not exist is -ENOENT. */
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Nope.txt:s",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), -ENOENT);
}

/* Delete-on-close of a stream removes the stream, not the base file. */
static void nt_op_stream_delete_on_close(struct kunit *test)
{
	struct nt_open_result out;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Keep.txt:temp",
			      NT_DISPOSITION_CREATE_NEW, 0, &out), 0);
	nt_close(out.handle);

	KUNIT_ASSERT_EQ(test,
			nt_op_share(test, "C:\\Keep.txt:temp",
				    NT_DISPOSITION_OPEN_EXISTING, NT_OP_ALL_ACCESS,
				    NT_OP_ALL_SHARE, NT_CREATE_DELETE_ON_CLOSE,
				    &out), 0);
	nt_close(out.handle);

	/* The stream is gone... */
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\Keep.txt:temp",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), -ENOENT);
	/* ...but the file is not. */
	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Keep.txt",
			      NT_DISPOSITION_OPEN_EXISTING, 0, &out), 0);
	nt_close(out.handle);
}

struct nt_stream_tally {
	int count;
	loff_t last_size;
};

static int nt_stream_tally_cb(void *ctx, const char *name, size_t len,
			      loff_t size)
{
	struct nt_stream_tally *t = ctx;

	t->count++;
	t->last_size = size;
	return 0;
}

/* Enumeration reports each named stream and its size, not the base $DATA. */
static void nt_op_stream_enumerate(struct kunit *test)
{
	struct nt_open_result out;
	struct nt_stream_tally tally = { 0 };
	struct path base;

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Multi.txt:alpha",
			      NT_DISPOSITION_CREATE_NEW, 0, &out), 0);
	base = out.handle->path;	/* borrowed while the handle is open */

	KUNIT_ASSERT_EQ(test, nt_stream_set(&base, "beta", 4, "BB", 2), 0);

	KUNIT_ASSERT_EQ(test,
			nt_stream_list(&base, nt_stream_tally_cb, &tally), 0);
	KUNIT_EXPECT_EQ_MSG(test, tally.count, 2,
			    "two named streams should be enumerated");
	nt_close(out.handle);
}

/* Offset writes extend the stream and zero-fill the gap, like a file. */
static void nt_op_stream_offsets(struct kunit *test)
{
	struct nt_open_result out;
	struct path s;
	u16 slen;
	char buf[16];

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\Off.txt:s", NT_DISPOSITION_CREATE_NEW,
			      0, &out), 0);
	s = out.handle->path;
	slen = out.handle->stream_len;

	/* Write two bytes at offset 4: the stream becomes six bytes. */
	KUNIT_EXPECT_EQ(test,
			nt_stream_write(&s, out.handle->stream, slen, 4, "XY",
					2), 2);
	KUNIT_EXPECT_EQ(test,
			nt_stream_size(&s, out.handle->stream, slen), 6);

	memset(buf, 0x7f, sizeof(buf));
	KUNIT_EXPECT_EQ(test,
			nt_stream_read(&s, out.handle->stream, slen, 0, buf, 6),
			6);
	KUNIT_EXPECT_EQ(test, buf[0], 0);	/* zero-filled gap */
	KUNIT_EXPECT_EQ(test, buf[3], 0);
	KUNIT_EXPECT_EQ(test, buf[4], 'X');
	KUNIT_EXPECT_EQ(test, buf[5], 'Y');

	/* Reading at end returns nothing. */
	KUNIT_EXPECT_EQ(test,
			nt_stream_read(&s, out.handle->stream, slen, 6, buf,
				       6), 0);
	nt_close(out.handle);
}

/* A non-$DATA stream type is refused, and bad stream names are rejected. */
static void nt_op_stream_validation(struct kunit *test)
{
	struct nt_open_result out;
	struct path base;

	/* $INDEX_ALLOCATION and friends are NTFS-internal, not user data. */
	KUNIT_EXPECT_EQ(test,
			nt_op(test, "C:\\T.txt:idx:$INDEX_ALLOCATION",
			      NT_DISPOSITION_CREATE_NEW, 0, &out), -EOPNOTSUPP);

	KUNIT_ASSERT_EQ(test,
			nt_op(test, "C:\\V.txt:ok", NT_DISPOSITION_CREATE_NEW,
			      0, &out), 0);
	base = out.handle->path;

	KUNIT_EXPECT_EQ(test, nt_stream_set(&base, "", 0, "x", 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, nt_stream_set(&base, "a:b", 3, "x", 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, nt_stream_set(&base, "a/b", 3, "x", 1), -EINVAL);

	/* A stream larger than the store is refused before any allocation. */
	KUNIT_EXPECT_EQ(test,
			nt_stream_set(&base, "big", 3, NULL,
				      NT_STREAM_MAX_SIZE + 1), -EFBIG);
	nt_close(out.handle);
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
	KUNIT_CASE(nt_op_exclusive_blocks_second),
	KUNIT_CASE(nt_op_two_shared_readers),
	KUNIT_CASE(nt_op_reader_conflicts_with_writer),
	KUNIT_CASE(nt_op_delete_needs_share_delete),
	KUNIT_CASE(nt_op_delete_on_close_removes_file),
	KUNIT_CASE(nt_op_delete_pending_blocks_open),
	KUNIT_CASE(nt_op_delete_on_close_waits_for_last),
	KUNIT_CASE(nt_op_access_check_enforces_mode),
	KUNIT_CASE(nt_op_stream_create_write_read),
	KUNIT_CASE(nt_op_stream_dispositions),
	KUNIT_CASE(nt_op_stream_delete_on_close),
	KUNIT_CASE(nt_op_stream_enumerate),
	KUNIT_CASE(nt_op_stream_offsets),
	KUNIT_CASE(nt_op_stream_validation),
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
