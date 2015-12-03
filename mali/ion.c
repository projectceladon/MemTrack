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
#include <dirent.h>
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

size_t get_ion(pid_t pid, char* ion_heap)
{
    FILE *fp;
    char tmp[128];
    size_t unaccounted_size = 0;

    sprintf(tmp, "/d/ion/heaps/%s", ion_heap);

    fp = fopen(tmp, "r");
    if (fp == NULL) {
        ALOGE("%s not found", ion_heap);
        return 0;
    }

    while(1) {
        char line[1024];
        int ret, matched_pid;
        size_t IONmem;

        if (fgets(line, sizeof(line), fp) == NULL) {
            break;
        }

        /* Format:
         *           client              pid             size
         *   surfaceflinger              179         33423360
        */

        ret = sscanf(line, "%*s %d %zd %*[^\n]", &matched_pid, &IONmem);

        if (ret == 2 && matched_pid == pid) {
            ALOGE("ION is %zd", IONmem);
            unaccounted_size += IONmem;
            continue;
        }
    }
    fclose(fp);
    return unaccounted_size;
}


int ion_memtrack_get_memory(pid_t pid, enum memtrack_type type,
                             struct memtrack_record *records,
                             size_t *num_records)
{
    size_t allocated_records = min(*num_records, ARRAY_SIZE(record_templates));
    int i;
    FILE *fp;
    DIR *pdir;
    struct dirent *pdirent;
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

    unaccounted_size += get_ion(pid, "cma-heap");
    unaccounted_size += get_ion(pid, "system-heap");

    records[0].size_in_bytes = unaccounted_size;

    return 0;
}
