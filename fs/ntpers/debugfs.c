// SPDX-License-Identifier: GPL-2.0
/*
 * NT personality debugfs interface.
 *
 * A Win32 syscall surface does not exist yet, so this is how the volume
 * table is populated and how the parser and resolver are driven from
 * userspace.  It is a development and test interface: everything here is
 * root-only and none of it is ABI.
 *
 *   <debugfs>/ntpers/volumes	  read   the volume table
 *   <debugfs>/ntpers/control	  write  mount/unmount/system commands
 *   <debugfs>/ntpers/parse	  rw     write a path, read its parse
 *   <debugfs>/ntpers/resolve	  rw     write a path, read what it resolved to
 *   <debugfs>/ntpers/getinfo	  rw     write a path, read its NT metadata
 *   <debugfs>/ntpers/cache	  read   fold-hint cache counters
 *   <debugfs>/ntpers/personality rw     this process's personality flags
 *
 * The rw files keep their result in per-open state, so two processes
 * using them at the same time cannot see each other's answers.
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/nt_personality.h>

#include "internal.h"

#define NT_DBG_RESULT_SIZE	(PATH_MAX + 512)
#define NT_DBG_INPUT_SIZE	(PATH_MAX + 64)

static struct dentry *nt_debugfs_root;

struct nt_dbg_state {
	/* serialises @result against a concurrent read */
	struct mutex	lock;
	char		*result;
	size_t		result_len;
};

static int nt_dbg_open(struct inode *inode, struct file *file)
{
	struct nt_dbg_state *st;

	st = kzalloc_obj(struct nt_dbg_state);
	if (!st)
		return -ENOMEM;

	st->result = kzalloc(NT_DBG_RESULT_SIZE, GFP_KERNEL);
	if (!st->result) {
		kfree(st);
		return -ENOMEM;
	}

	mutex_init(&st->lock);
	file->private_data = st;
	return 0;
}

static int nt_dbg_release(struct inode *inode, struct file *file)
{
	struct nt_dbg_state *st = file->private_data;

	kfree(st->result);
	kfree(st);
	return 0;
}

static ssize_t nt_dbg_read(struct file *file, char __user *ubuf, size_t count,
			   loff_t *ppos)
{
	struct nt_dbg_state *st = file->private_data;
	ssize_t ret;

	mutex_lock(&st->lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, st->result,
				      st->result_len);
	mutex_unlock(&st->lock);

	return ret;
}

/* Read a written command into a kernel buffer, trimming the newline. */
static char *nt_dbg_get_input(const char __user *ubuf, size_t count)
{
	char *buf;

	if (count == 0 || count > NT_DBG_INPUT_SIZE)
		return ERR_PTR(-EINVAL);

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return buf;

	strim(buf);
	if (!buf[0]) {
		kfree(buf);
		return ERR_PTR(-EINVAL);
	}

	return buf;
}

/* ---------------------------------------------------------------- parse */

static ssize_t nt_dbg_parse_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct nt_dbg_state *st = file->private_data;
	struct nt_path_parse parse;
	char *input, *pbuf;
	int err;
	int n = 0;

	input = nt_dbg_get_input(ubuf, count);
	if (IS_ERR(input))
		return PTR_ERR(input);

	pbuf = __getname();
	if (!pbuf) {
		kfree(input);
		return -ENOMEM;
	}

	err = nt_path_parse(input, strlen(input), 0, pbuf, PATH_MAX, &parse);

	mutex_lock(&st->lock);
	if (err) {
		n = scnprintf(st->result, NT_DBG_RESULT_SIZE,
			      "error %d\n", err);
	} else {
		n = scnprintf(st->result, NT_DBG_RESULT_SIZE,
			      "type %s\ndrive %c\nrel %s\ncomponents %u\nflags 0x%x%s%s%s%s%s%s%s\n",
			      nt_path_type_name(parse.type),
			      parse.drive ? parse.drive : '-',
			      parse.rel, parse.nr_components, parse.flags,
			      (parse.flags & NT_PARSE_VERBATIM) ?
					" verbatim" : "",
			      (parse.flags & NT_PARSE_TRAILING_SEP) ?
					" trailing-sep" : "",
			      (parse.flags & NT_PARSE_HAS_STREAM) ?
					" stream" : "",
			      (parse.flags & NT_PARSE_RESERVED_NAME) ?
					" reserved" : "",
			      (parse.flags & NT_PARSE_VOLUME_ROOT) ?
					" volume-root" : "",
			      (parse.flags & NT_PARSE_LONG_PATH) ?
					" long-path" : "",
			      (parse.flags & NT_PARSE_TRIMMED) ?
					" trimmed" : "");

		if (parse.flags & NT_PARSE_HAS_STREAM)
			n += scnprintf(st->result + n,
				       NT_DBG_RESULT_SIZE - n,
				       "stream %.*s\nstream_type 0x%x\n",
				       parse.stream_len, parse.stream,
				       parse.stream_type);
		if (parse.device_len)
			n += scnprintf(st->result + n,
				       NT_DBG_RESULT_SIZE - n,
				       "device %.*s\n", parse.device_len,
				       parse.device);
		if (parse.unc_server_len)
			n += scnprintf(st->result + n,
				       NT_DBG_RESULT_SIZE - n,
				       "unc_server %.*s\nunc_share %.*s\n",
				       parse.unc_server_len, parse.unc_server,
				       parse.unc_share_len,
				       parse.unc_share ?: "");
	}
	st->result_len = n;
	mutex_unlock(&st->lock);

	__putname(pbuf);
	kfree(input);
	*ppos = 0;
	return count;
}

static const struct file_operations nt_dbg_parse_fops = {
	.owner		= THIS_MODULE,
	.open		= nt_dbg_open,
	.release	= nt_dbg_release,
	.read		= nt_dbg_read,
	.write		= nt_dbg_parse_write,
};

/* -------------------------------------------------------------- resolve */

static ssize_t nt_dbg_resolve_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct nt_dbg_state *st = file->private_data;
	struct nt_path_parse parse;
	struct nt_path ntp;
	char *input, *pbuf, *dbuf, *shown;
	u32 rflags = NT_RESOLVE_FOLLOW | NT_RESOLVE_CASE_INSENSITIVE;
	int err;
	int n;

	input = nt_dbg_get_input(ubuf, count);
	if (IS_ERR(input))
		return PTR_ERR(input);

	/*
	 * A leading "-s " asks for case-sensitive resolution, so a test can
	 * check that the case-insensitive path is really doing something.
	 */
	if (!strncmp(input, "-s ", 3)) {
		rflags &= ~NT_RESOLVE_CASE_INSENSITIVE;
		memmove(input, input + 3, strlen(input + 3) + 1);
		strim(input);
	}

	pbuf = __getname();
	dbuf = __getname();
	if (!pbuf || !dbuf) {
		err = -ENOMEM;
		goto out_free;
	}

	err = nt_path_parse(input, strlen(input), 0, pbuf, PATH_MAX, &parse);
	if (err)
		goto out_report;

	err = nt_path_resolve(nt_ctx_current(), &parse, rflags, &ntp);
	if (err)
		goto out_report;

	shown = d_path(&ntp.path, dbuf, PATH_MAX);
	if (IS_ERR(shown))
		shown = (char *)"<unreachable>";

	mutex_lock(&st->lock);
	n = scnprintf(st->result, NT_DBG_RESULT_SIZE,
		      "ok\nvolume %c\nposix %s\nino %llu\ntype %s\n",
		      ntp.volume && ntp.volume->letter ?
				ntp.volume->letter : '-',
		      shown,
		      d_really_is_positive(ntp.path.dentry) ?
				d_inode(ntp.path.dentry)->i_ino : 0,
		      nt_path_type_name(parse.type));
	st->result_len = n;
	mutex_unlock(&st->lock);

	nt_path_put(&ntp);
	err = 0;
	goto out_free;

out_report:
	mutex_lock(&st->lock);
	st->result_len = scnprintf(st->result, NT_DBG_RESULT_SIZE,
				   "error %d\n", err);
	mutex_unlock(&st->lock);
	err = 0;

out_free:
	if (pbuf)
		__putname(pbuf);
	if (dbuf)
		__putname(dbuf);
	kfree(input);
	*ppos = 0;
	return err ? err : count;
}

static const struct file_operations nt_dbg_resolve_fops = {
	.owner		= THIS_MODULE,
	.open		= nt_dbg_open,
	.release	= nt_dbg_release,
	.read		= nt_dbg_read,
	.write		= nt_dbg_resolve_write,
};

/* -------------------------------------------------------------- getinfo */

/*
 * Resolve an NT path and report the NT view of the file: DOS attributes,
 * all four timestamps in NT units, and the file identifiers.
 *
 * "setattr <hex> <path>" sets the DOS attributes instead of reporting
 * them, so the whole metadata layer is reachable without a Win32 syscall
 * surface.
 */
static ssize_t nt_dbg_getinfo_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct nt_dbg_state *st = file->private_data;
	struct nt_path_parse parse;
	struct nt_file_info info;
	struct nt_path ntp;
	char *input, *pbuf;
	const char *ntpath;
	u32 set_attrs = 0;
	bool setting = false;
	int err, n;

	input = nt_dbg_get_input(ubuf, count);
	if (IS_ERR(input))
		return PTR_ERR(input);

	ntpath = input;

	if (!strncmp(input, "setattr ", 8)) {
		char *attrs_arg = input + 8;
		char *sep;

		while (*attrs_arg == ' ')
			attrs_arg++;

		sep = strchr(attrs_arg, ' ');
		if (!sep) {
			err = -EINVAL;
			goto out_input;
		}
		*sep++ = '\0';
		while (*sep == ' ')
			sep++;

		err = kstrtou32(attrs_arg, 16, &set_attrs);
		if (err)
			goto out_input;

		setting = true;
		ntpath = sep;
	}

	if (!*ntpath) {
		err = -EINVAL;
		goto out_input;
	}

	pbuf = __getname();
	if (!pbuf) {
		err = -ENOMEM;
		goto out_input;
	}

	err = nt_path_parse(ntpath, strlen(ntpath), 0, pbuf, PATH_MAX,
			    &parse);
	if (err)
		goto out_report;

	err = nt_path_resolve(nt_ctx_current(), &parse,
			      NT_RESOLVE_FOLLOW | NT_RESOLVE_CASE_INSENSITIVE,
			      &ntp);
	if (err)
		goto out_report;

	if (setting) {
		err = nt_set_file_attributes(&ntp.path, set_attrs);
		if (err) {
			nt_path_put(&ntp);
			goto out_report;
		}
	}

	err = nt_query_file_info(&ntp.path, ntp.volume, &info);
	nt_path_put(&ntp);
	if (err)
		goto out_report;

	mutex_lock(&st->lock);
	n = scnprintf(st->result, NT_DBG_RESULT_SIZE,
		      "ok\nattributes 0x%08x\nreparse_tag 0x%08x\n"
		      "creation %llu\ncreation_exact %d\n"
		      "last_access %llu\nlast_write %llu\nchange %llu\n"
		      "file_id %llu\nfile_id_128 %16phN\n"
		      "volume_serial %08X\nsize %llu\nalloc_size %llu\n"
		      "nlink %u\n",
		      info.attributes, info.reparse_tag,
		      nt_time_from_timespec(&info.creation),
		      !!(info.time_flags & NT_TIME_CREATION_EXACT),
		      nt_time_from_timespec(&info.last_access),
		      nt_time_from_timespec(&info.last_write),
		      nt_time_from_timespec(&info.change),
		      info.file_id, info.file_id_128,
		      info.volume_serial, info.size, info.alloc_size,
		      info.nlink);
	st->result_len = n;
	mutex_unlock(&st->lock);

	err = 0;
	goto out_name;

out_report:
	mutex_lock(&st->lock);
	st->result_len = scnprintf(st->result, NT_DBG_RESULT_SIZE,
				   "error %d\n", err);
	mutex_unlock(&st->lock);
	err = 0;

out_name:
	__putname(pbuf);
out_input:
	kfree(input);
	*ppos = 0;
	return err ? err : count;
}

static const struct file_operations nt_dbg_getinfo_fops = {
	.owner		= THIS_MODULE,
	.open		= nt_dbg_open,
	.release	= nt_dbg_release,
	.read		= nt_dbg_read,
	.write		= nt_dbg_getinfo_write,
};

/* -------------------------------------------------------------- control */

/*
 * Commands, one per write():
 *
 *	mount <letter> <posix-path> [label]
 *	system <letter>
 *	unmount <letter>
 *
 * "mount" does not mount anything; it registers an existing Linux
 * directory as the root of an NT volume.  Registering the root of the
 * filesystem as C: is how the system volume is established until an
 * installer does it.
 */
static int nt_dbg_cmd_mount(char *args)
{
	struct nt_volume *vol;
	struct path root;
	char *letter_s, *path_s, *label;
	int err;

	letter_s = strsep(&args, " \t");
	path_s = strsep(&args, " \t");
	label = args;

	if (!letter_s || !path_s || strlen(letter_s) != 1)
		return -EINVAL;
	if (nt_drive_index(letter_s[0]) < 0)
		return -EINVAL;

	err = kern_path(path_s, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &root);
	if (err)
		return err;

	vol = nt_volume_create(&init_nt_ns, &root, letter_s[0], 0,
			       label && *label ? label : NULL);
	path_put(&root);

	if (IS_ERR(vol))
		return PTR_ERR(vol);

	/* The namespace holds the reference that keeps it alive. */
	nt_volume_put(vol);
	return 0;
}

static int nt_dbg_cmd_system(char *args)
{
	struct nt_volume *vol;
	int err;

	if (!args || strlen(args) != 1)
		return -EINVAL;

	vol = nt_volume_lookup_letter(&init_nt_ns, args[0]);
	if (!vol)
		return -ENODEV;

	err = nt_volume_set_system(&init_nt_ns, vol);
	nt_volume_put(vol);
	return err;
}

static int nt_dbg_cmd_unmount(char *args)
{
	struct nt_volume *vol;

	if (!args || strlen(args) != 1)
		return -EINVAL;

	vol = nt_volume_lookup_letter(&init_nt_ns, args[0]);
	if (!vol)
		return -ENODEV;

	nt_volume_destroy(&init_nt_ns, vol);
	nt_volume_put(vol);
	return 0;
}

static ssize_t nt_dbg_control_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char *input, *args, *cmd;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	input = nt_dbg_get_input(ubuf, count);
	if (IS_ERR(input))
		return PTR_ERR(input);

	args = input;
	cmd = strsep(&args, " \t");
	if (args)
		strim(args);

	if (!strcmp(cmd, "mount"))
		err = nt_dbg_cmd_mount(args);
	else if (!strcmp(cmd, "system"))
		err = nt_dbg_cmd_system(args);
	else if (!strcmp(cmd, "unmount"))
		err = nt_dbg_cmd_unmount(args);
	else
		err = -EINVAL;

	kfree(input);
	return err ? err : count;
}

static const struct file_operations nt_dbg_control_fops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.write		= nt_dbg_control_write,
};

/* ---------------------------------------------------------- personality */

static ssize_t nt_dbg_pers_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct nt_task_ctx *ctx = nt_ctx_current();
	char buf[64];
	int n;

	n = scnprintf(buf, sizeof(buf), "flags 0x%x\ndrive %c\n",
		      nt_personality_get(), ctx ? ctx->cur_drive : '-');

	return simple_read_from_buffer(ubuf, count, ppos, buf, n);
}

static ssize_t nt_dbg_pers_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct nt_task_ctx *ctx;
	char *input;
	u32 flags;
	int err;

	input = nt_dbg_get_input(ubuf, count);
	if (IS_ERR(input))
		return PTR_ERR(input);

	/* "drive X" sets the current drive; anything else is a flag mask. */
	if (!strncmp(input, "drive ", 6)) {
		ctx = nt_ctx_current_or_create();
		err = ctx ? nt_ctx_set_current_drive(ctx, input[6]) : -ENOMEM;
		goto out;
	}

	err = kstrtou32(input, 0, &flags);
	if (!err)
		err = nt_personality_set(flags);

out:
	kfree(input);
	return err ? err : count;
}

static const struct file_operations nt_dbg_pers_fops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.read		= nt_dbg_pers_read,
	.write		= nt_dbg_pers_write,
};

/* ------------------------------------------------------------- seq files */

static int nt_dbg_volumes_show(struct seq_file *m, void *v)
{
	struct nt_namespace *ns = &init_nt_ns;
	struct nt_volume *vol;

	seq_printf(m, "computer %s\nvolumes %u\n", ns->computer_name,
		   ns->nr_volumes);

	spin_lock(&ns->lock);
	list_for_each_entry(vol, &ns->volumes, list)
		nt_volume_seq_show(m, vol);
	spin_unlock(&ns->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nt_dbg_volumes);

static int nt_dbg_cache_show(struct seq_file *m, void *v)
{
	nt_ci_cache_stats(m);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nt_dbg_cache);

int __init nt_debugfs_init(void)
{
	nt_debugfs_root = debugfs_create_dir(NTPERS_NAME, NULL);
	if (IS_ERR(nt_debugfs_root)) {
		nt_debugfs_root = NULL;
		return 0;	/* debugfs is optional */
	}

	debugfs_create_file("volumes", 0400, nt_debugfs_root, NULL,
			    &nt_dbg_volumes_fops);
	debugfs_create_file("cache", 0400, nt_debugfs_root, NULL,
			    &nt_dbg_cache_fops);
	debugfs_create_file("control", 0200, nt_debugfs_root, NULL,
			    &nt_dbg_control_fops);
	debugfs_create_file("parse", 0600, nt_debugfs_root, NULL,
			    &nt_dbg_parse_fops);
	debugfs_create_file("resolve", 0600, nt_debugfs_root, NULL,
			    &nt_dbg_resolve_fops);
	debugfs_create_file("personality", 0600, nt_debugfs_root, NULL,
			    &nt_dbg_pers_fops);
	debugfs_create_file("getinfo", 0600, nt_debugfs_root, NULL,
			    &nt_dbg_getinfo_fops);

	return 0;
}

void nt_debugfs_exit(void)
{
	debugfs_remove_recursive(nt_debugfs_root);
	nt_debugfs_root = NULL;
}
