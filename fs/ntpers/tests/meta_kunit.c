// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for NT file metadata.
 *
 * Run against a real private tmpfs mount, because almost every assertion
 * here is about how a Linux inode is translated, and a mock inode would
 * only test the mock.
 */

#include <linux/xattr.h>
#include <linux/nt_personality.h>

#include "ntpers_kunit.h"

static int nt_meta_test_init(struct kunit *test)
{
	struct nt_test_fs *fs;

	fs = kunit_kzalloc(test, sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;
	test->priv = fs;

	return nt_test_fs_init(test, fs);
}

static void nt_meta_test_exit(struct kunit *test)
{
	if (test->priv)
		nt_test_fs_exit(test->priv);
}

#define CTX(test) ((struct nt_test_fs *)(test)->priv)

/* Create in the fixture root; the fixture owns the reference. */
static struct dentry *nt_meta_create(struct kunit *test, const char *name,
				     bool dir)
{
	struct nt_test_fs *fs = CTX(test);

	return nt_test_create(test, fs, fs->root.dentry, name, dir);
}

static void nt_meta_path(struct kunit *test, struct dentry *dentry,
			 struct path *out)
{
	nt_test_path(CTX(test), dentry, out);
}

/* ---------------------------------------------------------- attributes */

/*
 * A plain file has no stored metadata, so everything comes from the
 * inode.  ARCHIVE is the default Windows gives a newly created file.
 */
static void test_attrs_defaults(struct kunit *test)
{
	struct dentry *file, *dir;
	struct path path;
	u32 attrs = 0;

	file = nt_meta_create(test, "Plain.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_DIRECTORY);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_HIDDEN);
	/* Mode 0644 is writable, so not read-only. */
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_READONLY);

	dir = nt_meta_create(test, "Folder", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));
	nt_meta_path(test, dir, &path);

	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_DIRECTORY);
	/* A directory is not an archive candidate. */
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE);
}

/* A dotfile is hidden, the Unix way, unless something says otherwise. */
static void test_attrs_dotfile_is_hidden(struct kunit *test)
{
	struct dentry *file;
	struct path path;
	u32 attrs = 0;

	file = nt_meta_create(test, ".hidden", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_HIDDEN);

	/* Clearing it must stick, rather than the dot winning again. */
	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
						     NT_FILE_ATTRIBUTE_ARCHIVE),
			0);
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_HIDDEN);
}

static void test_attrs_set_and_get(struct kunit *test)
{
	struct dentry *file;
	struct path path;
	u32 attrs = 0;

	file = nt_meta_create(test, "Attrs.dat", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
			NT_FILE_ATTRIBUTE_HIDDEN | NT_FILE_ATTRIBUTE_SYSTEM |
			NT_FILE_ATTRIBUTE_ARCHIVE |
			NT_FILE_ATTRIBUTE_TEMPORARY |
			NT_FILE_ATTRIBUTE_OFFLINE |
			NT_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED), 0);

	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_HIDDEN);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_SYSTEM);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_ARCHIVE);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_TEMPORARY);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_OFFLINE);
	KUNIT_EXPECT_TRUE(test,
			  attrs & NT_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);

	/* Clearing everything returns the file to its derived defaults. */
	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path, 0), 0);
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_HIDDEN);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_SYSTEM);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_TEMPORARY);
}

/*
 * READONLY is backed by the file mode rather than stored, so that the
 * Linux and NT answers to "can this be written?" cannot diverge.
 */
static void test_attrs_readonly_follows_mode(struct kunit *test)
{
	struct dentry *file;
	struct path path;
	u32 attrs = 0;

	file = nt_meta_create(test, "ReadOnly.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
						NT_FILE_ATTRIBUTE_READONLY), 0);

	/* The mode really changed, not just a stored bit. */
	KUNIT_EXPECT_EQ(test, d_inode(file)->i_mode & S_IWUGO, 0);

	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_READONLY);

	/* And clearing it gives write permission back - to the owner. */
	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
						NT_FILE_ATTRIBUTE_ARCHIVE), 0);
	KUNIT_EXPECT_TRUE(test, d_inode(file)->i_mode & S_IWUSR);
	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_READONLY);

	/*
	 * Only to the owner.  A DOS attribute says nothing about who
	 * should be able to write, so clearing it must not widen access
	 * beyond the least privileged reading of the request.  Reaching
	 * 0666 here would be a silent permission escalation on every
	 * Windows program that clears a read-only flag.
	 */
	KUNIT_EXPECT_EQ(test, d_inode(file)->i_mode & (S_IWGRP | S_IWOTH),
			0);
}

/*
 * Round-tripping the read-only attribute must not accumulate
 * permissions: a file that starts at 0644 must be 0644 again afterwards.
 */
static void test_attrs_readonly_roundtrip_preserves_mode(struct kunit *test)
{
	struct dentry *file;
	struct path path;
	umode_t before;

	file = nt_meta_create(test, "RoundTrip.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	before = d_inode(file)->i_mode;
	KUNIT_ASSERT_EQ(test, before & 0777, 0644);

	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
						NT_FILE_ATTRIBUTE_READONLY), 0);
	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
						NT_FILE_ATTRIBUTE_ARCHIVE), 0);

	KUNIT_EXPECT_EQ(test, d_inode(file)->i_mode, before);
}

/*
 * The filesystem owns DIRECTORY and the rest of the derived bits.  A
 * caller cannot set them, and trying must not corrupt what is stored.
 */
static void test_attrs_fs_owned_bits_ignored(struct kunit *test)
{
	struct dentry *file;
	struct path path;
	u32 attrs = 0;

	file = nt_meta_create(test, "NotADir.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, nt_set_file_attributes(&path,
			NT_FILE_ATTRIBUTE_DIRECTORY |
			NT_FILE_ATTRIBUTE_REPARSE_POINT |
			NT_FILE_ATTRIBUTE_SYSTEM), 0);

	KUNIT_ASSERT_EQ(test, nt_get_file_attributes(&path, &attrs), 0);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_DIRECTORY);
	KUNIT_EXPECT_FALSE(test, attrs & NT_FILE_ATTRIBUTE_REPARSE_POINT);
	/* The legitimate bit in the same call still took effect. */
	KUNIT_EXPECT_TRUE(test, attrs & NT_FILE_ATTRIBUTE_SYSTEM);
}

static void test_attrs_reject_invalid(struct kunit *test)
{
	struct dentry *file;
	struct path path;

	file = nt_meta_create(test, "Invalid.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_EXPECT_EQ(test, nt_set_file_attributes(&path, 0x80000000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, nt_get_file_attributes(&path, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, nt_set_file_attributes(NULL, 0), -EINVAL);
}

/* ----------------------------------------------------------- timestamps */

/*
 * The three timestamps that genuinely correspond must map to the right
 * NT concept.  ChangeTime is ctime; CreationTime is not.
 */
static void test_times_map_correctly(struct kunit *test)
{
	struct nt_file_info info;
	struct dentry *file;
	struct kstat stat;
	struct path path;

	file = nt_meta_create(test, "Times.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, vfs_getattr(&path, &stat, STATX_BASIC_STATS,
					  AT_STATX_SYNC_AS_STAT), 0);
	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, NULL, &info), 0);

	KUNIT_EXPECT_EQ(test, info.last_write.tv_sec, stat.mtime.tv_sec);
	KUNIT_EXPECT_EQ(test, info.last_access.tv_sec, stat.atime.tv_sec);
	KUNIT_EXPECT_EQ(test, info.change.tv_sec, stat.ctime.tv_sec);
}

/*
 * CreationTime must be reported as exact only when it really is.  Either
 * the filesystem has a birth time, or it does not and the estimate is
 * flagged - never both, and never silently.
 */
static void test_creation_time_is_honest(struct kunit *test)
{
	struct nt_file_info info;
	struct dentry *file;
	struct kstat stat;
	struct path path;
	bool fs_has_btime;

	file = nt_meta_create(test, "Created.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, vfs_getattr(&path, &stat,
					  STATX_BASIC_STATS | STATX_BTIME,
					  AT_STATX_SYNC_AS_STAT), 0);
	fs_has_btime = stat.result_mask & STATX_BTIME;

	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, NULL, &info), 0);

	/* Exactly one of the two flags, always. */
	KUNIT_EXPECT_NE(test, info.time_flags & (NT_TIME_CREATION_EXACT |
						 NT_TIME_CREATION_ESTIMATED),
			0);
	KUNIT_EXPECT_NE(test,
			!!(info.time_flags & NT_TIME_CREATION_EXACT),
			!!(info.time_flags & NT_TIME_CREATION_ESTIMATED));

	if (fs_has_btime) {
		KUNIT_EXPECT_TRUE(test,
				  info.time_flags & NT_TIME_CREATION_EXACT);
		KUNIT_EXPECT_EQ(test, info.creation.tv_sec, stat.btime.tv_sec);
	} else {
		KUNIT_EXPECT_TRUE(test,
			info.time_flags & NT_TIME_CREATION_ESTIMATED);
		/*
		 * The estimate must not be the ctime masquerading as a
		 * creation time - it is the oldest timestamp available, and
		 * it is labelled.
		 */
		KUNIT_EXPECT_LE(test, info.creation.tv_sec, stat.mtime.tv_sec);
		KUNIT_EXPECT_LE(test, info.creation.tv_sec, stat.ctime.tv_sec);
	}
}

/* A stored creation time is used when the filesystem has none. */
static void test_creation_time_stored(struct kunit *test)
{
	struct timespec64 want = { .tv_sec = 1000000000, .tv_nsec = 0 };
	struct nt_file_info info;
	struct dentry *file;
	struct kstat stat;
	struct path path;
	int err;

	file = nt_meta_create(test, "Stored.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	err = nt_set_creation_time(&path, &want);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "backing filesystem has no xattr support");
	KUNIT_ASSERT_EQ(test, err, 0);

	KUNIT_ASSERT_EQ(test, vfs_getattr(&path, &stat, STATX_BTIME,
					  AT_STATX_SYNC_AS_STAT), 0);
	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, NULL, &info), 0);

	KUNIT_EXPECT_TRUE(test, info.time_flags & NT_TIME_CREATION_EXACT);

	if (stat.result_mask & STATX_BTIME) {
		/*
		 * A real birth time outranks a stored one; that is the
		 * better answer and must not be overridden.
		 */
		KUNIT_EXPECT_EQ(test, info.creation.tv_sec, stat.btime.tv_sec);
	} else {
		KUNIT_EXPECT_EQ(test, info.creation.tv_sec, want.tv_sec);
	}
}

/* NT time is 100ns units since 1601; check the conversion round-trips. */
static void test_nt_time_conversion(struct kunit *test)
{
	struct timespec64 ts, back;
	u64 nt;

	/* The Unix epoch is a known constant in NT time. */
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	KUNIT_EXPECT_EQ(test, nt_time_from_timespec(&ts), 116444736000000000ULL);

	nt_time_to_timespec(116444736000000000ULL, &back);
	KUNIT_EXPECT_EQ(test, back.tv_sec, 0);
	KUNIT_EXPECT_EQ(test, back.tv_nsec, 0);

	/* An arbitrary time, with sub-second precision. */
	ts.tv_sec = 1600000000;
	ts.tv_nsec = 123456700;
	nt = nt_time_from_timespec(&ts);
	nt_time_to_timespec(nt, &back);
	KUNIT_EXPECT_EQ(test, back.tv_sec, ts.tv_sec);
	KUNIT_EXPECT_EQ(test, back.tv_nsec, ts.tv_nsec);

	/* Before the Unix epoch but after 1601: must not go negative wrong. */
	ts.tv_sec = -1000000;
	ts.tv_nsec = 0;
	nt = nt_time_from_timespec(&ts);
	nt_time_to_timespec(nt, &back);
	KUNIT_EXPECT_EQ(test, back.tv_sec, ts.tv_sec);
}

/* ------------------------------------------------------------ file ids */

static void test_file_ids(struct kunit *test)
{
	struct nt_file_info a, b;
	struct dentry *f1, *f2;
	struct path p1, p2;

	f1 = nt_meta_create(test, "Id1.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f1));
	f2 = nt_meta_create(test, "Id2.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f2));

	nt_meta_path(test, f1, &p1);
	nt_meta_path(test, f2, &p2);

	KUNIT_ASSERT_EQ(test, nt_query_file_info(&p1, NULL, &a), 0);
	KUNIT_ASSERT_EQ(test, nt_query_file_info(&p2, NULL, &b), 0);

	/* Different files, different ids. */
	KUNIT_EXPECT_NE(test, a.file_id, b.file_id);
	KUNIT_EXPECT_NE(test, memcmp(a.file_id_128, b.file_id_128, 16), 0);

	/* An id must never be zero; software uses zero as "unknown". */
	KUNIT_EXPECT_NE(test, a.file_id, 0);

	/* And it must be stable across queries. */
	KUNIT_ASSERT_EQ(test, nt_query_file_info(&p1, NULL, &b), 0);
	KUNIT_EXPECT_EQ(test, a.file_id, b.file_id);
	KUNIT_EXPECT_EQ(test, memcmp(a.file_id_128, b.file_id_128, 16), 0);
}

/*
 * The 128-bit id carries the volume serial, so the same inode number on
 * two different volumes does not collide.
 */
static void test_file_id_includes_volume(struct kunit *test)
{
	struct nt_test_fs *fs = CTX(test);
	struct nt_file_info with_vol, without;
	struct nt_namespace *ns;
	struct nt_volume *vol;
	struct dentry *file;
	struct path path;

	file = nt_meta_create(test, "VolId.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	ns = nt_ns_create();
	KUNIT_ASSERT_NOT_NULL(test, ns);
	vol = nt_volume_create(ns, &fs->root, 'C', 0, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(vol));

	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, vol, &with_vol), 0);
	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, NULL, &without), 0);

	KUNIT_EXPECT_EQ(test, with_vol.volume_serial, vol->serial);
	KUNIT_EXPECT_EQ(test, without.volume_serial, 0);

	/* Same inode, different volume identity, different 128-bit id. */
	KUNIT_EXPECT_EQ(test, with_vol.file_id, without.file_id);
	KUNIT_EXPECT_NE(test, memcmp(with_vol.file_id_128,
				     without.file_id_128, 16), 0);

	nt_volume_destroy(ns, vol);
	nt_volume_put(vol);
	nt_ns_put(ns);
}

/* ------------------------------------------------------- full query */

static void test_query_file_info(struct kunit *test)
{
	struct nt_file_info info;
	struct dentry *file, *dir;
	struct path path;

	file = nt_meta_create(test, "Query.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, NULL, &info), 0);
	KUNIT_EXPECT_EQ(test, info.nlink, 1);
	KUNIT_EXPECT_EQ(test, info.size, 0);
	KUNIT_EXPECT_EQ(test, info.reparse_tag, 0);
	KUNIT_EXPECT_FALSE(test,
			   info.attributes & NT_FILE_ATTRIBUTE_DIRECTORY);

	dir = nt_meta_create(test, "QueryDir", true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dir));
	nt_meta_path(test, dir, &path);

	KUNIT_ASSERT_EQ(test, nt_query_file_info(&path, NULL, &info), 0);
	KUNIT_EXPECT_TRUE(test,
			  info.attributes & NT_FILE_ATTRIBUTE_DIRECTORY);

	KUNIT_EXPECT_EQ(test, nt_query_file_info(NULL, NULL, &info), -EINVAL);
	KUNIT_EXPECT_EQ(test, nt_query_file_info(&path, NULL, NULL), -EINVAL);
}

/* -------------------------------------------------- security descriptor */

/* A minimal but structurally valid self-relative security descriptor. */
static void nt_build_sd(u8 *buf, size_t size)
{
	memset(buf, 0, size);
	buf[0] = 1;			/* revision */
	buf[1] = 0;			/* sbz1 */
	buf[2] = 0x00;			/* control, low byte */
	buf[3] = 0x80;			/* SE_SELF_RELATIVE */
	/* owner/group/sacl/dacl offsets all zero: absent, which is legal. */
}

static void test_security_descriptor_roundtrip(struct kunit *test)
{
	u8 sd[64], back[64];
	struct dentry *file;
	struct path path;
	ssize_t got;
	int err;

	file = nt_meta_create(test, "Secured.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	/* A file created through POSIX has no NT descriptor, and says so. */
	got = nt_get_security_descriptor(&path, back, sizeof(back));
	KUNIT_EXPECT_TRUE(test, got == -ENODATA || got == -EOPNOTSUPP);

	nt_build_sd(sd, sizeof(sd));

	err = nt_set_security_descriptor(&path, sd, sizeof(sd));
	if (err == -EOPNOTSUPP || err == -EPERM || err == -EACCES)
		kunit_skip(test,
			   "backing filesystem rejects security.* xattrs (%d)",
			   err);
	KUNIT_ASSERT_EQ(test, err, 0);

	got = nt_get_security_descriptor(&path, back, sizeof(back));
	KUNIT_ASSERT_EQ(test, got, sizeof(sd));
	KUNIT_EXPECT_EQ(test, memcmp(sd, back, sizeof(sd)), 0);

	/* Querying the size without a buffer must work too. */
	got = nt_get_security_descriptor(&path, NULL, 0);
	KUNIT_EXPECT_EQ(test, got, sizeof(sd));
}

/*
 * Storing a malformed descriptor must be refused, so that anything
 * reading one back can trust the header.
 */
static void test_security_descriptor_validation(struct kunit *test)
{
	struct dentry *file;
	struct path path;
	u8 sd[64];

	file = nt_meta_create(test, "BadSd.txt", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	nt_meta_path(test, file, &path);

	/* Too short to be a descriptor at all. */
	nt_build_sd(sd, sizeof(sd));
	KUNIT_EXPECT_EQ(test, nt_set_security_descriptor(&path, sd, 4),
			-EINVAL);

	/* Wrong revision. */
	nt_build_sd(sd, sizeof(sd));
	sd[0] = 2;
	KUNIT_EXPECT_EQ(test,
			nt_set_security_descriptor(&path, sd, sizeof(sd)),
			-EINVAL);

	/* Not self-relative, so the offsets would not mean what we assume. */
	nt_build_sd(sd, sizeof(sd));
	sd[3] = 0x00;
	KUNIT_EXPECT_EQ(test,
			nt_set_security_descriptor(&path, sd, sizeof(sd)),
			-EINVAL);

	/* A DACL offset pointing outside the buffer. */
	nt_build_sd(sd, sizeof(sd));
	sd[16] = 0xff;
	sd[17] = 0xff;
	KUNIT_EXPECT_EQ(test,
			nt_set_security_descriptor(&path, sd, sizeof(sd)),
			-EINVAL);

	/* An offset inside the header itself is equally impossible. */
	nt_build_sd(sd, sizeof(sd));
	sd[16] = 0x04;
	KUNIT_EXPECT_EQ(test,
			nt_set_security_descriptor(&path, sd, sizeof(sd)),
			-EINVAL);

	KUNIT_EXPECT_EQ(test, nt_set_security_descriptor(&path, NULL, 64),
			-EINVAL);
}

static struct kunit_case nt_meta_test_cases[] = {
	KUNIT_CASE(test_attrs_defaults),
	KUNIT_CASE(test_attrs_dotfile_is_hidden),
	KUNIT_CASE(test_attrs_set_and_get),
	KUNIT_CASE(test_attrs_readonly_follows_mode),
	KUNIT_CASE(test_attrs_readonly_roundtrip_preserves_mode),
	KUNIT_CASE(test_attrs_fs_owned_bits_ignored),
	KUNIT_CASE(test_attrs_reject_invalid),
	KUNIT_CASE(test_times_map_correctly),
	KUNIT_CASE(test_creation_time_is_honest),
	KUNIT_CASE(test_creation_time_stored),
	KUNIT_CASE(test_nt_time_conversion),
	KUNIT_CASE(test_file_ids),
	KUNIT_CASE(test_file_id_includes_volume),
	KUNIT_CASE(test_query_file_info),
	KUNIT_CASE(test_security_descriptor_roundtrip),
	KUNIT_CASE(test_security_descriptor_validation),
	{}
};

static struct kunit_suite nt_meta_test_suite = {
	.name = "ntpers-meta",
	.init = nt_meta_test_init,
	.exit = nt_meta_test_exit,
	.test_cases = nt_meta_test_cases,
};
kunit_test_suite(nt_meta_test_suite);

MODULE_DESCRIPTION("KUnit tests for NT file metadata");
MODULE_LICENSE("GPL");
