// SPDX-License-Identifier: GPL-2.0
/*
 * NT file system calls: create, open, read, write, close and query.
 *
 * This is the NtCreateFile / NtReadFile / ... surface of the personality.
 * Following the split fs/binfmt_pe.c uses - a pure pe_parse_headers() below an
 * I/O-driving load_pe_binary() - every call here is two functions:
 *
 *   - a *core* (nt_file_create() and its kin) that works purely on in-kernel
 *     data: a decoded UTF-8 path, kernel buffers, a struct nt_task_ctx.  It
 *     does the real work - nt_create(), the handle table, VFS read/write - and
 *     returns an NTSTATUS.  These are what the KUnit suite drives, against a
 *     tmpfs fixture, with no user memory in sight;
 *
 *   - a *wrapper* (NtCreateFile() and its kin) that is the future syscall entry
 *     point.  It copies the NT ABI structures in and out of user memory,
 *     transcodes the UTF-16 ObjectName to the UTF-8 path the core wants, and
 *     calls the core.  Wiring the `syscall` instruction to these wrappers is a
 *     separate stage's job; nothing dispatches to them yet.  See the note on
 *     each wrapper.
 *
 * NTSTATUS is the currency.  A core returns STATUS_SUCCESS or a 0xC0000xxx
 * error; the errno the VFS layer speaks is translated once, in
 * nt_errno_to_status(), so the mapping lives in exactly one place.
 */

#include <linux/build_bug.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/nls.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/nt_personality.h>

#include "internal.h"

/*
 * The ABI structures must match the 64-bit Windows layout byte for byte, or a
 * native PE handed one of these will read the wrong field.  Assert the sizes
 * and the offsets that the padding is there to produce, so a mistake in the
 * struct definition is a build failure rather than a runtime corruption.
 */
static_assert(sizeof(struct nt_unicode_string) == 16);
static_assert(offsetof(struct nt_unicode_string, buffer) == 8);
static_assert(sizeof(struct nt_object_attributes) == 48);
static_assert(offsetof(struct nt_object_attributes, root_directory) == 8);
static_assert(offsetof(struct nt_object_attributes, object_name) == 16);
static_assert(offsetof(struct nt_object_attributes, attributes) == 24);
static_assert(offsetof(struct nt_object_attributes, security_descriptor) == 32);
static_assert(offsetof(struct nt_object_attributes, security_qos) == 40);
static_assert(sizeof(struct nt_io_status_block) == 16);
static_assert(offsetof(struct nt_io_status_block, information) == 8);
static_assert(sizeof(struct nt_file_basic_information) == 40);
static_assert(sizeof(struct nt_file_standard_information) == 24);
static_assert(offsetof(struct nt_file_standard_information, end_of_file) == 8);

/*
 * Largest slice a single NtReadFile / NtWriteFile bounces through a kernel
 * buffer.  A caller asking for more gets a short transfer and the count it
 * really moved, which NtReadFile is allowed to return; looping on a short read
 * is already how a caller drives these calls.
 */
#define NT_IO_MAX_XFER		(8U * 1024 * 1024)

/*
 * Translate the errno the VFS and the create/share layers speak into the
 * NTSTATUS an NT caller expects.  nt_create() collapses a delete-pending file
 * into -ENOENT, so that surfaces as STATUS_OBJECT_NAME_NOT_FOUND rather than
 * STATUS_DELETE_PENDING - a documented consequence of that layer, not a loss
 * here.
 */
static u32 nt_errno_to_status(int err)
{
	switch (err) {
	case 0:			return STATUS_SUCCESS;
	case -EEXIST:		return STATUS_OBJECT_NAME_COLLISION;
	case -ENOENT:		return STATUS_OBJECT_NAME_NOT_FOUND;
	case -ENOTDIR:		return STATUS_OBJECT_PATH_NOT_FOUND;
	case -EISDIR:		return STATUS_FILE_IS_A_DIRECTORY;
	case -EACCES:
	case -EPERM:		return STATUS_ACCESS_DENIED;
	case -EBUSY:		return STATUS_SHARING_VIOLATION;
	case -ENOMEM:		return STATUS_NO_MEMORY;
	case -ENAMETOOLONG:	return STATUS_NAME_TOO_LONG;
	case -ENOSPC:		return STATUS_DISK_FULL;
	case -EFBIG:
	case -EOPNOTSUPP:	return STATUS_NOT_SUPPORTED;
	case -EINVAL:		return STATUS_INVALID_PARAMETER;
	default:		return STATUS_UNSUCCESSFUL;
	}
}

/*
 * Map an NtCreateFile CreateDisposition (the NT kernel numbering) onto the
 * internal NT_DISPOSITION_* (the Win32 CreateFile numbering nt_create() uses).
 * FILE_SUPERSEDE has no equivalent - it resets the file and reports
 * FILE_SUPERSEDED, which nt_create() does not do - so it is refused with
 * STATUS_NOT_IMPLEMENTED rather than silently treated as an overwrite.
 */
static u32 nt_map_disposition(u32 create_disposition, u32 *internal)
{
	switch (create_disposition) {
	case NT_FILE_OPEN:
		*internal = NT_DISPOSITION_OPEN_EXISTING;
		return STATUS_SUCCESS;
	case NT_FILE_CREATE:
		*internal = NT_DISPOSITION_CREATE_NEW;
		return STATUS_SUCCESS;
	case NT_FILE_OPEN_IF:
		*internal = NT_DISPOSITION_OPEN_ALWAYS;
		return STATUS_SUCCESS;
	case NT_FILE_OVERWRITE:
		*internal = NT_DISPOSITION_TRUNCATE_EXISTING;
		return STATUS_SUCCESS;
	case NT_FILE_OVERWRITE_IF:
		*internal = NT_DISPOSITION_CREATE_ALWAYS;
		return STATUS_SUCCESS;
	case NT_FILE_SUPERSEDE:
		return STATUS_NOT_IMPLEMENTED;
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

/*
 * Map the CreateOptions bits this layer honours onto the internal NT_CREATE_*.
 * FILE_NON_DIRECTORY_FILE has no NT_CREATE_* bit; the wrapper passes it as the
 * forbid_dir argument to nt_file_create() instead.  Every other bit selects a
 * caching or completion behaviour this scaffold does not model, and is
 * accepted and ignored.
 */
static u32 nt_map_create_options(u32 options)
{
	u32 out = 0;

	if (options & NT_FILE_DIRECTORY_FILE)
		out |= NT_CREATE_DIRECTORY;
	if (options & NT_FILE_DELETE_ON_CLOSE)
		out |= NT_CREATE_DELETE_ON_CLOSE;
	if (options & NT_FILE_OPEN_REPARSE_POINT)
		out |= NT_CREATE_OPEN_REPARSE;
	return out;
}

/*
 * The internal NT_RESULT_* uses a private numbering; the IoStatusBlock reports
 * the real Windows FILE_* value.  Translate on the way out.
 */
static u64 nt_result_to_information(u32 result)
{
	switch (result) {
	case NT_RESULT_CREATED:		return NT_FILE_CREATED;
	case NT_RESULT_OVERWRITTEN:	return NT_FILE_OVERWRITTEN;
	case NT_RESULT_OPENED:
	default:			return NT_FILE_OPENED;
	}
}

/* Does the granted access include a data-read right? */
static bool nt_access_can_read(u32 access)
{
	if (access & NT_ACCESS_GENERIC_ALL)
		return true;
	return access & (NT_ACCESS_GENERIC_READ | NT_ACCESS_FILE_READ_DATA);
}

/* Does the granted access include a data-write (or append) right? */
static bool nt_access_can_write(u32 access)
{
	if (access & NT_ACCESS_GENERIC_ALL)
		return true;
	return access & (NT_ACCESS_GENERIC_WRITE | NT_ACCESS_FILE_WRITE_DATA |
			 NT_ACCESS_FILE_APPEND_DATA);
}

/**
 * nt_file_create - create or open a file and name it with a handle
 * @ctx:         the calling task's NT context; the handle lands in its table
 * @name:        NT or Win32 pathname, a NUL-terminated UTF-8 kernel string
 * @req:         what to do; see struct nt_create_req
 * @forbid_dir:  refuse a directory (FILE_NON_DIRECTORY_FILE)
 * @handle:      filled in with the new handle, or NT_NULL_HANDLE on failure
 * @information: filled in with the IoStatusBlock.Information result (FILE_*)
 *
 * Drives nt_create() and installs the resulting file object in @ctx's handle
 * table.  On any failure nothing is installed and the object is released.
 *
 * Returns STATUS_SUCCESS or an NTSTATUS error.
 */
u32 nt_file_create(struct nt_task_ctx *ctx, const char *name,
		   const struct nt_create_req *req, bool forbid_dir,
		   u32 *handle, u64 *information)
{
	struct nt_open_result res;
	u32 st;
	int err;

	if (!ctx || !name || !req || !handle || !information)
		return STATUS_INVALID_PARAMETER;

	*handle = NT_NULL_HANDLE;
	*information = 0;

	err = nt_create(ctx, name, req, &res);
	if (err)
		return nt_errno_to_status(err);

	/*
	 * FILE_NON_DIRECTORY_FILE: the caller demanded a non-directory but the
	 * name resolved to one.  nt_create() has no bit for this, so enforce it
	 * here, closing the handle it just handed back.
	 */
	if (forbid_dir && d_is_dir(res.handle->path.dentry)) {
		nt_close(res.handle);
		return STATUS_FILE_IS_A_DIRECTORY;
	}

	st = nt_handle_alloc(&ctx->handles, res.handle, handle);
	if (st != STATUS_SUCCESS) {
		nt_close(res.handle);
		return st;
	}

	*information = nt_result_to_information(res.result);
	return STATUS_SUCCESS;
}
EXPORT_SYMBOL_GPL(nt_file_create);

/*
 * Resolve an NT ByteOffset to a position and say whether it is the handle's
 * own file position.  The "use current position" sentinel is reported through
 * @use_fpos; a genuine negative offset is rejected by the caller.
 */
static loff_t nt_resolve_offset(s64 offset, bool *use_fpos)
{
	*use_fpos = (u64)offset == NT_FILE_USE_FILE_POINTER_POSITION;
	return offset;
}

/**
 * nt_file_read - read from an open handle
 * @ctx:        the calling task's NT context
 * @handle:     the handle to read from
 * @offset:     byte offset, or the NT_FILE_USE_FILE_POINTER_POSITION sentinel
 * @buf:        kernel buffer to fill
 * @len:        bytes to read
 * @bytes_read: filled in with the number of bytes actually read
 *
 * Reads through the handle's open struct file (or, for a named-stream handle,
 * through the stream's xattr store).  A read that lands at or past end of file
 * returns STATUS_END_OF_FILE with @bytes_read set to zero, exactly as
 * NtReadFile does.
 *
 * Returns STATUS_SUCCESS, STATUS_END_OF_FILE, or an NTSTATUS error.
 */
u32 nt_file_read(struct nt_task_ctx *ctx, u32 handle, s64 offset,
		 void *buf, u32 len, u32 *bytes_read)
{
	struct nt_open *open;
	bool use_fpos;
	loff_t pos;
	ssize_t n;
	u32 st;

	if (!ctx || !buf || !bytes_read)
		return STATUS_INVALID_PARAMETER;
	*bytes_read = 0;

	st = nt_handle_lookup(&ctx->handles, handle, &open);
	if (st != STATUS_SUCCESS)
		return st;

	if (!nt_access_can_read(open->access))
		return STATUS_ACCESS_DENIED;

	pos = nt_resolve_offset(offset, &use_fpos);
	if (!use_fpos && pos < 0)
		return STATUS_INVALID_PARAMETER;

	/* A named stream keeps its bytes in an xattr, reached through the path. */
	if (open->stream) {
		if (use_fpos)
			pos = 0;
		n = nt_stream_read(&open->path, open->stream, open->stream_len,
				   pos, buf, len);
		if (n < 0)
			return nt_errno_to_status(n);
		if (n == 0 && len > 0)
			return STATUS_END_OF_FILE;
		*bytes_read = n;
		return STATUS_SUCCESS;
	}

	if (!open->file)
		return STATUS_INVALID_DEVICE_REQUEST;
	if (S_ISDIR(file_inode(open->file)->i_mode))
		return STATUS_INVALID_DEVICE_REQUEST;

	if (use_fpos)
		pos = open->file->f_pos;

	n = kernel_read(open->file, buf, len, &pos);
	if (n < 0)
		return nt_errno_to_status(n);

	/* A current-position read advances the handle's position by what it moved. */
	if (use_fpos)
		open->file->f_pos = pos;

	if (n == 0 && len > 0)
		return STATUS_END_OF_FILE;

	*bytes_read = n;
	return STATUS_SUCCESS;
}
EXPORT_SYMBOL_GPL(nt_file_read);

/**
 * nt_file_write - write to an open handle
 * @ctx:           the calling task's NT context
 * @handle:        the handle to write to
 * @offset:        byte offset, or a NT_FILE_* position sentinel
 * @buf:           kernel buffer to write
 * @len:           bytes to write
 * @bytes_written: filled in with the number of bytes actually written
 *
 * Writes through the handle's open struct file (or the stream's xattr store).
 * The NT_FILE_WRITE_TO_END_OF_FILE sentinel appends at the current end of the
 * file; NT_FILE_USE_FILE_POINTER_POSITION writes at the handle's position.
 *
 * Returns STATUS_SUCCESS or an NTSTATUS error.
 */
u32 nt_file_write(struct nt_task_ctx *ctx, u32 handle, s64 offset,
		  const void *buf, u32 len, u32 *bytes_written)
{
	struct nt_open *open;
	bool use_fpos, append;
	loff_t pos;
	ssize_t n;
	u32 st;

	if (!ctx || !buf || !bytes_written)
		return STATUS_INVALID_PARAMETER;
	*bytes_written = 0;

	st = nt_handle_lookup(&ctx->handles, handle, &open);
	if (st != STATUS_SUCCESS)
		return st;

	if (!nt_access_can_write(open->access))
		return STATUS_ACCESS_DENIED;

	pos = nt_resolve_offset(offset, &use_fpos);
	append = (u64)offset == NT_FILE_WRITE_TO_END_OF_FILE;
	if (!use_fpos && !append && pos < 0)
		return STATUS_INVALID_PARAMETER;

	if (open->stream) {
		if (append) {
			ssize_t sz = nt_stream_size(&open->path, open->stream,
						    open->stream_len);

			pos = sz > 0 ? sz : 0;	/* -ENODATA (empty) starts at 0 */
		} else if (use_fpos) {
			pos = 0;
		}
		n = nt_stream_write(&open->path, open->stream, open->stream_len,
				    pos, buf, len);
		if (n < 0)
			return nt_errno_to_status(n);
		*bytes_written = n;
		return STATUS_SUCCESS;
	}

	if (!open->file)
		return STATUS_INVALID_DEVICE_REQUEST;
	if (S_ISDIR(file_inode(open->file)->i_mode))
		return STATUS_INVALID_DEVICE_REQUEST;

	if (append)
		pos = i_size_read(file_inode(open->file));
	else if (use_fpos)
		pos = open->file->f_pos;

	n = kernel_write(open->file, buf, len, &pos);
	if (n < 0)
		return nt_errno_to_status(n);

	if (use_fpos || append)
		open->file->f_pos = pos;

	*bytes_written = n;
	return STATUS_SUCCESS;
}
EXPORT_SYMBOL_GPL(nt_file_write);

/**
 * nt_file_close - close a handle
 * @ctx:    the calling task's NT context
 * @handle: the handle to close
 *
 * Returns STATUS_SUCCESS, or STATUS_INVALID_HANDLE for a handle that names
 * nothing (including one already closed).
 */
u32 nt_file_close(struct nt_task_ctx *ctx, u32 handle)
{
	if (!ctx)
		return STATUS_INVALID_PARAMETER;

	return nt_handle_close(&ctx->handles, handle);
}
EXPORT_SYMBOL_GPL(nt_file_close);

/* Fill a FILE_STANDARD_INFORMATION from a queried file. */
static void nt_fill_standard_info(struct nt_file_standard_information *si,
				  const struct nt_open *open,
				  const struct nt_file_info *info)
{
	memset(si, 0, sizeof(*si));
	si->allocation_size = info->alloc_size;
	si->end_of_file = info->size;
	si->number_of_links = info->nlink;
	si->delete_pending = open->delete_on_close ? 1 : 0;
	si->directory = (info->attributes & NT_FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* Fill a FILE_BASIC_INFORMATION from a queried file. */
static void nt_fill_basic_info(struct nt_file_basic_information *bi,
			       const struct nt_file_info *info)
{
	memset(bi, 0, sizeof(*bi));
	bi->creation_time = nt_time_from_timespec(&info->creation);
	bi->last_access_time = nt_time_from_timespec(&info->last_access);
	bi->last_write_time = nt_time_from_timespec(&info->last_write);
	bi->change_time = nt_time_from_timespec(&info->change);
	bi->file_attributes = info->attributes;
}

/**
 * nt_file_query_information - answer NtQueryInformationFile for a handle
 * @ctx:        the calling task's NT context
 * @handle:     the handle to query
 * @info_class: a FILE_INFORMATION_CLASS value (NT_FILEINFO_*)
 * @buf:        kernel buffer for the information structure
 * @len:        size of @buf, the caller's declared Length
 * @out_len:    filled in with the number of bytes written
 *
 * Fills @buf with the requested class of file information.  A buffer shorter
 * than the class's structure is STATUS_INFO_LENGTH_MISMATCH, with nothing
 * written; an unhandled class is STATUS_NOT_IMPLEMENTED.  For a named-stream
 * handle the base file is described, not the stream - a documented limitation.
 *
 * Returns STATUS_SUCCESS or an NTSTATUS error.
 */
u32 nt_file_query_information(struct nt_task_ctx *ctx, u32 handle,
			      u32 info_class, void *buf, u32 len, u32 *out_len)
{
	struct nt_file_info info;
	struct nt_open *open;
	u32 st;
	int err;

	if (!ctx || !buf || !out_len)
		return STATUS_INVALID_PARAMETER;
	*out_len = 0;

	st = nt_handle_lookup(&ctx->handles, handle, &open);
	if (st != STATUS_SUCCESS)
		return st;

	switch (info_class) {
	case NT_FILEINFO_STANDARD:
		if (len < sizeof(struct nt_file_standard_information))
			return STATUS_INFO_LENGTH_MISMATCH;
		break;
	case NT_FILEINFO_BASIC:
		if (len < sizeof(struct nt_file_basic_information))
			return STATUS_INFO_LENGTH_MISMATCH;
		break;
	default:
		return STATUS_NOT_IMPLEMENTED;
	}

	err = nt_query_file_info(&open->path, open->volume, &info);
	if (err)
		return nt_errno_to_status(err);

	if (info_class == NT_FILEINFO_STANDARD) {
		nt_fill_standard_info(buf, open, &info);
		*out_len = sizeof(struct nt_file_standard_information);
	} else {
		nt_fill_basic_info(buf, &info);
		*out_len = sizeof(struct nt_file_basic_information);
	}
	return STATUS_SUCCESS;
}
EXPORT_SYMBOL_GPL(nt_file_query_information);

/*
 * ---------------------------------------------------------------------
 * User-copy entry wrappers - the NT system service ABI
 * ---------------------------------------------------------------------
 *
 * Each wrapper below is the entry point a later dispatch stage will bind a
 * syscall number to; none is called yet.  They marshal the ABI in and out of
 * user memory and drive the cores above, and are kept thin on purpose - the
 * behaviour is in the cores, which the KUnit suite can reach.
 */

/*
 * Transcode an NtCreateFile ObjectName into the UTF-8 path nt_create() wants.
 *
 * The ObjectName is a UNICODE_STRING: a byte length and a user pointer to that
 * many UTF-16LE code units, not necessarily NUL terminated.  Read it in,
 * convert with utf16s_to_utf8s() - UTF16_LITTLE_ENDIAN, because a native NT
 * caller stores little-endian whatever the CPU - and NUL terminate.  A BMP
 * code unit is at most three UTF-8 bytes, and a surrogate pair (two units) is
 * four bytes, i.e. fewer, so 3 * units + 1 is a bound that never truncates;
 * nt_create()'s own parser then applies the real path-length limit.
 *
 * On success *out is a kvmalloc()ed NUL-terminated string the caller frees
 * with kvfree().
 */
static u32 nt_copy_object_name(const struct nt_unicode_string *us, char **out)
{
	unsigned int units, out_max;
	u16 *wbuf;
	char *path;
	int n;

	*out = NULL;

	if (us->length == 0 || (us->length & 1))
		return STATUS_OBJECT_NAME_INVALID;
	units = us->length / 2;
	if (units > NT_MAX_NT_PATH)
		return STATUS_NAME_TOO_LONG;

	wbuf = kmalloc(us->length, GFP_KERNEL);
	if (!wbuf)
		return STATUS_NO_MEMORY;
	if (copy_from_user(wbuf, u64_to_user_ptr(us->buffer), us->length)) {
		kfree(wbuf);
		return STATUS_ACCESS_VIOLATION;
	}

	out_max = units * 3 + 1;
	path = kvmalloc(out_max, GFP_KERNEL);
	if (!path) {
		kfree(wbuf);
		return STATUS_NO_MEMORY;
	}

	n = utf16s_to_utf8s((wchar_t *)wbuf, units, UTF16_LITTLE_ENDIAN,
			    path, out_max - 1);
	kfree(wbuf);
	path[n] = '\0';
	if (n == 0) {
		kvfree(path);
		return STATUS_OBJECT_NAME_INVALID;
	}

	*out = path;
	return STATUS_SUCCESS;
}

/* Write an IoStatusBlock back to user memory. */
static int nt_put_io_status(struct nt_io_status_block __user *iosb, u32 status,
			    u64 information)
{
	struct nt_io_status_block sb;

	memset(&sb, 0, sizeof(sb));
	sb.status = status;
	sb.information = information;
	return copy_to_user(iosb, &sb, sizeof(sb)) ? -EFAULT : 0;
}

/**
 * NtCreateFile - create or open a file (NT system service)
 * @file_handle:       out: the new handle (an 8-byte HANDLE slot)
 * @desired_access:    ACCESS_MASK the caller wants
 * @object_attributes: the OBJECT_ATTRIBUTES, carrying the ObjectName
 * @io_status_block:   out: status and Information (FILE_CREATED / FILE_OPENED)
 * @allocation_size:   preallocation hint (accepted and ignored)
 * @file_attributes:   NT_FILE_ATTRIBUTE_* to stamp on a created file
 * @share_access:      NT_SHARE_* this open grants others
 * @create_disposition: FILE_OPEN / FILE_CREATE / ... (NT numbering)
 * @create_options:    FILE_DIRECTORY_FILE / FILE_DELETE_ON_CLOSE / ...
 * @ea_buffer:         extended attributes (accepted and ignored)
 * @ea_length:         length of @ea_buffer (accepted and ignored)
 *
 * Entry wrapper for the NtCreateFile system service.  Dispatch from the
 * `syscall` frame is a later stage's work; nothing calls this yet.
 *
 * RootDirectory-relative opens are not implemented: a non-zero RootDirectory
 * is refused with STATUS_NOT_IMPLEMENTED rather than silently ignored.
 * Extended attributes and the allocation-size hint are accepted and ignored.
 *
 * Returns an NTSTATUS.
 */
u32 NtCreateFile(u64 __user *file_handle, u32 desired_access,
		 struct nt_object_attributes __user *object_attributes,
		 struct nt_io_status_block __user *io_status_block,
		 s64 __user *allocation_size, u32 file_attributes,
		 u32 share_access, u32 create_disposition, u32 create_options,
		 void __user *ea_buffer, u32 ea_length)
{
	struct nt_object_attributes oa;
	struct nt_unicode_string us;
	struct nt_create_req req;
	struct nt_task_ctx *ctx;
	u32 st, disposition, handle;
	u64 information;
	char *name;

	ctx = nt_ctx_current_or_create();
	if (!ctx)
		return STATUS_NO_MEMORY;

	if (!file_handle || !object_attributes || !io_status_block)
		return STATUS_INVALID_PARAMETER;

	if (copy_from_user(&oa, object_attributes, sizeof(oa)))
		return STATUS_ACCESS_VIOLATION;

	/* RootDirectory-relative opens are future work; refuse them honestly. */
	if (oa.root_directory != 0)
		return STATUS_NOT_IMPLEMENTED;
	if (oa.object_name == 0)
		return STATUS_INVALID_PARAMETER;
	if (copy_from_user(&us, u64_to_user_ptr(oa.object_name), sizeof(us)))
		return STATUS_ACCESS_VIOLATION;

	st = nt_map_disposition(create_disposition, &disposition);
	if (st != STATUS_SUCCESS)
		return st;

	st = nt_copy_object_name(&us, &name);
	if (st != STATUS_SUCCESS)
		return st;

	req.disposition = disposition;
	req.access = desired_access;
	req.share = share_access;
	req.options = nt_map_create_options(create_options);
	req.attributes = file_attributes & NT_FILE_ATTRIBUTE_VALID;
	req.resolve_flags = (oa.attributes & NT_OBJ_CASE_INSENSITIVE) ?
			    NT_RESOLVE_CASE_INSENSITIVE : 0;

	st = nt_file_create(ctx, name, &req,
			    create_options & NT_FILE_NON_DIRECTORY_FILE,
			    &handle, &information);
	kvfree(name);
	if (st != STATUS_SUCCESS)
		return st;

	/*
	 * Publish the results.  If a copy faults after the handle exists, close
	 * it: a caller that never received the handle must not be left with one
	 * open in its table.
	 */
	if (put_user((u64)handle, file_handle) ||
	    nt_put_io_status(io_status_block, STATUS_SUCCESS, information)) {
		nt_file_close(ctx, handle);
		return STATUS_ACCESS_VIOLATION;
	}
	return STATUS_SUCCESS;
}

/**
 * NtOpenFile - open an existing file (NT system service)
 * @file_handle:       out: the new handle
 * @desired_access:    ACCESS_MASK the caller wants
 * @object_attributes: the OBJECT_ATTRIBUTES, carrying the ObjectName
 * @io_status_block:   out: status and Information
 * @share_access:      NT_SHARE_* this open grants others
 * @open_options:      FILE_* open options
 *
 * Entry wrapper for the NtOpenFile system service, which is NtCreateFile
 * restricted to opening: it is expressed here as exactly that, a create with
 * disposition FILE_OPEN and none of the create-only arguments.
 *
 * Returns an NTSTATUS.
 */
u32 NtOpenFile(u64 __user *file_handle, u32 desired_access,
	       struct nt_object_attributes __user *object_attributes,
	       struct nt_io_status_block __user *io_status_block,
	       u32 share_access, u32 open_options)
{
	return NtCreateFile(file_handle, desired_access, object_attributes,
			    io_status_block, NULL, 0, share_access,
			    NT_FILE_OPEN, open_options, NULL, 0);
}

/*
 * Read the ByteOffset argument shared by NtReadFile and NtWriteFile.  A NULL
 * pointer means "use the handle's current position", the sentinel NT records
 * as FILE_USE_FILE_POINTER_POSITION.
 */
static int nt_fetch_byte_offset(s64 __user *byte_offset, s64 *offset)
{
	if (!byte_offset) {
		*offset = (s64)NT_FILE_USE_FILE_POINTER_POSITION;
		return 0;
	}
	return copy_from_user(offset, byte_offset, sizeof(*offset)) ? -EFAULT : 0;
}

/**
 * NtReadFile - read from a file handle (NT system service)
 * @file_handle:     the handle to read from (an 8-byte HANDLE, by value)
 * @event:           completion event (accepted and ignored; synchronous only)
 * @apc_routine:     completion APC (accepted and ignored)
 * @apc_context:     completion APC context (accepted and ignored)
 * @io_status_block: out: status and the byte count in Information
 * @buffer:          user buffer to fill
 * @length:          bytes to read
 * @byte_offset:     explicit offset, or NULL for the handle's position
 * @key:             byte-range-lock key (accepted and ignored)
 *
 * Entry wrapper for the NtReadFile system service; dispatch is a later stage's
 * work.  A single call transfers at most NT_IO_MAX_XFER bytes and reports what
 * it moved, so a caller reads a large region by looping, which NtReadFile's
 * short-read contract already allows.
 *
 * Returns an NTSTATUS; STATUS_END_OF_FILE with a zero Information at EOF.
 */
u32 NtReadFile(u64 file_handle, u64 event, u64 apc_routine, u64 apc_context,
	       struct nt_io_status_block __user *io_status_block,
	       void __user *buffer, u32 length, s64 __user *byte_offset,
	       u32 __user *key)
{
	struct nt_task_ctx *ctx;
	void *kbuf;
	s64 offset;
	u32 st, n, xfer;

	ctx = nt_ctx_current_or_create();
	if (!ctx)
		return STATUS_NO_MEMORY;
	if (!io_status_block || (length && !buffer))
		return STATUS_INVALID_PARAMETER;

	if (nt_fetch_byte_offset(byte_offset, &offset))
		return STATUS_ACCESS_VIOLATION;

	xfer = min(length, NT_IO_MAX_XFER);
	kbuf = kvmalloc(xfer ? xfer : 1, GFP_KERNEL);
	if (!kbuf)
		return STATUS_NO_MEMORY;

	st = nt_file_read(ctx, (u32)file_handle, offset, kbuf, xfer, &n);
	if (st == STATUS_SUCCESS && n && copy_to_user(buffer, kbuf, n))
		st = STATUS_ACCESS_VIOLATION;
	kvfree(kbuf);

	if (st == STATUS_SUCCESS || st == STATUS_END_OF_FILE) {
		u64 information = st == STATUS_SUCCESS ? n : 0;

		if (nt_put_io_status(io_status_block, st, information))
			return STATUS_ACCESS_VIOLATION;
	}
	return st;
}

/**
 * NtWriteFile - write to a file handle (NT system service)
 * @file_handle:     the handle to write to (an 8-byte HANDLE, by value)
 * @event:           completion event (accepted and ignored; synchronous only)
 * @apc_routine:     completion APC (accepted and ignored)
 * @apc_context:     completion APC context (accepted and ignored)
 * @io_status_block: out: status and the byte count in Information
 * @buffer:          user buffer to write
 * @length:          bytes to write
 * @byte_offset:     explicit offset, NULL for the position, or the append
 *                   sentinel FILE_WRITE_TO_END_OF_FILE
 * @key:             byte-range-lock key (accepted and ignored)
 *
 * Entry wrapper for the NtWriteFile system service; dispatch is a later
 * stage's work.  As with NtReadFile a single call moves at most NT_IO_MAX_XFER
 * bytes and reports the count.
 *
 * Returns an NTSTATUS.
 */
u32 NtWriteFile(u64 file_handle, u64 event, u64 apc_routine, u64 apc_context,
		struct nt_io_status_block __user *io_status_block,
		const void __user *buffer, u32 length, s64 __user *byte_offset,
		u32 __user *key)
{
	struct nt_task_ctx *ctx;
	void *kbuf;
	s64 offset;
	u32 st, n, xfer;

	ctx = nt_ctx_current_or_create();
	if (!ctx)
		return STATUS_NO_MEMORY;
	if (!io_status_block || (length && !buffer))
		return STATUS_INVALID_PARAMETER;

	if (nt_fetch_byte_offset(byte_offset, &offset))
		return STATUS_ACCESS_VIOLATION;

	xfer = min(length, NT_IO_MAX_XFER);
	kbuf = kvmalloc(xfer ? xfer : 1, GFP_KERNEL);
	if (!kbuf)
		return STATUS_NO_MEMORY;

	if (xfer && copy_from_user(kbuf, buffer, xfer)) {
		kvfree(kbuf);
		return STATUS_ACCESS_VIOLATION;
	}

	st = nt_file_write(ctx, (u32)file_handle, offset, kbuf, xfer, &n);
	kvfree(kbuf);

	if (st == STATUS_SUCCESS) {
		if (nt_put_io_status(io_status_block, st, n))
			return STATUS_ACCESS_VIOLATION;
	}
	return st;
}

/**
 * NtClose - close a handle (NT system service)
 * @handle: the handle to close (an 8-byte HANDLE, by value)
 *
 * Entry wrapper for the NtClose system service; dispatch is a later stage's
 * work.
 *
 * Returns an NTSTATUS.
 */
u32 NtClose(u64 handle)
{
	struct nt_task_ctx *ctx = nt_ctx_current_or_create();

	if (!ctx)
		return STATUS_NO_MEMORY;

	return nt_file_close(ctx, (u32)handle);
}

/**
 * NtQueryInformationFile - query file information (NT system service)
 * @file_handle:      the handle to query (an 8-byte HANDLE, by value)
 * @io_status_block:  out: status and the byte count in Information
 * @file_information: user buffer for the information structure
 * @length:           size of @file_information
 * @info_class:       a FILE_INFORMATION_CLASS value (NT_FILEINFO_*)
 *
 * Entry wrapper for the NtQueryInformationFile system service; dispatch is a
 * later stage's work.  FileBasicInformation and FileStandardInformation are
 * served; every other class returns STATUS_NOT_IMPLEMENTED.
 *
 * Returns an NTSTATUS.
 */
u32 NtQueryInformationFile(u64 file_handle,
			   struct nt_io_status_block __user *io_status_block,
			   void __user *file_information, u32 length,
			   u32 info_class)
{
	/* The larger of the two structures this call can return. */
	u8 kbuf[sizeof(struct nt_file_basic_information)];
	struct nt_task_ctx *ctx;
	u32 st, out_len;

	ctx = nt_ctx_current_or_create();
	if (!ctx)
		return STATUS_NO_MEMORY;
	if (!io_status_block || (length && !file_information))
		return STATUS_INVALID_PARAMETER;

	st = nt_file_query_information(ctx, (u32)file_handle, info_class, kbuf,
				      min(length, (u32)sizeof(kbuf)), &out_len);
	if (st != STATUS_SUCCESS)
		return st;

	if (copy_to_user(file_information, kbuf, out_len))
		return STATUS_ACCESS_VIOLATION;
	if (nt_put_io_status(io_status_block, STATUS_SUCCESS, out_len))
		return STATUS_ACCESS_VIOLATION;
	return STATUS_SUCCESS;
}

/**
 * NtTerminateProcess - terminate a process (NT system service)
 * @process_handle: the process to terminate
 * @exit_status:    NTSTATUS to report as the exit code
 *
 * The minimal NtTerminateProcess an NT-mode process needs to exit without
 * ever making a Linux system call.  Only self-termination is implemented:
 * @process_handle must be the current-process pseudo-handle (HANDLE)-1 or
 * the null handle (0), both of which name the caller.  A handle to any other
 * process returns STATUS_NOT_IMPLEMENTED - honest about what this scaffold
 * does rather than silently ignoring the target.
 *
 * Self-termination ends the whole thread group through the kernel's
 * group-exit path, exactly as the exit_group() system call does, and does
 * not return.  Only the low eight bits of @exit_status survive into the
 * Linux wait status, the same truncation every Linux process exit undergoes;
 * preserving the full 32-bit NTSTATUS a Win32 GetExitCodeProcess would see
 * is future work.
 *
 * Returns STATUS_NOT_IMPLEMENTED for a non-self handle; does not return on
 * self-termination.
 */
u32 NtTerminateProcess(u64 process_handle, u32 exit_status)
{
	if (process_handle != 0 && process_handle != NT_CURRENT_PROCESS)
		return STATUS_NOT_IMPLEMENTED;

	do_group_exit((exit_status & 0xff) << 8);

	/* do_group_exit() does not return; keep the compiler content. */
	return STATUS_SUCCESS;
}
