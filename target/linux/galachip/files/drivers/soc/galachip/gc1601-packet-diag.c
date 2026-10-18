// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#define GC1601_BUFFER_COUNT 512
#define GC1601_BUFFER_SIZE 2048

static const size_t gc1601_dma_sizes[] = { 1024, 1024 * 1024, 65536, 4096 };
static const char * const gc1601_dma_names[] = { "free-list", "packets", "rx-ring", "tx-ring" };

struct gc1601_descriptor {
	__le32 word[4];
};

static bool gc1601_rx_bounds(u32 word0, u32 word2)
{
	u32 length = word0 & 0x3fff;
	u32 id = word2 >> 17;
	u32 offset = (word2 & 0xfff) >> 6;

	return id < GC1601_BUFFER_COUNT && length &&
	       length <= GC1601_BUFFER_SIZE - 1 &&
	       offset + length <= GC1601_BUFFER_SIZE;
}

static int gc1601_dma_check(void **memory)
{
	__be16 *list = memory[0];
	u32 *packets = memory[1];
	struct gc1601_descriptor *tx = memory[3];
	unsigned int i;

	static_assert(sizeof(*tx) == 16);
	for (i = 0; i < GC1601_BUFFER_COUNT; i++)
		list[i] = cpu_to_be16(i);
	for (i = 0; i < gc1601_dma_sizes[1] / sizeof(*packets); i++)
		packets[i] = i ^ 0xa5a55a5a;
	for (i = 0; i < 256; i++) {
		tx[i].word[0] = cpu_to_le32(64 + i);
		tx[i].word[1] = cpu_to_le32(0x1200);
		tx[i].word[2] = cpu_to_le32((i << 17) | 0x4000);
		tx[i].word[3] = cpu_to_le32(BIT(31));
	}
	dma_wmb();
	dma_rmb();
	for (i = 0; i < GC1601_BUFFER_COUNT; i++)
		if (be16_to_cpu(list[i]) != i)
			return -EIO;
	for (i = 0; i < gc1601_dma_sizes[1] / sizeof(*packets); i++)
		if (packets[i] != (i ^ 0xa5a55a5a))
			return -EIO;
	for (i = 0; i < 256; i++)
		if (le32_to_cpu(tx[i].word[0]) != 64 + i ||
		    le32_to_cpu(tx[i].word[1]) != 0x1200 ||
		    le32_to_cpu(tx[i].word[2]) != ((i << 17) | 0x4000) ||
		    le32_to_cpu(tx[i].word[3]) != BIT(31))
			return -EIO;
	for (i = 0; i < GC1601_BUFFER_COUNT; i++)
		if (!gc1601_rx_bounds(1985, (i << 17) | (63 << 6)))
			return -EINVAL;
	if (!gc1601_rx_bounds(2047, 0) ||
	    gc1601_rx_bounds(0, 0) || gc1601_rx_bounds(2048, 0) ||
	    gc1601_rx_bounds(64, GC1601_BUFFER_COUNT << 17) ||
	    gc1601_rx_bounds(1986, 63 << 6))
		return -EINVAL;
	return 0;
}

static int gc1601_dma_test(struct device *dev, dma_addr_t *addresses)
{
	void *memory[ARRAY_SIZE(gc1601_dma_sizes)] = {};
	unsigned int i;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(memory); i++) {
		memory[i] = dma_alloc_coherent(dev, gc1601_dma_sizes[i],
					       &addresses[i], GFP_KERNEL);
		if (!memory[i]) {
			ret = -ENOMEM;
			goto out;
		}
		if (!IS_ALIGNED(addresses[i], 64) ||
		    addresses[i] > DMA_BIT_MASK(32) - (gc1601_dma_sizes[i] - 1)) {
			ret = -ERANGE;
			goto out;
		}
	}
	ret = gc1601_dma_check(memory);
out:
	/* Never published to hardware, so no DMA drain is needed before freeing. */
	for (i = ARRAY_SIZE(memory); i-- > 0;)
		if (memory[i])
			dma_free_coherent(dev, gc1601_dma_sizes[i], memory[i], addresses[i]);
	return ret;
}

struct gc1601_registers {
	const char *name;
	u16 offsets[16];
	u8 count;
};

enum gc1601_window {
	GC1601_GMAC,
	GC1601_BUFFERS,
	GC1601_DESCRIPTORS,
	GC1601_RX,
	GC1601_TX_RING,
	GC1601_RX_PENDING,
	GC1601_TX_SPACE,
	GC1601_SCHEDULER_READY,
	GC1601_BUFFER_CONTROL,
	GC1601_BUFFER_ENABLE,
};

/* Exclude IRQ status, allocation, release, doorbell and completion reads. */
static const struct gc1601_registers gc1601_registers[] = {
	[GC1601_GMAC] = { "gmac", { 0x00, 0x04, 0x08 }, 3 },
	[GC1601_BUFFERS] = { "buffers", { 0x00, 0x04, 0x0c, 0x14, 0x20, 0x24,
			0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x44, 0x74 }, 14 },
	[GC1601_DESCRIPTORS] = { "descriptors", { 0x00, 0x04, 0x14, 0x1c, 0x40 }, 5 },
	[GC1601_RX] = { "rx", { 0x00, 0x18, 0x1c, 0x20, 0x24 }, 5 },
	[GC1601_TX_RING] = { "tx-ring", { 0x00 }, 1 },
	[GC1601_RX_PENDING] = { "rx-pending", { 0x00 }, 1 },
	[GC1601_TX_SPACE] = { "tx-space", { 0x00 }, 1 },
	[GC1601_SCHEDULER_READY] = { "scheduler-ready", { 0x00, 0x0c }, 2 },
	[GC1601_BUFFER_CONTROL] = { "buffer-control", { 0x00 }, 1 },
	[GC1601_BUFFER_ENABLE] = { "buffer-enable", { 0x00 }, 1 },
};

struct gc1601_packet_diag {
	void __iomem *base[ARRAY_SIZE(gc1601_registers)];
	resource_size_t address[ARRAY_SIZE(gc1601_registers)];
	dma_addr_t test_addresses[ARRAY_SIZE(gc1601_dma_sizes)];
	int test_result;
	struct mutex completion_lock;
	bool gate_attempted;
	bool gate_requested;
	bool gate_sampled;
	int gate_result;
	u32 gate_before;
	u32 gate_enabled;
	u32 gate_after;
	u32 gate_request;
	u32 gate_response;
	u32 gate_mac_after;
	s64 gate_time_us;
	u32 completions[16];
	unsigned int completion_count;
	int completion_result;
	bool sram_attempted;
	u32 sram_before[2];
	u32 sram_after[2];
	s64 sram_time_us[2];
	unsigned int sram_stages;
	int sram_result;
	bool tx_attempted;
	bool tx_submitted;
	bool allocation_only;
	bool allocation_requested;
	bool allocation_sampled;
	u32 allocation_request;
	u32 allocation_response;
	s64 allocation_time_us;
	s64 tx_hold_time_us;
	u32 tx_gate_active;
	u32 tx_gate_after;
	u32 tx_mac_active;
	u32 tx_mac_after;
	int tx_result;
	u32 tx_buffer_id;
	u32 tx_completion;
	u32 tx_baseline;
	struct gc1601_descriptor *tx_ring;
	dma_addr_t tx_ring_dma;
	dma_addr_t tx_packet_dma;
	bool rx_attempted;
	bool rx_received;
	int rx_result;
	u32 rx_pending_before;
	u32 rx_pending_sampled;
	u32 rx_pending_after;
	u32 rx_gate_active;
	u32 rx_gate_after;
	u32 rx_mac_active;
	u32 rx_mac_after;
	s64 rx_elapsed_us;
	int rx_slot;
	u32 rx_desc_words[4];
	u32 rx_len;
	u32 rx_buffer_id;
	u32 rx_offset;
	u32 rx_strobe;
	u32 rx_crcerr;
	u8 rx_sample[64];
	size_t rx_sample_len;
};

static bool gc1601_completion_allowed(struct gc1601_packet_diag *diag)
{
	return !(readl(diag->base[GC1601_GMAC]) & 3) &&
	       !(readl(diag->base[GC1601_RX]) & 1) &&
	       !(readl(diag->base[GC1601_RX_PENDING]) & 0xffff) &&
	       !readl(diag->base[GC1601_SCHEDULER_READY]) &&
	       !(readl(diag->base[GC1601_SCHEDULER_READY] + 0x0c) & 0xff);
}

static ssize_t tx_completion_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len;
	unsigned int i;

	mutex_lock(&diag->completion_lock);
	if (diag->completion_result == -ENODATA) {
		len = sysfs_emit(buf, "not run; write 1 to sample potentially consuming TX completion reads\n");
	} else {
		len = sysfs_emit(buf, "result %d, samples %u; not a DMA-idle proof\n",
				 diag->completion_result, diag->completion_count);
		for (i = 0; i < diag->completion_count; i++)
			len += sysfs_emit_at(buf, len, "%u: raw %08x, count %u\n", i,
					    diag->completions[i], diag->completions[i] & 0xffff);
	}
	mutex_unlock(&diag->completion_lock);
	return len;
}

static ssize_t tx_completion_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	unsigned int i;
	int ret;

	if (IS_ENABLED(CONFIG_GC1601_PACKET_LOADER_STATE) ||
	    IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST) ||
	    IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST))
		return -EOPNOTSUPP;
	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = mutex_lock_interruptible(&diag->completion_lock);
	if (ret)
		return ret;
	if (diag->tx_attempted || diag->rx_attempted) {
		mutex_unlock(&diag->completion_lock);
		return -EBUSY;
	}
	diag->completion_count = 0;
	for (i = 0; i < ARRAY_SIZE(diag->completions); i++) {
		if (!gc1601_completion_allowed(diag)) {
			ret = -EBUSY;
			break;
		}
		/* Completion reads may consume events; keep them out of snapshot. */
		diag->completions[i] = readl(diag->base[GC1601_TX_RING] + 0x08);
		diag->completion_count++;
		if (i + 1 < ARRAY_SIZE(diag->completions))
			usleep_range(1000, 1500);
	}
	diag->completion_result = ret;
	mutex_unlock(&diag->completion_lock);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_RW(tx_completion);

static bool gc1601_loader_profile(struct gc1601_packet_diag *diag)
{
	void __iomem *base = diag->base[GC1601_BUFFERS];

	return gc1601_completion_allowed(diag) &&
	       readl(diag->base[GC1601_GMAC]) == 0x00bb2000 &&
	       readl(base + 0x28) == 0x40800000 &&
	       readl(base + 0x34) == 0x40801000 &&
	       readl(base + 0x30) == 0x40901000 &&
	       readl(base + 0x2c) == 0x50008000 &&
	       readl(base + 0x38) == 0x54009000 &&
	       readl(base + 0x3c) == 0x30000800 &&
	       readl(base + 0x24) == 0x10 &&
	       readl(base + 0x74) == 2 &&
	       !(readl(diag->base[GC1601_RX] + 0x14) & 1) &&
	       readl(diag->base[GC1601_TX_RING]) == 0x40911000 &&
	       (readl(diag->base[GC1601_TX_SPACE]) & 0x2ff) == 0x2ff;
}

static int gc1601_gate_buffer_alloc(struct gc1601_packet_diag *diag)
{
	void __iomem *rx = diag->base[GC1601_RX];
	u32 value;
	ktime_t start;
	int ret;

	if (!gc1601_loader_profile(diag) ||
	    readl(diag->base[GC1601_BUFFER_ENABLE]) != 1 ||
	    readl(diag->base[GC1601_BUFFER_CONTROL]) != 0x0800001f ||
	    readl(diag->base[GC1601_BUFFERS] + 0x0c) != 0x001fffff ||
	    readl(diag->base[GC1601_BUFFERS] + 0x14) != 0x0007ffff ||
	    readl(diag->base[GC1601_BUFFERS] + 0x20) ||
	    readl(rx) || readl(rx + 0x18) != 0x60)
		return -EBUSY;

	diag->gate_before = readl(rx);
	writel(diag->gate_before | 1, rx);
	diag->gate_enabled = readl(rx);
	if (diag->gate_enabled != (diag->gate_before | 1) ||
	    (readl(diag->base[GC1601_GMAC]) & 3)) {
		ret = -EIO;
		goto restore;
	}
	start = ktime_get();
	diag->gate_requested = true;
	writel(1, rx + 0x14);
	ret = readl_poll_timeout(rx + 0x14, value, !(value & 1), 1, 1000);
	diag->gate_request = value;
	diag->gate_time_us = ktime_us_delta(ktime_get(), start);
	if (ret)
		goto restore;
	diag->gate_response = readl(rx + 0x0c);
	diag->gate_sampled = true;
	if (!(diag->gate_response & BIT(31)))
		ret = -EFAULT;
	else if ((diag->gate_response & ~BIT(31)) >= GC1601_BUFFER_COUNT)
		ret = -ERANGE;
restore:
	/* Keep the inherited arena and any allocated ID reserved until power-off. */
	writel(diag->gate_before, rx);
	diag->gate_after = readl(rx);
	diag->gate_mac_after = readl(diag->base[GC1601_GMAC]);
	if (diag->gate_after != diag->gate_before || (diag->gate_mac_after & 3))
		ret = -EIO;
	return ret;
}

static int gc1601_tx_once(struct device *dev, struct gc1601_packet_diag *diag)
{
	static const u8 frame[64] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x02, 0x47, 0x37, 0x00, 0x00, 0x01, 0x88, 0xb5,
		'G', '7', '-', 'T', 'X', '-', 'O', 'N', 'C', 'E',
	};
	void __iomem *rx = diag->base[GC1601_RX];
	phys_addr_t packet_address;
	void *packet;
	ktime_t hold_start;
	u32 value;
	unsigned int i;
	int ret;

	if (!gc1601_loader_profile(diag))
		return -EBUSY;
	if (diag->allocation_only)
		goto allocate_buffer;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	diag->tx_ring = dma_alloc_coherent(dev, 4096, &diag->tx_ring_dma, GFP_KERNEL);
	if (!diag->tx_ring)
		return -ENOMEM;
	/* Retain all test storage until power-off: completion is not DMA idle. */
	if (!IS_ALIGNED(diag->tx_ring_dma, 64) ||
	    diag->tx_ring_dma > DMA_BIT_MASK(32) - 4095)
		return -ERANGE;
	if (!gc1601_loader_profile(diag))
		return -EBUSY;
	diag->tx_baseline = readl(diag->base[GC1601_TX_RING] + 8);
	if (diag->tx_baseline & 0xffff)
		return -EBUSY;
allocate_buffer:
	ret = gc1601_gate_buffer_alloc(diag);
	diag->allocation_requested = diag->gate_requested;
	diag->allocation_sampled = diag->gate_sampled;
	diag->allocation_request = diag->gate_request;
	diag->allocation_response = diag->gate_response;
	diag->allocation_time_us = diag->gate_time_us;
	if (ret)
		return ret;
	diag->tx_buffer_id = diag->allocation_response & ~BIT(31);
	if (diag->allocation_only)
		return 0;
	packet_address = 0x40801000 + diag->tx_buffer_id * GC1601_BUFFER_SIZE;
	packet = phys_to_virt(packet_address);
	memset(packet, 0, GC1601_BUFFER_SIZE);
	memcpy(packet, frame, sizeof(frame));
	diag->tx_packet_dma = dma_map_single(dev, packet, GC1601_BUFFER_SIZE,
					     DMA_TO_DEVICE);
	if (dma_mapping_error(dev, diag->tx_packet_dma))
		return -EIO;
	/* The inherited pool resolves IDs to physical slots, not bounce buffers. */
	if (diag->tx_packet_dma != packet_address) {
		dma_unmap_single(dev, diag->tx_packet_dma, GC1601_BUFFER_SIZE, DMA_TO_DEVICE);
		return -ERANGE;
	}
	for (i = 0; i < 256; i++) {
		diag->tx_ring[i].word[0] = cpu_to_le32(sizeof(frame));
		diag->tx_ring[i].word[1] = cpu_to_le32(0x1200);
		diag->tx_ring[i].word[2] = cpu_to_le32((diag->tx_buffer_id << 17) | 0x4000);
		diag->tx_ring[i].word[3] = cpu_to_le32(BIT(31));
	}
	if (!gc1601_loader_profile(diag))
		return -EBUSY;
	dma_wmb();
	writel(diag->tx_ring_dma, diag->base[GC1601_TX_RING]);
	if (readl(diag->base[GC1601_TX_RING]) != diag->tx_ring_dma)
		return -EIO;
	writel(1, rx);
	writel(0x00bb2003, diag->base[GC1601_GMAC]);
	diag->tx_gate_active = readl(rx);
	diag->tx_mac_active = readl(diag->base[GC1601_GMAC]);
	if (diag->tx_mac_active != 0x00bb2003 || !(diag->tx_gate_active & 1)) {
		ret = -EIO;
		goto stop_tx;
	}
	writel(diag->tx_buffer_id, rx + 0x38);
	diag->tx_submitted = true;
	writel(1, diag->base[GC1601_TX_RING] + 4);
	ret = readl_poll_timeout(diag->base[GC1601_TX_RING] + 8, value,
				 value & 0xffff, 1000, 100000);
	diag->tx_completion = value;
	if (!ret && (value & 0xffff) == 1) {
		/* Ring completion is not proof that the MAC has finished egress. */
		hold_start = ktime_get();
		usleep_range(100000, 110000);
		diag->tx_hold_time_us = ktime_us_delta(ktime_get(), hold_start);
		if (readl(diag->base[GC1601_GMAC]) != 0x00bb2003 ||
		    !(readl(rx) & 1))
			ret = -EIO;
	}
stop_tx:
	writel(0x00bb2000, diag->base[GC1601_GMAC]);
	writel(0, rx);
	diag->tx_mac_after = readl(diag->base[GC1601_GMAC]);
	diag->tx_gate_after = readl(rx);
	if (diag->tx_mac_after != 0x00bb2000 || (diag->tx_gate_after & 1))
		ret = -EIO;
	if (!ret && (diag->tx_completion & 0xffff) != 1)
		ret = -EIO;
	return ret;
}

static ssize_t tx_once_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST))
		return sysfs_emit(buf, "disabled\n");
	mutex_lock(&diag->completion_lock);
	if (!diag->tx_attempted)
		len = sysfs_emit(buf, "not run; alloc=allocation, 1=TX; power-cycle afterward\n");
	else {
		len = sysfs_emit(buf, "result %d submitted %u buffer %u ring %pad\n",
				 diag->tx_result, diag->tx_submitted, diag->tx_buffer_id,
				 &diag->tx_ring_dma);
		len += sysfs_emit_at(buf, len, "baseline %08x completion %08x\n",
				    diag->tx_baseline, diag->tx_completion);
		len += sysfs_emit_at(buf, len,
			"allocation-only %u requested %u sampled %u elapsed %lld us\n",
			diag->allocation_only, diag->allocation_requested,
			diag->allocation_sampled, diag->allocation_time_us);
		len += sysfs_emit_at(buf, len, "allocation request %08x response %08x\n",
				    diag->allocation_request, diag->allocation_response);
		len += sysfs_emit_at(buf, len, "gate before %08x enabled %08x after %08x\n",
				    diag->gate_before, diag->gate_enabled, diag->gate_after);
		len += sysfs_emit_at(buf, len, "allocation mac-after %08x\n",
				    diag->gate_mac_after);
		if (!diag->allocation_only) {
			len += sysfs_emit_at(buf, len,
				"tx gate active %08x after %08x\n",
				diag->tx_gate_active, diag->tx_gate_after);
			len += sysfs_emit_at(buf, len,
				"tx mac active %08x after %08x\n",
				diag->tx_mac_active, diag->tx_mac_after);
			len += sysfs_emit_at(buf, len, "post-completion hold %lld us\n",
					    diag->tx_hold_time_us);
			len += sysfs_emit_at(buf, len, "LAN capture required\n");
		}
		len += sysfs_emit_at(buf, len, "Not an index-reset or DMA-idle proof\n");
	}
	mutex_unlock(&diag->completion_lock);
	return len;
}

static ssize_t tx_once_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	int ret;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST))
		return -EOPNOTSUPP;
	if (!sysfs_streq(buf, "1") && !sysfs_streq(buf, "alloc"))
		return -EINVAL;
	ret = mutex_lock_interruptible(&diag->completion_lock);
	if (ret)
		return ret;
	if (diag->tx_attempted || diag->rx_attempted) {
		mutex_unlock(&diag->completion_lock);
		return -EBUSY;
	}
	diag->tx_attempted = true;
	diag->allocation_only = sysfs_streq(buf, "alloc");
	diag->tx_result = gc1601_tx_once(dev, diag);
	ret = diag->tx_result;
	mutex_unlock(&diag->completion_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(tx_once);

#if IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST)
static int gc1601_rx_once(struct device *dev, struct gc1601_packet_diag *diag)
{
	void __iomem *rx = diag->base[GC1601_RX];
	struct gc1601_descriptor *rx_ring = phys_to_virt(0x40901000);
	dma_addr_t rx_ring_dma = 0;
	ktime_t start;
	u32 val;
	int ret, i, slot;

	if (!gc1601_loader_profile(diag))
		return -EBUSY;

	diag->rx_pending_before = readl(diag->base[GC1601_RX_PENDING]) & 0xffff;
	if (diag->rx_pending_before)
		return -EBUSY;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	memset(rx_ring, 0, 4096);
	dma_wmb();

	rx_ring_dma = dma_map_single(dev, rx_ring, 4096, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, rx_ring_dma))
		return -EIO;
	if (rx_ring_dma != 0x40901000) {
		dma_unmap_single(dev, rx_ring_dma, 4096, DMA_BIDIRECTIONAL);
		return -ERANGE;
	}
	dma_sync_single_for_device(dev, rx_ring_dma, 4096, DMA_BIDIRECTIONAL);
	dma_wmb();

	start = ktime_get();
	writel(1, rx);
	writel(0x00bb2003, diag->base[GC1601_GMAC]);
	diag->rx_gate_active = readl(rx);
	diag->rx_mac_active = readl(diag->base[GC1601_GMAC]);
	if (diag->rx_mac_active != 0x00bb2003 || !(diag->rx_gate_active & 1)) {
		ret = -EIO;
		goto stop_rx;
	}

	ret = readl_poll_timeout(diag->base[GC1601_RX_PENDING], val,
				 (val & 0xffff) > 0, 1000, 10000000);
	diag->rx_elapsed_us = ktime_us_delta(ktime_get(), start);
	diag->rx_pending_sampled = val & 0xffff;
	if (ret)
		goto stop_rx;

	diag->rx_received = true;
	dma_sync_single_for_cpu(dev, rx_ring_dma, 4096, DMA_BIDIRECTIONAL);
	dma_rmb();

	slot = -1;
	for (i = 0; i < 256; i++) {
		u32 w0 = le32_to_cpu(rx_ring[i].word[0]);

		if (w0 & 0x3fff) {
			slot = i;
			break;
		}
	}
	diag->rx_slot = slot;
	if (slot < 0) {
		ret = -ENOMSG;
		goto stop_rx;
	}

	for (i = 0; i < 4; i++)
		diag->rx_desc_words[i] = le32_to_cpu(rx_ring[slot].word[i]);

	diag->rx_len = diag->rx_desc_words[0] & 0x3fff;
	diag->rx_crcerr = (diag->rx_desc_words[1] >> 31) & 1;
	diag->rx_buffer_id = diag->rx_desc_words[2] >> 17;
	diag->rx_offset = (diag->rx_desc_words[2] & 0xfff) >> 6;
	diag->rx_strobe = (diag->rx_desc_words[3] >> 16) & 1;

	if (diag->rx_buffer_id >= GC1601_BUFFER_COUNT ||
	    diag->rx_len > GC1601_BUFFER_SIZE) {
		ret = -ERANGE;
		goto stop_rx;
	}

	{
		phys_addr_t pkt_phys = 0x40801000 + diag->rx_buffer_id * GC1601_BUFFER_SIZE;
		void *pkt_virt = phys_to_virt(pkt_phys);
		dma_addr_t pkt_dma;

		pkt_dma = dma_map_single(dev, pkt_virt, GC1601_BUFFER_SIZE, DMA_FROM_DEVICE);
		if (!dma_mapping_error(dev, pkt_dma)) {
			dma_sync_single_for_cpu(dev, pkt_dma, GC1601_BUFFER_SIZE, DMA_FROM_DEVICE);
			dma_rmb();
			diag->rx_sample_len = min_t(size_t, diag->rx_len, sizeof(diag->rx_sample));
			if (diag->rx_offset + diag->rx_sample_len <= GC1601_BUFFER_SIZE)
				memcpy(diag->rx_sample, pkt_virt + diag->rx_offset, diag->rx_sample_len);
			dma_unmap_single(dev, pkt_dma, GC1601_BUFFER_SIZE, DMA_FROM_DEVICE);
		}
	}

	writel((diag->rx_strobe << 3) | 0x10, diag->base[GC1601_DESCRIPTORS] + 0x64);
	writel(1, diag->base[GC1601_DESCRIPTORS] + 0x60);
	writel(diag->rx_buffer_id, rx + 0x10);
	diag->rx_pending_after = readl(diag->base[GC1601_RX_PENDING]) & 0xffff;

stop_rx:
	writel(0x00bb2000, diag->base[GC1601_GMAC]);
	writel(0, rx);
	diag->rx_mac_after = readl(diag->base[GC1601_GMAC]);
	diag->rx_gate_after = readl(rx);
	if (rx_ring_dma && !dma_mapping_error(dev, rx_ring_dma))
		dma_unmap_single(dev, rx_ring_dma, 4096, DMA_BIDIRECTIONAL);
	if (diag->rx_mac_after != 0x00bb2000 || (diag->rx_gate_after & 1))
		ret = -EIO;
	return ret;
}
#else
static int gc1601_rx_once(struct device *dev, struct gc1601_packet_diag *diag)
{
	return -EOPNOTSUPP;
}
#endif

static ssize_t rx_once_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len;
	unsigned int i;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST))
		return sysfs_emit(buf, "disabled\n");

	mutex_lock(&diag->completion_lock);
	if (!diag->rx_attempted) {
		len = sysfs_emit(buf, "not run; write 1 to arm single-packet RX capture (10s timeout); power-cycle afterward\n");
	} else {
		len = sysfs_emit(buf, "result %d packet %u slot %d buffer %u len %u crcerr %u\n",
				 diag->rx_result, diag->rx_received, diag->rx_slot,
				 diag->rx_buffer_id, diag->rx_len, diag->rx_crcerr);
		len += sysfs_emit_at(buf, len,
			"pending before %04x sampled %04x after %04x elapsed %lld us\n",
			diag->rx_pending_before, diag->rx_pending_sampled,
			diag->rx_pending_after, diag->rx_elapsed_us);
		len += sysfs_emit_at(buf, len, "gate active %08x after %08x\n",
				    diag->rx_gate_active, diag->rx_gate_after);
		len += sysfs_emit_at(buf, len, "mac active %08x after %08x\n",
				    diag->rx_mac_active, diag->rx_mac_after);
		if (diag->rx_received && diag->rx_slot >= 0) {
			len += sysfs_emit_at(buf, len,
				"descriptor: %08x %08x %08x %08x (strobe %u offset %u)\n",
				diag->rx_desc_words[0], diag->rx_desc_words[1],
				diag->rx_desc_words[2], diag->rx_desc_words[3],
				diag->rx_strobe, diag->rx_offset);
			if (diag->rx_sample_len >= 14) {
				const u8 *p = diag->rx_sample;

				len += sysfs_emit_at(buf, len,
					"dst: %pM src: %pM ethertype: %02x%02x\n",
					p, p + 6, p[12], p[13]);
			}
			if (diag->rx_sample_len > 0) {
				len += sysfs_emit_at(buf, len, "sample (%zu bytes):\n",
						    diag->rx_sample_len);
				for (i = 0; i < diag->rx_sample_len; i++) {
					if (i > 0 && !(i % 16))
						len += sysfs_emit_at(buf, len, "\n");
					len += sysfs_emit_at(buf, len, " %02x",
							    diag->rx_sample[i]);
				}
				len += sysfs_emit_at(buf, len, "\n");
			}
		}
		len += sysfs_emit_at(buf, len, "Not an index-reset or DMA-idle proof\n");
	}
	mutex_unlock(&diag->completion_lock);
	return len;
}

static ssize_t rx_once_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	int ret;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST))
		return -EOPNOTSUPP;
	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = mutex_lock_interruptible(&diag->completion_lock);
	if (ret)
		return ret;
	if (diag->tx_attempted || diag->rx_attempted) {
		mutex_unlock(&diag->completion_lock);
		return -EBUSY;
	}
	diag->rx_attempted = true;
	diag->rx_result = gc1601_rx_once(dev, diag);
	ret = diag->rx_result;
	mutex_unlock(&diag->completion_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(rx_once);

static ssize_t sram_init_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len;
	unsigned int i;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_SRAM_TEST))
		return sysfs_emit(buf, "disabled\n");
	mutex_lock(&diag->completion_lock);
	if (!diag->sram_attempted) {
		len = sysfs_emit(buf, "not run; write 1 once to initialize PM/OS SRAM; power-cycle afterward\n");
	} else {
		len = sysfs_emit(buf, "result %d, stages %u; nonzero responses are not a fresh-ack or DMA-idle proof\n",
				 diag->sram_result, diag->sram_stages);
		for (i = 0; i < diag->sram_stages; i++)
			len += sysfs_emit_at(buf, len, "%s: before %08x after %08x elapsed %lld us\n",
					    i ? "OS" : "PM", diag->sram_before[i],
					    diag->sram_after[i], diag->sram_time_us[i]);
	}
	mutex_unlock(&diag->completion_lock);
	return len;
}

static ssize_t sram_init_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	void __iomem *base = diag->base[GC1601_BUFFERS];
	unsigned int i;
	int ret;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_SRAM_TEST))
		return -EOPNOTSUPP;
	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = mutex_lock_interruptible(&diag->completion_lock);
	if (ret)
		return ret;
	if (diag->sram_attempted || !gc1601_completion_allowed(diag) ||
	    readl(base + 0x28) != 0x40800000 ||
	    readl(base + 0x34) != 0x40801000 ||
	    readl(base + 0x30) != 0x40901000 ||
	    readl(diag->base[GC1601_TX_RING]) != 0x40911000) {
		ret = -EBUSY;
		goto out;
	}
	diag->sram_attempted = true;
	writel(U32_MAX, base + 0x04);
	writel(U32_MAX, base + 0x44);
	for (i = 0; i < 2; i++) {
		void __iomem *request = base + 0x08 + i * 8;
		void __iomem *response = request + 4;
		ktime_t start;

		if (!gc1601_completion_allowed(diag)) {
			ret = -EBUSY;
			break;
		}
		diag->sram_before[i] = readl(response);
		start = ktime_get();
		writel(U32_MAX, request);
		ret = readl_poll_timeout(response, diag->sram_after[i],
					 diag->sram_after[i] != 0, 100, 50000);
		diag->sram_time_us[i] = ktime_us_delta(ktime_get(), start);
		diag->sram_stages++;
		if (ret)
			break;
	}
	if (!ret && !gc1601_completion_allowed(diag))
		ret = -EBUSY;
	diag->sram_result = ret;
out:
	mutex_unlock(&diag->completion_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(sram_init);

static ssize_t dma_test_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len;
	unsigned int i;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_DMA_TEST))
		return sysfs_emit(buf, "disabled\n");
	if (diag->test_result)
		return sysfs_emit(buf, "FAIL: %d; no hardware DMA enabled\n", diag->test_result);
	len = sysfs_emit(buf, "PASS: allocation, CPU readback and descriptor bounds; not device DMA\n");
	for (i = 0; i < ARRAY_SIZE(gc1601_dma_sizes); i++)
		len += sysfs_emit_at(buf, len, "%s: %zu bytes, tested DMA address %pad (freed)\n",
				    gc1601_dma_names[i], gc1601_dma_sizes[i],
				    &diag->test_addresses[i]);
	return len;
}
static DEVICE_ATTR_RO(dma_test);

static ssize_t loader_free_list_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	const __be16 *list = phys_to_virt(0x40800000);
	ssize_t len;
	unsigned int i;
	int ret;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_LOADER_STATE))
		return -EOPNOTSUPP;
	ret = mutex_lock_interruptible(&diag->completion_lock);
	if (ret)
		return ret;
	if (!gc1601_completion_allowed(diag) ||
	    readl(diag->base[GC1601_BUFFERS] + 0x28) != 0x40800000) {
		len = -EBUSY;
		goto out;
	}
	/* Read the existing linear mapping; do not flush or alias inherited DMA RAM. */
	len = sysfs_emit(buf,
			 "CPU view of 512 big-endian IDs at 40800000; not a free-count or coherency proof\n");
	for (i = 0; i < GC1601_BUFFER_COUNT; i++) {
		if (!(i % 16))
			len += sysfs_emit_at(buf, len, "%03x:", i * 2);
		len += sysfs_emit_at(buf, len, " %04x%s", be16_to_cpu(READ_ONCE(list[i])),
				    i % 16 == 15 ? "\n" : "");
	}
out:
	mutex_unlock(&diag->completion_lock);
	return len;
}
static DEVICE_ATTR_RO(loader_free_list);

static ssize_t gate_alloc_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_GATE_TEST))
		return sysfs_emit(buf, "disabled\n");
	mutex_lock(&diag->completion_lock);
	if (!diag->gate_attempted) {
		len = sysfs_emit(buf, "not run; write 1 for one RX-gate allocation\n");
	} else {
		len = sysfs_emit(buf, "result %d requested %u sampled %u elapsed %lld us\n",
				 diag->gate_result, diag->gate_requested,
				 diag->gate_sampled, diag->gate_time_us);
		len += sysfs_emit_at(buf, len, "gate before %08x enabled %08x after %08x\n",
				    diag->gate_before, diag->gate_enabled, diag->gate_after);
		len += sysfs_emit_at(buf, len, "request %08x response %08x mac-after %08x\n",
				    diag->gate_request, diag->gate_response, diag->gate_mac_after);
		if (diag->gate_sampled && (diag->gate_response & BIT(31)))
			len += sysfs_emit_at(buf, len, "buffer ID %u retained until power-off\n",
					    diag->gate_response & 0x7fffffff);
	}
	mutex_unlock(&diag->completion_lock);
	return len;
}

static ssize_t gate_alloc_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	int ret;

	if (!IS_ENABLED(CONFIG_GC1601_PACKET_GATE_TEST))
		return -EOPNOTSUPP;
	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = mutex_lock_interruptible(&diag->completion_lock);
	if (ret)
		return ret;
	if (diag->gate_attempted) {
		ret = -EBUSY;
		goto out;
	}
	diag->gate_attempted = true;
	ret = gc1601_gate_buffer_alloc(diag);
	diag->gate_result = ret;
out:
	mutex_unlock(&diag->completion_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(gate_alloc);

static ssize_t snapshot_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct gc1601_packet_diag *diag = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i, j;

	len += sysfs_emit_at(buf, len, "Read-only configuration snapshot; not an atomic DMA-idle check\n");
	for (i = 0; i < ARRAY_SIZE(gc1601_registers); i++) {
		const struct gc1601_registers *regs = &gc1601_registers[i];

		if (!diag->base[i])
			continue;
		for (j = 0; j < regs->count; j++) {
			resource_size_t address = diag->address[i] + regs->offsets[j];

			len += sysfs_emit_at(buf, len, "%s %pa: %08x\n", regs->name,
					    &address, readl(diag->base[i] + regs->offsets[j]));
		}
	}
	return len;
}
static DEVICE_ATTR_RO(snapshot);

static struct attribute *gc1601_packet_diag_attrs[] = {
	&dev_attr_snapshot.attr,
	&dev_attr_loader_free_list.attr,
	&dev_attr_gate_alloc.attr,
	&dev_attr_dma_test.attr,
	&dev_attr_tx_completion.attr,
	&dev_attr_sram_init.attr,
	&dev_attr_tx_once.attr,
	&dev_attr_rx_once.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gc1601_packet_diag);

static int gc1601_packet_diag_probe(struct platform_device *pdev)
{
	struct gc1601_packet_diag *diag;
	unsigned int i;

	if ((IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST) ||
	     IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST) ||
	     IS_ENABLED(CONFIG_GC1601_PACKET_LOADER_STATE)) &&
	    !of_machine_is_compatible("tplink,xz000-g7"))
		return -ENODEV;

	if (IS_ENABLED(CONFIG_GC1601_PACKET_SRAM_TEST) ||
	    IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST) ||
	    IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST) ||
	    IS_ENABLED(CONFIG_GC1601_PACKET_LOADER_STATE)) {
		struct device_node *node;
		struct reserved_mem *memory;
		bool valid;

		node = of_find_node_by_path("/reserved-memory/dma@40800000");
		if (!node)
			return -EINVAL;
		memory = of_reserved_mem_lookup(node);
		valid = memory && memory->base == 0x40800000 &&
			memory->size == 0x00112000 &&
			!of_property_read_bool(node, "no-map") &&
			!of_property_read_bool(node, "reusable");
		of_node_put(node);
		if (!valid)
			return -EINVAL;
		/* Check actual page ownership, not just the reservation's DT entry. */
		for (i = PHYS_PFN(memory->base);
		     i < PHYS_PFN(memory->base + memory->size); i++)
			if (!pfn_valid(i) || !PageReserved(pfn_to_page(i)))
				return -EINVAL;
	}
	diag = devm_kzalloc(&pdev->dev, sizeof(*diag), GFP_KERNEL);
	if (!diag)
		return -ENOMEM;
	mutex_init(&diag->completion_lock);
	diag->completion_result = -ENODATA;
	for (i = 0; i < ARRAY_SIZE(gc1601_registers); i++) {
		const struct gc1601_registers *regs = &gc1601_registers[i];
		struct resource *res;
		size_t minimum = regs->offsets[regs->count - 1] + 4;

		if (i == GC1601_TX_RING)
			minimum = 0x0c;
		if (i == GC1601_DESCRIPTORS && IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST))
			minimum = 0x68;
		if (i == GC1601_RX && (IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST) ||
				      IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST)))
			minimum = 0x3c;
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, regs->name);
		if (!res && i >= GC1601_BUFFER_CONTROL &&
		    !IS_ENABLED(CONFIG_GC1601_PACKET_LOADER_STATE) &&
		    !IS_ENABLED(CONFIG_GC1601_PACKET_TX_TEST) &&
		    !IS_ENABLED(CONFIG_GC1601_PACKET_RX_TEST))
			continue;
		if (!res || resource_size(res) < minimum)
			return -EINVAL;
		diag->address[i] = res->start;
		diag->base[i] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(diag->base[i]))
			return PTR_ERR(diag->base[i]);
	}
	platform_set_drvdata(pdev, diag);
	if (IS_ENABLED(CONFIG_GC1601_PACKET_DMA_TEST)) {
		diag->test_result = gc1601_dma_test(&pdev->dev, diag->test_addresses);
		dev_info(&pdev->dev, "DMA allocation test result %d; no hardware DMA enabled\n",
			 diag->test_result);
	}
	dev_info(&pdev->dev, "Packet diagnostic; no resets or DMA programming at probe\n");
	return 0;
}

static const struct of_device_id gc1601_packet_diag_match[] = {
	{ .compatible = "galachip,gc1601-packet-engine" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc1601_packet_diag_match);

static struct platform_driver gc1601_packet_diag_driver = {
	.probe = gc1601_packet_diag_probe,
	.driver = {
		.name = "gc1601-packet-diag",
		.of_match_table = gc1601_packet_diag_match,
		.dev_groups = gc1601_packet_diag_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(gc1601_packet_diag_driver);

MODULE_DESCRIPTION("GC1601 packet-engine bring-up diagnostics");
MODULE_LICENSE("GPL");
