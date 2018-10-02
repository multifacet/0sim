
#include <linux/gfp.h>
#include <linux/radix_bitmap.h>
#include <linux/vmalloc.h>

///////////////////////////////////////////////////////////////////////////////
// Various helpers
///////////////////////////////////////////////////////////////////////////////

#define L0_MASK GENMASK(L0_ORDER + L1_ORDER - 1, L1_ORDER)
#define L1_MASK GENMASK(L1_ORDER - 1, 0)

#define L0_SIZE (1 << (L0_ORDER + 3))
#define L1_SIZE (1 << (L1_ORDER - 3))

/*
 * Deallocate the given l0 map.
 */
void destroy_radix_bitmap_l0(struct radix_bitmap_l0 *l0) {
    vfree((void *)l0);
}

/*
 * Deallocate the given l1 map.
 */
void destroy_radix_bitmap_l1(struct radix_bitmap_l1 *l1) {
    vfree((void *)l1);
}

///////////////////////////////////////////////////////////////////////////////
// Implement the API
///////////////////////////////////////////////////////////////////////////////

/*
 * Allocate and initialize a new empty l0 map.
 */
struct radix_bitmap_l0 *mk_radix_bitmap_l0(gfp_t gfp) {
    void *pages = __vmalloc(L0_SIZE,
                            gfp | GFP_ATOMIC | __GFP_ZERO,
                            PAGE_KERNEL);
    if (!pages) {
        return NULL;
    }
    //printk(KERN_ERR "l0: %p %lx\n", pages, (unsigned long)L0_SIZE);
    return (struct radix_bitmap_l0 *)pages;
}

/*
 * Allocate and initialize a new empty l1 map.
 */
struct radix_bitmap_l1 *mk_radix_bitmap_l1(gfp_t gfp) {
    void *pages = __vmalloc(L1_SIZE,
                            gfp | GFP_ATOMIC | __GFP_ZERO,
                            PAGE_KERNEL);
    if (!pages) {
        return NULL;
    }
    //printk(KERN_ERR "l1: %p %lx\n", pages, (unsigned long)L1_SIZE);
    return (struct radix_bitmap_l1 *)pages;
}

/*
 * Initialize the given radix bitmap struct to a valid empty bitmap
 * using the given L0 bitmap.
 */
void radix_bitmap_init(struct radix_bitmap *rb, struct radix_bitmap_l0 *l0) {
    BUG_ON(!l0);
    rb->l0 = l0;
    rb->size = L0_SIZE;
}

/*
 * Returns 1 iff the given bitmap is initialized.
 */
bool radix_bitmap_is_init(struct radix_bitmap *rb) {
    return rb->size > 0;
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
    unsigned long l0_idx = (idx & L0_MASK) >> L1_ORDER;
    unsigned long l1_idx = idx & L1_MASK;
    struct radix_bitmap_l1 *l1;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    BUG_ON(idx >= (1ul << 48));

    // Access the l0-th map entry
    l1 = rb->l0->map[l0_idx];
    //printk(KERN_ERR "GET %lx %lx %lx %p\n", idx, l0_idx, l1_idx, l1);

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
 *
 * If the L1 bitmap containing the given bit does not exist and no
 * new_l1 bitmap has been passed, then fail.
 *
 * Returns 0 on success and -ENOMEM on failure (which indicates no L1).
 */
int radix_bitmap_set(struct radix_bitmap *rb, unsigned long idx,
                     struct radix_bitmap_l1 *new_l1) {
    // Get the l0 and l1 offset
    unsigned long l0_idx = (idx & L0_MASK) >> L1_ORDER;
    unsigned long l1_idx = idx & L1_MASK;
    struct radix_bitmap_l1 *l1;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    BUG_ON(idx >= (1ul << 48));

    // Access the l0-th map entry
    l1 = rb->l0->map[l0_idx];
    //printk(KERN_ERR "SET %lx %lx %lx %p\n", idx, l0_idx, l1_idx, l1);

    // Is there an entry? If not, create one.
    if (!l1) {
        if (!new_l1) {
            return -ENOMEM;
        }

        l1 = rb->l0->map[l0_idx] = new_l1;
        rb->size += L1_SIZE;
    }

    // Set the appropriate bit. First 24 bits are which byte in bitmap.
    // Last 3 bits (mask=0x7) are idx of bit in char.
    l1->bits[l1_idx >> 3] |= BIT(l1_idx & 7);

    return 0;
}

/*
 * Unset the given bit.
 */
void radix_bitmap_unset(struct radix_bitmap *rb, unsigned long idx) {
    // Get the l0 and l1 offset
    unsigned long l0_idx = (idx & L0_MASK) >> L1_ORDER;
    unsigned long l1_idx = idx & L1_MASK;
    struct radix_bitmap_l1 *l1;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    BUG_ON(idx >= (1ul << 48));

    // Access the l0-th map entry
    l1 = rb->l0->map[l0_idx];
    //printk(KERN_ERR "UNSET %lx %lx %lx %p\n", idx, l0_idx, l1_idx, l1);

    // Is there an entry? If not, we're done.
    if (!l1) {
        return;
    }

    // Unset the appropriate bit. First 24 bits are which byte in bitmap.
    // Last 3 bits (mask=0x7) are idx of bit in char.
    l1->bits[l1_idx >> 3] &= ~BIT(l1_idx & 7);
}

/*
 * Clear the entire bitmap.
 */
void radix_bitmap_clear(struct radix_bitmap *rb) {
    int i;

    BUG_ON(!rb);
    BUG_ON(!rb->l0);

    // For each entry in the l0, free the corresponding l1 bitmap if there is one.
    for (i = 0; i < (1 << L0_ORDER); i++) {
        if (rb->l0->map[i]) {
            destroy_radix_bitmap_l1(rb->l0->map[i]);
            rb->l0->map[i] = NULL;
        }
    }

    rb->size = L0_SIZE;
}
