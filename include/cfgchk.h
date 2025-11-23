// SPDX-License-Identifier: GPL-2.0-only
#ifndef __HVISOR_CFGCHK_H
#define __HVISOR_CFGCHK_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CFGCHK_MAX_CPUS 64
#define CFGCHK_MAX_MEM 32
#define CFGCHK_MAX_IRQS 64
#define CFGCHK_MAX_VIRTIO 16
#define CFGCHK_MAX_PHYSMEM 16
#define CFGCHK_MAX_RESERVED 16
#define CFGCHK_MAX_ZONES 16

#define CFGCHK_IOCTL_VERSION 1

enum cfgchk_mem_type {
    CFGCHK_MEM_RAM = 0,
    CFGCHK_MEM_IO = 1,
    CFGCHK_MEM_VIRTIO = 2,
};

enum cfgchk_mem_flags {
    CFGCHK_MEM_F_NONE = 0,
    CFGCHK_MEM_F_REQUIRES_RESERVATION = 1 << 0,
};

struct cfgchk_physmem_range {
    __u64 start;
    __u64 end;
    __u32 type;
    __u32 rsvd;
};

struct cfgchk_reserved_range {
    __u64 start;
    __u64 size;
};

struct cfgchk_mem_region {
    __u64 start;
    __u64 size;
    __u32 type;
    __u32 flags;
};

struct cfgchk_virtio_desc {
    __u64 base;
    __u64 size;
    __u32 irq;
    __u32 rsvd;
};

struct cfgchk_board_info {
    __u32 total_cpus;
    __u32 reserved;
    __u64 root_cpu_bitmap;
    __u64 mpidr_map[CFGCHK_MAX_CPUS];

    __u32 root_irq_count;
    __u32 root_irqs[CFGCHK_MAX_IRQS];

    __u32 physmem_count;
    __u32 reserved_count;
    struct cfgchk_physmem_range physmem[CFGCHK_MAX_PHYSMEM];
    struct cfgchk_reserved_range reserved_mem[CFGCHK_MAX_RESERVED];

    __u64 gicd_base;
    __u64 gicd_size;
    __u64 gicr_base;
    __u64 gicr_size;
    __u32 gic_version;
    __u32 pad;
};

struct cfgchk_zone_summary {
    __u32 zone_id;
    __u32 cpu_count;
    __u64 cpu_bitmap;
    __u32 cpus[CFGCHK_MAX_CPUS];

    __u32 mem_count;
    struct cfgchk_mem_region mem_regions[CFGCHK_MAX_MEM];

    __u32 irq_count;
    __u32 irqs[CFGCHK_MAX_IRQS];

    __u32 virtio_count;
    struct cfgchk_virtio_desc virtio[CFGCHK_MAX_VIRTIO];

    __u64 gicd_base;
    __u64 gicd_size;
    __u64 gicr_base;
    __u64 gicr_size;
    __u32 gic_version;
    __u32 pad;
};

struct cfgchk_dts_summary {
    __u32 cpu_count;
    __u32 cpus[CFGCHK_MAX_CPUS];

    __u32 mem_count;
    struct cfgchk_mem_region mem_regions[CFGCHK_MAX_MEM];

    __u32 virtio_count;
    struct cfgchk_virtio_desc virtio[CFGCHK_MAX_VIRTIO];
};

struct cfgchk_request {
    __u32 version;
    __u32 zone_count;
    __u32 target_index;
    __u32 flags;

    struct cfgchk_board_info board;
    struct cfgchk_zone_summary zones[CFGCHK_MAX_ZONES];

    struct cfgchk_dts_summary dts_zone;
    struct cfgchk_dts_summary dts_root;
};

#define HVISOR_CFG_VALIDATE _IOW('C', 0x10, struct cfgchk_request *)

#endif /* __HVISOR_CFGCHK_H */
