#include "layout.h"
#include <string.h>
#include <stdio.h>

_Static_assert(sizeof(MbEntry) == 32, "ESP partition entry size");

static const MbEntry system_entries[] = {
    {0x50aa, 1, 2, 0x9000, 0x6000, "nvs", 0},
    {0x50aa, 1, 1, 0xf000, 0x1000, "phy_init", 0},
    {0x50aa, 1, 0, 0x10000, 0x2000, "otadata", 0},
    {0x50aa, 1, 0x40, MB_BACKUP_ADDRESS, MB_SECTOR_SIZE, "partbackup", 0},
    {0x50aa, 1, 0x40, MB_PENDING_ADDRESS, MB_SECTOR_SIZE, "partpending", 0},
    {0x50aa, 0, 0, 0x20000, 0x500000, "factory", 0},
    {0x50aa, 0, 0x10, 0x520000, 0x400000, "otaupd", 0},
    {0x50aa, 1, 3, MB_POOL_END, 0x20000, "coredump", 0},
    {0x50aa, 1, 0x82, 0xf40000, 0xc0000, "spiffs", 0},
};

bool mb_layout_valid(const MbEntry* e, size_t count) {
    if(!e || count < MB_SYSTEM_COUNT || count > MB_SYSTEM_COUNT + MB_MAX_APPS) return false;
    unsigned systems = 0, apps = 0, subtype_mask = 0;
    uint32_t end = 0;
    for(size_t i = 0; i < count; ++i) {
        if(e[i].magic != 0x50aa || !e[i].size || e[i].offset < end ||
           e[i].offset > 0x1000000 || e[i].size > 0x1000000 - e[i].offset) return false;
        end = e[i].offset + e[i].size;
        bool system = false;
        for(size_t j = 0; j < sizeof(system_entries) / sizeof(system_entries[0]); ++j) {
            if(memcmp(&e[i], &system_entries[j], sizeof(MbEntry)) == 0) {
                systems |= 1U << j;
                system = true;
                break;
            }
        }
        if(system) continue;
        if(e[i].type != 0 || e[i].subtype < 0x11 || e[i].subtype > 0x1f ||
           e[i].flags || e[i].offset < MB_POOL_START || end > MB_POOL_END ||
           e[i].offset % MB_ALIGNMENT || e[i].size % MB_ALIGNMENT ||
           !memchr(e[i].label, 0, sizeof(e[i].label))) return false;
        const unsigned bit = 1U << (e[i].subtype - 0x11);
        if(subtype_mask & bit) return false;
        subtype_mask |= bit;
        ++apps;
    }
    return systems == 0x1ff && subtype_mask == ((1U << apps) - 1U);
}

uint32_t mb_layout_largest_gap(const MbEntry* e, size_t count) {
    if(!mb_layout_valid(e, count)) return 0;
    uint32_t next = MB_POOL_START, largest = 0;
    for(size_t i = 0; i < count; ++i) {
        if(e[i].offset < MB_POOL_START) continue;
        if(e[i].offset > next && e[i].offset - next > largest) largest = e[i].offset - next;
        if(e[i].offset >= MB_POOL_END) break;
        next = e[i].offset + e[i].size;
    }
    return largest;
}

bool mb_layout_add(MbEntry* e, size_t* count, uint32_t image_size, MbEntry* added) {
    if(!count || !added || !mb_layout_valid(e, *count) || *count >= MB_SYSTEM_COUNT + MB_MAX_APPS ||
       !image_size || image_size > MB_POOL_END - MB_POOL_START) return false;
    const uint32_t size = (image_size + MB_ALIGNMENT - 1U) & ~(MB_ALIGNMENT - 1U);
    uint32_t next = MB_POOL_START;
    for(size_t i = 0; i < *count; ++i) {
        if(e[i].offset < MB_POOL_START) continue;
        if(e[i].offset - next >= size) {
            MbEntry entry = {0x50aa, 0, (uint8_t)(0x11 + *count - MB_SYSTEM_COUNT), next, size, {0}, 0};
            snprintf(entry.label, sizeof(entry.label), "fw%u", entry.subtype - 0x11);
            memmove(&e[i + 1], &e[i], (*count - i) * sizeof(*e));
            e[i] = entry;
            ++*count;
            *added = entry;
            return true;
        }
        if(e[i].offset >= MB_POOL_END) break;
        next = e[i].offset + e[i].size;
    }
    return false;
}

bool mb_layout_remove(MbEntry* e, size_t* count, uint32_t address) {
    if(!count || !mb_layout_valid(e, *count)) return false;
    for(size_t i = 0; i < *count; ++i) {
        /* The offset gate keeps the fixed otaupd slot (also type 0, subtype
         * 0x10) out of reach: only pool images may be removed. */
        if(e[i].offset != address || e[i].type != 0 || e[i].subtype < 0x11 ||
           e[i].offset < MB_POOL_START) continue;
        memmove(&e[i], &e[i + 1], (*count - i - 1) * sizeof(*e));
        --*count;
        unsigned subtype = 0x11;
        for(size_t j = 0; j < *count; ++j) {
            if(e[j].type == 0 && e[j].offset >= MB_POOL_START) {
                e[j].subtype = (uint8_t)subtype++;
                snprintf(e[j].label, sizeof(e[j].label), "fw%u", e[j].subtype - 0x11);
            }
        }
        return true;
    }
    return false;
}
