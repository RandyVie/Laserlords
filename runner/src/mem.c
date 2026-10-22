/* Emulated memory and the allocators behind F$SRqMem / F$SRqCMem / modules. */
#include "ll.h"
#include <stdlib.h>
#include <string.h>

uint8_t *mem;

/* First-fit free list per region, blocks rounded to 16 bytes. */
typedef struct Block { uint32_t addr, size; struct Block *next; } Block;
typedef struct { Block *free; uint32_t base, end; } Region;

static Region heap, cmem[2], modarea;

static void region_init(Region *r, uint32_t base, uint32_t end)
{
    r->base = base; r->end = end;
    r->free = malloc(sizeof(Block));
    *r->free = (Block){ base, end - base, NULL };
}

static uint32_t region_alloc(Region *r, uint32_t size)
{
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    for (Block **pp = &r->free; *pp; pp = &(*pp)->next) {
        Block *b = *pp;
        if (b->size < size) continue;
        uint32_t a = b->addr;
        b->addr += size; b->size -= size;
        if (!b->size) { *pp = b->next; free(b); }
        memset(host(a), 0, size);
        return a;
    }
    return 0;
}

static void region_free(Region *r, uint32_t addr, uint32_t size)
{
    size = (size + 15) & ~15u;
    for (Block *f = r->free; f; f = f->next)
        if (addr < f->addr + f->size && f->addr < addr + size) {
            ll_log("        !! free of %06x+%x overlaps free block %06x+%x (double free?) - ignored\n", addr, size, f->addr, f->size);
            return;
        }
    Block **pp = &r->free;
    while (*pp && (*pp)->addr < addr) pp = &(*pp)->next;
    Block *b = malloc(sizeof(Block));
    *b = (Block){ addr, size, *pp };
    *pp = b;
    /* coalesce with the following and the preceding block */
    if (b->next && b->addr + b->size == b->next->addr) {
        Block *n = b->next; b->size += n->size; b->next = n->next; free(n);
    }
    for (Block *p = r->free; p && p != b; p = p->next)
        if (p->next == b && p->addr + p->size == b->addr) {
            p->size += b->size; p->next = b->next; free(b); break;
        }
}

void mem_init(void)
{
    mem = calloc(1, MEM_SIZE + 16);  /* +16: unaligned 32-bit reads at the top stay in bounds */
    region_init(&heap, HEAP_BASE, HEAP_END);
    region_init(&cmem[0], CMEM_A_BASE, CMEM_A_BASE + CMEM_SIZE);
    region_init(&cmem[1], CMEM_B_BASE, CMEM_B_BASE + CMEM_SIZE);
    region_init(&modarea, MODULE_BASE, MODULE_END);
}

uint32_t heap_alloc(uint32_t size)            { return region_alloc(&heap, size); }

/* Return memory to whichever region owns the address (general heap or a video bank). */
void heap_free(uint32_t addr, uint32_t sz)
{
    Region *r = addr >= CMEM_B_BASE ? &cmem[1] : addr >= CMEM_A_BASE ? &cmem[0] : &heap;
    if (addr < r->base || addr + sz > r->end) { ll_log("        !! free of %06x+%x outside any region - ignored\n", addr, sz); return; }
    region_free(r, addr, sz);
}
uint32_t modarea_alloc(uint32_t size)         { return region_alloc(&modarea, size); }
void     modarea_free(uint32_t addr, uint32_t sz) { region_free(&modarea, addr, sz); }

uint32_t cmem_alloc(int color, uint32_t size)
{
    /* colour 1 = plane A bank, 2 = plane B bank (verify); anything else falls back to the heap */
    if (color == 1 || color == 2) return region_alloc(&cmem[color - 1], size);
    return heap_alloc(size);
}

size_t mem_strlen(uint32_t a, size_t max)
{
    size_t n = 0;
    while (n < max && rd8(a + n)) n++;
    return n;
}

void mem_cstr(uint32_t a, char *out, size_t max)
{
    size_t n = mem_strlen(a, max - 1);
    for (size_t i = 0; i < n; i++) out[i] = (char)rd8(a + i);
    out[n] = 0;
}
