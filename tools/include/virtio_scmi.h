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
#include <stdint.h>
#include <stdbool.h>

#define NSEC_PER_SEC 1000000000L
#define SCMI_MAX_POLL_TO_NS    (30 * NSEC_PER_SEC)

/*
 * SCMI Packed Message Header Format
 *
 *  31          28 27          18 17      10 9    8 7             0
 * +--------------+--------------+----------+------+---------------+
 * |   Reserved   |   Token ID   |ProtocolID| Type |  Message ID   |
 * +--------------+--------------+----------+------+---------------+
 */

/* -------------------------- Bitfield Operation Macros -------------------------- */

/* Generate mask, e.g. GENMASK(7,0)=0xFF */
#define BITMASK(high, low)   (((1u << ((high) - (low) + 1)) - 1) << (low))

/* Extract field: mask first, then right shift */
#define EXTRACT_BITS(val, high, low)   (((val) & BITMASK((high), (low))) >> (low))

/* Insert field: left shift first, then mask */
#define INSERT_BITS(val, high, low)    (((val) << (low)) & BITMASK((high), (low)))

/* -------------------------- SCMI Header Field Definitions -------------------------- */

/* Message ID: bits [7:0] */
#define SCMI_MSG_ID_LOW    0
#define SCMI_MSG_ID_HIGH   7

/* Message Type: bits [9:8] */
#define SCMI_MSG_TYPE_LOW  8
#define SCMI_MSG_TYPE_HIGH 9

/* Protocol ID: bits [17:10] */
#define SCMI_PROTOCOL_ID_LOW  10
#define SCMI_PROTOCOL_ID_HIGH 17

/* Token ID: bits [27:18] */
#define SCMI_TOKEN_ID_LOW   18
#define SCMI_TOKEN_ID_HIGH  27

/* -------------------------- SCMI Header Parsing Macros -------------------------- */

/* Extract message ID */
#define SCMI_MSG_ID(hdr) \
	EXTRACT_BITS((hdr), SCMI_MSG_ID_HIGH, SCMI_MSG_ID_LOW)

/* Extract message type */
#define SCMI_MSG_TYPE(hdr) \
	EXTRACT_BITS((hdr), SCMI_MSG_TYPE_HIGH, SCMI_MSG_TYPE_LOW)

/* Extract protocol ID */
#define SCMI_PROTOCOL_ID(hdr) \
	EXTRACT_BITS((hdr), SCMI_PROTOCOL_ID_HIGH, SCMI_PROTOCOL_ID_LOW)

/* Extract Token ID */
#define SCMI_TOKEN_ID(hdr) \
	EXTRACT_BITS((hdr), SCMI_TOKEN_ID_HIGH, SCMI_TOKEN_ID_LOW)

/* Construct response header */
#define SCMI_RESP_HDR(token) \
	(INSERT_BITS(0x0, SCMI_MSG_ID_HIGH, SCMI_MSG_ID_LOW) | \
	 INSERT_BITS(SCMI_MSG_TYPE_COMMAND, SCMI_MSG_TYPE_HIGH, SCMI_MSG_TYPE_LOW) | \
	 INSERT_BITS(0x10, SCMI_PROTOCOL_ID_HIGH, SCMI_PROTOCOL_ID_LOW) | \
	 INSERT_BITS((token), SCMI_TOKEN_ID_HIGH, SCMI_TOKEN_ID_LOW))

/* -------------------------- SCMI Message Type Values -------------------------- */
#define SCMI_MSG_TYPE_COMMAND         0  /* Command */
#define SCMI_MSG_TYPE_DELAYED_RESP    2  /* Delayed Response */
#define SCMI_MSG_TYPE_NOTIFICATION    3  /* Notification */

/* -------------------------- Token Maximum Value -------------------------- */
#define SCMI_TOKEN_MAX  ((1u << (SCMI_TOKEN_ID_HIGH - SCMI_TOKEN_ID_LOW + 1)) - 1)

enum scmi_error_codes {
	SCMI_SUCCESS = 0,	/* Success */
	SCMI_ERR_SUPPORT = -1,	/* Not supported */
	SCMI_ERR_PARAMS = -2,	/* Invalid Parameters */
	SCMI_ERR_ACCESS = -3,	/* Invalid access/permission denied */
	SCMI_ERR_ENTRY = -4,	/* Not found */
	SCMI_ERR_RANGE = -5,	/* Value out of range */
	SCMI_ERR_BUSY = -6,	/* Device busy */
	SCMI_ERR_COMMS = -7,	/* Communication Error */
	SCMI_ERR_GENERIC = -8,	/* Generic Error */
	SCMI_ERR_HARDWARE = -9,	/* Hardware Error */
	SCMI_ERR_PROTOCOL = -10,/* Protocol Error */
};

typedef struct virtio_scmi_dev {
    int fd;
} SCMIDev;

#define SCMI_SUPPORTED_FEATURES                                             \
    (1ULL << VIRTIO_F_VERSION_1)

#define SCMI_MAX_QUEUES 1
#define VIRTQUEUE_SCMI_MAX_SIZE 64
#define SCMI_QUEUE_TX 0



SCMIDev *init_scmi_dev();
int virtio_scmi_txq_notify_handler(VirtIODevice *vdev, VirtQueue *vq);

#endif
