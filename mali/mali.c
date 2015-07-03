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

int mali_memtrack_get_memory(pid_t pid, enum memtrack_type type,
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

    sprintf(tmp, "/sys/kernel/debug/mali/gpu_memory");

    fp = fopen(tmp, "r");
    if (fp == NULL) {
        return -errno;
    }

    while (1) {
        char line[1024];
        int size;
        int ret, matched_pid, Gfxmem;

        if (fgets(line, sizeof(line), fp) == NULL) {
             break;
        }

        /* Format:
         * Name (:bytes)              pid         mali_mem    max_mali_mem     external_mem     ump_mem     dma_mem
         * RenderThread               3941        13008896    37167104         0                0           11640832
        */

        ret = sscanf(line, "  %*25[^\n] %d %d %*[^\n]", &matched_pid, &Gfxmem);

        if (ret == 2 && matched_pid == pid) {
            ALOGD("Gfxmem is %d", Gfxmem);
            unaccounted_size = Gfxmem;
            break;
        }
    }

    fclose(fp);

    sprintf(tmp, "/sys/kernel/debug/ion/cma-heap");

    fp = fopen(tmp, "r");
    if (fp == NULL) {
        return -errno;
    }

    while (1) {
        char line[1024];
        int size;
        int ret, matched_pid, IONmem;

        if (fgets(line, sizeof(line), fp) == NULL) {
             break;
        }

        /* Format:
         *           client              pid             size
         *   surfaceflinger              179         33423360
        */

        ret = sscanf(line, "%*s %d %d %*[^\n]", &matched_pid, &IONmem);

        if (ret == 2 && matched_pid == pid) {
            ALOGD("IONmem is %d", IONmem);
            unaccounted_size += IONmem;
            continue;
        }
    }

    records[0].size_in_bytes = unaccounted_size;

    fclose(fp);

    return 0;
}
