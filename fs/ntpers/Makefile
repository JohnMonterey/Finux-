# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the NT filesystem personality.
#

obj-$(CONFIG_NT_FS_PERSONALITY)	+= ntpers.o

ntpers-y := main.o path.o reserved.o volume.o namespace.o resolve.o casefold.o \
	     meta.o open.o share.o stream.o handle.o
ntpers-$(CONFIG_NT_FS_PERSONALITY_DEBUGFS) += debugfs.o

obj-$(CONFIG_NT_FS_PERSONALITY_KUNIT_TEST) += tests/
