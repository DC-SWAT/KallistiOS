# KallistiOS ##version##
#
# kernel Makefile
# Copyright (C) 2001 Megan Potter
# Copyright (C) 2024 Falco Girgis
#

OBJS = version.o
SUBDIRS = arch debug fs thread net libc exports romdisk
STUBS = stubs/kernel_export_stubs.o stubs/arch_export_stubs.o

# Everything from here up should be plain old C.
KOS_CFLAGS += $(KOS_CSTD)

all: defaultall $(STUBS)
	rm -f $(KOS_BASE)/lib/$(KOS_ARCH)/libkallisti.a
	kos-ar rcs $(KOS_BASE)/lib/$(KOS_ARCH)/libromdiskbase.a romdisk/*.o
	kos-ar rcs $(KOS_BASE)/lib/$(KOS_ARCH)/libkallisti.a build/*.o
	kos-ar rcs $(KOS_BASE)/lib/$(KOS_ARCH)/libkallisti_exports.a stubs/*.o

stubs/kernel_export_stubs.c: exports.txt
	$(KOS_BASE)/utils/genexports/genexportstubs.sh $< stubs/kernel_export_stubs.c

stubs/arch_export_stubs.c: arch/$(KOS_ARCH)/exports-$(KOS_SUBARCH).txt
	$(KOS_BASE)/utils/genexports/genexportstubs.sh $< stubs/arch_export_stubs.c

include $(KOS_BASE)/Makefile.prefab

clean: defaultclean
	rm -f build/*.o
	rm -f build/libc/*.o
	rm -f romdisk/*.o
	rm -f stubs/*.o stubs/kernel_export_stubs.c stubs/arch_export_stubs.c

run:
