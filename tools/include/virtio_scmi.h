// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 *      https://www.syswonder.org
 *
 * Authors:
 *      Linkun Chen <lkchen01@foxmail.com>
 */
#ifndef _HVISOR_VIRTIO_SCMI_H
#define _HVISOR_VIRTIO_SCMI_H

#include "virtio.h"

typedef struct virtio_scmi_dev {
    int fd;
} SCMIDev;

#define SCMI_SUPPORTED_FEATURES                                             \
    (1ULL << VIRTIO_F_VERSION_1)

#define SCMI_MAX_QUEUES 1
#define VIRTQUEUE_SCMI_MAX_SIZE 64
#define SCMI_QUEUE_RX 0

SCMIDev *init_scmi_dev();
int virtio_scmi_rxq_notify_handler(VirtIODevice *vdev, VirtQueue *vq);

#endif