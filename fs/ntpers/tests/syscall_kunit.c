// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NT file system calls.
 *
 * These drive the *core* calls - nt_file_create() and its kin - end to end
 * against a private tmpfs mount, exactly as the syscall wrappers will once a
 * dispatch stage exists, but with kernel buffers and no user memory.  The
 * headline case walks a file through its whole life: create it, write bytes,
 * read them back at an offset, confirm the queried size matches, close it,
 * confirm the handle then names nothing, and re-open it to read the bytes
 * again.  The rest pin down the NTSTATUS error mappings - a collision on
 * FILE_CREATE, a missing name on FILE_OPEN, end of file on a read past the
 * end - and the position, directory and query behaviours.
 *
 * The wrappers themselves (NtCreateFile() and friends) copy to and from user
 * memory, which a KUnit case has none of, so they are exercised through their
 * cores here and left to a later dispatch stage to test against a real task.
 */

#include <linux/fs.h>
#include <linux/string.h>
#include <linux/nt_personality.h>

#include "ntpers_kunit.h"

struct nt_sc_ctx {
	struct nt_test_fs	fs;
	struct nt_namespace	*ns;
	struct nt_task_ctx	*tc;
	struct nt_volume	*vol;
};

static int nt_sc_test_init(struct kunit *test)
{
	struct nt_sc_ctx *ctx;
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

static void nt_sc_test_exit(struct kunit *test)
{
	struct nt_sc_ctx *ctx = test->priv;

	if (!ctx)
		return;

	/*
	 * nt_ctx_put() destroys the context's handle table, which closes every
	 * handle a test left open - releasing the struct file and the mount and
	 * dentry references each pins.  The clean unmount in nt_test_fs_exit()
	 * that follows is the proof nothing leaked.
	 */
	nt_ctx_put(ctx->tc);
	nt_ns_put(ctx->ns);
	nt_test_fs_exit(&ctx->fs);
}

#define CTX(test) ((struct nt_sc_ctx *)(test)->priv)
#define TC(test)  (CTX(test)->tc)

/* Full access, full sharing: these tests are not about the access gate. */
#define NT_SC_ACCESS	(NT_ACCESS_GENERIC_READ | NT_ACCESS_GENERIC_WRITE | \
			 NT_ACCESS_DELETE)
#define NT_SC_SHARE	(NT_SHARE_READ | NT_SHARE_WRITE | NT_SHARE_DELETE)

/* Drive nt_file_create() with the given internal disposition and options. */
static u32 nt_sc_open(struct kunit *test, const char *path, u32 disposition,
		      u32 options, bool forbid_dir, u32 *handle, u64 *info)
{
	struct nt_create_req req = {
		.disposition	= disposition,
		.access		= NT_SC_ACCESS,
		.share		= NT_SC_SHARE,
		.options	= options,
		.attributes	= 0,
		.resolve_flags	= NT_RESOLVE_CASE_INSENSITIVE,
	};

	return nt_file_create(TC(test), path, &req, forbid_dir, handle, info);
}

/*
 * The full life of a file: FILE_CREATE, write, read at an offset, query the
 * size, close, confirm the handle is dead, then FILE_OPEN and read again.
 */
static void nt_sc_file_roundtrip(struct kunit *test)
{
	struct nt_file_standard_information si;
	struct nt_open *found = NULL;
	u32 handle, reopened, n, out_len;
	char buf[16];
	u64 info;

	/* FILE_CREATE a fresh file. */
	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Data.bin",
				   NT_DISPOSITION_CREATE_NEW, 0, false, &handle,
				   &info),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, info, (u64)NT_FILE_CREATED);

	/* Write eleven bytes at offset zero. */
	KUNIT_ASSERT_EQ(test,
			nt_file_write(TC(test), handle, 0, "hello world", 11, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, n, (u32)11);

	/* Read five bytes back at offset six: "world". */
	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_EQ(test,
			nt_file_read(TC(test), handle, 6, buf, 5, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, n, (u32)5);
	KUNIT_EXPECT_EQ(test, memcmp(buf, "world", 5), 0);

	/* The queried size matches what was written. */
	KUNIT_ASSERT_EQ(test,
			nt_file_query_information(TC(test), handle,
						  NT_FILEINFO_STANDARD, &si,
						  sizeof(si), &out_len),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, out_len, (u32)sizeof(si));
	KUNIT_EXPECT_EQ(test, si.end_of_file, (s64)11);
	KUNIT_EXPECT_EQ(test, si.number_of_links, (u32)1);
	KUNIT_EXPECT_FALSE(test, si.directory);

	/* Close, and the handle no longer resolves; a second close is invalid. */
	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test,
			nt_handle_lookup(&TC(test)->handles, handle, &found),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_INVALID_HANDLE);

	/* FILE_OPEN the same name and read the bytes again. */
	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Data.bin",
				   NT_DISPOSITION_OPEN_EXISTING, 0, false,
				   &reopened, &info),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, info, (u64)NT_FILE_OPENED);
	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_EQ(test,
			nt_file_read(TC(test), reopened, 0, buf, 11, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, n, (u32)11);
	KUNIT_EXPECT_EQ(test, memcmp(buf, "hello world", 11), 0);
	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), reopened),
			(u32)STATUS_SUCCESS);
}

/*
 * The "use current position" sentinel walks the handle's own position forward
 * across successive reads, and stops at end of file.
 */
static void nt_sc_current_position_reads(struct kunit *test)
{
	const s64 here = (s64)NT_FILE_USE_FILE_POINTER_POSITION;
	u32 handle, n;
	char buf[8];
	u64 info;

	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Seq.bin", NT_DISPOSITION_CREATE_NEW,
				   0, false, &handle, &info),
			(u32)STATUS_SUCCESS);
	KUNIT_ASSERT_EQ(test,
			nt_file_write(TC(test), handle, 0, "ABCDEFGH", 8, &n),
			(u32)STATUS_SUCCESS);

	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_EQ(test, nt_file_read(TC(test), handle, here, buf, 4, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, n, (u32)4);
	KUNIT_EXPECT_EQ(test, memcmp(buf, "ABCD", 4), 0);

	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_EQ(test, nt_file_read(TC(test), handle, here, buf, 4, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, n, (u32)4);
	KUNIT_EXPECT_EQ(test, memcmp(buf, "EFGH", 4), 0);

	/* At end of file the next current-position read reports EOF. */
	KUNIT_EXPECT_EQ(test, nt_file_read(TC(test), handle, here, buf, 4, &n),
			(u32)STATUS_END_OF_FILE);
	KUNIT_EXPECT_EQ(test, n, (u32)0);

	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);
}

/* Appending with the FILE_WRITE_TO_END_OF_FILE sentinel grows the file. */
static void nt_sc_append(struct kunit *test)
{
	const s64 append = (s64)NT_FILE_WRITE_TO_END_OF_FILE;
	struct nt_file_standard_information si;
	u32 handle, n, out_len;
	char buf[8];
	u64 info;

	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\App.bin", NT_DISPOSITION_CREATE_NEW,
				   0, false, &handle, &info),
			(u32)STATUS_SUCCESS);

	KUNIT_ASSERT_EQ(test,
			nt_file_write(TC(test), handle, append, "ab", 2, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_ASSERT_EQ(test,
			nt_file_write(TC(test), handle, append, "cd", 2, &n),
			(u32)STATUS_SUCCESS);

	KUNIT_ASSERT_EQ(test,
			nt_file_query_information(TC(test), handle,
						  NT_FILEINFO_STANDARD, &si,
						  sizeof(si), &out_len),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, si.end_of_file, (s64)4);

	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_EQ(test, nt_file_read(TC(test), handle, 0, buf, 4, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, memcmp(buf, "abcd", 4), 0);

	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);
}

/* A read that starts at or past end of file is STATUS_END_OF_FILE, zero bytes. */
static void nt_sc_read_past_end(struct kunit *test)
{
	u32 handle, n;
	char buf[8];
	u64 info;

	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Short.bin",
				   NT_DISPOSITION_CREATE_NEW, 0, false, &handle,
				   &info),
			(u32)STATUS_SUCCESS);
	KUNIT_ASSERT_EQ(test, nt_file_write(TC(test), handle, 0, "hi", 2, &n),
			(u32)STATUS_SUCCESS);

	/* Exactly at the end. */
	KUNIT_EXPECT_EQ(test, nt_file_read(TC(test), handle, 2, buf, 8, &n),
			(u32)STATUS_END_OF_FILE);
	KUNIT_EXPECT_EQ(test, n, (u32)0);

	/* Well past the end. */
	KUNIT_EXPECT_EQ(test, nt_file_read(TC(test), handle, 100, buf, 8, &n),
			(u32)STATUS_END_OF_FILE);

	/* A read that starts inside the file still succeeds. */
	KUNIT_EXPECT_EQ(test, nt_file_read(TC(test), handle, 0, buf, 8, &n),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, n, (u32)2);

	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);
}

/* FILE_CREATE of a name that already exists collides. */
static void nt_sc_create_collision(struct kunit *test)
{
	u32 handle, second;
	u64 info;

	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Once.bin",
				   NT_DISPOSITION_CREATE_NEW, 0, false, &handle,
				   &info),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);

	KUNIT_EXPECT_EQ(test,
			nt_sc_open(test, "C:\\Once.bin",
				   NT_DISPOSITION_CREATE_NEW, 0, false, &second,
				   &info),
			(u32)STATUS_OBJECT_NAME_COLLISION);
}

/* FILE_OPEN of a name that does not exist reports it not found. */
static void nt_sc_open_missing(struct kunit *test)
{
	u32 handle;
	u64 info;

	KUNIT_EXPECT_EQ(test,
			nt_sc_open(test, "C:\\Ghost.bin",
				   NT_DISPOSITION_OPEN_EXISTING, 0, false,
				   &handle, &info),
			(u32)STATUS_OBJECT_NAME_NOT_FOUND);
}

/* FileBasicInformation carries the attributes; a short buffer is a mismatch. */
static void nt_sc_query_basic_and_lengths(struct kunit *test)
{
	struct nt_file_basic_information bi;
	u32 handle, out_len;
	u64 info;

	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Meta.bin",
				   NT_DISPOSITION_CREATE_NEW, 0, false, &handle,
				   &info),
			(u32)STATUS_SUCCESS);

	KUNIT_ASSERT_EQ(test,
			nt_file_query_information(TC(test), handle,
						  NT_FILEINFO_BASIC, &bi,
						  sizeof(bi), &out_len),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, out_len, (u32)sizeof(bi));
	KUNIT_EXPECT_TRUE(test, bi.file_attributes & NT_FILE_ATTRIBUTE_ARCHIVE);

	/* A buffer smaller than the structure is STATUS_INFO_LENGTH_MISMATCH. */
	KUNIT_EXPECT_EQ(test,
			nt_file_query_information(TC(test), handle,
						  NT_FILEINFO_BASIC, &bi, 8,
						  &out_len),
			(u32)STATUS_INFO_LENGTH_MISMATCH);

	/* An unhandled information class is STATUS_NOT_IMPLEMENTED. */
	KUNIT_EXPECT_EQ(test,
			nt_file_query_information(TC(test), handle, 6, &bi,
						  sizeof(bi), &out_len),
			(u32)STATUS_NOT_IMPLEMENTED);

	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);
}

/*
 * A directory handle reports Directory in its standard information, refuses
 * data reads, and cannot be opened with FILE_NON_DIRECTORY_FILE.
 */
static void nt_sc_directory(struct kunit *test)
{
	struct nt_file_standard_information si;
	u32 handle, other, out_len, n;
	char buf[4];
	u64 info;

	KUNIT_ASSERT_EQ(test,
			nt_sc_open(test, "C:\\Folder", NT_DISPOSITION_CREATE_NEW,
				   NT_CREATE_DIRECTORY, false, &handle, &info),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, info, (u64)NT_FILE_CREATED);

	KUNIT_ASSERT_EQ(test,
			nt_file_query_information(TC(test), handle,
						  NT_FILEINFO_STANDARD, &si,
						  sizeof(si), &out_len),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_TRUE(test, si.directory);

	/* Reading a directory's bytes is refused. */
	KUNIT_EXPECT_EQ(test, nt_file_read(TC(test), handle, 0, buf, 4, &n),
			(u32)STATUS_INVALID_DEVICE_REQUEST);

	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), handle),
			(u32)STATUS_SUCCESS);

	/* FILE_NON_DIRECTORY_FILE on the same name is refused. */
	KUNIT_EXPECT_EQ(test,
			nt_sc_open(test, "C:\\Folder",
				   NT_DISPOSITION_OPEN_EXISTING, 0, true, &other,
				   &info),
			(u32)STATUS_FILE_IS_A_DIRECTORY);
}

/* A malformed handle value names nothing, for every call that takes one. */
static void nt_sc_bad_handle(struct kunit *test)
{
	struct nt_file_standard_information si;
	u32 out_len, n;
	char buf[4];

	KUNIT_EXPECT_EQ(test, nt_file_read(TC(test), 4000, 0, buf, 4, &n),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_EQ(test, nt_file_write(TC(test), 4000, 0, "x", 1, &n),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_EQ(test,
			nt_file_query_information(TC(test), 4000,
						  NT_FILEINFO_STANDARD, &si,
						  sizeof(si), &out_len),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_EQ(test, nt_file_close(TC(test), 4000),
			(u32)STATUS_INVALID_HANDLE);
}

static struct kunit_case nt_sc_test_cases[] = {
	KUNIT_CASE(nt_sc_file_roundtrip),
	KUNIT_CASE(nt_sc_current_position_reads),
	KUNIT_CASE(nt_sc_append),
	KUNIT_CASE(nt_sc_read_past_end),
	KUNIT_CASE(nt_sc_create_collision),
	KUNIT_CASE(nt_sc_open_missing),
	KUNIT_CASE(nt_sc_query_basic_and_lengths),
	KUNIT_CASE(nt_sc_directory),
	KUNIT_CASE(nt_sc_bad_handle),
	{}
};

static struct kunit_suite nt_sc_test_suite = {
	.name = "ntpers-syscall",
	.init = nt_sc_test_init,
	.exit = nt_sc_test_exit,
	.test_cases = nt_sc_test_cases,
};
kunit_test_suite(nt_sc_test_suite);

MODULE_DESCRIPTION("KUnit tests for the NT file system calls");
MODULE_LICENSE("GPL");
