/*
 *  kexecboot - A kexec based bootloader
 *
 *  Copyright (c) 2008-2011 Yuri Bushmelev <jay4mail@gmail.com>
 *  Copyright (c) 2008 Thomas Kunze <thommycheck@gmx.de>
 *
 *  small parts:
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <sys/mount.h>
#include <linux/loop.h>
#include <ctype.h>
#include <errno.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "config.h"
#include "util.h"
#include "cfgparser.h"
#include "devicescan.h"
#include "evdevs.h"
#include "menu.h"
#include "kexecboot.h"

#ifdef USE_FBMENU
#include "gui.h"
#endif

#ifdef USE_TEXTUI
#include "tui.h"
#endif

#ifdef USE_ZAURUS
#include "machine/zaurus.h"
#endif

/* Don't re-create devices when executing on host */
#ifdef USE_HOST_DEBUG
#undef USE_DEVICES_RECREATING
#endif

#define __USE_LARGEFILE64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE

#ifdef USE_MACHINE_KERNEL
/* Machine-dependent kernel patch */
char *machine_kernel = NULL;
#endif

/* NULL-terminated array of kernel search paths
 * First item should be filled with machine-dependent path */
char *default_kernels[] = {
#ifdef USE_ZIMAGE
	"/mnt/boot/zImage",
	"/mnt/zImage",
#endif
#ifdef USE_UIMAGE
	"/mnt/boot/uImage",
	"/mnt/uImage",
#endif
	NULL
};

/* Init mode flag */
int initmode = 0;

/* Contexts available - menu and textview */
typedef enum {
	KX_CTX_MENU,
	KX_CTX_TEXTVIEW,
} kx_context;

/* Common parameters */
struct params_t {
	struct cfgdata_t *cfg;
	struct bootconf_t *bootcfg;
	kx_menu *menu;
	kx_context context;
#ifdef USE_FBMENU
	struct gui_t *gui;
#endif
#ifdef USE_TEXTUI
	kx_tui *tui;
#endif
};

static char *kxb_ttydev = NULL;
static int kxb_echo_state = 0;

static void atexit_restore_terminal(void)
{
	setup_terminal(kxb_ttydev, &kxb_echo_state, 0);
}

#ifdef USE_MACHINE_KERNEL
/* Return lowercased and stripped machine-specific kernel path */
/* Return value should be free()'d */
char *get_machine_kernelpath() {
	FILE *f;
	char *tmp, *hw = NULL, c;
	char line[80];

	f = fopen("/proc/cpuinfo", "r");
	if (!f) {
		perror("/proc/cpuinfo");
		exit(-1);
	}

	/* Search string 'Hardware' */
	while (fgets(line, sizeof(line), f)) {
		line[strlen(line) - 1] = '\0';
		hw = strstr(line, "Hardware");
		if (NULL != hw) break;
	}
	fclose(f);

	if ( NULL != hw) {
		/* Search colon then skip it and space after */
		hw = strchr(hw, ':');
		if (NULL == hw) {	/* Should not happens but anyway */
			log_msg(lg, "Can't find ':' in 'Hardware' line");
			return NULL;
		}
		hw += 2;	/* May be ltrim()? */

		/* Lowercase name and replace spaces with '_' */
		tmp = hw;
		while('\0' != *tmp) {
			c = *tmp;
			if (isspace(c)) {
				*tmp = '_';
			} else {
				*tmp = tolower(c);
			}
			++tmp;
		}

		/* Prepend "/mnt/boot/zImage-" to hw */
		tmp = malloc(strlen(hw) + 17 + 1);	/* strlen("/mnt/boot/zImage-") */
		if (NULL == tmp) {
			DPRINTF("Can't allocate memory for machine-specific kernel path");
			return NULL;
		}

		strcpy(tmp, "/mnt/boot/zImage-");
		strcat(tmp, hw);

		return tmp;
	}

	log_msg(lg, "Can't find 'Hardware' line in cpuinfo");
	return NULL;
}
#endif	/* USE_MACHINE_KERNEL */


int start_booting(struct params_t *params, int choice)
{
	struct boot_item_t *item;
	int file_fd, device_fd;
	
	item = params->bootcfg->list[choice];
	
	if ( ! (item->boottype & BOOT_TYPE_LINUX)) {
		char *const envp[] = { NULL };
		const char *exec_argv[] = { "/init-android", NULL};
		execve("/init-android", (char *const *)exec_argv, envp);
	}
	
	if (item->boottype & BOOT_TYPE_IMAGE) {
		if (-1 == mount(item->device, MOUNTPOINT, item->fstype, 0, NULL)) {
			log_msg(lg, "+ can't mount device containing boot image file '%s': %s", item->device, ERRMSG);
			return -1;
		}
		
		file_fd = open64(item->imagepath, O_RDWR);
		if (file_fd < 0) {
			log_msg(lg, "open image file '%s' failed", item->imagepath);
			umount(MOUNTPOINT);
			return -1;
		}
		
		device_fd = open("/dev/loop0", O_RDWR);
		if (device_fd < 0) {
			log_msg(lg, "open loop device failed");
			close(file_fd);
			umount(MOUNTPOINT);
			return -1;
		}
		
		if (ioctl(device_fd, LOOP_SET_FD, file_fd) < 0) {
			log_msg(lg, "ioctl LOOP_SET_FD failed");
			close(file_fd);
			close(device_fd);
			umount(MOUNTPOINT);
			return -1;
		}
		
		close(file_fd);
		close(device_fd);
		
		mount("/dev/loop0", ROOTFS, "ext4", 0, NULL);

		log_msg(lg, "Image mounted!\n");
	} else {
		if (-1 == mount(item->device, ROOTFS, item->fstype, 0, NULL)) {
			log_msg(lg, "+ can't mount boot device '%s': %s", item->device, ERRMSG);
			return -1;
		}
	}
	
	if (item->boottype & BOOT_TYPE_KEXEC) {
		/* we use var[] instead of *var because sizeof(var) using */
		const char kexec_path[] = KEXEC_PATH;
		
		const char str_cmdline_start[] = "--command-line=";
		const char str_partition[] = " partition=";
		const char str_image[] = " image=";
		const char str_directory[] = " directory=";
		const char str_initrd_start[] = "--initrd=";
		
		const char *load_argv[] = { NULL, "--load-hardboot", item->kernelpath, NULL, "--mem-min=0x84000000", NULL, NULL };
		const char *exec_argv[] = { NULL, "-e", NULL};
		
		char *cmdline_arg = NULL, *initrd_arg = NULL, cmdline[1024];
		int n;
		
		exec_argv[0] = kexec_path;
		load_argv[0] = kexec_path;
		
		/* --command-line arg generation */
		
		/* fill '--initrd' option */
		if (item->initrd) {
			/* allocate space */
			n = sizeof(str_initrd_start) + strlen(item->initrd);
			
			initrd_arg = (char *)malloc(n);
			if (NULL == initrd_arg) {
				log_msg(lg, "Can't allocate memory for initrd_arg");
			} else {
				strcpy(initrd_arg, str_initrd_start);	/* --initrd= */
				strcat(initrd_arg, item->initrd);
				load_argv[3] = initrd_arg;
			}
		}
		
		/* fill '--command-line' option */
		/* load current cmdline */
		FILE *f;
		f = fopen("/proc/cmdline", "r");
		if (NULL == f) {
			log_msg(lg, "No cmdline!\n");
			return -1;
		}
		fscanf(f, "%[^\n]", cmdline);
		fclose(f);
		
		/* allocate space */
		if (item->boottype & BOOT_TYPE_IMAGE) {
			n = strlen(str_cmdline_start) + 2 + strlen(cmdline) * 2 + strlen(str_partition)
			+ strlen(item->device) + strlen(str_image) + strlen(item->image);
		} else {
			n = strlen(str_cmdline_start) + 2 + strlen(cmdline) * 2 + strlen(str_partition)
			+ strlen(item->device) + strlen(str_directory) + strlen(item->directory);;
		}
		
		cmdline_arg = (char *)malloc(n);
		if (NULL == cmdline_arg) {
			log_msg(lg, "Can't allocate memory for cmdline_arg");
		} else {
			strcpy(cmdline_arg, str_cmdline_start);	/* --command-line= */
			strcat(cmdline_arg, "\"");
			strcat(cmdline_arg, cmdline);
			strcat(cmdline_arg, str_partition);
			strcat(cmdline_arg, item->device);
			if (item->boottype & BOOT_TYPE_IMAGE) {
				strcat(cmdline_arg, str_image);
				strcat(cmdline_arg, item->image);
			} else {
				strcat(cmdline_arg, str_directory);
				strcat(cmdline_arg, item->directory);
			}
			strcat(cmdline_arg, "\"");
			load_argv[5] = cmdline_arg;
		}
		
		log_msg(lg, "load_argv: %s, %s, %s, %s, %s, %s\n", load_argv[0],
			load_argv[1], load_argv[2], load_argv[3], load_argv[4], load_argv[5]);
			
		/* Load kernel */
		//n = fexecw(kexec_path, (char *const *)load_argv, envp);
		char op[4096];
		sprintf(op, "%s %s %s %s %s %s", load_argv[0],
				load_argv[1], load_argv[2], load_argv[3], load_argv[4], load_argv[5]);
		n = system(op);
		
		umount(ROOTFS);
		if (item->boottype & BOOT_TYPE_IMAGE) {
			file_fd = open64(item->imagepath, O_RDWR);
			if (file_fd < 0) {
				log_msg(lg, "open image file '%s' failed", item->imagepath);
			}
			
			device_fd = open("/dev/loop0", O_RDWR);
			if (device_fd < 0) {
				log_msg(lg, "open loop device failed");
				close(file_fd);
			}
			
			if (ioctl(device_fd, LOOP_CLR_FD, file_fd) < 0) {
				log_msg(lg, "ioctl LOOP_CLR_FD failed");
				close(file_fd);
				close(device_fd);
			}
			
			if (file_fd >= 0) close(file_fd);
			if (device_fd >= 0) close(device_fd);
			
			umount(MOUNTPOINT);
		}
			
		dispose(cmdline_arg);
		dispose(initrd_arg);
		
		log_msg(lg, "exec_argv: %s, %s", exec_argv[0], exec_argv[1]);
			
		/* Boot new kernel */
		//execve(kexec_path, (char *const *)exec_argv, envp);
		system("kexec -e");
	} else {
		
	}
	
	return -1;
}


int scan_devices(struct params_t *params)
{
	struct charlist *fl;
	struct bootconf_t *bootconf;
	struct cfgdata_t cfgdata;
	char cfgpath[256];
	int rc;
	
/*#ifdef USE_ICONS
	kx_cfg_section *sc;
	int i;
	int rows;
	char **xpm_data;
#endif*/

	init_cfgdata(&cfgdata);

	bootconf = create_bootcfg(4);
	if (NULL == bootconf) {
		DPRINTF("Can't allocate bootconf structure");
		return -1;
	}
	
	rc = devscan_open(&fl);
	if (-1 == rc) {
		log_msg(lg, "can't open device\n");
		return -1;
	}
	
	mkdir(MOUNTPOINT, 0666);
	mkdir(ROOTFS, 0666);
	if (-1 == mount(MMCBLK_BOOTCONF, MOUNTPOINT, MMCBLK_BOOTCONF_FSTYPE, MS_RDONLY, NULL)) {
		log_msg(lg, "+ can't mount bootconf device '%s': %s", MMCBLK_BOOTCONF, ERRMSG);
		goto end_scan_devices;
	}

	DIR* dir;
	struct dirent* entry;
	dir = opendir(BOOTCONF_PATH);
	if (dir != NULL) {
		for (;;) {
			entry = readdir(dir);
			if (entry == NULL) break;
			if (entry->d_type & DT_DIR) continue;
			log_msg(lg, "Configuration File: %s Found!\n", entry->d_name);
			
			sprintf(cfgpath, "%s/%s", BOOTCONF_PATH, entry->d_name);
			parse_cfgfile(cfgpath, &cfgdata);
		}
		
		closedir(dir);
	} else {
		log_msg(lg, "Configuration File NOT Found!\n");
	}
	
	if (-1 == umount(MOUNTPOINT)) {
		log_msg(lg, "+ can't umount device: %s", ERRMSG);
		goto end_scan_devices;
	}
	
	addto_bootcfg(bootconf, fl, &cfgdata);
	destroy_cfgdata(&cfgdata);
	
end_scan_devices:
	params->bootcfg = bootconf;
	return 0;
}

/* Create system menu */
kx_menu *build_menu(struct params_t *params)
{
	kx_menu *menu;
	kx_menu_level *ml;
	kx_menu_item *mi;
	
#ifdef USE_ICONS
	kx_picture **icons;
	
	if (params->gui) icons = params->gui->icons;
	else icons = NULL;
#endif
	
	/* Create menu with 2 levels (main and system) */
	menu = menu_create(2);
	if (!menu) {
		DPRINTF("Can't create menu");
		return NULL;
	}
	
	/* Create main menu level */
	menu->top = menu_level_create(menu, 4, NULL);
	
	/* Create system menu level */
	ml = menu_level_create(menu, 6, menu->top);
	if (!ml) {
		DPRINTF("Can't create system menu");
		return menu;
	}

	mi = menu_item_add(menu->top, A_SUBMENU, "System menu", NULL, ml);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_SYSTEM]);
#endif

	mi = menu_item_add(ml, A_PARENTMENU, "Back", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_BACK]);
#endif

	mi = menu_item_add(ml, A_RESCAN, "Rescan", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_RESCAN]);
#endif

	mi = menu_item_add(ml, A_DEBUG, "Show debug info", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_DEBUG]);
#endif

	mi = menu_item_add(ml, A_REBOOT, "Reboot", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_REBOOT]);
#endif

	mi = menu_item_add(ml, A_SHUTDOWN, "Shutdown", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_SHUTDOWN]);
#endif

	if (!initmode) {
		mi = menu_item_add(ml, A_EXIT, "Exit", NULL, NULL);
#ifdef USE_ICONS
		if (icons) menu_item_set_data(mi, icons[ICON_EXIT]);
#endif
	}

	menu->current = menu->top;
	menu_item_select(menu, 0);
	return menu;
}


/* Fill main menu with boot items */
int fill_menu(struct params_t *params)
{
	kx_menu_item *mi;
	int i, b_items, max_pri, max_i, *a;
	struct boot_item_t *tbi;
	struct bootconf_t *bl;
	const int sizeof_desc = 160;
	char *desc, *label;
#ifdef USE_ICONS
	kx_picture *icon;
	struct gui_t *gui;

	gui = params->gui;
#endif

	bl = params->bootcfg;

	if ( (NULL != bl) && (bl->fill > 0) ) b_items = bl->fill;
	else {
		log_msg(lg, "No items for menu found");
		return 0;
	}

	log_msg(lg, "Populating menu: %d item(s)", b_items);

	desc = malloc(sizeof_desc);
	if (NULL == desc) {
		DPRINTF("Can't allocate item description");
		goto dirty_exit;
	}

	a = malloc(b_items * sizeof(*a));	/* Markers array */
	if (NULL == a) {
		DPRINTF("Can't allocate markers array");
		goto dirty_exit;
	}

	for (i = 0; i < b_items; i++) a[i] = 0;	/* Clean markers array */

	/* Create menu of sorted by priority boot items */
	max_i = -1;
	for(;;) {
		max_pri = -1;
		/* Search item with maximum priority */
		for (i = 0; i < b_items; i++) {
			if (0 == a[i]) {	/* Check that item is not processed yet */
				tbi = bl->list[i];
				if (tbi->priority > max_pri) {
					max_pri = tbi->priority;	/* Max priority */
					max_i = i;					/* Max priority item index */
				}
			}
		}

		if (max_pri >= 0) {
			a[max_i] = 1;	/* Mark item as processed */
			/* We have found new max priority - insert into menu */
			tbi = bl->list[max_i];
			snprintf(desc, sizeof_desc, "%s %s %lluMb",
					tbi->device, tbi->fstype, tbi->blocks/1024);

			if (tbi->label)
				label = tbi->label;
			else
				label = tbi->kernelpath + sizeof(MOUNTPOINT) - 1;

			log_msg(lg, "+ [%s]", label);
			mi = menu_item_add(params->menu->top, A_DEVICES + max_i,
					label, desc, NULL);

#ifdef USE_ICONS
			if (gui) {
				/* Search associated with boot item icon if any */
				icon = tbi->icondata;
				if (!icon && (gui->icons)) {
					/* We have no custom icon - use default */
					switch (tbi->dtype) {
					case DVT_STORAGE:
						icon = gui->icons[ICON_STORAGE];
						break;
					case DVT_MMC:
						icon = gui->icons[ICON_MMC];
						break;
					case DVT_MTD:
						icon = gui->icons[ICON_MEMORY];
						break;
					case DVT_UNKNOWN:
					default:
						break;
					}
				}

				/* Add icon to menu */
				if (mi) mi->data = icon;
			}
#endif
		}

		if (-1 == max_pri) break;	/* We have no items to process */
	}

	free(a);
	free(desc);
	return 0;

dirty_exit:
	dispose(desc);
	return -1;
}


/* Return 0 if we are ordinary app or 1 if we are init */
int do_init(void)
{
	/* When our pid is 1 we are init-process */
	if ( 1 != getpid() ) {
		return 0;
	}

	log_msg(lg, "I'm the init-process!");

#ifdef USE_DEVTMPFS
	if (-1 == mount("devtmpfs", "/dev", "devtmpfs",
			0, NULL) ) {
		perror("Can't mount devtmpfs");
	}
#endif

	/* Mount procfs */
	if ( -1 == mount("proc", "/proc", "proc",
			0, NULL) ) {
		perror("Can't mount procfs");
		exit(-1);
	}

	/* Mount sysfs */
	if ( -1 == mount("sysfs", "/sys", "sysfs",
			0, NULL) ) {
		perror("Can't mount sysfs");
		exit(-1);
	}

	FILE *f;
	/* Set up console loglevel */
	f = fopen("/proc/sys/kernel/printk", "w");
	if (NULL == f) {
		/* CONFIG_PRINTK may be disabled */
		log_msg(lg, "/proc/sys/kernel/printk", ERRMSG);
	} else {
		fputs("0 4 1 7\n", f);
		fclose(f);
	}

	return 1;
}


int do_rescan(struct params_t *params)
{
	int i;

	/* Clean top menu level except system menu item */
	/* FIXME should be done by some function from menu module */
	kx_menu_item *mi;
	for (i = 1; i < params->menu->top->count; i++) {
		mi = params->menu->top->list[i];
		if (mi) {
			dispose(mi->label);
			dispose(mi->description);
			free(mi);
		}
		params->menu->top->list[i] = NULL;
	}
	params->menu->top->count = 1;

#ifdef USE_ICONS
	/* Destroy icons */
	/* FIXME should be done by some function from devicescan module */
	for (i = 0; i < params->bootcfg->fill; i++) {
		fb_destroy_picture(params->bootcfg->list[i]->icondata);
	}
#endif

	free_bootcfg(params->bootcfg);
	params->bootcfg = NULL;
	scan_devices(params);

	return fill_menu(params);
}


/* Process menu context 
 * Return 0 to select, <0 to raise error, >0 to continue
 */
int process_ctx_menu(struct params_t *params, int action) {
	static int rc;
	static int menu_action;
	static kx_menu *menu;
	menu = params->menu;

#ifdef USE_NUMKEYS
	/* Some hacks to allow menu items selection by keys 0-9 */
	if ((action >= A_KEY0) && (action <= A_KEY9)) {
		rc = action - A_KEY0;
		if (-1 == menu_item_select_by_no(menu, rc)) {
			/* There is no item with such number - do nothing */
			return 1;
		} else {
			action = A_SELECT;
		}
	}
#endif

	menu_action = (A_SELECT == action ? menu->current->current->id : action);
	rc = 1;

	switch (menu_action) {
	case A_UP:
		menu_item_select(menu, -1);
		break;
	case A_DOWN:
		menu_item_select(menu, 1);
		break;
	case A_SUBMENU:
		menu->current = menu->current->current->submenu;
		break;
	case A_PARENTMENU:
		menu->current = menu->current->parent;
		break;

	case A_REBOOT:
#ifdef USE_FBMENU
		gui_show_msg(params->gui, "Rebooting...");
#endif
#ifdef USE_TEXTUI
		tui_show_msg(params->tui, "Rebooting...");
#endif
#ifdef USE_HOST_DEBUG
		sleep(1);
#else
		sync();
		/* if ( -1 == reboot(LINUX_REBOOT_CMD_RESTART) ) { */
		if ( -1 == reboot(RB_AUTOBOOT) ) {
			log_msg(lg, "Can't initiate reboot: %s", ERRMSG);
		}
#endif
		break;
	case A_SHUTDOWN:
#ifdef USE_FBMENU
		gui_show_msg(params->gui, "Shutting down...");
#endif
#ifdef USE_TEXTUI
		tui_show_msg(params->tui, "Shutting down...");
#endif
#ifdef USE_HOST_DEBUG
		sleep(1);
#else
		sync();
		/* if ( -1 == reboot(LINUX_REBOOT_CMD_POWER_OFF) ) { */
		if ( -1 == reboot(RB_POWER_OFF) ) {
			log_msg(lg, "Can't initiate shutdown: %s", ERRMSG);
		}
#endif
		break;

	case A_RESCAN:
#ifdef USE_FBMENU
		gui_show_msg(params->gui, "Rescanning devices.\nPlease wait...");
#endif
#ifdef USE_TEXTUI
		tui_show_msg(params->tui, "Rescanning devices.\nPlease wait...");
#endif
		if (-1 == do_rescan(params)) {
			log_msg(lg, "Rescan failed");
			return -1;
		}
		menu = params->menu;
		break;

	case A_DEBUG:
		params->context = KX_CTX_TEXTVIEW;
		break;

	case A_EXIT:
		if (initmode) break;	// don't exit if we are init
	case A_ERROR:
		rc = -1;
		break;

#ifdef USE_TIMEOUT
	case A_TIMEOUT:		// timeout was reached - boot 1st kernel if exists
		if (menu->current->count > 1) {
			menu_item_select(menu, 0);	/* choose first item */
			menu_item_select(menu, 1);	/* and switch to next item */
			rc = 0;
		}
		break;
#endif

	default:
		if (menu_action >= A_DEVICES) rc = 0;
		break;
	}

	return rc;
}

/* Draw menu context */
void draw_ctx_menu(struct params_t *params)
{
#ifdef USE_FBMENU
	gui_show_menu(params->gui, params->menu);
#endif
#ifdef USE_TEXTUI
	tui_show_menu(params->tui, params->menu);
#endif
}


/* Process text view context
 * Return 0 to select, <0 to raise error, >0 to continue
 */
int process_ctx_textview(struct params_t *params, int action) {
	static int rc;

	rc = 1;
	switch (action) {
	case A_UP:
		if (lg->current_line_no > 0) --lg->current_line_no;
		break;
	case A_DOWN:
		if (lg->current_line_no + 1 < lg->rows->fill) ++lg->current_line_no;
		break;
	case A_SELECT:
		/* Rewind log view to top. This should make log view usable
		 * on devices with 2 buttons only (DOWN and SELECT)
		 */
		lg->current_line_no = 0;
		params->context = KX_CTX_MENU;
		break;
	case A_EXIT:
		if (initmode) break;	// don't exit if we are init
	case A_ERROR:
		rc = -1;
		break;
	}
	return rc;
}

/* Draw text view context */
void draw_ctx_textview(struct params_t *params)
{
#ifdef USE_FBMENU
	gui_show_text(params->gui, lg);
#endif
#ifdef USE_TEXTUI
	tui_show_text(params->tui, lg);
#endif
}


/* Main event loop */
int do_main_loop(struct params_t *params, kx_inputs *inputs)
{
	int rc = 0;
	int action;

	/* Start with menu context */
	params->context = KX_CTX_MENU;
	draw_ctx_menu(params);

	/* Event loop */
	do {
		/* Read events */
		action = inputs_process(inputs);
		if (action != A_NONE) {

			/* Process events in current context */
			switch (params->context) {
			case KX_CTX_MENU:
				rc = process_ctx_menu(params, action);
				break;
			case KX_CTX_TEXTVIEW:
				rc = process_ctx_textview(params, action);
			}

			/* Draw current context */
			if (rc > 0) {
				switch (params->context) {
				case KX_CTX_MENU:
					draw_ctx_menu(params);
					break;
				case KX_CTX_TEXTVIEW:
					draw_ctx_textview(params);
					break;
				}
			}
		}
		else
			rc = 1;

	/* rc: 0 - select, <0 - raise error, >0 - continue */
	} while (rc > 0);

	/* If item is selected then return his id */
	if (0 == rc) rc = params->menu->current->current->id;

	return rc;
}


int main(int argc, char **argv)
{
	int rc = 0;
	struct cfgdata_t cfg;
	struct params_t params;
	kx_inputs inputs;

	lg = log_open(16);
	log_msg(lg, "%s starting", PACKAGE_STRING);

	initmode = do_init();

	/* Get cmdline parameters */
	params.cfg = &cfg;
	init_cfgdata(&cfg);
	cfg.angle = 0;	/* No rotation by default */
	parse_cmdline(&cfg);

	kxb_ttydev = cfg.ttydev;
	setup_terminal(kxb_ttydev, &kxb_echo_state, 1);
	/* Setup function that will restore terminal when exit() will called */
	atexit(atexit_restore_terminal);

	log_msg(lg, "FB angle is %d, tty is %s", cfg.angle, cfg.ttydev);

#ifdef USE_MACHINE_KERNEL
	machine_kernel = get_machine_kernelpath();	/* FIXME should be passed as arg to get_bootinfo() */
#endif

#ifdef USE_DELAY
	/* extra delay for initializing slow SD/CF */
	sleep(USE_DELAY);
#endif

	int no_ui = 1;	/* UI presence flag */
#ifdef USE_FBMENU
	params.gui = NULL;
	if (no_ui) {
		params.gui = gui_init(cfg.angle);
		if (NULL == params.gui) {
			log_msg(lg, "Can't initialize GUI");
		} else no_ui = 0;
	}
#endif
#ifdef USE_TEXTUI
	FILE *ttyfp;
	params.tui = NULL;
	if (no_ui) {

		if (cfg.ttydev) ttyfp = fopen(cfg.ttydev, "w");
		else ttyfp = stdout;

		params.tui = tui_init(ttyfp);
		if (NULL == params.tui) {
			log_msg(lg, "Can't initialize TUI");
			if (ttyfp != stdout) fclose(ttyfp);
		} else no_ui = 0;
	}
#endif
	if (no_ui) exit(-1); /* Exit if no one UI was initialized */
	
	params.menu = build_menu(&params);
	params.bootcfg = NULL;
	scan_devices(&params);

	if (-1 == fill_menu(&params)) {
		exit(-1);
	}

	/* Collect input devices */
	inputs_init(&inputs, 8);
	inputs_open(&inputs);
	inputs_preprocess(&inputs);

	/* Run main event loop
	 * Return values: <0 - error, >=0 - selected item id */
	rc = do_main_loop(&params, &inputs);

#ifdef USE_FBMENU
	if (params.gui) {
		if (rc < 0) gui_clear(params.gui);
		gui_destroy(params.gui);
	}
#endif
#ifdef USE_TEXTUI
	if (params.tui) {
		tui_destroy(params.tui);
		if (ttyfp != stdout) fclose(ttyfp);
	}
#endif
	inputs_close(&inputs);
	inputs_clean(&inputs);

	log_close(lg);
	lg = NULL;

	/* rc < 0 indicate error */
	if (rc < 0) exit(rc);

	menu_destroy(params.menu, 0);

	if (rc >= A_DEVICES) {
		start_booting(&params, rc - A_DEVICES);
	}

	/* When we reach this point then some error has occured */
	DPRINTF("We should not reach this point!");
	exit(-1);
}
