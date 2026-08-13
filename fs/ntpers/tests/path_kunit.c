// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NT/Win32 pathname parser and the Win32 reserved
 * device name rules.
 *
 * The parser is pure - it takes bytes and a buffer and touches nothing
 * else - so it can be tested exhaustively without a filesystem, which is
 * exactly why it was written that way.
 */

#include <kunit/test.h>
#include <linux/slab.h>
#include <linux/nt_personality.h>

struct nt_path_test_ctx {
	char			*buf;
	struct nt_path_parse	parse;
};

static int nt_path_test_init(struct kunit *test)
{
	struct nt_path_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->buf = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	if (!ctx->buf)
		return -ENOMEM;

	test->priv = ctx;
	return 0;
}

static int nt_parse(struct kunit *test, const char *path, u32 flags)
{
	struct nt_path_test_ctx *ctx = test->priv;

	return nt_path_parse(path, strlen(path), flags, ctx->buf,
			     NT_PATH_BUF_SIZE, &ctx->parse);
}

#define P(test) (&((struct nt_path_test_ctx *)(test)->priv)->parse)

/*
 * Assert the full shape of a parse in one place, so a test reads as a
 * statement about behaviour rather than a pile of individual checks.
 */
static void expect_parse(struct kunit *test, const char *path,
			 enum nt_path_type type, char drive, const char *rel)
{
	int err = nt_parse(test, path, 0);

	KUNIT_ASSERT_EQ_MSG(test, err, 0, "parsing \"%s\"", path);
	KUNIT_EXPECT_EQ_MSG(test, P(test)->type, type,
			    "type of \"%s\"", path);
	KUNIT_EXPECT_EQ_MSG(test, P(test)->drive, (u8)drive,
			    "drive of \"%s\"", path);
	KUNIT_EXPECT_STREQ_MSG(test, P(test)->rel, rel,
			       "canonical form of \"%s\"", path);
}

static void expect_error(struct kunit *test, const char *path, int expected)
{
	int err = nt_parse(test, path, 0);

	KUNIT_EXPECT_EQ_MSG(test, err, expected, "parsing \"%s\"", path);
	KUNIT_EXPECT_EQ_MSG(test, P(test)->type, NT_PATH_INVALID,
			    "type of \"%s\" after failure", path);
}

/* ------------------------------------------------------------- basics */

static void test_drive_absolute(struct kunit *test)
{
	expect_parse(test, "C:\\Windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	expect_parse(test, "C:\\Windows\\System32", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "Windows/System32");
	expect_parse(test, "C:\\", NT_PATH_DRIVE_ABSOLUTE, 'C', "");
	expect_parse(test, "D:\\Program Files (x86)\\App",
		     NT_PATH_DRIVE_ABSOLUTE, 'D',
		     "Program Files (x86)/App");
}

/* Drive letters are case-insensitive; the rest of the path is preserved. */
static void test_drive_letter_case(struct kunit *test)
{
	expect_parse(test, "c:\\Windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	expect_parse(test, "C:\\windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "windows");
	expect_parse(test, "c:\\WINDOWS", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "WINDOWS");
	expect_parse(test, "z:\\x", NT_PATH_DRIVE_ABSOLUTE, 'Z', "x");
}

static void test_drive_relative(struct kunit *test)
{
	expect_parse(test, "C:Windows", NT_PATH_DRIVE_RELATIVE, 'C',
		     "Windows");
	expect_parse(test, "C:foo\\bar", NT_PATH_DRIVE_RELATIVE, 'C',
		     "foo/bar");

	/* Bare "C:" is the current directory on C:, not the root of C:. */
	expect_parse(test, "C:", NT_PATH_DRIVE_RELATIVE, 'C', "");
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_VOLUME_ROOT);
}

static void test_rooted_and_relative(struct kunit *test)
{
	expect_parse(test, "\\Windows", NT_PATH_ROOTED, 0, "Windows");
	expect_parse(test, "\\", NT_PATH_ROOTED, 0, "");
	expect_parse(test, "Windows", NT_PATH_RELATIVE, 0, "Windows");
	expect_parse(test, "a\\b\\c", NT_PATH_RELATIVE, 0, "a/b/c");
}

/* Win32 accepts '/' as a separator everywhere '\' is accepted. */
static void test_forward_slash_separator(struct kunit *test)
{
	expect_parse(test, "C:/Windows/System32", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "Windows/System32");
	expect_parse(test, "C:\\Windows/System32", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "Windows/System32");
}

/* --------------------------------------------------------- normalising */

static void test_separator_collapse(struct kunit *test)
{
	expect_parse(test, "C:\\\\Windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	expect_parse(test, "C:\\Windows\\\\\\System32",
		     NT_PATH_DRIVE_ABSOLUTE, 'C', "Windows/System32");
}

static void test_trailing_separator(struct kunit *test)
{
	expect_parse(test, "C:\\Windows\\", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_TRAILING_SEP);

	expect_parse(test, "C:\\Windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	KUNIT_EXPECT_FALSE(test, P(test)->flags & NT_PARSE_TRAILING_SEP);
}

static void test_dot_components(struct kunit *test)
{
	expect_parse(test, "C:\\Windows\\.\\System32",
		     NT_PATH_DRIVE_ABSOLUTE, 'C', "Windows/System32");
	expect_parse(test, "C:\\Windows\\..\\Users", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "Users");
	expect_parse(test, "C:\\a\\b\\..\\..\\c", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "c");

	/* Popping past the root is not an error in Win32. */
	expect_parse(test, "C:\\..\\..\\Windows", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "Windows");
	expect_parse(test, "C:\\..", NT_PATH_DRIVE_ABSOLUTE, 'C', "");

	/* Only "." and ".." are relative names; longer runs are invalid. */
	expect_error(test, "C:\\...", -EINVAL);
	expect_error(test, "C:\\a\\....\\b", -EINVAL);
}

/*
 * Win32 trims trailing dots and spaces off each component before the
 * path ever reaches the filesystem.
 */
static void test_trailing_dot_and_space_trim(struct kunit *test)
{
	expect_parse(test, "C:\\Windows.", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_TRIMMED);

	expect_parse(test, "C:\\Windows   ", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	expect_parse(test, "C:\\a. \\b", NT_PATH_DRIVE_ABSOLUTE, 'C', "a/b");

	/* A component of nothing but spaces normalises away to nothing. */
	expect_error(test, "C:\\   \\b", -EINVAL);
}

static void test_component_count(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, nt_parse(test, "C:\\a\\b\\c", 0), 0);
	KUNIT_EXPECT_EQ(test, P(test)->nr_components, 3);

	KUNIT_ASSERT_EQ(test, nt_parse(test, "C:\\a\\b\\..\\c", 0), 0);
	KUNIT_EXPECT_EQ(test, P(test)->nr_components, 2);

	KUNIT_ASSERT_EQ(test, nt_parse(test, "C:\\", 0), 0);
	KUNIT_EXPECT_EQ(test, P(test)->nr_components, 0);
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_VOLUME_ROOT);
}

/* ----------------------------------------------------------- verbatim */

static void test_extended_prefix(struct kunit *test)
{
	expect_parse(test, "\\\\?\\C:\\Windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_VERBATIM);

	/*
	 * The whole point of \\?\ is that Win32 stops normalising, so "."
	 * and ".." survive as literal names and trailing dots are kept.
	 */
	expect_parse(test, "\\\\?\\C:\\a\\..\\b", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "a/../b");
	expect_parse(test, "\\\\?\\C:\\Windows.", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows.");
	KUNIT_EXPECT_FALSE(test, P(test)->flags & NT_PARSE_TRIMMED);
}

static void test_extended_unc(struct kunit *test)
{
	int err = nt_parse(test, "\\\\?\\UNC\\server\\share\\dir", 0);

	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, P(test)->type, NT_PATH_UNC);
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_VERBATIM);
	KUNIT_EXPECT_EQ(test, P(test)->unc_server_len, 6);
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->unc_server, "server", 6), 0);
	KUNIT_EXPECT_EQ(test, P(test)->unc_share_len, 5);
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->unc_share, "share", 5), 0);
	KUNIT_EXPECT_STREQ(test, P(test)->rel, "dir");
}

static void test_unc(struct kunit *test)
{
	int err = nt_parse(test, "\\\\server\\share\\a\\b", 0);

	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, P(test)->type, NT_PATH_UNC);
	KUNIT_EXPECT_FALSE(test, P(test)->flags & NT_PARSE_VERBATIM);
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->unc_server, "server", 6), 0);
	KUNIT_EXPECT_STREQ(test, P(test)->rel, "a/b");

	/* A UNC path with no server name is not a path. */
	expect_error(test, "\\\\", -EINVAL);
}

static void test_win32_device_namespace(struct kunit *test)
{
	int err = nt_parse(test, "\\\\.\\PhysicalDrive0", 0);

	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, P(test)->type, NT_PATH_DEVICE);
	KUNIT_EXPECT_EQ(test, P(test)->device_len, 14);
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->device, "PhysicalDrive0", 14),
			0);

	/*
	 * "\\.\C:" names the volume device itself, while "\\?\C:\" names
	 * its root directory.  Conflating the two is a classic bug.
	 */
	err = nt_parse(test, "\\\\.\\C:", 0);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, P(test)->type, NT_PATH_DEVICE);
	KUNIT_EXPECT_EQ(test, P(test)->drive, 'C');

	expect_parse(test, "\\\\?\\C:\\", NT_PATH_DRIVE_ABSOLUTE, 'C', "");
}

/* ---------------------------------------------------- NT object manager */

static void test_nt_object_dos_devices(struct kunit *test)
{
	/*
	 * "\??\" is the Object Manager's per-session DOS device directory.
	 * This is what Win32 turns "\\?\" into, and it resolves the same
	 * way once the drive letter is looked up.
	 */
	expect_parse(test, "\\??\\C:\\Windows", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Windows");
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_VERBATIM);
}

static void test_nt_object_device_path(struct kunit *test)
{
	int err = nt_parse(test, "\\Device\\HarddiskVolume1\\Windows", 0);

	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, P(test)->type, NT_PATH_NT_OBJECT);
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_VERBATIM);
	KUNIT_EXPECT_EQ(test, P(test)->device_len, 23);
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->device,
				     "\\Device\\HarddiskVolume1", 23), 0);
	KUNIT_EXPECT_STREQ(test, P(test)->rel, "Windows");
}

/* ------------------------------------------------------------ streams */

static void test_named_streams(struct kunit *test)
{
	int err = nt_parse(test, "C:\\hello.txt:metadata", 0);

	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_HAS_STREAM);
	KUNIT_EXPECT_STREQ(test, P(test)->rel, "hello.txt");
	KUNIT_EXPECT_EQ(test, P(test)->stream_len, 8);
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->stream, "metadata", 8), 0);
	KUNIT_EXPECT_EQ(test, P(test)->stream_type, NT_STREAM_TYPE_DATA);

	/* The explicit attribute type is equivalent. */
	err = nt_parse(test, "C:\\hello.txt:metadata:$DATA", 0);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_HAS_STREAM);
	KUNIT_EXPECT_STREQ(test, P(test)->rel, "hello.txt");
	KUNIT_EXPECT_EQ(test, memcmp(P(test)->stream, "metadata", 8), 0);
}

/* "file::$DATA" is the unnamed default stream, which is the file itself. */
static void test_default_stream(struct kunit *test)
{
	int err = nt_parse(test, "C:\\hello.txt::$DATA", 0);

	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_FALSE(test, P(test)->flags & NT_PARSE_HAS_STREAM);
	KUNIT_EXPECT_STREQ(test, P(test)->rel, "hello.txt");
	KUNIT_EXPECT_NULL(test, P(test)->stream);
}

static void test_stream_errors(struct kunit *test)
{
	/* No filename for the stream to belong to. */
	expect_error(test, "C:\\dir\\:stream", -EINVAL);
	/* Unknown attribute type. */
	expect_error(test, "C:\\a.txt:s:$NOPE", -EINVAL);
	/* A colon is only meaningful on the final component. */
	expect_error(test, "C:\\a:s\\b", -EINVAL);

	/* NT_PARSE_F_NO_STREAM makes the colon a plain syntax error. */
	KUNIT_EXPECT_EQ(test,
			nt_parse(test, "C:\\a.txt:s", NT_PARSE_F_NO_STREAM),
			-EINVAL);
}

/* ------------------------------------------------------------- limits */

static void test_invalid_characters(struct kunit *test)
{
	expect_error(test, "C:\\a<b", -EINVAL);
	expect_error(test, "C:\\a>b", -EINVAL);
	expect_error(test, "C:\\a\"b", -EINVAL);
	expect_error(test, "C:\\a|b", -EINVAL);
	expect_error(test, "C:\\a*b", -EINVAL);
	expect_error(test, "C:\\a?b", -EINVAL);
	KUNIT_EXPECT_EQ(test, nt_path_parse("C:\\a\x01" "b", 8, 0,
					    ((struct nt_path_test_ctx *)
					     test->priv)->buf,
					    NT_PATH_BUF_SIZE, P(test)),
			-EINVAL);
}

static void test_empty_and_nul(struct kunit *test)
{
	struct nt_path_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, nt_path_parse("", 0, 0, ctx->buf,
					    NT_PATH_BUF_SIZE, &ctx->parse),
			-ENOENT);

	/* An embedded NUL truncates in C and must be rejected outright. */
	KUNIT_EXPECT_EQ(test, nt_path_parse("C:\\a\0b", 6, 0, ctx->buf,
					    NT_PATH_BUF_SIZE, &ctx->parse),
			-EINVAL);
}

static void test_component_too_long(struct kunit *test)
{
	struct nt_path_test_ctx *ctx = test->priv;
	char *path;

	path = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, path);

	/* 255 characters is the NTFS limit and must be accepted. */
	memcpy(path, "C:\\", 3);
	memset(path + 3, 'a', NT_MAX_COMPONENT + 1);
	KUNIT_EXPECT_EQ(test, nt_path_parse(path, 3 + NT_MAX_COMPONENT, 0,
					    ctx->buf, NT_PATH_BUF_SIZE,
					    &ctx->parse), 0);

	/* 256 is one too many. */
	KUNIT_EXPECT_EQ(test, nt_path_parse(path, 3 + NT_MAX_COMPONENT + 1, 0,
					    ctx->buf, NT_PATH_BUF_SIZE,
					    &ctx->parse), -ENAMETOOLONG);
}

/*
 * MAX_PATH is a Win32 limit and nothing else.  It is reported always and
 * enforced only when the caller says it is a Win32 caller.
 */
static void test_max_path_is_win32_only(struct kunit *test)
{
	struct nt_path_test_ctx *ctx = test->priv;
	char *path;
	int i, pos;

	path = kunit_kzalloc(test, 1024, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, path);

	/* 40 components of 9 characters: comfortably past MAX_PATH. */
	pos = scnprintf(path, 1024, "C:\\");
	for (i = 0; i < 40; i++)
		pos += scnprintf(path + pos, 1024 - pos, "dirname%02d\\", i);

	KUNIT_ASSERT_EQ(test, nt_path_parse(path, strlen(path), 0, ctx->buf,
					    NT_PATH_BUF_SIZE, &ctx->parse), 0);
	KUNIT_EXPECT_TRUE(test, ctx->parse.flags & NT_PARSE_LONG_PATH);

	KUNIT_EXPECT_EQ(test, nt_path_parse(path, strlen(path),
					    NT_PARSE_F_WIN32_LIMITS, ctx->buf,
					    NT_PATH_BUF_SIZE, &ctx->parse),
			-ENAMETOOLONG);

	/* A verbatim path is exempt, which is why \\?\ exists. */
	{
		char *verbatim = kunit_kzalloc(test, 1024, GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, verbatim);
		scnprintf(verbatim, 1024, "\\\\?\\%s", path);
		KUNIT_EXPECT_EQ(test, nt_path_parse(verbatim,
						    strlen(verbatim),
						    NT_PARSE_F_WIN32_LIMITS,
						    ctx->buf,
						    NT_PATH_BUF_SIZE,
						    &ctx->parse), 0);
		KUNIT_EXPECT_FALSE(test,
				   ctx->parse.flags & NT_PARSE_LONG_PATH);
	}
}

static void test_deep_path(struct kunit *test)
{
	struct nt_path_test_ctx *ctx = test->priv;
	char *path;
	int i, pos;

	path = kunit_kzalloc(test, NT_PATH_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, path);

	pos = scnprintf(path, NT_PATH_BUF_SIZE, "C:");
	for (i = 0; i < 300; i++)
		pos += scnprintf(path + pos, NT_PATH_BUF_SIZE - pos, "\\d%d",
				 i);

	KUNIT_ASSERT_EQ(test, nt_path_parse(path, strlen(path), 0, ctx->buf,
					    NT_PATH_BUF_SIZE, &ctx->parse), 0);
	KUNIT_EXPECT_EQ(test, ctx->parse.nr_components, 300);
	KUNIT_EXPECT_EQ(test, ctx->parse.drive, 'C');
}

/* The buffer bound is honoured rather than overrun. */
static void test_buffer_too_small(struct kunit *test)
{
	struct nt_path_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, nt_path_parse("C:\\aaaaaaaaaaaaaaaa", 19, 0,
					    ctx->buf, 8, &ctx->parse),
			-ENAMETOOLONG);
}

/* ------------------------------------------------------------ unicode */

static void test_unicode_names(struct kunit *test)
{
	/* UTF-8 is just bytes to the parser; it must pass through intact. */
	expect_parse(test, "C:\\Users\\Ünïcøde\\Документы",
		     NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Users/Ünïcøde/Документы");
	expect_parse(test, "C:\\日本語\\ファイル.txt",
		     NT_PATH_DRIVE_ABSOLUTE, 'C', "日本語/ファイル.txt");
	expect_parse(test, "C:\\emoji\\\xf0\x9f\x93\x81",
		     NT_PATH_DRIVE_ABSOLUTE, 'C', "emoji/\xf0\x9f\x93\x81");
}

/* --------------------------------------------------- Win32 device names */

static void test_reserved_device_names(struct kunit *test)
{
	static const char * const reserved[] = {
		"CON", "PRN", "AUX", "NUL", "COM1", "COM9", "LPT1", "LPT9",
		"con", "Nul", "cOm1",
	};
	static const char * const ordinary[] = {
		"CONS", "CO", "COM", "COM0", "LPT0", "COM10", "NULL",
		"CONTROL", "AUXILIARY",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(reserved); i++) {
		const char *name = reserved[i];

		KUNIT_EXPECT_TRUE_MSG(test,
				      nt_name_is_reserved_device(name,
								 strlen(name)),
				      "\"%s\" should be a device", name);
	}

	for (i = 0; i < ARRAY_SIZE(ordinary); i++) {
		const char *name = ordinary[i];

		KUNIT_EXPECT_FALSE_MSG(test,
				       nt_name_is_reserved_device(name,
								  strlen(name)),
				       "\"%s\" should not be a device", name);
	}
}

/*
 * Win32 matches a device name against the stem before the first dot and
 * ignores trailing spaces, so "CON.txt" is still the console.
 */
static void test_reserved_name_suffixes(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nt_name_is_reserved_device("CON.txt", 7));
	KUNIT_EXPECT_TRUE(test, nt_name_is_reserved_device("NUL.log.gz", 10));
	KUNIT_EXPECT_TRUE(test, nt_name_is_reserved_device("COM1.anything",
							   13));
	KUNIT_EXPECT_TRUE(test, nt_name_is_reserved_device("CON   ", 6));
	KUNIT_EXPECT_FALSE(test, nt_name_is_reserved_device("MYCON.txt", 9));
}

/*
 * A reserved name is flagged, not rejected: whether it means "open the
 * console" or "this is an error" is a Win32 decision, and NTFS itself
 * has no opinion at all.
 */
static void test_reserved_name_is_flagged_not_enforced(struct kunit *test)
{
	expect_parse(test, "C:\\Users\\CON", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Users/CON");
	KUNIT_EXPECT_TRUE(test, P(test)->flags & NT_PARSE_RESERVED_NAME);

	expect_parse(test, "C:\\Users\\CONS", NT_PATH_DRIVE_ABSOLUTE, 'C',
		     "Users/CONS");
	KUNIT_EXPECT_FALSE(test, P(test)->flags & NT_PARSE_RESERVED_NAME);

	/* Only when the caller asks does it become an error. */
	KUNIT_EXPECT_EQ(test, nt_parse(test, "C:\\Users\\CON",
				       NT_PARSE_F_REJECT_DEVICE), -EINVAL);

	/* A verbatim path is never intercepted; that is how you make one. */
	expect_parse(test, "\\\\?\\C:\\Users\\CON", NT_PATH_DRIVE_ABSOLUTE,
		     'C', "Users/CON");
	KUNIT_EXPECT_FALSE(test, P(test)->flags & NT_PARSE_RESERVED_NAME);
	KUNIT_EXPECT_EQ(test, nt_parse(test, "\\\\?\\C:\\Users\\CON",
				       NT_PARSE_F_REJECT_DEVICE), 0);
}

static void test_valid_win32_names(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nt_name_is_valid_win32("file.txt", 8));
	KUNIT_EXPECT_TRUE(test, nt_name_is_valid_win32("a b c", 5));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32(".", 1));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32("..", 2));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32("a/b", 3));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32("a\\b", 3));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32("a:b", 3));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32("   ", 3));
	KUNIT_EXPECT_FALSE(test, nt_name_is_valid_win32("", 0));
}

static void test_path_type_names(struct kunit *test)
{
	KUNIT_EXPECT_STREQ(test, nt_path_type_name(NT_PATH_DRIVE_ABSOLUTE),
			   "drive-absolute");
	KUNIT_EXPECT_STREQ(test, nt_path_type_name(NT_PATH_UNC), "unc");
	KUNIT_EXPECT_STREQ(test, nt_path_type_name(NT_PATH_INVALID),
			   "invalid");
	KUNIT_EXPECT_STREQ(test, nt_path_type_name((enum nt_path_type)999),
			   "unknown");
}

static struct kunit_case nt_path_test_cases[] = {
	KUNIT_CASE(test_drive_absolute),
	KUNIT_CASE(test_drive_letter_case),
	KUNIT_CASE(test_drive_relative),
	KUNIT_CASE(test_rooted_and_relative),
	KUNIT_CASE(test_forward_slash_separator),
	KUNIT_CASE(test_separator_collapse),
	KUNIT_CASE(test_trailing_separator),
	KUNIT_CASE(test_dot_components),
	KUNIT_CASE(test_trailing_dot_and_space_trim),
	KUNIT_CASE(test_component_count),
	KUNIT_CASE(test_extended_prefix),
	KUNIT_CASE(test_extended_unc),
	KUNIT_CASE(test_unc),
	KUNIT_CASE(test_win32_device_namespace),
	KUNIT_CASE(test_nt_object_dos_devices),
	KUNIT_CASE(test_nt_object_device_path),
	KUNIT_CASE(test_named_streams),
	KUNIT_CASE(test_default_stream),
	KUNIT_CASE(test_stream_errors),
	KUNIT_CASE(test_invalid_characters),
	KUNIT_CASE(test_empty_and_nul),
	KUNIT_CASE(test_component_too_long),
	KUNIT_CASE(test_max_path_is_win32_only),
	KUNIT_CASE(test_deep_path),
	KUNIT_CASE(test_buffer_too_small),
	KUNIT_CASE(test_unicode_names),
	KUNIT_CASE(test_reserved_device_names),
	KUNIT_CASE(test_reserved_name_suffixes),
	KUNIT_CASE(test_reserved_name_is_flagged_not_enforced),
	KUNIT_CASE(test_valid_win32_names),
	KUNIT_CASE(test_path_type_names),
	{}
};

static struct kunit_suite nt_path_test_suite = {
	.name = "ntpers-path",
	.init = nt_path_test_init,
	.test_cases = nt_path_test_cases,
};
kunit_test_suite(nt_path_test_suite);

MODULE_DESCRIPTION("KUnit tests for the NT/Win32 pathname parser");
MODULE_LICENSE("GPL");
