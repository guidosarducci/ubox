/*
 * Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <libgen.h>
#include <glob.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/swap.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <uci.h>
#include <uci_blob.h>

#include <libubox/list.h>
#include <libubox/vlist.h>
#include <libubox/blobmsg_json.h>
#include <libubox/avl-cmp.h>

#include "libblkid-tiny/libblkid-tiny.h"

enum {
	TYPE_MOUNT,
	TYPE_SWAP,
};

struct mount {
	struct vlist_node node;
	int type;

	char *target;
	char *path;
	char *options;
	char *uuid;
	char *label;
	char *device;
	int extroot;
	int overlay;
	int disabled_fsck;
	unsigned int prio;
};

static struct vlist_tree mounts;
static struct blob_buf b;
static LIST_HEAD(devices);
static int anon_mount, anon_swap, auto_mount, auto_swap, check_fs;
static unsigned int delay_root;

enum {
	CFG_ANON_MOUNT,
	CFG_ANON_SWAP,
	CFG_AUTO_MOUNT,
	CFG_AUTO_SWAP,
	CFG_DELAY_ROOT,
	CFG_CHECK_FS,
	__CFG_MAX
};

static const struct blobmsg_policy config_policy[__CFG_MAX] = {
	[CFG_ANON_SWAP] = { .name = "anon_swap", .type = BLOBMSG_TYPE_INT32 },
	[CFG_ANON_MOUNT] = { .name = "anon_mount", .type = BLOBMSG_TYPE_INT32 },
	[CFG_AUTO_SWAP] = { .name = "auto_swap", .type = BLOBMSG_TYPE_INT32 },
	[CFG_AUTO_MOUNT] = { .name = "auto_mount", .type = BLOBMSG_TYPE_INT32 },
	[CFG_DELAY_ROOT] = { .name = "delay_root", .type = BLOBMSG_TYPE_INT32 },
	[CFG_CHECK_FS] = { .name = "check_fs", .type = BLOBMSG_TYPE_INT32 },
};

enum {
	MOUNT_UUID,
	MOUNT_LABEL,
	MOUNT_ENABLE,
	MOUNT_TARGET,
	MOUNT_DEVICE,
	MOUNT_OPTIONS,
	__MOUNT_MAX
};

static const struct uci_blob_param_list config_attr_list = {
	.n_params = __CFG_MAX,
	.params = config_policy,
};

static const struct blobmsg_policy mount_policy[__MOUNT_MAX] = {
	[MOUNT_UUID] = { .name = "uuid", .type = BLOBMSG_TYPE_STRING },
	[MOUNT_LABEL] = { .name = "label", .type = BLOBMSG_TYPE_STRING },
	[MOUNT_DEVICE] = { .name = "device", .type = BLOBMSG_TYPE_STRING },
	[MOUNT_TARGET] = { .name = "target", .type = BLOBMSG_TYPE_STRING },
	[MOUNT_OPTIONS] = { .name = "options", .type = BLOBMSG_TYPE_STRING },
	[MOUNT_ENABLE] = { .name = "enabled", .type = BLOBMSG_TYPE_INT32 },
};

static const struct uci_blob_param_list mount_attr_list = {
	.n_params = __MOUNT_MAX,
	.params = mount_policy,
};

enum {
	SWAP_ENABLE,
	SWAP_UUID,
	SWAP_DEVICE,
	SWAP_PRIO,
	__SWAP_MAX
};

static const struct blobmsg_policy swap_policy[__SWAP_MAX] = {
	[SWAP_ENABLE] = { .name = "enabled", .type = BLOBMSG_TYPE_INT32 },
	[SWAP_UUID] = { .name = "uuid", .type = BLOBMSG_TYPE_STRING },
	[SWAP_DEVICE] = { .name = "device", .type = BLOBMSG_TYPE_STRING },
	[SWAP_PRIO] = { .name = "priority", .type = BLOBMSG_TYPE_INT32 },
};

static const struct uci_blob_param_list swap_attr_list = {
	.n_params = __SWAP_MAX,
	.params = swap_policy,
};

static char *blobmsg_get_strdup(struct blob_attr *attr)
{
	if (!attr)
		return NULL;

	return strdup(blobmsg_get_string(attr));
}

static int mount_add(struct uci_section *s)
{
	struct blob_attr *tb[__MOUNT_MAX] = { 0 };
	struct mount *m;

        blob_buf_init(&b, 0);
	uci_to_blob(&b, s, &mount_attr_list);
	blobmsg_parse(mount_policy, __MOUNT_MAX, tb, blob_data(b.head), blob_len(b.head));

	if (!tb[MOUNT_LABEL] && !tb[MOUNT_UUID] && !tb[MOUNT_DEVICE])
		return -1;

	if (tb[MOUNT_ENABLE] && !blobmsg_get_u32(tb[MOUNT_ENABLE]))
		return -1;

	m = malloc(sizeof(struct mount));
	m->type = TYPE_MOUNT;
	m->uuid = blobmsg_get_strdup(tb[MOUNT_UUID]);
	m->label = blobmsg_get_strdup(tb[MOUNT_LABEL]);
	m->target = blobmsg_get_strdup(tb[MOUNT_TARGET]);
	m->options = blobmsg_get_strdup(tb[MOUNT_OPTIONS]);
	m->device = blobmsg_get_strdup(tb[MOUNT_DEVICE]);
	m->overlay = m->extroot = 0;
	if (m->target && !strcmp(m->target, "/"))
		m->extroot = 1;
	if (m->target && !strcmp(m->target, "/overlay"))
		m->extroot = m->overlay = 1;

	if (m->uuid)
		vlist_add(&mounts, &m->node, m->uuid);
	else if (m->label)
		vlist_add(&mounts, &m->node, m->label);
	else if (m->device)
		vlist_add(&mounts, &m->node, m->device);

	return 0;
}

static int swap_add(struct uci_section *s)
{
	struct blob_attr *tb[__SWAP_MAX] = { 0 };
	struct mount *m;

        blob_buf_init(&b, 0);
	uci_to_blob(&b, s, &swap_attr_list);
	blobmsg_parse(swap_policy, __SWAP_MAX, tb, blob_data(b.head), blob_len(b.head));

	if (!tb[SWAP_UUID] && !tb[SWAP_DEVICE])
		return -1;

	m = malloc(sizeof(struct mount));
	memset(m, 0, sizeof(struct mount));
	m->type = TYPE_SWAP;
	m->uuid = blobmsg_get_strdup(tb[SWAP_UUID]);
	m->device = blobmsg_get_strdup(tb[SWAP_DEVICE]);
	if (tb[SWAP_PRIO])
		m->prio = blobmsg_get_u32(tb[SWAP_PRIO]);
	if (m->prio)
		m->prio = ((m->prio << SWAP_FLAG_PRIO_SHIFT) & SWAP_FLAG_PRIO_MASK) | SWAP_FLAG_PREFER;

	if ((!tb[SWAP_ENABLE]) || blobmsg_get_u32(tb[SWAP_ENABLE]))
		vlist_add(&mounts, &m->node, (m->uuid) ? (m->uuid) : (m->device));

	return 0;
}

static int global_add(struct uci_section *s)
{
	struct blob_attr *tb[__CFG_MAX] = { 0 };

        blob_buf_init(&b, 0);
	uci_to_blob(&b, s, &config_attr_list);
	blobmsg_parse(config_policy, __CFG_MAX, tb, blob_data(b.head), blob_len(b.head));

	if ((tb[CFG_ANON_MOUNT]) && blobmsg_get_u32(tb[CFG_ANON_MOUNT]))
		anon_mount = 1;
	if ((tb[CFG_ANON_SWAP]) && blobmsg_get_u32(tb[CFG_ANON_SWAP]))
		anon_swap = 1;

	if ((tb[CFG_AUTO_MOUNT]) && blobmsg_get_u32(tb[CFG_AUTO_MOUNT]))
		auto_mount = 1;
	if ((tb[CFG_AUTO_SWAP]) && blobmsg_get_u32(tb[CFG_AUTO_SWAP]))
		auto_swap = 1;

	if (tb[CFG_DELAY_ROOT])
		delay_root = blobmsg_get_u32(tb[CFG_DELAY_ROOT]);

	if ((tb[CFG_CHECK_FS]) && blobmsg_get_u32(tb[CFG_CHECK_FS]))
		check_fs = 1;

	return 0;
}

static struct mount* find_swap(const char *uuid, const char *device)
{
	struct mount *m;

	vlist_for_each_element(&mounts, m, node) {
		if (m->type != TYPE_SWAP)
			continue;
		if (uuid && m->uuid && !strcmp(m->uuid, uuid))
			return m;
		if (device && m->device && !strcmp(m->device, device))
			return m;
	}

	return NULL;
}

static struct mount* find_block(const char *uuid, const char *label, const char *device,
				const char *target)
{
	struct mount *m;

	vlist_for_each_element(&mounts, m, node) {
		if (m->type != TYPE_MOUNT)
			continue;
		if (m->uuid && uuid && !strcmp(m->uuid, uuid))
			return m;
		if (m->label && label && !strcmp(m->label, label))
			return m;
		if (m->target && target && !strcmp(m->target, target))
			return m;
		if (m->device && device && !strcmp(m->device, device))
			return m;
	}

	return NULL;
}

static void mounts_update(struct vlist_tree *tree, struct vlist_node *node_new,
			  struct vlist_node *node_old)
{
}

static int config_load(char *cfg)
{
	struct uci_context *ctx;
	struct uci_package *pkg;
	struct uci_element *e;

	vlist_init(&mounts, avl_strcmp, mounts_update);

	ctx = uci_alloc_context();
	if (cfg) {
		char path[32];
		snprintf(path, 32, "%s/etc/config", cfg);
		uci_set_confdir(ctx, path);
	}

	if (uci_load(ctx, "fstab", &pkg))
		return -1;

	vlist_update(&mounts);
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "mount"))
			mount_add(s);
		if (!strcmp(s->type, "swap"))
			swap_add(s);
		if (!strcmp(s->type, "global"))
			global_add(s);
	}
	vlist_flush(&mounts);

	return 0;
}

static int _cache_load(const char *path)
{
	int gl_flags = GLOB_NOESCAPE | GLOB_MARK;
	int j;
	glob_t gl;

	if (glob(path, gl_flags, NULL, &gl) < 0)
		return -1;

	for (j = 0; j < gl.gl_pathc; j++) {
		struct blkid_struct_probe *pr = malloc(sizeof(struct blkid_struct_probe));
		memset(pr, 0, sizeof(struct blkid_struct_probe));
		probe_block(gl.gl_pathv[j], pr);
		if (pr->err)
			free(pr);
		else
			list_add_tail(&pr->list, &devices);
	}

	globfree(&gl);

	return 0;
}

static void cache_load(int mtd)
{
	if (mtd)
		_cache_load("/dev/mtdblock*");
	_cache_load("/dev/mmcblk*");
	_cache_load("/dev/sd*");
}

static int print_block_info(struct blkid_struct_probe *pr)
{
	printf("%s:", pr->dev);
	if (pr->uuid[0])
		printf(" UUID=\"%s\"", pr->uuid);

	if (pr->label[0])
		printf(" LABEL=\"%s\"", pr->label);

	if (pr->name[0])
		printf(" NAME=\"%s\"", pr->name);

	if (pr->version[0])
		printf(" VERSION=\"%s\"", pr->version);

	printf(" TYPE=\"%s\"\n", pr->id->name);

	return 0;
}

static int print_block_uci(struct blkid_struct_probe *pr)
{
	if (!strcmp(pr->id->name, "swap")) {
		printf("config 'swap'\n");
	} else {
		printf("config 'mount'\n");
		printf("\toption\ttarget\t'/mnt/%s'\n", basename(pr->dev));
	}
	if (pr->uuid[0])
		printf("\toption\tuuid\t'%s'\n", pr->uuid);
	else
		printf("\toption\tdevice\t'%s'\n", basename(pr->dev));
	printf("\toption\tenabled\t'0'\n\n");

	return 0;
}

static struct blkid_struct_probe* find_block_info(char *uuid, char *label, char *path)
{
	struct blkid_struct_probe *pr = NULL;

	if (uuid)
		list_for_each_entry(pr, &devices, list)
			if (!strcmp(pr->uuid, uuid))
				return pr;

	if (label)
		list_for_each_entry(pr, &devices, list)
			if (strcmp(pr->label, label))
				return pr;

	if (path)
		list_for_each_entry(pr, &devices, list)
			if (!strcmp(pr->dev, path))
				return pr;

	return NULL;
}

static char* find_mount_point(char *block)
{
	FILE *fp = fopen("/proc/mounts", "r");
	static char line[256];
	int len = strlen(block);
	char *point = NULL;

	if(!fp)
		return NULL;

	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, block, len)) {
			char *p = &line[len + 1];
			char *t = strstr(p, " ");

			if (!t)
				return NULL;
			*t = '\0';
			point = p;
			break;
		}
	}

	fclose(fp);

	return point;
}

static void mkdir_p(char *dir)
{
	char *l = strrchr(dir, '/');

	if (l) {
		*l = '\0';
		mkdir_p(dir);
		*l = '/';
		mkdir(dir, 0755);
	}
}

static void check_filesystem(struct blkid_struct_probe *pr)
{
	pid_t pid;
	struct stat statbuf;
	char *e2fsck = "/usr/sbin/e2fsck";

	if (strncmp(pr->id->name, "ext", 3)) {
		fprintf(stderr, "check_filesystem: %s is not supported\n", pr->id->name);
		return;
	}

	if (stat(e2fsck, &statbuf) < 0)
		return;

	if (!(statbuf.st_mode & S_IXUSR))
		return;

	pid = fork();
	if (!pid) {
		execl(e2fsck, e2fsck, "-p", pr->dev, NULL);
		exit(-1);
	} else if (pid > 0) {
		int status;
		waitpid(pid, &status, 0);
	}
}

static int mount_device(struct blkid_struct_probe *pr, int hotplug)
{
	struct mount *m;
	char *device = basename(pr->dev);

	if (!pr)
		return -1;

	if (!strcmp(pr->id->name, "swap")) {
		if (hotplug && !auto_swap)
			return -1;
		m = find_swap(pr->uuid, device);
		if (m || anon_swap)
			swapon(pr->dev, (m) ? (m->prio) : (0));

		return 0;
	}

	if (hotplug && !auto_mount)
		return -1;

	if (find_mount_point(pr->dev)) {
		fprintf(stderr, "%s is already mounted\n", pr->dev);
		return -1;
	}

	m = find_block(pr->uuid, pr->label, device, NULL);
	if (m && m->extroot)
		return -1;

	if (m) {
		char *target = m->target;
		char _target[] = "/mnt/mmcblk123";
		int err = 0;

		if (!target) {
			snprintf(_target, sizeof(_target), "/mnt/%s", device);
			target = _target;
		}
		mkdir_p(target);

		if (check_fs)
			check_filesystem(pr);

		err = mount(pr->dev, target, pr->id->name, 0, (m->options) ? (m->options) : (""));
		if (err)
			fprintf(stderr, "mounting %s (%s) as %s failed (%d) - %s\n",
					pr->dev, pr->id->name, target, err, strerror(err));
		return err;
	}

	if (anon_mount) {
		char target[] = "/mnt/mmcblk123";
		int err = 0;

		snprintf(target, sizeof(target), "/mnt/%s", device);
		mkdir_p(target);

		if (check_fs)
			check_filesystem(pr);

		err = mount(pr->dev, target, pr->id->name, 0, "");
		if (err)
			fprintf(stderr, "mounting %s (%s) as %s failed (%d) - %s\n",
					pr->dev, pr->id->name, target, err, strerror(err));
		return err;
	}

	return 0;
}

static int umount_device(struct blkid_struct_probe *pr)
{
	struct mount *m;
	char *device = basename(pr->dev);
	char *mp;
	int err;

	if (!pr)
		return -1;

	if (!strcmp(pr->id->name, "swap"))
		return -1;

	mp = find_mount_point(pr->dev);
	if (!mp)
		return -1;

	m = find_block(pr->uuid, pr->label, device, NULL);
	if (m && m->extroot)
		return -1;

	err = umount2(mp, MNT_DETACH);
	if (err)
		fprintf(stderr, "unmounting %s (%s)  failed (%d) - %s\n",
			pr->dev, mp, err, strerror(err));
	else
		fprintf(stderr, "unmounted %s (%s)\n",
			pr->dev, mp);

	return err;
}

static int main_hotplug(int argc, char **argv)
{
	char path[32];
	char *action, *device, *mount_point;

	action = getenv("ACTION");
	device = getenv("DEVNAME");

	if (!action || !device)
		return -1;
	snprintf(path, sizeof(path), "/dev/%s", device);

	if (!strcmp(action, "remove")) {
		int err = 0;
		mount_point = find_mount_point(path);
		if (mount_point)
			err = umount2(mount_point, MNT_DETACH);

		if (err)
			fprintf(stderr, "umount of %s failed (%d) - %s\n",
					mount_point, err, strerror(err));

		return 0;
	} else if (strcmp(action, "add")) {
		fprintf(stderr, "Unkown action %s\n", action);

		return -1;
	}

	if (config_load(NULL))
		return -1;
	cache_load(0);

	return mount_device(find_block_info(NULL, NULL, path), 1);
}

static int find_block_mtd(char *name, char *part, int plen)
{
	FILE *fp = fopen("/proc/mtd", "r");
	static char line[256];
	char *index = NULL;

	if(!fp)
		return -1;

	while (!index && fgets(line, sizeof(line), fp)) {
		if (strstr(line, name)) {
			char *eol = strstr(line, ":");

			if (!eol)
				continue;

			*eol = '\0';
			index = &line[3];
		}
	}

	fclose(fp);

	if (!index)
		return -1;

	snprintf(part, plen, "/dev/mtdblock%s", index);

	return 0;
}

static int check_extroot(char *path)
{
	struct blkid_struct_probe *pr = NULL;
	char fs[32];

	if (find_block_mtd("rootfs", fs, sizeof(fs)))
		return -1;

	list_for_each_entry(pr, &devices, list) {
		if (!strcmp(pr->dev, fs)) {
			struct stat s;
			FILE *fp = NULL;
			char tag[32];
			char uuid[32] = { 0 };

			snprintf(tag, sizeof(tag), "%s/etc/.extroot-uuid", path);
			if (stat(tag, &s)) {
				fp = fopen(tag, "w+");
				if (!fp) {
					fprintf(stderr, "extroot: failed to write uuid tag file\n");
					/* return 0 to continue boot regardless of error */
					return 0;
				}
				fputs(pr->uuid, fp);
				fclose(fp);
				return 0;
			}

			fp = fopen(tag, "r");
			if (!fp) {
				fprintf(stderr, "extroot: failed to open uuid tag file\n");
				return -1;
			}

			fgets(uuid, sizeof(uuid), fp);
			fclose(fp);
			if (!strcmp(uuid, pr->uuid))
				return 0;
			fprintf(stderr, "extroot: uuid tag does not match rom uuid\n");
		}
	}
	return -1;
}

static int mount_extroot(char *cfg)
{
	char overlay[] = "/tmp/overlay";
	char mnt[] = "/tmp/mnt";
	char *path = mnt;
	struct blkid_struct_probe *pr;
	struct mount *m;
	int err = -1;

	if (config_load(cfg))
		return -2;

	m = find_block(NULL, NULL, NULL, "/");
	if (!m)
		m = find_block(NULL, NULL, NULL, "/overlay");

	if (!m || !m->extroot)
		return -1;

	pr = find_block_info(m->uuid, m->label, NULL);

	if (!pr && delay_root){
		fprintf(stderr, "extroot: is not ready yet, retrying in %ui seconds\n", delay_root);
		sleep(delay_root);
		pr = find_block_info(m->uuid, m->label, NULL);
	}
	if (pr) {
		if (strncmp(pr->id->name, "ext", 3)) {
			fprintf(stderr, "extroot: %s is not supported, try ext4\n", pr->id->name);
			return -1;
		}
		if (m->overlay)
			path = overlay;
		mkdir_p(path);

		if (check_fs)
			check_filesystem(pr);

		err = mount(pr->dev, path, pr->id->name, 0, (m->options) ? (m->options) : (""));

		if (err) {
			fprintf(stderr, "mounting %s (%s) as %s failed (%d) - %s\n",
					pr->dev, pr->id->name, path, err, strerror(err));
		} else if (m->overlay) {
			err = check_extroot(path);
			if (err)
				umount(path);
		}
	}

	return err;
}

static int main_extroot(int argc, char **argv)
{
	struct blkid_struct_probe *pr;
	char fs[32] = { 0 };
	char fs_data[32] = { 0 };
	int err = -1;

	if (!getenv("PREINIT"))
		return -1;

	if (argc != 2) {
		fprintf(stderr, "Usage: block extroot mountpoint\n");
		return -1;
	}

	mkblkdev();
	cache_load(1);

	find_block_mtd("rootfs", fs, sizeof(fs));
	if (!fs[0])
		return -2;

	pr = find_block_info(NULL, NULL, fs);
	if (!pr)
		return -3;

	find_block_mtd("rootfs_data", fs_data, sizeof(fs_data));
	if (fs_data[0]) {
		pr = find_block_info(NULL, NULL, fs_data);
		if (pr && !strcmp(pr->id->name, "jffs2")) {
			char cfg[] = "/tmp/jffs_cfg";

			mkdir_p(cfg);
			if (!mount(fs_data, cfg, "jffs2", MS_NOATIME, NULL)) {
				err = mount_extroot(cfg);
				umount2(cfg, MNT_DETACH);
			}
			if (err < 0)
				rmdir("/tmp/overlay");
			rmdir(cfg);
			return err;
		}
	}

	return mount_extroot(NULL);
}

static int main_mount(int argc, char **argv)
{
	struct blkid_struct_probe *pr;

	if (config_load(NULL))
		return -1;

	cache_load(0);
	list_for_each_entry(pr, &devices, list)
		mount_device(pr, 0);

	return 0;
}

static int main_umount(int argc, char **argv)
{
	struct blkid_struct_probe *pr;

	if (config_load(NULL))
		return -1;

	cache_load(0);
	list_for_each_entry(pr, &devices, list)
		umount_device(pr);

	return 0;
}

static int main_detect(int argc, char **argv)
{
	struct blkid_struct_probe *pr;

	cache_load(0);
	printf("config 'global'\n");
	printf("\toption\tanon_swap\t'0'\n");
	printf("\toption\tanon_mount\t'0'\n");
	printf("\toption\tauto_swap\t'1'\n");
	printf("\toption\tauto_mount\t'1'\n");
	printf("\toption\tdelay_root\t'0'\n");
	printf("\toption\tcheck_fs\t'0'\n\n");
	list_for_each_entry(pr, &devices, list)
		print_block_uci(pr);

	return 0;
}

static int main_info(int argc, char **argv)
{
	int i;
	struct blkid_struct_probe *pr;

	cache_load(1);
	if (argc == 2) {
		list_for_each_entry(pr, &devices, list)
			print_block_info(pr);

		return 0;
	};

	for (i = 2; i < argc; i++) {
		struct stat s;

		if (stat(argv[i], &s)) {
			fprintf(stderr, "failed to stat %s\n", argv[i]);
			continue;
		}
		if (!S_ISBLK(s.st_mode)) {
			fprintf(stderr, "%s is not a block device\n", argv[i]);
			continue;
		}
		pr = find_block_info(NULL, NULL, argv[i]);
		if (pr)
			print_block_info(pr);
	}

	return 0;
}

static int main_swapon(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: swapoff [-a] [DEVICE]\n\nStop swapping on DEVICE\n\n\t-a      Stop swapping on all swap devices\n");
		return -1;
	}

	if (!strcmp(argv[1], "-a")) {
		struct blkid_struct_probe *pr;

		cache_load(0);
		list_for_each_entry(pr, &devices, list) {
			if (strcmp(pr->id->name, "swap"))
				continue;
			if (swapon(pr->dev, 0))
				fprintf(stderr, "failed to swapon %s\n", pr->dev);
		}
	} else {
		struct stat s;
		int err;

		if (stat(argv[1], &s) || !S_ISBLK(s.st_mode)) {
			fprintf(stderr, "%s is not a block device\n", argv[1]);
			return -1;
		}
		err = swapon(argv[1], 0);
		if (err) {
			fprintf(stderr, "failed to swapon %s (%d)\n", argv[1], err);
			return err;
		}
	}

	return 0;
}

static int main_swapoff(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: swapoff [-a] [DEVICE]\n\nStop swapping on DEVICE\n\n\t-a      Stop swapping on all swap devices\n");
		return -1;
	}

	if (!strcmp(argv[1], "-a")) {
		FILE *fp = fopen("/proc/swaps", "r");
		char line[256];

		if (!fp) {
			fprintf(stderr, "failed to open /proc/swaps\n");
			return -1;
		}
		fgets(line, sizeof(line), fp);
		while (fgets(line, sizeof(line), fp)) {
			char *end = strchr(line, ' ');
			int err;

			if (!end)
				continue;
			*end = '\0';
			err = swapoff(line);
			if (err)
				fprintf(stderr, "failed to swapoff %s (%d)\n", line, err);
		}
		fclose(fp);
	} else {
		struct stat s;
		int err;

		if (stat(argv[1], &s) || !S_ISBLK(s.st_mode)) {
			fprintf(stderr, "%s is not a block device\n", argv[1]);
			return -1;
		}
		err = swapoff(argv[1]);
		if (err) {
			fprintf(stderr, "fsiled to swapoff %s (%d)\n", argv[1], err);
			return err;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	char *base = basename(*argv);

	umask(0);

	if (!strcmp(base, "swapon"))
		return main_swapon(argc, argv);

	if (!strcmp(base, "swapoff"))
		return main_swapoff(argc, argv);

	if ((argc > 1) && !strcmp(base, "block")) {
		if (!strcmp(argv[1], "info"))
			return main_info(argc, argv);

		if (!strcmp(argv[1], "detect"))
			return main_detect(argc, argv);

		if (!strcmp(argv[1], "hotplug"))
			return main_hotplug(argc, argv);

		if (!strcmp(argv[1], "extroot"))
			return main_extroot(argc, argv);

		if (!strcmp(argv[1], "mount"))
			return main_mount(argc, argv);

		if (!strcmp(argv[1], "umount"))
			return main_umount(argc, argv);
	}

	fprintf(stderr, "Usage: block <info|mount|umount|detect>\n");

	return -1;
}
