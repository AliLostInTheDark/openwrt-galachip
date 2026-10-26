// SPDX-License-Identifier: GPL-2.0-only
/* ZX279125 activation/PLOAM handling, using the vendor 2.6.32 register ABI. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "zx279125-pon.h"

#define ZX_PLOAM_TIMEOUT BIT(31)
#define ZX_PLOAM_SYNC_CHANGE BIT(5)
#define ZX_PLOAM_RECEIVED BIT(7)
#define ZX_PLOAM_TO1_MS 10000
#define ZX_PLOAM_TO2_MS 100

static int zx_ploam_set_state(struct zx279125_pon *pon,
			      enum zx_gpon_onu_state state)
{
	int ret = zx279125_gpon_set_state(pon, state);

	if (!ret && pon->odev)
		omci_device_set_state(pon->odev, state);
	return ret;
}

static void zx_ploam_preamble(struct zx279125_pon *pon, u16 type3)
{
	u32 val = (u32)pon->preamble1 << 18 |
		  (u32)pon->preamble2 << 10 | (type3 & 0x3ff);

	writel(val, pon->base_tx + 0xd4);
	writel(pon->preamble3, pon->base_tx + 0xd8);
	writel(1, pon->base_tx + 0xe4);
}

static void zx_ploam_clear_assignment(struct zx279125_pon *pon)
{
	unsigned int i;
	u32 val;

	if (pon->omci_port < 0x1000)
		zx279125_gpon_set_omcc(pon, pon->omci_port, false);
	for (i = 0; i <= 32; i++)
		zx279125_gpon_set_alloc(pon, i, 0xfff, false);
	pon->alloc_valid = 0;
	pon->onu_id = 0xff;
	if (pon->odev)
		omci_device_set_onu_id(pon->odev, pon->onu_id);
	val = readl(pon->base_onu);
	writel((val & ~GENMASK(23, 16)) | (0xff << 16), pon->base_onu);
	pon->overhead_valid = false;
	pon->serial_mask_enabled = false;
	pon->serial_mask_match = false;
	zx279125_gpon_crypto_reset(pon);
}

static void zx_ploam_restart(struct zx279125_pon *pon, bool synchronized)
{
	zx279125_gpon_hold_tx(pon, true);
	cancel_delayed_work(&pon->state_timeout);
	zx_ploam_clear_assignment(pon);
	zx_ploam_set_state(pon, synchronized ? ZX_ONU_STATE_O2_STANDBY :
			       ZX_ONU_STATE_O1_INITIAL);
}

static int zx_ploam_overhead(struct zx279125_pon *pon, const u8 *data)
{
	unsigned int guard = data[0] >> 3;
	unsigned int first = data[1] >> 3;
	unsigned int second = data[2] >> 3;
	u16 type3;
	u32 ctrl, coarse;

	/* SDK OverheadMsg keeps byte counts, then PreambleSet converts to bits. */
	if (data[0] > 64)
		return -EINVAL;
	pon->serial_mask_enabled = data[7] & BIT(4);
	pon->serial_mask_match = !pon->serial_mask_enabled;
	type3 = guard + first + second + 3 < 13 ?
		(9 - guard - first - second) * 8 : 0;
	pon->preamble1 = first * 8;
	pon->preamble2 = second * 8;
	pon->preamble3 = data[3];
	pon->preamble3_sync = type3;
	pon->preamble3_oper = type3;
	pon->delimiter = get_unaligned_be24(data + 4);
	pon->delimiter_bits = 24;
	writel(pon->delimiter, pon->base_tx + 0xe0);
	writel(3, pon->base_tx + 0xdc);
	/* OverheadMsg writes SDK field 0x1c8 only for defined power modes. */
	if ((data[7] & 3) != 3) {
		ctrl = readl(pon->base_tx + 0x64);
		ctrl = (ctrl & ~3) | (2 - (data[7] & 3));
		writel(ctrl, pon->base_tx + 0x64);
	}
	/* SDK default g_preClaDelay=0, g_respTime=5598; random window is 233. */
	pon->random_delay = get_random_u32_below(233);
	ctrl = readl(pon->base_onu + 4);
	writel((ctrl & ~0xff) | pon->random_delay, pon->base_onu + 4);
	writel(readl(pon->base_onu + 8) | BIT(0), pon->base_onu + 8);
	ctrl = readl(pon->base_onu + 0x10);
	writel((ctrl & ~0x3fff) | 5598, pon->base_onu + 0x10);
	if (data[7] & BIT(5)) {
		coarse = ((u32)get_unaligned_be16(data + 8) << 5) & 0xfffff;
		ctrl = ((coarse / 19440) & 0x1f) << 16 | (coarse % 19440);
		writel((readl(pon->base_onu + 0x24) & ~0xffffff) | ctrl,
		       pon->base_onu + 0x24);
	}
	zx_ploam_preamble(pon, type3);
	pon->overhead_valid = true;
	zx_ploam_set_state(pon, ZX_ONU_STATE_O3_SERIAL_NUM);
	mod_delayed_work(system_wq, &pon->state_timeout,
			 msecs_to_jiffies(ZX_PLOAM_TO1_MS));
	zx279125_gpon_hold_tx(pon, !pon->serial_mask_match);
	return 0;
}

static int zx_ploam_assign_onu(struct zx279125_pon *pon, const u8 *data)
{
	u32 val;
	int ret;

	if (!pon->serial_mask_match || data[0] >= 0xfe ||
	    memcmp(data + 1, pon->serial_number, 8))
		return -EINVAL;
	pon->onu_id = data[0];
	if (pon->odev)
		omci_device_set_onu_id(pon->odev, pon->onu_id);
	val = readl(pon->base_onu);
	writel((val & ~GENMASK(23, 16)) | (u32)pon->onu_id << 16,
	       pon->base_onu);
	ret = zx279125_gpon_set_alloc(pon, 0, pon->onu_id, true);
	if (ret)
		return ret;
	pon->alloc_ids[0] = pon->onu_id;
	set_bit(0, &pon->alloc_valid);
	return zx_ploam_set_state(pon, ZX_ONU_STATE_O4_RANGING);
}

static int zx_ploam_ranging(struct zx279125_pon *pon, const u8 *data)
{
	u32 eqd = get_unaligned_be32(data + 1);
	u32 coarse = (eqd >> 3) & 0xfffff;
	u32 val = (eqd & 7) << 21 | (coarse % 19440) |
		  ((coarse / 19440) & 0x1f) << 16;

	/* c002384c uses 155520-bit frame periods and a three-bit fine delay. */
	if (coarse / 19440 > 31)
		return -ERANGE;
	writel(val, pon->base_onu + 0x14);
	if (pon->state == ZX_ONU_STATE_O5_OPERATION)
		return 0;
	cancel_delayed_work(&pon->state_timeout);
	zx_ploam_preamble(pon, pon->preamble3_oper);
	return zx_ploam_set_state(pon, ZX_ONU_STATE_O5_OPERATION);
}

static int zx_ploam_alloc(struct zx279125_pon *pon, const u8 *data)
{
	u16 alloc = (u16)data[0] << 4 | data[1] >> 4;
	unsigned int i, slot = 32;
	int ret;

	if (data[2] != 1 && data[2] != 0xff)
		return -EOPNOTSUPP;
	/* Slot zero is the implicit ONU-ID allocation. */
	if (alloc == pon->onu_id)
		return 0;
	for (i = 1; i < 32; i++) {
		if (test_bit(i, &pon->alloc_valid) && pon->alloc_ids[i] == alloc) {
			slot = i;
			break;
		}
	}
	if (data[2] == 0xff) {
		if (slot == 32)
			return 0;
		ret = zx279125_gpon_set_alloc(pon, slot, alloc, false);
		if (!ret)
			clear_bit(slot, &pon->alloc_valid);
		return ret;
	}
	if (slot != 32)
		return 0;
	for (i = 1; i < 32; i++)
		if (!test_bit(i, &pon->alloc_valid))
			break;
	if (i == 32)
		return -ENOSPC;
	ret = zx279125_gpon_set_alloc(pon, i, alloc, true);
	if (!ret) {
		pon->alloc_ids[i] = alloc;
		set_bit(i, &pon->alloc_valid);
	}
	return ret;
}

static int zx_ploam_ack(struct zx279125_pon *pon, const u8 *msg)
{
	u8 data[10];

	data[0] = msg[1];
	memcpy(data + 1, msg, 9);
	return zx279125_gpon_send_ploam(pon, 9, data, 1);
}

static int zx_ploam_process(struct zx279125_pon *pon, const u8 *msg)
{
	const u8 *data = msg + 2;
	enum zx_gpon_onu_state state = pon->state;
	int ret;

	if (msg[0] != 0xff && msg[0] != pon->onu_id)
		return 0;
	/* Disable_Serial_Number is handled before any ordinary state transition. */
	if (msg[1] == 6 && data[0] == 0xff &&
	    !memcmp(data + 1, pon->serial_number, 8)) {
		pon->emergency_stop = true;
		zx279125_gpon_hold_tx(pon, true);
		cancel_delayed_work(&pon->state_timeout);
		return zx_ploam_set_state(pon, ZX_ONU_STATE_O7_EMERGENCY_STOP);
	}
	if (pon->emergency_stop) {
		/* Matching enable, or the vendor's broadcast-enable value 0x0f. */
		if (msg[1] != 6 || (data[0] != 0x0f &&
		    (data[0] != 0 || memcmp(data + 1, pon->serial_number, 8))))
			return -EPERM;
		pon->emergency_stop = false;
		zx_ploam_restart(pon, true);
		return 0;
	}
	switch (msg[1]) {
	case 1:
		if (state == ZX_ONU_STATE_O2_STANDBY)
			return zx_ploam_overhead(pon, data);
		break;
	case 2:
		if (state == ZX_ONU_STATE_O3_SERIAL_NUM && pon->serial_mask_enabled) {
			unsigned int bits = data[0], byte;
			bool match = true;

			if (bits > 64)
				return -EINVAL;
			for (byte = 0; byte < DIV_ROUND_UP(bits, 8); byte++) {
				unsigned int width = min(bits - byte * 8, 8U);
				u8 mask = (1U << width) - 1;

				if ((pon->serial_number[7 - byte] ^ data[8 - byte]) & mask)
					match = false;
			}
			pon->serial_mask_match = match;
			zx279125_gpon_hold_tx(pon, !match);
		}
		break;
	case 3:
		if (state == ZX_ONU_STATE_O3_SERIAL_NUM)
			return zx_ploam_assign_onu(pon, data);
		break;
	case 4:
		if (msg[0] == pon->onu_id &&
		    (state == ZX_ONU_STATE_O4_RANGING ||
		     state == ZX_ONU_STATE_O5_OPERATION))
			return zx_ploam_ranging(pon, data);
		break;
	case 5:
		if (state >= ZX_ONU_STATE_O3_SERIAL_NUM &&
		    state <= ZX_ONU_STATE_O6_POPUP)
			zx_ploam_restart(pon, true);
		break;
	case 9:
		if (state == ZX_ONU_STATE_O5_OPERATION && msg[0] == pon->onu_id)
			return zx279125_gpon_send_ploam(pon, 2, pon->password, 3);
		break;
	case 10:
		if (state != ZX_ONU_STATE_O5_OPERATION || msg[0] != pon->onu_id)
			break;
		ret = zx_ploam_alloc(pon, data);
		return ret ? ret : zx_ploam_ack(pon, msg);
	case 12:
		if (state != ZX_ONU_STATE_O6_POPUP ||
		    (readl(pon->base_rx + 0x14) & BIT(4)))
			break;
		cancel_delayed_work(&pon->state_timeout);
		if (msg[0] == pon->onu_id) {
			zx_ploam_preamble(pon, pon->preamble3_oper);
			zx_ploam_set_state(pon, ZX_ONU_STATE_O5_OPERATION);
		} else {
			zx_ploam_preamble(pon, pon->preamble3_sync);
			zx_ploam_set_state(pon, ZX_ONU_STATE_O4_RANGING);
			mod_delayed_work(system_wq, &pon->state_timeout,
					 msecs_to_jiffies(ZX_PLOAM_TO1_MS));
		}
		zx279125_gpon_hold_tx(pon, false);
		break;
	case 14:
		if (state != ZX_ONU_STATE_O5_OPERATION || msg[0] != pon->onu_id)
			break;
		if (data[0] > 1)
			return -EINVAL;
		ret = zx279125_gpon_set_omcc(pon,
				(u16)data[1] << 4 | data[2] >> 4, data[0] == 1);
		return ret ? ret : zx_ploam_ack(pon, msg);
	case 20:
		if (state == ZX_ONU_STATE_O3_SERIAL_NUM) {
			if (data[0] > 127 || data[1] > 127)
				return -ERANGE;
			pon->preamble3_sync = (u16)data[0] << 3;
			pon->preamble3_oper = (u16)data[1] << 3;
			zx_ploam_preamble(pon, pon->preamble3_sync);
		}
		break;
	case 8:
		if (state != ZX_ONU_STATE_O5_OPERATION || msg[0] != pon->onu_id)
			break;
		ret = zx279125_gpon_encrypt_port(pon, data);
		return ret ? ret : zx_ploam_ack(pon, msg);
	case 13:
		if (state == ZX_ONU_STATE_O5_OPERATION && msg[0] == pon->onu_id)
			return zx279125_gpon_request_key(pon);
		break;
	case 19:
		if (state != ZX_ONU_STATE_O5_OPERATION || msg[0] != pon->onu_id)
			break;
		ret = zx279125_gpon_switch_key(pon, data);
		return ret ? ret : zx_ploam_ack(pon, msg);
	default:
		break;
	}
	return 0;
}

static int zx_ploam_receive(struct zx279125_pon *pon, u8 *msg)
{
	u32 status = readl(pon->base_rx + 0x94);
	unsigned int i;

	if ((status & 3) != 1)
		return -EAGAIN;
	for (i = 0; i < 3; i++)
		put_unaligned_be32(readl(pon->base_rx + 0x84 + i * 4), msg + i * 4);
	/* gpon_receive_ploam acknowledges the RX latch by writing zero. */
	writel(0, pon->base_rx + 0x94);
	return 0;
}

static void zx_ploam_run(struct work_struct *work)
{
	struct zx279125_pon *pon = container_of(work, struct zx279125_pon, state_work);
	unsigned long flags;
	u32 pending, status, unmask;
	u8 msg[12];
	unsigned int budget;
	int ret;

	for (;;) {
		spin_lock_irqsave(&pon->lock, flags);
		pending = pon->pending_irqs;
		pon->pending_irqs = 0;
		spin_unlock_irqrestore(&pon->lock, flags);
		if (!READ_ONCE(pon->running) || !pending)
			return;
		unmask = pending & ZX_PON_INT_VALID;
		if (pending & (ZX_PLOAM_SYNC_CHANGE | BIT(6))) {
			status = readl(pon->base_rx + 0x14);
			if (pon->xpon)
				xpon_device_report_optical(pon->xpon,
					!(status & (BIT(4) | BIT(5))),
					status & (BIT(4) | BIT(5)));
			if (status & (BIT(4) | BIT(5))) {
				zx279125_gpon_hold_tx(pon, true);
				if (!pon->emergency_stop && pon->state == ZX_ONU_STATE_O5_OPERATION) {
					zx_ploam_set_state(pon, ZX_ONU_STATE_O6_POPUP);
					mod_delayed_work(system_wq, &pon->state_timeout,
						msecs_to_jiffies(ZX_PLOAM_TO2_MS));
				} else if (!pon->emergency_stop) {
					zx_ploam_restart(pon, false);
				}
			} else if (!pon->emergency_stop && pon->state == ZX_ONU_STATE_O1_INITIAL) {
				zx_ploam_restart(pon, true);
			}
		}
		if ((pending & ZX_PLOAM_TIMEOUT) && !pon->emergency_stop &&
		    (pon->state == ZX_ONU_STATE_O3_SERIAL_NUM ||
		     pon->state == ZX_ONU_STATE_O4_RANGING ||
		     pon->state == ZX_ONU_STATE_O6_POPUP))
			zx_ploam_restart(pon, !(readl(pon->base_rx + 0x14) & BIT(4)));
		if (pending & ZX_PLOAM_RECEIVED) {
			for (budget = 0; budget < 32; budget++) {
				if (zx_ploam_receive(pon, msg))
					break;
				ret = zx_ploam_process(pon, msg);
				if (ret)
					dev_warn_ratelimited(pon->dev, "PLOAM %u rejected: %d\n", msg[1], ret);
			}
			if ((readl(pon->base_rx + 0x94) & 3) != 0) {
				unmask &= ~ZX_PLOAM_RECEIVED;
				zx279125_gpon_hold_tx(pon, true);
				dev_err_ratelimited(pon->dev,
					"PLOAM RX did not drain; IRQ masked, TX held\n");
			}
		}
		spin_lock_irqsave(&pon->lock, flags);
		writel(pending & ZX_PON_INT_VALID,
		       pon->base_gpon + ZX_PON_INT_STATUS);
		if (pon->running)
			writel(readl(pon->base_gpon + ZX_PON_INT_MASK) & ~unmask,
			       pon->base_gpon + ZX_PON_INT_MASK);
		spin_unlock_irqrestore(&pon->lock, flags);
	}
}

static void zx_ploam_work(struct work_struct *work)
{
	struct zx279125_pon *pon = container_of(work, struct zx279125_pon, state_work);

	mutex_lock(&pon->config_lock);
	zx_ploam_run(work);
	mutex_unlock(&pon->config_lock);
}

static void zx_ploam_timeout(struct work_struct *work)
{
	struct zx279125_pon *pon = container_of(to_delayed_work(work),
					     struct zx279125_pon, state_timeout);
	unsigned long flags;

	spin_lock_irqsave(&pon->lock, flags);
	if (pon->running) {
		pon->pending_irqs |= ZX_PLOAM_TIMEOUT;
		schedule_work(&pon->state_work);
	}
	spin_unlock_irqrestore(&pon->lock, flags);
}

void zx279125_ploam_init(struct zx279125_pon *pon)
{
	INIT_WORK(&pon->state_work, zx_ploam_work);
	INIT_DELAYED_WORK(&pon->state_timeout, zx_ploam_timeout);
}

void zx279125_ploam_stop(struct zx279125_pon *pon)
{
	/* Caller clears running and synchronizes the IRQ before cancellation. */
	cancel_work_sync(&pon->state_work);
	cancel_delayed_work_sync(&pon->state_timeout);
}
