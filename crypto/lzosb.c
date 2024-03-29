/*
 * Cryptographic API.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/lzo.h>

struct lzo_ctx {
	void *lzo_comp_mem;
};

/*
 * If the src is full of zeros, compress to a null byte and return true.
 * Otherwise, return false and do nothing.
 */
static bool zero_compress(
        const u8 *src,
        unsigned int slen,
        u8 *dst,
        size_t *dlen)
{
    // Check for all zeros
    int i;
    for(i = 0; i < slen; i++) {
        if (src[i] != 0) {
            return false;
        }
    }

    // compress
    *dst = 0;
    *dlen = 1;

    return true;
}

/*
 * If the src is a single null byte, fill the entire output buffer with null
 * bytes and return true. Otherwise, return false and do nothing.
 *
 * This doesn't seem to be documented anywhere else, so I am writing it here:
 *
 * `dlen` should contain the length of the output buffer when it is passed to
 * the compressor, and after a successful call to the compressor, it will
 * contain the actual compressed length.
 */
static bool zero_decompress(
        const u8 *src,
        unsigned int slen,
        u8 *dst,
        size_t *dlen)
{
    if (slen > 1) {
        return false;
    }

    if (*src != 0) {
        return false;
    }

    // optimized decompress
    memset(dst, 0, *dlen);

    return true;
}

static int lzo_init(struct crypto_tfm *tfm)
{
	struct lzo_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->lzo_comp_mem = kmalloc(LZO1X_MEM_COMPRESS,
				    GFP_KERNEL | __GFP_NOWARN | __GFP_REPEAT);
	if (!ctx->lzo_comp_mem)
		ctx->lzo_comp_mem = vmalloc(LZO1X_MEM_COMPRESS);
	if (!ctx->lzo_comp_mem)
		return -ENOMEM;

	return 0;
}

static void lzo_exit(struct crypto_tfm *tfm)
{
	struct lzo_ctx *ctx = crypto_tfm_ctx(tfm);

	kvfree(ctx->lzo_comp_mem);
}

static int lzo_compress(struct crypto_tfm *tfm, const u8 *src,
			    unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lzo_ctx *ctx = crypto_tfm_ctx(tfm);
	size_t tmp_len = *dlen; /* size_t(ulong) <-> uint on 64 bit */
	int err;
    bool zc = zero_compress(src, slen, dst, &tmp_len);

    // Only do lzo if zero-compress failed
    if (!zc) {
        err = lzo1x_1_compress(src, slen, dst, &tmp_len, ctx->lzo_comp_mem);

        if (err != LZO_E_OK)
            return -EINVAL;
    }

	*dlen = tmp_len;
	return 0;
}

static int lzo_decompress(struct crypto_tfm *tfm, const u8 *src,
			      unsigned int slen, u8 *dst, unsigned int *dlen)
{
	int err;
	size_t tmp_len = (size_t)*dlen; /* size_t(ulong) <-> uint on 64 bit */
    bool zd = zero_decompress(src, slen, dst, &tmp_len);

    // Only do lzo if zero-decompress failed
    if (!zd) {
        err = lzo1x_decompress_safe(src, slen, dst, &tmp_len);

        if (err != LZO_E_OK)
            return -EINVAL;
    }

	*dlen = tmp_len;
	return 0;

}

static struct crypto_alg alg = {
	.cra_name		= "lzosb",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct lzo_ctx),
	.cra_module		= THIS_MODULE,
	.cra_init		= lzo_init,
	.cra_exit		= lzo_exit,
	.cra_u			= { .compress = {
	.coa_compress 		= lzo_compress,
	.coa_decompress  	= lzo_decompress } }
};

static int __init lzo_mod_init(void)
{
	return crypto_register_alg(&alg);
}

static void __exit lzo_mod_fini(void)
{
	crypto_unregister_alg(&alg);
}

module_init(lzo_mod_init);
module_exit(lzo_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LZO Compression Algorithm with Single-byte optimization");
MODULE_ALIAS_CRYPTO("lzosb");
