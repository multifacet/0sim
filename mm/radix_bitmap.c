
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/radix_bitmap.h>

///////////////////////////////////////////////////////////////////////////////
// Various helpers
///////////////////////////////////////////////////////////////////////////////

#define L0_MASK GENMASK(L0_ORDER + L1_ORDER - 1, L1_ORDER)
#define L1_MASK GENMASK(L1_ORDER - 1, 0)

#define L0_SIZE (L0_ORDER + 3 - PAGE_SHIFT)
#define L1_SIZE (L1_ORDER - 3 - PAGE_SHIFT)

/*
 * Allocate and initialize a new empty l0 map.
 */
struct radix_bitmap_l0 *mk_radix_bitmap_l0(gfp_t gfp) {
    struct page *pages = alloc_pages(gfp | __GFP_ZERO, L0_SIZE);
    void *raw_pages = page_address(pages);
    return (struct radix_bitmap_l0 *)raw_pages;
}

/*
 * Allocate and initialize a new empty l1 map.
 */
struct radix_bitmap_l1 *mk_radix_bitmap_l1(gfp_t gfp) {
    struct page *pages = alloc_pages(gfp | __GFP_ZERO, L1_SIZE);
    void *raw_pages = page_address(pages);
    return (struct radix_bitmap_l1 *)raw_pages;
}

/*
 * Deallocate the given l0 map.
 */
void destroy_radix_bitmap_l0(struct radix_bitmap_l0 *l0) {
    free_pages((unsigned long)l0, L0_SIZE);
}

/*
 * Deallocate the given l1 map.
 */
void destroy_radix_bitmap_l1(struct radix_bitmap_l1 *l1) {
    free_pages((unsigned long)l1, L1_SIZE);
}

///////////////////////////////////////////////////////////////////////////////
// Implement the API
///////////////////////////////////////////////////////////////////////////////

/*
 * Initialize the given radix bitmap struct to a valid empty bitmap.
 */
void radix_bitmap_create(struct radix_bitmap *rb, gfp_t gfp) {
    rb->l0 = mk_radix_bitmap_l0(gfp);
}

/*
 * Destroy the given bitmap and free all of its memory.
 */
void radix_bitmap_destroy(struct radix_bitmap *rb) {
    int i;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    // For each entry in the l0, free the corresponding l1 bitmap if there is one.
    for (i = 0; i < (1 << L0_ORDER); i++) {
        if (rb->l0->map[i]) {
            destroy_radix_bitmap_l1(rb->l0->map[i]);
        }
    }

    // Then, free the l0 map itself
    destroy_radix_bitmap_l0(rb->l0);
}

/*
 * Returns the bit.
 */
bool radix_bitmap_get(struct radix_bitmap *rb, unsigned long idx) {
    // Get the l0 and l1 offset
    unsigned long l0_idx = idx & L0_MASK;
    unsigned long l1_idx = idx & L1_MASK;
    struct radix_bitmap_l1 *l1;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    BUG_ON(idx >= (1ul << 48));

    // Access the l0-th map entry
    l1 = rb->l0->map[l0_idx];

    // Is there an entry? If not, the bit is unset.
    if (!l1) {
        return 0;
    }

    // Otherwise, check the l1 map.  First 24 bits are which byte in bitmap.
    // Last 3 bits (mask=0x7) are idx of bit in char.
    return l1->bits[l1_idx >> 3] & BIT(l1_idx & 7);
}

/*
 * Set the given bit.
 */
void radix_bitmap_set(struct radix_bitmap *rb, unsigned long idx, gfp_t gfp) {
    // Get the l0 and l1 offset
    unsigned long l0_idx = idx & L0_MASK;
    unsigned long l1_idx = idx & L1_MASK;
    struct radix_bitmap_l1 *l1;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    BUG_ON(idx >= (1ul << 48));

    // Access the l0-th map entry
    l1 = rb->l0->map[l0_idx];

    // Is there an entry? If not, create one.
    if (!l1) {
        l1 = rb->l0->map[l0_idx] = mk_radix_bitmap_l1(gfp);
    }

    // Set the appropriate bit. First 24 bits are which byte in bitmap.
    // Last 3 bits (mask=0x7) are idx of bit in char.
    l1->bits[l1_idx >> 3] |= BIT(l1_idx & 7);
}

/*
 * Unset the given bit.
 */
void radix_bitmap_unset(struct radix_bitmap *rb, unsigned long idx) {
    // Get the l0 and l1 offset
    unsigned long l0_idx = idx & L0_MASK;
    unsigned long l1_idx = idx & L1_MASK;
    struct radix_bitmap_l1 *l1;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    BUG_ON(idx >= (1ul << 48));

    // Access the l0-th map entry
    l1 = rb->l0->map[l0_idx];

    // Is there an entry? If not, we're done.
    if (!l1) {
        return;
    }

    // Unset the appropriate bit. First 24 bits are which byte in bitmap.
    // Last 3 bits (mask=0x7) are idx of bit in char.
    l1->bits[l1_idx >> 3] &= ~BIT(l1_idx & 7);
}
