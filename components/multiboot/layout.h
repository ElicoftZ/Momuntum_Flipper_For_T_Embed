#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_TABLE_ADDRESS 0x8000U
#define MB_BACKUP_ADDRESS 0x12000U
#define MB_PENDING_ADDRESS 0x13000U
#define MB_SYSTEM_COUNT 8U
#define MB_SECTOR_SIZE 0x1000U
#define MB_POOL_START 0x420000U
#define MB_POOL_END 0xf20000U
#define MB_ALIGNMENT 0x10000U
#define MB_MAX_APPS 16U
#define MB_MAX_ENTRIES 95U

typedef struct {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    char label[16];
    uint32_t flags;
} MbEntry;

bool mb_layout_valid(const MbEntry* entries, size_t count);
bool mb_layout_add(MbEntry* entries, size_t* count, uint32_t image_size, MbEntry* added);
bool mb_layout_remove(MbEntry* entries, size_t* count, uint32_t address);
uint32_t mb_layout_largest_gap(const MbEntry* entries, size_t count);
