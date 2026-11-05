#include "hvisor.h"
#include "server.h"
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

extern bool __clk_is_enabled(const struct clk *clk);
extern const char *__clk_get_name(const struct clk *clk);

static struct clk **clock_cache;
static u32 clock_max_num;

/* Per-zone enable accounting: clk_zone_en[zone][clock_id] counts how many
 * times zone @zone enabled clock @clock_id through SCMI (rows allocated
 * lazily). The kernel driver owns this state because it executes the clk
 * ops and sees zone start/shutdown; zone shutdown / next zone start undo
 * exactly the zone's own contributions. */
static u32 **clk_zone_en;
static DEFINE_MUTEX(clock_zone_lock);

static struct clk *clock_get_cached(u32 clock_id);

static u32 *clock_zone_row(u32 zone) {
    u32 *row;

    if (zone >= SCMI_MAX_ZONES || clock_max_num == 0)
        return NULL;
    row = clk_zone_en ? clk_zone_en[zone] : NULL;
    if (!row) {
        row = kcalloc(clock_max_num, sizeof(u32), GFP_KERNEL);
        if (!row)
            return NULL;
        if (!clk_zone_en) {
            clk_zone_en = kcalloc(SCMI_MAX_ZONES, sizeof(u32 *), GFP_KERNEL);
            if (!clk_zone_en) {
                kfree(row);
                return NULL;
            }
        }
        clk_zone_en[zone] = row;
    }
    return row;
}

void clock_zone_adjust(u32 zone, u32 clk, bool enable) {
    u32 *row;

    mutex_lock(&clock_zone_lock);
    row = clock_zone_row(zone);
    if (row && clk < clock_max_num) {
        if (enable)
            row[clk]++;
        else if (row[clk] > 0)
            row[clk]--;
    }
    mutex_unlock(&clock_zone_lock);
}

void clock_zone_release(u32 zone) {
    u32 *row;
    u32 clk;

    mutex_lock(&clock_zone_lock);
    row = (zone < SCMI_MAX_ZONES && clk_zone_en) ? clk_zone_en[zone] : NULL;
    if (!row) {
        mutex_unlock(&clock_zone_lock);
        return;
    }
    for (clk = 0; clk < clock_max_num; clk++) {
        while (row[clk] > 0) {
            struct clk *c = clock_get_cached(clk);
            if (IS_ERR(c))
                break;
            clk_disable_unprepare(c);
            row[clk]--;
        }
    }
    mutex_unlock(&clock_zone_lock);
}

int clock_init(void) {
    struct device_node *np;
    int c;

    np = hvisor_get_node();
    if (!np)
        return -ENODEV;

    c = of_count_phandle_with_args(np, "clocks", "#clock-cells");
    if (c == -ENOENT) {
        pr_debug(
            "no clocks property in hvisor node, clock protocol disabled\n");
        clock_max_num = 0;
        clock_cache = NULL;
        return 0;
    }
    if (c < 0)
        return c;
    clock_max_num = (u32)c;
    clock_cache = kcalloc(clock_max_num, sizeof(*clock_cache), GFP_KERNEL);
    if (!clock_cache)
        return -ENOMEM;
    return 0;
}

static struct clk *clock_get_cached(u32 clk_id) {
    if (clk_id >= clock_max_num)
        return ERR_PTR(-ENOENT);

    if (clock_cache[clk_id])
        return clock_cache[clk_id];

    clock_cache[clk_id] = of_clk_get(hvisor_get_node(), clk_id);
    return clock_cache[clk_id];
}

static int get_clock_count(void) { return (int)clock_max_num; }

static int get_clock_config(u32 clock_id, u32 *config,
                            u32 *extended_config_val) {
    struct clk *clk;

    clk = clock_get_cached(clock_id);
    if (IS_ERR(clk))
        return PTR_ERR(clk) == -ENOENT ? -ENOENT : PTR_ERR(clk);

    *config = __clk_is_enabled(clk) ? 1 : 0;
    *extended_config_val = 0;
    return 0;
}

static int get_clock_name(u32 clock_id, char *name) {
    struct clk *clk;
    const char *clk_name;

    clk = clock_get_cached(clock_id);
    if (IS_ERR(clk))
        return PTR_ERR(clk) == -ENOENT ? -ENOENT : PTR_ERR(clk);

    clk_name = __clk_get_name(clk);
    if (clk_name) {
        strncpy(name, clk_name, 63);
        name[63] = '\0';
    } else {
        snprintf(name, 64, "unknown_clk_%u", clock_id);
    }
    return 0;
}

static int get_clock_attributes(u32 clock_id, u32 *enabled, u32 *parent_id,
                                char *clock_name, u32 *is_valid) {
    struct clk *clk;
    const char *name;

    clk = clock_get_cached(clock_id);
    if (IS_ERR(clk))
        return PTR_ERR(clk) == -ENOENT ? -ENOENT : PTR_ERR(clk);

    *is_valid = 1;
    *enabled = __clk_is_enabled(clk) ? 1 : 0;
    *parent_id = -1;

    name = __clk_get_name(clk);
    if (name) {
        strncpy(clock_name, name, 63);
        clock_name[63] = '\0';
    } else {
        snprintf(clock_name, 64, "unknown_clk_%u", clock_id);
    }
    return 0;
}

static int get_clock_rate(u32 clock_id, u64 *rate) {
    struct clk *clk;

    clk = clock_get_cached(clock_id);
    if (IS_ERR(clk))
        return PTR_ERR(clk) == -ENOENT ? -ENOENT : PTR_ERR(clk);

    *rate = (u64)clk_get_rate(clk);
    return 0;
}

static int set_clock_rate(u32 clock_id, u64 rate) {
    int ret;
    struct clk *clk;

    clk = clock_get_cached(clock_id);
    if (IS_ERR(clk))
        return PTR_ERR(clk) == -ENOENT ? -ENOENT : PTR_ERR(clk);

    ret = clk_set_rate(clk, (unsigned long)rate);
    if (ret)
        pr_err("Failed to set clock[%u] rate to %llu Hz, error=%d\n", clock_id,
               rate, ret);
    return ret;
}

static int set_clock_config(u32 clock_id, u32 config) {
    int ret = 0;
    struct clk *clk;

    clk = clock_get_cached(clock_id);
    if (IS_ERR(clk))
        return PTR_ERR(clk) == -ENOENT ? -ENOENT : PTR_ERR(clk);

    if (config & 1)
        ret = clk_prepare_enable(clk);
    else
        clk_disable_unprepare(clk);

    if (ret)
        pr_err("Failed to set clock[%u] config 0x%x, error=%d\n", clock_id,
               config, ret);
    return ret;
}

void clock_ctrl_finish(void) {
    u32 i;

    if (!clock_cache)
        goto out;

    for (i = 0; i < clock_max_num; i++) {
        if (clock_cache[i] && !IS_ERR(clock_cache[i]))
            clk_put(clock_cache[i]);
        clock_cache[i] = NULL;
    }

out:
    if (clk_zone_en) {
        u32 z;

        for (z = 0; z < SCMI_MAX_ZONES; z++)
            kfree(clk_zone_en[z]);
        kfree(clk_zone_en);
        clk_zone_en = NULL;
    }
    kfree(clock_cache);
    clock_cache = NULL;
    clock_max_num = 0;
}

int hvisor_scmi_clock_ioctl(struct hvisor_scmi_clock_args __user *user_args) {
    struct hvisor_scmi_clock_args args;

    if (copy_from_user(&args, user_args, sizeof(struct hvisor_scmi_clock_args)))
        return -EFAULT;

    switch (args.subcmd) {
    case HVISOR_SCMI_CLOCK_GET_COUNT: {
        int ret = get_clock_count();
        if (ret < 0)
            return ret;
        args.u.clock_count = (u32)ret;
        if (copy_to_user(&user_args->u.clock_count, &args.u.clock_count,
                         sizeof(u32)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_GET_ATTRIBUTES: {
        u32 enabled, parent_id, is_valid;
        char clock_name[64];
        int ret = get_clock_attributes(args.u.clock_attr.clock_id, &enabled,
                                       &parent_id, clock_name, &is_valid);
        if (ret < 0)
            return ret;
        args.u.clock_attr.enabled = enabled;
        args.u.clock_attr.parent_id = parent_id;
        args.u.clock_attr.is_valid = is_valid;
        strncpy(args.u.clock_attr.clock_name, clock_name, 63);
        args.u.clock_attr.clock_name[63] = '\0';
        if (copy_to_user(&user_args->u.clock_attr, &args.u.clock_attr,
                         sizeof(args.u.clock_attr)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_DESCRIBE_RATES:
        return 0;
    case HVISOR_SCMI_CLOCK_RATE_GET: {
        u64 rate = 0;
        int ret = get_clock_rate(args.u.clock_rate_info.clock_id, &rate);
        if (ret < 0)
            return ret;
        args.u.clock_rate_info.rate = rate;
        if (copy_to_user(&user_args->u.clock_rate_info, &args.u.clock_rate_info,
                         sizeof(args.u.clock_rate_info)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_RATE_SET:
        return set_clock_rate(args.u.clock_rate_set_info.clock_id,
                              args.u.clock_rate_set_info.rate);
    case HVISOR_SCMI_CLOCK_CONFIG_GET: {
        u32 config, extended_config_val;
        int ret = get_clock_config(args.u.clock_config_info.clock_id, &config,
                                   &extended_config_val);
        if (ret < 0)
            return ret;
        args.u.clock_config_info.config = config;
        args.u.clock_config_info.extended_config_val = extended_config_val;
        if (copy_to_user(&user_args->u.clock_config_info,
                         &args.u.clock_config_info,
                         sizeof(args.u.clock_config_info)))
            return -EFAULT;
        return 0;
    }
    case HVISOR_SCMI_CLOCK_CONFIG_SET: {
        u32 clk = args.u.clock_config_info.clock_id;
        u32 cfg = args.u.clock_config_info.config;
        int r = set_clock_config(clk, cfg);

        if (r == 0)
            clock_zone_adjust(args.zone_id, clk, cfg & 1);
        return r;
    }
    case HVISOR_SCMI_CLOCK_NAME_GET: {
        char name[64];
        int ret = get_clock_name(args.u.clock_name_info.clock_id, name);
        if (ret < 0)
            return ret;
        strncpy(args.u.clock_name_info.name, name, 63);
        args.u.clock_name_info.name[63] = '\0';
        if (copy_to_user(&user_args->u.clock_name_info, &args.u.clock_name_info,
                         sizeof(args.u.clock_name_info)))
            return -EFAULT;
        return 0;
    }
    default:
        return -EINVAL;
    }
}
