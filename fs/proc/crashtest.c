// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static ssize_t crashtest_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	char cmd;

	if (!count)
		return 0;

	if (get_user(cmd, buf))
		return -EFAULT;

	switch (cmd) {
	case 'p':
		panic("crashtest");
		break;
	case 'w':
		WARN(1, "crashtest");
		break;
	case 'o':
		*(int*)0 = 0;
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static const struct file_operations proc_crashtest_ops = {
	.write	= crashtest_write,
};

static int __init proc_crashtest_init(void)
{
	return !proc_create("crashtest", 0220, NULL, &proc_crashtest_ops);
}
fs_initcall(proc_crashtest_init);
