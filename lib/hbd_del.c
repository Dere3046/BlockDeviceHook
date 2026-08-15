// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/blkdev.h>

#include "hk.h"
#include "hk_inline.h"
#include "hbd.h"

typedef void (*hbd_del_disk_orig_fn)(struct gendisk *disk);
typedef int (*hbd_del_part_orig_fn)(struct gendisk *disk, int partno);
typedef int (*hbd_disk_changed_orig_fn)(struct gendisk *disk, bool invalidate);

static struct hk_inline g_del_disk_inline;
static struct hk_inline g_del_part_inline;
static struct hk_inline g_disk_changed_inline;
static hbd_del_disk_orig_fn g_del_disk_orig;
static hbd_del_part_orig_fn g_del_part_orig;
static hbd_disk_changed_orig_fn g_disk_changed_orig;

void hbd_stub_del_disk(void) __attribute__((naked));
void hbd_stub_del_disk(void)
{
	__asm__ volatile(".space 96");
}

void hbd_stub_del_part(void) __attribute__((naked));
void hbd_stub_del_part(void)
{
	__asm__ volatile(".space 96");
}

void hbd_stub_disk_changed(void) __attribute__((naked));
void hbd_stub_disk_changed(void)
{
	__asm__ volatile(".space 96");
}

void __nocfi hbd_del_disk_wrap(struct gendisk *gd)
{
	if (gd && hbd_lookup_disk_whole(MKDEV(gd->major, gd->first_minor)))
		return;
	g_del_disk_orig(gd);
}

int __nocfi hbd_del_part_wrap(struct gendisk *gd, int partno)
{
	if (gd && hbd_lookup_disk_part(MKDEV(gd->major, gd->first_minor),
				       partno))
		return -EACCES;
	return g_del_part_orig(gd, partno);
}

int __nocfi hbd_disk_changed_wrap(struct gendisk *gd, bool invalidate)
{
	if (gd && hbd_lookup_disk_any(MKDEV(gd->major, gd->first_minor)))
		return -EBUSY;
	return g_disk_changed_orig(gd, invalidate);
}

int hbd_del_init(void)
{
	int ret;

	ret = hk_inline_hook(&g_del_disk_inline, "del_gendisk",
			     "hbd_stub_del_disk", "hbd_del_disk_wrap");
	if (ret)
		return ret;
	g_del_disk_orig = (hbd_del_disk_orig_fn)g_del_disk_inline.stub;

	ret = hk_inline_hook(&g_del_part_inline, "bdev_del_partition",
			     "hbd_stub_del_part", "hbd_del_part_wrap");
	if (ret)
		goto err;
	g_del_part_orig = (hbd_del_part_orig_fn)g_del_part_inline.stub;

	ret = hk_inline_hook(&g_disk_changed_inline, "bdev_disk_changed",
			     "hbd_stub_disk_changed", "hbd_disk_changed_wrap");
	if (ret)
		goto err;
	g_disk_changed_orig =
		(hbd_disk_changed_orig_fn)g_disk_changed_inline.stub;
	return 0;

err:
	hk_inline_unhook(&g_del_part_inline);
	hk_inline_unhook(&g_del_disk_inline);
	return ret;
}

void hbd_del_exit(void)
{
	hk_inline_unhook(&g_disk_changed_inline);
	hk_inline_unhook(&g_del_part_inline);
	hk_inline_unhook(&g_del_disk_inline);
}
