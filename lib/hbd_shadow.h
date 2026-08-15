// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef HBD_SHADOW_H
#define HBD_SHADOW_H

#include <linux/bio.h>
#include <linux/types.h>

struct hbd_shadow {
	struct xarray xa;
	struct mutex lock;
	size_t nr_pages;
	size_t max_pages;
};

struct hbd_shadow *hbd_shadow_alloc(size_t max_pages);
void hbd_shadow_free(struct hbd_shadow *s);
int hbd_shadow_write(struct hbd_shadow *s, struct bio *bio, u64 sect);
int hbd_shadow_zero(struct hbd_shadow *s, u64 sect, u64 bytes);
int hbd_shadow_read(struct hbd_shadow *s, struct bio *bio, u64 sect,
		    bool zero_missing);
void hbd_bio_zero(struct bio *bio);
size_t hbd_shadow_pages(struct hbd_shadow *s);

#endif
