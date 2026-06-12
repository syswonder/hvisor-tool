// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 *      https://www.syswonder.org
 *
 * Authors:
 *      Guowei Li <2401213322@stu.pku.edu.cn>
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "event_monitor.h"
#include "hvisor.h"
#include "json_parse.h"
#include "log.h"
#include "safe_cjson.h"
#include "virtio.h"
#include "zone_config.h"

// Multiboot2 constants
#define MULTIBOOT2_MAGIC 0x36D76289
#define MULTIBOOT_TAG_TYPE_END 0
#define MULTIBOOT_TAG_TYPE_CMDLINE 1
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME 2
#define MULTIBOOT_TAG_TYPE_MODULE 3
#define MULTIBOOT_TAG_TYPE_BASIC_MEMINFO 4
#define MULTIBOOT_TAG_TYPE_MEMORY_MAP 6
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER 8

#define MULTIBOOT_FLAG_MEMORY (1 << 0)
#define MULTIBOOT_FLAG_BOOTDEV (1 << 2)
#define MULTIBOOT_FLAG_CMDLINE (1 << 3)
#define MULTIBOOT_FLAG_MMAP (1 << 6)

static void __attribute__((noreturn)) help(int exit_status) {
    printf("Hypervisor Management Tool\n\n");
    printf("Usage:\n");
    printf("  hvisor <command> [options]\n\n");
    printf("Commands:\n");
    printf("  zone start    <config.json>    Initialize an isolation zone\n");
    printf("  zone shutdown -id <zone_id>   Terminate a zone by ID\n");
    printf("  zone list                      List all active zones\n");
    printf("  virtio start  <virtio.json>    Activate virtio devices\n\n");
    printf("Options:\n");
    printf("  --id <zone_id>    Specify zone ID for shutdown\n");
    printf("  --help            Show this help message\n\n");
    printf("Examples:\n");
    printf("  Start zone:    hvisor zone start /path/to/vm.json\n");
    printf("  Shutdown zone: hvisor zone shutdown -id 1\n");
    printf("  List zones:    hvisor zone list\n");
    exit(exit_status);
}

void *read_file(const char *filename, uint64_t *filesize) {
    int fd;
    struct stat st;
    void *buf;
    ssize_t len;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        log_error("read_file: open file %s failed", filename);
        exit(1);
    }

    if (fstat(fd, &st) < 0) {
        log_error("read_file: fstat %s failed", filename);
        exit(1);
    }

    long page_size = sysconf(_SC_PAGESIZE);

    // Calculate buffer size, ensuring alignment to page boundary
    ssize_t buf_size = (st.st_size + page_size - 1) & ~(page_size - 1);

    buf = malloc(buf_size);
    memset(buf, 0, buf_size);

    len = read(fd, buf, st.st_size);
    if (len < 0) {
        perror("read_file: read failed");
        exit(1);
    }

    if (filesize)
        *filesize = len;

    close(fd);

    return buf;
}

int open_dev() {
    int fd = open("/dev/hvisor", O_RDWR);
    if (fd < 0) {
        log_error("Failed to open /dev/hvisor!");
        exit(1);
    }
    return fd;
}

static __u64 load_buffer_to_memory(const void *buf, __u64 size,
                                   __u64 load_paddr) {
    int fd;
    long page_size;
    __u64 map_size;
    struct hvisor_load_image_args args;

    if (size == 0) {
        return 0;
    }

    page_size = sysconf(_SC_PAGESIZE);
    map_size = (size + page_size - 1) & ~((__u64)page_size - 1);

    fd = open_dev();
    args.user_buffer = (__u64)(uintptr_t)buf;
    args.size = size;
    args.load_paddr = load_paddr;
    if (ioctl(fd, HVISOR_LOAD_IMAGE, &args) != 0) {
        perror("load_buffer_to_memory: HVISOR_LOAD_IMAGE failed");
        close(fd);
        exit(1);
    }
    close(fd);

    return map_size;
}

static __u64 load_str_to_memory(const char *str, __u64 load_paddr) {
    /* Include trailing '\0' so guest side can safely parse cmdline. */
    __u64 size = strlen(str) + 1;
    return load_buffer_to_memory(str, size, load_paddr);
}

static __u64 load_image_to_memory(const char *path, __u64 load_paddr) {
    if (strcmp(path, "null") == 0) {
        return 0;
    }
    __u64 size;
    __u64 map_size;
    void *image_content;

    log_info("[MULTIBOOT] Loading image from: %s to paddr: 0x%llx", path,
             load_paddr);

    image_content = read_file(path, (uint64_t *)&size);
    map_size = load_buffer_to_memory(image_content, size, load_paddr);

    log_info("[MULTIBOOT] Image loaded, size: %llu, mapped: %llu", size,
             map_size);

    free(image_content);
    return map_size;
}

/**
 * @brief Build Multiboot2 info structure
 *
 * This function builds a basic Multiboot2 info structure in memory
 * that Asterinas can use to boot.
 *
 * Multiboot2 structure:
 * - offset 0: total_size (uint32_t) - total size of info structure
 * - offset 4: reserved (uint32_t) - must be 0
 * - offset 8: tags...
 * - last: end tag (type=0, size=8)
 *
 * @param total_mem_size Total memory size in bytes
 * @param cmdline Kernel command line string
 * @return Pointer to allocated Multiboot info, or NULL on error
 */
// Multiboot2 memory map entry types
#define MULTIBOOT_MEMORY_AVAILABLE 1
#define MULTIBOOT_MEMORY_RESERVED 2

// Multiboot2 memory map entry
struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

// Multiboot2 memory map tag header
struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[];
};

static void *build_multiboot2_info(uint64_t total_mem_size, const char *cmdline,
                                   memory_region_t *mem_regions,
                                   int num_regions, uint64_t initramfs_gpa,
                                   uint64_t initramfs_size,
                                   const char *module_cmdline) {
    int has_initramfs = (initramfs_size > 0);

    // Calculate needed size for memory map tag
    int num_ram_regions = 0;
    for (int i = 0; i < num_regions; i++) {
        if (mem_regions[i].type == MEM_TYPE_RAM) {
            num_ram_regions++;
        }
    }

    size_t mmap_tag_size =
        sizeof(struct multiboot_tag_mmap) +
        num_ram_regions * sizeof(struct multiboot_mmap_entry);

    // Calculate module tag size (if initramfs is provided)
    size_t module_tag_size = 0;
    if (has_initramfs) {
        size_t cmdline_len = module_cmdline ? strlen(module_cmdline) + 1 : 1;
        module_tag_size =
            8 + 8 +
            ((cmdline_len + 7) &
             ~7); // header(8) + mod_start/mod_end(8) + padded cmdline
    }

    // Allocate buffer for Multiboot info
    size_t info_size = 4096 + mmap_tag_size + module_tag_size;
    void *info = malloc(info_size);

    if (!info) {
        log_error("[MULTIBOOT] Failed to allocate memory for Multiboot info");
        return NULL;
    }

    memset(info, 0, info_size);
    log_info("[MULTIBOOT] Allocated Multiboot info buffer at %p", info);

    // Build tags - start at offset 8 (after total_size and reserved)
    char *tag_ptr = (char *)info + 8;

    // Memory info tag (required by Asterinas)
    {
        uint32_t *tag_type = (uint32_t *)tag_ptr;
        uint32_t *tag_size_field = (uint32_t *)(tag_ptr + 4);
        uint32_t *mem_lower = (uint32_t *)(tag_ptr + 8);
        uint32_t *mem_upper = (uint32_t *)(tag_ptr + 12);

        *tag_type = MULTIBOOT_TAG_TYPE_BASIC_MEMINFO;
        *tag_size_field = 16;
        *mem_lower = 640; // 640KB low memory
        *mem_upper = (total_mem_size > 0x100000)
                         ? (total_mem_size / 1024 - 1024)
                         : 0; // KB above 1MB

        log_info(
            "[MULTIBOOT] Added basic meminfo tag: lower=%u KB, upper=%u KB",
            *mem_lower, *mem_upper);
        tag_ptr += 16;
    }

    // Memory map tag (CRITICAL for Asterinas memory allocator)
    // For x86_64, we need to split memory to leave MMIO space below 4GB:
    // - Low memory: 0x0 - 0xC0000000 (3GB)
    // - MMIO space: 0xC0000000 - 0x100000000 (1GB, reserved for devices)
    // - High memory: 0x100000000+ (4GB+)
    {
        struct multiboot_tag_mmap *mmap_tag =
            (struct multiboot_tag_mmap *)tag_ptr;
        mmap_tag->type = MULTIBOOT_TAG_TYPE_MEMORY_MAP;
        mmap_tag->entry_size = sizeof(struct multiboot_mmap_entry);
        mmap_tag->entry_version = 0;

        int entry_idx = 0;
        const uint64_t LOW_MMIO_TOP = 0x100000000ULL; // 4GB
        const uint64_t LOW_MEM_TOP = 0xC0000000ULL; // 3GB - leave 1GB for MMIO

        for (int i = 0; i < num_regions; i++) {
            if (mem_regions[i].type == MEM_TYPE_RAM) {
                uint64_t region_start = mem_regions[i].virtual_start;
                uint64_t region_size = mem_regions[i].size;
                uint64_t region_end = region_start + region_size;

                // Split into low and high memory regions
                if (region_start < LOW_MEM_TOP) {
                    // Low memory region (below 3GB)
                    uint64_t low_start = region_start;
                    uint64_t low_end =
                        (region_end < LOW_MEM_TOP) ? region_end : LOW_MEM_TOP;
                    uint64_t low_size = low_end - low_start;

                    if (low_size > 0) {
                        mmap_tag->entries[entry_idx].addr = low_start;
                        mmap_tag->entries[entry_idx].len = low_size;
                        mmap_tag->entries[entry_idx].type =
                            MULTIBOOT_MEMORY_AVAILABLE;
                        mmap_tag->entries[entry_idx].zero = 0;
                        log_info("[MULTIBOOT] Memory map entry %d: addr=0x%llx "
                                 "(GPA), len=0x%llx, type=AVAILABLE (low)",
                                 entry_idx, low_start, low_size);
                        entry_idx++;
                    }
                }

                // High memory region (above 4GB)
                if (region_end > LOW_MMIO_TOP) {
                    uint64_t high_start = (region_start > LOW_MMIO_TOP)
                                              ? region_start
                                              : LOW_MMIO_TOP;
                    uint64_t high_size = region_end - high_start;

                    if (high_size > 0) {
                        mmap_tag->entries[entry_idx].addr = high_start;
                        mmap_tag->entries[entry_idx].len = high_size;
                        mmap_tag->entries[entry_idx].type =
                            MULTIBOOT_MEMORY_AVAILABLE;
                        mmap_tag->entries[entry_idx].zero = 0;
                        log_info("[MULTIBOOT] Memory map entry %d: addr=0x%llx "
                                 "(GPA), len=0x%llx, type=AVAILABLE (high)",
                                 entry_idx, high_start, high_size);
                        entry_idx++;
                    }
                }
            }
        }

        mmap_tag->size = sizeof(struct multiboot_tag_mmap) +
                         entry_idx * sizeof(struct multiboot_mmap_entry);

        log_info("[MULTIBOOT] Added memory map tag: %d entries, size=%u bytes",
                 entry_idx, mmap_tag->size);

        // Align to 8 bytes
        size_t aligned_size = (mmap_tag->size + 7) & ~7;
        tag_ptr += aligned_size;
    }

    // Command line tag
    if (cmdline && strlen(cmdline) > 0) {
        size_t cmdline_len = strlen(cmdline) + 1;
        size_t tag_size = 8 + ((cmdline_len + 7) & ~7); // Align to 8 bytes

        uint32_t *tag_type = (uint32_t *)tag_ptr;
        uint32_t *tag_size_field = (uint32_t *)(tag_ptr + 4);
        char *tag_data = tag_ptr + 8;

        *tag_type = MULTIBOOT_TAG_TYPE_CMDLINE;
        *tag_size_field = tag_size;
        strncpy(tag_data, cmdline, cmdline_len);

        log_info("[MULTIBOOT] Added cmdline tag: '%s' (size: %zu)", cmdline,
                 tag_size);

        tag_ptr += tag_size;
    }

    // Module tag (initramfs) - must come before End tag
    if (has_initramfs) {
        const char *mod_cmd = module_cmdline ? module_cmdline : "";
        size_t mod_cmd_len = strlen(mod_cmd) + 1;
        size_t mod_tag_size = 8 + 8 + ((mod_cmd_len + 7) & ~7);

        uint32_t *mod_type = (uint32_t *)tag_ptr;
        uint32_t *mod_size = (uint32_t *)(tag_ptr + 4);
        uint32_t *mod_start = (uint32_t *)(tag_ptr + 8);
        uint32_t *mod_end = (uint32_t *)(tag_ptr + 12);

        *mod_type = MULTIBOOT_TAG_TYPE_MODULE;
        *mod_size = mod_tag_size;
        *mod_start = (uint32_t)(initramfs_gpa & 0xFFFFFFFF);
        *mod_end = (uint32_t)((initramfs_gpa + initramfs_size) & 0xFFFFFFFF);

        // Copy cmdline after the fixed fields
        char *mod_cmd_dst = tag_ptr + 16;
        memcpy(mod_cmd_dst, mod_cmd, mod_cmd_len);

        log_info("[MULTIBOOT] Added initramfs module tag: start=0x%llx, "
                 "end=0x%llx, cmd='%s'",
                 (unsigned long long)initramfs_gpa,
                 (unsigned long long)(initramfs_gpa + initramfs_size), mod_cmd);

        tag_ptr += mod_tag_size;
    }

    // End tag
    uint32_t *end_type = (uint32_t *)tag_ptr;
    uint32_t *end_size = (uint32_t *)(tag_ptr + 4);
    *end_type = MULTIBOOT_TAG_TYPE_END;
    *end_size = 8;
    tag_ptr += 8;

    // Now set the total_size at the beginning
    uint32_t total_size = (uint32_t)(tag_ptr - (char *)info);
    uint32_t *total_size_ptr = (uint32_t *)info;
    *total_size_ptr = total_size;

    // Reserved field is already 0 from memset

    log_info("[MULTIBOOT] Multiboot2 info built: total_size=%u bytes",
             total_size);

    return info;
}

/**
 * @brief Find ELF entry point from ELF file
 *
 * @param elf_path Path to ELF file
 * @param entry_point Output: entry point address
 * @return 0 on success, -1 on error
 */
static int find_elf_entry_point(const char *elf_path, uint64_t *entry_point) {
    uint64_t size;
    void *elf = read_file(elf_path, &size);

    if (!elf) {
        log_error("[MULTIBOOT] Failed to read ELF file: %s", elf_path);
        return -1;
    }

    log_info("[MULTIBOOT] Reading ELF file: %s, size: %llu", elf_path, size);

    // Check ELF magic
    unsigned char *e_ident = (unsigned char *)elf;
    if (e_ident[0] != 0x7f || e_ident[1] != 'E' || e_ident[2] != 'L' ||
        e_ident[3] != 'F') {
        log_error("[MULTIBOOT] Not a valid ELF file: %s", elf_path);
        free(elf);
        return -1;
    }

    log_info("[MULTIBOOT] ELF file is valid");

    // Check ELF class (64-bit)
    if (e_ident[4] != 2) {
        log_error("[MULTIBOOT] Not a 64-bit ELF file");
        free(elf);
        return -1;
    }

    // Get entry point from ELF header
    // ELF header entry point is at offset 24 (8 bytes)
    uint64_t *entry_ptr = (uint64_t *)((char *)elf + 24);
    *entry_point = *entry_ptr;

    log_info("[MULTIBOOT] ELF entry point: 0x%llx", *entry_point);

    free(elf);
    return 0;
}

/**
 * @brief Load ELF segments to correct physical addresses with GPA-to-HPA
 * translation
 *
 * This function parses the ELF file and loads each PT_LOAD segment
 * to its specified physical address (p_paddr) plus an offset.
 *
 * @param elf_path Path to ELF file
 * @param total_size Output: total size of loaded segments (max p_paddr +
 * p_memsz)
 * @param gpa_to_hpa_offset Offset to add to p_paddr for GPA-to-HPA translation
 * @return 0 on success, -1 on error
 */
static int load_elf_segments(const char *elf_path, uint64_t *total_size,
                             int64_t gpa_to_hpa_offset) {
    fprintf(stderr,
            "[ELF] load_elf_segments called with path: %s, "
            "gpa_to_hpa_offset=0x%llx\n",
            elf_path, (unsigned long long)gpa_to_hpa_offset);
    fflush(stderr);

    uint64_t file_size;
    void *elf = read_file(elf_path, &file_size);

    fprintf(stderr, "[ELF] read_file returned, file_size=%llu\n", file_size);
    fflush(stderr);

    if (!elf) {
        fprintf(stderr, "[ELF] Failed to read ELF file: %s\n", elf_path);
        return -1;
    }

    fprintf(stderr, "[ELF] Loading ELF segments from: %s, file size: %llu\n",
            elf_path, file_size);

    // Check ELF magic
    unsigned char *e_ident = (unsigned char *)elf;
    if (e_ident[0] != 0x7f || e_ident[1] != 'E' || e_ident[2] != 'L' ||
        e_ident[3] != 'F') {
        fprintf(stderr, "[ELF] Not a valid ELF file\n");
        free(elf);
        return -1;
    }

    // Check 64-bit
    if (e_ident[4] != 2) {
        fprintf(stderr, "[ELF] Not a 64-bit ELF file\n");
        free(elf);
        return -1;
    }

    // Parse ELF64 header
    // e_phoff: program header table file offset (offset 32, 8 bytes)
    // e_phentsize: program header entry size (offset 54, 2 bytes)
    // e_phnum: number of program headers (offset 56, 2 bytes)
    uint64_t e_phoff = *(uint64_t *)((char *)elf + 32);
    uint16_t e_phentsize = *(uint16_t *)((char *)elf + 54);
    uint16_t e_phnum = *(uint16_t *)((char *)elf + 56);

    fprintf(stderr,
            "[ELF] Program headers: offset=0x%llx, entry_size=%u, count=%u\n",
            e_phoff, e_phentsize, e_phnum);

    *total_size = 0;

    // Process each program header
    for (int i = 0; i < e_phnum; i++) {
        char *phdr = (char *)elf + e_phoff + i * e_phentsize;

        // Parse program header (ELF64)
        // p_type: offset 0, 4 bytes (PT_LOAD = 1)
        // p_flags: offset 4, 4 bytes
        // p_offset: offset 8, 8 bytes
        // p_vaddr: offset 16, 8 bytes
        // p_paddr: offset 24, 8 bytes
        // p_filesz: offset 32, 8 bytes
        // p_memsz: offset 40, 8 bytes
        uint32_t p_type = *(uint32_t *)(phdr + 0);
        uint64_t p_offset =
            *(uint64_t *)((char *)elf + e_phoff + i * e_phentsize + 8);
        uint64_t p_paddr =
            *(uint64_t *)((char *)elf + e_phoff + i * e_phentsize + 24);
        uint64_t p_filesz =
            *(uint64_t *)((char *)elf + e_phoff + i * e_phentsize + 32);
        uint64_t p_memsz =
            *(uint64_t *)((char *)elf + e_phoff + i * e_phentsize + 40);

        fprintf(stderr,
                "[ELF] Segment %d: type=%u, p_offset=0x%llx, "
                "p_paddr(GPA)=0x%llx, p_filesz=0x%llx\n",
                i, p_type, p_offset, p_paddr, p_filesz);

        // Only process PT_LOAD segments
        if (p_type != 1) { // PT_LOAD
            fprintf(stderr, "[ELF] Segment %d: skipping (not PT_LOAD)\n", i);
            continue;
        }

        // Check bounds
        if (p_offset + p_filesz > file_size) {
            fprintf(stderr,
                    "[ELF] Segment %d file offset+size exceeds file size\n", i);
            free(elf);
            return -1;
        }

        // Calculate actual load address (HPA) by applying GPA-to-HPA offset
        uint64_t load_paddr = (uint64_t)((int64_t)p_paddr + gpa_to_hpa_offset);

        // Load segment content to physical memory
        if (p_filesz > 0) {
            fprintf(stderr,
                    "[ELF] Loading segment %d to HPA 0x%llx (GPA 0x%llx), size "
                    "0x%llx\n",
                    i, load_paddr, p_paddr, p_filesz);
            // load_buffer_to_memory exits on failure, so we don't need to check
            // return value
            load_buffer_to_memory((char *)elf + p_offset, p_filesz, load_paddr);
            fprintf(stderr, "[ELF] Segment %d loaded successfully\n", i);
        }

        // Handle p_memsz > p_filesz: zero-fill the remaining memory
        // This is crucial for BSS sections and stack areas
        if (p_memsz > p_filesz) {
            uint64_t zero_fill_start = load_paddr + p_filesz;
            uint64_t zero_fill_size = p_memsz - p_filesz;
            fprintf(stderr,
                    "[ELF] Zero-filling segment %d: HPA 0x%llx, size 0x%llx "
                    "(BSS/stack)\n",
                    i, zero_fill_start, zero_fill_size);
            // Allocate a zero buffer and load it
            void *zero_buf = calloc(1, zero_fill_size);
            if (!zero_buf) {
                fprintf(stderr,
                        "[ELF] Failed to allocate zero buffer for segment %d\n",
                        i);
                free(elf);
                return -1;
            }
            load_buffer_to_memory(zero_buf, zero_fill_size, zero_fill_start);
            free(zero_buf);
            fprintf(stderr, "[ELF] Segment %d zero-fill completed\n", i);
        }

        // Track total size (for information)
        if (p_paddr + p_memsz > *total_size) {
            *total_size = p_paddr + p_memsz;
        }
    }

    free(elf);
    fprintf(stderr, "[ELF] All segments loaded, total GPA range: 0x%llx\n",
            *total_size);
    return 0;
}

/**
 * @brief Parse modules configuration from a JSON array.
 *
 * This function parses modules configuration from a JSON array. Each module
 * configuration item should contain string fields: `name`, `filepath`, and
 * `load_paddr`.
 *
 * @param modules_json The JSON array containing modules configuration.
 * @return 0 on success, -1 on error.
 */
static int parse_modules(const cJSON *const modules_json) {
    // if not configured, just info it and return success
    if (modules_json == NULL || !cJSON_IsArray(modules_json)) {
        log_info("No additional modules configured, skip loading.");
        return 0;
    }

    // info total number of modules
    size_t num_modules = SAFE_CJSON_GET_ARRAY_SIZE(modules_json);
    log_info("Found %zu module configurations in JSON", num_modules);

    // parse each module configuration item, and load them to memory
    for (size_t i = 0; i < num_modules; i++) {
        const cJSON *const module_json =
            SAFE_CJSON_GET_ARRAY_ITEM(modules_json, i);
        const cJSON *const name_json =
            SAFE_CJSON_GET_OBJECT_ITEM(module_json, "name");
        const cJSON *const filepath_json =
            SAFE_CJSON_GET_OBJECT_ITEM(module_json, "filepath");
        const cJSON *const load_paddr_json =
            SAFE_CJSON_GET_OBJECT_ITEM(module_json, "load_paddr");

        // name, filepath, load_paddr are required
        if (name_json == NULL || !cJSON_IsString(name_json) ||
            filepath_json == NULL || !cJSON_IsString(filepath_json) ||
            load_paddr_json == NULL || !cJSON_IsString(load_paddr_json)) {
            log_error(
                "Missing required string field (name, filepath, load_paddr) "
                "in module configuration at index %zu",
                i);
            return -1;
        }

        // check file accessibility
        if (access(filepath_json->valuestring, R_OK) != 0) {
            log_error("Cannot access module file: %s - %s",
                      filepath_json->valuestring, strerror(errno));
            return -1;
        }

        // load module image to memory
        uintptr_t item_load_paddr;
        if (parse_json_address(load_paddr_json, &item_load_paddr) != 0) {
            log_error("Failed to parse module load_paddr");
            return -1;
        }

        size_t item_size =
            load_image_to_memory(filepath_json->valuestring, item_load_paddr);

        // record module info
        log_info("Loaded index %zu module '%s' (path: %s) to memory at "
                 "0x%" PRIxPTR ", size: %zu bytes",
                 i, name_json->valuestring, filepath_json->valuestring,
                 item_load_paddr, item_size);

        // warn if loaded size is 0
        if (item_size == 0) {
            log_warn("Module '%s' loaded with size 0, "
                     "please check the file content.",
                     name_json->valuestring);
        }
    }

    // info total number of modules loaded
    log_info("Total %zu modules loaded", num_modules);

    return 0;
}

#define CHECK_JSON_NULL(json_ptr, json_name)                                   \
    if (json_ptr == NULL) {                                                    \
        log_error("\'%s\' is missing in json file.", json_name);               \
        return -1;                                                             \
    }

#define CHECK_JSON_NULL_ERR_OUT(json_ptr, json_name)                           \
    if (json_ptr == NULL) {                                                    \
        log_error("\'%s\' is missing in json file.", json_name);               \
        goto err_out;                                                          \
    }

static int parse_arch_config(cJSON *root, zone_config_t *config,
                              int64_t gpa_to_hpa_offset) {
    cJSON *arch_config_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "arch_config");
    CHECK_JSON_NULL(arch_config_json, "arch_config");

    arch_zone_config_t *arch_config = &config->arch_config;
#ifdef ARM64
    cJSON *gic_version_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gic_version");
    cJSON *gicd_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicd_base");
    cJSON *gicr_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicr_base");
    cJSON *gits_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gits_base");
    cJSON *gicc_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicc_base");
    cJSON *gich_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gich_base");
    cJSON *gicv_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicv_base");
    cJSON *gicc_offset_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicc_offset");
    cJSON *gicv_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicv_size");
    cJSON *gich_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gich_size");
    cJSON *gicc_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicc_size");
    cJSON *gicd_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicd_size");
    cJSON *gicr_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicr_size");
    cJSON *gits_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gits_size");
    cJSON *is_aarch32_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "is_aarch32");
    CHECK_JSON_NULL(gic_version_json, "gic_version");
    char *gic_version = gic_version_json->valuestring;
    if (!strcmp(gic_version, "v2")) {
        CHECK_JSON_NULL(gicd_base_json, "gicd_base")
        CHECK_JSON_NULL(gicd_size_json, "gicd_size")
        CHECK_JSON_NULL(gicc_base_json, "gicc_base")
        CHECK_JSON_NULL(gicc_size_json, "gicc_size")
        CHECK_JSON_NULL(gicc_offset_json, "gicc_offset")
        CHECK_JSON_NULL(gich_base_json, "gich_base")
        CHECK_JSON_NULL(gich_size_json, "gich_size")
        CHECK_JSON_NULL(gicv_base_json, "gicv_base")
        CHECK_JSON_NULL(gicv_size_json, "gicv_size")

        struct Gicv2Payload *gicv2_payload = &arch_config->gic_config.gicv2;
        struct Gicv2Config *gicv2 = &gicv2_payload->gicv2_config;

        gicv2_payload->gic_version_tag = 0;
        if (parse_json_linux_u64(gicd_base_json, &gicv2->gicd_base) != 0 ||
            parse_json_linux_u64(gicd_size_json, &gicv2->gicd_size) != 0 ||
            parse_json_linux_u64(gicc_base_json, &gicv2->gicc_base) != 0 ||
            parse_json_linux_u64(gicc_size_json, &gicv2->gicc_size) != 0 ||
            parse_json_linux_u64(gicc_offset_json, &gicv2->gicc_offset) != 0 ||
            parse_json_linux_u64(gich_base_json, &gicv2->gich_base) != 0 ||
            parse_json_linux_u64(gich_size_json, &gicv2->gich_size) != 0 ||
            parse_json_linux_u64(gicv_base_json, &gicv2->gicv_base) != 0 ||
            parse_json_linux_u64(gicv_size_json, &gicv2->gicv_size) != 0) {
            log_error("Failed to parse gicv2 config\n");
            return -1;
        }
    } else if (!strcmp(gic_version, "v3")) {
        CHECK_JSON_NULL(gicd_base_json, "gicd_base")
        CHECK_JSON_NULL(gicr_base_json, "gicr_base")
        CHECK_JSON_NULL(gicd_size_json, "gicd_size")
        CHECK_JSON_NULL(gicr_size_json, "gicr_size")

        struct Gicv3Payload *gicv3_payload = &arch_config->gic_config.gicv3;
        struct Gicv3Config *gicv3 = &gicv3_payload->gicv3_config;

        gicv3_payload->gic_version_tag = 1;
        if (parse_json_linux_u64(gicd_base_json, &gicv3->gicd_base) != 0 ||
            parse_json_linux_u64(gicd_size_json, &gicv3->gicd_size) != 0 ||
            parse_json_linux_u64(gicr_base_json, &gicv3->gicr_base) != 0 ||
            parse_json_linux_u64(gicr_size_json, &gicv3->gicr_size) != 0) {
            log_error("Failed to parse gicv3 config\n");
            return -1;
        }

        if (gits_base_json == NULL || gits_size_json == NULL) {
            log_warn("No gits fields in arch_config.\n");
        } else {
            if (parse_json_linux_u64(gits_base_json, &gicv3->gits_base) != 0 ||
                parse_json_linux_u64(gits_size_json, &gicv3->gits_size) != 0) {
                log_error("Failed to parse gits config\n");
                return -1;
            }
        }
    } else {
        log_error("Invalid GIC version. It should be either of v2 or v3\n");
        return -1;
    }
    if (is_aarch32_json == NULL) {
        log_warn("No is_aarch32 field in arch_config. If you are booting an "
                 "aarch32 guest, "
                 "please set it to true.\n");
        arch_config->is_aarch32 = 0;
    } else {
        arch_config->is_aarch32 = is_aarch32_json->valueint;
    }
#endif

#ifdef RISCV64
    cJSON *plic_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "plic_base");
    cJSON *plic_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "plic_size");
    cJSON *aplic_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "aplic_base");
    cJSON *aplic_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "aplic_size");

    if (plic_base_json == NULL || plic_size_json == NULL) {
        log_warn("Missing fields in arch_config.");
        return -1;
    }
    if (aplic_base_json == NULL || aplic_size_json == NULL) {
        log_warn("Missing fields in arch_config.");
        return -1;
    }

    if (parse_json_linux_u64(plic_base_json, &arch_config->plic_base) != 0 ||
        parse_json_linux_u64(plic_size_json, &arch_config->plic_size) != 0 ||
        parse_json_linux_u64(aplic_base_json, &arch_config->aplic_base) != 0 ||
        parse_json_linux_u64(aplic_size_json, &arch_config->aplic_size) != 0) {
        log_error("Failed to parse plic/aplic config\n");
        return -1;
    }
#endif

#ifdef X86_64
    cJSON *ioapic_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "ioapic_base");
    cJSON *ioapic_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "ioapic_size");
    cJSON *boot_filepath_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "boot_filepath");
    cJSON *boot_load_paddr_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "boot_load_paddr");
    cJSON *cmdline_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "cmdline");
    cJSON *cmdline_load_hpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "cmdline_load_hpa");
    cJSON *cmdline_load_gpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "cmdline_load_gpa");
    cJSON *kernel_entry_gpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "kernel_entry_gpa");
    cJSON *setup_filepath_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "setup_filepath");
    cJSON *setup_load_hpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "setup_load_hpa");
    cJSON *setup_load_gpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "setup_load_gpa");
    cJSON *initrd_filepath_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "initrd_filepath");
    cJSON *initrd_load_hpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "initrd_load_hpa");
    cJSON *initrd_load_gpa_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "initrd_load_gpa");
    cJSON *rsdp_memory_region_id_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "rsdp_memory_region_id");
    cJSON *acpi_memory_region_id_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "acpi_memory_region_id");
    cJSON *uefi_memory_region_id_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "uefi_memory_region_id");
    cJSON *screen_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "screen_base");

    if (parse_json_linux_u64(ioapic_base_json, &arch_config->ioapic_base) !=
            0 ||
        parse_json_linux_u64(ioapic_size_json, &arch_config->ioapic_size) !=
            0 ||
        (config->arch_config.multiboot_enabled ? 0 :
         parse_json_linux_u64(kernel_entry_gpa_json,
                              &arch_config->kernel_entry_gpa)) != 0) {
        log_error("Failed to parse ioapic or kernel_entry_gpa\n");
        return -1;
    }

    if (boot_filepath_json != NULL) {
        __u64 boot_load_paddr;
        if (parse_json_linux_u64(boot_load_paddr_json, &boot_load_paddr) != 0) {
            log_error("Failed to parse boot_load_paddr\n");
            return -1;
        }
        __u64 boot_load_hpa = (__u64)((int64_t)boot_load_paddr + gpa_to_hpa_offset);
        load_image_to_memory(boot_filepath_json->valuestring, boot_load_hpa);
    }

    if (setup_filepath_json != NULL) {
        __u64 setup_load_hpa;
        if (parse_json_linux_u64(setup_load_gpa_json,
                                 &arch_config->setup_load_gpa) != 0 ||
            parse_json_linux_u64(setup_load_hpa_json, &setup_load_hpa) != 0) {
            log_error("Failed to parse setup_load_gpa or setup_load_hpa\n");
            return -1;
        }
        __u64 size = load_image_to_memory(setup_filepath_json->valuestring,
                                          setup_load_hpa);

        log_info("setup size: %llu", size);
    }

    if (cmdline_json != NULL) {
        __u64 cmdline_load_hpa;
        if (parse_json_linux_u64(cmdline_load_gpa_json,
                                 &arch_config->cmdline_load_gpa) != 0 ||
            parse_json_linux_u64(cmdline_load_hpa_json, &cmdline_load_hpa) !=
                0) {
            log_error("Failed to parse cmdline_load_gpa or cmdline_load_hpa\n");
            return -1;
        }
        __u64 size =
            load_str_to_memory(cmdline_json->valuestring, cmdline_load_hpa);

        log_info("cmdline size: %llu", size);
    }

    if (initrd_filepath_json != NULL) {
        __u64 initrd_load_hpa;
        if (parse_json_linux_u64(initrd_load_gpa_json,
                                 &arch_config->initrd_load_gpa) != 0 ||
            parse_json_linux_u64(initrd_load_hpa_json, &initrd_load_hpa) != 0) {
            log_error("Failed to parse initrd_load_gpa or initrd_load_hpa\n");
            return -1;
        }
        arch_config->initrd_size = load_image_to_memory(
            initrd_filepath_json->valuestring, initrd_load_hpa);

        log_info("initrd size: %llu", arch_config->initrd_size);
    }

    if (parse_json_linux_u64(rsdp_memory_region_id_json,
                             &arch_config->rsdp_memory_region_id) != 0 ||
        parse_json_linux_u64(acpi_memory_region_id_json,
                             &arch_config->acpi_memory_region_id) != 0 ||
        parse_json_linux_u64(uefi_memory_region_id_json,
                             &arch_config->uefi_memory_region_id) != 0 ||
        parse_json_linux_u64(screen_base_json, &arch_config->screen_base) !=
            0) {
        log_error("Failed to parse region ids or screen_base\n");
        return -1;
    }
#endif

    return 0;
}

/**
 * @brief Parse PCI configuration from zone JSON.
 *
 * This function requires the top-level `pci_config` section to be present, but
 * allows it to be an empty array when the zone does not expose any PCI buses.
 * Once `pci_config` contains entries, all required fields inside each PCI bus
 * entry must be valid, otherwise the function returns an error.
 *
 * @param root Parsed zone configuration JSON object.
 * @param config Zone configuration output structure to populate.
 *
 * @return 0 on success, -1 if the provided PCI configuration is invalid.
 */
static int parse_pci_config(cJSON *root, zone_config_t *config) {
    cJSON *pci_configs_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "pci_config");
    CHECK_JSON_NULL_ERR_OUT(pci_configs_json, "pci_config")

    int num_pci_bus = SAFE_CJSON_GET_ARRAY_SIZE(pci_configs_json);
    if (num_pci_bus > CONFIG_PCI_BUS_MAXNUM) {
        log_error("Exceeded maximum allowed pci configs.");
        goto err_out;
    }

    config->num_pci_bus = num_pci_bus;
    log_info("num pci bus %d", num_pci_bus);

    for (int i = 0; i < num_pci_bus; i++) {
        cJSON *pci_config_json = SAFE_CJSON_GET_ARRAY_ITEM(pci_configs_json, i);
        pci_config_t *pci_config = &config->pci_config[i];

        cJSON *ecam_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "ecam_base");
        CHECK_JSON_NULL_ERR_OUT(ecam_base_json, "ecam_base")
        cJSON *ecam_size_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "ecam_size");
        CHECK_JSON_NULL_ERR_OUT(ecam_size_json, "ecam_size")
        cJSON *io_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "io_base");
        CHECK_JSON_NULL_ERR_OUT(io_base_json, "io_base")
        cJSON *io_size_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "io_size");
        CHECK_JSON_NULL_ERR_OUT(io_size_json, "io_size")
        cJSON *pci_io_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "pci_io_base");
        CHECK_JSON_NULL_ERR_OUT(pci_io_base_json, "pci_io_base")
        cJSON *mem32_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem32_base");
        CHECK_JSON_NULL_ERR_OUT(mem32_base_json, "mem32_base")
        cJSON *mem32_size_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem32_size");
        CHECK_JSON_NULL_ERR_OUT(mem32_size_json, "mem32_size")
        cJSON *pci_mem32_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "pci_mem32_base");
        CHECK_JSON_NULL_ERR_OUT(pci_mem32_base_json, "pci_mem32_base")
        cJSON *mem64_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem64_base");
        CHECK_JSON_NULL_ERR_OUT(mem64_base_json, "mem64_base")
        cJSON *mem64_size_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem64_size");
        CHECK_JSON_NULL_ERR_OUT(mem64_size_json, "mem64_size")
        cJSON *pci_mem64_base_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "pci_mem64_base");
        CHECK_JSON_NULL_ERR_OUT(pci_mem64_base_json, "pci_mem64_base")
        cJSON *bus_range_begin_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "bus_range_begin");
        CHECK_JSON_NULL_ERR_OUT(bus_range_begin_json, "bus_range_begin")
        cJSON *bus_range_end_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "bus_range_end");
        CHECK_JSON_NULL_ERR_OUT(bus_range_end_json, "bus_range_end")
        cJSON *domain_json =
            SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "domain");
        CHECK_JSON_NULL_ERR_OUT(domain_json, "domain")

        if (parse_json_linux_u64(ecam_base_json, &pci_config->ecam_base) != 0 ||
            parse_json_linux_u64(ecam_size_json, &pci_config->ecam_size) != 0 ||
            parse_json_linux_u64(io_base_json, &pci_config->io_base) != 0 ||
            parse_json_linux_u64(io_size_json, &pci_config->io_size) != 0 ||
            parse_json_linux_u64(pci_io_base_json, &pci_config->pci_io_base) !=
                0 ||
            parse_json_linux_u64(mem32_base_json, &pci_config->mem32_base) !=
                0 ||
            parse_json_linux_u64(mem32_size_json, &pci_config->mem32_size) !=
                0 ||
            parse_json_linux_u64(pci_mem32_base_json,
                                 &pci_config->pci_mem32_base) != 0 ||
            parse_json_linux_u64(mem64_base_json, &pci_config->mem64_base) !=
                0 ||
            parse_json_linux_u64(mem64_size_json, &pci_config->mem64_size) !=
                0 ||
            parse_json_linux_u64(pci_mem64_base_json,
                                 &pci_config->pci_mem64_base) != 0 ||
            parse_json_linux_u32(bus_range_begin_json,
                                 &pci_config->bus_range_begin) != 0 ||
            parse_json_linux_u32(bus_range_end_json,
                                 &pci_config->bus_range_end) != 0 ||
            parse_json_linux_u8(domain_json, &pci_config->domain) != 0) {
            log_error("Failed to parse pci_config\n");
            goto err_out;
        }

        // log_info("pci_config %d: ecam_base=0x%llx, ecam_size=0x%llx, "
        //          "io_base=0x%llx, io_size=0x%llx, "
        //          "pci_io_base=0x%llx, mem32_base=0x%llx, mem32_size=0x%llx, "
        //          "pci_mem32_base=0x%llx, mem64_base=0x%llx,
        //          mem64_size=0x%llx, " "pci_mem64_base=0x%llx", i,
        //          pci_config->ecam_base, pci_config->ecam_size,
        //          pci_config->io_base, pci_config->io_size,
        //          pci_config->pci_io_base, pci_config->mem32_base,
        //          pci_config->mem32_size, pci_config->pci_mem32_base,
        //          pci_config->mem64_base, pci_config->mem64_size,
        //          pci_config->pci_mem64_base);
    }

    cJSON *alloc_pci_devs_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "alloc_pci_devs");
    int num_pci_devs = SAFE_CJSON_GET_ARRAY_SIZE(alloc_pci_devs_json);
    config->num_pci_devs = num_pci_devs;
    for (int i = 0; i < num_pci_devs; i++) {
        cJSON *dev_config_json =
            SAFE_CJSON_GET_ARRAY_ITEM(alloc_pci_devs_json, i);
        hv_pci_dev_config_t *dev_config = &config->alloc_pci_devs[i];

        cJSON *dev_domain_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "domain");
        cJSON *dev_bus_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "bus");
        cJSON *dev_device_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "device");
        cJSON *dev_function_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "function");
        cJSON *dev_vbus_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "v_bus");
        cJSON *dev_vdevice_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "v_device");
        cJSON *dev_vfunction_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "v_function");
        cJSON *dev_type_json =
            SAFE_CJSON_GET_OBJECT_ITEM(dev_config_json, "dev_type");

        if (parse_json_linux_u8(dev_domain_json, &dev_config->domain) != 0 ||
            parse_json_linux_u8(dev_bus_json, &dev_config->bus) != 0 ||
            parse_json_linux_u8(dev_device_json, &dev_config->device) != 0 ||
            parse_json_linux_u8(dev_function_json, &dev_config->function) !=
                0 ||
            parse_json_linux_u8(dev_vbus_json, &dev_config->v_bus) != 0 ||
            parse_json_linux_u8(dev_vdevice_json, &dev_config->v_device) != 0 ||
            parse_json_linux_u8(dev_vfunction_json, &dev_config->v_function) !=
                0 ||
            parse_json_linux_u32(dev_type_json, &dev_config->dev_type) != 0) {
            log_error("Failed to parse pci device config\n");
            goto err_out;
        }
    }
    return 0;
err_out:
    return -1;
}

static int zone_start_from_json(const char *json_config_path,
                                zone_config_t *config) {
    cJSON *root = NULL;

    FILE *file = fopen(json_config_path, "r");
    if (file == NULL) {
        log_error("Error opening json file: %s", json_config_path);
        exit(1);
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = malloc(file_size + 1);
    if (fread(buffer, 1, file_size, file) == 0) {
        log_error("Error reading json file: %s", json_config_path);
        fclose(file);
        goto err_out;
    }
    fclose(file);
    buffer[file_size] = '\0';

    // parse JSON
    root = SAFE_CJSON_PARSE(buffer);
    cJSON *zone_id_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "zone_id");
    cJSON *cpus_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "cpus");
    cJSON *name_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "name");
    cJSON *memory_regions_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "memory_regions");
    cJSON *kernel_filepath_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "kernel_filepath");
    cJSON *dtb_filepath_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "dtb_filepath");
    cJSON *kernel_load_paddr_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "kernel_load_paddr");
    cJSON *dtb_load_paddr_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "dtb_load_paddr");
    cJSON *entry_point_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "entry_point");
    cJSON *interrupts_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "interrupts");
    cJSON *ivc_configs_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "ivc_configs");
    cJSON *modules_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "modules");

    CHECK_JSON_NULL_ERR_OUT(zone_id_json, "zone_id")
    CHECK_JSON_NULL_ERR_OUT(cpus_json, "cpus")
    CHECK_JSON_NULL_ERR_OUT(name_json, "name")
    CHECK_JSON_NULL_ERR_OUT(memory_regions_json, "memory_regions")
    CHECK_JSON_NULL_ERR_OUT(kernel_filepath_json, "kernel_filepath")
    CHECK_JSON_NULL_ERR_OUT(dtb_filepath_json, "dtb_filepath")
    CHECK_JSON_NULL_ERR_OUT(kernel_load_paddr_json, "kernel_load_paddr")
    CHECK_JSON_NULL_ERR_OUT(dtb_load_paddr_json, "dtb_load_paddr")
    CHECK_JSON_NULL_ERR_OUT(entry_point_json, "entry_point")
    CHECK_JSON_NULL_ERR_OUT(interrupts_json, "interrupts")
    CHECK_JSON_NULL_ERR_OUT(ivc_configs_json, "ivc_configs")
    // modules is an optional configuration, just skip it here

    config->zone_id = zone_id_json->valueint;

    int num_cpus = SAFE_CJSON_GET_ARRAY_SIZE(cpus_json);

    for (int i = 0; i < num_cpus; i++) {
        config->cpus |=
            (1 << SAFE_CJSON_GET_ARRAY_ITEM(cpus_json, i)->valueint);
    }

    int num_memory_regions = SAFE_CJSON_GET_ARRAY_SIZE(memory_regions_json);
    int num_interrupts = SAFE_CJSON_GET_ARRAY_SIZE(interrupts_json);

    if (num_memory_regions > CONFIG_MAX_MEMORY_REGIONS ||
        num_interrupts > CONFIG_MAX_INTERRUPTS) {
        log_error("Exceeded maximum allowed regions/interrupts.");
        goto err_out;
    }

    // Iterate through each memory region of the zone
    // Including memory and MMIO regions of the zone
    config->num_memory_regions = num_memory_regions;
    for (int i = 0; i < num_memory_regions; i++) {
        cJSON *region = SAFE_CJSON_GET_ARRAY_ITEM(memory_regions_json, i);
        memory_region_t *mem_region = &config->memory_regions[i];

        cJSON *physical_start_json =
            SAFE_CJSON_GET_OBJECT_ITEM(region, "physical_start");
        cJSON *virtual_start_json =
            SAFE_CJSON_GET_OBJECT_ITEM(region, "virtual_start");
        cJSON *size_json = SAFE_CJSON_GET_OBJECT_ITEM(region, "size");

        if (parse_json_linux_u64(physical_start_json,
                                 &mem_region->physical_start) != 0 ||
            parse_json_linux_u64(virtual_start_json,
                                 &mem_region->virtual_start) != 0 ||
            parse_json_linux_u64(size_json, &mem_region->size) != 0) {
            log_error("Failed to parse memory region %d\n", i);
            goto err_out;
        }

        const char *type_str =
            SAFE_CJSON_GET_OBJECT_ITEM(region, "type")->valuestring;
        if (strcmp(type_str, "ram") == 0) {
            mem_region->type = MEM_TYPE_RAM;
        } else if (strcmp(type_str, "io") == 0) {
            // io device
            mem_region->type = MEM_TYPE_IO;
        } else if (strcmp(type_str, "virtio") == 0) {
            // virtio device
            mem_region->type = MEM_TYPE_VIRTIO;
        } else {
            log_error("Unknown memory region type: %s", type_str);
            goto err_out;
        }

        log_debug("memory_region %d: type %d, physical_start %llx, "
                  "virtual_start %llx, size %llx",
                  i, mem_region->type, mem_region->physical_start,
                  mem_region->virtual_start, mem_region->size);
    }

    // irq
    log_info("num interrupts %d", num_interrupts);
    memset(config->interrupts_bitmap, 0, sizeof(config->interrupts_bitmap));
    log_info("interrupts_bitmap %p, size %zu, is cleared",
             config->interrupts_bitmap, sizeof(config->interrupts_bitmap));
    for (int i = 0; i < num_interrupts; i++) {
        const cJSON *const item = SAFE_CJSON_GET_ARRAY_ITEM(interrupts_json, i);

        size_t irq;
        if (parse_json_size(item, &irq) != 0) {
            log_error("Failed to parse irq %d", i);
            goto err_out;
        }

        if (irq >= CONFIG_MAX_INTERRUPTS) {
            log_error("irq %zu is out of range", irq);
            goto err_out;
        }

        // irq is valid, set the bit in the bitmap
        const size_t word_index = irq / CONFIG_INTERRUPTS_BITMAP_BITS_PER_WORD;
        const size_t bit_index = irq % CONFIG_INTERRUPTS_BITMAP_BITS_PER_WORD;
        config->interrupts_bitmap[word_index] |= ((BitmapWord)1) << bit_index;
        log_info("irq %zu is valid, set the bit in the bitmap", irq);
    }

    // ivc
    int num_ivc_configs = SAFE_CJSON_GET_ARRAY_SIZE(ivc_configs_json);
    if (num_ivc_configs > CONFIG_MAX_IVC_CONFIGS) {
        log_error("Exceeded maximum allowed ivc configs.");
        goto err_out;
    }

    config->num_ivc_configs = num_ivc_configs;
    for (int i = 0; i < num_ivc_configs; i++) {
        cJSON *ivc_config_json = SAFE_CJSON_GET_ARRAY_ITEM(ivc_configs_json, i);
        ivc_config_t *ivc_config = &config->ivc_configs[i];
        ivc_config->ivc_id =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "ivc_id")->valueint;
        ivc_config->peer_id =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "peer_id")->valueint;

        cJSON *shared_mem_ipa_json =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "shared_mem_ipa");
        cJSON *control_table_ipa_json =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "control_table_ipa");
        cJSON *rw_sec_size_json =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "rw_sec_size");
        cJSON *out_sec_size_json =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "out_sec_size");

        if (parse_json_linux_u64(shared_mem_ipa_json,
                                 &ivc_config->shared_mem_ipa) != 0 ||
            parse_json_linux_u64(control_table_ipa_json,
                                 &ivc_config->control_table_ipa) != 0 ||
            parse_json_linux_u32(rw_sec_size_json, &ivc_config->rw_sec_size) !=
                0 ||
            parse_json_linux_u32(out_sec_size_json,
                                 &ivc_config->out_sec_size) != 0) {
            log_error("Failed to parse ivc config\n");
            goto err_out;
        }

        ivc_config->interrupt_num =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "interrupt_num")
                ->valueint;
        ivc_config->max_peers =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "max_peers")->valueint;
        log_info("ivc_config %d: ivc_id %d, peer_id %d, shared_mem_ipa %llx, "
                 "interrupt_num %d, max_peers %d\n",
                 i, ivc_config->ivc_id, ivc_config->peer_id,
                 ivc_config->shared_mem_ipa, ivc_config->interrupt_num,
                 ivc_config->max_peers);
    }

    if (parse_json_linux_u64(entry_point_json, &config->entry_point) != 0 ||
        parse_json_linux_u64(kernel_load_paddr_json,
                             &config->kernel_load_paddr) != 0 ||
        parse_json_linux_u64(dtb_load_paddr_json, &config->dtb_load_paddr) !=
            0) {
        log_error("Failed to parse entry_point, kernel_load_paddr or "
                  "dtb_load_paddr\n");
        goto err_out;
    }
    // MULTIBOOT SUPPORT: Check for Multiboot mode first
    cJSON *multiboot_json = cJSON_GetObjectItem(root, "multiboot_enabled");
    if (multiboot_json != NULL && cJSON_IsBool(multiboot_json)) {
        config->arch_config.multiboot_enabled =
            cJSON_IsTrue(multiboot_json) ? 1 : 0;
    } else {
        config->arch_config.multiboot_enabled = 0; // Default: disabled
    }

    // Calculate GPA-to-HPA offset from first memory region
    // This is used to translate ELF p_paddr (GPA) to actual load address (HPA)
    int64_t gpa_to_hpa_offset = 0;
    if (config->arch_config.multiboot_enabled && num_memory_regions > 0) {
        // Use first RAM region for offset calculation
        for (int i = 0; i < num_memory_regions; i++) {
            memory_region_t *mem_region = &config->memory_regions[i];
            if (mem_region->type == MEM_TYPE_RAM) {
                gpa_to_hpa_offset = (int64_t)mem_region->physical_start -
                                    (int64_t)mem_region->virtual_start;
                fprintf(stderr,
                        "[MULTIBOOT] GPA-to-HPA offset: 0x%llx (HPA=0x%llx, "
                        "GPA=0x%llx)\n",
                        (long long)gpa_to_hpa_offset,
                        (long long)mem_region->physical_start,
                        (long long)mem_region->virtual_start);
                break;
            }
        }
    }

    // Load kernel image to memory
    if (config->arch_config.multiboot_enabled) {
        // For Multiboot/ELF kernels, properly load each ELF segment
        fprintf(stderr,
                "[MULTIBOOT] Loading ELF segments for Multiboot kernel\n");
        fprintf(stderr, "[MULTIBOOT] Kernel path: %s\n",
                kernel_filepath_json->valuestring);
        fflush(stderr);
        uint64_t total_size = 0;
        int ret = load_elf_segments(kernel_filepath_json->valuestring,
                                    &total_size, gpa_to_hpa_offset);
        fprintf(
            stderr,
            "[MULTIBOOT] load_elf_segments returned: %d, total_size: 0x%llx\n",
            ret, total_size);
        fflush(stderr);
        if (ret != 0) {
            log_error("Failed to load ELF segments for Multiboot kernel\n");
            goto err_out;
        }
        config->kernel_size = total_size;
    } else {
        // Non-Multiboot: load entire image to kernel_load_paddr
        config->kernel_size = load_image_to_memory(
            kernel_filepath_json->valuestring, config->kernel_load_paddr);
    }

// Load dtb to memory
// x86_64 uses ACPI
#ifndef X86_64
    config->dtb_size = load_image_to_memory(dtb_filepath_json->valuestring,
                                            config->dtb_load_paddr);
#endif

    log_info("Kernel size: %llu, DTB size: %llu", config->kernel_size,
             config->dtb_size);

    // ============================================================
    // MULTIBOOT SUPPORT: Build Multiboot2 info for Asterinas
    // ============================================================
    fprintf(stderr, "[MULTIBOOT] ====== Starting Multiboot2 support ======\n");

    fprintf(stderr, "[MULTIBOOT] multiboot_enabled = %u\n",
            config->arch_config.multiboot_enabled);

    if (config->arch_config.multiboot_enabled) {
        fprintf(stderr, "[MULTIBOOT] Multiboot mode enabled!\n");

        // Get kernel command line
        cJSON *kcmdline_json = cJSON_GetObjectItem(root, "kernel_cmdline");
        const char *cmdline = "";
        if (kcmdline_json != NULL && kcmdline_json->valuestring != NULL) {
            cmdline = kcmdline_json->valuestring;
        }
        fprintf(stderr, "[MULTIBOOT] Kernel cmdline: '%s'\n", cmdline);

        // Get Multiboot info address from JSON (or use default)
        // This is the GPA where guest expects multiboot info
        cJSON *mb_info_paddr_json =
            cJSON_GetObjectItem(root, "multiboot_info_paddr");
        uint64_t mb_info_gpa = 0x9000000; // Default GPA
        if (mb_info_paddr_json != NULL) {
            parse_json_linux_u64(mb_info_paddr_json, &mb_info_gpa);
        }
        // Calculate actual HPA for loading
        uint64_t mb_info_hpa =
            (uint64_t)((int64_t)mb_info_gpa + gpa_to_hpa_offset);
        // Store GPA in config (guest will see this address)
        config->arch_config.multiboot_info_paddr = mb_info_gpa;
        fprintf(stderr, "[MULTIBOOT] Multiboot info: GPA=0x%llx, HPA=0x%llx\n",
                mb_info_gpa, mb_info_hpa);

        // Find ELF entry point from kernel ELF
        uint64_t elf_entry = 0;
        if (find_elf_entry_point(kernel_filepath_json->valuestring,
                                 &elf_entry) == 0) {
            fprintf(stderr, "[MULTIBOOT] Found ELF entry point (GPA): 0x%llx\n",
                    elf_entry);
            // Bootloader jumps to kernel entry; store it in kernel_entry_gpa
            config->arch_config.kernel_entry_gpa = elf_entry;
            fprintf(stderr, "[MULTIBOOT] kernel_entry_gpa set to: 0x%llx\n",
                    elf_entry);
        } else {
            log_error("[MULTIBOOT] Failed to find ELF entry point!");
            goto err_out;
        }

        // Load initramfs for Multiboot2 (if specified)
        uint64_t initramfs_gpa = 0;
        uint64_t initramfs_size = 0;
        const char *initramfs_cmdline = "";
        cJSON *initramfs_filepath_json =
            cJSON_GetObjectItem(root, "initramfs_filepath");
        cJSON *initramfs_load_gpa_json =
            cJSON_GetObjectItem(root, "initramfs_load_gpa");
        if (initramfs_filepath_json != NULL &&
            initramfs_load_gpa_json != NULL &&
            initramfs_filepath_json->valuestring != NULL &&
            strcmp(initramfs_filepath_json->valuestring, "null") != 0) {
            parse_json_linux_u64(initramfs_load_gpa_json, &initramfs_gpa);
            uint64_t initramfs_hpa =
                (uint64_t)((int64_t)initramfs_gpa + gpa_to_hpa_offset);
            fprintf(stderr,
                    "[MULTIBOOT] Loading initramfs: %s to GPA=0x%llx "
                    "(HPA=0x%llx)\n",
                    initramfs_filepath_json->valuestring, initramfs_gpa,
                    initramfs_hpa);
            initramfs_size = load_image_to_memory(
                initramfs_filepath_json->valuestring, initramfs_hpa);
            fprintf(stderr, "[MULTIBOOT] Initramfs loaded: size=0x%llx\n",
                    initramfs_size);
            initramfs_cmdline = initramfs_filepath_json->valuestring;
        } else {
            fprintf(stderr, "[MULTIBOOT] No initramfs configured\n");
        }

        // Build Multiboot2 info structure
        // Calculate total memory from memory regions
        uint64_t total_mem = 0;
        for (int i = 0; i < num_memory_regions; i++) {
            memory_region_t *mem_region = &config->memory_regions[i];
            if (mem_region->type == MEM_TYPE_RAM) {
                total_mem += mem_region->size;
                fprintf(stderr, "[MULTIBOOT] Memory region %d: size 0x%llx\n",
                        i, mem_region->size);
            }
        }
        fprintf(stderr, "[MULTIBOOT] Total RAM: 0x%llx bytes\n", total_mem);

        void *mb_info = build_multiboot2_info(
            total_mem, cmdline, config->memory_regions, num_memory_regions,
            initramfs_gpa, initramfs_size, initramfs_cmdline);
        if (!mb_info) {
            log_error("[MULTIBOOT] Failed to build Multiboot2 info!");
            goto err_out;
        }

        fprintf(stderr,
                "[MULTIBOOT] Loading Multiboot2 info to HPA: 0x%llx (GPA: "
                "0x%llx)\n",
                mb_info_hpa, mb_info_gpa);

        // Load Multiboot info to memory at HPA (use larger size to accommodate
        // tags)
        uint64_t mb_info_size = 8192; // 8KB - enough for module tags
        load_buffer_to_memory(mb_info, mb_info_size, mb_info_hpa);

        // Free the allocated buffer
        free(mb_info);

        fprintf(stderr, "[MULTIBOOT] Multiboot2 info loaded successfully!\n");
        fprintf(stderr, "[MULTIBOOT] ====== Multiboot2 support ready ======\n");
    } else {
        fprintf(stderr,
                "[MULTIBOOT] Multiboot disabled, using default boot mode\n");
    }
    // ============================================================
    // END MULTIBOOT SUPPORT
    // ============================================================

    // modules configuration is optional, return -1 if failed, otherwise 0
    if (parse_modules(modules_json)) {
        goto err_out;
    }

    // check name length
    if (strlen(name_json->valuestring) > CONFIG_NAME_MAXLEN) {
        log_error("Zone name too long: %s", name_json->valuestring);
        goto err_out;
    }
    strncpy(config->name, name_json->valuestring, CONFIG_NAME_MAXLEN);

    log_info("Zone name: %s", config->name);

#ifndef LOONGARCH64
    // Parse architecture-specific configurations (interrupts for each platform)
    if (parse_arch_config(root, config, gpa_to_hpa_offset))
        goto err_out;

#endif

    if (parse_pci_config(root, config))
        goto err_out;

    if (root)
        cJSON_Delete(root);
    if (buffer)
        free(buffer);

    int fd = open_dev();
    if (fd < 0) {
        perror("zone_start: open hvisor failed");
        goto err_out;
    }

    log_info("Calling ioctl to start zone: [%s]", config->name);

    int err = ioctl(fd, HVISOR_ZONE_START, config);

    if (err)
        perror("zone_start: ioctl failed");

    close(fd);

    return 0;
err_out:
    if (root)
        cJSON_Delete(root);
    if (buffer)
        free(buffer);
    return -1;
}

// ./hvisor zone start <path_to_config_file>
static int zone_start(int argc, char *argv[]) {
    char *json_config_path = NULL;
    zone_config_t config;
    int fd, ret;
    uint64_t hvisor_config_version;

    if (argc != 4) {
        help(1);
    }
    json_config_path = argv[3];

    memset(&config, 0, sizeof(zone_config_t));

    fd = open_dev();
    ret = ioctl(fd, HVISOR_CONFIG_CHECK, &hvisor_config_version);
    close(fd);

    if (ret) {
        log_error("ioctl: hvisor config check failed, ret %d", ret);
        return -1;
    }

    if (hvisor_config_version != CONFIG_MAGIC_VERSION) {
        log_error("zone start failed because config versions mismatch, "
                  "hvisor-tool is 0x%x, hvisor is 0x%x",
                  CONFIG_MAGIC_VERSION, hvisor_config_version);
        return -1;
    } else {
        log_info("zone config check pass");
    }

    return zone_start_from_json(json_config_path, &config);
}

// ./hvisor zone shutdown -id 1
static int zone_shutdown(int argc, char *argv[]) {
    if (argc != 2 || strcmp(argv[0], "-id") != 0) {
        help(1);
    }
    __u64 zone_id;
    sscanf(argv[1], "%llu", &zone_id);
    int fd = open_dev();
    int err = ioctl(fd, HVISOR_ZONE_SHUTDOWN, zone_id);
    if (err)
        perror("zone_shutdown: ioctl failed");
    close(fd);
    return err;
}

static void print_cpu_list(__u64 cpu_mask, char *outbuf, size_t bufsize) {
    int found_cpu = 0;
    char *buf = outbuf;

    for (int i = 0; i < MAX_CPUS && buf - outbuf < bufsize; i++) {
        if ((cpu_mask & (1ULL << i)) != 0) {
            if (found_cpu) {
                *buf++ = ',';
                *buf++ = ' ';
            }
            snprintf(buf, bufsize - (buf - outbuf), "%d", i);
            buf += strlen(buf);
            found_cpu = 1;
        }
    }
    if (!found_cpu) {
        memcpy(outbuf, "none", 5);
    }
}

// ./hvisor zone list
static int zone_list(int argc, char *argv[]) {
    if (argc != 0) {
        help(1);
    }
    __u64 cnt = CONFIG_MAX_ZONES;
    zone_info_t *zones = malloc(sizeof(zone_info_t) * cnt);
    zone_list_args_t args = {cnt, zones};
    // printf("zone_list: cnt %llu, zones %p\n", cnt, zones);
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_ZONE_LIST, &args);
    if (ret < 0)
        perror("zone_list: ioctl failed");

    printf("| %11s     | %10s        | %9s       | %10s |\n", "zone_id", "cpus",
           "name", "status");

    for (int i = 0; i < ret; i++) {
        char cpu_list_str[256]; // Assuming this buffer size is enough
        memset(cpu_list_str, 0, sizeof(cpu_list_str));
        print_cpu_list(zones[i].cpus, cpu_list_str, sizeof(cpu_list_str));
        printf("| %15u | %17s | %15s | %10s |\n", zones[i].zone_id,
               cpu_list_str, zones[i].name,
               zones[i].is_err ? "error" : "running");
    }
    free(zones);
    close(fd);
    return ret;
}

int main(int argc, char *argv[]) {
    // Direct debug output to stderr to ensure we see it
    for (int i = 0; i < argc; i++) {
    }

    // Set log level to INFO to see all logs
    log_set_level(LOG_INFO);
    log_set_quiet(false);

    int err = 0;

    multithread_log_init();
    initialize_log();
    atexit(multithread_log_exit);

    if (argc < 3)
        help(1);

    if (strcmp(argv[1], "zone") == 0) {
        if (argc < 3)
            help(1);

        if (strcmp(argv[2], "start") == 0) {
            err = zone_start(argc, argv);
        } else if (strcmp(argv[2], "shutdown") == 0) {
            err = zone_shutdown(argc - 3, &argv[3]);
        } else if (strcmp(argv[2], "list") == 0) {
            err = zone_list(argc - 3, &argv[3]);
        } else {
            help(1);
        }
    } else if (strcmp(argv[1], "virtio") == 0) {
        if (argc < 3)
            help(1);

        if (strcmp(argv[2], "start") == 0) {
            err = virtio_start(argc, argv);
        } else {
            help(1);
        }
    } else {
        help(1);
    }

    return err ? 1 : 0;
}
