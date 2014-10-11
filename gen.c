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

struct memtrack_record record_templates[] = {
    {
        .flags = MEMTRACK_FLAG_SMAPS_UNACCOUNTED |
                 MEMTRACK_FLAG_PRIVATE |
                 MEMTRACK_FLAG_NONSECURE,
    },
};

static int str_hash(void *key)
{
    return hashmapHash(key, strlen(key));
}

static bool str_equals(void *keyA, void *keyB)
{
    return strcmp((char*)keyA, (char*)keyB) == 0;
}

static bool free_key(void* key, void* value, void* context) {
    free(key);
    return true;
}

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
    
    fp = fopen("/sys/class/drm/card0/i915_gem_objinfo", "r");
    if (fp == NULL) {
        return -errno;
    }

    while (1) {
       char line[1024];
       int size;
       int ret, matched_pid;

       if (fgets(line, sizeof(line), fp) == NULL) {
            break;
       }

       ret = sscanf(line, "%d %*s", &matched_pid);
       if (ret != 1) {
            continue;
       }

       if (matched_pid == pid) {
           sprintf(tmp, "/proc/%d/smaps", pid);
           smaps_fp = fopen(tmp, "r");
           if (smaps_fp == NULL) {
              fclose(fp);
              return -errno;
           }

           Hashmap *kernel_address_map = hashmapCreate(32, str_hash, str_equals);
           if (kernel_address_map == NULL) {
              fclose(fp);
              fclose(smaps_fp);
              return -errno;
           }

           while (1) {
               char content[1024];
               char kernel_address[1024];
               char *info, *pch;
               int shared_count;
               unsigned long address1_output = 0, address2_output = 0;

               if (fgets(line, sizeof(line), fp) == NULL) {
                    break;
               }

               ret = sscanf(line, "%[0-9a-f]: %dK %[^\n]", kernel_address, &size, content);

               if (ret != 3) {
                   if (sscanf(line, "  PID %s", content) == 1) {
                       break;
                   }
               }else {
                   if (hashmapContainsKey(kernel_address_map, kernel_address)) {
                       continue;
                   }else {
                       char* key = strdup(kernel_address);
                       hashmapPut(kernel_address_map, key, &size);
                   }

                   shared_count = 0;
                   if (strstr(content, "allocated") == NULL && strstr(content, "purgeable") == NULL) {
                       continue;
                   }

                   info = strchr(content, '(');
                   
                   if (info == NULL) {
                       break;
                   }

                   pch = strtok(info, "()");

                   while (pch != NULL) {
                       if(strcmp(pch, "  ") > 0) {
                           int address1, address2;
                           shared_count++;
                           ret = sscanf(pch, "%d: %*s %16x %16x", &matched_pid, &address1, &address2);
                           if (matched_pid == pid && ret == 2) {
                               address1_output = address1;
                           }else if (matched_pid == pid && ret == 3) {
                               address1_output = address1;
                               address2_output = address2;
                           }
                       }
                       pch = strtok(NULL, "()");
                   }

                   unsigned long smaps_addr = 0;
                   unsigned long start, end, smaps_size;

                   if (address1_output == 0  && address2_output == 0 && shared_count != 0) {
                       unaccounted_size += size / shared_count;
                   }else if (address2_output == 0) {
                       fseek(smaps_fp, 0, SEEK_SET);

                       while (smaps_addr <= address1_output) {
                          if (fgets(line, sizeof(line), smaps_fp) == NULL) {
                              break;
                          }

                          if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                              smaps_addr = start;
                              continue;
                          }

                          if (smaps_addr != address1_output) {
                              continue;
                          }

                          if (sscanf(line, "Pss: %lu kB", &smaps_size) == 1 && shared_count != 0) {
                              unaccounted_size += (size - smaps_size) / shared_count;
                              break;
                          }
                       }
                   }else {
                       fseek(smaps_fp, 0, SEEK_SET);

                       while (smaps_addr <= address1_output || smaps_addr <= address2_output) {
                          if (fgets(line, sizeof(line), smaps_fp) == NULL) {
                              break;
                          }

                          if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                              smaps_addr = start;
                              continue;
                          }

                          if (smaps_addr != address1_output && smaps_addr != address2_output) {
                              continue;
                          }

                          if (sscanf(line, "Pss: %lu kB", &smaps_size) == 1) {
                              unaccounted_size += size - smaps_size;
                              continue;
                          }
                       }
                   }
              }
           }

           fclose(smaps_fp);
           hashmapForEach(kernel_address_map, free_key, (void*)0);
           hashmapFree(kernel_address_map);
        }
    }

    records[0].size_in_bytes = unaccounted_size * 1024;

    fclose(fp);

    return 0;
}
