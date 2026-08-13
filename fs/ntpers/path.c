// SPDX-License-Identifier: GPL-2.0
/*
 * NT/Win32 pathname parser.
 *
 * This file is deliberately free of VFS references.  It turns a byte
 * string into a classified, normalised description of what the caller
 * asked for; deciding what that description resolves to is the job of
 * resolve.c.  Keeping the two apart is what lets the parser be unit
 * tested and lets a future Win32 personality reuse it for paths it never
 * intends to open (GetFullPathName, PathCanonicalize, and friends).
 *
 * Which normalisation rules apply is a layering question that matters:
 *
 *   - Collapsing "." and "..", collapsing repeated separators, trimming
 *     trailing dots and spaces, and applying MAX_PATH are *Win32* rules,
 *     implemented by RtlGetFullPathName_U before a path ever reaches the
 *     NT kernel.  A "\\?\" prefix tells Win32 to skip all of them.
 *
 *   - Rejecting '/' as a filename character, rejecting NUL, and the
 *     255-character component limit are *filesystem* rules.
 *
 *   - "CON", "NUL", "COM1" and the rest are *Win32* device names.  NTFS
 *     will happily store a file called CON; it is CreateFileW that
 *     refuses to look at the disk.  We flag them and let the caller
 *     decide, rather than failing here.
 *
 * The parser writes its output into a caller-supplied buffer and never
 * allocates, so there is no per-component allocation on the hot path.
 */

#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/nt_personality.h>

#include "internal.h"

/*
 * Characters Win32 refuses in a filename.  ':' is excluded here because
 * it is handled separately: it introduces a stream name, and whether that
 * is legal depends on the caller.
 */
static bool nt_char_is_illegal(unsigned char c)
{
	if (c < 0x20)
		return true;
	switch (c) {
	case '<':
	case '>':
	case '"':
	case '|':
	case '?':
	case '*':
		return true;
	default:
		return false;
	}
}

/* The kernel has no memrchr(); find the last '/' in a known-length run. */
static char *nt_last_slash(char *start, size_t len)
{
	while (len--) {
		if (start[len] == '/')
			return start + len;
	}
	return NULL;
}

/**
 * nt_path_type_name - human readable name for a path classification
 * @type: the classification
 *
 * Used by tracing and debugfs.
 */
const char *nt_path_type_name(enum nt_path_type type)
{
	static const char * const names[] = {
		[NT_PATH_INVALID]	 = "invalid",
		[NT_PATH_DRIVE_ABSOLUTE] = "drive-absolute",
		[NT_PATH_DRIVE_RELATIVE] = "drive-relative",
		[NT_PATH_ROOTED]	 = "rooted",
		[NT_PATH_RELATIVE]	 = "relative",
		[NT_PATH_UNC]		 = "unc",
		[NT_PATH_DEVICE]	 = "device",
		[NT_PATH_NT_OBJECT]	 = "nt-object",
	};

	if ((unsigned int)type >= ARRAY_SIZE(names) || !names[type])
		return "unknown";
	return names[type];
}
EXPORT_SYMBOL_GPL(nt_path_type_name);

/*
 * Split a ":stream[:$TYPE]" suffix off the final component.
 *
 * Windows accepts three shapes:
 *	"file:name"		-> named $DATA stream
 *	"file:name:$DATA"	-> the same thing, spelled out
 *	"file::$DATA"		-> the unnamed default stream, i.e. "file"
 *
 * On success *@len is shortened to cover only the filename and the stream
 * details are stored in @p.  Returns 1 if a colon was present, 0 if not,
 * or a negative errno.  A trailing type suffix naming something other
 * than $DATA is accepted and reported; only $DATA can hold
 * caller-supplied bytes, which is a decision for the layer above.
 */
static int nt_split_stream(struct nt_path_parse *p, const char *comp,
			   size_t *len)
{
	const char *colon, *end = comp + *len;
	const char *sname, *stype;
	size_t snamelen, stypelen;

	colon = memchr(comp, ':', *len);
	if (!colon)
		return 0;

	sname = colon + 1;
	snamelen = end - sname;

	/* A second colon introduces the attribute type. */
	stype = memchr(sname, ':', snamelen);
	if (stype) {
		snamelen = stype - sname;
		stype++;
		stypelen = end - stype;

		if (stypelen == 5 && !memcmp(stype, "$DATA", 5))
			p->stream_type = NT_STREAM_TYPE_DATA;
		else if (stypelen == 17 &&
			 !memcmp(stype, "$INDEX_ALLOCATION", 17))
			p->stream_type = NT_STREAM_TYPE_INDEX_ALLOCATION;
		else if (stypelen == 7 && !memcmp(stype, "$BITMAP", 7))
			p->stream_type = NT_STREAM_TYPE_BITMAP;
		else
			return -EINVAL;
	} else {
		p->stream_type = NT_STREAM_TYPE_DATA;
	}

	if (snamelen > NT_MAX_COMPONENT)
		return -ENAMETOOLONG;

	*len = colon - comp;

	if (snamelen == 0) {
		/*
		 * "file::$DATA" names the unnamed default stream, which is
		 * the file itself.  Report no stream so callers do not have
		 * to special-case it.
		 */
		p->stream = NULL;
		p->stream_len = 0;
		p->stream_type = 0;
		return 1;
	}

	p->stream = sname;
	p->stream_len = snamelen;
	p->flags |= NT_PARSE_HAS_STREAM;
	return 1;
}

/*
 * Trim trailing dots and spaces from a component, the way Win32 does.
 *
 * This is a Win32 normalisation rule, not a filesystem rule: NTFS can
 * store "foo." perfectly well, and a \\?\ path reaches it.  Callers have
 * already dealt with components made entirely of dots, so a zero-length
 * result here means the component was only spaces, which Win32 rejects.
 */
static size_t nt_trim_component(struct nt_path_parse *p, const char *comp,
				size_t len)
{
	size_t orig = len;

	while (len > 0 && (comp[len - 1] == '.' || comp[len - 1] == ' '))
		len--;

	if (len != orig)
		p->flags |= NT_PARSE_TRIMMED;
	return len;
}

/*
 * Append one component to the canonical output, handling "." and "..".
 *
 * @out is the running write position inside p->buf; it is advanced in
 * place.  Components are emitted '/'-separated with no leading or
 * trailing separator.
 */
static int nt_emit_component(struct nt_path_parse *p, char **out,
			     const char *comp, size_t len, bool verbatim)
{
	char *w = *out;
	size_t avail;

	if (!verbatim) {
		size_t dots = 0;

		while (dots < len && comp[dots] == '.')
			dots++;

		if (dots == len && len > 0) {
			char *slash;

			/* "." keeps the current directory. */
			if (len == 1)
				return 0;
			/*
			 * "..." and longer runs of dots are a Win32 syntax
			 * error; only "." and ".." are relative names.
			 */
			if (len > 2)
				return -EINVAL;

			/*
			 * ".." pops the previous component.  Popping past
			 * the root is not an error in Win32: "C:\..\Windows"
			 * is "C:\Windows".
			 */
			if (w == p->rel)
				return 0;
			slash = nt_last_slash(p->rel, w - p->rel);
			*out = slash ? slash : p->rel;
			p->nr_components--;
			return 0;
		}

		len = nt_trim_component(p, comp, len);
		/* A component of nothing but spaces is not a name. */
		if (len == 0)
			return -EINVAL;
	}

	if (len == 0)
		return 0;
	if (len > NT_MAX_COMPONENT)
		return -ENAMETOOLONG;

	/*
	 * No component reaches here with a colon still in it.  On the
	 * final component the caller has already split off any stream
	 * suffix; on any other component, and on the final one when the
	 * caller disabled stream parsing, a colon is a syntax error rather
	 * than a filename character.
	 */
	if (memchr(comp, ':', len))
		return -EINVAL;

	/* One byte for the separator, one for the terminating NUL. */
	avail = p->buf + p->buf_size - w;
	if (len + 2 > avail)
		return -ENAMETOOLONG;

	if (w != p->rel)
		*w++ = '/';
	memcpy(w, comp, len);
	w += len;

	*out = w;
	p->nr_components++;
	return 0;
}

/*
 * Walk the component part of a path and build the canonical form.
 *
 * @name/@len cover everything after any prefix (drive, UNC root, device)
 * has been consumed.  Both '\' and '/' are accepted as separators, as
 * Win32 does.
 */
static int nt_parse_components(struct nt_path_parse *p, const char *name,
			       size_t len, bool verbatim, bool allow_stream)
{
	const char *pos = name, *end = name + len;
	char *out = p->rel;
	int err;

	while (pos < end) {
		const char *comp, *scan;
		size_t clen, i;
		bool is_last;

		if (nt_is_sep(*pos)) {
			/*
			 * Win32 collapses repeated separators; NT, reached
			 * through a verbatim path, treats an empty name as
			 * a syntax error.
			 */
			if (verbatim && pos != name && nt_is_sep(pos[-1]))
				return -EINVAL;
			pos++;
			continue;
		}

		comp = pos;
		while (pos < end && !nt_is_sep(*pos))
			pos++;
		clen = pos - comp;

		/* Is this the last component that carries a name? */
		is_last = true;
		for (scan = pos; scan < end; scan++) {
			if (!nt_is_sep(*scan)) {
				is_last = false;
				break;
			}
		}

		for (i = 0; i < clen; i++) {
			if (nt_char_is_illegal(comp[i]))
				return -EINVAL;
		}

		if (is_last && allow_stream) {
			err = nt_split_stream(p, comp, &clen);
			if (err < 0)
				return err;
			/*
			 * "C:\dir\:stream" and "C:\dir\::$DATA" have no
			 * filename for the stream to belong to.
			 */
			if (err > 0 && clen == 0)
				return -EINVAL;
		}

		err = nt_emit_component(p, &out, comp, clen, verbatim);
		if (err)
			return err;
	}

	*out = '\0';
	p->rel_len = out - p->rel;
	if (p->rel_len == 0)
		p->flags |= NT_PARSE_VOLUME_ROOT;
	return 0;
}

/*
 * Parse "\\server\share\rest" (@name starts after the "\\").
 *
 * A UNC path naming only a server is not a path to a file, but it is a
 * legal thing to name, so we accept it and let the resolver refuse.
 */
static int nt_parse_unc(struct nt_path_parse *p, const char *name, size_t len,
			bool verbatim)
{
	const char *pos = name, *end = name + len;
	const char *server, *share;

	p->type = NT_PATH_UNC;

	while (pos < end && nt_is_sep(*pos))
		pos++;

	server = pos;
	while (pos < end && !nt_is_sep(*pos))
		pos++;
	p->unc_server = server;
	p->unc_server_len = pos - server;
	if (!p->unc_server_len)
		return -EINVAL;

	while (pos < end && nt_is_sep(*pos))
		pos++;

	share = pos;
	while (pos < end && !nt_is_sep(*pos))
		pos++;
	p->unc_share = share;
	p->unc_share_len = pos - share;

	return nt_parse_components(p, pos, end - pos, verbatim, true);
}

/*
 * Consume an NT Object Manager prefix.
 *
 * Two forms reach us:
 *
 *	\??\...		the per-session DOS device directory.  This is what
 *			Win32's "\\?\" is translated into, and it is where
 *			"C:" actually lives, as a symbolic link object
 *			pointing at a device.
 *	\Device\...	the real device directory.
 *
 * We report the device name and leave the rest to the resolver, which is
 * the only layer that knows which volume a device name belongs to.
 */
static int nt_parse_nt_object(struct nt_path_parse *p, const char *name,
			      size_t len)
{
	const char *pos = name, *end = name + len;
	const char *dev;

	p->type = NT_PATH_NT_OBJECT;
	p->flags |= NT_PARSE_VERBATIM;

	while (pos < end && nt_is_sep(*pos))
		pos++;

	/*
	 * "\??\" is a directory in the object namespace, so the element
	 * after it is a DOS device name: "\??\C:\Windows".
	 */
	if (end - pos >= 3 && pos[0] == '?' && pos[1] == '?' &&
	    nt_is_sep(pos[2])) {
		pos += 3;

		if (end - pos >= 2 && isalpha(pos[0]) && pos[1] == ':') {
			if (end - pos > 2 && !nt_is_sep(pos[2]))
				return -EINVAL;
			p->type = NT_PATH_DRIVE_ABSOLUTE;
			p->drive = nt_drive_upper(pos[0]);
			return nt_parse_components(p, pos + 2,
						   end - (pos + 2), true,
						   true);
		}
		if (end - pos >= 3 && !strncasecmp(pos, "UNC", 3) &&
		    (end - pos == 3 || nt_is_sep(pos[3])))
			return nt_parse_unc(p, pos + 3, end - (pos + 3),
					    true);

		/* "\??\SomeDevice\..." - one element of device name. */
		dev = pos;
		while (pos < end && !nt_is_sep(*pos))
			pos++;
		if (pos == dev)
			return -EINVAL;
		p->device = dev;
		p->device_len = pos - dev;
		return nt_parse_components(p, pos, end - pos, true, true);
	}

	/*
	 * "\Device\HarddiskVolume1\Windows": the device name is two
	 * object-namespace elements, "Device" and the volume name, so keep
	 * them together and report them as one device name.
	 */
	{
		const char *scan = pos;
		int elements = 0;

		while (scan < end && elements < 2) {
			while (scan < end && !nt_is_sep(*scan))
				scan++;
			elements++;
			if (scan < end && elements < 2)
				scan++;
		}

		p->device = name;
		p->device_len = scan - name;
		return nt_parse_components(p, scan, end - scan, true, true);
	}
}

/**
 * nt_path_parse - parse an NT or Win32 pathname
 * @name:	the pathname; need not be NUL terminated
 * @len:	length of @name in bytes
 * @flags:	NT_PARSE_F_* input flags
 * @buf:	scratch buffer the result points into
 * @buf_size:	size of @buf
 * @out:	parse result, filled in on success
 *
 * Classifies @name, applies the normalisation appropriate to its form,
 * and produces a canonical '/'-separated relative path in @buf that the
 * resolver can hand to the VFS.
 *
 * @buf must stay alive for as long as @out is used; every string in @out
 * points into it or into @name.
 *
 * Returns 0 on success, or a negative errno.  -EINVAL means the path is
 * syntactically invalid, -ENAMETOOLONG that it does not fit @buf or
 * exceeds a component or Win32 limit, -ENOENT that it is empty.
 */
int nt_path_parse(const char *name, size_t len, u32 flags,
		  char *buf, size_t buf_size, struct nt_path_parse *out)
{
	bool verbatim = flags & NT_PARSE_F_VERBATIM;
	bool allow_stream = !(flags & NT_PARSE_F_NO_STREAM);
	const char *end;
	int err;

	if (!name || !buf || buf_size < 2 || !out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->buf = buf;
	out->buf_size = buf_size;
	out->rel = buf;
	out->type = NT_PATH_INVALID;
	buf[0] = '\0';

	if (verbatim)
		out->flags |= NT_PARSE_VERBATIM;

	if (len == 0)
		return -ENOENT;
	if (memchr(name, '\0', len))
		return -EINVAL;
	if (len > NT_MAX_NT_PATH)
		return -ENAMETOOLONG;

	end = name + len;

	/* A trailing separator means the caller is naming a directory. */
	if (nt_is_sep(end[-1]))
		out->flags |= NT_PARSE_TRAILING_SEP;

	if (len >= 2 && nt_is_sep(name[0]) && nt_is_sep(name[1])) {
		/*
		 * "\\?\" and "\\.\" are Win32 escapes into the NT object
		 * namespace.  They differ in that "\\?\" suppresses
		 * normalisation while "\\.\" does not.
		 */
		bool win32_escape = len >= 4 && nt_is_sep(name[3]) &&
				    (name[2] == '?' || name[2] == '.');

		if (win32_escape) {
			const char *rest = name + 4;
			size_t restlen = end - rest;

			if (name[2] == '?') {
				verbatim = true;
				out->flags |= NT_PARSE_VERBATIM;
			}

			/* "\\?\C:\Windows" */
			if (restlen >= 2 && isalpha(rest[0]) &&
			    rest[1] == ':') {
				if (restlen > 2 && !nt_is_sep(rest[2]))
					return -EINVAL;

				/*
				 * "\\.\C:" with no trailing separator opens
				 * the volume device itself, not its root
				 * directory.  "\\?\C:\" opens the root.
				 */
				if (restlen == 2 && name[2] == '.') {
					out->type = NT_PATH_DEVICE;
					out->device = rest;
					out->device_len = 2;
					out->drive = nt_drive_upper(rest[0]);
					out->flags |= NT_PARSE_VOLUME_ROOT;
					err = 0;
					goto done;
				}

				out->type = NT_PATH_DRIVE_ABSOLUTE;
				out->drive = nt_drive_upper(rest[0]);
				err = nt_parse_components(out, rest + 2,
							  restlen - 2,
							  verbatim,
							  allow_stream);
				goto done;
			}

			/* "\\?\UNC\server\share\..." */
			if (restlen >= 3 && !strncasecmp(rest, "UNC", 3) &&
			    (restlen == 3 || nt_is_sep(rest[3]))) {
				err = nt_parse_unc(out, rest + 3, restlen - 3,
						   verbatim);
				goto done;
			}

			/*
			 * Anything else names a device object directly:
			 * "\\.\PhysicalDrive0", "\\?\Volume{...}\".
			 */
			{
				const char *dev = rest;
				const char *pos = rest;

				while (pos < end && !nt_is_sep(*pos))
					pos++;
				if (pos == dev)
					return -EINVAL;
				out->type = NT_PATH_DEVICE;
				out->device = dev;
				out->device_len = pos - dev;
				err = nt_parse_components(out, pos, end - pos,
							  verbatim,
							  allow_stream);
				goto done;
			}
		}

		/* Plain "\\server\share\...". */
		err = nt_parse_unc(out, name + 2, len - 2, verbatim);
		goto done;
	}

	/* "\??\..." or "\Device\..." - NT native, no Win32 involved. */
	if (nt_is_sep(name[0]) &&
	    ((len >= 4 && name[1] == '?' && name[2] == '?' &&
	      nt_is_sep(name[3])) ||
	     (len >= 8 && !strncasecmp(name + 1, "Device", 6) &&
	      nt_is_sep(name[7])))) {
		err = nt_parse_nt_object(out, name, len);
		goto done;
	}

	/* "C:..." - drive absolute or drive relative. */
	if (len >= 2 && isalpha(name[0]) && name[1] == ':') {
		out->drive = nt_drive_upper(name[0]);

		if (len == 2) {
			/*
			 * Bare "C:" is the current directory on C:, not the
			 * root of C:.
			 */
			out->type = NT_PATH_DRIVE_RELATIVE;
			out->flags |= NT_PARSE_VOLUME_ROOT;
			err = 0;
			goto done;
		}

		if (nt_is_sep(name[2])) {
			out->type = NT_PATH_DRIVE_ABSOLUTE;
			err = nt_parse_components(out, name + 3, len - 3,
						  verbatim, allow_stream);
		} else {
			out->type = NT_PATH_DRIVE_RELATIVE;
			err = nt_parse_components(out, name + 2, len - 2,
						  verbatim, allow_stream);
		}
		goto done;
	}

	/* "\Windows" - rooted on the caller's current drive. */
	if (nt_is_sep(name[0])) {
		out->type = NT_PATH_ROOTED;
		err = nt_parse_components(out, name + 1, len - 1, verbatim,
					  allow_stream);
		goto done;
	}

	/* Anything else is relative to the current directory. */
	out->type = NT_PATH_RELATIVE;
	err = nt_parse_components(out, name, len, verbatim, allow_stream);

done:
	if (err) {
		out->type = NT_PATH_INVALID;
		return err;
	}

	/*
	 * Win32-only checks.  These never apply to verbatim paths, which
	 * is the entire point of the "\\?\" prefix.
	 */
	if (!(out->flags & NT_PARSE_VERBATIM)) {
		if (out->rel_len > NT_MAX_PATH)
			out->flags |= NT_PARSE_LONG_PATH;

		if (out->nr_components) {
			char *slash = nt_last_slash(out->rel, out->rel_len);
			const char *last = slash ? slash + 1 : out->rel;
			size_t last_len = out->rel + out->rel_len - last;

			if (nt_name_is_reserved_device(last, last_len))
				out->flags |= NT_PARSE_RESERVED_NAME;
		}

		if ((flags & NT_PARSE_F_WIN32_LIMITS) &&
		    (out->flags & NT_PARSE_LONG_PATH)) {
			out->type = NT_PATH_INVALID;
			return -ENAMETOOLONG;
		}

		if ((flags & NT_PARSE_F_REJECT_DEVICE) &&
		    (out->flags & NT_PARSE_RESERVED_NAME)) {
			out->type = NT_PATH_INVALID;
			return -EINVAL;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(nt_path_parse);
