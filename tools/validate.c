#include "validate.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/cfgchk.h"
#include "log.h"
#include "safe_cjson.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CFGCHK_JSON_SUFFIX ".json"
#define CFGCHK_DTS_SUFFIX ".dts"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct zone_parse_meta {
    char name[128];
};

static char *trim_whitespace(char *str);
static bool has_suffix(const char *name, const char *suffix);
static int join_path(const char *dir, const char *leaf, char *out, size_t len);
static int read_text_file(const char *path, char **buf_out);
static int parse_hex_or_dec(const char *token, unsigned long long *value);
static int parse_first_unsigned(const char *cursor, unsigned long long *value);
static int parse_board_file(const char *path, struct cfgchk_board_info *board);
static int parse_zone_json(const char *path, struct cfgchk_zone_summary *zone,
                           struct zone_parse_meta *meta);
static int parse_zone_dts(const char *path, struct cfgchk_dts_summary *dts,
                          const struct cfgchk_board_info *board);
static int parse_root_dts(const char *path, struct cfgchk_dts_summary *dts,
                          struct cfgchk_board_info *board);
static void apply_reservation_flags(const struct cfgchk_board_info *board,
                                    struct cfgchk_zone_summary *zone);
static int build_cfg_request(const char *target_json,
                             const struct cfgchk_board_info *board,
                             struct cfgchk_zone_summary *target_zone,
                             const struct cfgchk_dts_summary *dts_zone,
                             const struct cfgchk_dts_summary *dts_root,
                             struct cfgchk_request *req);
static int locate_board_file(const char *json_dir, char *out, size_t len);
static int locate_root_dts(const char *platform_dir, char *out, size_t len);
static int locate_zone_dts(const char *platform_dir, const char *json_dir,
                           const char *json_base, const char *zone_name,
                           uint32_t zone_id, char *out, size_t len);

static inline bool range_within(unsigned long long start,
                                unsigned long long size,
                                unsigned long long base,
                                unsigned long long end) {
    unsigned long long range_end;
    if (!size)
        return false;
    if (__builtin_add_overflow(start, size, &range_end))
        return false;
    return start >= base && range_end <= end;
}

static int get_dirname(const char *path, char *out, size_t len) {
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof(tmp))
        return -1;
    strcpy(tmp, path);
    char *dir = dirname(tmp);
    if (!dir)
        return -1;
    if (snprintf(out, len, "%s", dir) >= (int)len)
        return -1;
    return 0;
}

static int get_basename(const char *path, char *out, size_t len) {
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof(tmp))
        return -1;
    strcpy(tmp, path);
    char *base = basename(tmp);
    if (!base)
        return -1;
    if (snprintf(out, len, "%s", base) >= (int)len)
        return -1;
    return 0;
}

static int ensure_realpath(const char *path, char *resolved, size_t len) {
    char *rp = realpath(path, NULL);
    if (!rp)
        return -1;
    int ret = snprintf(resolved, len, "%s", rp);
    free(rp);
    if (ret < 0 || (size_t)ret >= len)
        return -1;
    return 0;
}

static int join_path(const char *dir, const char *leaf, char *out, size_t len) {
    if (!dir || !leaf || !out)
        return -1;
    size_t dir_len = strlen(dir);
    const char *fmt =
        (dir_len > 0 && dir[dir_len - 1] == '/') ? "%s%s" : "%s/%s";
    if (snprintf(out, len, fmt, dir, leaf) >= (int)len)
        return -1;
    return 0;
}

static bool has_suffix(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suf_len = strlen(suffix);
    if (name_len < suf_len)
        return false;
    return strcasecmp(name + name_len - suf_len, suffix) == 0;
}

static char *trim_whitespace(char *str) {
    char *start = str;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (*start == '\0')
        return start;
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }
    return start;
}

static int read_text_file(const char *path, char **buf_out) {
    FILE *fp = fopen(path, "rb");
    char *buf;
    size_t len;

    if (!fp) {
        log_error("Failed to open %s (%s)", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        log_error("Failed to seek %s", path);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        log_error("Failed to get size of %s", path);
        return -1;
    }
    rewind(fp);
    len = (size_t)sz;
    buf = calloc(1, len + 1);
    if (!buf) {
        fclose(fp);
        log_error("Out of memory reading %s", path);
        return -1;
    }
    if (len && fread(buf, 1, len, fp) != len) {
        fclose(fp);
        free(buf);
        log_error("Failed to read %s", path);
        return -1;
    }
    fclose(fp);
    *buf_out = buf;
    return 0;
}

static int parse_hex_or_dec(const char *token, unsigned long long *value) {
    char *end = NULL;
    unsigned long long val;
    if (!token || !value)
        return -1;
    errno = 0;
    val = strtoull(token, &end, 0);
    if (errno || end == token)
        return -1;
    *value = val;
    return 0;
}

static int parse_first_unsigned(const char *cursor, unsigned long long *value) {
    while (*cursor && !isdigit((unsigned char)*cursor))
        cursor++;
    if (!*cursor)
        return -1;
    return parse_hex_or_dec(cursor, value);
}

static int parse_cpu_expr_token(const char *token, unsigned long long *cpu) {
    char *lt;
    if (!token)
        return -1;
    lt = strstr(token, "<<");
    if (lt) {
        lt += 2;
        return parse_hex_or_dec(lt, cpu);
    }
    if (strstr(token, "1 <<")) {
        lt = strstr(token, "1 <<");
        lt += 3;
        return parse_hex_or_dec(lt, cpu);
    }
    return parse_hex_or_dec(token, cpu);
}

static int extract_between(const char *start, char open, char close, char *dst,
                           size_t len) {
    const char *o = strchr(start, open);
    const char *c;
    size_t copy_len;
    if (!o)
        return -1;
    c = strchr(o + 1, close);
    if (!c || c <= o)
        return -1;
    copy_len = (size_t)(c - o - 1);
    if (copy_len >= len)
        copy_len = len - 1;
    memcpy(dst, o + 1, copy_len);
    dst[copy_len] = '\0';
    return 0;
}

static int parse_symbol_u64(const char *buf, const char *symbol,
                            unsigned long long *value) {
    const char *cursor = strstr(buf, symbol);
    if (!cursor)
        return -1;
    return parse_first_unsigned(cursor, value);
}

static int parse_symbol_list(const char *buf, const char *symbol, char open,
                             char close, char *out, size_t len) {
    const char *cursor = strstr(buf, symbol);
    if (!cursor)
        return -1;
    return extract_between(cursor, open, close, out, len);
}

static int derive_cpus_from_mpidr(const char *buf, unsigned int *count) {
    char list[2048];
    unsigned long long mpidr;
    unsigned int cnt = 0;
    char *saveptr;
    char *token;

    if (parse_symbol_list(buf, "BOARD_MPIDR_MAPPINGS", '[', ']', list,
                          sizeof(list)) != 0)
        return -1;
    token = strtok_r(list, ",", &saveptr);
    while (token && cnt < CFGCHK_MAX_CPUS) {
        token = trim_whitespace(token);
        if (parse_hex_or_dec(token, &mpidr) == 0)
            cnt++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    if (!cnt)
        return -1;
    *count = cnt;
    return 0;
}

static int board_parse_physmem(const char *buf,
                               struct cfgchk_board_info *board) {
    char tuples[4096];
    char *token;
    char *saveptr;
    if (parse_symbol_list(buf, "BOARD_PHYSMEM_LIST", '[', ']', tuples,
                          sizeof(tuples)) != 0) {
        log_error("BOARD_PHYSMEM_LIST not found in board.rs");
        return -1;
    }
    token = strtok_r(tuples, "()", &saveptr);
    while (token) {
        char entry[256];
        char *fields[3] = {NULL};
        unsigned long long start, end;
        char *inner = trim_whitespace(token);
        if (*inner == '\0') {
            token = strtok_r(NULL, "()", &saveptr);
            continue;
        }
        snprintf(entry, sizeof(entry), "%s", inner);
        char *field_save;
        char *field = strtok_r(entry, ",", &field_save);
        for (int i = 0; i < 3 && field; ++i) {
            fields[i] = trim_whitespace(field);
            field = strtok_r(NULL, ",", &field_save);
        }
        if (!fields[0] || !fields[1] || !fields[2]) {
            token = strtok_r(NULL, "()", &saveptr);
            continue;
        }
        if (parse_hex_or_dec(fields[0], &start) != 0 ||
            parse_hex_or_dec(fields[1], &end) != 0) {
            token = strtok_r(NULL, "()", &saveptr);
            continue;
        }
        if (board->physmem_count >= CFGCHK_MAX_PHYSMEM) {
            log_warn("BOARD_PHYSMEM_LIST exceeds limit (%u)",
                     CFGCHK_MAX_PHYSMEM);
            break;
        }
        struct cfgchk_physmem_range *pm =
            &board->physmem[board->physmem_count++];
        pm->start = start;
        pm->end = end;
        pm->type = (strstr(fields[2], "Normal") != NULL) ? CFGCHK_MEM_RAM
                                                         : CFGCHK_MEM_IO;
        pm->rsvd = 0;
        token = strtok_r(NULL, "()", &saveptr);
    }
    return 0;
}

static int parse_board_mpidr(const char *buf, struct cfgchk_board_info *board) {
    char list[4096];
    char *saveptr;
    char *token;
    if (parse_symbol_list(buf, "BOARD_MPIDR_MAPPINGS", '[', ']', list,
                          sizeof(list)) != 0) {
        log_warn(
            "BOARD_MPIDR_MAPPINGS not found, CPU<->MPIDR mapping fallback");
        return 0;
    }
    token = strtok_r(list, ",", &saveptr);
    unsigned int idx = 0;
    while (token && idx < CFGCHK_MAX_CPUS) {
        unsigned long long mpidr;
        token = trim_whitespace(token);
        if (parse_hex_or_dec(token, &mpidr) == 0)
            board->mpidr_map[idx++] = mpidr;
        token = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

static int parse_board_irqs(const char *buf, struct cfgchk_board_info *board) {
    char list[2048];
    char *token;
    char *saveptr;
    if (parse_symbol_list(buf, "ROOT_ZONE_IRQS", '[', ']', list,
                          sizeof(list)) != 0) {
        log_warn("ROOT_ZONE_IRQS not found");
        return 0;
    }
    token = strtok_r(list, ",", &saveptr);
    while (token && board->root_irq_count < CFGCHK_MAX_IRQS) {
        unsigned long long irq;
        token = trim_whitespace(token);
        if (parse_hex_or_dec(token, &irq) == 0)
            board->root_irqs[board->root_irq_count++] = (uint32_t)irq;
        token = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

static int parse_board_root_cpus(const char *buf,
                                 struct cfgchk_board_info *board) {
    char expr[512];
    if (parse_symbol_list(buf, "ROOT_ZONE_CPUS", '=', ';', expr,
                          sizeof(expr)) != 0) {
        log_warn("ROOT_ZONE_CPUS not found");
        return 0;
    }
    char *token;
    char *saveptr;
    token = strtok_r(expr, "|", &saveptr);
    while (token) {
        unsigned long long cpu;
        if (parse_cpu_expr_token(trim_whitespace(token), &cpu) == 0 && cpu < 64)
            board->root_cpu_bitmap |= 1ULL << cpu;
        token = strtok_r(NULL, "|", &saveptr);
    }
    return 0;
}

static int parse_board_file(const char *path, struct cfgchk_board_info *board) {
    char *buf = NULL;
    unsigned long long tmp;
    unsigned int derived;
    int ret = -1;

    memset(board, 0, sizeof(*board));
    if (read_text_file(path, &buf) != 0)
        return -1;

    if (parse_symbol_u64(buf, "BOARD_NCPUS", &tmp) == 0) {
        board->total_cpus = (uint32_t)tmp;
    } else if (derive_cpus_from_mpidr(buf, &derived) == 0) {
        log_warn("BOARD_NCPUS missing, derived from BOARD_MPIDR_MAPPINGS (%u)",
                 derived);
        board->total_cpus = derived;
    } else {
        log_error("Failed to determine BOARD_NCPUS from %s", path);
        goto out;
    }

    parse_board_root_cpus(buf, board);
    if (board_parse_physmem(buf, board) != 0)
        goto out;
    parse_board_irqs(buf, board);
    parse_board_mpidr(buf, board);

    if (parse_symbol_u64(buf, "gicd_base", &tmp) == 0)
        board->gicd_base = tmp;
    if (parse_symbol_u64(buf, "gicd_size", &tmp) == 0)
        board->gicd_size = tmp;
    if (parse_symbol_u64(buf, "gicr_base", &tmp) == 0)
        board->gicr_base = tmp;
    if (parse_symbol_u64(buf, "gicr_size", &tmp) == 0)
        board->gicr_size = tmp;
    if (parse_symbol_u64(buf, "gic_version", &tmp) == 0) {
        board->gic_version = (uint32_t)tmp;
    } else {
        board->gic_version = board->gicr_base ? 3 : 2;
    }

    ret = 0;
out:
    free(buf);
    return ret;
}

static int parse_zone_json(const char *path, struct cfgchk_zone_summary *zone,
                           struct zone_parse_meta *meta) {
    char *buf = NULL;
    cJSON *root = NULL;
    cJSON *item;
    int ret = -1;
    unsigned long long tmp = 0;

    memset(zone, 0, sizeof(*zone));
    if (meta)
        memset(meta, 0, sizeof(*meta));

    if (read_text_file(path, &buf) != 0)
        return -1;
    root = SAFE_CJSON_PARSE(buf);
    if (!root)
        goto out;

    item = SAFE_CJSON_GET_OBJECT_ITEM(root, "zone_id");
    if (!cJSON_IsNumber(item)) {
        log_error("%s: zone_id missing", path);
        goto out;
    }
    zone->zone_id = (uint32_t)item->valuedouble;

    item = SAFE_CJSON_GET_OBJECT_ITEM(root, "name");
    if (meta && cJSON_IsString(item) && item->valuestring)
        snprintf(meta->name, sizeof(meta->name), "%s", item->valuestring);

    cJSON *cpus = SAFE_CJSON_GET_OBJECT_ITEM(root, "cpus");
    int cpu_count = SAFE_CJSON_GET_ARRAY_SIZE(cpus);
    if (!cpu_count || cpu_count > CFGCHK_MAX_CPUS) {
        log_error("%s: invalid cpus array", path);
        goto out;
    }
    for (int i = 0; i < cpu_count; ++i) {
        cJSON *cpu = SAFE_CJSON_GET_ARRAY_ITEM(cpus, i);
        if (!cJSON_IsNumber(cpu)) {
            log_error("%s: cpu entry not numeric", path);
            goto out;
        }
        uint32_t idx = (uint32_t)cpu->valuedouble;
        zone->cpus[i] = idx;
        zone->cpu_bitmap |= 1ULL << idx;
    }
    zone->cpu_count = cpu_count;

    cJSON *mems = SAFE_CJSON_GET_OBJECT_ITEM(root, "memory_regions");
    int mem_count = SAFE_CJSON_GET_ARRAY_SIZE(mems);
    for (int i = 0; i < mem_count && zone->mem_count < CFGCHK_MAX_MEM; ++i) {
        cJSON *region = SAFE_CJSON_GET_ARRAY_ITEM(mems, i);
        cJSON *type = SAFE_CJSON_GET_OBJECT_ITEM(region, "type");
        cJSON *start = SAFE_CJSON_GET_OBJECT_ITEM(region, "physical_start");
        cJSON *size = SAFE_CJSON_GET_OBJECT_ITEM(region, "size");
        if (!cJSON_IsString(type) ||
            (!cJSON_IsString(start) && !cJSON_IsNumber(start)) ||
            (!cJSON_IsString(size) && !cJSON_IsNumber(size))) {
            log_warn("%s: malformed memory region entry", path);
            continue;
        }
        struct cfgchk_mem_region *mr = &zone->mem_regions[zone->mem_count];
        if (cJSON_IsString(start))
            parse_hex_or_dec(start->valuestring, &tmp);
        else
            tmp = (unsigned long long)start->valuedouble;
        mr->start = tmp;

        if (cJSON_IsString(size))
            parse_hex_or_dec(size->valuestring, &tmp);
        else
            tmp = (unsigned long long)size->valuedouble;
        mr->size = tmp;

        const char *type_str = type->valuestring;
        if (strcasecmp(type_str, "ram") == 0) {
            mr->type = CFGCHK_MEM_RAM;
        } else if (strcasecmp(type_str, "virtio") == 0) {
            mr->type = CFGCHK_MEM_VIRTIO;
        } else {
            mr->type = CFGCHK_MEM_IO;
        }
        mr->flags = 0;
        if (mr->type == CFGCHK_MEM_VIRTIO &&
            zone->virtio_count < CFGCHK_MAX_VIRTIO) {
            struct cfgchk_virtio_desc *vd = &zone->virtio[zone->virtio_count++];
            vd->base = mr->start;
            vd->size = mr->size;
            vd->irq = 0;
        }
        zone->mem_count++;
    }

    cJSON *irqs = SAFE_CJSON_GET_OBJECT_ITEM(root, "interrupts");
    int irq_count = SAFE_CJSON_GET_ARRAY_SIZE(irqs);
    for (int i = 0; i < irq_count && zone->irq_count < CFGCHK_MAX_IRQS; ++i) {
        cJSON *irq = SAFE_CJSON_GET_ARRAY_ITEM(irqs, i);
        if (!cJSON_IsNumber(irq))
            continue;
        zone->irqs[zone->irq_count++] = (uint32_t)irq->valuedouble;
    }

    for (uint32_t i = 0; i < zone->virtio_count && i < zone->irq_count; ++i) {
        zone->virtio[i].irq = zone->irqs[i];
    }

    cJSON *arch = SAFE_CJSON_GET_OBJECT_ITEM(root, "arch_config");
    if (arch) {
        cJSON *gic_version = SAFE_CJSON_GET_OBJECT_ITEM(arch, "gic_version");
        if (cJSON_IsString(gic_version) && gic_version->valuestring) {
            if (strcasecmp(gic_version->valuestring, "v3") == 0 ||
                strcasecmp(gic_version->valuestring, "gicv3") == 0) {
                zone->gic_version = 3;
            } else if (strcasecmp(gic_version->valuestring, "v2") == 0 ||
                       strcasecmp(gic_version->valuestring, "gicv2") == 0) {
                zone->gic_version = 2;
            }
        } else if (cJSON_IsNumber(gic_version)) {
            zone->gic_version = (uint32_t)gic_version->valuedouble;
        }
        cJSON *field = SAFE_CJSON_GET_OBJECT_ITEM(arch, "gicd_base");
        if (field && cJSON_IsString(field) &&
            parse_hex_or_dec(field->valuestring, &tmp) == 0) {
            zone->gicd_base = tmp;
        } else if (field && cJSON_IsNumber(field)) {
            zone->gicd_base = (unsigned long long)field->valuedouble;
        }
        field = SAFE_CJSON_GET_OBJECT_ITEM(arch, "gicd_size");
        if (field && cJSON_IsString(field) &&
            parse_hex_or_dec(field->valuestring, &tmp) == 0) {
            zone->gicd_size = tmp;
        } else if (field && cJSON_IsNumber(field)) {
            zone->gicd_size = (unsigned long long)field->valuedouble;
        }
        field = SAFE_CJSON_GET_OBJECT_ITEM(arch, "gicr_base");
        if (field && cJSON_IsString(field) &&
            parse_hex_or_dec(field->valuestring, &tmp) == 0) {
            zone->gicr_base = tmp;
        } else if (field && cJSON_IsNumber(field)) {
            zone->gicr_base = (unsigned long long)field->valuedouble;
        }
        field = SAFE_CJSON_GET_OBJECT_ITEM(arch, "gicr_size");
        if (field && cJSON_IsString(field) &&
            parse_hex_or_dec(field->valuestring, &tmp) == 0) {
            zone->gicr_size = tmp;
        } else if (field && cJSON_IsNumber(field)) {
            zone->gicr_size = (unsigned long long)field->valuedouble;
        }
    }

    ret = 0;
out:
    if (root)
        cJSON_Delete(root);
    free(buf);
    return ret;
}

static int mpidr_to_cpu(const struct cfgchk_board_info *board,
                        unsigned long long mpidr, uint32_t *cpu_out) {
    if (!board || !cpu_out)
        return -1;
    for (uint32_t i = 0; i < board->total_cpus && i < CFGCHK_MAX_CPUS; ++i) {
        if (board->mpidr_map[i] == mpidr ||
            (board->mpidr_map[i] & 0xffffffffULL) == (mpidr & 0xffffffffULL)) {
            *cpu_out = i;
            return 0;
        }
    }
    *cpu_out = (uint32_t)(mpidr & 0xff);
    return 0;
}

static int parse_reg_cells(const char *line, unsigned long long *base,
                           unsigned long long *size) {
    char buf[256];
    unsigned long long values[8];
    size_t count = 0;
    if (extract_between(line, '<', '>', buf, sizeof(buf)) != 0)
        return -1;
    char *token;
    char *saveptr;
    token = strtok_r(buf, " ,\t", &saveptr);
    while (token && count < ARRAY_SIZE(values)) {
        if (parse_hex_or_dec(token, &values[count]) != 0)
            return -1;
        count++;
        token = strtok_r(NULL, " ,\t", &saveptr);
    }
    if (count == 1) {
        *base = values[0];
        *size = 0;
    } else if (count == 2) {
        *base = values[0];
        *size = values[1];
    } else if (count >= 4) {
        *base = (values[0] << 32) | values[1];
        *size = (values[2] << 32) | values[3];
    } else {
        return -1;
    }
    return 0;
}

static int parse_interrupt_cells(const char *line, uint32_t *irq) {
    char buf[256];
    unsigned long long values[8];
    size_t count = 0;
    if (extract_between(line, '<', '>', buf, sizeof(buf)) != 0)
        return -1;
    char *token;
    char *saveptr;
    token = strtok_r(buf, " ,\t", &saveptr);
    while (token && count < ARRAY_SIZE(values)) {
        if (parse_hex_or_dec(token, &values[count]) != 0)
            return -1;
        count++;
        token = strtok_r(NULL, " ,\t", &saveptr);
    }
    if (!count)
        return -1;
    *irq = (uint32_t)(count >= 2 ? values[1] : values[0]);
    return 0;
}

static int parse_zone_dts(const char *path, struct cfgchk_dts_summary *dts,
                          const struct cfgchk_board_info *board) {
    FILE *fp = fopen(path, "r");
    char line[512];
    bool in_cpu = false;
    bool in_memory = false;
    int current_virtio = -1;
    int ret = -1;

    if (!fp) {
        log_error("Failed to open %s (%s)", path, strerror(errno));
        return -1;
    }
    memset(dts, 0, sizeof(*dts));

    while (fgets(line, sizeof(line), fp)) {
        char *trim = trim_whitespace(line);
        if (strncmp(trim, "cpu@", 4) == 0) {
            in_cpu = true;
            continue;
        }
        if (in_cpu && strstr(trim, "reg =") != NULL) {
            unsigned long long mpidr;
            unsigned long long unused_size = 0;
            uint32_t cpu;
            if (parse_reg_cells(trim, &mpidr, &unused_size) == 0 &&
                mpidr_to_cpu(board, mpidr, &cpu) == 0 &&
                dts->cpu_count < CFGCHK_MAX_CPUS) {
                dts->cpus[dts->cpu_count++] = cpu;
            }
            in_cpu = false;
            continue;
        }
        if (strncmp(trim, "memory@", 7) == 0) {
            in_memory = true;
            continue;
        }
        if (in_memory && strstr(trim, "reg =") != NULL) {
            unsigned long long base, size;
            if (parse_reg_cells(trim, &base, &size) == 0 &&
                dts->mem_count < CFGCHK_MAX_MEM) {
                struct cfgchk_mem_region *mr =
                    &dts->mem_regions[dts->mem_count++];
                mr->start = base;
                mr->size = size;
                mr->type = CFGCHK_MEM_RAM;
                mr->flags = 0;
            }
            in_memory = false;
            continue;
        }
        if (strncmp(trim, "virtio_mmio@", 12) == 0) {
            if (dts->virtio_count < CFGCHK_MAX_VIRTIO) {
                current_virtio = dts->virtio_count++;
                memset(&dts->virtio[current_virtio], 0,
                       sizeof(struct cfgchk_virtio_desc));
            } else {
                current_virtio = -1;
            }
            continue;
        }
        if (current_virtio >= 0 && strstr(trim, "reg =") != NULL) {
            unsigned long long base, size;
            if (parse_reg_cells(trim, &base, &size) == 0) {
                dts->virtio[current_virtio].base = base;
                dts->virtio[current_virtio].size = size;
            }
            continue;
        }
        if (current_virtio >= 0 && strstr(trim, "interrupts =") != NULL) {
            uint32_t irq;
            if (parse_interrupt_cells(trim, &irq) == 0)
                dts->virtio[current_virtio].irq = irq;
            current_virtio = -1;
            continue;
        }
        if (strstr(trim, "};"))
            current_virtio = -1;
    }

    ret = 0;
    fclose(fp);
    return ret;
}

static int parse_root_dts(const char *path, struct cfgchk_dts_summary *dts,
                          struct cfgchk_board_info *board) {
    FILE *fp = fopen(path, "r");
    char line[512];
    bool in_reserved = false;
    int depth = 0;
    int ret = -1;

    if (!fp) {
        log_error("Failed to open %s (%s)", path, strerror(errno));
        return -1;
    }
    memset(dts, 0, sizeof(*dts));

    while (fgets(line, sizeof(line), fp)) {
        char *trim = trim_whitespace(line);
        if (!in_reserved && strstr(trim, "reserved-memory") != NULL) {
            in_reserved = true;
            depth = strchr(trim, '{') ? 1 : 0;
            continue;
        }
        if (in_reserved) {
            if (strchr(trim, '{'))
                depth++;
            if (strstr(trim, "reg =") != NULL) {
                unsigned long long base, size;
                if (parse_reg_cells(trim, &base, &size) == 0) {
                    if (board->reserved_count < CFGCHK_MAX_RESERVED) {
                        struct cfgchk_reserved_range *r =
                            &board->reserved_mem[board->reserved_count++];
                        r->start = base;
                        r->size = size;
                    }
                    if (dts->mem_count < CFGCHK_MAX_MEM) {
                        struct cfgchk_mem_region *mr =
                            &dts->mem_regions[dts->mem_count++];
                        mr->start = base;
                        mr->size = size;
                        mr->type = CFGCHK_MEM_RAM;
                        mr->flags = 0;
                    }
                }
            }
            if (strchr(trim, '}')) {
                depth--;
                if (depth <= 0) {
                    in_reserved = false;
                    depth = 0;
                }
            }
        }
    }
    fclose(fp);
    ret = 0;
    return ret;
}

static void apply_reservation_flags(const struct cfgchk_board_info *board,
                                    struct cfgchk_zone_summary *zone) {
    if (!board->reserved_count)
        return;
    for (uint32_t i = 0; i < zone->mem_count; ++i) {
        struct cfgchk_mem_region *mem = &zone->mem_regions[i];
        if (mem->type != CFGCHK_MEM_RAM)
            continue;
        for (uint32_t j = 0; j < board->reserved_count; ++j) {
            const struct cfgchk_reserved_range *r = &board->reserved_mem[j];
            unsigned long long res_end;
            if (__builtin_add_overflow(r->start, r->size, &res_end))
                continue;
            if (range_within(mem->start, mem->size, r->start, res_end)) {
                mem->flags |= CFGCHK_MEM_F_REQUIRES_RESERVATION;
                break;
            }
        }
    }
}

static int build_cfg_request(const char *target_json,
                             const struct cfgchk_board_info *board,
                             struct cfgchk_zone_summary *target_zone,
                             const struct cfgchk_dts_summary *dts_zone,
                             const struct cfgchk_dts_summary *dts_root,
                             struct cfgchk_request *req) {
    DIR *dir = NULL;
    struct dirent *ent;
    char dir_path[PATH_MAX];
    char other_path[PATH_MAX];
    char real_target[PATH_MAX];

    memset(req, 0, sizeof(*req));
    req->version = CFGCHK_IOCTL_VERSION;
    req->board = *board;
    req->dts_zone = *dts_zone;
    req->dts_root = *dts_root;
    req->zones[0] = *target_zone;
    req->zone_count = 1;
    req->target_index = 0;

    if (ensure_realpath(target_json, real_target, sizeof(real_target)) != 0)
        return -1;
    if (get_dirname(target_json, dir_path, sizeof(dir_path)) != 0)
        return -1;
    dir = opendir(dir_path);
    if (!dir) {
        log_warn("Failed to open directory %s (%s)", dir_path, strerror(errno));
        return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        if (!has_suffix(ent->d_name, CFGCHK_JSON_SUFFIX))
            continue;
        if (join_path(dir_path, ent->d_name, other_path, sizeof(other_path)) !=
            0)
            continue;
        char real_other[PATH_MAX];
        if (ensure_realpath(other_path, real_other, sizeof(real_other)) != 0)
            continue;
        if (strcmp(real_other, real_target) == 0)
            continue;
        if (req->zone_count >= CFGCHK_MAX_ZONES) {
            log_warn("zone list exceeds maximum %u", CFGCHK_MAX_ZONES);
            break;
        }
        struct cfgchk_zone_summary zone;
        if (parse_zone_json(real_other, &zone, NULL) != 0) {
            log_warn("Skip invalid zone JSON %s", real_other);
            continue;
        }
        apply_reservation_flags(board, &zone);
        req->zones[req->zone_count++] = zone;
    }
    closedir(dir);
    return 0;
}

static int locate_board_file(const char *json_dir, char *out, size_t len) {
    char current[PATH_MAX];
    if (snprintf(current, sizeof(current), "%s", json_dir) >=
        (int)sizeof(current))
        return -1;
    while (true) {
        char candidate[PATH_MAX];
        if (join_path(current, "board.rs", candidate, sizeof(candidate)) == 0 &&
            access(candidate, R_OK) == 0) {
            return ensure_realpath(candidate, out, len);
        }
        if (strcmp(current, "/") == 0)
            break;
        if (get_dirname(current, current, sizeof(current)) != 0)
            break;
    }
    return -1;
}

static int locate_root_dts(const char *platform_dir, char *out, size_t len) {
    const char *candidates[] = {"zone0.dts", "image/zone0.dts",
                                "dts/zone0.dts"};
    for (size_t i = 0; i < ARRAY_SIZE(candidates); ++i) {
        char path[PATH_MAX];
        if (join_path(platform_dir, candidates[i], path, sizeof(path)) != 0)
            continue;
        if (access(path, R_OK) == 0)
            return ensure_realpath(path, out, len);
    }
    log_error("Unable to locate zone0.dts under %s", platform_dir);
    return -1;
}

static int search_dts_with_hint(const char *dir, const char *hint, char *out,
                                size_t len) {
    DIR *dp = opendir(dir);
    struct dirent *ent;
    if (!dp)
        return -1;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_type == DT_DIR)
            continue;
        if (!has_suffix(ent->d_name, CFGCHK_DTS_SUFFIX))
            continue;
        if (hint && strstr(ent->d_name, hint) == NULL)
            continue;
        char path[PATH_MAX];
        if (join_path(dir, ent->d_name, path, sizeof(path)) == 0 &&
            access(path, R_OK) == 0) {
            closedir(dp);
            return ensure_realpath(path, out, len);
        }
    }
    closedir(dp);
    return -1;
}

static int locate_zone_dts(const char *platform_dir, const char *json_dir,
                           const char *json_base, const char *zone_name,
                           uint32_t zone_id, char *out, size_t len) {
    char candidate[PATH_MAX];
    const char *image_dir_candidates[] = {"image", "dts", "."};
    for (size_t i = 0; i < ARRAY_SIZE(image_dir_candidates); ++i) {
        char base_dir[PATH_MAX];
        if (strcmp(image_dir_candidates[i], ".") == 0) {
            snprintf(base_dir, sizeof(base_dir), "%s", json_dir);
        } else if (join_path(platform_dir, image_dir_candidates[i], base_dir,
                             sizeof(base_dir)) != 0) {
            continue;
        }
        if (join_path(base_dir, json_base, candidate, sizeof(candidate)) == 0) {
            size_t len_without_ext = strlen(candidate);
            if (len_without_ext >= sizeof(candidate) - 5)
                continue;
            strcpy(candidate + len_without_ext, ".dts");
            if (access(candidate, R_OK) == 0)
                return ensure_realpath(candidate, out, len);
        }
        if (zone_name && zone_name[0]) {
            if (join_path(base_dir, zone_name, candidate, sizeof(candidate)) ==
                0) {
                size_t l = strlen(candidate);
                if (l < sizeof(candidate) - 5) {
                    strcpy(candidate + l, ".dts");
                    if (access(candidate, R_OK) == 0)
                        return ensure_realpath(candidate, out, len);
                }
            }
        }
        if (snprintf(candidate, sizeof(candidate), "zone%u.dts", zone_id) >=
            (int)sizeof(candidate))
            continue;
        char tmp[PATH_MAX];
        if (join_path(base_dir, candidate, tmp, sizeof(tmp)) == 0 &&
            access(tmp, R_OK) == 0)
            return ensure_realpath(tmp, out, len);
    }

    if (zone_name && zone_name[0]) {
        if (search_dts_with_hint(platform_dir, zone_name, out, len) == 0)
            return 0;
    }
    char id_hint[32];
    snprintf(id_hint, sizeof(id_hint), "zone%u", zone_id);
    if (search_dts_with_hint(platform_dir, id_hint, out, len) == 0)
        return 0;

    log_error("Unable to locate DTS for zone %u", zone_id);
    return -1;
}

int zone_validate_command(int argc, char **argv) {
    struct cfgchk_board_info board;
    struct cfgchk_zone_summary target_zone;
    struct cfgchk_dts_summary dts_zone;
    struct cfgchk_dts_summary dts_root;
    struct cfgchk_request req;
    struct zone_parse_meta meta;
    char abs_json[PATH_MAX];
    char json_dir[PATH_MAX];
    char json_base[PATH_MAX];
    char board_path[PATH_MAX];
    char platform_dir[PATH_MAX];
    char zone_dts_path[PATH_MAX];
    char root_dts_path[PATH_MAX];
    int fd, ret;

    if (argc < 1) {
        log_error("zone validate requires <config.json>");
        return -1;
    }
    if (!realpath(argv[0], abs_json)) {
        log_error("Failed to resolve %s (%s)", argv[0], strerror(errno));
        return -1;
    }
    if (get_dirname(abs_json, json_dir, sizeof(json_dir)) != 0 ||
        get_basename(abs_json, json_base, sizeof(json_base)) != 0) {
        log_error("Failed to derive config path for %s", abs_json);
        return -1;
    }
    char *dot = strrchr(json_base, '.');
    if (dot)
        *dot = '\0';

    if (locate_board_file(json_dir, board_path, sizeof(board_path)) != 0) {
        log_error("Unable to locate board.rs for %s", abs_json);
        return -1;
    }
    if (get_dirname(board_path, platform_dir, sizeof(platform_dir)) != 0) {
        log_error("Failed to derive platform directory from %s", board_path);
        return -1;
    }

    if (parse_board_file(board_path, &board) != 0)
        return -1;
    if (parse_zone_json(abs_json, &target_zone, &meta) != 0)
        return -1;
    if (locate_zone_dts(platform_dir, json_dir, json_base, meta.name,
                        target_zone.zone_id, zone_dts_path,
                        sizeof(zone_dts_path)) != 0)
        return -1;
    if (locate_root_dts(platform_dir, root_dts_path, sizeof(root_dts_path)) !=
        0)
        return -1;
    if (parse_zone_dts(zone_dts_path, &dts_zone, &board) != 0)
        return -1;
    if (parse_root_dts(root_dts_path, &dts_root, &board) != 0)
        return -1;

    apply_reservation_flags(&board, &target_zone);
    if (build_cfg_request(abs_json, &board, &target_zone, &dts_zone, &dts_root,
                          &req) != 0)
        return -1;

    fd = open("/dev/hvisor_cfgchk", O_RDWR);
    if (fd < 0) {
        log_error("Failed to open /dev/hvisor_cfgchk (%s)", strerror(errno));
        return -1;
    }
    ret = ioctl(fd, HVISOR_CFG_VALIDATE, &req);
    close(fd);
    if (ret != 0) {
        log_error("Kernel validation interface returned error (errno=%d: %s)",
                  errno, strerror(errno));
        return -1;
    }

    printf("[OK] cfgchk validation success.\n");
    return 0;
}
