// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <asm/mach/arch.h>

#define GC1601_SCU_DDR_DRV	0x83c
#define GC1601_SCU_DDR_DRV_MASK	GENMASK(20, 15)

/*
 * Pin function select, as the stock U-Boot leaves it. Without these bits the
 * GPIO block's outputs do not reach the pads after a full chip reset: on the
 * XZ000-G7 the LEDs (GPIO 0-2 and 13) then stay at their reset level.
 */
#define GC1601_SCU_PINMUX	0x81c
#define GC1601_SCU_PINMUX_STOCK	0x0800002a

static void __init gc1601_init_machine(void)
{
	struct regmap *scu;

	scu = syscon_regmap_lookup_by_compatible("galachip,gc1601-scu");
	if (IS_ERR(scu))
		return;

	/* As the stock kernel does: clear the SCU DDR drive bits 20:15. */
	regmap_clear_bits(scu, GC1601_SCU_DDR_DRV, GC1601_SCU_DDR_DRV_MASK);

	/* As the stock U-Boot does: route the GPIO block to its pads. */
	regmap_set_bits(scu, GC1601_SCU_PINMUX, GC1601_SCU_PINMUX_STOCK);
}

static const char *const gc1601_dt_compat[] = {
	"galachip,gc1601",
	NULL,
};

/* Reboot goes through the FTWDT010 watchdog's restart handler. */
DT_MACHINE_START(GC1601_DT, "GalaChip GC1601A")
	.dt_compat = gc1601_dt_compat,
	.init_machine = gc1601_init_machine,
MACHINE_END
