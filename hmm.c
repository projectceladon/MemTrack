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

int hmm_memtrack_get_memory(pid_t pid, enum memtrack_type type,
                             struct memtrack_record *records,
                             size_t *num_records)
{
    size_t allocated_records = min(*num_records, ARRAY_SIZE(record_templates));
    int i;
    FILE *fp;
    char line[1024];
    char tmp[128];
    size_t unaccounted_size = 0;

    *num_records = ARRAY_SIZE(record_templates);

    /* fastpath to return the necessary number of records */
    if (allocated_records == 0) {
        return 0;
    }

    memcpy(records, record_templates,
           sizeof(struct memtrack_record) * allocated_records);

    /* Calculate active buffer */
    fp = fopen("/sys/devices/pci0000:00/0000:00:03.0/active_bo", "r");
    if (fp == NULL) {
        return -errno;
    }

    while (1) {
        unsigned long size;
        int ret;

        if (fgets(line, sizeof(line), fp) == NULL) {
            break;
        }

        /* Format:
         * 39 p buffer objects: 9696 KB
         */
        ret = sscanf(line, "%*d p %*s %*s %zd\n", &size);
        if (ret != 1) {
            continue;
        }

        if (pid == 1) {
            unaccounted_size += size;
        }
    }

    records[0].size_in_bytes = unaccounted_size * 1024;

    fclose(fp);

    /* Calculate reserved_pool's buffer */
    fp = fopen("/sys/devices/pci0000:00/0000:00:03.0/reserved_pool", "r");
    if (fp == NULL) {
        return -errno;
    }

    while (1) {
        unsigned long size;
        int ret;

        if (fgets(line, sizeof(line), fp) == NULL) {
            break;
        }

        /* Format:
         * 16008 out of 18432 pages available
         */
        ret = sscanf(line, "%ld %*s\n", &size);
        if (ret != 1) {
            continue;
        }

        if (pid == 1) {
            unaccounted_size += size * 4;
        }
    }

    records[0].size_in_bytes = unaccounted_size * 1024;

    fclose(fp);

    /* Calculate dynamic_pool's buffer */
    fp = fopen("/sys/devices/pci0000:00/0000:00:03.0/dynamic_pool", "r");
    if (fp == NULL) {
        return -errno;
    }

    while (1) {
        unsigned long size;
        int ret;

        if (fgets(line, sizeof(line), fp) == NULL) {
            break;
        }

        /* Format:
         * 16008 (max 18432) pages available
         */
        ret = sscanf(line, "%ld %*s\n", &size);
        if (ret != 1) {
            continue;
        }

        if (pid == 1) {
            unaccounted_size += size * 4;
        }
    }

    records[0].size_in_bytes = unaccounted_size * 1024;

    fclose(fp);

    return 0;
}
