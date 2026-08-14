.. SPDX-License-Identifier: GPL-2.0

==============================
The NT filesystem personality
==============================

:Config: ``CONFIG_NT_FS_PERSONALITY``
:Status: experimental
:Behavioural target: Windows 7 SP1 x64

Introduction
============

This subsystem gives the kernel a first-class notion of an NT-style
filesystem namespace.  A process that opts in perceives volumes with drive
letters and resolves ``C:\Windows\System32`` through the kernel, rather
than through a translation layer bolted on above it.

It is *not* a filesystem.  It does not require NTFS on disk, it does not
replace POSIX pathname resolution, and it is not a Wine prefix.  It is a
namespace and a set of semantics layered on the existing VFS, in the same
spirit as an ABI personality: the same kernel, presenting a different
world to processes that ask for it.

The end goal is that a PE loader and Win32 compatibility subsystem can ask
the kernel for ``C:\Windows\System32\kernel32.dll`` and reach a genuine,
system-wide volume, with no per-application ``drive_c`` anywhere.

Layering
========

Almost every mistake in this area comes from attributing a behaviour to
the wrong layer.  "NTFS is case-insensitive" is false; "Windows paths
can't contain a colon" is false; "``CON`` is a reserved filename" is
false.  The architecture therefore separates six layers explicitly, and
each behaviour is implemented in exactly one of them.

.. code-block:: none

    +---------------------------------------------------------------+
    | 6. Win32 / NT API subsystem     CreateFileW, NtCreateFile      |  future
    +---------------------------------------------------------------+
    | 5. Win32 pathname rules         reserved names, MAX_PATH,      |  fs/ntpers/
    |                                 normalisation, \\?\ escape     |  path.c
    |                                                                |  reserved.c
    +---------------------------------------------------------------+
    | 4. NT namespace / volumes       C:, \Device\HarddiskVolume1,   |  fs/ntpers/
    |                                 volume GUIDs, \??\             |  volume.c
    |                                                                |  namespace.c
    +---------------------------------------------------------------+
    | 3. NT filesystem personality    DOS attributes, NT timestamps, |  fs/ntpers/
    |                                 security descriptors, file IDs |  meta.c
    |                                 streams, reparse points,       |  (streams and
    |                                 share modes                    |  locks: 4-5)
    +---------------------------------------------------------------+
    | 2. Linux VFS                    dentries, inodes, mounts,      |  fs/
    |                                 struct path, struct file       |
    +---------------------------------------------------------------+
    | 1. Backing filesystem           ext4, bcachefs, ntfs3, tmpfs   |  fs/*/
    +---------------------------------------------------------------+

Worked examples of the distinction
----------------------------------

``CON``
    NTFS stores a file called ``CON`` without complaint, and ``fs/ntfs3``
    reads and writes it.  The NT Object Manager has a device object called
    ``\Device\Null``, but does not consult it when parsing a filesystem
    path.  It is ``CreateFileW`` - layer 5 - that inspects the last
    component, recognises ``CON``, and opens a device instead of touching
    the disk.  A ``\\?\`` path skips that inspection, which is exactly how
    you create a file called ``CON`` on Windows.

    Implemented in ``reserved.c``, which *flags* such names and never
    rejects them.  Only a layer 6 caller may act on the flag.

Case insensitivity
    The NT kernel is case-sensitive by default.  ``OBJ_CASE_INSENSITIVE``
    is a per-open flag, and NTFS stores an ``$UpCase`` table precisely so
    that the flag can be honoured.  Every Win32 caller sets it, which is
    why Windows *appears* case-insensitive.

    Implemented in ``casefold.c``, driven by a resolve flag rather than
    being unconditional.

``MAX_PATH``
    260 characters is a Win32 constant.  The NT kernel accepts up to 32767
    UTF-16 code units, and ``\\?\`` exists to reach them.

    Reported by the parser as ``NT_PARSE_LONG_PATH``, enforced only when
    the caller passes ``NT_PARSE_F_WIN32_LIMITS``.

``.`` and ``..``
    Collapsed by ``RtlGetFullPathName_U`` in ntdll before a path reaches
    the NT kernel.  A ``\\?\`` path keeps them as literal component names.

    Implemented in the parser, suppressed for verbatim paths.

Objects
=======

``struct nt_volume``
--------------------

The binding between an NT drive letter and a Linux mount.  This is where
``C:`` stops being a string and becomes an object.

A volume deliberately duplicates nothing the VFS already knows: it holds a
``struct path``, which pins a ``(vfsmount, dentry)`` pair, and that is the
whole of its relationship with storage.  What it adds is the identity
metadata NT expects and Linux has nowhere to keep:

==================  ==========================================================
Field               Meaning
==================  ==========================================================
``root``            backing ``struct path``; the volume's root directory
``id``              dense internal id, allocated in registration order
``guid``            volume GUID, as in ``\\?\Volume{...}\``
``serial``          32-bit serial reported by ``GetVolumeInformation``
``letter``          assigned drive letter, or 0
``label``           volume label
``fs_name``         filesystem name reported to Win32
``nt_device``       ``\Device\HarddiskVolume<id>``
==================  ==========================================================

The GUID comes from the superblock UUID when there is one, so it is stable
across boots.  The serial is folded down from the same UUID, falling back
to the device number.

``fs_name`` reports ``NTFS`` for filesystems that can carry the semantics
this subsystem needs.  That is a compatibility decision, not a claim about
the on-disk format: Win32 has no vocabulary for "ext4", and software
branches on this string.  The real filesystem type is always visible in
``/proc/mounts`` and in the debugfs volume table.

``fs_flags`` is the honest half of that answer.  It carries the
``NT_FS_*`` capability bits ``GetVolumeInformation()`` returns in
``lpFileSystemFlags``, and they are set from what the volume can actually
do rather than from what the name implies.  The distinction matters
because an application told it has named streams will open one.  So
``NT_FS_SUPPORTS_REPARSE_POINTS``, ``NT_FS_NAMED_STREAMS`` and
``NT_FS_SUPPORTS_OPEN_BY_FILE_ID`` are clear until those features exist,
and ``NT_FS_PERSISTENT_ACLS`` is clear because security descriptors are
stored but do not yet govern access.  A capability bit set ahead of its
feature is not an optimistic placeholder; it is a promise with a caller
attached.

``struct nt_namespace``
-----------------------

Owns the drive-letter table.  There is one per machine (``init_nt_ns``),
refcounted so a container-visible NT namespace can be added later without
changing any of its users.  Readers use RCU, so looking up a drive letter
during a pathname walk takes no locks.

``struct nt_task_ctx``
----------------------

Per-process state: personality flags, current drive, remembered current
directory per drive, and the namespace to resolve against.

It hangs off ``struct fs_struct``, which means it is shared and copied on
exactly the same terms as the root and current directory: threads of a
process share it, and ``CLONE_FS`` shares it across processes.  That
matches Windows, where the current directory is process-wide.

A process with no context behaves exactly as it does today.  The pointer
is ``NULL`` until something calls ``prctl()``.

.. note::
   In real Windows the per-drive current directory lives in the Win32
   process environment block as a hidden ``=C:`` variable, not in the NT
   kernel.  It is kept in the kernel here because our Win32 personality
   has no PEB of its own yet.

Pathname parsing
================

``nt_path_parse()`` in ``path.c`` classifies a pathname and produces a
canonical form.  It has no VFS references at all, which is what lets it be
unit tested exhaustively and reused by a future Win32 personality for
paths it never intends to open.

Recognised forms
----------------

======================================  ==============================  ========
Input                                   ``enum nt_path_type``           Layer
======================================  ==============================  ========
``C:\Windows``                          ``NT_PATH_DRIVE_ABSOLUTE``      Win32
``C:Windows``                           ``NT_PATH_DRIVE_RELATIVE``      Win32
``\Windows``                            ``NT_PATH_ROOTED``              Win32
``Windows``                             ``NT_PATH_RELATIVE``            Win32
``\\server\share\x``                    ``NT_PATH_UNC``                 Win32
``\\?\C:\x``                            ``NT_PATH_DRIVE_ABSOLUTE``      Win32
``\\?\UNC\server\share\x``              ``NT_PATH_UNC``                 Win32
``\\.\PhysicalDrive0``                  ``NT_PATH_DEVICE``              Win32
``\??\C:\x``                            ``NT_PATH_DRIVE_ABSOLUTE``      NT OM
``\Device\HarddiskVolume1\x``           ``NT_PATH_NT_OBJECT``           NT OM
======================================  ==============================  ========

``\\?\`` and ``\\.\`` look alike and are not: ``\\?\`` suppresses all
Win32 normalisation, ``\\.\`` does not.  ``\\.\C:`` names the volume
device itself; ``\\?\C:\`` names its root directory.

Canonical output
----------------

The parser emits components ``/``-separated into a caller-supplied buffer.
This is not a string substitution hack, it is a deliberate encoding
choice: ``/`` cannot appear in an NT filename, because Win32 accepts it as
a separator everywhere ``\`` is accepted.  The transformation is therefore
lossless, and the result can be handed straight to ``vfs_path_lookup()``
without a second pass or a private walker.

The parser never allocates.  Everything it produces points into the
caller's buffer or into the input, so there is no per-component
allocation on the hot path.

Backslash is the normal separator for NT-personality processes; nothing
converts backslashes to slashes in applications.

Named streams
-------------

``file.txt:name``, ``file.txt:name:$DATA`` and ``file.txt::$DATA`` are
parsed into a filename plus a stream name and attribute type.  The last
form names the unnamed default stream, which *is* the file, and is
reported as no stream at all so callers need not special-case it.

The parser understands the syntax today.  Stream storage is stage 4; until
then ``nt_path_resolve()`` fails a stream request with ``-EOPNOTSUPP``
rather than silently returning the default stream, which would hand the
caller the wrong bytes.

Resolution
==========

``nt_path_resolve()`` joins the NT namespace to the VFS.  Two decisions
shape it.

**The starting point comes from the volume layer.**  ``C:\Windows`` starts
at the root of whatever volume holds drive letter C, which is a real
object with a real mount behind it.  This is what makes ``C:`` a
first-class concept rather than an alias for ``/``.

**The walk is the ordinary VFS walk.**  Once the starting point is known
and the path is canonical, ``vfs_path_lookup()`` provides mount crossing,
symlinks, permission checks and RCU pathwalk.  Reimplementing any of that
would be a large amount of duplicated code and a security liability.

Starting points by path type:

=============================  ================================================
Type                           Resolves relative to
=============================  ================================================
``DRIVE_ABSOLUTE``             root of the named volume
``DRIVE_RELATIVE``             remembered directory on that drive, else its root
``ROOTED``                     root of the process's current drive
``RELATIVE``                   ``current->fs->pwd``
``NT_OBJECT`` / ``DEVICE``     volume named by device name or GUID
``UNC``                        ``-EOPNOTSUPP``; a redirector's job
=============================  ================================================

Case-insensitive resolution
===========================

Two tiers, chosen per directory.

Tier 1: the filesystem folds case itself
----------------------------------------

ext4, f2fs and bcachefs support per-directory casefolding.  The dcache
then does everything: the superblock's ``->d_hash()`` hashes the folded
name and ``->d_compare()`` compares folded, so an ordinary lookup of
``WINDOWS`` finds the dentry for ``Windows`` at full RCU-walk speed.  The
NT resolver contributes nothing and costs nothing.

**This is the intended configuration.**  Format the volume with
casefolding enabled and set ``+F`` on its root so new directories inherit
it::

    mkfs.ext4 -O casefold /dev/sda2
    mount /dev/sda2 /mnt/c
    chattr +F /mnt/c

Casefolding is a property of each *directory*, not of the filesystem, so
a tree can be mixed: ``+F`` is inherited at creation, but a directory
that predates the flag does not have it and cannot be given it while it
is non-empty.  ``NT_VOL_NATIVE_CI`` is therefore only a hint - it records
what the volume *root* does.  When the fast path uses it and the lookup
returns ``-ENOENT``, the resolver retries through tier 2 before believing
the name is absent, because the miss may only mean some directory along
the way does not fold.  The retry costs nothing on the common path, where
the fast path succeeds.

Tier 2: the filesystem is case-sensitive
----------------------------------------

There is no index to consult, so somebody has to read the directory.
Doing that on every lookup would be exactly the pathological behaviour
this subsystem must avoid, so instead ``casefold.c`` keeps a *fold-hint
cache*: a hash table keyed by ``(superblock, parent inode number, folded
name)`` whose value is the real spelling of the name on disk.

* A hit becomes one ordinary, exact VFS lookup of the real name.
* A miss costs one directory read.  After that, every lookup of any casing
  of that name is a hash lookup.

The cache is only ever a hint.  Its answer is verified by really looking
the name up, so a stale entry cannot produce a wrong result - only a
wasted lookup followed by a rescan.  That is what lets it avoid any
coherency protocol with the dcache, and why it can key on a superblock
pointer it never dereferences.

Known limitations of tier 2, stated plainly:

* It cannot use RCU pathwalk, because a directory read can sleep.  Every
  component takes a reference.
* It does not follow symlinks in the middle of a path; such a component
  stops the walk with ``-ELOOP`` rather than resolving to something the
  caller did not ask for.  A symlink as the **final** component is
  handled: with ``NT_RESOLVE_FOLLOW`` the component is handed to
  ``vfs_path_lookup()`` under its real on-disk spelling so that the VFS
  does the loop counting and nesting limits, and without the flag the
  link itself is returned, which is what a create carrying
  ``FILE_OPEN_REPARSE_POINT`` wants.  The link *target* resolves
  case-sensitively - it is a Linux path stored on a filesystem that does
  not fold.
* The first touch of each differently-cased name reads a directory.
* It needs *read* permission on a directory, not merely search
  permission, to match a name by a casing other than the stored one.  On
  a mode 0711 directory a caller can open a file by its exact name but
  not by a different casing.  This is deliberate: the alternative would
  let a caller enumerate an unreadable directory one guess at a time.

None of these apply on a tier 1 volume, and the first three are not
architectural: they are the cost of a filesystem with no case-insensitive
index.  Counters for the cache are in ``<debugfs>/ntpers/cache``.

Performance notes
=================

* The parser makes no allocations and no per-component allocations.
* Drive letter lookup is an RCU-protected array index.
* Tier 1 case folding adds nothing to the VFS fast path.
* There is no FUSE, no userspace daemon, and no user/kernel transition
  anywhere in resolution.
* Tier 2 gives up RCU-walk.  This is documented above rather than hidden;
  the structure of ``nt_walk_ci()`` keeps the slow path contained so it
  can be optimised later without changing callers.

Userspace interface
===================

prctl
-----

``PR_SET_NT_PERSONALITY`` / ``PR_GET_NT_PERSONALITY`` set and read a mask
of ``NT_PERSONALITY_*`` from ``<linux/nt_personality.h>``.
``PR_SET_NT_DRIVE`` / ``PR_GET_NT_DRIVE`` set and read the current drive
letter.

There is deliberately no privilege check on the personality itself: it
only changes how a process's own pathnames are interpreted, and every
resolution still goes through the same VFS permission checks.  It cannot
reach anything the process could not already reach.

debugfs
-------

A development and test interface, root-only, not ABI::

    <debugfs>/ntpers/volumes       read   the volume table
    <debugfs>/ntpers/control       write  mount/unmount/system commands
    <debugfs>/ntpers/parse         rw     write a path, read its parse
    <debugfs>/ntpers/resolve       rw     write a path, read what it resolved to
    <debugfs>/ntpers/getinfo       rw     write a path, read its NT metadata
    <debugfs>/ntpers/cache         read   fold-hint cache counters
    <debugfs>/ntpers/personality   rw     this process's personality flags

Establishing a system volume::

    echo "mount C /" > /sys/kernel/debug/ntpers/control
    echo "system C" > /sys/kernel/debug/ntpers/control
    cat /sys/kernel/debug/ntpers/volumes

``mount`` does not mount anything; it registers an existing Linux
directory as the root of an NT volume.

Tracing
=======

Tracepoints under ``ntpers``::

    echo 1 > /sys/kernel/debug/tracing/events/ntpers/enable

======================  ====================================================
Event                   Reports
======================  ====================================================
``ntpath_parse``        input, classification, canonical form, flags
``ntpath_resolve``      starting volume, canonical form, resulting dentry
``ntpath_ci_lookup``    which case-folding tier answered, and whether it hit
``ntmeta_attrs``        DOS attributes read or written, and from where
``ntmeta_query``        a metadata query, and whether CreationTime is real
``ntvol_event``         volume created, relettered, destroyed
======================  ====================================================

Sample output::

    ntpath_parse: process=explorer.exe input="C:\Windows\System32" \
        type=drive-absolute drive=C rel="Windows/System32" comps=2 flags= err=0
    ntpath_resolve: process=explorer.exe type=drive-absolute volume=C \
        rel="Windows/System32" resolved=ffff888104a3c000 ino=131074 err=0

Testing
=======

KUnit, covering the parser, the reserved-name rules, the volume registry
and resolution::

    tools/testing/kunit/kunit.py run --kunitconfig fs/ntpers/tests

Selftests, covering the userspace-visible behaviour including real
case-insensitive lookup against a live filesystem::

    make -C tools/testing/selftests TARGETS=filesystems/nt_personality run_tests

Implementation status
=====================

============================================  =========================
Stage                                          State
============================================  =========================
1. Volumes, namespaces, per-process contexts  implemented
2. NT/Win32 pathname parser and resolver      implemented
2. Case-insensitive resolution                implemented (both tiers)
3. DOS attributes, NT timestamps, file IDs    implemented
3. NT security descriptor storage             implemented (advisory)
4. Alternate streams, reparse points, 8.3     parsed only; not stored
5. Share modes, delete-pending, range locks   designed; not implemented
6. System volume layout (C:\Windows, ...)     volume concept implemented
7. Win32 subsystem hooks                      partial; see below
============================================  =========================

File metadata
=============

``meta.c`` answers NT's questions about a file using an inode that was
never designed to answer them.  Three sources, in order of preference:
the filesystem itself, the inode, and this subsystem's own extended
attribute.

DOS attributes
--------------

Which source owns which attribute is the whole design:

=====================  ==========  ==============================================
Attribute              Source      Notes
=====================  ==========  ==============================================
``DIRECTORY``          inode       ``S_ISDIR``
``REPARSE_POINT``      inode       ``S_ISLNK`` until stage 4 adds real tags
``DEVICE``             inode       character, block, fifo or socket
``COMPRESSED``         statx       ``STATX_ATTR_COMPRESSED``
``ENCRYPTED``          statx       ``STATX_ATTR_ENCRYPTED``
``SPARSE_FILE``        statx       fewer blocks than the size accounts for
``READONLY``           file mode   no write bit set for anybody
``HIDDEN``             xattr       defaults to "name begins with a dot"
``ARCHIVE``            xattr       defaults to set, for regular files
``SYSTEM``             xattr
``TEMPORARY``          xattr
``OFFLINE``            xattr
``NOT_CONTENT_INDEXED`` xattr
=====================  ==========  ==============================================

Anything derived is recomputed on every query, because every one of those
can change through an ordinary POSIX operation that has no reason to
report it.  Storing them would only let them go stale.

``READONLY`` is backed by the file mode rather than stored on its own, so
that it genuinely prevents writes and so that the Linux and NT answers to
"can this be written?" cannot disagree.  Windows applies it per-file
rather than per-user, so setting it removes write permission from
everybody.

Putting it back is the hard half.  A DOS attribute carries no information
about *who* should be able to write, so the attribute alone cannot
reconstruct the mode.  Guessing wide - mirroring the read bits - turns an
ordinary 0644 into 0666 and grants access nobody asked for.  Guessing
narrow - owner write only - is safe but lossy: 0664 comes back as 0644
and the group silently loses write access.

So the write bits that were removed are recorded in
``user.nt.saved_write`` and exactly those bits are restored, ORed into
whatever the mode is at the time.  A ``chmod`` made while the file was
read-only therefore survives rather than being rolled back, and a round
trip through ``READONLY`` is lossless.  Filesystems and file types that
will not carry a ``user.*`` xattr fall back to the narrow guess, which is
the right failure mode: it never grants access that was not there before.

On the xattr namespace, which is a security question rather than a naming
one:

=============  ==========================================================
Namespace      Why it is or is not used
=============  ==========================================================
``system.``    Reserved for the filesystem; a generic filesystem rejects
               names it does not implement.  Used only for the ntfs3
               passthrough, where the values are real NTFS metadata.
``trusted.``   Requires ``CAP_SYS_ADMIN`` on every access, so an ordinary
               program could not set the hidden bit on its own file.
               That is not the semantics Windows has.
``user.``      Writable by the file's owner, which is exactly right for
               DOS attributes.  Used for ``user.nt.dos_attrib`` and
               ``user.nt.crtime``.  Its limitation is that the kernel
               forbids ``user.*`` on symlinks and device nodes, so DOS
               attributes on those fall back to derivation.
``security.``  Mediated by the LSM rather than freely writable.  Used for
               the security descriptor, and where Samba keeps NT ACLs.
=============  ==========================================================

On a volume backed by real NTFS, ``system.ntfs_attrib`` and
``system.ntfs_security`` are tried first, so the values read and written
are the ones in ``$STANDARD_INFORMATION``.  A volume backed by NTFS and
one backed by ext4 look identical from above.

Timestamps
----------

NT expects four timestamps and Linux provides three.  The mapping is not
the naive one:

===================  ====================================================
NT                   Linux
===================  ====================================================
``LastWriteTime``    ``mtime``
``LastAccessTime``   ``atime``
``ChangeTime``       ``ctime`` - metadata change time, the correct match
``CreationTime``     a value set through ``nt_set_creation_time()``,
                     otherwise ``statx`` ``btime``, otherwise an estimate
===================  ====================================================

``ctime`` is **not** ``CreationTime``.  It is the inode change time, and
NT already has a concept for that.  When no birth time exists anywhere,
``nt_query_file_info()`` reports the oldest timestamp the inode does have
and sets ``NT_TIME_CREATION_ESTIMATED``, so a caller that needs to know
the difference can tell.  Exactly one of ``NT_TIME_CREATION_EXACT`` and
``NT_TIME_CREATION_ESTIMATED`` is always set; the KUnit suite asserts
that.

A stored creation time takes precedence over the filesystem's own birth
time, and that ordering is deliberate.  No filesystem lets anything
change its birth time, so ``nt_set_creation_time()`` has to store the
value alongside; if the query then preferred ``btime``, a Windows
``SetFileTime(...CreationTime...)`` would report success and change
nothing observable.  Installers and archive extractors do that call
routinely and expect it to mean something.  Reporting the inode's birth
time is the better answer to a POSIX question, and this is not one - the
POSIX answer is still in ``statx`` for anything that wants it.

NT time is 100ns units since 1601-01-01;
``nt_time_from_timespec()``/``nt_time_to_timespec()`` are the conversion,
matching ``fs/ntfs3``'s ``kernel2nt()``/``nt2kernel()``.

File identifiers
----------------

The property that matters is that an id must not silently start referring
to a different file.  An inode number alone does not have that property,
because inode numbers are reused after deletion - the same problem NFS
has, with the same answer: pair the inode number with the inode
generation, which changes on reuse.

The 64-bit id is the inode number, because that is the width NT gives us.
The 128-bit id carries the inode number, the generation and the volume
serial, and is the one to prefer.

Security descriptors
--------------------

Stored alongside the Linux credentials, not instead of them.  Linux uid,
gid, mode and POSIX ACLs remain the only thing that governs access; the
descriptor is metadata a future Win32 subsystem can hand back to a caller
that asks for an owner SID or a DACL.  The header is validated before
storage, so anything reading one back can trust it.

.. warning::
   This is why the descriptor is safe in ``security.`` today: it is
   inert, so a forged one grants nothing.  Before anything starts making
   access decisions from it, this needs revisiting.  At that point a
   descriptor an unprivileged owner can rewrite becomes an
   access-control bypass, and the write path will have to enforce that a
   new descriptor is no more permissive than the caller could already
   achieve through ``chmod``.

What stages 4 and 5 will need
=============================

Recorded here so the design is not lost.

Streams and reparse points (stage 4)
------------------------------------

A named stream needs a real backing object, not an xattr: xattrs are
size-limited and an alternate data stream is not.  The plan is a hidden
per-file store with an xattr fast path for small streams, keeping the
unnamed ``$DATA`` stream as the file itself.

Reparse points need tag plus arbitrary payload, so a Linux symlink alone
is not sufficient.  A symlink is a valid optimised backing for
``IO_REPARSE_TAG_SYMLINK``, but the tag and payload must stay visible to
the NT personality.

Handle semantics (stage 5)
--------------------------

Windows share modes are a property of open handles, so they belong above
individual filesystems: a VFS-level sharing state keyed to the inode,
checked at open, holding desired access and share mode, and implementing
delete-pending.  POSIX ``unlink`` semantics are not equivalent and must
not be assumed to be.  Byte-range locking should reuse ``fs/locks.c``
rather than reinventing it.

Things this subsystem deliberately does not do
==============================================

* It does not delete or hide ``/proc``, ``/sys`` or ``/dev``.  Those are
  kernel interfaces.  What an NT-personality process *sees* is a separate
  question from what exists.
* It does not require NTFS on disk.
* It does not rewrite ext4, bcachefs, the block layer, the page cache or
  writeback.
* It does not change behaviour for any process that has not opted in.
* It does not put a FUSE filesystem or a userspace daemon in the lookup
  path.
