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
#include <string.h>
#include <errno.h>

#define SCMI_MAX_DESCRIPTORS     16
#define SCMI_VERSION_REQUEST     0
#define SCMI_VERSION_2_1        0x20001
#define SCMI_RESPONSE_SIZE      (sizeof(uint32_t[3]))
#define SCMI_MAX_BUFFER_SIZE    (1024 * 1024) // 1MB

SCMIDev *init_scmi_dev() {
    SCMIDev *dev = (SCMIDev *)malloc(sizeof(SCMIDev));
    if (dev) {
        dev->fd = -1;
    }
    return dev;
}

static int virtq_tx_handle_one_request(void *dev, VirtQueue *vq) {
    uint16_t desc_idx;
    struct iovec *iov = NULL;
    uint16_t *flags = NULL;
    int ret = process_descriptor_chain(vq, &desc_idx, &iov, &flags, SCMI_MAX_DESCRIPTORS, true);
    
    if (ret <= 0 || iov == NULL || flags == NULL) {
        log_error("Failed to process descriptor chain or allocate memory");
        goto error;
    }

    // Log IOV
    for (int i = 0; i < ret; i++) {
        log_info("IOV[%d]: base=%p, len=%zu", i, iov[i].iov_base, iov[i].iov_len);
    }

    // Check buffer sizes and pointers - accept 4-byte packed header
    if (ret < 2 || iov[0].iov_len < sizeof(uint32_t) || 
        iov[0].iov_base == NULL || iov[0].iov_len > SCMI_MAX_BUFFER_SIZE) {
        log_error("Invalid request buffer");
        goto error;
    }

    // Parse packed 32-bit header using Linux kernel style extractors
    uint32_t packed_header = *(uint32_t *)iov[0].iov_base;
    uint8_t protocol_id = SCMI_PROTOCOL_ID(packed_header);
    uint8_t msg_id = SCMI_MSG_ID(packed_header);
    uint8_t msg_type = SCMI_MSG_TYPE(packed_header);
    uint16_t token = SCMI_TOKEN_ID(packed_header);
    
    log_info("SCMI request: protocol=0x%x, msg=0x%x, type=%d, token=0x%x, len=%d", 
            protocol_id, msg_id, msg_type, token, iov[0].iov_len);

    // Handle SCMI version request (protocol_id=0x10, msg_id=0x0)
    if (protocol_id == 0x10 && msg_id == 0x0) {
        if (msg_type != SCMI_MSG_TYPE_COMMAND) {
            log_error("Invalid message type: %d", msg_type);
            goto error;
        }

        if (iov[1].iov_len >= 12 && iov[1].iov_base != NULL) {
            uint32_t *resp = (uint32_t *)iov[1].iov_base;
            resp[0] = packed_header;            // Construct packed header
            resp[1] = SCMI_SUCCESS;        // Status (4-7 bytes)
            resp[2] = SCMI_VERSION_2_1;         // SCMI version (8-11 bytes)
            log_debug("SCMI version response sent with packed header");
        } else {
            log_error("Invalid response buffer: len=%zu, base=%p", 
                     iov[1].iov_len, iov[1].iov_base);
            goto error;
        }
    } 
    // Handle SCMI vendor ID request (protocol_id=0x10, msg_id=0x1)
    else if (protocol_id == 0x10 && msg_id == 0x1) {
        if (msg_type != SCMI_MSG_TYPE_COMMAND) {
            log_error("Invalid message type: %d", msg_type);
            goto error;
        }

        if (iov[1].iov_len >= 12 && iov[1].iov_base != NULL) {
            uint32_t *resp = (uint32_t *)iov[1].iov_base;
            resp[0] = packed_header;            // Construct packed header
            resp[1] = SCMI_SUCCESS;        // Status (4-7 bytes)
            memcpy(&resp[2], "HVIS", 4);    // Vendor ID (8-11 bytes)
            log_debug("SCMI vendor ID response sent");
        } else {
            log_error("Invalid response buffer: len=%zu, base=%p", 
                     iov[1].iov_len, iov[1].iov_base);
            goto error;
        }
    }
    else {
        log_warn("Unsupported SCMI request: protocol=0x%x, msg=0x%x, token=0x%x",
                protocol_id, msg_id, token);
        goto error;
    }

    // Update used ring with total length (request + response)
    update_used_ring(vq, desc_idx, iov[0].iov_len + sizeof(uint32_t[3]));
    free(iov);
    free(flags);
    return 0;

error:
    free(iov);
    free(flags);
    return -EINVAL;
}

int virtio_scmi_txq_notify_handler(VirtIODevice *vdev, VirtQueue *vq) {    
    while (!virtqueue_is_empty(vq)) {
        virtqueue_disable_notify(vq);
        while (!virtqueue_is_empty(vq)) {
            if (virtq_tx_handle_one_request(vdev->dev, vq) < 0) {
                log_error("Failed to handle SCMI request");
                return -1;
            }
        }
        virtqueue_enable_notify(vq);
    }
    virtio_inject_irq(vq);
    return 0;
}