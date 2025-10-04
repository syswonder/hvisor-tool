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
#define _GNU_SOURCE

#include "virtio_scmi.h"
#include "log.h"
#include "virtio.h"
#include <stdlib.h>

SCMIDev *init_scmi_dev() {
    SCMIDev *dev = (SCMIDev *)malloc(sizeof(SCMIDev));
    dev->fd = -1;
    return dev;
}

int virtio_scmi_rxq_notify_handler(VirtIODevice *vdev, VirtQueue *vq) {
    log_debug("%s", __func__);
    for (;;) {}
    // while (!virtqueue_is_empty(vq)) {
    //     virtqueue_disable_notify(vq);
    //     while (!virtqueue_is_empty(vq)) {
    //         virtq_tx_handle_one_request(vdev->dev, vq);
    //     }
    //     virtqueue_enable_notify(vq);
    // }
    // virtio_inject_irq(vq);
    return 0;
}