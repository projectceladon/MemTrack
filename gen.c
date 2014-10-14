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

static int cmpfn(const void* a, const void* b)
{
    if (*(unsigned long *)a > *(unsigned long *)b) return 1;
    if (*(unsigned long *)a < *(unsigned long *)b) return -1;
    return 0;
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
               char *outptr, *innerptr;
               int shared_count;
               unsigned long address1_output = 0, address2_output = 0;

               if (fgets(line, sizeof(line), fp) == NULL) {
                    break;
               }

               /* Format:
                *   Obj Identifier       Size Pin Tiling Dirty Shared Vmap Stolen Mappable  AllocState Global/PP  GttOffset (PID: handle count: user virt addrs)
                *  ffff88000e998ac0:      16K             Y      N     N      N       N      allocated    G       04ef2000  (12609: 1: 000000004212e000)
                */

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

                   unsigned long user_addresses[16];
                   int user_addresses_num = 0;

                   pch = strtok_r(info, "()", &outptr);
                   while (pch != NULL) {
                       if (strcmp(pch, "  ") > 0) {
                           char addresses_output[1024];
                           ret = sscanf(pch, "%d: %*d: %[^\n]", &matched_pid, addresses_output);
                           if (ret == 1) { /* Handle pattern like: (12383: 1:) */
                               shared_count++;
                           }else {
                             char* pch2;
                             pch2 = strtok_r(addresses_output, " ", &innerptr);
                             while (pch2 != NULL) { /* Handle pattern like:  (12383: 1: 000000004046b000) and (12437: 1: 000000004247f000 0000000042412000*) */
                                 shared_count++;
                                 if (matched_pid == pid) {
                                     if (user_addresses_num <= 15) {
                                         user_addresses[user_addresses_num++] = strtol(pch2, NULL, 16);
                                     }
                                 }
                                 pch2 = strtok_r(NULL, " ", &innerptr);
                             }
                           }
                       }
                       pch = strtok_r(NULL, "()", &outptr);
                   }

                   qsort(user_addresses, user_addresses_num, sizeof(user_addresses[0]), cmpfn);

                   unsigned long smaps_addr = 0;
                   unsigned long start, end, smaps_size;

                   if (user_addresses_num == 0) { /* Handle pattern like: (12383: 1:)  (12437: 1:)  (12609: 1:) */
                       unaccounted_size += size / shared_count;
                   }else { /* Handle pattern like: (12364: 2:)  (12383: 1: 000000004046b000) and (12437: 1: 000000004247f000 0000000042412000*) */
                       fseek(smaps_fp, 0, SEEK_SET);

                       int index = 0;
                       while (fgets(line, sizeof(line), smaps_fp) != NULL && index < user_addresses_num) {
                          if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                              smaps_addr = start;
                              continue;
                          }

                          if (smaps_addr != user_addresses[index]) {
                              continue;
                          }

                          if (sscanf(line, "Pss: %lu kB", &smaps_size) == 1 && shared_count != 0) {
                              unaccounted_size += size / shared_count - smaps_size;
                              index++;
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
