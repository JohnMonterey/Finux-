// SPDX-License-Identifier: GPL-2.0
/*
 * Selftests for the NT filesystem personality.
 *
 * Covers the parts that are only visible from userspace: the prctl
 * personality interface, and end-to-end pathname resolution through the
 * debugfs test interface, including real case-insensitive lookup against
 * a live filesystem.
 *
 * The KUnit suites in fs/ntpers/tests cover the parser exhaustively; this
 * checks that the pieces are wired together.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest_harness.h"

#ifndef PR_SET_NT_PERSONALITY
#define PR_SET_NT_PERSONALITY	82
#define PR_GET_NT_PERSONALITY	83
#define PR_SET_NT_DRIVE		84
#define PR_GET_NT_DRIVE		85
#endif

#ifndef NT_PERSONALITY_ENABLED
#define NT_PERSONALITY_ENABLED		(1U << 0)
#define NT_PERSONALITY_CASE_INSENSITIVE	(1U << 1)
#define NT_PERSONALITY_WIN32_RULES	(1U << 2)
#endif

#define DEBUGFS_DIR	"/sys/kernel/debug/ntpers"

static int debugfs_present(void)
{
	return access(DEBUGFS_DIR "/parse", W_OK) == 0;
}

/*
 * Write a request to one of the rw debugfs files and read the answer back
 * through the same file description, which is what keeps concurrent users
 * from seeing each other's results.
 */
static int nt_query(const char *file, const char *request, char *out,
		    size_t out_size)
{
	char path[128];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), DEBUGFS_DIR "/%s", file);

	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;

	if (write(fd, request, strlen(request)) < 0) {
		close(fd);
		return -1;
	}

	n = pread(fd, out, out_size - 1, 0);
	close(fd);

	if (n < 0)
		return -1;

	out[n] = '\0';
	return 0;
}

static int nt_control(const char *cmd)
{
	int fd, ret;

	fd = open(DEBUGFS_DIR "/control", O_WRONLY);
	if (fd < 0)
		return -1;

	ret = write(fd, cmd, strlen(cmd)) < 0 ? -1 : 0;
	close(fd);
	return ret;
}

/* Pull "key value" out of the key/value blocks the debugfs files emit. */
static const char *field(const char *blob, const char *key, char *buf,
			 size_t size)
{
	const char *p = blob;
	size_t keylen = strlen(key);

	while (p && *p) {
		if (!strncmp(p, key, keylen) && p[keylen] == ' ') {
			const char *val = p + keylen + 1;
			const char *end = strchr(val, '\n');
			size_t len = end ? (size_t)(end - val) : strlen(val);

			if (len >= size)
				len = size - 1;
			memcpy(buf, val, len);
			buf[len] = '\0';
			return buf;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}

	buf[0] = '\0';
	return NULL;
}

/* ------------------------------------------------------------- prctl */

TEST(personality_set_and_get)
{
	unsigned int flags = 0xdeadbeef;
	int ret;

	ret = prctl(PR_SET_NT_PERSONALITY, NT_PERSONALITY_ENABLED, 0, 0, 0);
	if (ret == -1 && errno == EINVAL)
		SKIP(return, "CONFIG_NT_FS_PERSONALITY not enabled");
	ASSERT_EQ(0, ret);

	ASSERT_EQ(0, prctl(PR_GET_NT_PERSONALITY, &flags, 0, 0, 0));
	EXPECT_EQ(NT_PERSONALITY_ENABLED, flags);

	ASSERT_EQ(0, prctl(PR_SET_NT_PERSONALITY,
			   NT_PERSONALITY_ENABLED |
			   NT_PERSONALITY_CASE_INSENSITIVE, 0, 0, 0));
	ASSERT_EQ(0, prctl(PR_GET_NT_PERSONALITY, &flags, 0, 0, 0));
	EXPECT_EQ(NT_PERSONALITY_ENABLED | NT_PERSONALITY_CASE_INSENSITIVE,
		  flags);

	/* Turning everything off is allowed and leaves the process usable. */
	ASSERT_EQ(0, prctl(PR_SET_NT_PERSONALITY, 0, 0, 0, 0));
	ASSERT_EQ(0, prctl(PR_GET_NT_PERSONALITY, &flags, 0, 0, 0));
	EXPECT_EQ(0u, flags);
}

TEST(personality_rejects_unknown_flags)
{
	int ret = prctl(PR_SET_NT_PERSONALITY, 0x80000000, 0, 0, 0);

	ASSERT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);
}

TEST(current_drive)
{
	unsigned int drive = 0;
	int ret;

	ret = prctl(PR_SET_NT_DRIVE, 'D', 0, 0, 0);
	if (ret == -1 && errno == EINVAL)
		SKIP(return, "CONFIG_NT_FS_PERSONALITY not enabled");
	ASSERT_EQ(0, ret);

	ASSERT_EQ(0, prctl(PR_GET_NT_DRIVE, &drive, 0, 0, 0));
	EXPECT_EQ((unsigned int)'D', drive);

	/* Lowercase is accepted and normalised, as everywhere in NT. */
	ASSERT_EQ(0, prctl(PR_SET_NT_DRIVE, 'e', 0, 0, 0));
	ASSERT_EQ(0, prctl(PR_GET_NT_DRIVE, &drive, 0, 0, 0));
	EXPECT_EQ((unsigned int)'E', drive);

	EXPECT_EQ(-1, prctl(PR_SET_NT_DRIVE, '3', 0, 0, 0));
	EXPECT_EQ(EINVAL, errno);
}

/* The personality is process state and must survive fork into the child. */
TEST(personality_inherited_across_fork)
{
	unsigned int flags = 0;
	int status;
	pid_t pid;

	if (prctl(PR_SET_NT_PERSONALITY, NT_PERSONALITY_ENABLED, 0, 0, 0))
		SKIP(return, "CONFIG_NT_FS_PERSONALITY not enabled");
	ASSERT_EQ(0, prctl(PR_SET_NT_DRIVE, 'F', 0, 0, 0));

	pid = fork();
	ASSERT_LE(0, pid);

	if (pid == 0) {
		unsigned int cflags = 0, cdrive = 0;

		if (prctl(PR_GET_NT_PERSONALITY, &cflags, 0, 0, 0))
			_exit(10);
		if (prctl(PR_GET_NT_DRIVE, &cdrive, 0, 0, 0))
			_exit(11);
		if (cflags != NT_PERSONALITY_ENABLED)
			_exit(12);
		if (cdrive != 'F')
			_exit(13);

		/* A change in the child must not reach the parent. */
		if (prctl(PR_SET_NT_DRIVE, 'G', 0, 0, 0))
			_exit(14);
		_exit(0);
	}

	ASSERT_EQ(pid, waitpid(pid, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	ASSERT_EQ(0, prctl(PR_GET_NT_DRIVE, &flags, 0, 0, 0));
	EXPECT_EQ((unsigned int)'F', flags);
}

/* -------------------------------------------------------- parse via debugfs */

FIXTURE(nt_debugfs) {
	char reply[4096];
	char value[512];
};

FIXTURE_SETUP(nt_debugfs)
{
	if (geteuid() != 0)
		SKIP(return, "need root for " DEBUGFS_DIR);
	if (!debugfs_present())
		SKIP(return, DEBUGFS_DIR " not available");
}

FIXTURE_TEARDOWN(nt_debugfs)
{
}

#define PARSE(path) \
	ASSERT_EQ(0, nt_query("parse", path, self->reply, sizeof(self->reply)))

#define FIELD(key) field(self->reply, key, self->value, sizeof(self->value))

TEST_F(nt_debugfs, parse_drive_paths)
{
	PARSE("C:\\Windows\\System32");
	EXPECT_STREQ("drive-absolute", FIELD("type"));
	EXPECT_STREQ("C", FIELD("drive"));
	EXPECT_STREQ("Windows/System32", FIELD("rel"));

	PARSE("c:\\windows");
	EXPECT_STREQ("C", FIELD("drive"));
	EXPECT_STREQ("windows", FIELD("rel"));

	PARSE("C:\\WINDOWS");
	EXPECT_STREQ("C", FIELD("drive"));
	EXPECT_STREQ("WINDOWS", FIELD("rel"));

	PARSE("C:Windows");
	EXPECT_STREQ("drive-relative", FIELD("type"));

	PARSE("\\Windows");
	EXPECT_STREQ("rooted", FIELD("type"));

	PARSE("Windows");
	EXPECT_STREQ("relative", FIELD("type"));
}

TEST_F(nt_debugfs, parse_unicode_and_deep)
{
	char deep[3000];
	int pos, i;

	PARSE("C:\\Users\\Ünïcøde\\Документы");
	EXPECT_STREQ("Users/Ünïcøde/Документы", FIELD("rel"));

	PARSE("C:\\日本語\\ファイル.txt");
	EXPECT_STREQ("日本語/ファイル.txt", FIELD("rel"));

	pos = snprintf(deep, sizeof(deep), "C:");
	for (i = 0; i < 200 && pos < (int)sizeof(deep) - 16; i++)
		pos += snprintf(deep + pos, sizeof(deep) - pos, "\\d%d", i);

	PARSE(deep);
	EXPECT_STREQ("drive-absolute", FIELD("type"));
	EXPECT_STREQ("200", FIELD("components"));
}

TEST_F(nt_debugfs, parse_invalid_paths)
{
	PARSE("C:\\a<b");
	EXPECT_STREQ("-22", FIELD("error"));

	PARSE("C:\\a|b");
	EXPECT_STREQ("-22", FIELD("error"));

	PARSE("C:\\...");
	EXPECT_STREQ("-22", FIELD("error"));
}

TEST_F(nt_debugfs, parse_trailing_separator_and_dots)
{
	PARSE("C:\\Windows\\");
	EXPECT_STREQ("Windows", FIELD("rel"));
	EXPECT_NE(NULL, strstr(self->reply, "trailing-sep"));

	PARSE("C:\\Windows.");
	EXPECT_STREQ("Windows", FIELD("rel"));
	EXPECT_NE(NULL, strstr(self->reply, "trimmed"));

	PARSE("C:\\a\\..\\b");
	EXPECT_STREQ("b", FIELD("rel"));
}

TEST_F(nt_debugfs, parse_streams)
{
	PARSE("C:\\hello.txt:metadata");
	EXPECT_STREQ("hello.txt", FIELD("rel"));
	EXPECT_STREQ("metadata", FIELD("stream"));

	/* The unnamed default stream is the file itself. */
	PARSE("C:\\hello.txt::$DATA");
	EXPECT_STREQ("hello.txt", FIELD("rel"));
	EXPECT_EQ(NULL, strstr(self->reply, "\nstream "));
}

/*
 * A reserved device name is flagged for a Win32 personality to act on;
 * it is not an error at this layer, and a verbatim path is not even
 * flagged.  Getting this wrong is the classic layering mistake.
 */
TEST_F(nt_debugfs, parse_reserved_names_are_flagged_only)
{
	PARSE("C:\\Users\\CON");
	EXPECT_STREQ("Users/CON", FIELD("rel"));
	EXPECT_NE(NULL, strstr(self->reply, "reserved"));

	PARSE("C:\\Users\\CON.txt");
	EXPECT_NE(NULL, strstr(self->reply, "reserved"));

	PARSE("C:\\Users\\CONS");
	EXPECT_EQ(NULL, strstr(self->reply, "reserved"));

	PARSE("\\\\?\\C:\\Users\\CON");
	EXPECT_EQ(NULL, strstr(self->reply, "reserved"));
	EXPECT_NE(NULL, strstr(self->reply, "verbatim"));
}

TEST_F(nt_debugfs, parse_nt_namespace_forms)
{
	PARSE("\\\\?\\C:\\Windows");
	EXPECT_STREQ("drive-absolute", FIELD("type"));
	EXPECT_NE(NULL, strstr(self->reply, "verbatim"));

	PARSE("\\??\\C:\\Windows");
	EXPECT_STREQ("drive-absolute", FIELD("type"));
	EXPECT_STREQ("C", FIELD("drive"));

	PARSE("\\Device\\HarddiskVolume1\\Windows");
	EXPECT_STREQ("nt-object", FIELD("type"));
	EXPECT_STREQ("\\Device\\HarddiskVolume1", FIELD("device"));
	EXPECT_STREQ("Windows", FIELD("rel"));

	PARSE("\\\\server\\share\\dir");
	EXPECT_STREQ("unc", FIELD("type"));
	EXPECT_STREQ("server", FIELD("unc_server"));

	PARSE("\\\\.\\PhysicalDrive0");
	EXPECT_STREQ("device", FIELD("type"));
	EXPECT_STREQ("PhysicalDrive0", FIELD("device"));
}

/* ------------------------------------------------------- resolve end-to-end */

FIXTURE(nt_volume) {
	char reply[4096];
	char value[512];
	char dir[64];
	int have_volume;
};

FIXTURE_SETUP(nt_volume)
{
	if (geteuid() != 0)
		SKIP(return, "need root for " DEBUGFS_DIR);
	if (!debugfs_present())
		SKIP(return, DEBUGFS_DIR " not available");

	/*
	 * A tmpfs of our own, registered as drive T:, so the test never
	 * touches the machine's real drive letters or its real files.
	 */
	strcpy(self->dir, "/tmp/ntpers_selftest_XXXXXX");
	ASSERT_NE(NULL, mkdtemp(self->dir));

	if (nt_control("unmount T") == 0)
		; /* left over from an interrupted run; ignore result */

	{
		char cmd[PATH_MAX];

		snprintf(cmd, sizeof(cmd), "mount T %s TestVol", self->dir);
		if (nt_control(cmd)) {
			rmdir(self->dir);
			SKIP(return, "could not register test volume");
		}
	}
	self->have_volume = 1;
}

FIXTURE_TEARDOWN(nt_volume)
{
	char victim[PATH_MAX];

	if (self->have_volume)
		nt_control("unmount T");

	if (self->dir[0]) {
		snprintf(victim, sizeof(victim), "%s/TestFile.txt", self->dir);
		unlink(victim);
		snprintf(victim, sizeof(victim), "%s/MixedCaseDir",
			 self->dir);
		rmdir(victim);
		rmdir(self->dir);
	}
}

/*
 * Join a directory and a name without a format string, so the bound is
 * checked here rather than guessed at by the compiler.
 */
static int join_path(char *out, size_t size, const char *dir,
		     const char *name)
{
	size_t dlen = strlen(dir), nlen = strlen(name);

	if (dlen + 1 + nlen + 1 > size)
		return -1;

	memcpy(out, dir, dlen);
	out[dlen] = '/';
	memcpy(out + dlen + 1, name, nlen);
	out[dlen + 1 + nlen] = '\0';
	return 0;
}

static int make_file(const char *dir, const char *name)
{
	char path[PATH_MAX];
	int fd;

	if (join_path(path, sizeof(path), dir, name))
		return -1;

	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

#define RESOLVE(path) \
	ASSERT_EQ(0, nt_query("resolve", path, self->reply, \
			      sizeof(self->reply)))

TEST_F(nt_volume, volume_appears_in_table)
{
	char blob[8192];
	int fd;
	ssize_t n;

	fd = open(DEBUGFS_DIR "/volumes", O_RDONLY);
	ASSERT_LE(0, fd);
	n = read(fd, blob, sizeof(blob) - 1);
	close(fd);
	ASSERT_LE(0, n);
	blob[n] = '\0';

	EXPECT_NE(NULL, strstr(blob, "letter=T:"));
	EXPECT_NE(NULL, strstr(blob, "TestVol"));
	/* Every volume gets an NT device name, not just a letter. */
	EXPECT_NE(NULL, strstr(blob, "\\Device\\HarddiskVolume"));
}

TEST_F(nt_volume, resolve_volume_root)
{
	RESOLVE("T:\\");
	EXPECT_STREQ("T", FIELD("volume"));
	EXPECT_STREQ(self->dir, FIELD("posix"));
}

TEST_F(nt_volume, resolve_file)
{
	char expect[PATH_MAX];

	ASSERT_EQ(0, make_file(self->dir, "TestFile.txt"));
	snprintf(expect, sizeof(expect), "%s/TestFile.txt", self->dir);

	RESOLVE("T:\\TestFile.txt");
	EXPECT_EQ(NULL, strstr(self->reply, "error"));
	EXPECT_STREQ("T", FIELD("volume"));
	EXPECT_STREQ(expect, FIELD("posix"));
}

/*
 * The headline behaviour: create TestFile.txt, find it through any
 * casing, and confirm the stored name keeps its original spelling.
 */
TEST_F(nt_volume, case_insensitive_lookup_preserves_case)
{
	static const char * const spellings[] = {
		"T:\\TestFile.txt",
		"T:\\testfile.txt",
		"T:\\TESTFILE.TXT",
		"T:\\TeStFiLe.TxT",
	};
	char expect[PATH_MAX];
	size_t i;

	ASSERT_EQ(0, make_file(self->dir, "TestFile.txt"));
	snprintf(expect, sizeof(expect), "%s/TestFile.txt", self->dir);

	for (i = 0; i < ARRAY_SIZE(spellings); i++) {
		ASSERT_EQ(0, nt_query("resolve", spellings[i], self->reply,
				      sizeof(self->reply)));
		EXPECT_EQ(NULL, strstr(self->reply, "error"))
			TH_LOG("resolving %s: %s", spellings[i],
			       self->reply);
		/*
		 * The resolved POSIX path must show the *stored* spelling,
		 * which is what case-preserving means.
		 */
		EXPECT_STREQ(expect, FIELD("posix"))
			TH_LOG("resolving %s", spellings[i]);
	}

	/* Case-sensitive resolution must still refuse the wrong casing. */
	ASSERT_EQ(0, nt_query("resolve", "-s T:\\TESTFILE.TXT", self->reply,
			      sizeof(self->reply)));
	EXPECT_NE(NULL, strstr(self->reply, "error"));

	/* ... and still accept the right one. */
	ASSERT_EQ(0, nt_query("resolve", "-s T:\\TestFile.txt", self->reply,
			      sizeof(self->reply)));
	EXPECT_STREQ(expect, FIELD("posix"));
}

TEST_F(nt_volume, case_insensitive_directory_lookup)
{
	char path[PATH_MAX];
	char expect[PATH_MAX];

	snprintf(path, sizeof(path), "%s/MixedCaseDir", self->dir);
	ASSERT_EQ(0, mkdir(path, 0755));
	snprintf(expect, sizeof(expect), "%s/MixedCaseDir", self->dir);

	RESOLVE("T:\\mixedcasedir");
	EXPECT_STREQ(expect, FIELD("posix"));

	RESOLVE("T:\\MIXEDCASEDIR");
	EXPECT_STREQ(expect, FIELD("posix"));

	/* And through a differently-cased directory to a file inside it. */
	ASSERT_EQ(0, make_file(path, "Inner.txt"));
	snprintf(expect, sizeof(expect), "%s/MixedCaseDir/Inner.txt",
		 self->dir);

	RESOLVE("T:\\MIXEDCASEDIR\\INNER.TXT");
	EXPECT_STREQ(expect, FIELD("posix"));

	unlink(expect);
}

TEST_F(nt_volume, resolve_missing_file_and_drive)
{
	RESOLVE("T:\\does_not_exist");
	EXPECT_STREQ("-2", FIELD("error"));

	/* A drive letter with no volume is -ENODEV, distinct from -ENOENT. */
	RESOLVE("Q:\\anything");
	EXPECT_STREQ("-19", FIELD("error"));
}

TEST_F(nt_volume, resolve_dot_dot_stays_on_volume)
{
	char expect[PATH_MAX];

	ASSERT_EQ(0, make_file(self->dir, "TestFile.txt"));
	snprintf(expect, sizeof(expect), "%s/TestFile.txt", self->dir);

	/* Escaping above the volume root is not an error; it clamps. */
	RESOLVE("T:\\..\\..\\TestFile.txt");
	EXPECT_STREQ(expect, FIELD("posix"));

	RESOLVE("T:\\sub\\..\\TestFile.txt");
	EXPECT_STREQ(expect, FIELD("posix"));
}

TEST_F(nt_volume, resolve_nt_object_path)
{
	char blob[8192], devname[64] = "", path[PATH_MAX];
	const char *p;
	int fd;
	ssize_t n;

	/* Find the NT device name the volume layer gave our test volume. */
	fd = open(DEBUGFS_DIR "/volumes", O_RDONLY);
	ASSERT_LE(0, fd);
	n = read(fd, blob, sizeof(blob) - 1);
	close(fd);
	ASSERT_LE(0, n);
	blob[n] = '\0';

	p = strstr(blob, "letter=T:");
	ASSERT_NE(NULL, p);
	p = strstr(p, "nt=");
	ASSERT_NE(NULL, p);
	ASSERT_EQ(1, sscanf(p + 3, "%63s", devname));
	ASSERT_NE('\0', devname[0]);

	/* The same volume must be reachable without any drive letter. */
	snprintf(path, sizeof(path), "%s\\", devname);
	ASSERT_EQ(0, nt_query("resolve", path, self->reply,
			      sizeof(self->reply)));
	EXPECT_STREQ(self->dir, FIELD("posix"));
}

TEST_HARNESS_MAIN
