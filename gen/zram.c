/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <cutils/log.h>

#include <hardware/memtrack.h>

#include "memtrack_intel.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#define min(x, y) ((x) < (y) ? (x) : (y))

static struct memtrack_record record_templates[] = {
    {
        .flags = MEMTRACK_FLAG_SMAPS_UNACCOUNTED |
                 MEMTRACK_FLAG_PRIVATE |
                 MEMTRACK_FLAG_NONSECURE,
    },
};

static long get_zram_used_total_size()
{
    FILE *fp;
    int len;
    char line[1024];
    unsigned long used_total = 0;

    fp = fopen("/sys/block/zram0/mem_used_total", "r");
    if (fp == NULL) {
        return used_total;
    }

    if (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "%lu", &used_total) != 1) {
            fclose(fp);
            return -errno;
        }
    }

    fclose(fp);
    return used_total;
}

static long get_swapped_total_size()
{
    FILE *fp;
    char line[1024];
    unsigned long swap_free = 0;
    unsigned long swap_total = 0;

    fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) {
        return -errno;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "SwapTotal: %lu kB", &swap_total) == 1) {
            break;
        }
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "SwapFree: %lu kB", &swap_free) == 1) {
            break;
        }
    }

    fclose(fp);
    return (swap_total - swap_free) * 1024;
}

int zram_memtrack_get_memory(pid_t pid, enum memtrack_type type,
                             struct memtrack_record *records,
                             size_t *num_records)
{

    if (type != MEMTRACK_TYPE_OTHER) {
        return 0;
    }

    size_t allocated_records = min(*num_records, ARRAY_SIZE(record_templates));
    FILE *fp;
    char line[1024];
    char file_name[128];

    double ratio = 0.0;
    long zram_used = 0, swapped = 0;
    unsigned long pswap_size = 0, pswap_total = 0;

    *num_records = ARRAY_SIZE(record_templates);

    /* fastpath to return the necessary number of records */
    if (allocated_records == 0) {
        return 0;
    }

    memcpy(records, record_templates,
           sizeof(struct memtrack_record) * allocated_records);

    zram_used = get_zram_used_total_size();
    swapped = get_swapped_total_size();
    if (swapped > 0) {
        ratio = (double)zram_used / swapped;
    }

    sprintf(file_name, "/proc/%d/smaps", pid);
    fp = fopen(file_name, "r");
    if (fp == NULL) {
        return -errno;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "PSwap: %lu kB", &pswap_size) == 1) {
            pswap_total += pswap_size;
        }
    }

    fclose(fp);

    records[0].size_in_bytes = (size_t)(pswap_total * (1024 * ratio));

#if 0
    if (pswap_total > 0) {
        ALOGE("Memtrack process: %d", pid);
        ALOGE("Swapped total size: %lu kB", get_swapped_total_size() / 1024);
        ALOGE("Zram used total size: %lu kB", zram_used / 1024);
        ALOGE("Zram compress ratio (swapped/zram): %f", ratio > 0.0 ? 1/ratio : 1.0);
        ALOGE("Process memtrack size: %zu kB", records[0].size_in_bytes / 1024);

        fp = fopen(file_name, "r");
        if (fp == NULL) {
            return -errno;
        }

        ALOGE("Memtrack dump smpas: %s", file_name);
        while (fgets(line, sizeof(line), fp) != NULL) {
            ALOGE("%s", line);
        }
        fclose(fp);
    }
#endif

    return 0;
}
