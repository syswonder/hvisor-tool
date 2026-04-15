/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _LINUX_VIRTIO_MMIO_H
#define _LINUX_VIRTIO_MMIO_H

/* Magic value ("virt" string) - Read Only */
#define VIRTIO_MMIO_MAGIC_VALUE 0x000

/* Virtio device version - Read Only */
#define VIRTIO_MMIO_VERSION 0x004

/* Virtio device ID - Read Only */
#define VIRTIO_MMIO_DEVICE_ID 0x008

/* Virtio vendor ID - Read Only */
#define VIRTIO_MMIO_VENDOR_ID 0x00c

/* Device features - Read Only */
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010

/* Device features selector - Write Only */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014

/* Driver features - Write Only */
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020

/* Driver features selector - Write Only */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024

#ifndef VIRTIO_MMIO_NO_LEGACY
/* Legacy guest page size - Write Only */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#endif

/* Queue selector - Write Only */
#define VIRTIO_MMIO_QUEUE_SEL 0x030

/* Queue size max - Read Only */
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034

/* Queue size - Write Only */
#define VIRTIO_MMIO_QUEUE_NUM 0x038

#ifndef VIRTIO_MMIO_NO_LEGACY
/* Legacy used ring alignment - Write Only */
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c

/* Legacy queue PFN - Read Write */
#define VIRTIO_MMIO_QUEUE_PFN 0x040
#endif

/* Queue ready - Read Write */
#define VIRTIO_MMIO_QUEUE_READY 0x044

/* Queue notifier - Write Only */
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050

/* Interrupt status - Read Only */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060

/* Interrupt acknowledge - Write Only */
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064

/* Device status - Read Write */
#define VIRTIO_MMIO_STATUS 0x070

/* Queue descriptor table address (64-bit split) */
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084

/* Queue available ring address (64-bit split) */
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094

/* Queue used ring address (64-bit split) */
#define VIRTIO_MMIO_QUEUE_USED_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

/* Shared memory selector */
#define VIRTIO_MMIO_SHM_SEL 0x0ac

/* Shared memory length (64-bit split) */
#define VIRTIO_MMIO_SHM_LEN_LOW 0x0b0
#define VIRTIO_MMIO_SHM_LEN_HIGH 0x0b4

/* Shared memory base address (64-bit split) */
#define VIRTIO_MMIO_SHM_BASE_LOW 0x0b8
#define VIRTIO_MMIO_SHM_BASE_HIGH 0x0bc

/* Configuration atomicity value */
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc

/* Per-device config space base */
#define VIRTIO_MMIO_CONFIG 0x100

/* Interrupt flags */
#define VIRTIO_MMIO_INT_VRING (1 << 0)
#define VIRTIO_MMIO_INT_CONFIG (1 << 1)

#endif /* _LINUX_VIRTIO_MMIO_H */
