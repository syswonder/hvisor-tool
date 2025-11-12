// SPDX-License-Identifier: GPL-2.0-only
/*
 * hvisor configuration consistency checker.
 *
 * This kernel module receives parsed board / zone descriptions from user space
 * and performs a second-line validation inside the kernel to make sure the
 * guest configuration will not violate platform level constraints before a
 * zone is started.
 */

#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "cfgchk.h"
#include "hvisor.h"

#define CFGCHK_ERR(fmt, ...) pr_err("cfgchk: " fmt "\n", ##__VA_ARGS__)
#define CFGCHK_INFO(fmt, ...) pr_info("cfgchk: " fmt "\n", ##__VA_ARGS__)

static inline u64 cfgchk_min(u64 a, u64 b) { return a < b ? a : b; }
static inline u64 cfgchk_max(u64 a, u64 b) { return a > b ? a : b; }

static bool range_overlaps(u64 s1, u64 e1, u64 s2, u64 e2) {
    return cfgchk_max(s1, s2) < cfgchk_min(e1, e2);
}

static bool range_within(u64 start, u64 size,
                         const struct cfgchk_physmem_range *r) {
    u64 end;
    if (!size)
        return false;
    if (check_add_overflow(start, size, &end))
        return false;
    if (end <= start)
        return false;
    if (r->end <= r->start)
        return false;
    return start >= r->start && end <= r->end;
}

static bool reserved_contains(u64 start, u64 size,
                              const struct cfgchk_reserved_range *r) {
    u64 end, rend;
    if (check_add_overflow(start, size, &end))
        return false;
    if (check_add_overflow(r->start, r->size, &rend))
        return false;
    return start >= r->start && end <= rend;
}

static int validate_cpu(const struct cfgchk_board_info *board,
                        const struct cfgchk_zone_summary *zones, u32 zone_index,
                        u32 zone_count, const struct cfgchk_dts_summary *dts) {
    const struct cfgchk_zone_summary *target = &zones[zone_index];
    unsigned long long seen = 0;
    u32 i, j;

    if (!board->total_cpus || board->total_cpus > CFGCHK_MAX_CPUS) {
        CFGCHK_ERR("invalid board cpu topology");
        return -EINVAL;
    }
    if (!target->cpu_count) {
        CFGCHK_ERR("zone %u has no CPU assigned", target->zone_id);
        return -EINVAL;
    }

    for (i = 0; i < target->cpu_count; ++i) {
        u32 cpu = target->cpus[i];
        if (cpu >= board->total_cpus) {
            CFGCHK_ERR("zone %u cpu %u exceeds board cpu count %u",
                       target->zone_id, cpu, board->total_cpus);
            return -EINVAL;
        }
        if (seen & BIT_ULL(cpu)) {
            CFGCHK_ERR("zone %u cpu %u duplicated in json", target->zone_id,
                       cpu);
            return -EINVAL;
        }
        seen |= BIT_ULL(cpu);
    }

    if (seen != target->cpu_bitmap) {
        CFGCHK_ERR("zone %u cpu bitmap mismatch (json internal inconsistency)",
                   target->zone_id);
        return -EINVAL;
    }

    if (target->cpu_bitmap & board->root_cpu_bitmap) {
        CFGCHK_ERR("zone %u cpu conflicts root zone mask 0x%llx",
                   target->zone_id, board->root_cpu_bitmap);
        return -EINVAL;
    }

    for (i = 0; i < zone_count; ++i) {
        const struct cfgchk_zone_summary *other;
        if (i == zone_index)
            continue;
        other = &zones[i];
        if (!other->cpu_bitmap)
            continue;
        if (target->cpu_bitmap & other->cpu_bitmap) {
            CFGCHK_ERR("zone %u cpu conflicts zone %u", target->zone_id,
                       other->zone_id);
            return -EINVAL;
        }
    }

    /* DTS cross-check */
    if (dts->cpu_count != target->cpu_count) {
        CFGCHK_ERR("zone %u cpu count mismatch dts(%u) vs json(%u)",
                   target->zone_id, dts->cpu_count, target->cpu_count);
        return -EINVAL;
    }
    for (i = 0; i < dts->cpu_count; ++i) {
        u32 cpu = dts->cpus[i];
        if (cpu >= board->total_cpus) {
            CFGCHK_ERR("zone %u dts cpu %u exceeds board limit",
                       target->zone_id, cpu);
            return -EINVAL;
        }
        if (!(target->cpu_bitmap & BIT_ULL(cpu))) {
            CFGCHK_ERR("zone %u dts cpu %u missing in json list",
                       target->zone_id, cpu);
            return -EINVAL;
        }
        for (j = i + 1; j < dts->cpu_count; ++j) {
            if (cpu == dts->cpus[j]) {
                CFGCHK_ERR("zone %u dts cpu %u duplicated", target->zone_id,
                           cpu);
                return -EINVAL;
            }
        }
    }
    return 0;
}

static int validate_memory(const struct cfgchk_board_info *board,
                           const struct cfgchk_zone_summary *zones,
                           u32 zone_index, u32 zone_count,
                           const struct cfgchk_dts_summary *dts_zone,
                           const struct cfgchk_dts_summary *dts_root) {
    const struct cfgchk_zone_summary *target = &zones[zone_index];
    u32 i, j;

    for (i = 0; i < target->mem_count; ++i) {
        const struct cfgchk_mem_region *mem = &target->mem_regions[i];
        bool covered = false;
        if (!mem->size) {
            CFGCHK_ERR("zone %u memory region %u has zero size",
                       target->zone_id, i);
            return -EINVAL;
        }
        if (mem->type == CFGCHK_MEM_VIRTIO) {
            if (!IS_ALIGNED(mem->start, SZ_4K) ||
                !IS_ALIGNED(mem->size, SZ_4K)) {
                CFGCHK_ERR(
                    "zone %u virtio region 0x%llx size 0x%llx not 4K aligned",
                    target->zone_id, mem->start, mem->size);
                return -EINVAL;
            }
        }
        for (j = 0; j < board->physmem_count; ++j) {
            const struct cfgchk_physmem_range *pm = &board->physmem[j];
            if (pm->type != CFGCHK_MEM_VIRTIO &&
                mem->type == CFGCHK_MEM_VIRTIO && pm->type != CFGCHK_MEM_IO)
                continue;
            if (mem->type == CFGCHK_MEM_RAM && pm->type != CFGCHK_MEM_RAM)
                continue;
            if (mem->type == CFGCHK_MEM_VIRTIO && pm->type != CFGCHK_MEM_IO)
                continue;
            if (range_within(mem->start, mem->size, pm)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            CFGCHK_ERR("zone %u memory region %u (0x%llx size 0x%llx type %u) "
                       "not inside board physmem list",
                       target->zone_id, i, mem->start, mem->size, mem->type);
            return -EINVAL;
        }
        if ((mem->flags & CFGCHK_MEM_F_REQUIRES_RESERVATION) &&
            board->reserved_count) {
            bool ok = false;
            for (j = 0; j < board->reserved_count; ++j) {
                if (reserved_contains(mem->start, mem->size,
                                      &board->reserved_mem[j])) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                CFGCHK_ERR("zone %u memory 0x%llx size 0x%llx requires "
                           "reserved-memory",
                           target->zone_id, mem->start, mem->size);
                return -EINVAL;
            }
        }

        for (j = 0; j < zone_count; ++j) {
            u32 k;
            if (j == zone_index)
                continue;
            for (k = 0; k < zones[j].mem_count; ++k) {
                const struct cfgchk_mem_region *other =
                    &zones[j].mem_regions[k];
                u64 end = mem->start + mem->size;
                u64 other_end = other->start + other->size;
                if (range_overlaps(mem->start, end, other->start, other_end)) {
                    CFGCHK_ERR("zone %u memory (0x%llx-0x%llx) conflicts zone "
                               "%u region "
                               "(0x%llx-0x%llx)",
                               target->zone_id, mem->start, end,
                               zones[j].zone_id, other->start, other_end);
                    return -EINVAL;
                }
            }
        }
    }

    /* Cross check with DTS memory section when provided (best effort) */
    if (dts_zone->mem_count) {
        for (i = 0; i < dts_zone->mem_count; ++i) {
            const struct cfgchk_mem_region *dmem = &dts_zone->mem_regions[i];
            bool match = false;
            for (j = 0; j < target->mem_count; ++j) {
                const struct cfgchk_mem_region *mem = &target->mem_regions[j];
                if (mem->type != dmem->type)
                    continue;
                if (mem->start == dmem->start && mem->size == dmem->size) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                CFGCHK_ERR(
                    "zone %u dts memory 0x%llx size 0x%llx missing in json",
                    target->zone_id, dmem->start, dmem->size);
                return -EINVAL;
            }
        }
    }

    /* Ensure reserved-memory in root DTS covers guest reserved ranges */
    if (dts_root->mem_count && board->reserved_count) {
        for (i = 0; i < board->reserved_count; ++i) {
            const struct cfgchk_reserved_range *r = &board->reserved_mem[i];
            bool match = false;
            for (j = 0; j < dts_root->mem_count; ++j) {
                const struct cfgchk_mem_region *dmem =
                    &dts_root->mem_regions[j];
                struct cfgchk_reserved_range tmp;
                if (dmem->type != CFGCHK_MEM_RAM)
                    continue;
                tmp.start = dmem->start;
                tmp.size = dmem->size;
                if (reserved_contains(r->start, r->size, &tmp)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                CFGCHK_ERR(
                    "reserved range 0x%llx size 0x%llx missing in zone0 dts",
                    r->start, r->size);
                return -EINVAL;
            }
        }
    }

    return 0;
}

static int validate_irqs(const struct cfgchk_board_info *board,
                         const struct cfgchk_zone_summary *zones,
                         u32 zone_index, u32 zone_count,
                         const struct cfgchk_dts_summary *dts_zone) {
    const struct cfgchk_zone_summary *target = &zones[zone_index];
    u32 i, j;
    for (i = 0; i < target->irq_count; ++i) {
        u32 irq = target->irqs[i];
        for (j = i + 1; j < target->irq_count; ++j) {
            if (irq == target->irqs[j]) {
                CFGCHK_ERR("zone %u irq %u duplicated", target->zone_id, irq);
                return -EINVAL;
            }
        }
        for (j = 0; j < board->root_irq_count; ++j) {
            if (irq == board->root_irqs[j]) {
                CFGCHK_ERR("zone %u irq %u conflicts root zone",
                           target->zone_id, irq);
                return -EINVAL;
            }
        }
        for (j = 0; j < zone_count; ++j) {
            u32 k;
            if (j == zone_index)
                continue;
            for (k = 0; k < zones[j].irq_count; ++k) {
                if (irq == zones[j].irqs[k]) {
                    CFGCHK_ERR("zone %u irq %u conflicts zone %u",
                               target->zone_id, irq, zones[j].zone_id);
                    return -EINVAL;
                }
            }
        }
    }

    if (dts_zone->virtio_count != target->virtio_count) {
        CFGCHK_ERR("zone %u virtio device count mismatch dts(%u) json(%u)",
                   target->zone_id, dts_zone->virtio_count,
                   target->virtio_count);
        return -EINVAL;
    }

    for (i = 0; i < target->virtio_count; ++i) {
        const struct cfgchk_virtio_desc *va = &target->virtio[i];
        const struct cfgchk_virtio_desc *vd = &dts_zone->virtio[i];
        bool irq_found = false;
        u32 irq_idx;

        if (va->base != vd->base || va->size != vd->size) {
            CFGCHK_ERR("zone %u virtio #%u addr mismatch json(0x%llx/0x%llx) "
                       "dts(0x%llx/0x%llx)",
                       target->zone_id, i, va->base, va->size, vd->base,
                       vd->size);
            return -EINVAL;
        }
        if (va->irq != vd->irq) {
            CFGCHK_ERR("zone %u virtio #%u irq mismatch json(%u) dts(%u)",
                       target->zone_id, i, va->irq, vd->irq);
            return -EINVAL;
        }
        for (irq_idx = 0; irq_idx < target->irq_count; ++irq_idx) {
            if (target->irqs[irq_idx] == va->irq) {
                irq_found = true;
                break;
            }
        }
        if (!irq_found) {
            CFGCHK_ERR("zone %u virtio irq %u missing from interrupt list",
                       target->zone_id, va->irq);
            return -EINVAL;
        }
    }

    if (dts_zone->virtio_count) {
        /* ensure dts interrupts appear in json list */
        for (i = 0; i < dts_zone->virtio_count; ++i) {
            bool found = false;
            for (j = 0; j < target->irq_count; ++j) {
                if (target->irqs[j] == dts_zone->virtio[i].irq) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                CFGCHK_ERR(
                    "zone %u virtio irq %u missing from json interrupt list",
                    target->zone_id, dts_zone->virtio[i].irq);
                return -EINVAL;
            }
        }
    }

    return 0;
}

static int validate_gic(const struct cfgchk_board_info *board,
                        const struct cfgchk_zone_summary *zone,
                        const struct cfgchk_dts_summary *dts_zone) {
    if (zone->gic_version != board->gic_version) {
        CFGCHK_ERR("zone %u gic version mismatch board(%u) zone(%u)",
                   zone->zone_id, board->gic_version, zone->gic_version);
        return -EINVAL;
    }
    if (zone->gicd_base != board->gicd_base ||
        zone->gicd_size != board->gicd_size) {
        CFGCHK_ERR("zone %u gicd base/size mismatch board", zone->zone_id);
        return -EINVAL;
    }
    if (zone->gicr_base != board->gicr_base ||
        zone->gicr_size != board->gicr_size) {
        CFGCHK_ERR("zone %u gicr base/size mismatch board", zone->zone_id);
        return -EINVAL;
    }

    if (dts_zone->virtio_count >= 0) {
        /* Nothing additional yet; placeholder for future cross-check. */
    }
    return 0;
}

static int cfgchk_validate_request(const struct cfgchk_request *req) {
    const struct cfgchk_zone_summary *target;
    int ret;

    if (!req) {
        CFGCHK_ERR("null request pointer");
        return -EINVAL;
    }

    CFGCHK_INFO("received cfgchk request: version=%u zone_count=%u target=%u",
                req->version, req->zone_count, req->target_index);
    if (req->version != CFGCHK_IOCTL_VERSION) {
        CFGCHK_ERR("unsupported request version %u (kernel expects %u)",
                   req->version, CFGCHK_IOCTL_VERSION);
        return -EINVAL;
    }
    if (!req->zone_count) {
        CFGCHK_ERR("request zone_count is zero");
        return -EINVAL;
    }
    if (req->zone_count > CFGCHK_MAX_ZONES) {
        CFGCHK_ERR("request zone_count %u exceeds max %u", req->zone_count,
                   CFGCHK_MAX_ZONES);
        return -EINVAL;
    }
    if (req->target_index >= req->zone_count) {
        CFGCHK_ERR("target index %u out of range (zone_count=%u)",
                   req->target_index, req->zone_count);
        return -EINVAL;
    }

    target = &req->zones[req->target_index];

    ret = validate_cpu(&req->board, req->zones, req->target_index,
                       req->zone_count, &req->dts_zone);
    if (ret)
        return ret;

    ret = validate_memory(&req->board, req->zones, req->target_index,
                          req->zone_count, &req->dts_zone, &req->dts_root);
    if (ret)
        return ret;

    ret = validate_irqs(&req->board, req->zones, req->target_index,
                        req->zone_count, &req->dts_zone);
    if (ret)
        return ret;

    ret = validate_gic(&req->board, target, &req->dts_zone);
    if (ret)
        return ret;

    CFGCHK_INFO("zone %u validation passed", target->zone_id);

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
    int ret;
    ret = misc_register(&cfgchk_misc_dev);
    if (ret) {
        CFGCHK_ERR("failed to register misc device (%d)", ret);
        return ret;
    }
    CFGCHK_INFO("init done");
    return 0;
}

static void __exit cfgchk_exit(void) {
    misc_deregister(&cfgchk_misc_dev);
    CFGCHK_INFO("exit");
}

module_init(cfgchk_init);
module_exit(cfgchk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex Assistant");
MODULE_DESCRIPTION("hvisor guest configuration checker");
