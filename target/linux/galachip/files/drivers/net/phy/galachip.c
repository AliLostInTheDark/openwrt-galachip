// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 internal gigabit PHY.
 *
 * Ported from the stock xpon_sdk faraday_1601_config_init and
 * faraday_1601_read_status: the vendor tuning U-Boot does not apply, and a
 * soft reset when the link stays down, which the PHY sometimes needs to
 * link up again.
 */

#include <linux/module.h>
#include <linux/phy.h>

#define GC1601_PHY_ID		0x16010000
#define GC1601_PAGE_SEL		0x1f

/* Link-down polls before a reset, and resets before giving up until link. */
#define GC1601_DOWN_POLLS	10
#define GC1601_RESETS		10

struct gc1601_phy_priv {
	unsigned int down_polls;
	unsigned int resets_left;
};

static int gc1601_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, GC1601_PAGE_SEL);
}

static int gc1601_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, GC1601_PAGE_SEL, page);
}

static int gc1601_phy_tune(struct phy_device *phydev)
{
	int ret;

	ret = phy_modify_paged(phydev, 2, 0x14, 0x0fff, 0x492);
	if (ret)
		return ret;
	ret = phy_modify_paged(phydev, 1, 0x1e, 0, BIT(14));
	if (ret)
		return ret;
	ret = phy_modify_paged(phydev, 0, 0x10, 0, 0x300);
	if (ret)
		return ret;
	return phy_modify_paged(phydev, 0, 0x12, 0, BIT(11));
}

static int gc1601_phy_probe(struct phy_device *phydev)
{
	struct gc1601_phy_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->resets_left = GC1601_RESETS;
	phydev->priv = priv;
	return 0;
}

static int gc1601_phy_config_init(struct phy_device *phydev)
{
	return gc1601_phy_tune(phydev);
}

static int gc1601_phy_read_status(struct phy_device *phydev)
{
	struct gc1601_phy_priv *priv = phydev->priv;
	int ret;

	ret = genphy_read_status(phydev);
	if (ret)
		return ret;

	if (phydev->link) {
		priv->down_polls = 0;
		priv->resets_left = GC1601_RESETS;
		return 0;
	}
	if (phydev->autoneg != AUTONEG_ENABLE || !priv->resets_left ||
	    ++priv->down_polls <= GC1601_DOWN_POLLS)
		return 0;

	priv->down_polls = 0;
	priv->resets_left--;
	phydev_dbg(phydev, "link still down: resetting the PHY\n");
	ret = phy_set_bits(phydev, MII_BMCR, BMCR_RESET);
	if (ret)
		return ret;
	/* As stock: autoneg restarts from reset defaults; put the tuning back. */
	return gc1601_phy_tune(phydev);
}

static struct phy_driver gc1601_phy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(GC1601_PHY_ID),
		.name		= "GalaChip GC1601",
		.probe		= gc1601_phy_probe,
		.config_init	= gc1601_phy_config_init,
		.read_status	= gc1601_phy_read_status,
		.read_page	= gc1601_read_page,
		.write_page	= gc1601_write_page,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
	},
};
module_phy_driver(gc1601_phy_driver);

static const struct mdio_device_id __maybe_unused gc1601_phy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(GC1601_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, gc1601_phy_tbl);

MODULE_DESCRIPTION("GalaChip GC1601 PHY driver");
MODULE_LICENSE("GPL");
