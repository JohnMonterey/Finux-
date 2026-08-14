// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NT handle table.
 *
 * The handles here name real file objects: each nt_open is created through
 * nt_create() against a private tmpfs mount, exactly as a syscall would,
 * so the table is exercised with the objects it will really hold rather
 * than with stand-ins.  The suite proves the Windows-visible properties -
 * handle values are 4, 8, 12, ...; a closed handle stops resolving and
 * cannot be closed twice; a garbage handle never resolves - and that
 * table destruction closes every outstanding handle, which the fixture's
 * clean unmount then confirms leaked nothing.
 */

#include <linux/nt_personality.h>

#include "ntpers_kunit.h"

struct nt_handle_ctx {
	struct nt_test_fs	fs;
	struct nt_namespace	*ns;
	struct nt_task_ctx	*tc;
	struct nt_volume	*vol;
	struct nt_handle_table	ht;
	bool			ht_ready;
};

/* Full access and full sharing: these tests are about handles, not sharing. */
static const struct nt_create_req add_req = {
	.disposition	= NT_DISPOSITION_OPEN_ALWAYS,
	.access		= NT_ACCESS_GENERIC_READ | NT_ACCESS_GENERIC_WRITE |
			  NT_ACCESS_DELETE,
	.share		= NT_SHARE_READ | NT_SHARE_WRITE | NT_SHARE_DELETE,
	.options	= 0,
	.attributes	= 0,
	.resolve_flags	= NT_RESOLVE_CASE_INSENSITIVE,
};

static int nt_handle_test_init(struct kunit *test)
{
	struct nt_handle_ctx *ctx;
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

	nt_handle_table_init(&ctx->ht);
	ctx->ht_ready = true;

	return 0;
}

static void nt_handle_test_exit(struct kunit *test)
{
	struct nt_handle_ctx *ctx = test->priv;

	if (!ctx)
		return;

	/*
	 * Close every handle still open before the mount is torn down.  A test
	 * that leaves handles open (destroy is one of the things under test)
	 * relies on this to release them; a leak would surface as a "Busy
	 * inodes after unmount" splat from nt_test_fs_exit().
	 */
	if (ctx->ht_ready)
		nt_handle_table_destroy(&ctx->ht);

	nt_ctx_put(ctx->tc);
	nt_ns_put(ctx->ns);
	nt_test_fs_exit(&ctx->fs);
}

#define CTX(test) ((struct nt_handle_ctx *)(test)->priv)

/*
 * Create a file object through nt_create() and install it in the table.
 * On success the table owns the object; @open_out, when given, receives the
 * borrowed pointer so a test can check that a later lookup returns it.
 * Returns the handle value.
 */
static u32 add_handle(struct kunit *test, const char *path,
		      struct nt_open **open_out)
{
	struct nt_handle_ctx *ctx = CTX(test);
	struct nt_open_result res;
	u32 handle = NT_NULL_HANDLE;
	u32 st;

	KUNIT_ASSERT_EQ(test, nt_create(ctx->tc, path, &add_req, &res), 0);

	st = nt_handle_alloc(&ctx->ht, res.handle, &handle);
	if (st != STATUS_SUCCESS) {
		/* Do not leak the object if the table refused it. */
		nt_close(res.handle);
		KUNIT_ASSERT_EQ_MSG(test, st, (u32)STATUS_SUCCESS,
				    "handle allocation failed");
	}

	if (open_out)
		*open_out = res.handle;
	return handle;
}

/* Handle values are multiples of four starting at four, in order. */
static void nt_handle_values_start_at_four(struct kunit *test)
{
	u32 h1, h2, h3;

	h1 = add_handle(test, "C:\\One.txt", NULL);
	h2 = add_handle(test, "C:\\Two.txt", NULL);
	h3 = add_handle(test, "C:\\Three.txt", NULL);

	KUNIT_EXPECT_EQ(test, h1, (u32)4);
	KUNIT_EXPECT_EQ(test, h2, (u32)8);
	KUNIT_EXPECT_EQ(test, h3, (u32)12);
}

/* A lookup returns the exact object the handle was allocated for. */
static void nt_handle_lookup_returns_object(struct kunit *test)
{
	struct nt_open *open, *found = NULL;
	u32 handle;

	handle = add_handle(test, "C:\\Look.txt", &open);

	KUNIT_EXPECT_EQ(test, nt_handle_lookup(&CTX(test)->ht, handle, &found),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_PTR_EQ(test, found, open);
}

/* Distinct objects get distinct handles, each resolving to its own object. */
static void nt_handle_two_objects_distinct(struct kunit *test)
{
	struct nt_open *oa, *ob, *fa = NULL, *fb = NULL;
	u32 ha, hb;

	ha = add_handle(test, "C:\\Alpha.txt", &oa);
	hb = add_handle(test, "C:\\Bravo.txt", &ob);

	KUNIT_EXPECT_NE(test, ha, hb);
	KUNIT_ASSERT_EQ(test, nt_handle_lookup(&CTX(test)->ht, ha, &fa),
			(u32)STATUS_SUCCESS);
	KUNIT_ASSERT_EQ(test, nt_handle_lookup(&CTX(test)->ht, hb, &fb),
			(u32)STATUS_SUCCESS);
	KUNIT_EXPECT_PTR_EQ(test, fa, oa);
	KUNIT_EXPECT_PTR_EQ(test, fb, ob);
}

/*
 * Closing a handle releases the object, invalidates the handle for lookup,
 * and a second close reports it invalid rather than closing twice.
 */
static void nt_handle_close_invalidates(struct kunit *test)
{
	struct nt_open *found = NULL;
	u32 handle;

	handle = add_handle(test, "C:\\Close.txt", NULL);

	KUNIT_EXPECT_EQ(test, nt_handle_close(&CTX(test)->ht, handle),
			(u32)STATUS_SUCCESS);

	KUNIT_EXPECT_EQ(test, nt_handle_lookup(&CTX(test)->ht, handle, &found),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_NULL(test, found);

	KUNIT_EXPECT_EQ(test, nt_handle_close(&CTX(test)->ht, handle),
			(u32)STATUS_INVALID_HANDLE);
}

/* The null handle, a misaligned value, and an unallocated value all fail. */
static void nt_handle_garbage_fails(struct kunit *test)
{
	struct nt_open *found = (void *)1;
	u32 handle;

	/* Keep one real handle in the table so it is not empty. */
	handle = add_handle(test, "C:\\Real.txt", NULL);
	KUNIT_EXPECT_EQ(test, handle, (u32)4);

	/* The null handle is never valid. */
	KUNIT_EXPECT_EQ(test,
			nt_handle_lookup(&CTX(test)->ht, NT_NULL_HANDLE, &found),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_NULL(test, found);

	/* A value with tag bits set is not one the table hands out. */
	KUNIT_EXPECT_EQ(test, nt_handle_lookup(&CTX(test)->ht, 6, &found),
			(u32)STATUS_INVALID_HANDLE);

	/* A well-formed value that was never allocated. */
	KUNIT_EXPECT_EQ(test, nt_handle_lookup(&CTX(test)->ht, 4000, &found),
			(u32)STATUS_INVALID_HANDLE);

	/* Closing garbage fails the same way. */
	KUNIT_EXPECT_EQ(test, nt_handle_close(&CTX(test)->ht, 4000),
			(u32)STATUS_INVALID_HANDLE);
	KUNIT_EXPECT_EQ(test, nt_handle_close(&CTX(test)->ht, 7),
			(u32)STATUS_INVALID_HANDLE);
}

/* Freeing a handle returns its slot to the allocator: values are reused. */
static void nt_handle_reuses_freed_slot(struct kunit *test)
{
	u32 h1, h2, h3;

	h1 = add_handle(test, "C:\\R1.txt", NULL);
	h2 = add_handle(test, "C:\\R2.txt", NULL);
	KUNIT_EXPECT_EQ(test, h1, (u32)4);
	KUNIT_EXPECT_EQ(test, h2, (u32)8);

	KUNIT_EXPECT_EQ(test, nt_handle_close(&CTX(test)->ht, h1),
			(u32)STATUS_SUCCESS);

	/* The next allocation takes the lowest free slot, which is now 4. */
	h3 = add_handle(test, "C:\\R3.txt", NULL);
	KUNIT_EXPECT_EQ(test, h3, (u32)4);
}

/*
 * Table destruction closes every outstanding handle.  These handles are
 * left open on purpose; the fixture's exit() destroys the table, and the
 * clean unmount that follows is the proof that nothing leaked.
 */
static void nt_handle_destroy_closes_outstanding(struct kunit *test)
{
	add_handle(test, "C:\\D1.txt", NULL);
	add_handle(test, "C:\\D2.txt", NULL);
	add_handle(test, "C:\\D3.txt", NULL);
	add_handle(test, "C:\\D4.txt", NULL);

	KUNIT_SUCCEED(test);
}

static struct kunit_case nt_handle_test_cases[] = {
	KUNIT_CASE(nt_handle_values_start_at_four),
	KUNIT_CASE(nt_handle_lookup_returns_object),
	KUNIT_CASE(nt_handle_two_objects_distinct),
	KUNIT_CASE(nt_handle_close_invalidates),
	KUNIT_CASE(nt_handle_garbage_fails),
	KUNIT_CASE(nt_handle_reuses_freed_slot),
	KUNIT_CASE(nt_handle_destroy_closes_outstanding),
	{}
};

static struct kunit_suite nt_handle_test_suite = {
	.name = "ntpers-handle",
	.init = nt_handle_test_init,
	.exit = nt_handle_test_exit,
	.test_cases = nt_handle_test_cases,
};
kunit_test_suite(nt_handle_test_suite);

MODULE_DESCRIPTION("KUnit tests for the NT handle table");
MODULE_LICENSE("GPL");
