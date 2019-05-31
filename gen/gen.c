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
#include <cutils/hashmap.h>

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

int gen_memtrack_get_memory(pid_t pid, enum memtrack_type type,
                             struct memtrack_record *records,
                             size_t *num_records)
{
    size_t allocated_records = min(*num_records, ARRAY_SIZE(record_templates));
    int i;
    FILE *fp;
    FILE *smaps_fp;
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

    sprintf(tmp, "/sys/class/drm/card0/gfx_memtrack/%d", pid);

    fp = fopen(tmp, "r");
    if (fp == NULL) {
        return -errno;
    }

    snprintf(tmp, sizeof(tmp), "/proc/%d/smaps", pid);
    smaps_fp = fopen(tmp, "r");
    if (smaps_fp == NULL) {
        while (1) {
            char line[1024];
            int ret, matched_pid, Gfxmem;

            if (fgets(line, sizeof(line), fp) == NULL) {
                break;
            }
            ret = sscanf(line, "%d %dK %*[^\n]", &matched_pid, &Gfxmem);

            if (ret == 2 && matched_pid == pid) {
                records[0].size_in_bytes = Gfxmem * 1024;
                break;
            }
        }

        fclose(fp);
        return 0;
    }

    while (1) {
        char line[1024];
        int size;
        int ret, matched_pid, Gfxmem, mapped_size = 0;

        if (fgets(line, sizeof(line), fp) == NULL) {
             break;
        }

        /* Format:
         *  PID    GfxMem   Process
         * 2454    37060K /system/bin/surfaceflinger
        */

        ret = sscanf(line, "%d %dK %*[^\n]", &matched_pid, &Gfxmem);

        if (ret == 2 && matched_pid == pid) {
            while (1) {
                char cmdline[1024];
                unsigned long smaps_size;

                if (fgets(line, sizeof(line), smaps_fp) == NULL) {
                    break;
                }

                if (sscanf(line, "%*s %*s %*s %*s %*s %1000[^\n]", cmdline) == 1) {
                    continue;
                }

                if (strcmp(cmdline, "/dev/dri/card0") && strncmp(cmdline, "/drm mm object", 12)) {
                    continue;
                }

                if (sscanf(line, "Rss: %lu kB", &smaps_size) == 1) {
                    if (smaps_size) {
                        mapped_size += smaps_size;
                        continue;
                    }
                }
            }
            unaccounted_size = Gfxmem - mapped_size;
            break;
        }
    }

    records[0].size_in_bytes = unaccounted_size * 1024;

    fclose(smaps_fp);
    fclose(fp);

    return 0;
}
