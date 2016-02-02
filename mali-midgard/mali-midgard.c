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

int mali_midgard_memtrack_get_memory(pid_t pid, enum memtrack_type type,
                             struct memtrack_record *records,
                             size_t *num_records)
{
    size_t allocated_records = min(*num_records, ARRAY_SIZE(record_templates));
    int i;
    FILE *fp;
    DIR *pdir;
    struct dirent *pdirent;
    char line[1024];
    char cmdline[64];
    char tmp[128];
    size_t unaccounted_size = 0;

    *num_records = ARRAY_SIZE(record_templates);

    /* fastpath to return the necessary number of records */
    if (allocated_records == 0) {
        return 0;
    }

    memcpy(records, record_templates,
           sizeof(struct memtrack_record) * allocated_records);

    pdir = opendir("/sys/kernel/debug/mali0/ctx");

    if (pdir == NULL) {
        return -errno;
    }

    while (pdirent = readdir(pdir)) {
        int ret, matched_pid;

        if (strcmp(pdirent->d_name, ".") == 0 ||
            strcmp(pdirent->d_name, "..") == 0) {
            continue;
        }

        ret = sscanf(pdirent->d_name, "%d_%*d", &matched_pid);

        if (ret == 1 && matched_pid == pid) { 
            snprintf(tmp, 128, "/sys/kernel/debug/mali0/ctx/%s/mem_profile", pdirent->d_name);

            fp = fopen(tmp, "r");

            if (fp == NULL) {
               closedir(pdir);
               return -errno;
            }

            while(1) {
                size_t Gfxmem;
                fseek(fp, -50, SEEK_END);

                if (fgets(line, sizeof(line), fp) == NULL) {
                     break;
                }

                /* Format:
                 * .....
                 * Total allocated memory: 2822048
                */

                ret = sscanf(line, "Total allocated memory: %zd", &Gfxmem);

                if (ret == 1) {
                    ALOGD("Gfxmem is %zd", Gfxmem);
                    unaccounted_size += Gfxmem;
                    break;
                }
            }
            fclose(fp);
            break;
        }
    }

    closedir(pdir);

    records[0].size_in_bytes = unaccounted_size;

    return 0;
}
