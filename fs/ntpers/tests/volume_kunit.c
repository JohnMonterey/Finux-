// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NT volume and namespace layer.
 *
 * These need a real struct path to root a volume at, because a volume is
 * defined by its binding to a Linux mount and testing it against a fake
 * one would test nothing.  The root of the running system's filesystem
 * is used, which is always present and is never modified here: the tests
 * only ever register and unregister volumes in a private namespace.
 */

#include <kunit/test.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/nt_personality.h>

struct nt_vol_test_ctx {
	struct nt_namespace	*ns;
	struct path		root;
};

static int nt_vol_test_init(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx;
	int err;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/*
	 * A private namespace, so nothing here can disturb the machine's
	 * real drive letters.
	 */
	ctx->ns = nt_ns_create();
	if (!ctx->ns)
		return -ENOMEM;

	err = kern_path("/", LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &ctx->root);
	if (err) {
		nt_ns_put(ctx->ns);
		return err;
	}

	test->priv = ctx;
	return 0;
}

static void nt_vol_test_exit(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = test->priv;

	if (!ctx)
		return;

	path_put(&ctx->root);
	nt_ns_put(ctx->ns);
}

#define CTX(test) ((struct nt_vol_test_ctx *)(test)->priv)

static void test_create_and_lookup(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *vol, *found;

	vol = nt_volume_create(ctx->ns, &ctx->root, 'C', 0, "System");
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));
	KUNIT_EXPECT_EQ(test, vol->letter, 'C');
	KUNIT_EXPECT_STREQ(test, vol->label, "System");
	KUNIT_EXPECT_EQ(test, ctx->ns->nr_volumes, 1);

	found = nt_volume_lookup_letter(ctx->ns, 'C');
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	/* Drive letters are case-insensitive. */
	found = nt_volume_lookup_letter(ctx->ns, 'c');
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	KUNIT_EXPECT_NULL(test, nt_volume_lookup_letter(ctx->ns, 'D'));

	nt_volume_destroy(ctx->ns, vol);
	KUNIT_EXPECT_NULL(test, nt_volume_lookup_letter(ctx->ns, 'C'));
	KUNIT_EXPECT_EQ(test, ctx->ns->nr_volumes, 0);
	nt_volume_put(vol);
}

static void test_letter_conflict(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *a, *b;

	a = nt_volume_create(ctx->ns, &ctx->root, 'C', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(a));

	b = nt_volume_create(ctx->ns, &ctx->root, 'C', 0, NULL);
	KUNIT_EXPECT_TRUE(test, IS_ERR(b));
	KUNIT_EXPECT_EQ(test, PTR_ERR(b), -EBUSY);

	/* A different letter is fine. */
	b = nt_volume_create(ctx->ns, &ctx->root, 'D', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(b));

	nt_volume_destroy(ctx->ns, a);
	nt_volume_put(a);
	nt_volume_destroy(ctx->ns, b);
	nt_volume_put(b);
}

static void test_invalid_letter(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *vol;

	vol = nt_volume_create(ctx->ns, &ctx->root, '1', 0, NULL);
	KUNIT_EXPECT_TRUE(test, IS_ERR(vol));
	KUNIT_EXPECT_EQ(test, PTR_ERR(vol), -EINVAL);

	KUNIT_EXPECT_EQ(test, nt_drive_index('A'), 0);
	KUNIT_EXPECT_EQ(test, nt_drive_index('a'), 0);
	KUNIT_EXPECT_EQ(test, nt_drive_index('Z'), 25);
	KUNIT_EXPECT_EQ(test, nt_drive_index('z'), 25);
	KUNIT_EXPECT_EQ(test, nt_drive_index('0'), -1);
	KUNIT_EXPECT_EQ(test, nt_drive_index(0), -1);
}

static void test_reletter(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *vol, *other, *found;

	vol = nt_volume_create(ctx->ns, &ctx->root, 'D', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	KUNIT_EXPECT_EQ(test, nt_volume_assign_letter(ctx->ns, vol, 'E'), 0);
	KUNIT_EXPECT_EQ(test, vol->letter, 'E');
	KUNIT_EXPECT_NULL(test, nt_volume_lookup_letter(ctx->ns, 'D'));

	found = nt_volume_lookup_letter(ctx->ns, 'E');
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	/* Moving onto an occupied letter fails and changes nothing. */
	other = nt_volume_create(ctx->ns, &ctx->root, 'F', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(other));
	KUNIT_EXPECT_EQ(test, nt_volume_assign_letter(ctx->ns, vol, 'F'),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, vol->letter, 'E');

	/* Unassigning leaves the volume registered but unreachable by letter. */
	KUNIT_EXPECT_EQ(test, nt_volume_assign_letter(ctx->ns, vol, 0), 0);
	KUNIT_EXPECT_EQ(test, vol->letter, 0);
	KUNIT_EXPECT_NULL(test, nt_volume_lookup_letter(ctx->ns, 'E'));
	KUNIT_EXPECT_EQ(test, ctx->ns->nr_volumes, 2);

	nt_volume_destroy(ctx->ns, vol);
	nt_volume_put(vol);
	nt_volume_destroy(ctx->ns, other);
	nt_volume_put(other);
}

/* Designating the system volume also moves it to C:, as Windows does. */
static void test_system_volume(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *vol, *found;

	KUNIT_EXPECT_NULL(test, nt_volume_get_system(ctx->ns));

	vol = nt_volume_create(ctx->ns, &ctx->root, 'E', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	KUNIT_EXPECT_EQ(test, nt_volume_set_system(ctx->ns, vol), 0);
	KUNIT_EXPECT_EQ(test, vol->letter, 'C');
	KUNIT_EXPECT_TRUE(test, vol->flags & NT_VOL_SYSTEM);

	found = nt_volume_get_system(ctx->ns);
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	found = nt_volume_lookup_letter(ctx->ns, 'C');
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	nt_volume_destroy(ctx->ns, vol);
	KUNIT_EXPECT_NULL(test, nt_volume_get_system(ctx->ns));
	nt_volume_put(vol);
}

static void test_system_volume_letter_conflict(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *squatter, *vol;

	squatter = nt_volume_create(ctx->ns, &ctx->root, 'C', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(squatter));

	vol = nt_volume_create(ctx->ns, &ctx->root, 'D', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	KUNIT_EXPECT_EQ(test, nt_volume_set_system(ctx->ns, vol), -EBUSY);
	KUNIT_EXPECT_FALSE(test, vol->flags & NT_VOL_SYSTEM);
	KUNIT_EXPECT_EQ(test, vol->letter, 'D');

	nt_volume_destroy(ctx->ns, vol);
	nt_volume_put(vol);
	nt_volume_destroy(ctx->ns, squatter);
	nt_volume_put(squatter);
}

/* Volume identity metadata NT expects and Linux has nowhere to put. */
static void test_volume_identity(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *vol, *found;
	guid_t zero_guid;

	vol = nt_volume_create(ctx->ns, &ctx->root, 'C', NT_VOL_SYSTEM,
			       NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	/* The first volume registered becomes HarddiskVolume1, as on NT. */
	KUNIT_EXPECT_EQ(test, vol->id, 1);
	KUNIT_EXPECT_STREQ(test, vol->nt_device, "\\Device\\HarddiskVolume1");

	/* A serial of zero would make GetVolumeInformation look broken. */
	KUNIT_EXPECT_NE(test, vol->serial, 0);

	memset(&zero_guid, 0, sizeof(zero_guid));
	KUNIT_EXPECT_FALSE(test, guid_equal(&vol->guid, &zero_guid));

	KUNIT_EXPECT_STRNEQ(test, vol->fs_name, "");

	/* Volumes are reachable by NT device name, not only by letter. */
	found = nt_volume_lookup_device(ctx->ns, "\\Device\\HarddiskVolume1",
					23);
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	/* The Object Manager is case-insensitive too. */
	found = nt_volume_lookup_device(ctx->ns, "\\device\\harddiskvolume1",
					23);
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	/* And by id. */
	found = nt_volume_lookup_id(ctx->ns, vol->id);
	KUNIT_EXPECT_PTR_EQ(test, found, vol);
	nt_volume_put(found);

	KUNIT_EXPECT_NULL(test, nt_volume_lookup_id(ctx->ns, 12345));

	nt_volume_destroy(ctx->ns, vol);
	nt_volume_put(vol);
}

/* Letters are handed out from D:, because A:/B: are floppies and C: is the
 * system volume.
 */
static void test_first_free_letter(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_volume *d, *e;

	KUNIT_EXPECT_EQ(test, nt_volume_first_free_letter(ctx->ns), 'D');

	d = nt_volume_create(ctx->ns, &ctx->root, 'D', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(d));
	KUNIT_EXPECT_EQ(test, nt_volume_first_free_letter(ctx->ns), 'E');

	e = nt_volume_create(ctx->ns, &ctx->root, 'E', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(e));
	KUNIT_EXPECT_EQ(test, nt_volume_first_free_letter(ctx->ns), 'F');

	nt_volume_destroy(ctx->ns, d);
	KUNIT_EXPECT_EQ(test, nt_volume_first_free_letter(ctx->ns), 'D');

	nt_volume_put(d);
	nt_volume_destroy(ctx->ns, e);
	nt_volume_put(e);
}

/* A volume must be rooted at a directory. */
static void test_volume_needs_directory(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	static const char * const candidates[] = {
		"/proc/self/stat", "/dev/null", "/etc/hostname",
	};
	struct nt_volume *vol;
	struct path file;
	size_t i;
	int err = -ENOENT;

	/*
	 * Any non-directory will do.  Which ones exist depends on what the
	 * test kernel has mounted, so try a few before giving up.
	 */
	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		err = kern_path(candidates[i], LOOKUP_FOLLOW, &file);
		if (!err)
			break;
	}
	if (err)
		kunit_skip(test, "no non-directory available to test against");

	vol = nt_volume_create(ctx->ns, &file, 'D', 0, NULL);
	KUNIT_EXPECT_TRUE(test, IS_ERR(vol));
	KUNIT_EXPECT_EQ(test, PTR_ERR(vol), -ENOTDIR);

	path_put(&file);
}

/* --------------------------------------------------- per-task contexts */

static void test_task_context(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_task_ctx *tc;

	tc = nt_ctx_alloc(ctx->ns);
	KUNIT_ASSERT_NOT_NULL(test, tc);

	/* A fresh context is inert: allocating one changes no behaviour. */
	KUNIT_EXPECT_EQ(test, tc->flags, 0);
	KUNIT_EXPECT_EQ(test, tc->cur_drive, 'C');
	KUNIT_EXPECT_PTR_EQ(test, tc->ns, ctx->ns);

	KUNIT_EXPECT_EQ(test, nt_ctx_set_current_drive(tc, 'd'), 0);
	KUNIT_EXPECT_EQ(test, tc->cur_drive, 'D');
	KUNIT_EXPECT_EQ(test, nt_ctx_set_current_drive(tc, '?'), -EINVAL);
	KUNIT_EXPECT_EQ(test, tc->cur_drive, 'D');

	nt_ctx_put(tc);
}

static void test_drive_cwd(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_task_ctx *tc;
	struct path got;

	tc = nt_ctx_alloc(ctx->ns);
	KUNIT_ASSERT_NOT_NULL(test, tc);

	/* Nothing remembered yet: not an error, just absent. */
	KUNIT_EXPECT_FALSE(test, nt_ctx_get_drive_cwd(tc, 'C', &got));

	KUNIT_EXPECT_EQ(test, nt_ctx_set_drive_cwd(tc, 'C', &ctx->root), 0);
	KUNIT_ASSERT_TRUE(test, nt_ctx_get_drive_cwd(tc, 'C', &got));
	KUNIT_EXPECT_PTR_EQ(test, got.dentry, ctx->root.dentry);
	path_put(&got);

	/* Case-insensitive, like every other use of a drive letter. */
	KUNIT_ASSERT_TRUE(test, nt_ctx_get_drive_cwd(tc, 'c', &got));
	path_put(&got);

	/* Other drives are unaffected. */
	KUNIT_EXPECT_FALSE(test, nt_ctx_get_drive_cwd(tc, 'D', &got));

	KUNIT_EXPECT_EQ(test, nt_ctx_set_drive_cwd(tc, '?', &ctx->root),
			-EINVAL);

	nt_ctx_put(tc);
}

/* ------------------------------------------------------------ resolving */

static void test_resolve_drive_absolute(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_path_parse parse;
	struct nt_task_ctx *tc;
	struct nt_volume *vol;
	struct nt_path out;
	char *buf;
	int err;

	buf = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	vol = nt_volume_create(ctx->ns, &ctx->root, 'C', NT_VOL_SYSTEM, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	tc = nt_ctx_alloc(ctx->ns);
	KUNIT_ASSERT_NOT_NULL(test, tc);

	/* "C:\" is the root of the volume, resolved through the volume. */
	err = nt_path_parse("C:\\", 3, 0, buf, NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	err = nt_path_resolve(tc, &parse, NT_RESOLVE_DIRECTORY, &out);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_PTR_EQ(test, out.path.dentry, ctx->root.dentry);
	KUNIT_EXPECT_PTR_EQ(test, out.volume, vol);
	nt_path_put(&out);

	/* A drive with no volume behind it is -ENODEV, not -ENOENT. */
	err = nt_path_parse("X:\\", 3, 0, buf, NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, nt_path_resolve(tc, &parse, 0, &out), -ENODEV);

	/* UNC is understood syntactically but not served by this layer. */
	err = nt_path_parse("\\\\srv\\share\\x", 13, 0, buf,
			    NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, nt_path_resolve(tc, &parse, 0, &out),
			-EOPNOTSUPP);

	nt_ctx_put(tc);
	nt_volume_destroy(ctx->ns, vol);
	nt_volume_put(vol);
}

/*
 * "\Windows" is rooted on the caller's current drive, so changing the
 * current drive changes what it means.  This is the behaviour that makes
 * a drive letter a real piece of process state rather than a prefix.
 */
static void test_resolve_follows_current_drive(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_path_parse parse;
	struct nt_task_ctx *tc;
	struct nt_volume *c;
	struct nt_path out;
	char *buf;
	int err;

	buf = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	c = nt_volume_create(ctx->ns, &ctx->root, 'C', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(c));

	tc = nt_ctx_alloc(ctx->ns);
	KUNIT_ASSERT_NOT_NULL(test, tc);

	err = nt_path_parse("\\", 1, 0, buf, NT_PATH_BUF_SIZE, &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, parse.type, NT_PATH_ROOTED);

	/* Current drive is C:, which exists. */
	err = nt_path_resolve(tc, &parse, NT_RESOLVE_DIRECTORY, &out);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_PTR_EQ(test, out.volume, c);
	nt_path_put(&out);

	/* Move to a drive with nothing on it and the same path fails. */
	KUNIT_EXPECT_EQ(test, nt_ctx_set_current_drive(tc, 'X'), 0);
	KUNIT_EXPECT_EQ(test, nt_path_resolve(tc, &parse, 0, &out), -ENODEV);

	nt_ctx_put(tc);
	nt_volume_destroy(ctx->ns, c);
	nt_volume_put(c);
}

/*
 * Resolving a stream without saying you understand streams must fail
 * rather than quietly hand back the default stream, which would give the
 * caller the wrong bytes.
 */
static void test_resolve_rejects_unhandled_stream(struct kunit *test)
{
	struct nt_vol_test_ctx *ctx = CTX(test);
	struct nt_path_parse parse;
	struct nt_volume *vol;
	struct nt_path out;
	char *buf;
	int err;

	buf = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	vol = nt_volume_create(ctx->ns, &ctx->root, 'C', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	err = nt_path_parse("C:\\x.txt:stream", 15, 0, buf, NT_PATH_BUF_SIZE,
			    &parse);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_TRUE(test, parse.flags & NT_PARSE_HAS_STREAM);

	KUNIT_EXPECT_EQ(test, nt_path_resolve(NULL, &parse, 0, &out),
			-EOPNOTSUPP);

	nt_volume_destroy(ctx->ns, vol);
	nt_volume_put(vol);
}

static struct kunit_case nt_volume_test_cases[] = {
	KUNIT_CASE(test_create_and_lookup),
	KUNIT_CASE(test_letter_conflict),
	KUNIT_CASE(test_invalid_letter),
	KUNIT_CASE(test_reletter),
	KUNIT_CASE(test_system_volume),
	KUNIT_CASE(test_system_volume_letter_conflict),
	KUNIT_CASE(test_volume_identity),
	KUNIT_CASE(test_first_free_letter),
	KUNIT_CASE(test_volume_needs_directory),
	KUNIT_CASE(test_task_context),
	KUNIT_CASE(test_drive_cwd),
	KUNIT_CASE(test_resolve_drive_absolute),
	KUNIT_CASE(test_resolve_follows_current_drive),
	KUNIT_CASE(test_resolve_rejects_unhandled_stream),
	{}
};

static struct kunit_suite nt_volume_test_suite = {
	.name = "ntpers-volume",
	.init = nt_vol_test_init,
	.exit = nt_vol_test_exit,
	.test_cases = nt_volume_test_cases,
};
kunit_test_suite(nt_volume_test_suite);

MODULE_DESCRIPTION("KUnit tests for the NT volume and namespace layer");
MODULE_LICENSE("GPL");
