// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef HBD_H
#define HBD_H

#include <linux/blkdev.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>

#define HBD_NAME_MAX 64

enum hbd_flag {
	HBD_RD_BLOCK	= 1 << 0,
	HBD_RD_ZERO	= 1 << 1,
	HBD_WR_SINK	= 1 << 2,
	HBD_WR_SHADOW	= 1 << 3,
	HBD_NODEL_DISK	= 1 << 4,
	HBD_NODEL_PART	= 1 << 5,
};

struct hbd_target;
struct hbd_shadow;

typedef int (*hbd_read_cb)(struct hbd_target *t, struct bio *bio, u64 sect);

struct hbd_target {
	char name[HBD_NAME_MAX];
	dev_t dev;
	dev_t disk_dev;
	unsigned int partno;
	struct request_queue *queue;
	u32 flags;
	hbd_read_cb on_read;
	void *priv;
	struct hbd_shadow *shadow;
	struct list_head node;
	struct kref ref;
	struct rcu_head rcu;
};

int hbd_init(void);
void hbd_exit(void);

int hbd_protect(const char *name, u32 flags, hbd_read_cb cb, void *priv);
int hbd_unprotect(const char *name);
void hbd_clear(void);
int hbd_for_each(int (*fn)(struct hbd_target *t, void *arg), void *arg);
void hbd_set_shadow_cap(size_t max_pages);

struct hbd_target *hbd_lookup_dev(dev_t dev);
struct hbd_target *hbd_lookup_queue(struct request_queue *q);
bool hbd_lookup_disk_whole(dev_t ddev);
bool hbd_lookup_disk_part(dev_t ddev, int partno);
bool hbd_lookup_disk_any(dev_t ddev);
void hbd_put_target(struct hbd_target *t);

int hbd_io_init(void);
void hbd_io_exit(void);
int hbd_del_init(void);
void hbd_del_exit(void);

#endif
