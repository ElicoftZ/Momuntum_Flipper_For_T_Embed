#include "layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    MbEntry table[MB_MAX_ENTRIES], initial[MB_MAX_ENTRIES], before[MB_MAX_ENTRIES];
    FILE* f = fopen(argv[1], "rb");
    assert(f);
    memset(initial, 0, sizeof(initial));
    assert(fread(initial, sizeof(MbEntry), MB_SYSTEM_COUNT, f) == MB_SYSTEM_COUNT);
    fclose(f);
    memcpy(table, initial, sizeof(table));
    size_t count = MB_SYSTEM_COUNT;
    MbEntry added;
    assert(mb_layout_valid(table, count));
    assert(mb_layout_largest_gap(table, count) == 11U * 1024U * 1024U);
    assert(!mb_layout_add(table, &count, 0, &added));
    assert(!mb_layout_add(table, &count, UINT32_MAX, &added));
    assert(mb_layout_add(table, &count, 1, &added) && added.size == MB_ALIGNMENT);
    const uint32_t first = added.offset;
    assert(mb_layout_add(table, &count, 2 * MB_ALIGNMENT + 1, &added));
    assert(added.size == 3 * MB_ALIGNMENT);
    const uint32_t second = added.offset;
    assert(mb_layout_remove(table, &count, first));
    assert(mb_layout_add(table, &count, MB_ALIGNMENT, &added) && added.offset == first);
    assert(mb_layout_remove(table, &count, second));
    assert(mb_layout_valid(table, count));
    assert(!mb_layout_remove(table, &count, 0x20000));
    assert(!mb_layout_remove(table, &count, 0x9000));
    memcpy(table, initial, sizeof(table)); count = MB_SYSTEM_COUNT;
    assert(mb_layout_add(table, &count, MB_POOL_END - MB_POOL_START, &added));
    assert(!mb_layout_add(table, &count, 1, &added));
    assert(mb_layout_largest_gap(table, count) == 0);
    assert(mb_layout_remove(table, &count, MB_POOL_START));
    assert(mb_layout_largest_gap(table, count) == MB_POOL_END - MB_POOL_START);
    for(unsigned i = 0; i < MB_MAX_APPS; ++i) assert(mb_layout_add(table, &count, 1, &added));
    assert(!mb_layout_add(table, &count, 1, &added));
    memcpy(table, initial, sizeof(table)); count = MB_SYSTEM_COUNT;
    table[5].size -= MB_ALIGNMENT;
    assert(!mb_layout_valid(table, count));
    assert(!mb_layout_add(table, &count, 1, &added));
    assert(!mb_layout_remove(table, &count, MB_POOL_START));

    /* Variable-size install/remove histories must never move or overlap any
     * surviving image, even with fragmented free space and subtype renumbering. */
    memcpy(table, initial, sizeof(table)); count = MB_SYSTEM_COUNT;
    srand(731);
    for(unsigned run = 0; run < 20000; ++run) {
        memcpy(before, table, sizeof(table));
        const size_t old_count = count;
        uint32_t removed = 0;
        if(count > MB_SYSTEM_COUNT && rand() % 3 == 0) {
            size_t index = 6 + (size_t)rand() % (count - MB_SYSTEM_COUNT);
            removed = table[index].offset;
            assert(mb_layout_remove(table, &count, removed));
        } else {
            const uint32_t size = 1U + (uint32_t)rand() % (4U * 1024U * 1024U);
            if(!mb_layout_add(table, &count, size, &added)) {
                assert(count == old_count && memcmp(before, table, sizeof(table)) == 0);
            }
        }
        assert(mb_layout_valid(table, count));
        for(size_t i = 0; i < old_count; ++i) {
            if(before[i].offset == removed) continue;
            bool found = false;
            for(size_t j = 0; j < count; ++j)
                if(before[i].offset == table[j].offset && before[i].size == table[j].size) found = true;
            assert(found);
        }
    }
    puts("PASS: dynamic sizing, gap reuse, full flash, 16-slot limit, protected system entries, 20,000 fragmented install/remove histories");
    return 0;
}
