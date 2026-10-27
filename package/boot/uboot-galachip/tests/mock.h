/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal driver-model/MMIO seam; production driver C is included unchanged. */
#ifndef GC1601_MOCK_H
#define GC1601_MOCK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned char uchar;
typedef uint16_t __be16;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define ENOTSUPP EOPNOTSUPP
#define MDIO_DEVAD_NONE -1
#define SPEED_1000 1000
#define SPEED_100 100
#define SPEED_10 10
#define DUPLEX_FULL 1
#define UCLASS_SPI 1
#define UCLASS_ETH 2
#define UCLASS_MDIO 3
#define UCLASS_CLK 4
#define DM_FLAG_PRE_RELOC 1
struct udevice { void *priv, *plat, *address; struct udevice *parent; };
struct udevice_id { const char *compatible; };
struct mock_driver {
	const char *name;
	int id, flags;
	const void *of_match, *ops;
	int (*probe)(struct udevice *);
	size_t priv_auto, plat_auto;
};
#define U_BOOT_DRIVER(name) struct mock_driver name
static inline void *dev_get_priv(struct udevice *d) { return d->priv; }
static inline void *dev_get_plat(struct udevice *d) { return d->plat; }
static inline void *dev_read_addr_ptr(struct udevice *d) { return d->address; }
static inline struct udevice *dev_get_parent(struct udevice *d) { return d->parent; }
struct clk { struct udevice *dev; unsigned long id; };
struct clk_ops { ulong (*get_rate)(struct clk *); };
static inline int clk_get_by_index(struct udevice *d, int i, struct clk *c)
{ (void)d; (void)i; (void)c; return 0; }
static inline ulong clk_get_rate(struct clk *c) { (void)c; return 200000000; }
#define IS_ERR_VALUE(a) ((ulong)(a) >= (ulong)-4095)
struct phy_device { int link, speed, duplex; };
extern struct phy_device mock_phy;
static inline struct phy_device *dm_eth_phy_connect(struct udevice *d)
{ (void)d; return &mock_phy; }
static inline int phy_config(struct phy_device *p) { (void)p; return 0; }
static inline int phy_startup(struct phy_device *p) { (void)p; return 0; }
static inline int phy_read(struct phy_device *p, int d, int r)
{ (void)p; (void)d; (void)r; return 0; }
static inline int phy_write(struct phy_device *p, int d, int r, u16 v)
{ (void)p; (void)d; (void)r; (void)v; return 0; }
struct eth_pdata { u8 enetaddr[6]; };
struct eth_ops {
	int (*start)(struct udevice *);
	void (*stop)(struct udevice *);
	int (*send)(struct udevice *, void *, int);
	int (*recv)(struct udevice *, int, uchar **);
	int (*write_hwaddr)(struct udevice *);
};
struct mdio_ops {
	int (*read)(struct udevice *, int, int, int);
	int (*write)(struct udevice *, int, int, int, u16);
};
struct spi_slave { struct udevice *dev; };
#define SPI_MEM_DATA_IN 1
#define SPI_MEM_DATA_OUT 2
struct spi_mem_op {
	struct { u8 nbytes, buswidth, opcode; } cmd;
	struct { u8 nbytes, buswidth; u64 val; } addr;
	struct { u8 nbytes, buswidth; } dummy;
	struct { u32 nbytes; u8 buswidth, dir; union { void *in; const void *out; } buf; } data;
};
static inline bool spi_mem_default_supports_op(struct spi_slave *s, const struct spi_mem_op *o)
{ (void)s; (void)o; return true; }
static inline int spi_chip_select(struct udevice *d) { (void)d; return 0; }
struct spi_controller_mem_ops {
	bool (*supports_op)(struct spi_slave *, const struct spi_mem_op *);
	int (*exec_op)(struct spi_slave *, const struct spi_mem_op *);
	int (*adjust_op_size)(struct spi_slave *, struct spi_mem_op *);
};
struct dm_spi_ops {
	int (*set_speed)(struct udevice *, uint);
	int (*set_mode)(struct udevice *, uint);
	const struct spi_controller_mem_ops *mem_ops;
};
u32 mock_read(uintptr_t addr);
void mock_write(u32 value, uintptr_t addr);
static inline u32 readl(const void *p) { return mock_read((uintptr_t)p); }
static inline void writel(u32 v, void *p) { mock_write(v, (uintptr_t)p); }
static inline void setbits_le32(void *p, u32 v) { writel(readl(p) | v, p); }
static inline void clrbits_le32(void *p, u32 v) { writel(readl(p) & ~v, p); }
static inline void clrsetbits_le32(void *p, u32 clear, u32 set)
{ writel((readl(p) & ~clear) | set, p); }
#define readl_poll_timeout(addr, value, condition, timeout) ({ \
	int result = -ETIMEDOUT; unsigned int polls; \
	(void)(timeout); \
	for (polls = 0; polls < 8; polls++) { \
		(value) = readl(addr); if (condition) { result = 0; break; } \
	} result; })
static inline void udelay(unsigned long us) { (void)us; }
static inline void mdelay(unsigned long ms) { (void)ms; }
#define cpu_to_be16(v) __builtin_bswap16(v)
static inline void dsb(void) {}
#endif
