// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "cfgchk.h"
#include "hvisor.h"

#define CFGCHK_ERR(fmt, ...) pr_err("cfgchk: " fmt "\n", ##__VA_ARGS__)
#define CFGCHK_INFO(fmt, ...) pr_info("cfgchk: " fmt "\n", ##__VA_ARGS__)

static int cfgchk_validate_request(const struct cfgchk_request *req) {
    if (!req) {
        CFGCHK_ERR("null request pointer");
        return -EINVAL;
    }

    CFGCHK_INFO("received cfgchk request: version=%u", req->version);

    if (req->version != CFGCHK_IOCTL_VERSION) {
        CFGCHK_ERR("unsupported version %u (expect %u)", req->version,
                   CFGCHK_IOCTL_VERSION);
        return -EINVAL;
    }

    return 0;
}

static long cfgchk_ioctl(struct file *file, unsigned int cmd,
                         unsigned long arg) {
    struct cfgchk_request *req;
    long ret;

    switch (cmd) {
    case HVISOR_CFG_VALIDATE:
        req = kmalloc(sizeof(*req), GFP_KERNEL);
        if (!req)
            return -ENOMEM;
        if (copy_from_user(req, (void __user *)arg, sizeof(*req))) {
            kfree(req);
            return -EFAULT;
        }
        ret = cfgchk_validate_request(req);
        kfree(req);
        return ret;
    default:
        return -ENOIOCTLCMD;
    }
}

static const struct file_operations cfgchk_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = cfgchk_ioctl,
    .compat_ioctl = cfgchk_ioctl,
};

static struct miscdevice cfgchk_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "hvisor_cfgchk",
    .fops = &cfgchk_fops,
};

static int __init cfgchk_init(void) {
    int ret = misc_register(&cfgchk_misc_dev);
    if (ret) {
        CFGCHK_ERR("failed to register misc device (%d)", ret);
        return ret;
    }

    CFGCHK_INFO("communication interface ready");
    return 0;
}

static void __exit cfgchk_exit(void) {
    misc_deregister(&cfgchk_misc_dev);
    CFGCHK_INFO("communication interface removed");
}

module_init(cfgchk_init);
module_exit(cfgchk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex Assistant");
MODULE_DESCRIPTION("hvisor cfgchk communication stub");
