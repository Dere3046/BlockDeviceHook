// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/blkdev.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "hbd.h"
#include "hbd_shadow.h"

static LIST_HEAD(g_targets);
static DEFINE_MUTEX(g_lock);
static size_t g_shadow_cap;

struct hbd_open {
	struct block_device *bdev;
	struct file *file;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static int hbd_open_path(const char *path, struct hbd_open *o)
{
	o->file = bdev_file_open_by_path(path, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(o->file))
		return PTR_ERR(o->file);
	o->bdev = file_bdev(o->file);
	return 0;
}

static void hbd_close_path(struct hbd_open *o)
{
	bdev_fput(o->file);
}
#else
static int hbd_open_path(const char *path, struct hbd_open *o)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	o->bdev = blkdev_get_by_path(path, BLK_OPEN_READ, NULL, NULL);
#else
	o->bdev = blkdev_get_by_path(path, FMODE_READ, NULL);
#endif
	if (IS_ERR(o->bdev))
		return PTR_ERR(o->bdev);
	return 0;
}

static void hbd_close_path(struct hbd_open *o)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	blkdev_put(o->bdev, NULL);
#else
	blkdev_put(o->bdev, FMODE_READ);
#endif
}
#endif

static int hbd_open_name(const char *name, struct hbd_open *o)
{
	char path[256];
	int ret;

	if (name[0] == '/')
		return hbd_open_path(name, o);
	if (snprintf(path, sizeof(path), "/dev/block/by-name/%s", name) < 0)
		return -EINVAL;
	ret = hbd_open_path(path, o);
	if (!ret)
		return 0;
	if (snprintf(path, sizeof(path), "/dev/%s", name) < 0)
		return -EINVAL;
	return hbd_open_path(path, o);
}

static void hbd_target_free_rcu(struct rcu_head *rcu)
{
	struct hbd_target *t = container_of(rcu, struct hbd_target, rcu);

	hbd_shadow_free(t->shadow);
	kfree(t);
}

static void hbd_target_release(struct kref *ref)
{
	struct hbd_target *t = container_of(ref, struct hbd_target, ref);

	call_rcu(&t->rcu, hbd_target_free_rcu);
}

void hbd_put_target(struct hbd_target *t)
{
	kref_put(&t->ref, hbd_target_release);
}

struct hbd_target *hbd_lookup_dev(dev_t dev)
{
	struct hbd_target *t;

	rcu_read_lock();
	list_for_each_entry_rcu(t, &g_targets, node) {
		if (t->dev == dev && kref_get_unless_zero(&t->ref)) {
			rcu_read_unlock();
			return t;
		}
	}
	rcu_read_unlock();
	return NULL;
}

struct hbd_target *hbd_lookup_queue(struct request_queue *q)
{
	struct hbd_target *t;

	rcu_read_lock();
	list_for_each_entry_rcu(t, &g_targets, node) {
		if (t->queue == q && kref_get_unless_zero(&t->ref)) {
			rcu_read_unlock();
			return t;
		}
	}
	rcu_read_unlock();
	return NULL;
}

bool hbd_lookup_disk_whole(dev_t ddev)
{
	struct hbd_target *t;
	bool found = false;

	rcu_read_lock();
	list_for_each_entry_rcu(t, &g_targets, node) {
		if (t->partno == 0 && t->disk_dev == ddev &&
		    (t->flags & HBD_NODEL_DISK)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

bool hbd_lookup_disk_part(dev_t ddev, int partno)
{
	struct hbd_target *t;
	bool found = false;

	rcu_read_lock();
	list_for_each_entry_rcu(t, &g_targets, node) {
		if (t->partno == partno && t->disk_dev == ddev &&
		    (t->flags & HBD_NODEL_PART)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

bool hbd_lookup_disk_any(dev_t ddev)
{
	struct hbd_target *t;
	bool found = false;

	rcu_read_lock();
	list_for_each_entry_rcu(t, &g_targets, node) {
		if (t->disk_dev == ddev) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

void hbd_set_shadow_cap(size_t max_pages)
{
	g_shadow_cap = max_pages;
}

int hbd_protect(const char *name, u32 flags, hbd_read_cb cb, void *priv)
{
	struct hbd_open o;
	struct hbd_target *t, *iter;
	struct gendisk *gd;
	u32 rd, wr;
	int ret;

	if (!name || !name[0])
		return -EINVAL;
	rd = flags & (HBD_RD_BLOCK | HBD_RD_ZERO);
	wr = flags & (HBD_WR_SINK | HBD_WR_SHADOW);
	if (!(flags & (HBD_RD_BLOCK | HBD_RD_ZERO | HBD_WR_SINK | HBD_WR_SHADOW |
		       HBD_NODEL_DISK | HBD_NODEL_PART)))
		return -EINVAL;
	if (rd == (HBD_RD_BLOCK | HBD_RD_ZERO))
		return -EINVAL;
	if (wr == (HBD_WR_SINK | HBD_WR_SHADOW))
		return -EINVAL;
	if ((flags & HBD_NODEL_DISK) && (flags & HBD_NODEL_PART))
		return -EINVAL;

	ret = hbd_open_name(name, &o);
	if (ret)
		return ret;

	gd = o.bdev->bd_disk;
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		hbd_close_path(&o);
		return -ENOMEM;
	}
	kref_init(&t->ref);
	strscpy(t->name, name, sizeof(t->name));
	t->dev = o.bdev->bd_dev;
	t->disk_dev = MKDEV(gd->major, gd->first_minor);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	t->partno = bdev_partno(o.bdev);
#else
	t->partno = o.bdev->bd_partno;
#endif
	t->queue = bdev_get_queue(o.bdev);
	t->flags = flags;
	t->on_read = cb;
	t->priv = priv;
	if (t->partno && (flags & HBD_NODEL_DISK)) {
		ret = -EINVAL;
		goto out_free;
	}
	if (!t->partno && (flags & HBD_NODEL_PART)) {
		ret = -EINVAL;
		goto out_free;
	}
	if (flags & HBD_WR_SHADOW) {
		t->shadow = hbd_shadow_alloc(g_shadow_cap);
		if (!t->shadow) {
			ret = -ENOMEM;
			goto out_free;
		}
	}
	hbd_close_path(&o);

	mutex_lock(&g_lock);
	list_for_each_entry(iter, &g_targets, node) {
		if (!strcmp(iter->name, name)) {
			mutex_unlock(&g_lock);
			hbd_shadow_free(t->shadow);
			kfree(t);
			return -EEXIST;
		}
	}
	list_add_tail_rcu(&t->node, &g_targets);
	mutex_unlock(&g_lock);
	return 0;

out_free:
	hbd_shadow_free(t->shadow);
	kfree(t);
	hbd_close_path(&o);
	return ret;
}

int hbd_unprotect(const char *name)
{
	struct hbd_target *t;
	int ret = -ENOENT;

	mutex_lock(&g_lock);
	list_for_each_entry(t, &g_targets, node) {
		if (!strcmp(t->name, name)) {
			list_del_rcu(&t->node);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&g_lock);
	if (ret)
		return ret;
	synchronize_rcu();
	hbd_put_target(t);
	return 0;
}

void hbd_clear(void)
{
	struct hbd_target *t, *tmp;
	LIST_HEAD(discard);

	mutex_lock(&g_lock);
	list_for_each_entry_safe(t, tmp, &g_targets, node) {
		list_del_rcu(&t->node);
		list_add_tail(&t->node, &discard);
	}
	mutex_unlock(&g_lock);
	synchronize_rcu();
	list_for_each_entry_safe(t, tmp, &discard, node)
		hbd_put_target(t);
}

int hbd_init(void)
{
	int ret;

	hbd_shadow_kmap_init();
	ret = hbd_io_init();
	if (ret)
		return ret;
	ret = hbd_del_init();
	if (ret)
		hbd_io_exit();
	return ret;
}

void hbd_exit(void)
{
	hbd_del_exit();
	hbd_io_exit();
	hbd_clear();
}

int hbd_for_each(int (*fn)(struct hbd_target *t, void *arg), void *arg)
{
	struct hbd_target *t;
	int ret = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(t, &g_targets, node) {
		ret = fn(t, arg);
		if (ret)
			break;
	}
	rcu_read_unlock();
	return ret;
}
