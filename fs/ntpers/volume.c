// SPDX-License-Identifier: GPL-2.0
/*
 * NT volume layer.
 *
 * A volume is the binding between an NT-visible drive letter and a Linux
 * mount.  This is where "C:" stops being a string and becomes an object.
 *
 * Deliberately *not* duplicated here: anything the Linux VFS already
 * knows.  A volume holds a struct path, which pins a (vfsmount, dentry)
 * pair, and that is the whole of its relationship with storage.  What it
 * adds is the identity metadata NT expects a volume to have and Linux has
 * nowhere to put: a volume GUID, a 32-bit serial number, a label, and an
 * NT device name.
 *
 * Locking: nt_namespace::lock serialises all mutation.  Readers use RCU,
 * so nt_volume_lookup_letter() is safe from a pathname walk without
 * taking any lock.  Volumes are refcounted and freed after a grace
 * period.
 */

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/nt_personality.h>

#include "internal.h"

#include <trace/events/ntpers.h>

/*
 * Derive a Win32 volume serial number.
 *
 * Windows stores a 32-bit serial in the boot sector and reports it from
 * GetVolumeInformation.  We do not have one for an arbitrary Linux
 * filesystem, so fold the superblock UUID down to 32 bits when there is
 * one, and fall back to the device number otherwise.  Either way the
 * value is stable across boots for the same filesystem, which is the
 * property callers actually depend on.
 */
static u32 nt_volume_derive_serial(const struct super_block *sb)
{
	u32 serial = 0;
	int i;

	for (i = 0; i < sb->s_uuid_len; i++) {
		/* Rotate and mix, so every UUID byte contributes. */
		serial = (serial << 8) | (serial >> 24);
		serial ^= sb->s_uuid.b[i];
	}

	if (!serial)
		serial = new_encode_dev(sb->s_dev);
	if (!serial)
		serial = 0x4e544653;	/* "NTFS", so it is never zero */

	return serial;
}

/*
 * Derive a volume GUID.
 *
 * Windows names volumes \\?\Volume{GUID}\ independently of any drive
 * letter, and that name is expected to be stable.  When the backing
 * superblock has a UUID we use it directly; otherwise we generate one,
 * which is stable for the lifetime of the mount but not across boots.
 * That limitation is recorded in the volume's flags via the caller.
 */
static void nt_volume_derive_guid(struct nt_volume *vol,
				  const struct super_block *sb)
{
	if (sb->s_uuid_len == sizeof(vol->guid)) {
		memcpy(&vol->guid, &sb->s_uuid, sizeof(vol->guid));
		return;
	}

	generate_random_guid((unsigned char *)&vol->guid);
}

/*
 * Build the NT device name for a volume.
 *
 * Real NT numbers these by discovery order of the underlying partition,
 * as \Device\HarddiskVolume1, \Device\HarddiskVolume2 and so on.  We use
 * the volume's dense id for the same purpose: it is allocated in creation
 * order, so the first volume registered becomes HarddiskVolume1, which is
 * what a system volume normally is on Windows.
 */
static void nt_volume_build_device_name(struct nt_volume *vol)
{
	snprintf(vol->nt_device, sizeof(vol->nt_device),
		 "\\Device\\HarddiskVolume%u", vol->id);
}

/*
 * Report the backing filesystem name the way Win32 expects it.
 *
 * GetVolumeInformation returns things like "NTFS" and "FAT32"; there is
 * no Win32 vocabulary for "ext4".  Rather than lie about the format, we
 * report NTFS for filesystems that can carry the semantics the NT
 * personality needs, because that is the answer that makes Win32
 * software behave correctly, and pass through the real name otherwise.
 * The genuine Linux filesystem type is always available from
 * /proc/mounts and from the debugfs volume table.
 */
static void nt_volume_set_fs_name(struct nt_volume *vol,
				  const struct super_block *sb)
{
	/*
	 * Everything we can host the NT personality on supports the
	 * metadata we need through xattrs, so all of them present as NTFS
	 * to a Win32 caller.  This is a compatibility decision, not a
	 * claim about the on-disk format.
	 */
	strscpy(vol->fs_name, "NTFS", sizeof(vol->fs_name));

	if (sb->s_type && sb->s_type->name &&
	    !strcmp(sb->s_type->name, "vfat"))
		strscpy(vol->fs_name, "FAT32", sizeof(vol->fs_name));
}

/**
 * nt_volume_get - take a reference on a volume
 * @vol: the volume, may be NULL
 *
 * Returns @vol.
 */
struct nt_volume *nt_volume_get(struct nt_volume *vol)
{
	if (vol)
		refcount_inc(&vol->count);
	return vol;
}
EXPORT_SYMBOL_GPL(nt_volume_get);

static void nt_volume_free_rcu(struct rcu_head *head)
{
	struct nt_volume *vol = container_of(head, struct nt_volume, rcu);

	kmem_cache_free(nt_volume_cache, vol);
}

void nt_volume_free(struct nt_volume *vol)
{
	path_put(&vol->root);
	call_rcu(&vol->rcu, nt_volume_free_rcu);
}

/**
 * nt_volume_put - drop a reference on a volume
 * @vol: the volume, may be NULL
 */
void nt_volume_put(struct nt_volume *vol)
{
	if (vol && refcount_dec_and_test(&vol->count))
		nt_volume_free(vol);
}
EXPORT_SYMBOL_GPL(nt_volume_put);

/**
 * nt_volume_create - register a new volume in a namespace
 * @ns:     the namespace
 * @root:   backing path the volume is rooted at; a reference is taken
 * @letter: drive letter to assign, or 0 for none
 * @flags:  NT_VOL_* flags
 * @label:  volume label, or NULL
 *
 * The caller must hold a reference on @root for the duration of the call;
 * the volume takes its own.
 *
 * Returns the new volume with one reference held by the namespace and one
 * returned to the caller, or an ERR_PTR.
 */
struct nt_volume *nt_volume_create(struct nt_namespace *ns,
				   const struct path *root, u8 letter,
				   u32 flags, const char *label)
{
	struct nt_volume *vol;
	struct super_block *sb;
	int idx = -1;
	int err;

	if (!ns || !root || !root->dentry || !root->mnt)
		return ERR_PTR(-EINVAL);

	if (!d_is_dir(root->dentry))
		return ERR_PTR(-ENOTDIR);

	if (letter) {
		idx = nt_drive_index(letter);
		if (idx < 0)
			return ERR_PTR(-EINVAL);
	}

	vol = kmem_cache_zalloc(nt_volume_cache, GFP_KERNEL);
	if (!vol)
		return ERR_PTR(-ENOMEM);

	sb = root->dentry->d_sb;

	refcount_set(&vol->count, 1);
	INIT_LIST_HEAD(&vol->list);
	vol->root = *root;
	path_get(&vol->root);
	vol->flags = flags;
	vol->letter = letter ? nt_drive_upper(letter) : 0;
	vol->serial = nt_volume_derive_serial(sb);
	nt_volume_derive_guid(vol, sb);
	nt_volume_set_fs_name(vol, sb);

	/*
	 * Record whether the backing filesystem can resolve names
	 * case-insensitively by itself.  When it can, the NT resolver has
	 * nothing to do and the dcache handles case folding at full speed;
	 * when it cannot, casefold.c has to make up the difference.  See
	 * Documentation/filesystems/nt-personality.rst.
	 */
	if (nt_dir_is_native_ci(root->dentry))
		vol->flags |= NT_VOL_NATIVE_CI;

	if (label)
		strscpy(vol->label, label, sizeof(vol->label));
	else
		strscpy(vol->label, "", sizeof(vol->label));

	spin_lock(&ns->lock);

	if (idx >= 0 && rcu_dereference_protected(ns->drives[idx],
						  lockdep_is_held(&ns->lock))) {
		err = -EBUSY;
		goto out_unlock;
	}

	if ((flags & NT_VOL_SYSTEM) &&
	    rcu_dereference_protected(ns->system, lockdep_is_held(&ns->lock))) {
		err = -EEXIST;
		goto out_unlock;
	}

	vol->id = ++ns->next_id;
	nt_volume_build_device_name(vol);

	/* One reference for the namespace, one for the caller. */
	refcount_inc(&vol->count);
	list_add_tail(&vol->list, &ns->volumes);
	ns->nr_volumes++;

	if (idx >= 0)
		rcu_assign_pointer(ns->drives[idx], vol);
	if (flags & NT_VOL_SYSTEM)
		rcu_assign_pointer(ns->system, vol);

	spin_unlock(&ns->lock);

	trace_ntvol_event("created", vol->id, vol->letter, vol->flags);
	return vol;

out_unlock:
	spin_unlock(&ns->lock);
	path_put(&vol->root);
	kmem_cache_free(nt_volume_cache, vol);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(nt_volume_create);

/**
 * nt_volume_destroy - unregister a volume
 * @ns:  the namespace
 * @vol: the volume
 *
 * Removes @vol from the namespace and drops the namespace's reference.
 * Callers that still hold their own reference keep a usable volume until
 * they drop it, but it is no longer reachable by lookup.
 */
void nt_volume_destroy(struct nt_namespace *ns, struct nt_volume *vol)
{
	int idx;

	if (!ns || !vol)
		return;

	spin_lock(&ns->lock);

	if (vol->flags & NT_VOL_DYING) {
		spin_unlock(&ns->lock);
		return;
	}
	vol->flags |= NT_VOL_DYING;

	idx = nt_drive_index(vol->letter);
	if (idx >= 0 && rcu_dereference_protected(ns->drives[idx],
						  lockdep_is_held(&ns->lock)) == vol)
		rcu_assign_pointer(ns->drives[idx], NULL);

	if (rcu_dereference_protected(ns->system,
				      lockdep_is_held(&ns->lock)) == vol)
		rcu_assign_pointer(ns->system, NULL);

	list_del_init(&vol->list);
	ns->nr_volumes--;

	spin_unlock(&ns->lock);

	trace_ntvol_event("destroyed", vol->id, vol->letter, vol->flags);
	nt_volume_put(vol);
}
EXPORT_SYMBOL_GPL(nt_volume_destroy);

/**
 * nt_volume_lookup_letter - find the volume mounted at a drive letter
 * @ns:     the namespace
 * @letter: drive letter, either case
 *
 * Returns a referenced volume, or NULL.  Safe to call from a pathname
 * walk: takes no locks.
 */
struct nt_volume *nt_volume_lookup_letter(struct nt_namespace *ns, u8 letter)
{
	struct nt_volume *vol;
	int idx;

	if (!ns)
		return NULL;

	idx = nt_drive_index(letter);
	if (idx < 0)
		return NULL;

	rcu_read_lock();
	vol = rcu_dereference(ns->drives[idx]);
	if (vol && !refcount_inc_not_zero(&vol->count))
		vol = NULL;
	rcu_read_unlock();

	return vol;
}
EXPORT_SYMBOL_GPL(nt_volume_lookup_letter);

/**
 * nt_volume_get_system - find the system volume, normally C:
 * @ns: the namespace
 *
 * Returns a referenced volume, or NULL if no volume has been designated.
 */
struct nt_volume *nt_volume_get_system(struct nt_namespace *ns)
{
	struct nt_volume *vol;

	if (!ns)
		return NULL;

	rcu_read_lock();
	vol = rcu_dereference(ns->system);
	if (vol && !refcount_inc_not_zero(&vol->count))
		vol = NULL;
	rcu_read_unlock();

	return vol;
}
EXPORT_SYMBOL_GPL(nt_volume_get_system);

/**
 * nt_volume_lookup_id - find a volume by its internal id
 * @ns: the namespace
 * @id: the id
 *
 * Returns a referenced volume, or NULL.
 */
struct nt_volume *nt_volume_lookup_id(struct nt_namespace *ns, u32 id)
{
	struct nt_volume *vol, *found = NULL;

	if (!ns)
		return NULL;

	spin_lock(&ns->lock);
	list_for_each_entry(vol, &ns->volumes, list) {
		if (vol->id == id) {
			found = nt_volume_get(vol);
			break;
		}
	}
	spin_unlock(&ns->lock);

	return found;
}
EXPORT_SYMBOL_GPL(nt_volume_lookup_id);

/**
 * nt_volume_lookup_device - find a volume by NT device or GUID name
 * @ns:   the namespace
 * @name: device name without a leading separator, e.g.
 *        "Device\HarddiskVolume1" or "Volume{...}"
 * @len:  length of @name
 *
 * Resolves the two names by which NT identifies a volume independently of
 * any drive letter.  Matching is case-insensitive, as it is in the NT
 * object namespace.
 *
 * Returns a referenced volume, or NULL.
 */
struct nt_volume *nt_volume_lookup_device(struct nt_namespace *ns,
					  const char *name, size_t len)
{
	struct nt_volume *vol, *found = NULL;
	char guid_name[UUID_STRING_LEN + 10];
	size_t devlen;

	if (!ns || !name || !len)
		return NULL;

	spin_lock(&ns->lock);
	list_for_each_entry(vol, &ns->volumes, list) {
		/* "\Device\HarddiskVolume1", with or without the leading \ */
		devlen = strlen(vol->nt_device);
		if (len == devlen &&
		    !strncasecmp(name, vol->nt_device, len)) {
			found = nt_volume_get(vol);
			break;
		}
		if (len == devlen - 1 &&
		    !strncasecmp(name, vol->nt_device + 1, len)) {
			found = nt_volume_get(vol);
			break;
		}

		/* "Volume{01234567-89ab-cdef-0123-456789abcdef}" */
		snprintf(guid_name, sizeof(guid_name), "Volume{%pUl}",
			 &vol->guid);
		if (len == strlen(guid_name) &&
		    !strncasecmp(name, guid_name, len)) {
			found = nt_volume_get(vol);
			break;
		}
	}
	spin_unlock(&ns->lock);

	return found;
}
EXPORT_SYMBOL_GPL(nt_volume_lookup_device);

/**
 * nt_volume_first_free_letter - pick an unassigned drive letter
 * @ns: the namespace
 *
 * Returns an uppercase letter, or 0 if all 26 are taken.  Starts at D:,
 * matching Windows, which reserves A: and B: for floppy drives and gives
 * C: to the system volume.
 */
u8 nt_volume_first_free_letter(struct nt_namespace *ns)
{
	u8 letter = 0;
	int i;

	if (!ns)
		return 0;

	spin_lock(&ns->lock);
	for (i = nt_drive_index('D'); i < NT_NR_DRIVES; i++) {
		if (!rcu_dereference_protected(ns->drives[i],
					       lockdep_is_held(&ns->lock))) {
			letter = 'A' + i;
			break;
		}
	}
	spin_unlock(&ns->lock);

	return letter;
}
EXPORT_SYMBOL_GPL(nt_volume_first_free_letter);

/**
 * nt_volume_assign_letter - move a volume to a drive letter
 * @ns:     the namespace
 * @vol:    the volume
 * @letter: new letter, or 0 to unassign
 *
 * Returns 0, -EINVAL for a letter outside A-Z, or -EBUSY if the letter is
 * already taken by another volume.
 */
int nt_volume_assign_letter(struct nt_namespace *ns, struct nt_volume *vol,
			    u8 letter)
{
	int old_idx, new_idx = -1;
	int err = 0;

	if (!ns || !vol)
		return -EINVAL;

	if (letter) {
		new_idx = nt_drive_index(letter);
		if (new_idx < 0)
			return -EINVAL;
	}

	spin_lock(&ns->lock);

	if (vol->flags & NT_VOL_DYING) {
		err = -ENOENT;
		goto out;
	}

	if (new_idx >= 0) {
		struct nt_volume *cur;

		cur = rcu_dereference_protected(ns->drives[new_idx],
						lockdep_is_held(&ns->lock));
		if (cur && cur != vol) {
			err = -EBUSY;
			goto out;
		}
	}

	old_idx = nt_drive_index(vol->letter);
	if (old_idx >= 0 && old_idx != new_idx &&
	    rcu_dereference_protected(ns->drives[old_idx],
				      lockdep_is_held(&ns->lock)) == vol)
		rcu_assign_pointer(ns->drives[old_idx], NULL);

	vol->letter = letter ? nt_drive_upper(letter) : 0;
	if (new_idx >= 0)
		rcu_assign_pointer(ns->drives[new_idx], vol);

out:
	spin_unlock(&ns->lock);

	if (!err)
		trace_ntvol_event("relettered", vol->id, vol->letter,
				  vol->flags);
	return err;
}
EXPORT_SYMBOL_GPL(nt_volume_assign_letter);

/**
 * nt_volume_set_system - designate the system volume
 * @ns:  the namespace
 * @vol: the volume, or NULL to clear
 *
 * The system volume is the one that holds \Windows.  Windows always
 * presents it as C:, so designating a volume also moves it to C: if it is
 * not there already.
 *
 * Returns 0, or -EBUSY if C: belongs to a different volume.
 */
int nt_volume_set_system(struct nt_namespace *ns, struct nt_volume *vol)
{
	struct nt_volume *old;
	int err;

	if (!ns)
		return -EINVAL;

	if (!vol) {
		spin_lock(&ns->lock);
		old = rcu_dereference_protected(ns->system,
						lockdep_is_held(&ns->lock));
		if (old)
			old->flags &= ~NT_VOL_SYSTEM;
		rcu_assign_pointer(ns->system, NULL);
		spin_unlock(&ns->lock);
		return 0;
	}

	err = nt_volume_assign_letter(ns, vol, 'C');
	if (err)
		return err;

	spin_lock(&ns->lock);
	old = rcu_dereference_protected(ns->system,
					lockdep_is_held(&ns->lock));
	if (old && old != vol)
		old->flags &= ~NT_VOL_SYSTEM;
	vol->flags |= NT_VOL_SYSTEM;
	rcu_assign_pointer(ns->system, vol);
	spin_unlock(&ns->lock);

	trace_ntvol_event("system", vol->id, vol->letter, vol->flags);
	return 0;
}
EXPORT_SYMBOL_GPL(nt_volume_set_system);

/**
 * nt_volume_seq_show - render one volume for debugfs
 * @m:   the seq_file
 * @vol: the volume
 */
void nt_volume_seq_show(struct seq_file *m, struct nt_volume *vol)
{
	struct super_block *sb = vol->root.dentry->d_sb;

	seq_printf(m, "id=%u letter=%c: nt=%s guid={%pUl} serial=%08X fs=%s backing=%s label=\"%s\"%s%s%s\n",
		   vol->id,
		   vol->letter ? vol->letter : '-',
		   vol->nt_device,
		   &vol->guid,
		   vol->serial,
		   vol->fs_name,
		   sb->s_type ? sb->s_type->name : "?",
		   vol->label,
		   (vol->flags & NT_VOL_SYSTEM) ? " system" : "",
		   (vol->flags & NT_VOL_NATIVE_CI) ? " native-ci" : "",
		   (vol->flags & NT_VOL_READONLY) ? " ro" : "");
}
EXPORT_SYMBOL_GPL(nt_volume_seq_show);
