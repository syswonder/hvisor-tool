// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 *      https://www.syswonder.org
 *
 * Authors:
 *      Guowei Li <2401213322@stu.pku.edu.cn>
 */
#include <asm/cacheflush.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/clk.h>

#include "hvisor.h"
#include "zone_config.h"

/* Clock provider phandle, hardcoded as requested */
#define CLOCK_PROVIDER_PHANDLE 2

extern bool __clk_is_enabled(const struct clk *clk);
extern const char *__clk_get_name(const struct clk *clk);
extern unsigned long clk_get_rate(struct clk *clk);
extern int clk_set_rate(struct clk *clk, unsigned long rate);
extern int clk_prepare_enable(struct clk *clk);
extern void clk_disable_unprepare(struct clk *clk);

/**
 * get_clock_provider_node - 获取时钟提供者节点
 *
 * 返回: 成功时返回时钟提供者节点指针，失败时返回NULL
 */
static struct device_node *get_clock_provider_node(void) {
    struct device_node *provider_np = of_find_node_by_phandle(CLOCK_PROVIDER_PHANDLE);
    if (!provider_np) {
        pr_err("Failed to find clock provider node\n");
    }
    return provider_np;
}

static struct clk *get_clock_by_id(u32 clk_id, struct device_node *provider_np) {
    struct of_phandle_args clkspec;
    
    if (!provider_np) {
        return ERR_PTR(-ENODEV);
    }
    
    clkspec.np = provider_np;
    clkspec.args[0] = clk_id;
    clkspec.args_count = 1;
    
    // pr_info("clk_id = %u, now try to get clock...\n", clk_id);
    return of_clk_get_from_provider(&clkspec);
}

static int get_clock_count(void) {
    struct device_node *provider_np;
    struct clk *clk;
    int count = 0;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    while (1) {
        clk = get_clock_by_id(count, provider_np);

        if (IS_ERR(clk)) {
            if (PTR_ERR(clk) == -EINVAL) break; // idx >= provider->data->clk_num, which means no more clocks
        } else {
            clk_put(clk);
        }
        
        count++;
    }

    of_node_put(provider_np);
    pr_info("total clocks = %d\n", count);
    return count;
}



static int get_clock_config(u32 clock_id, u32 *config, u32 *extended_config_val) {
    struct device_node *provider_np;
    struct clk *clk;
    u32 clk_config = 0;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    clk = get_clock_by_id(clock_id, provider_np);
    if (IS_ERR(clk)) {
        of_node_put(provider_np);
        if (PTR_ERR(clk) == -ENOENT)
            return -ENOENT;
        return PTR_ERR(clk);
    }

    // Bit 0: Clock enabled status
    if (__clk_is_enabled(clk)) {
        clk_config |= 1;
    }

    *config = clk_config;
    *extended_config_val = 0; // Not implemented

    clk_put(clk);
    of_node_put(provider_np);
    
    pr_info("clock[%u] config=0x%x, extended_config_val=0x%x\n", clock_id, *config, *extended_config_val);
    return 0;
}

static int get_clock_name(u32 clock_id, char *name) {
    struct device_node *provider_np;
    struct clk *clk;
    const char *clk_name;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    clk = get_clock_by_id(clock_id, provider_np);
    if (IS_ERR(clk)) {
        of_node_put(provider_np);
        if (PTR_ERR(clk) == -ENOENT)
            return -ENOENT;
        return PTR_ERR(clk);
    }

    clk_name = __clk_get_name(clk);
    if (clk_name) {
        strncpy(name, clk_name, 63);
        name[63] = '\0';
    } else {
        snprintf(name, 64, "unknown_clk_%u", clock_id);
    }

    clk_put(clk);
    of_node_put(provider_np);
    
    pr_info("clock[%u] name=%s\n", clock_id, name);
    return 0;
}

static int get_clock_attributes(u32 clock_id, u32 *enabled, u32 *parent_id, char *clock_name, u32 *is_valid) {
    struct device_node *provider_np;
    struct clk *clk, *parent_clk;
    const char *name;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    clk = get_clock_by_id(clock_id, provider_np);
    if (IS_ERR(clk)) {
        of_node_put(provider_np);
        *is_valid = 0; // Clock is invalid
        *enabled = 0;
        *parent_id = -1;
        snprintf(clock_name, 64, "invalid_clock_%u", clock_id);
        return 0;
    }
    
    *is_valid = 1; // Clock is valid

    // Get clock enable status
    *enabled = __clk_is_enabled(clk) ? 1 : 0;

    // Get parent clock
    parent_clk = clk_get_parent(clk);
    if (IS_ERR(parent_clk) || !parent_clk) {
        *parent_id = -1; // No parent
    } else {
        // For now, we'll set a default parent_id
        // In a more complete implementation, we'd need to find the parent's index
        *parent_id = -1; // Simplified for now
    }

    // Get clock name
    name = __clk_get_name(clk);
    if (name) {
        strncpy(clock_name, name, 63);
        clock_name[63] = '\0';
    } else {
        snprintf(clock_name, 64, "unknown_clk_%u", clock_id);
    }

    clk_put(clk);
    of_node_put(provider_np);
    
    pr_info("clock[%u] name=%s enabled=%u parent_id=%d is_valid=%u\n", clock_id, clock_name, *enabled, *parent_id, *is_valid);
    return 0;
}

static int get_clock_rate(u32 clock_id, u64 *rate) {
    struct device_node *provider_np;
    struct clk *clk;
    unsigned long clk_rate;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    clk = get_clock_by_id(clock_id, provider_np);
    if (IS_ERR(clk)) {
        of_node_put(provider_np);
        if (PTR_ERR(clk) == -ENOENT)
            return -ENOENT;
        return PTR_ERR(clk);
    }

    clk_rate = clk_get_rate(clk);
    *rate = (u64)clk_rate;

    clk_put(clk);
    of_node_put(provider_np);
    
    pr_info("clock[%u] rate=%llu Hz\n", clock_id, *rate);
    return 0;
}

static int set_clock_rate(u32 clock_id, u64 rate) {
    struct device_node *provider_np;
    struct clk *clk;
    int ret;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    clk = get_clock_by_id(clock_id, provider_np);
    if (IS_ERR(clk)) {
        of_node_put(provider_np);
        if (PTR_ERR(clk) == -ENOENT)
            return -ENOENT;
        return PTR_ERR(clk);
    }

    ret = clk_set_rate(clk, (unsigned long)rate);

    clk_put(clk);
    of_node_put(provider_np);
    
    if (ret == 0) {
        pr_info("clock[%u] rate set to %llu Hz\n", clock_id, rate);
    } else {
        pr_err("Failed to set clock[%u] rate to %llu Hz, error=%d\n", clock_id, rate, ret);
    }
    
    return ret;
}

static int set_clock_config(u32 clock_id, u32 config) {
    struct device_node *provider_np;
    struct clk *clk;
    int ret = 0;

    provider_np = get_clock_provider_node();
    if (!provider_np) {
        return -ENODEV;
    }

    clk = get_clock_by_id(clock_id, provider_np);
    if (IS_ERR(clk)) {
        of_node_put(provider_np);
        if (PTR_ERR(clk) == -ENOENT)
            return -ENOENT;
        return PTR_ERR(clk);
    }

    // Bit 0: Clock enable/disable
    if (config & 1) {
        // Enable clock
        ret = clk_prepare_enable(clk);
        if (ret) {
            pr_err("Failed to enable clock[%u], error=%d\n", clock_id, ret);
        } else {
            pr_info("clock[%u] enabled\n", clock_id);
        }
    } else {
        // Disable clock
        clk_disable_unprepare(clk);
        pr_info("clock[%u] disabled\n", clock_id);
    }

    clk_put(clk);
    of_node_put(provider_np);
    
    return ret;
}

// Encapsulate the function to handle SCMI clock ioctl
static int hvisor_scmi_clock_ioctl(struct hvisor_scmi_clock_args __user *user_args) {
    struct hvisor_scmi_clock_args args;
    
    if (copy_from_user(&args, user_args, sizeof(struct hvisor_scmi_clock_args)))
        return -EFAULT;
    
    pr_info("SCMI clock ioctl, subcmd=%d\n", args.subcmd);

    switch (args.subcmd) {
    case HVISOR_SCMI_CLOCK_GET_COUNT: {
        int ret = get_clock_count();
        if (ret < 0)
            return ret;
        args.u.clock_count = (u32)ret;
        if (copy_to_user(&user_args->u.clock_count, &args.u.clock_count, sizeof(u32)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_GET_ATTRIBUTES: {
        u32 enabled, parent_id, is_valid;
        char clock_name[64];
        int ret = get_clock_attributes(args.u.clock_attr.clock_id, &enabled, &parent_id, clock_name, &is_valid);
        args.u.clock_attr.enabled = enabled;
        args.u.clock_attr.parent_id = parent_id;
        args.u.clock_attr.is_valid = is_valid;
        strncpy(args.u.clock_attr.clock_name, clock_name, 63);
        args.u.clock_attr.clock_name[63] = '\0';
        if (copy_to_user(&user_args->u.clock_attr, &args.u.clock_attr, sizeof(args.u.clock_attr)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_DESCRIBE_RATES: {
        // u32 num_rates, remaining;
        // u64 rates[8];
        
        // int ret = get_clock_rates(args.u.clock_rates_info.clock_id, 
        //                          args.u.clock_rates_info.rate_index, 
        //                          &num_rates, &remaining, rates);
        // if (ret < 0)
        //     return ret;
        
        // args.u.clock_rates_info.num_rates = num_rates;
        // args.u.clock_rates_info.remaining = remaining;
        
        // for (int i = 0; i < num_rates; i++) {
        //     args.u.clock_rates_info.rates[i] = rates[i];
        // }
        
        // if (copy_to_user(&user_args->u.clock_rates_info, &args.u.clock_rates_info, sizeof(args.u.clock_rates_info)))
        //     return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_RATE_GET: {
        u64 rate = 0;
        int ret = get_clock_rate(args.u.clock_rate_info.clock_id, &rate); // ignore return value
        args.u.clock_rate_info.rate = rate;
        if (copy_to_user(&user_args->u.clock_rate_info, &args.u.clock_rate_info, sizeof(args.u.clock_rate_info)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_RATE_SET: {
        return set_clock_rate(args.u.clock_rate_set_info.clock_id, args.u.clock_rate_set_info.rate);
    }
    case HVISOR_SCMI_CLOCK_CONFIG_GET: {
        u32 config, extended_config_val;
        int ret = get_clock_config(args.u.clock_config_info.clock_id, &config, &extended_config_val);
        if (ret < 0)
            return ret;
        args.u.clock_config_info.config = config;
        args.u.clock_config_info.extended_config_val = extended_config_val;
        if (copy_to_user(&user_args->u.clock_config_info, &args.u.clock_config_info, sizeof(args.u.clock_config_info)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_CONFIG_SET: {
        return set_clock_config(args.u.clock_config_info.clock_id, args.u.clock_config_info.config);
    }
    case HVISOR_SCMI_CLOCK_NAME_GET: {
        char name[64];
        int ret = get_clock_name(args.u.clock_name_info.clock_id, name);
        if (ret < 0)
            return ret;
        strncpy(args.u.clock_name_info.name, name, 63);
        args.u.clock_name_info.name[63] = '\0';
        if (copy_to_user(&user_args->u.clock_name_info, &args.u.clock_name_info, sizeof(args.u.clock_name_info)))
            return -EFAULT;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

struct virtio_bridge *virtio_bridge;
int virtio_irq = -1;
static struct task_struct *task = NULL;

// initial virtio el2 shared region
static int hvisor_init_virtio(void) {
    int err;
    if (virtio_irq == -1) {
        pr_err("virtio device is not available\n");
        return ENOTTY;
    }
    virtio_bridge = (struct virtio_bridge *)__get_free_pages(GFP_KERNEL, 0);
    if (virtio_bridge == NULL)
        return -ENOMEM;
    SetPageReserved(virt_to_page(virtio_bridge));
    // init device region
    memset(virtio_bridge, 0, sizeof(struct virtio_bridge));
    err = hvisor_call(HVISOR_HC_INIT_VIRTIO, __pa(virtio_bridge), 0);
    if (err)
        return err;
    return 0;
}

// finish virtio req and send result to el2
static int hvisor_finish_req(void) {
    int err;
    err = hvisor_call(HVISOR_HC_FINISH_REQ, 0, 0);
    if (err)
        return err;
    return 0;
}

// static int flush_cache(__u64 phys_start, __u64 size)
// {
//     void __iomem *vaddr;
//     int err = 0;

//     size = PAGE_ALIGN(size);

//     // Use ioremap to map physical address
//     vaddr = ioremap_cache(phys_start, size);
//     if (!vaddr) {
//         pr_err("hvisor.ko: failed to ioremap image\n");
//         return -ENOMEM;
//     }

//     // flush I-cache (flush_icache_range handles both D/I cache on ARM64 platform)
//     flush_icache_range((unsigned long)vaddr, (unsigned long)vaddr + size);

//     // Unmap
//     iounmap(vaddr);
//     return err;
// }
// static int flush_cache(__u64 phys_start, __u64 size)
// {
//     struct vm_struct *vma;
//     int err = 0;
//     size = PAGE_ALIGN(size);
//     vma = get_vm_area(size, VM_IOREMAP);
//     if (!vma)
//     {
//         pr_err("hvisor.ko: failed to allocate virtual kernel memory for
//         image\n"); return -ENOMEM;
//     }
//     vma->phys_addr = phys_start;

//     if (ioremap_page_range((unsigned long)vma->addr, (unsigned
//     long)(vma->addr + size), phys_start, PAGE_KERNEL_EXEC))
//     {
//         pr_err("hvisor.ko: failed to ioremap image\n");
//         err = -EFAULT;
//         goto unmap_vma;
//     }
//     // flush icache will also flush dcache
//     flush_icache_range((unsigned long)(vma->addr), (unsigned long)(vma->addr
//     + size));

// unmap_vma:
//     vunmap(vma->addr);
//     return err;
// }

static int hvisor_zone_start(zone_config_t __user *arg) {
    int err = 0;
    zone_config_t *zone_config = kmalloc(sizeof(zone_config_t), GFP_KERNEL);

    if (zone_config == NULL) {
        pr_err("hvisor.ko: failed to allocate memory for zone_config\n");
    }

    if (copy_from_user(zone_config, arg, sizeof(zone_config_t))) {
        pr_err("hvisor.ko: failed to copy from user\n");
        kfree(zone_config);
        return -EFAULT;
    }

    // flush_cache(zone_config->kernel_load_paddr, zone_config->kernel_size);
    // flush_cache(zone_config->dtb_load_paddr, zone_config->dtb_size);

    pr_info("hvisor.ko: invoking hypercall to start the zone\n");

    err = hvisor_call(HVISOR_HC_START_ZONE, __pa(zone_config),
                      sizeof(zone_config_t));
    kfree(zone_config);
    return err;
}

// #ifndef LOONGARCH64
// static int is_reserved_memory(unsigned long phys, unsigned long size) {
//     struct device_node *parent, *child;
//     struct reserved_mem *rmem;
//     phys_addr_t mem_base;
//     size_t mem_size;
//     int count = 0;
//     parent = of_find_node_by_path("/reserved-memory");
//     count = of_get_child_count(parent);

//     for_each_child_of_node(parent, child) {
//         rmem = of_reserved_mem_lookup(child);
//         mem_base = rmem->base;
//         mem_size = rmem->size;
//         if (mem_base <= phys && (mem_base + mem_size) >= (phys + size)) {
//             return 1;
//         }
//     }
//     return 0;
// }
// #endif

static int hvisor_config_check(u64 __user *arg) {
    int err = 0;
    u64 *config;
    config = kmalloc(sizeof(u64), GFP_KERNEL);
    err = hvisor_call(HVISOR_HC_CONFIG_CHECK, __pa(config), 0);

    if (err != 0) {
        pr_err("hvisor.ko: failed to get hvisor config\n");
    }

    if (copy_to_user(arg, config, sizeof(u64))) {
        pr_err("hvisor.ko: failed to copy to user\n");
        kfree(config);
        return -EFAULT;
    }

    kfree(config);
    return err;
}

static int hvisor_zone_list(zone_list_args_t __user *arg) {
    int ret;
    zone_info_t *zones;
    zone_list_args_t args;

    /* Copy user provided arguments to kernel space */
    if (copy_from_user(&args, arg, sizeof(zone_list_args_t))) {
        pr_err("hvisor.ko: failed to copy from user\n");
        return -EFAULT;
    }

    zones = kmalloc(args.cnt * sizeof(zone_info_t), GFP_KERNEL);
    memset(zones, 0, args.cnt * sizeof(zone_info_t));

    ret = hvisor_call(HVISOR_HC_ZONE_LIST, __pa(zones), args.cnt);
    if (ret < 0) {
        pr_err("hvisor.ko: failed to get zone list\n");
        goto out;
    }
    // copy result back to user space
    if (copy_to_user(args.zones, zones, ret * sizeof(zone_info_t))) {
        pr_err("hvisor.ko: failed to copy to user\n");
        goto out;
    }
out:
    kfree(zones);
    return ret;
}

static long hvisor_ioctl(struct file *file, unsigned int ioctl,
                         unsigned long arg) {
    int err = 0;
    switch (ioctl) {
    case HVISOR_INIT_VIRTIO:
        err = hvisor_init_virtio();
        task = get_current(); // get hvisor user process
        break;
    case HVISOR_ZONE_START:
        err = hvisor_zone_start((zone_config_t __user *)arg);
        break;
    case HVISOR_ZONE_SHUTDOWN:
        err = hvisor_call(HVISOR_HC_SHUTDOWN_ZONE, arg, 0);
        break;
    case HVISOR_ZONE_LIST:
        err = hvisor_zone_list((zone_list_args_t __user *)arg);
        break;
    case HVISOR_FINISH_REQ:
        err = hvisor_finish_req();
        break;
    case HVISOR_CONFIG_CHECK:
        err = hvisor_config_check((u64 __user *)arg);
        break;
    case HVISOR_SCMI_CLOCK_IOCTL:
        err = hvisor_scmi_clock_ioctl((struct hvisor_scmi_clock_args __user *)arg);
        break;
#ifdef LOONGARCH64
    case HVISOR_CLEAR_INJECT_IRQ:
        err = hvisor_call(HVISOR_HC_CLEAR_INJECT_IRQ, 0, 0);
        break;
#endif
    default:
        err = -EINVAL;
        break;
    }
    return err;
}

// Kernel mmap handler
static int hvisor_map(struct file *filp, struct vm_area_struct *vma) {
    unsigned long phys;
    int err;
    if (vma->vm_pgoff == 0) {
        // virtio_bridge must be aligned to one page.
        phys = virt_to_phys(virtio_bridge);
        // vma->vm_flags |= (VM_IO | VM_LOCKED | (VM_DONTEXPAND | VM_DONTDUMP));
        // Not sure should we add this line.
        err = remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
                              vma->vm_end - vma->vm_start, vma->vm_page_prot);
        if (err)
            return err;
        pr_info("virtio bridge mmap succeed!\n");
    } else {
        size_t size = vma->vm_end - vma->vm_start;
        // TODO: add check for non root memory region.
        // memremap(0x50000000, 0x30000000, MEMREMAP_WB);
        // vm_pgoff is the physical page number.
        // if (!is_reserved_memory(vma->vm_pgoff << PAGE_SHIFT, size)) {
        //     pr_err("The physical address to be mapped is not within the
        //     reserved memory\n"); return -EFAULT;
        // }
        err = remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
                              vma->vm_page_prot);
        if (err)
            return err;
        pr_info("non root region mmap succeed!\n");
    }
    return 0;
}

static const struct file_operations hvisor_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = hvisor_ioctl,
    .compat_ioctl = hvisor_ioctl,
    .mmap = hvisor_map,
};

static struct miscdevice hvisor_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "hvisor",
    .fops = &hvisor_fops,
};

// Interrupt handler for Virtio device.
static irqreturn_t virtio_irq_handler(int irq, void *dev_id) {
    struct siginfo info;
    if (dev_id != &hvisor_misc_dev) {
        return IRQ_NONE;
    }

    memset(&info, 0, sizeof(struct siginfo));
    info.si_signo = SIGHVI;
    info.si_code = SI_QUEUE;
    info.si_int = 1;
    // Send signal SIGHVI to hvisor user task
    if (task != NULL) {
        // pr_info("send signal to hvisor device\n");
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(4, 20, 0))
        if (send_sig_info(SIGHVI, (struct siginfo *)&info, task) < 0) {
            pr_err("Unable to send signal\n");
        }
#else
        if (send_sig_info(SIGHVI, (struct kernel_siginfo *)&info, task) < 0) {
            pr_err("Unable to send signal\n");
        }
#endif
    }
    return IRQ_HANDLED;
}

/*
** Module Init function
*/
static int __init hvisor_init(void) {
    int err;
    struct device_node *node = NULL;
    err = misc_register(&hvisor_misc_dev);
    if (err) {
        pr_err("hvisor_misc_register failed!!!\n");
        return err;
    }
    // probe hvisor virtio device.
    // The irq number must be retrieved from dtb node, because it is different
    // from GIC's IRQ number.
    node = of_find_node_by_path("/hvisor_virtio_device");
    if (!node) {
        pr_err("Critical: Missing device tree node!\n");
        pr_err("   Please add the following to your device tree:\n");
        pr_err("   hvisor_virtio_device {\n");
        pr_err("       compatible = \"hvisor\";\n");
        pr_err("       interrupts = <0x00 0x20 0x01>;\n");
        pr_err("   };\n");
        return -ENODEV;
    }

    virtio_irq = of_irq_get(node, 0);
    err = request_irq(virtio_irq, virtio_irq_handler,
                      IRQF_SHARED | IRQF_TRIGGER_RISING, "hvisor_virtio_device",
                      &hvisor_misc_dev);
    if (err)
        goto err_out;

    of_node_put(node);
    pr_info("hvisor init done!!!\n");
    return 0;
err_out:
    pr_err("hvisor cannot register IRQ, err is %d\n", err);
    if (virtio_irq != -1)
        free_irq(virtio_irq, &hvisor_misc_dev);
    misc_deregister(&hvisor_misc_dev);
    return err;
}

/*
** Module Exit function
*/
static void __exit hvisor_exit(void) {
    if (virtio_irq != -1)
        free_irq(virtio_irq, &hvisor_misc_dev);
    if (virtio_bridge != NULL) {
        ClearPageReserved(virt_to_page(virtio_bridge));
        free_pages((unsigned long)virtio_bridge, 0);
    }
    misc_deregister(&hvisor_misc_dev);
    pr_info("hvisor exit!!!\n");
}

module_init(hvisor_init);
module_exit(hvisor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KouweiLee <15035660024@163.com>");
MODULE_DESCRIPTION("The hvisor device driver");
MODULE_VERSION("1:0.0");