// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 switch: the packet engine's ports as a DSA switch.
 *
 * The packet engine driver (gc1601-eth) is the conduit and owns the
 * engine; this driver gives the LAN and PON ports their netdevs and
 * phylink. Frames reach a port through TX descriptor bits, set by the
 * conduit from the gc1601 tag, and carry their ingress port up as port
 * metadata. The PON driver picks a PON frame's GEM port and reports the
 * PON port's link: up while the ONU is registered with the OLT.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/soc/galachip/gc1601.h>
#include <net/dsa.h>

static enum dsa_tag_protocol gc1601_get_tag_protocol(struct dsa_switch *ds, int port,
						     enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_GC1601;
}

/* Fails until the conduit has probed, so DSA retries the switch later. */
static int gc1601_setup(struct dsa_switch *ds)
{
	return gc1601_pe_lan_attach(NULL);
}

static void gc1601_teardown(struct dsa_switch *ds)
{
	gc1601_pe_lan_detach();
}

static int gc1601_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy)
{
	return port == GC1601_PORT_LAN ? gc1601_pe_lan_attach(phy) : 0;
}

static void gc1601_port_disable(struct dsa_switch *ds, int port)
{
	if (port == GC1601_PORT_LAN)
		gc1601_pe_lan_detach();
}

static void gc1601_phylink_get_caps(struct dsa_switch *ds, int port,
				    struct phylink_config *config)
{
	config->mac_capabilities = MAC_1000FD;
	if (port == GC1601_PORT_CPU || port == GC1601_PORT_PON) {
		/* GPON runs at 2.488 Gbit/s downstream, EPON at 1 Gbit/s. */
		if (port == GC1601_PORT_PON)
			config->mac_capabilities |= MAC_2500FD;
		__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
		return;
	}
	config->mac_capabilities |= MAC_1000HD | MAC_100 | MAC_10 |
				    MAC_SYM_PAUSE | MAC_ASYM_PAUSE;
	__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
}

/* The GMAC follows the link; there is nothing to set before it is up. */
static void gc1601_mac_config(struct phylink_config *config, unsigned int mode,
			      const struct phylink_link_state *state)
{
}

static void gc1601_mac_link_down(struct phylink_config *config, unsigned int mode,
				 phy_interface_t interface)
{
	if (dsa_phylink_to_port(config)->index == GC1601_PORT_LAN)
		gc1601_pe_lan_link_down();
}

static void gc1601_mac_link_up(struct phylink_config *config, struct phy_device *phydev,
			       unsigned int mode, phy_interface_t interface,
			       int speed, int duplex, bool tx_pause, bool rx_pause)
{
	if (dsa_phylink_to_port(config)->index == GC1601_PORT_LAN)
		gc1601_pe_lan_link_up(speed, duplex, tx_pause, rx_pause);
}

/* The PON port's fixed link, polled: up at the line rate while registered. */
static void gc1601_phylink_fixed_state(struct dsa_switch *ds, int port,
				       struct phylink_link_state *state)
{
	int speed;

	if (port != GC1601_PORT_PON)
		return;
	speed = gc1601_pe_pon_link();
	state->link = speed > 0;
	if (speed > 0)
		state->speed = speed;
}

static int gc1601_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS || port != GC1601_PORT_PON)
		return 0;
	return gc1601_pe_pon_sset_count();
}

static void gc1601_get_strings(struct dsa_switch *ds, int port, u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS && port == GC1601_PORT_PON)
		gc1601_pe_pon_strings(data);
}

static void gc1601_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data)
{
	if (port == GC1601_PORT_PON)
		gc1601_pe_pon_stats(data);
}

/* The conduit's buffers fit GC1601_MAX_MTU; the switch needs no setting. */
static int gc1601_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	return 0;
}

static int gc1601_port_max_mtu(struct dsa_switch *ds, int port)
{
	return GC1601_MAX_MTU;
}

static const struct phylink_mac_ops gc1601_phylink_mac_ops = {
	.mac_config = gc1601_mac_config,
	.mac_link_down = gc1601_mac_link_down,
	.mac_link_up = gc1601_mac_link_up,
};

static const struct dsa_switch_ops gc1601_switch_ops = {
	.get_tag_protocol = gc1601_get_tag_protocol,
	.setup = gc1601_setup,
	.teardown = gc1601_teardown,
	.port_enable = gc1601_port_enable,
	.port_disable = gc1601_port_disable,
	.phylink_get_caps = gc1601_phylink_get_caps,
	.phylink_fixed_state = gc1601_phylink_fixed_state,
	.get_sset_count = gc1601_get_sset_count,
	.get_strings = gc1601_get_strings,
	.get_ethtool_stats = gc1601_get_ethtool_stats,
	.port_change_mtu = gc1601_port_change_mtu,
	.port_max_mtu = gc1601_port_max_mtu,
};

static int gc1601_switch_probe(struct platform_device *pdev)
{
	struct dsa_switch *ds;

	ds = devm_kzalloc(&pdev->dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;

	ds->dev = &pdev->dev;
	ds->num_ports = GC1601_PORTS;
	ds->ops = &gc1601_switch_ops;
	ds->phylink_mac_ops = &gc1601_phylink_mac_ops;
	platform_set_drvdata(pdev, ds);

	return dsa_register_switch(ds);
}

static void gc1601_switch_remove(struct platform_device *pdev)
{
	struct dsa_switch *ds = platform_get_drvdata(pdev);

	if (ds)
		dsa_unregister_switch(ds);
}

static void gc1601_switch_shutdown(struct platform_device *pdev)
{
	struct dsa_switch *ds = platform_get_drvdata(pdev);

	if (ds)
		dsa_switch_shutdown(ds);
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id gc1601_switch_of_match[] = {
	{ .compatible = "galachip,gc1601-switch" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc1601_switch_of_match);

static struct platform_driver gc1601_switch_driver = {
	.probe = gc1601_switch_probe,
	.remove = gc1601_switch_remove,
	.shutdown = gc1601_switch_shutdown,
	.driver = {
		.name = "gc1601-switch",
		.of_match_table = gc1601_switch_of_match,
	},
};
module_platform_driver(gc1601_switch_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("GalaChip GC1601 switch");
MODULE_LICENSE("GPL");
