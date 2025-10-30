// SPDX-License-Identifier: GPL-2.0-only
#ifndef __HVISOR_CFGCHK_H
#define __HVISOR_CFGCHK_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CFGCHK_MAX_CPUS 64
#define CFGCHK_MAX_IRQS 64
#define CFGCHK_MAX_PHYSMEM 16

#define CFGCHK_IOCTL_VERSION 1

struct cfgchk_physmem_range {
    __u64 start;
    __u64 end;
    __u32 type;
    __u32 rsvd;
};

struct cfgchk_board_info {
    __u32 total_cpus;
    __u64 root_cpu_bitmap;
    __u64 mpidr_map[CFGCHK_MAX_CPUS];

    __u32 root_irq_count;
    __u32 root_irqs[CFGCHK_MAX_IRQS];

    __u32 physmem_count;
    struct cfgchk_physmem_range physmem[CFGCHK_MAX_PHYSMEM];
};

struct cfgchk_request {
    __u32 version;
    struct cfgchk_board_info board;
};

#define HVISOR_CFG_VALIDATE _IOW('C', 0x10, struct cfgchk_request *)

#endif /* __HVISOR_CFGCHK_H */
