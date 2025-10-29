// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 *      https://www.syswonder.org
 *
 * Authors:
 *      Linkun Chen <lkchen01@foxmail.com>
 */

#include "virtio_scmi.h"
#include "log.h"
#include "hvisor.h"
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Clock Protocol version 3.0 */
#define SCMI_CLOCK_VERSION 0x30000

/* Number of fake clocks */
#define FAKE_CLOCK_COUNT 3

/* Max number of discrete rates per clock */
#define MAX_RATES_PER_CLOCK 8

/* Fake clock descriptor */
struct fake_clock {
    char name[64];
    bool enabled;
    uint64_t current_rate;          // in Hz
    uint64_t supported_rates[MAX_RATES_PER_CLOCK];
    uint32_t num_rates;
    int32_t parent_id;              // -1 means no parent
};

/* Response for CLOCK_ATTRIBUTES (Message ID 0x3) */
struct scmi_msg_resp_clock_clock_attributes {
    uint32_t attributes;          /* Bitfield as per spec */
    char clock_name[16];          /* Null-terminated, max 15 chars + \0 */
    // uint32_t clock_enable_delay;  /* in microseconds */
};

/* Global fake clocks */
static struct fake_clock g_clocks[FAKE_CLOCK_COUNT];

/* Initialize fake clocks */
static void init_fake_clocks(void) {
    static bool initialized = false;
    if (initialized) return;

    for (int i = 0; i < FAKE_CLOCK_COUNT; i++) {
        snprintf(g_clocks[i].name, sizeof(g_clocks[i].name), "fake_clk_%d", i);
        g_clocks[i].enabled = false;
        g_clocks[i].current_rate = 100000000ULL * (i + 1);
        g_clocks[i].parent_id = (i == 0) ? -1 : 0; // clk0 is root, others parent to clk0

        // Example supported rates: 100MHz, 200MHz, ..., up to 800MHz
        g_clocks[i].num_rates = 8;
        for (int j = 0; j < g_clocks[i].num_rates; j++) {
            g_clocks[i].supported_rates[j] = (j + 1) * 100000000ULL; // 100MHz steps
        }
    }
    initialized = true;
}

/* Helper: validate clock_id */
static bool is_valid_clock_id(uint32_t clock_id) {
    return clock_id < FAKE_CLOCK_COUNT;
}

/* Helper: find closest supported rate */
static uint64_t find_closest_rate(uint64_t requested, const uint64_t *rates, uint32_t num_rates,
                                  bool round_up, bool round_down) {
    if (num_rates == 0) return 0;

    uint64_t best = rates[0];
    bool found_exact = false;

    for (uint32_t i = 0; i < num_rates; i++) {
        if (rates[i] == requested) {
            best = rates[i];
            found_exact = true;
            break;
        }
    }

    if (found_exact) return best;

    if (round_up && round_down) {
        // Autonomous: pick closest
        uint64_t min_diff = UINT64_MAX;
        for (uint32_t i = 0; i < num_rates; i++) {
            uint64_t diff = (rates[i] > requested) ? (rates[i] - requested) : (requested - rates[i]);
            if (diff < min_diff) {
                min_diff = diff;
                best = rates[i];
            }
        }
        return best;
    } else if (round_up) {
        // Find smallest rate >= requested
        best = 0;
        for (uint32_t i = 0; i < num_rates; i++) {
            if (rates[i] >= requested) {
                if (best == 0 || rates[i] < best) {
                    best = rates[i];
                }
            }
        }
        if (best == 0) best = rates[num_rates - 1]; // fallback to max
        return best;
    } else {
        // round_down: find largest rate <= requested
        best = 0;
        for (uint32_t i = 0; i < num_rates; i++) {
            if (rates[i] <= requested) {
                if (rates[i] > best) {
                    best = rates[i];
                }
            }
        }
        if (best == 0) best = rates[0]; // fallback to min
        return best;
    }
}

/* ================ Handlers ================ */

static int handle_clock_version(SCMIDev *dev, uint16_t token,
                             const struct iovec *req_iov,
                             struct iovec *resp_iov) {
    int ret = scmi_validate_request(req_iov->iov_len, sizeof(struct scmi_request),
                                  resp_iov->iov_len, sizeof(struct scmi_response) + 
                                  sizeof(uint32_t));
    if (ret != SCMI_SUCCESS) {
        log_error("Invalid version request");
        return ret;
    }

    struct scmi_response *resp = resp_iov->iov_base;
    uint32_t *version = (uint32_t *)resp->payload;
    scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
    *version = SCMI_CLOCK_VERSION;
    return 0;
}

static int handle_clock_protocol_attributes(SCMIDev *dev, uint16_t token,
                                const struct iovec *req_iov,
                                struct iovec *resp_iov) {
    int ret = scmi_validate_request(req_iov->iov_len, sizeof(struct scmi_request),
                                  resp_iov->iov_len, sizeof(struct scmi_response) + 
                                  sizeof(struct scmi_msg_resp_clock_attributes));
    if (ret != SCMI_SUCCESS) {
        log_error("Invalid request/response sizes");
        return ret;
    }

    struct scmi_response *resp = resp_iov->iov_base;
    struct scmi_msg_resp_clock_attributes *attr = (struct scmi_msg_resp_clock_attributes *)resp->payload;
    scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);

    int fd = open(HVISOR_DEVICE, O_RDWR);
    if (fd < 0) {
        log_error("Failed to open hvisor device");
        return -ENODEV;
    }

    uint32_t clock_count = 0;
    if (ioctl(fd, HVISOR_GET_CLOCK_MESSAGE, &clock_count) < 0) {
        log_error("Failed to get clock count from kernel");
        close(fd);
        return -EIO;
    }
    close(fd);

    attr->num_clocks = clock_count;
    attr->max_async_req = 1; // support 1 async request
    attr->reserved = 0;

    log_debug("CLOCK_PROTOCOL_ATTRIBUTES: num_clocks=%d", attr->num_clocks);
    return 0;
}

static int handle_clock_clock_attributes(SCMIDev *dev, uint16_t token,
                                        const struct iovec *req_iov,
                                        struct iovec *resp_iov) {
    struct scmi_request *req = req_iov->iov_base;
    uint32_t clock_id = *(uint32_t *)req->payload;

    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    // Validate response buffer size
    size_t expected_resp_size = sizeof(struct scmi_response) +
                                sizeof(struct scmi_msg_resp_clock_clock_attributes);
    if (resp_iov->iov_len < expected_resp_size) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    struct scmi_response *resp = resp_iov->iov_base;
    struct scmi_msg_resp_clock_clock_attributes *attr =
        (struct scmi_msg_resp_clock_clock_attributes *)resp->payload;

    // Build attributes field
    uint32_t attributes = 0;
    // Bit[0]: enabled
    if (clk->enabled) {
        attributes |= (1U << 0);
    }
    // Bit[1]: restricted clock? (we don't implement permissions, so assume not restricted)
    // attributes |= (0 << 1); // default 0

    // Bit[27]: extended config support? (we only support basic enable/disable)
    // -> set to 0

    // Bit[28]: parent clock identifier support? (we have parent_id)
    if (clk->parent_id != -1) {
        attributes |= (1U << 28);
    }

    // Bit[29]: extended name? (our names are "fake_clk_X", <16 bytes)
    // -> set to 0 (no extended name needed)

    // Bit[30], [31]: notifications (rate change, etc.) – not supported
    // -> leave as 0

    attr->attributes = attributes;

    // Copy clock name (max 15 chars + null)
    strncpy(attr->clock_name, clk->name, 15);
    attr->clock_name[15] = '\0';

    // // Enable delay: we don't model real delay, so return 0 (unsupported)
    // attr->clock_enable_delay = 0;

    log_info("***CLOCK_CLOCK_ATTRIBUTES: clock_id=%u, name=%s, enabled=%d",
             clock_id, attr->clock_name, clk->enabled);

    scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
    log_debug("CLOCK_ATTRIBUTES: clock_id=%u, name=%s, enabled=%d",
              clock_id, attr->clock_name, clk->enabled);
    return 0;
}
static int handle_clock_describe_rates(SCMIDev *dev, uint16_t token,
                                      const struct iovec *req_iov,
                                      struct iovec *resp_iov)
{
    struct scmi_request *req = req_iov->iov_base;
    uint32_t clock_id = *(uint32_t *)req->payload;
    uint32_t rate_index = *(uint32_t *)((uint8_t *)req->payload + 4);

    log_warn("****input clock_id=%u, rate_index=%u", clock_id, rate_index);

    /* Validate clock ID */
    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    uint32_t total_rates = clk->num_rates;

    /* Check if rate_index is out of range */
    if (rate_index > total_rates) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    uint32_t remaining = total_rates - rate_index; // number of rates from index onward

    /* Calculate how many rates we can fit in the response buffer */
    size_t resp_hdr_size = sizeof(struct scmi_response); // includes status (4 bytes)
    size_t fixed_payload_size = sizeof(uint32_t);        // num_rates_flags only
    size_t rate_entry_size = 2 * sizeof(uint32_t);       // each rate: low + high (8 bytes)

    if (resp_iov->iov_len < resp_hdr_size + fixed_payload_size) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    size_t available_for_rates = resp_iov->iov_len - resp_hdr_size - fixed_payload_size;
    uint32_t max_rates_by_buffer = (uint32_t)(available_for_rates / rate_entry_size);

    uint32_t num_return = (remaining < max_rates_by_buffer) ? remaining : max_rates_by_buffer;

    /* If buffer is too small to return even one rate (but remaining > 0), error */
    if (num_return == 0 && remaining > 0) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    /* Prepare response */
    struct scmi_response *resp = (struct scmi_response *)resp_iov->iov_base;
    uint32_t *payload = (uint32_t *)resp->payload;

    uint32_t remaining_after = remaining - num_return;

    /* num_rates_flags:
     *   Bit[12] = 0 (discrete rates)
     *   Bits[31:16] = remaining_after
     *   Bits[11:0] = num_return
     */
    uint32_t num_rates_flags = (remaining_after << 16) | (num_return & 0xFFFU);
    payload[0] = num_rates_flags;

    /* Fill rates: each rate is {low32, high32} */
    uint32_t *rates = &payload[1];
    for (uint32_t i = 0; i < num_return; i++) {
        uint64_t rate = clk->supported_rates[rate_index + i];
        rates[i * 2]     = (uint32_t)(rate & 0xFFFFFFFFULL);
        rates[i * 2 + 1] = (uint32_t)(rate >> 32);
    }

    return scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
}

static int handle_clock_rate_get(SCMIDev *dev, uint16_t token,
                               const struct iovec *req_iov,
                               struct iovec *resp_iov) {
    struct scmi_request *req = req_iov->iov_base;
    uint32_t clock_id = *(uint32_t *)req->payload;

    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    uint64_t rate = clk->current_rate;

    if (resp_iov->iov_len < sizeof(struct scmi_response) + 8) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    struct scmi_response *resp = resp_iov->iov_base;
    uint32_t *payload = (uint32_t *)resp->payload;
    payload[0] = (uint32_t)(rate & 0xFFFFFFFFULL);
    payload[1] = (uint32_t)((rate >> 32) & 0xFFFFFFFFULL);

    return scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
}

static int handle_clock_rate_set(SCMIDev *dev, uint16_t token,
                               const struct iovec *req_iov,
                               struct iovec *resp_iov) {
    struct scmi_request *req = req_iov->iov_base;
    uint8_t *p = req->payload;
    uint32_t flags = *(uint32_t *)p; p += 4;
    uint32_t clock_id = *(uint32_t *)p; p += 4;
    uint32_t rate_lo = *(uint32_t *)p; p += 4;
    uint32_t rate_hi = *(uint32_t *)p;
    uint64_t requested_rate = ((uint64_t)rate_hi << 32) | rate_lo;

    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    bool async = (flags & 0x1) != 0;
    bool ignore_delayed = (flags & 0x2) != 0;
    bool round_autonomous = (flags & 0x8) != 0;
    bool round_up = (flags & 0x4) != 0;

    // For simplicity, we only support synchronous mode
    if (async) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_SUPPORT);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    uint64_t actual_rate = find_closest_rate(requested_rate,
                                            clk->supported_rates,
                                            clk->num_rates,
                                            round_autonomous ? true : round_up,
                                            round_autonomous ? true : !round_up);

    // Simulate setting rate
    clk->current_rate = actual_rate;

    return scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
}

static int handle_clock_config_get(SCMIDev *dev, uint16_t token,
                                 const struct iovec *req_iov,
                                 struct iovec *resp_iov) {
    struct scmi_request *req = req_iov->iov_base;
    uint32_t clock_id = *(uint32_t *)req->payload;
    uint32_t flags = *(uint32_t *)((uint8_t *)req->payload + 4);

    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    uint32_t ext_type = flags & 0xFF;

    if (ext_type != 0) {
        // Only support basic enable/disable for now
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_PARAMS);
    }

    if (resp_iov->iov_len < sizeof(struct scmi_response) + 12) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    struct scmi_response *resp = resp_iov->iov_base;
    uint32_t *payload = (uint32_t *)resp->payload;
    payload[0] = 0; // attributes (reserved)
    payload[1] = clk->enabled ? 1 : 0; // config: bit0 = enable
    payload[2] = 0; // extended_config_val (unused)

    return scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
}

static int handle_clock_config_set(SCMIDev *dev, uint16_t token,
                                 const struct iovec *req_iov,
                                 struct iovec *resp_iov) {
    struct scmi_request *req = req_iov->iov_base;
    uint8_t *p = req->payload;
    uint32_t clock_id = *(uint32_t *)p; p += 4;
    uint32_t attributes = *(uint32_t *)p; p += 4;
    uint32_t ext_val = *(uint32_t *)p;

    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    uint32_t enable_disable = attributes & 0x3;
    uint32_t ext_type = (attributes >> 16) & 0xFF;

    if (ext_type != 0) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_SUPPORT);
    }

    if (enable_disable == 3) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_PARAMS);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    clk->enabled = (enable_disable == 1);

    return scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
}

static int handle_clock_name_get(SCMIDev *dev, uint16_t token,
                               const struct iovec *req_iov,
                               struct iovec *resp_iov) {
    struct scmi_request *req = req_iov->iov_base;
    uint32_t clock_id = *(uint32_t *)req->payload;

    if (!is_valid_clock_id(clock_id)) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_ENTRY);
    }

    struct fake_clock *clk = &g_clocks[clock_id];
    size_t name_len = strlen(clk->name) + 1; // include null
    size_t payload_size = 4 + 64; // flags (4) + name[64]

    if (resp_iov->iov_len < sizeof(struct scmi_response) + payload_size) {
        return scmi_make_response(dev, token, resp_iov, SCMI_ERR_RANGE);
    }

    struct scmi_response *resp = resp_iov->iov_base;
    uint32_t *flags = (uint32_t *)resp->payload;
    char *name = (char *)(flags + 1);

    *flags = 0;
    strncpy(name, clk->name, 63);
    name[63] = '\0';

    return scmi_make_response(dev, token, resp_iov, SCMI_SUCCESS);
}

/* Main request handler */
static int virtio_scmi_clock_handle_req(SCMIDev *dev, uint8_t msg_id, uint16_t token,
                                     const struct iovec *req_iov, struct iovec *resp_iov) {
    if (resp_iov->iov_len < sizeof(struct scmi_response) || resp_iov->iov_base == NULL) {
        log_error("Invalid response buffer");
        return SCMI_ERR_PARAMS;
    }

    init_fake_clocks();

    switch (msg_id) {
        case SCMI_COMMON_MSG_VERSION:
            return handle_clock_version(dev, token, req_iov, resp_iov);
        case SCMI_COMMON_MSG_PROTOCOL_ATTRIBUTES:
            return handle_clock_protocol_attributes(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_CLK_ATTRIBUTES:
            return handle_clock_clock_attributes(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_DESCRIBE_RATES:
            return handle_clock_describe_rates(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_RATE_SET:
            return handle_clock_rate_set(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_RATE_GET:
            return handle_clock_rate_get(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_CONFIG_SET:
            return handle_clock_config_set(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_CONFIG_GET:
            return handle_clock_config_get(dev, token, req_iov, resp_iov);
        case SCMI_CLOCK_MSG_NAME_GET:
            return handle_clock_name_get(dev, token, req_iov, resp_iov);
        default:
            log_warn("Unsupported Clock protocol message: 0x%x", msg_id);
            return SCMI_ERR_SUPPORT;
    }
}

/* Clock Protocol Operations */
static const struct scmi_protocol clock_protocol = {
    .id = SCMI_PROTO_ID_CLOCK,
    .handle_request = virtio_scmi_clock_handle_req,
};

/* Initialize Clock Protocol */
int virtio_scmi_clock_init(void) {
    init_fake_clocks();
    return scmi_register_protocol(&clock_protocol);
}