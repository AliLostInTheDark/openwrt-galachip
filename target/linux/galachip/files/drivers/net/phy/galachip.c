// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 internal gigabit PHY.
 *
 * Applies the analog tuning the boot loader leaves out, and soft-resets the
 * PHY when the link stays down, which it sometimes needs to link up again.
 * Both follow what the stock firmware does.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/regmap.h>

#define GC1601_PHY_ID		0x16010000
#define GC1601_PAGE_SEL		0x1f

/* SCU strap bit 7: set while the cable is unplugged. */
#define GC1601_SCU_STRAP	0x800
#define GC1601_SCU_CABOFF	BIT(7)

/* GE SerDes debug status bit 11: checked on 100M link-up, as stock. */
#define GC1601_SERDES_DBG	0x8
#define GC1601_SERDES_100M_OK	BIT(11)

/* Link-down polls before a reset, and resets before giving up until link. */
#define GC1601_DOWN_POLLS	10
#define GC1601_RESETS		10

struct gc1601_phy_priv {
	struct regmap *scu;
	struct regmap *ge_serdes;
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
	ret = phy_write_paged(phydev, 1, 0x11, 0x1ce7);
	if (ret)
		return ret;
	ret = phy_modify_paged(phydev, 1, 0x1e, 0, BIT(14));
	if (ret)
		return ret;
	ret = phy_modify_paged(phydev, 0, 0x10, 0, 0x300);
	if (ret)
		return ret;
	return phy_modify_paged(phydev, 0, 0x12, ~0x07ff, 0x5000);
}

static bool gc1601_reg_test(struct regmap *map, unsigned int reg, u32 bit)
{
	unsigned int val;

	return map && !regmap_read(map, reg, &val) && (val & bit);
}

/* Both checks are optional: without the phandle the PHY simply skips them. */
static struct regmap *gc1601_phy_syscon(struct phy_device *phydev, const char *prop)
{
	struct regmap *map;

	map = syscon_regmap_lookup_by_phandle(phydev->mdio.dev.of_node, prop);
	return IS_ERR(map) ? NULL : map;
}

static int gc1601_phy_probe(struct phy_device *phydev)
{
	struct gc1601_phy_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->scu = gc1601_phy_syscon(phydev, "galachip,scu");
	priv->ge_serdes = gc1601_phy_syscon(phydev, "galachip,ge-serdes");
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
		if (phydev->speed == SPEED_100 && priv->ge_serdes &&
		    !gc1601_reg_test(priv->ge_serdes, GC1601_SERDES_DBG,
				     GC1601_SERDES_100M_OK)) {
			phydev_dbg(phydev, "100M link without SerDes bit 11: resetting the PHY\n");
			ret = phy_set_bits(phydev, MII_BMCR, BMCR_RESET);
			if (ret)
				return ret;
			return gc1601_phy_tune(phydev);
		}
		return 0;
	}
	if (gc1601_reg_test(priv->scu, GC1601_SCU_STRAP, GC1601_SCU_CABOFF))
		return 0;
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
		.name		= "GalaChip GC1601 Gigabit PHY",
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
