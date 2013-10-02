/*
 *  kexecboot - A kexec based bootloader
 *
 *  Copyright (c) 2008-2011 Yuri Bushmelev <jay4mail@gmail.com>
 *  Copyright (c) 2008 Thomas Kunze <thommycheck@gmx.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#ifndef _HAVE_DEVICESCAN_H_
#define _HAVE_DEVICESCAN_H_

#include "config.h"
#include "util.h"
#include "cfgparser.h"

/* Device structure */
struct device_t {
	char *device;		/* Device path (/dev/mmcblk0p1) */
	const char *fstype;	/* Filesystem (ext4) */
	unsigned long long blocks;	/* Device size in 1K blocks */
};

enum dtype_t {
	DVT_UNKNOWN,
	DVT_STORAGE,
	DVT_MMC,
	DVT_MTD
};

/* Boot item structure */
struct boot_item_t {
	char *device;		/* Device path (/dev/mmcblk0p1) */
	const char *fstype;	/* Filesystem (ext4) */
	unsigned long long blocks;	/* Device size in 1K blocks */
	char *label;		/* Partition label (name) */
	char *kernelpath;	/* Found kernel (/boot/zImage) */
	char *cmdline;		/* Kernel cmdline (logo.nologo debug) */
	char *initrd;		/* Initial ramdisk file */
	char *directory;	/* Boot directory */
	char *image;		/* Partition image */
	char *imagepath;	/* Partition image file */
	void *icondata;		/* Icon data */
	int boottype;		/* Boot type */
	int priority;		/* Priority of item in menu */
	enum dtype_t dtype;	/* Device type */
};

/* Boot configuration structure */
struct bootconf_t {
	int timeout;				/* Seconds before default item autobooting (0 - disabled) */
	struct boot_item_t *default_item;	/* Default menu item (NULL - none) */
	enum ui_type_t ui;			/* UI (graphics/text) */
	int debug;					/* Use debugging */

	struct boot_item_t **list;	/* Boot items list */
	unsigned int size;			/* Count of boot items in list */
	unsigned int fill;			/* Filled items count */
};

extern char *machine_kernel;
extern char *default_kernels[];

/* Prepare devicescan loop */
int devscan_open(struct charlist **fslist);

/* Get next device (fp & fslist in, dev out) */
int devscan(char *device, struct charlist *fslist, struct device_t *dev);

/* Allocate bootconf structure */
struct bootconf_t *create_bootcfg(unsigned int size);

/* Free bootconf structure */
void free_bootcfg(struct bootconf_t *bc);

/* Import values from cfgdata and boot to bootconf */
int addto_bootcfg(struct bootconf_t *bc, struct charlist *fl,
		struct cfgdata_t *cfgdata);

/* Check and parse config file */
int get_bootinfo(struct cfgdata_t *cfgdata);

#ifdef DEBUG
/* Print bootconf structure */
void print_bootcfg(struct bootconf_t *bc);
#endif

#endif
