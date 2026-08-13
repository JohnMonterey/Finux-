/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ntpers

#if !defined(_TRACE_NTPERS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NTPERS_H

#include <linux/tracepoint.h>
#include <linux/nt_personality.h>

TRACE_DEFINE_ENUM(NT_PATH_INVALID);
TRACE_DEFINE_ENUM(NT_PATH_DRIVE_ABSOLUTE);
TRACE_DEFINE_ENUM(NT_PATH_DRIVE_RELATIVE);
TRACE_DEFINE_ENUM(NT_PATH_ROOTED);
TRACE_DEFINE_ENUM(NT_PATH_RELATIVE);
TRACE_DEFINE_ENUM(NT_PATH_UNC);
TRACE_DEFINE_ENUM(NT_PATH_DEVICE);
TRACE_DEFINE_ENUM(NT_PATH_NT_OBJECT);

#define nt_show_path_type(type)						\
	__print_symbolic(type,						\
		{ NT_PATH_INVALID,	  "invalid" },			\
		{ NT_PATH_DRIVE_ABSOLUTE, "drive-absolute" },		\
		{ NT_PATH_DRIVE_RELATIVE, "drive-relative" },		\
		{ NT_PATH_ROOTED,	  "rooted" },			\
		{ NT_PATH_RELATIVE,	  "relative" },			\
		{ NT_PATH_UNC,		  "unc" },			\
		{ NT_PATH_DEVICE,	  "device" },			\
		{ NT_PATH_NT_OBJECT,	  "nt-object" })

#define nt_show_parse_flags(flags)					\
	__print_flags(flags, "|",					\
		{ NT_PARSE_VERBATIM,	  "VERBATIM" },			\
		{ NT_PARSE_TRAILING_SEP,  "TRAILING_SEP" },		\
		{ NT_PARSE_HAS_STREAM,	  "STREAM" },			\
		{ NT_PARSE_RESERVED_NAME, "RESERVED" },			\
		{ NT_PARSE_VOLUME_ROOT,	  "VOLUME_ROOT" },		\
		{ NT_PARSE_LONG_PATH,	  "LONG_PATH" },		\
		{ NT_PARSE_TRIMMED,	  "TRIMMED" })

/**
 * ntpath_parse - an NT/Win32 pathname was parsed
 *
 * The canonical form is recorded as well as the input, because the gap
 * between them is where most pathname bugs live.
 */
TRACE_EVENT(ntpath_parse,
	TP_PROTO(const char *input, const struct nt_path_parse *p, int err),

	TP_ARGS(input, p, err),

	TP_STRUCT__entry(
		__string(comm,	current->comm)
		__string(input,	input)
		__string(rel,	err ? "" : p->rel)
		__field(int,	type)
		__field(u32,	flags)
		__field(u8,	drive)
		__field(u16,	nr_components)
		__field(int,	err)
	),

	TP_fast_assign(
		__assign_str(comm);
		__assign_str(input);
		__assign_str(rel);
		__entry->type		= p->type;
		__entry->flags		= p->flags;
		__entry->drive		= p->drive;
		__entry->nr_components	= p->nr_components;
		__entry->err		= err;
	),

	TP_printk("process=%s input=\"%s\" type=%s drive=%c rel=\"%s\" comps=%u flags=%s err=%d",
		  __get_str(comm), __get_str(input),
		  nt_show_path_type(__entry->type),
		  __entry->drive ? __entry->drive : '-',
		  __get_str(rel), __entry->nr_components,
		  nt_show_parse_flags(__entry->flags), __entry->err)
);

/**
 * ntpath_resolve - an NT path was resolved to a Linux dentry
 */
TRACE_EVENT(ntpath_resolve,
	TP_PROTO(const struct nt_path_parse *p, u8 letter,
		 const struct dentry *dentry, int err),

	TP_ARGS(p, letter, dentry, err),

	TP_STRUCT__entry(
		__string(comm,	current->comm)
		__string(rel,	p->rel)
		__field(int,	type)
		__field(u8,	letter)
		__field(const void *, dentry)
		__field(u64,	ino)
		__field(int,	err)
	),

	TP_fast_assign(
		__assign_str(comm);
		__assign_str(rel);
		__entry->type	= p->type;
		__entry->letter	= letter;
		__entry->dentry	= dentry;
		__entry->ino	= (dentry && d_really_is_positive(dentry)) ?
				  d_inode(dentry)->i_ino : 0;
		__entry->err	= err;
	),

	TP_printk("process=%s type=%s volume=%c rel=\"%s\" resolved=%p ino=%llu err=%d",
		  __get_str(comm), nt_show_path_type(__entry->type),
		  __entry->letter ? __entry->letter : '-',
		  __get_str(rel), __entry->dentry, __entry->ino,
		  __entry->err)
);

/**
 * ntpath_ci_lookup - a case-insensitive component lookup was performed
 * @how: which tier answered; see fs/ntpers/casefold.c
 */
TRACE_EVENT(ntpath_ci_lookup,
	TP_PROTO(const char *name, unsigned int len, const char *how, int err),

	TP_ARGS(name, len, how, err),

	TP_STRUCT__entry(
		__string(how,	how)
		__dynamic_array(char, name, len + 1)
		__field(int,	err)
	),

	TP_fast_assign(
		__assign_str(how);
		memcpy(__get_dynamic_array(name), name, len);
		((char *)__get_dynamic_array(name))[len] = '\0';
		__entry->err = err;
	),

	TP_printk("name=\"%s\" via=%s err=%d",
		  __get_str(name), __get_str(how), __entry->err)
);

/**
 * ntvol_event - a volume appeared, changed letter, or went away
 */
TRACE_EVENT(ntvol_event,
	TP_PROTO(const char *action, u32 id, u8 letter, u32 flags),

	TP_ARGS(action, id, letter, flags),

	TP_STRUCT__entry(
		__string(action, action)
		__field(u32,	id)
		__field(u8,	letter)
		__field(u32,	flags)
	),

	TP_fast_assign(
		__assign_str(action);
		__entry->id	= id;
		__entry->letter	= letter;
		__entry->flags	= flags;
	),

	TP_printk("%s volume id=%u letter=%c flags=0x%x",
		  __get_str(action), __entry->id,
		  __entry->letter ? __entry->letter : '-', __entry->flags)
);

#endif /* _TRACE_NTPERS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
