// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "hbd.h"
#include "hk.h"

extern unsigned long (*kallrecon_klp)(const char *name);
extern void find_kallsyms_base(void);

static bool g_verbose;
module_param_named(verbose, g_verbose, bool, 0444);
MODULE_PARM_DESC(verbose, "log protected read hits via the demo callback");

static char *g_protect;
module_param_named(protect, g_protect, charp, 0444);
MODULE_PARM_DESC(protect, "name:FLAG|FLAG,name2:FLAG|FLAG, e.g. sda1:WR_SHADOW|RD_ZERO");

static unsigned long __nocfi hbd_resolve(const char *name)
{
	if (kallrecon_klp)
		return kallrecon_klp(name);
	return 0;
}

static int hbd_demo_cb(struct hbd_target *t, struct bio *bio, u64 sect)
{
	pr_info("hbd: read hit %s sector=%llu size=%u\n", t->name,
		(unsigned long long)sect, bio->bi_iter.bi_size);
	return 0;
}

static int hbd_parse_flags(char *s, u32 *flags)
{
	char *tok;
	u32 f = 0;

	while ((tok = strsep(&s, "|"))) {
		if (!strcmp(tok, "RD_BLOCK"))
			f |= HBD_RD_BLOCK;
		else if (!strcmp(tok, "RD_ZERO"))
			f |= HBD_RD_ZERO;
		else if (!strcmp(tok, "WR_SINK"))
			f |= HBD_WR_SINK;
		else if (!strcmp(tok, "WR_SHADOW"))
			f |= HBD_WR_SHADOW;
		else if (!strcmp(tok, "NODEL_DISK"))
			f |= HBD_NODEL_DISK;
		else if (!strcmp(tok, "NODEL_PART"))
			f |= HBD_NODEL_PART;
		else
			return -EINVAL;
	}
	*flags = f;
	return 0;
}

static void hbd_parse_param(char *s)
{
	char *entry;

	while ((entry = strsep(&s, ","))) {
		char *name = strsep(&entry, ":");
		u32 flags;
		int ret;

		if (!name || !*name || !entry || !*entry) {
			pr_warn("hbd: bad param entry '%s'\n", entry);
			continue;
		}
		if (hbd_parse_flags(entry, &flags)) {
			pr_warn("hbd: bad flags '%s'\n", entry);
			continue;
		}
		ret = hbd_protect(name, flags, g_verbose ? hbd_demo_cb : NULL,
				  NULL);
		pr_info("hbd: protect %s flags=%u -> %d\n", name, flags, ret);
	}
}

static int hbd_show_one(struct hbd_target *t, void *arg)
{
	struct seq_file *m = arg;
	char flags[128] = "";
	int n = 0;

	if (t->flags & HBD_RD_BLOCK)
		n += scnprintf(flags + n, sizeof(flags) - n, "RD_BLOCK|");
	if (t->flags & HBD_RD_ZERO)
		n += scnprintf(flags + n, sizeof(flags) - n, "RD_ZERO|");
	if (t->flags & HBD_WR_SINK)
		n += scnprintf(flags + n, sizeof(flags) - n, "WR_SINK|");
	if (t->flags & HBD_WR_SHADOW)
		n += scnprintf(flags + n, sizeof(flags) - n, "WR_SHADOW|");
	if (t->flags & HBD_NODEL_DISK)
		n += scnprintf(flags + n, sizeof(flags) - n, "NODEL_DISK|");
	if (t->flags & HBD_NODEL_PART)
		n += scnprintf(flags + n, sizeof(flags) - n, "NODEL_PART|");
	if (n)
		flags[n - 1] = 0;

	seq_printf(m, "%s dev=%u:%u disk=%u:%u part=%u flags=%s\n",
		   t->name, MAJOR(t->dev), MINOR(t->dev),
		   MAJOR(t->disk_dev), MINOR(t->disk_dev), t->partno, flags);
	return 0;
}

static int hbd_proc_show(struct seq_file *m, void *v)
{
	return hbd_for_each(hbd_show_one, m);
}

static ssize_t hbd_proc_write(struct file *f, const char __user *ubuf,
			      size_t len, loff_t *off)
{
	char buf[256];
	char *p = buf;
	char *cmd, *name, *fl;
	u32 flags;
	int ret;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	cmd = strim(strsep(&p, " \t"));
	if (!cmd)
		return -EINVAL;

	if (!strcmp(cmd, "add")) {
		name = strim(strsep(&p, " \t"));
		fl = strim(strsep(&p, " \t"));
		if (!name || !fl)
			return -EINVAL;
		ret = hbd_parse_flags(fl, &flags);
		if (ret)
			return ret;
		ret = hbd_protect(name, flags,
				  g_verbose ? hbd_demo_cb : NULL, NULL);
		if (ret)
			return ret;
	} else if (!strcmp(cmd, "del")) {
		name = strim(strsep(&p, " \t"));
		if (!name)
			return -EINVAL;
		ret = hbd_unprotect(name);
		if (ret)
			return ret;
	} else if (!strcmp(cmd, "clear")) {
		hbd_clear();
	} else {
		return -EINVAL;
	}
	return len;
}

static int hbd_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hbd_proc_show, NULL);
}

static const struct proc_ops hbd_proc_ops = {
	.proc_open = hbd_proc_open,
	.proc_read = seq_read,
	.proc_write = hbd_proc_write,
	.proc_release = single_release,
};

static struct proc_dir_entry *g_hbd_proc;

static int __init hbd_consumer_init(void)
{
	struct hk_cfg cfg = {
		.resolve = hbd_resolve,
	};
	int ret;

	find_kallsyms_base();
	if (!hbd_resolve("submit_bio_noacct")) {
		pr_warn("hbd: kallsyms recovery failed\n");
		return -ENODATA;
	}

	ret = hk_init(&cfg);
	if (ret)
		return ret;

	ret = hbd_init();
	if (ret) {
		hk_exit();
		return ret;
	}

	g_hbd_proc = proc_create("hbd", 0644, NULL, &hbd_proc_ops);
	if (!g_hbd_proc)
		pr_warn("hbd: proc create failed\n");

	if (g_protect)
		hbd_parse_param(g_protect);

	pr_info("hbd: loaded\n");
	return 0;
}

static void __exit hbd_consumer_exit(void)
{
	if (g_hbd_proc)
		proc_remove(g_hbd_proc);
	hbd_exit();
	hk_exit();
	pr_info("hbd: unloaded\n");
}

module_init(hbd_consumer_init);
module_exit(hbd_consumer_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("hbd: block device protection library consumer");
