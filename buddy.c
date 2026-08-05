#include "buddy.h"
#include <stdlib.h>
#include <string.h>

#define NULL ((void *)0)
#define MAXRANK 16
#define PAGESIZE 4096

static void *base_addr = NULL;
static int total_pages = 0;

/* For each page: 0 if free, rank (1..16) if allocated (first page or continuation) */
static int *alloc_info = NULL;

/* Free-list bitmaps for each rank 1..16.
   Bit j in free_bitmap[r] is set iff the block at page offset j*(2^(r-1)) is free at rank r. */
static unsigned long *free_bitmap[MAXRANK + 1];
static int free_count_arr[MAXRANK + 1];
static int bitmap_words[MAXRANK + 1];
static int num_blocks[MAXRANK + 1];

static void bm_set(unsigned long *bm, int idx) {
    bm[idx >> 6] |= (1UL << (idx & 63));
}

static void bm_clear(unsigned long *bm, int idx) {
    bm[idx >> 6] &= ~(1UL << (idx & 63));
}

static int bm_test(unsigned long *bm, int idx) {
    return (int)((bm[idx >> 6] >> (idx & 63)) & 1UL);
}

static int bm_first_set(unsigned long *bm, int words) {
    int i;
    for (i = 0; i < words; i++) {
        if (bm[i])
            return (i << 6) + __builtin_ctzl(bm[i]);
    }
    return -1;
}

static int addr_to_page(void *p) {
    unsigned long addr, base, offset;
    int page;
    if (p == NULL) return -1;
    addr = (unsigned long)p;
    base = (unsigned long)base_addr;
    if (addr < base) return -1;
    offset = addr - base;
    if (offset % PAGESIZE != 0) return -1;
    page = (int)(offset / PAGESIZE);
    if (page < 0 || page >= total_pages) return -1;
    return page;
}

int init_page(void *p, int pgcount) {
    int r;

    /* Free old data if re-initialized */
    if (alloc_info) { free(alloc_info); alloc_info = NULL; }
    for (r = 1; r <= MAXRANK; r++) {
        if (free_bitmap[r]) { free(free_bitmap[r]); free_bitmap[r] = NULL; }
        free_count_arr[r] = 0;
        bitmap_words[r] = 0;
        num_blocks[r] = 0;
    }

    base_addr = p;
    total_pages = pgcount;

    if (pgcount <= 0) return OK;

    alloc_info = (int *)malloc((size_t)pgcount * sizeof(int));
    if (!alloc_info) return -12;
    memset(alloc_info, 0, (size_t)pgcount * sizeof(int));

    for (r = 1; r <= MAXRANK; r++) {
        int nb = pgcount >> (r - 1);
        num_blocks[r] = nb;
        if (nb <= 0) {
            free_bitmap[r] = NULL;
            bitmap_words[r] = 0;
            free_count_arr[r] = 0;
        } else {
            bitmap_words[r] = (nb + 63) / 64;
            free_bitmap[r] = (unsigned long *)calloc((size_t)bitmap_words[r],
                                                     sizeof(unsigned long));
            free_count_arr[r] = 0;
        }
    }

    /* Decompose pgcount into the largest possible aligned buddy blocks */
    {
        int remaining = pgcount;
        int offset = 0;
        while (remaining > 0) {
            int k = 0;
            int rank, pages, block_idx;
            while (k < 15 &&
                   (1 << (k + 1)) <= remaining &&
                   (offset % (1 << (k + 1))) == 0) {
                k++;
            }
            rank = k + 1;
            pages = 1 << k;
            block_idx = offset >> (rank - 1);
            bm_set(free_bitmap[rank], block_idx);
            free_count_arr[rank]++;
            offset += pages;
            remaining -= pages;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    int found_rank;
    int block_idx, page_offset;
    int alloc_size, i;

    if (rank < 1 || rank > MAXRANK)
        return ERR_PTR(-EINVAL);

    /* Find the smallest rank >= requested rank that has a free block */
    for (found_rank = rank; found_rank <= MAXRANK; found_rank++) {
        if (free_count_arr[found_rank] > 0)
            break;
    }
    if (found_rank > MAXRANK)
        return ERR_PTR(-ENOSPC);

    /* Take the lowest-address free block at found_rank */
    block_idx = bm_first_set(free_bitmap[found_rank], bitmap_words[found_rank]);
    page_offset = block_idx << (found_rank - 1);

    bm_clear(free_bitmap[found_rank], block_idx);
    free_count_arr[found_rank]--;

    /* Split down to the requested rank, putting the upper buddy into the free list */
    while (found_rank > rank) {
        found_rank--;
        {
            int buddy_offset = page_offset + (1 << (found_rank - 1));
            int buddy_block_idx = buddy_offset >> (found_rank - 1);
            bm_set(free_bitmap[found_rank], buddy_block_idx);
            free_count_arr[found_rank]++;
        }
    }

    /* Mark the allocated block */
    alloc_size = 1 << (rank - 1);
    for (i = 0; i < alloc_size; i++)
        alloc_info[page_offset + i] = rank;

    return (void *)((unsigned long)base_addr +
                    (unsigned long)page_offset * PAGESIZE);
}

int return_pages(void *p) {
    int page_idx, rank, block_size, i;
    int block_idx;

    page_idx = addr_to_page(p);
    if (page_idx < 0) return -EINVAL;

    if (alloc_info[page_idx] == 0) return -EINVAL;

    rank = alloc_info[page_idx];
    block_size = 1 << (rank - 1);

    /* Must be the start of the block (aligned) */
    if (page_idx % block_size != 0) return -EINVAL;

    /* Clear allocation info for all pages in the block */
    for (i = 0; i < block_size; i++)
        alloc_info[page_idx + i] = 0;

    /* Add to free list */
    block_idx = page_idx >> (rank - 1);
    bm_set(free_bitmap[rank], block_idx);
    free_count_arr[rank]++;

    /* Merge with buddies as far as possible */
    while (rank < MAXRANK) {
        int buddy_block_idx = block_idx ^ 1;
        if (buddy_block_idx >= num_blocks[rank]) break;
        if (!bm_test(free_bitmap[rank], buddy_block_idx)) break;

        bm_clear(free_bitmap[rank], block_idx);
        bm_clear(free_bitmap[rank], buddy_block_idx);
        free_count_arr[rank] -= 2;

        rank++;
        block_idx = block_idx >> 1;
        bm_set(free_bitmap[rank], block_idx);
        free_count_arr[rank]++;
    }

    return OK;
}

int query_ranks(void *p) {
    int page_idx, r;

    page_idx = addr_to_page(p);
    if (page_idx < 0) return -EINVAL;

    /* Allocated page: return its allocation rank */
    if (alloc_info[page_idx] > 0)
        return alloc_info[page_idx];

    /* Free page: find the largest free block containing this page */
    for (r = MAXRANK; r >= 1; r--) {
        if (num_blocks[r] == 0) continue;
        {
            int bidx = page_idx >> (r - 1);
            if (bidx >= num_blocks[r]) continue;
            if (bm_test(free_bitmap[r], bidx))
                return r;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAXRANK) return -EINVAL;
    return free_count_arr[rank];
}
