#ifndef __RADIX_BITMAP_H__
#define __RADIX_BITMAP_H__

#include <linux/types.h>

/*
 * A sparse bitmap implementation, structured as a radix tree of two levels
 * with a bitmap in the second level.
 *
 * The implementation assumes a 64-bit virtual address space and 4KB pages.
 *
 * The top level is 16MB, and each bitmap leaf in the tree is 16MB. Altogether,
 * the both levels allow for 2^48 bits total.
 *
 * The use case for this is the zswap bitmap to map keep track of zero pages
 * efficiently. A single radix-bitmap can track up to a 2^60B (2^48 * 2^12)
 * sized address space, which should be sufficient for now. When given a virtual
 * address of a page for which we want to set the bit, the top 4 bits are
 * ignored, which is ok because they are not used (currently).
 *
 * As a first implementation, we make no attempt to reclaim empty bitmaps.
 */

#define L0_ORDER 21
#define L1_ORDER 27

struct radix_bitmap_l1 {
    char bits[1 << (L1_ORDER - 3)];
} __attribute__((packed));

struct radix_bitmap_l0 {
    struct radix_bitmap_l1 *map[1 << (L0_ORDER + 3)];
} __attribute__((packed));

struct radix_bitmap {
    struct radix_bitmap_l0 *l0;
} __attribute__((packed));

/*
 * Initialize the given radix bitmap struct to a valid empty bitmap.
 */
void radix_bitmap_create(struct radix_bitmap *rb);

/*
 * Destroy the given bitmap and free all of its memory.
 */
void radix_bitmap_destroy(struct radix_bitmap *rb);

/*
 * Returns the bit.
 */
bool radix_bitmap_get(struct radix_bitmap *rb, unsigned long idx);

/*
 * Set the given bit.
 */
void radix_bitmap_set(struct radix_bitmap *rb, unsigned long idx);

/*
 * Unset the given bit.
 */
void radix_bitmap_unset(struct radix_bitmap *rb, unsigned long idx);

#endif
