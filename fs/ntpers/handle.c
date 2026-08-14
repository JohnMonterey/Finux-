// SPDX-License-Identifier: GPL-2.0
/*
 * NT handle table - the Object Manager's per-process handle namespace.
 *
 * An NT process never touches an object directly; it holds a handle, and
 * every operation names the object through it.  This file is that
 * indirection: the map from a handle to the struct nt_open - the file
 * object nt_create() produced - that it stands for, together with the
 * allocation and lifetime rules the NT Object Manager gives it.
 *
 * Handle values follow the Windows convention.  They are multiples of four
 * starting at four: the low two bits are tag bits Win32 keeps for itself,
 * so the kernel only ever hands out values with those bits clear, and the
 * value is the table index shifted up by two.  Zero is the null handle and
 * is never allocated, which is why a zeroed field is safely "no handle".
 * The pseudo-handles Windows layers on top (GetCurrentProcess() and its
 * kind, the negative values) live at the far end of the range; the
 * allocator is capped well below them, so a later stage can add them
 * without renumbering anything here.
 *
 * The table is an xarray.  An NT handle table is a sparse, integer-keyed
 * map that has to allocate its own keys, look them up under RCU on the hot
 * path, and stay consistent under threads racing to open and close - which
 * is exactly what an allocating xarray provides, down to the internal
 * locking, so the table carries no lock of its own.  XA_FLAGS_ALLOC1
 * reserves index 0, so the null handle can never be allocated.
 *
 * Ownership.  A handle owns the object it names: nt_handle_alloc() takes
 * over the nt_open on success, and nt_handle_close() and
 * nt_handle_table_destroy() are the only paths that call nt_close(), each
 * exactly once.  A failed allocation hands the object back to the caller
 * untouched.
 *
 * Threading and teardown.  The table lives inside struct nt_task_ctx, so it
 * is shared by every thread of a process - they share one fs_struct, hence
 * one context - and it is destroyed on the last reference to that context,
 * which is process exit.  Destruction closes every handle still open, so an
 * exiting process cannot leak the mount and dentry references its open
 * files pin.
 *
 * Lookup returns a borrowed pointer.  An nt_open has no reference count of
 * its own yet, so a looked-up object is valid only until that handle is
 * closed; the syscall layer that will call this must not use a looked-up
 * object across a concurrent close of the same handle.  Giving nt_open a
 * reference count - the job NT does with ObReferenceObjectByHandle() - is
 * future work for that layer.
 */

#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/nt_personality.h>

#include "internal.h"

/*
 * Windows caps a process at 16,777,216 (2^24) open handles.  Honouring the
 * same ceiling bounds the xarray and keeps the largest handle value
 * (index * 4, so 64M) far below the range reserved for pseudo-handles.
 */
#define NT_HANDLE_MAX_INDEX	(16 * 1024 * 1024)

/*
 * A handle value is its table index shifted up by two.  A value could only
 * have come from this table if it is non-zero and has its two tag bits
 * clear; anything else maps to index 0, which is never a live index because
 * XA_FLAGS_ALLOC1 holds it reserved, and so reads back as no handle.
 */
static unsigned long nt_handle_to_index(u32 handle)
{
	if (handle == NT_NULL_HANDLE || (handle & 3))
		return 0;
	return handle >> 2;
}

static u32 nt_index_to_handle(unsigned long index)
{
	return (u32)(index << 2);
}

/**
 * nt_handle_table_init - initialise an NT handle table in place
 * @ht: the table
 *
 * The table is embedded in its owner rather than separately allocated, and
 * an xarray needs no allocation to become usable, so this cannot fail.  It
 * must be paired with nt_handle_table_destroy().
 */
void nt_handle_table_init(struct nt_handle_table *ht)
{
	/*
	 * XA_FLAGS_ALLOC1 makes xa_alloc() start at index 1 and hold index 0
	 * reserved, so handle 0 is never allocated.  xa_load() is RCU and the
	 * mutating xa_alloc()/xa_erase() take the xarray's own lock, so the
	 * table needs no further locking.
	 */
	xa_init_flags(&ht->xa, XA_FLAGS_ALLOC1);
}
EXPORT_SYMBOL_GPL(nt_handle_table_init);

/**
 * nt_handle_table_destroy - close every handle and release the table
 * @ht: the table
 *
 * Called on the last reference to the owning context, when no other thread
 * can reach the table.  Every handle still open is closed, so the nt_open
 * objects - and the mount and dentry references they hold - are released
 * rather than leaked at process exit.
 */
void nt_handle_table_destroy(struct nt_handle_table *ht)
{
	struct nt_open *open;
	unsigned long index;

	/*
	 * xa_for_each() holds no lock across iterations, so nt_close() - which
	 * can sleep, and can unlink a delete-on-close file - is safe to call
	 * from the loop body.  Nothing races us here, so the entries are
	 * closed in place and the whole tree is freed in one xa_destroy().
	 */
	xa_for_each(&ht->xa, index, open)
		nt_close(open);

	xa_destroy(&ht->xa);
}
EXPORT_SYMBOL_GPL(nt_handle_table_destroy);

/**
 * nt_handle_alloc - install an open object in the table and name it
 * @ht:     the table
 * @open:   the file object to install; the table takes ownership on success
 * @handle: filled in with the new handle value on success, and with
 *          NT_NULL_HANDLE otherwise
 *
 * Allocates the lowest free handle, which follows the Windows habit of
 * reusing the smallest available value.  On success the table owns @open
 * and will nt_close() it when the handle is closed or the table is
 * destroyed; on failure @open is untouched and still belongs to the caller.
 *
 * Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER for a NULL argument,
 * STATUS_TOO_MANY_OPENED_FILES when the table is full, or STATUS_NO_MEMORY.
 */
u32 nt_handle_alloc(struct nt_handle_table *ht, struct nt_open *open,
		    u32 *handle)
{
	u32 index;
	int err;

	if (!ht || !open || !handle)
		return STATUS_INVALID_PARAMETER;

	*handle = NT_NULL_HANDLE;

	err = xa_alloc(&ht->xa, &index, open, XA_LIMIT(1, NT_HANDLE_MAX_INDEX),
		       GFP_KERNEL);
	switch (err) {
	case 0:
		*handle = nt_index_to_handle(index);
		return STATUS_SUCCESS;
	case -EBUSY:
		return STATUS_TOO_MANY_OPENED_FILES;
	default:	/* -ENOMEM */
		return STATUS_NO_MEMORY;
	}
}
EXPORT_SYMBOL_GPL(nt_handle_alloc);

/**
 * nt_handle_lookup - resolve a handle to the object it names
 * @ht:     the table
 * @handle: the handle value
 * @open:   filled in with the object on success, and with NULL otherwise
 *
 * The returned pointer is borrowed: it is valid only until the handle is
 * closed, because an nt_open has no reference count of its own yet.
 *
 * Returns STATUS_SUCCESS, STATUS_INVALID_HANDLE if @handle is malformed or
 * names nothing, or STATUS_INVALID_PARAMETER for a NULL argument.
 */
u32 nt_handle_lookup(struct nt_handle_table *ht, u32 handle,
		     struct nt_open **open)
{
	unsigned long index;
	struct nt_open *found;

	if (!ht || !open)
		return STATUS_INVALID_PARAMETER;

	*open = NULL;

	index = nt_handle_to_index(handle);
	if (!index || index > NT_HANDLE_MAX_INDEX)
		return STATUS_INVALID_HANDLE;

	found = xa_load(&ht->xa, index);
	if (!found)
		return STATUS_INVALID_HANDLE;

	*open = found;
	return STATUS_SUCCESS;
}
EXPORT_SYMBOL_GPL(nt_handle_lookup);

/**
 * nt_handle_close - close a handle and release the object it named
 * @ht:     the table
 * @handle: the handle value
 *
 * Removes the handle from the table and nt_close()s the object.  The
 * removal is atomic, so two threads racing to close the same handle cannot
 * both reach nt_close(): one erases the entry and closes it, the other
 * finds nothing and gets STATUS_INVALID_HANDLE.
 *
 * Returns STATUS_SUCCESS, STATUS_INVALID_HANDLE if @handle is malformed or
 * already closed, or STATUS_INVALID_PARAMETER for a NULL table.
 */
u32 nt_handle_close(struct nt_handle_table *ht, u32 handle)
{
	unsigned long index;
	struct nt_open *open;

	if (!ht)
		return STATUS_INVALID_PARAMETER;

	index = nt_handle_to_index(handle);
	if (!index || index > NT_HANDLE_MAX_INDEX)
		return STATUS_INVALID_HANDLE;

	open = xa_erase(&ht->xa, index);
	if (!open)
		return STATUS_INVALID_HANDLE;

	nt_close(open);
	return STATUS_SUCCESS;
}
EXPORT_SYMBOL_GPL(nt_handle_close);
