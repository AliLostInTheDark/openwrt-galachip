// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 packet engine: debugfs
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include "gc1601-eth.h"

/*
 * Engine state for bring-up and bug reports, in /sys/kernel/debug/gc1601-eth.
 * Counters that only grow are in ethtool -S instead.
 */
static int gc1601_dbg_diagnostics_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	unsigned long flags;
	int slot;

	spin_lock_irqsave(&priv->tx_lock, flags);
	slot = priv->tx_probe_aligned ? priv->tx_resume_slot : -1;
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	seq_printf(s, "pm_sram=%08x os_sram=%08x read_clear=%08x\n",
		   priv->pm_sram_status, priv->os_sram_status,
		   readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR));
	seq_printf(s, "tx_detected_slot=%d tx_detected_w3=%08x tx_resume_slot=%d rx_first_slot=%d\n",
		   priv->tx_detected_slot, priv->tx_detected_w3, slot,
		   priv->rx_first_slot);
	seq_printf(s, "tx_producer=%u tx_consumer=%u rx_consumer=%u tx_pending=%u\n",
		   READ_ONCE(priv->tx_producer), READ_ONCE(priv->tx_consumer),
		   READ_ONCE(priv->rx_consumer[0]), READ_ONCE(priv->tx_pending));
	seq_printf(s, "tx_completion_reads=%u tx_completed_total=%u tx_completion_last=%u tx_completion_drained=%u\n",
		   READ_ONCE(priv->tx_completion_reads),
		   READ_ONCE(priv->tx_completed_total),
		   READ_ONCE(priv->tx_completion_last),
		   READ_ONCE(priv->tx_completion_drained));
	seq_printf(s, "omci_last_words=%08x %08x %08x %08x\n",
		   READ_ONCE(priv->rx_omci_last_words[0]),
		   READ_ONCE(priv->rx_omci_last_words[1]),
		   READ_ONCE(priv->rx_omci_last_words[2]),
		   READ_ONCE(priv->rx_omci_last_words[3]));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_diagnostics);

static int gc1601_dbg_tx_ring_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&priv->tx_lock, flags);
	dma_rmb();
	for (i = 0; i < 8; i++) {
		struct gc1601_desc *desc = &priv->tx_ring[i];

		seq_printf(s, "%u: %08x %08x %08x %08x\n", i,
			   le32_to_cpu(READ_ONCE(desc->word[0])),
			   le32_to_cpu(READ_ONCE(desc->word[1])),
			   le32_to_cpu(READ_ONCE(desc->word[2])),
			   le32_to_cpu(READ_ONCE(desc->word[3])));
	}
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_tx_ring);

static int gc1601_dbg_tx_config_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	void __iomem *base = priv->base[GC1601_TX_CONFIG];

	/* Skip doorbells and completion counters: reads must not consume state. */
	seq_printf(s, "dma_base=%08x hw_base=%08x cfg00=%08x cfg08=%08x cfg0c=%08x cfg10=%08x cfg14=%08x\n",
		   (u32)priv->tx_ring_dma,
		   readl(priv->base[GC1601_TX_RING] + GC_TX_RING_BASE),
		   readl(base), readl(base + 8), readl(base + 0xc),
		   readl(base + 0x10), readl(base + 0x14));
	seq_printf(s, "probe_sent=%u probe_retries=%u probe_failed=%u\n",
		   READ_ONCE(priv->tx_probe_sent), READ_ONCE(priv->tx_probe_retries),
		   READ_ONCE(priv->tx_probe_failed));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_tx_config);

static int gc1601_dbg_rx_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	void __iomem *gmac = priv->base[GC1601_GMAC];
	void __iomem *buffers = priv->base[GC1601_BUFFERS];
	void __iomem *rx = priv->base[GC1601_RX];
	u16 cons = READ_ONCE(priv->rx_consumer[0]) % GC1601_RING_SIZE;
	struct gc1601_desc *desc = &priv->rx_ring[cons];
	u32 pending = readl(priv->base[GC1601_RX_PENDING]);
	unsigned int i;

	/* Approximate snapshot; no DMA invalidation or PMU completion reads. */
	seq_printf(s, "running=%u napi_state=%lx timer_pending=%u irq_mask=%08x irq_last_status=%08x\n",
		   READ_ONCE(priv->running), READ_ONCE(priv->napi.state),
		   timer_pending(&priv->poll_timer), readl(buffers + GC_BUF_INTR_MASK),
		   READ_ONCE(priv->irq_last_status));
	seq_printf(s, "irq_poll_only=%u irq_empty_streak=%u event_mask=%08x\n",
		   READ_ONCE(priv->irq_poll_only), READ_ONCE(priv->irq_empty_streak),
		   readl(buffers + GC_BUF_EVENT_MASK));
	seq_printf(s, "irq_calls=%u irq_empty_calls=%u irq_duplicate_calls=%u pending_raw=%08x\n",
		   READ_ONCE(priv->irq_calls), READ_ONCE(priv->irq_empty_calls),
		   READ_ONCE(priv->irq_duplicate_calls), pending);
	seq_printf(s, "storm_jiffies=%lu storm_pending=%08x storm_mask=%08x storm_event_mask=%08x storm_consumer=%u\n",
		   READ_ONCE(priv->irq_storm_jiffies), READ_ONCE(priv->irq_storm_pending),
		   READ_ONCE(priv->irq_storm_mask), READ_ONCE(priv->irq_storm_event_mask),
		   READ_ONCE(priv->irq_storm_consumer));
	seq_printf(s, "pending_now=%u pending_last=%u consumer=%u polls=%u empty_desc=%u ack_count=%u release_count=%u\n",
		   pending & 0xffff, READ_ONCE(priv->rx_pending_last), cons,
		   READ_ONCE(priv->rx_poll_calls), READ_ONCE(priv->rx_empty_desc),
		   READ_ONCE(priv->rx_ack_count), READ_ONCE(priv->rx_release_count));
	seq_printf(s, "last_slot=%u last_words=%08x %08x %08x %08x\n",
		   READ_ONCE(priv->rx_last_slot), READ_ONCE(priv->rx_last_words[0]),
		   READ_ONCE(priv->rx_last_words[1]), READ_ONCE(priv->rx_last_words[2]),
		   READ_ONCE(priv->rx_last_words[3]));
	seq_printf(s, "cached_current_words=%08x %08x %08x %08x\n",
		   le32_to_cpu(READ_ONCE(desc->word[0])), le32_to_cpu(READ_ONCE(desc->word[1])),
		   le32_to_cpu(READ_ONCE(desc->word[2])), le32_to_cpu(READ_ONCE(desc->word[3])));
	seq_printf(s, "gmac=%08x ier=%08x maccr=%08x ercr=%08x acir=%08x rx_enable=%08x rx_partition=%08x rx_ctrl=%08x\n",
		   readl(gmac + GC_GMAC_CFG), readl(gmac + GC_GMAC_IER),
		   readl(gmac + GC_GMAC_MACCR), readl(gmac + GC_GMAC_ERCR),
		   readl(gmac + GC_GMAC_ACIR), readl(rx + GC_RX_ENABLE),
		   readl(rx + GC_RX_PARTITION), readl(rx + GC_RX_CTRL));
	seq_printf(s, "buf_ctrl=%08x pm_ctrl=%08x pool_cfg=%08x free_base=%08x pool_base=%08x rx_base=%08x\n",
		   readl(buffers + GC_BUF_CTRL), readl(buffers + GC_BUF_PM_CTRL),
		   readl(buffers + GC_BUF_POOL_CFG), readl(buffers + GC_BUF_FREE_LIST_BASE),
		   readl(buffers + GC_BUF_PKT_POOL_BASE), readl(buffers + GC_BUF_RX_RING_BASE));
	if (priv->switch_base) {
		void __iomem *l2 = priv->switch_base + GC_SR_L2;

		seq_printf(s, "sr_a0=%08x sr_a4=%08x sr_188=%08x sr_18c=%08x sr_198=%08x sr_19c=%08x gmac_300=%08x gmac_304=%08x\n",
			   readl(l2 + 0xa0), readl(l2 + 0xa4), readl(l2 + 0x188),
			   readl(l2 + 0x18c), readl(l2 + 0x198), readl(l2 + 0x19c),
			   readl(gmac + 0x300), readl(gmac + 0x304));
	}
	/* GMAC statistics, by offset until each counter is identified. */
	for (i = 0; i < ARRAY_SIZE(gc1601_diag_mac); i++)
		seq_printf(s, "mac_%03x=%u%c", gc1601_diag_mac[i],
			   readl(gmac + gc1601_diag_mac[i]),
			   (i % 5 == 4 || i + 1 == ARRAY_SIZE(gc1601_diag_mac)) ? '\n' : ' ');
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_rx);

static int gc1601_dbg_queues_show(struct seq_file *s, void *unused)
{
	static const u16 queue_bases[] = { 0x000, 0x008, 0x010, 0x018, 0x140, 0x180, 0x188 };
	struct gc1601_eth *priv = s->private;
	u32 outmax, outcnt, inmax, incnt, cfg[3], allcnt;
	unsigned int bank, queue, index;
	u16 cur_out, cur_in, cur_all;
	u32 iptr;
	int ret = 0;

	/* Setup is the only other software user of this table interface. */
	if (!rtnl_trylock())
		return -EBUSY;
	if (!READ_ONCE(priv->running)) {
		ret = -ENETDOWN;
		goto out;
	}
	if (priv->queue_read_failed) {
		seq_puts(s, "blocked_after_timeout=1; reboot before retry\n");
		goto out;
	}

	iptr = readl(priv->base[GC1601_TX_SPACE]) & 0x3ff;
	if (iptr < priv->min_left_iptr)
		priv->min_left_iptr = iptr;
	seq_printf(s, "setup_mismatches=%u left_iptr=%u min_left_iptr=%u\n",
		   priv->cvrd_mismatches, iptr, priv->min_left_iptr);

	for (bank = 0; bank < ARRAY_SIZE(queue_bases); bank++) {
		for (queue = 0; queue < 8; queue++) {
			index = queue_bases[bank] + queue;
			ret = gc1601_queue_read(priv, GC_CVRD_OUTMAX, index, &outmax, 1) ?:
			      gc1601_queue_read(priv, GC_CVRD_OUTCNT, index, &outcnt, 1);
			if (ret)
				goto failed;

			cur_out = GC_CVRD_HI(outcnt);
			if (cur_out > priv->peak_out[index])
				priv->peak_out[index] = cur_out;

			if (index >= GC_CVRD_CPU) {
				seq_printf(s, "q=%03x out %u/%u(cur %u,pk %u)\n", index,
					   GC_CVRD_LO(outmax), GC_CVRD_HI(outmax), cur_out,
					   priv->peak_out[index]);
				continue;
			}

			ret = gc1601_queue_read(priv, GC_CVRD_INMAX, index, &inmax, 1) ?:
			      gc1601_queue_read(priv, GC_CVRD_INCNT, index, &incnt, 1) ?:
			      gc1601_queue_read(priv, GC_CVRD_ALLCFG, index, cfg, 3) ?:
			      gc1601_queue_read(priv, GC_CVRD_ALLCNT, index, &allcnt, 1);
			if (ret)
				goto failed;

			cur_in = GC_CVRD_HI(incnt);
			if (cur_in > priv->peak_in[index])
				priv->peak_in[index] = cur_in;
			cur_all = allcnt & 0xffff;
			if (cur_all > priv->peak_all[index])
				priv->peak_all[index] = cur_all;

			seq_printf(s, "q=%03x out %u/%u(cur %u,pk %u) in %u/%u(cur %u,pk %u) all %u(cur %u,pk %u)\n",
				   index, GC_CVRD_LO(outmax), GC_CVRD_HI(outmax),
				   cur_out, priv->peak_out[index],
				   GC_CVRD_LO(inmax), GC_CVRD_HI(inmax),
				   cur_in, priv->peak_in[index],
				   cfg[0] & 0xffff, cur_all, priv->peak_all[index]);
		}
	}
	goto out;
failed:
	seq_printf(s, "index=%03x error=%d; further reads blocked\n", index, ret);
	ret = 0;
out:
	rtnl_unlock();
	return ret;
}

static int gc1601_dbg_queues_open(struct inode *inode, struct file *file)
{
	return single_open(file, gc1601_dbg_queues_show, inode->i_private);
}

/* Any write clears the peaks. */
static ssize_t gc1601_dbg_queues_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct gc1601_eth *priv = file_inode(file)->i_private;

	rtnl_lock();
	memset(priv->peak_in, 0, sizeof(priv->peak_in));
	memset(priv->peak_out, 0, sizeof(priv->peak_out));
	memset(priv->peak_all, 0, sizeof(priv->peak_all));
	priv->min_left_iptr = 0x3ff;
	rtnl_unlock();
	return count;
}

static const struct file_operations gc1601_dbg_queues_fops = {
	.owner = THIS_MODULE,
	.open = gc1601_dbg_queues_open,
	.read = seq_read,
	.write = gc1601_dbg_queues_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void gc1601_dbg_id_ranges(struct seq_file *s, const char *name,
				 const unsigned long *map)
{
	unsigned int start, end;

	seq_printf(s, "%s distinct=%u ranges=", name,
		   bitmap_weight(map, GC1601_BUFFER_COUNT));
	for_each_set_bitrange(start, end, map, GC1601_BUFFER_COUNT)
		seq_printf(s, " %u-%u", start, end - 1);
	seq_putc(s, '\n');
}

static int gc1601_dbg_buffer_ids_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;

	gc1601_dbg_id_ranges(s, "rx", priv->rx_id_seen);
	gc1601_dbg_id_ranges(s, "tx", priv->tx_id_seen);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_buffer_ids);

/* CPU view of the loader free-list arena; no hardware registers are touched. */
static int gc1601_dbg_free_list_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	const __be16 *list = phys_to_virt(GC1601_FREE_LIST_PHYS);
	unsigned int i, n = min_t(unsigned int, GC1601_BUFFER_COUNT, 768);

	dma_sync_single_for_cpu(priv->dev, GC1601_FREE_LIST_PHYS,
				n * sizeof(__be16), DMA_FROM_DEVICE);
	for (i = 0; i < n; i++)
		seq_printf(s, "%04x%c", be16_to_cpu(list[i]),
			   i % 16 == 15 || i + 1 == n ? '\n' : ' ');
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_free_list);

/* Read one row of the statistics table; reads may clear its counters. */
static int gc1601_stt_read(struct gc1601_eth *priv, u32 sel, u32 *words,
			   unsigned int count)
{
	void __iomem *base = priv->base[GC1601_STATISTICS];
	unsigned int i;
	u32 done;
	int ret;

	writel(0, base + GC_DESC_TABLE_DONE);
	writel(GC_DESC_TABLE_READ | sel, base + GC_DESC_TABLE_INDEX);
	ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done, done == 1, 1, 1000);
	if (ret)
		return ret;
	for (i = 0; i < count; i++)
		words[i] = readl(base + GC_DESC_TABLE_DATA0 + i * sizeof(u32));
	return 0;
}

static int gc1601_dbg_ingress_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	unsigned int index, word;
	u32 credit, iptr, words[12];
	int ret = 0;

	if (!rtnl_trylock())
		return -EBUSY;
	if (!READ_ONCE(priv->running)) {
		ret = -ENETDOWN;
		goto out;
	}
	credit = readl(priv->base[GC1601_TX_SPACE]);
	iptr = credit & 0x3ff;
	if (iptr < priv->min_left_iptr)
		priv->min_left_iptr = iptr;

	seq_printf(s, "left_iptr_raw=%08x left_iptr=%u min_left_iptr=%u read_clear=%u\n",
		   credit, iptr, priv->min_left_iptr,
		   !!(readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR) &
		      GC_BUF_READ_CLEAR_EN));
	if (priv->ingress_read_failed) {
		seq_puts(s, "blocked_after_timeout=1; reboot before retry\n");
		goto out;
	}
	/* The 16 drop-reason rows, BIT(22) selecting that bank. */
	for (index = 0; index < 16; index++) {
		if (gc1601_stt_read(priv, BIT(22) | index, words, ARRAY_SIZE(words))) {
			priv->ingress_read_failed = true;
			seq_printf(s, "dr=%u timed out; further reads blocked\n", index);
			goto out;
		}
		seq_printf(s, "dr=%02u %s words=", index,
			   memchr_inv(words, 0, sizeof(words)) ? "[DROPS]" : "       ");
		for (word = 0; word < ARRAY_SIZE(words); word++)
			seq_printf(s, "%08x%c", words[word],
				   word == ARRAY_SIZE(words) - 1 ? '\n' : ',');
	}
out:
	rtnl_unlock();
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_ingress);

/* The row read is the one last written to stats_index. */
static int gc1601_dbg_stats_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	unsigned int index = READ_ONCE(priv->stats_index), word;
	u32 words[GC_STT_STATS_WORDS];
	int ret = 0;

	if (!rtnl_trylock())
		return -EBUSY;
	if (!READ_ONCE(priv->running)) {
		ret = -ENETDOWN;
		goto out;
	}
	if (priv->stats_read_failed) {
		ret = -EIO;
		goto out;
	}
	if (gc1601_stt_read(priv, index, words, ARRAY_SIZE(words))) {
		priv->stats_read_failed = true;
		seq_printf(s, "index=%u timed out; reboot before retry\n", index);
		goto out;
	}
	seq_printf(s, "index=%u words=", index);
	for (word = 0; word < ARRAY_SIZE(words); word++)
		seq_printf(s, "%08x%c", words[word],
			   word == ARRAY_SIZE(words) - 1 ? '\n' : ',');
out:
	rtnl_unlock();
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_stats);

static int gc1601_dbg_stats_index_get(void *data, u64 *val)
{
	struct gc1601_eth *priv = data;

	*val = READ_ONCE(priv->stats_index);
	return 0;
}

static int gc1601_dbg_stats_index_set(void *data, u64 val)
{
	struct gc1601_eth *priv = data;

	if (val >= GC_STT_STATS_COUNT)
		return -EINVAL;
	WRITE_ONCE(priv->stats_index, val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(gc1601_dbg_stats_index_fops, gc1601_dbg_stats_index_get,
			 gc1601_dbg_stats_index_set, "%llu\n");

static int gc1601_dbg_sfu_show(struct seq_file *s, void *unused)
{
	struct gc1601_eth *priv = s->private;
	void __iomem *l3, *src;
	unsigned int i, count;
	u32 w[15];
	int ret;

	if (!rtnl_trylock())
		return -EBUSY;
	if (!priv->switch_base || !priv->running) {
		seq_puts(s, "switch unavailable\n");
		goto out;
	}

	l3 = priv->switch_base + GC_SR_L3;
	src = priv->switch_base + GC_SR_SRC;
	seq_printf(s, "ready=%u rules=%u services=%u hashed=%u lookup=%08x fallback=%08x class9=%08x\n",
		   priv->sr_ready, priv->sfu_rules, priv->sfu_flows,
		   priv->sfu_hashed_count, readl(l3), readl(l3 + 0x0c),
		   readl(l3 + 0xe8));
	seq_printf(s, "src=%08x/%08x/%08x/%08x src8=%08x\n",
		   readl(src + 0x60), readl(src + 0x64), readl(src + 0x70),
		   readl(src + 0x74), readl(src + 0x08));
	if (priv->sfu_read_failed) {
		seq_puts(s, "table reads blocked after timeout\n");
		goto out;
	}

	count = min(2 * priv->sfu_flows, (unsigned int)GC_SR_FLOW_SLOTS);
	for (i = 0; i < count + priv->sfu_hashed_count; i++) {
		u32 table = i < count ? 8 : priv->sfu_hashed[i - count] >> 16;
		u32 index = i < count ? i : priv->sfu_hashed[i - count] & 0xffff;
		bool up = i < count ? !(i & 1) : priv->sfu_hashed_up[i - count];

		ret = gc1601_sr_tbl_read(priv, table, index, w, ARRAY_SIZE(w));
		if (ret) {
			priv->sfu_read_failed = true;
			seq_printf(s, "table=%u index=%u error=%d; further reads blocked\n",
				   table, index, ret);
			break;
		}
		seq_printf(s, "table=%u index=%u direction=%s valid=%u words=%08x,%08x,%08x,%08x,%08x\n",
			   table, index, up ? "up" : "down",
			   !!(w[3] & GC_SR_FLOW_VALID), w[0], w[1], w[3], w[4], w[5]);
	}
out:
	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_sfu);

static int gc1601_dbg_diag_ms_get(void *data, u64 *val)
{
	struct gc1601_eth *priv = data;

	*val = READ_ONCE(priv->diag_ms);
	return 0;
}

static int gc1601_dbg_diag_ms_set(void *data, u64 val)
{
	struct gc1601_eth *priv = data;

	if (val > MSEC_PER_SEC * 60)
		return -EINVAL;
	WRITE_ONCE(priv->diag_ms, val);
	if (val)
		mod_delayed_work(system_wq, &priv->diag_work, 0);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(gc1601_dbg_diag_ms_fops, gc1601_dbg_diag_ms_get,
			 gc1601_dbg_diag_ms_set, "%llu\n");

static int gc1601_dbg_tcont_depth_get(void *data, u64 *val)
{
	*val = gc1601_tcont_depth;
	return 0;
}

static int gc1601_dbg_tcont_depth_set(void *data, u64 val)
{
	return gc1601_tcont_limit_set(data, &gc1601_tcont_depth, val);
}
DEFINE_DEBUGFS_ATTRIBUTE(gc1601_dbg_tcont_depth_fops, gc1601_dbg_tcont_depth_get,
			 gc1601_dbg_tcont_depth_set, "%llu\n");

static int gc1601_dbg_tcont_out_get(void *data, u64 *val)
{
	*val = gc1601_tcont_out;
	return 0;
}

static int gc1601_dbg_tcont_out_set(void *data, u64 val)
{
	return gc1601_tcont_limit_set(data, &gc1601_tcont_out, val);
}
DEFINE_DEBUGFS_ATTRIBUTE(gc1601_dbg_tcont_out_fops, gc1601_dbg_tcont_out_get,
			 gc1601_dbg_tcont_out_set, "%llu\n");

void gc1601_debugfs_init(struct gc1601_eth *priv)
{
	struct dentry *dir = debugfs_create_dir("gc1601-eth", NULL);

	priv->debugfs = dir;
	debugfs_create_file("diagnostics", 0400, dir, priv, &gc1601_dbg_diagnostics_fops);
	debugfs_create_file("tx_ring", 0400, dir, priv, &gc1601_dbg_tx_ring_fops);
	debugfs_create_file("tx_config", 0400, dir, priv, &gc1601_dbg_tx_config_fops);
	debugfs_create_file("rx", 0400, dir, priv, &gc1601_dbg_rx_fops);
	debugfs_create_file("queues", 0600, dir, priv, &gc1601_dbg_queues_fops);
	debugfs_create_file("ingress", 0400, dir, priv, &gc1601_dbg_ingress_fops);
	debugfs_create_file("stats", 0400, dir, priv, &gc1601_dbg_stats_fops);
	debugfs_create_file_unsafe("stats_index", 0600, dir, priv,
				   &gc1601_dbg_stats_index_fops);
	debugfs_create_file("sfu", 0400, dir, priv, &gc1601_dbg_sfu_fops);
	debugfs_create_file("buffer_ids", 0400, dir, priv, &gc1601_dbg_buffer_ids_fops);
	debugfs_create_file("free_list", 0400, dir, priv, &gc1601_dbg_free_list_fops);
	debugfs_create_file_unsafe("diag_ms", 0600, dir, priv, &gc1601_dbg_diag_ms_fops);
	debugfs_create_u32("pool_warn", 0600, dir, &gc1601_pool_warn);
	debugfs_create_u32("pon_desc_log", 0600, dir, &gc1601_pon_desc_log);
	/* T-CONT queue input depth and queue 0 output limit, in buffers (32-256). */
	debugfs_create_file_unsafe("tcont_depth", 0600, dir, priv, &gc1601_dbg_tcont_depth_fops);
	debugfs_create_file_unsafe("tcont_out", 0600, dir, priv, &gc1601_dbg_tcont_out_fops);
}
