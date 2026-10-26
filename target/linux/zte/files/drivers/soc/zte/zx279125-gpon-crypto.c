// SPDX-License-Identifier: GPL-2.0-only
/* GPON AES key exchange: register and PLOAM layouts from the shipped SDK. */
#include <linux/errno.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "zx279125-pon.h"

static void zx_crypto_update(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

int zx279125_gpon_request_key(struct zx279125_pon *pon)
{
	u8 data[10], key[16];
	unsigned int i, repeat, fragment;
	int ret;

	if (!pon->base_drs || !pon->base_aes)
		return -ENODEV;
	/* Unlike the vendor time/jiffies mixer, use the initialized kernel CSPRNG. */
	if (!rng_is_initialized())
		return -EAGAIN;
	get_random_bytes(key, sizeof(key));
	pon->aes_key_valid = false;
	pon->aes_key_index++;
	for (i = 0; i < 4; i++) {
		u32 word = get_unaligned_be32(key + i * 4);

		/* SDK IDs 0x224+i and 0x17d: upstream and downstream shadow banks. */
		writel(word, pon->base_aes + 0x38 + i * 4);
		writel(word, pon->base_drs + 0xd0 + i * 4);
	}
	zx_crypto_update(pon->base_aes + 0x20, BIT(30), BIT(30));
	for (repeat = 0; repeat < 3; repeat++) {
		for (fragment = 0; fragment < 2; fragment++) {
			data[0] = pon->aes_key_index;
			data[1] = fragment;
			memcpy(data + 2, key + fragment * 8, 8);
			ret = zx279125_gpon_send_ploam(pon, 5, data, 1);
			if (ret)
				goto out;
		}
	}
	memcpy(pon->aes_shadow_key, key, sizeof(key));
	pon->aes_key_valid = true;
	ret = 0;
out:
	memzero_explicit(key, sizeof(key));
	memzero_explicit(data, sizeof(data));
	return ret;
}

int zx279125_gpon_encrypt_port(struct zx279125_pon *pon, const u8 data[10])
{
	u16 port = (u16)data[1] << 4 | data[2] >> 4;
	unsigned long flags;

	/* SDK EncrptPortIdMsg: bit1 selects GEM, bit0 requests AES. */
	if (!(data[0] & BIT(1)))
		return -EOPNOTSUPP;
	spin_lock_irqsave(&pon->lock, flags);
	zx_crypto_update(pon->base_ports + port * 4, BIT(1),
			 data[0] & BIT(0) ? BIT(1) : 0);
	spin_unlock_irqrestore(&pon->lock, flags);
	return 0;
}

int zx279125_gpon_switch_key(struct zx279125_pon *pon, const u8 data[10])
{
	u32 frame = get_unaligned_be32(data) & GENMASK(29, 0);
	unsigned long flags;

	if (!pon->aes_key_valid)
		return -ENOKEY;
	spin_lock_irqsave(&pon->lock, flags);
	/* SDK IDs 0x21c/0x21e and 0x180/0x17f arm both switching engines. */
	zx_crypto_update(pon->base_aes + 0x20, GENMASK(29, 0), frame);
	zx_crypto_update(pon->base_aes + 0x24, BIT(31), BIT(31));
	zx_crypto_update(pon->base_drs + 0xe4, GENMASK(30, 0), frame);
	zx_crypto_update(pon->base_drs + 0xe4, BIT(30), BIT(30));
	spin_unlock_irqrestore(&pon->lock, flags);
	return 0;
}

void zx279125_gpon_crypto_reset(struct zx279125_pon *pon)
{
	unsigned int port;

	/* Caller owns config_lock and has already held upstream TX. */
	pon->aes_key_valid = false;
	memzero_explicit(pon->aes_shadow_key, sizeof(pon->aes_shadow_key));
	zx_crypto_update(pon->base_aes + 0x20, BIT(30), 0);
	zx_crypto_update(pon->base_aes + 0x24, BIT(31), 0);
	zx_crypto_update(pon->base_drs + 0xe4, BIT(30), 0);
	for (port = 0; port < 4; port++) {
		writel(0, pon->base_aes + 0x38 + port * 4);
		writel(0, pon->base_drs + 0xd0 + port * 4);
	}
	for (port = 0; port < 4096; port++)
		zx_crypto_update(pon->base_ports + port * 4, BIT(1), 0);
}
