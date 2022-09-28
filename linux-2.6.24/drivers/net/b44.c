/* b44.c: Broadcom 44xx/47xx Fast Ethernet device driver.
 *
 * Copyright (C) 2002 David S. Miller (davem@redhat.com)
 * Copyright (C) 2004 Pekka Pietikainen (pp@ee.oulu.fi)
 * Copyright (C) 2004 Florian Schirmer (jolt@tuxbox.org)
 * Copyright (C) 2006 Felix Fietkau (nbd@openwrt.org)
 * Copyright (C) 2006 Broadcom Corporation.
 * Copyright (C) 2007 Michael Buesch <mb@bu3sch.de>
 *
 * Distribute under GPL.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/ssb/ssb.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/irq.h>


#include "b44.h"

#define DRV_MODULE_NAME		"b44"
#define PFX DRV_MODULE_NAME	": "
#define DRV_MODULE_VERSION	"2.0"

#define B44_DEF_MSG_ENABLE	  \
	(NETIF_MSG_DRV		| \
	 NETIF_MSG_PROBE	| \
	 NETIF_MSG_LINK		| \
	 NETIF_MSG_TIMER	| \
	 NETIF_MSG_IFDOWN	| \
	 NETIF_MSG_IFUP		| \
	 NETIF_MSG_RX_ERR	| \
	 NETIF_MSG_TX_ERR)

/* length of time before we decide the hardware is borked,
 * and dev->tx_timeout() should be called to fix the problem
 */
#define B44_TX_TIMEOUT			(5 * HZ)

/* hardware minimum and maximum for a single frame's data payload */
#define B44_MIN_MTU			60
#define B44_MAX_MTU			1500

#define B44_RX_RING_SIZE		512
#define B44_DEF_RX_RING_PENDING		200
#define B44_RX_RING_BYTES	(sizeof(struct dma_desc) * \
				 B44_RX_RING_SIZE)
#define B44_TX_RING_SIZE		512
#define B44_DEF_TX_RING_PENDING		(B44_TX_RING_SIZE - 1)
#define B44_TX_RING_BYTES	(sizeof(struct dma_desc) * \
				 B44_TX_RING_SIZE)

#define TX_RING_GAP(BP)	\
	(B44_TX_RING_SIZE - (BP)->tx_pending)
#define TX_BUFFS_AVAIL(BP)						\
	(((BP)->tx_cons <= (BP)->tx_prod) ?				\
	  (BP)->tx_cons + (BP)->tx_pending - (BP)->tx_prod :		\
	  (BP)->tx_cons - (BP)->tx_prod - TX_RING_GAP(BP))
#define NEXT_TX(N)		(((N) + 1) & (B44_TX_RING_SIZE - 1))

#define RX_PKT_OFFSET		30
#define RX_PKT_BUF_SZ		(1536 + RX_PKT_OFFSET + 64)

/* minimum number of free TX descriptors required to wake up TX process */
#define B44_TX_WAKEUP_THRESH		(B44_TX_RING_SIZE / 4)

/* b44 internal pattern match filter info */
#define B44_PATTERN_BASE	0x400
#define B44_PATTERN_SIZE	0x80
#define B44_PMASK_BASE		0x600
#define B44_PMASK_SIZE		0x10
#define B44_MAX_PATTERNS	16
#define B44_ETHIPV6UDP_HLEN	62
#define B44_ETHIPV4UDP_HLEN	42

static char version[] __devinitdata =
	DRV_MODULE_NAME ".c:v" DRV_MODULE_VERSION "\n";

MODULE_AUTHOR("Felix Fietkau, Florian Schirmer, Pekka Pietikainen, David S. Miller");
MODULE_DESCRIPTION("Broadcom 44xx/47xx 10/100 PCI ethernet driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_MODULE_VERSION);

static int b44_debug = -1;	/* -1 == use B44_DEF_MSG_ENABLE as value */
module_param(b44_debug, int, 0);
MODULE_PARM_DESC(b44_debug, "B44 bitmapped debugging message enable value");


#ifdef CONFIG_B44_PCI
static const struct pci_device_id b44_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM4401) },
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM4401B0) },
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM4401B1) },
	{ 0 } /* terminate list with empty entry */
};
MODULE_DEVICE_TABLE(pci, b44_pci_tbl);

static struct pci_driver b44_pci_driver = {
	.name		= DRV_MODULE_NAME,
	.id_table	= b44_pci_tbl,
};
#endif /* CONFIG_B44_PCI */

static const struct ssb_device_id b44_ssb_tbl[] = {
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_ETHERNET, SSB_ANY_REV),
	SSB_DEVTABLE_END
};
MODULE_DEVICE_TABLE(ssb, b44_ssb_tbl);

static void b44_halt(struct b44 *);
static void b44_init_rings(struct b44 *);

#define B44_FULL_RESET		1
#define B44_FULL_RESET_SKIP_PHY	2
#define B44_PARTIAL_RESET	3

static void b44_init_hw(struct b44 *, int);

static int dma_desc_align_mask;
static int dma_desc_sync_size;
static int instance;

static const char b44_gstrings[][ETH_GSTRING_LEN] = {
#define _B44(x...)	# x,
B44_STAT_REG_DECLARE
#undef _B44
};

static inline void b44_sync_dma_desc_for_device(struct ssb_device *sdev,
						dma_addr_t dma_base,
						unsigned long offset,
						enum dma_data_direction dir)
{
	dma_sync_single_range_for_device(sdev->dev, dma_base,
					 offset & dma_desc_align_mask,
					 dma_desc_sync_size, dir);
}

static inline void b44_sync_dma_desc_for_cpu(struct ssb_device *sdev,
					     dma_addr_t dma_base,
					     unsigned long offset,
					     enum dma_data_direction dir)
{
	dma_sync_single_range_for_cpu(sdev->dev, dma_base,
				      offset & dma_desc_align_mask,
				      dma_desc_sync_size, dir);
}

static inline unsigned long br32(const struct b44 *bp, unsigned long reg)
{
	return ssb_read32(bp->sdev, reg);
}

static inline void bw32(const struct b44 *bp,
			unsigned long reg, unsigned long val)
{
	ssb_write32(bp->sdev, reg, val);
}

static int b44_wait_bit(struct b44 *bp, unsigned long reg,
			u32 bit, unsigned long timeout, const int clear)
{
	unsigned long i;

	for (i = 0; i < timeout; i++) {
		u32 val = br32(bp, reg);

		if (clear && !(val & bit))
			break;
		if (!clear && (val & bit))
			break;
		udelay(10);
	}
	if (i == timeout) {
		printk(KERN_ERR PFX "%s: BUG!  Timeout waiting for bit %08x of register "
		       "%lx to %s.\n",
		       bp->dev->name,
		       bit, reg,
		       (clear ? "clear" : "set"));
		return -ENODEV;
	}
	return 0;
}

static inline void __b44_cam_read(struct b44 *bp, unsigned char *data, int index)
{
	u32 val;

	bw32(bp, B44_CAM_CTRL, (CAM_CTRL_READ |
			    (index << CAM_CTRL_INDEX_SHIFT)));

	b44_wait_bit(bp, B44_CAM_CTRL, CAM_CTRL_BUSY, 100, 1);

	val = br32(bp, B44_CAM_DATA_LO);

	data[2] = (val >> 24) & 0xFF;
	data[3] = (val >> 16) & 0xFF;
	data[4] = (val >> 8) & 0xFF;
	data[5] = (val >> 0) & 0xFF;

	val = br32(bp, B44_CAM_DATA_HI);

	data[0] = (val >> 8) & 0xFF;
	data[1] = (val >> 0) & 0xFF;
}

static inline void __b44_cam_write(struct b44 *bp, unsigned char *data, int index)
{
	u32 val;

	val  = ((u32) data[2]) << 24;
	val |= ((u32) data[3]) << 16;
	val |= ((u32) data[4]) <<  8;
	val |= ((u32) data[5]) <<  0;
	bw32(bp, B44_CAM_DATA_LO, val);
	val = (CAM_DATA_HI_VALID |
	       (((u32) data[0]) << 8) |
	       (((u32) data[1]) << 0));
	bw32(bp, B44_CAM_DATA_HI, val);
	bw32(bp, B44_CAM_CTRL, (CAM_CTRL_WRITE |
			    (index << CAM_CTRL_INDEX_SHIFT)));
	b44_wait_bit(bp, B44_CAM_CTRL, CAM_CTRL_BUSY, 100, 1);
}

static inline void __b44_disable_ints(struct b44 *bp)
{
	bw32(bp, B44_IMASK, 0);
}

static void b44_disable_ints(struct b44 *bp)
{
	__b44_disable_ints(bp);

	/* Flush posted writes. */
	br32(bp, B44_IMASK);
}

static void b44_enable_ints(struct b44 *bp)
{
	bw32(bp, B44_IMASK, bp->imask);
}

static int __b44_readphy(struct b44 *bp, int phy_addr, int reg, u32 *val)
{
	int err;

	bw32(bp, B44_EMAC_ISTAT, EMAC_INT_MII);
	bw32(bp, B44_MDIO_DATA, (MDIO_DATA_SB_START |
			     (MDIO_OP_READ << MDIO_DATA_OP_SHIFT) |
			     (phy_addr << MDIO_DATA_PMD_SHIFT) |
			     (reg << MDIO_DATA_RA_SHIFT) |
			     (MDIO_TA_VALID << MDIO_DATA_TA_SHIFT)));
	err = b44_wait_bit(bp, B44_EMAC_ISTAT, EMAC_INT_MII, 100, 0);
	*val = br32(bp, B44_MDIO_DATA) & MDIO_DATA_DATA;

	return err;
}

static int __b44_writephy(struct b44 *bp, int phy_addr, int reg, u32 val)
{
	bw32(bp, B44_EMAC_ISTAT, EMAC_INT_MII);
	bw32(bp, B44_MDIO_DATA, (MDIO_DATA_SB_START |
			     (MDIO_OP_WRITE << MDIO_DATA_OP_SHIFT) |
			     (phy_addr << MDIO_DATA_PMD_SHIFT) |
			     (reg << MDIO_DATA_RA_SHIFT) |
			     (MDIO_TA_VALID << MDIO_DATA_TA_SHIFT) |
			     (val & MDIO_DATA_DATA)));
	return b44_wait_bit(bp, B44_EMAC_ISTAT, EMAC_INT_MII, 100, 0);
}

static inline int b44_readphy(struct b44 *bp, int reg, u32 *val)
{
	if (bp->phy_addr == B44_PHY_ADDR_NO_PHY)
		return 0;

	return __b44_readphy(bp, bp->phy_addr, reg, val);
}

static inline int b44_writephy(struct b44 *bp, int reg, u32 val)
{
	if (bp->phy_addr == B44_PHY_ADDR_NO_PHY)
		return 0;

	return __b44_writephy(bp, bp->phy_addr, reg, val);
}

/* miilib interface */
static int b44_mii_read(struct net_device *dev, int phy_id, int location)
{
	u32 val;
	struct b44 *bp = netdev_priv(dev);
	int rc = __b44_readphy(bp, phy_id, location, &val);
	if (rc)
		return 0xffffffff;
	return val;
}

static void b44_mii_write(struct net_device *dev, int phy_id, int location,
			 int val)
{
	struct b44 *bp = netdev_priv(dev);
	__b44_writephy(bp, phy_id, location, val);
}

static int b44_phy_reset(struct b44 *bp)
{
	u32 val;
	int err;

	if (bp->phy_addr == B44_PHY_ADDR_NO_PHY)
		return 0;
	err = b44_writephy(bp, MII_BMCR, BMCR_RESET);
	if (err)
		return err;
	udelay(100);
	err = b44_readphy(bp, MII_BMCR, &val);
	if (!err) {
		if (val & BMCR_RESET) {
			printk(KERN_ERR PFX "%s: PHY Reset would not complete.\n",
			       bp->dev->name);
			err = -ENODEV;
		}
	}

	return 0;
}

static void __b44_set_flow_ctrl(struct b44 *bp, u32 pause_flags)
{
	u32 val;

	bp->flags &= ~(B44_FLAG_TX_PAUSE | B44_FLAG_RX_PAUSE);
	bp->flags |= pause_flags;

	val = br32(bp, B44_RXCONFIG);
	if (pause_flags & B44_FLAG_RX_PAUSE)
		val |= RXCONFIG_FLOW;
	else
		val &= ~RXCONFIG_FLOW;
	bw32(bp, B44_RXCONFIG, val);

	val = br32(bp, B44_MAC_FLOW);
	if (pause_flags & B44_FLAG_TX_PAUSE)
		val |= (MAC_FLOW_PAUSE_ENAB |
			(0xc0 & MAC_FLOW_RX_HI_WATER));
	else
		val &= ~MAC_FLOW_PAUSE_ENAB;
	bw32(bp, B44_MAC_FLOW, val);
}

static void b44_set_flow_ctrl(struct b44 *bp, u32 local, u32 remote)
{
	u32 pause_enab = 0;

	/* The driver supports only rx pause by default because
	   the b44 mac tx pause mechanism generates excessive
	   pause frames.
	   Use ethtool to turn on b44 tx pause if necessary.
	 */
	if ((local & ADVERTISE_PAUSE_CAP) &&
	    (local & ADVERTISE_PAUSE_ASYM)){
		if ((remote & LPA_PAUSE_ASYM) &&
		    !(remote & LPA_PAUSE_CAP))
			pause_enab |= B44_FLAG_RX_PAUSE;
	}

	__b44_set_flow_ctrl(bp, pause_enab);
}

#ifdef SSB_DRIVER_MIPS
extern char *nvram_get(char *name);
static void b44_wap54g10_workaround(struct b44 *bp)
{
	const char *str;
	u32 val;
	int err;

	/*
	 * workaround for bad hardware design in Linksys WAP54G v1.0
	 * see https://dev.openwrt.org/ticket/146
	 * check and reset bit "isolate"
	 */
	str = nvram_get("boardnum");
	if (!str)
		return;
	if (simple_strtoul(str, NULL, 0) == 2) {
		err = __b44_readphy(bp, 0, MII_BMCR, &val);
		if (err)
			goto error;
		if (!(val & BMCR_ISOLATE))
			return;
		val &= ~BMCR_ISOLATE;
		err = __b44_writephy(bp, 0, MII_BMCR, val);
		if (err)
			goto error;
	}
	return;
error:
	printk(KERN_WARNING PFX "PHY: cannot reset MII transceiver isolate bit.\n");
}
#else
static inline void b44_wap54g10_workaround(struct b44 *bp)
{
}
#endif

static int b44_setup_phy(struct b44 *bp)
{
	u32 val;
	int err;

	b44_wap54g10_workaround(bp);

	if (bp->phy_addr == B44_PHY_ADDR_NO_PHY)
		return 0;
	if ((err = b44_readphy(bp, B44_MII_ALEDCTRL, &val)) != 0)
		goto out;
	if ((err = b44_writephy(bp, B44_MII_ALEDCTRL,
				val & MII_ALEDCTRL_ALLMSK)) != 0)
		goto out;
	if ((err = b44_readphy(bp, B44_MII_TLEDCTRL, &val)) != 0)
		goto out;
	if ((err = b44_writephy(bp, B44_MII_TLEDCTRL,
				val | MII_TLEDCTRL_ENABLE)) != 0)
		goto out;

	if (!(bp->flags & B44_FLAG_FORCE_LINK)) {
		u32 adv = ADVERTISE_CSMA;

		if (bp->flags & B44_FLAG_ADV_10HALF)
			adv |= ADVERTISE_10HALF;
		if (bp->flags & B44_FLAG_ADV_10FULL)
			adv |= ADVERTISE_10FULL;
		if (bp->flags & B44_FLAG_ADV_100HALF)
			adv |= ADVERTISE_100HALF;
		if (bp->flags & B44_FLAG_ADV_100FULL)
			adv |= ADVERTISE_100FULL;

		if (bp->flags & B44_FLAG_PAUSE_AUTO)
			adv |= ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;

		if ((err = b44_writephy(bp, MII_ADVERTISE, adv)) != 0)
			goto out;
		if ((err = b44_writephy(bp, MII_BMCR, (BMCR_ANENABLE |
						       BMCR_ANRESTART))) != 0)
			goto out;
	} else {
		u32 bmcr;

		if ((err = b44_readphy(bp, MII_BMCR, &bmcr)) != 0)
			goto out;
		bmcr &= ~(BMCR_FULLDPLX | BMCR_ANENABLE | BMCR_SPEED100);
		if (bp->flags & B44_FLAG_100_BASE_T)
			bmcr |= BMCR_SPEED100;
		if (bp->flags & B44_FLAG_FULL_DUPLEX)
			bmcr |= BMCR_FULLDPLX;
		if ((err = b44_writephy(bp, MII_BMCR, bmcr)) != 0)
			goto out;

		/* Since we will not be negotiating there is no safe way
		 * to determine if the link partner supports flow control
		 * or not.  So just disable it completely in this case.
		 */
		b44_set_flow_ctrl(bp, 0, 0);
	}

out:
	return err;
}

static void b44_stats_update(struct b44 *bp)
{
	unsigned long reg;
	u32 *val;

	val = &bp->hw_stats.tx_good_octets;
	for (reg = B44_TX_GOOD_O; reg <= B44_TX_PAUSE; reg += 4UL) {
		*val++ += br32(bp, reg);
	}

	/* Pad */
	reg += 8*4UL;

	for (reg = B44_RX_GOOD_O; reg <= B44_RX_NPAUSE; reg += 4UL) {
		*val++ += br32(bp, reg);
	}
}

static void b44_link_report(struct b44 *bp)
{
	if (!netif_carrier_ok(bp->dev)) {
		printk(KERN_INFO PFX "%s: Link is down.\n", bp->dev->name);
	} else {
		printk(KERN_INFO PFX "%s: Link is up at %d Mbps, %s duplex.\n",
		       bp->dev->name,
		       (bp->flags & B44_FLAG_100_BASE_T) ? 100 : 10,
		       (bp->flags & B44_FLAG_FULL_DUPLEX) ? "full" : "half");

		printk(KERN_INFO PFX "%s: Flow control is %s for TX and "
		       "%s for RX.\n",
		       bp->dev->name,
		       (bp->flags & B44_FLAG_TX_PAUSE) ? "on" : "off",
		       (bp->flags & B44_FLAG_RX_PAUSE) ? "on" : "off");
	}
}

static void b44_check_phy(struct b44 *bp)
{
	u32 bmsr, aux;

	if (bp->phy_addr == B44_PHY_ADDR_NO_PHY) {
		bp->flags |= B44_FLAG_100_BASE_T;
		bp->flags |= B44_FLAG_FULL_DUPLEX;
		if (!netif_carrier_ok(bp->dev)) {
			u32 val = br32(bp, B44_TX_CTRL);
			val |= TX_CTRL_DUPLEX;
			bw32(bp, B44_TX_CTRL, val);
			netif_carrier_on(bp->dev);
			b44_link_report(bp);
		}
		return;
	}

	if (!b44_readphy(bp, MII_BMSR, &bmsr) &&
	    !b44_readphy(bp, B44_MII_AUXCTRL, &aux) &&
	    (bmsr != 0xffff)) {
		if (aux & MII_AUXCTRL_SPEED)
			bp->flags |= B44_FLAG_100_BASE_T;
		else
			bp->flags &= ~B44_FLAG_100_BASE_T;
		if (aux & MII_AUXCTRL_DUPLEX)
			bp->flags |= B44_FLAG_FULL_DUPLEX;
		else
			bp->flags &= ~B44_FLAG_FULL_DUPLEX;

		if (!netif_carrier_ok(bp->dev) &&
		    (bmsr & BMSR_LSTATUS)) {
			u32 val = br32(bp, B44_TX_CTRL);
			u32 local_adv, remote_adv;

			if (bp->flags & B44_FLAG_FULL_DUPLEX)
				val |= TX_CTRL_DUPLEX;
			else
				val &= ~TX_CTRL_DUPLEX;
			bw32(bp, B44_TX_CTRL, val);

			if (!(bp->flags & B44_FLAG_FORCE_LINK) &&
			    !b44_readphy(bp, MII_ADVERTISE, &local_adv) &&
			    !b44_readphy(bp, MII_LPA, &remote_adv))
				b44_set_flow_ctrl(bp, local_adv, remote_adv);

			/* Link now up */
			netif_carrier_on(bp->dev);
			b44_link_report(bp);
		} else if (netif_carrier_ok(bp->dev) && !(bmsr & BMSR_LSTATUS)) {
			/* Link now down */
			netif_carrier_off(bp->dev);
			b44_link_report(bp);
		}

		if (bmsr & BMSR_RFAULT)
			printk(KERN_WARNING PFX "%s: Remote fault detected in PHY\n",
			       bp->dev->name);
		if (bmsr & BMSR_JCD)
			printk(KERN_WARNING PFX "%s: Jabber detected in PHY\n",
			       bp->dev->name);
	}
}

static void b44_timer(unsigned long __opaque)
{
	struct b44 *bp = (struct b44 *) __opaque;

	spin_lock_irq(&bp->lock);

	b44_check_phy(bp);

	b44_stats_update(bp);

	spin_unlock_irq(&bp->lock);

	mod_timer(&bp->timer, round_jiffies(jiffies + HZ));
}

static void b44_tx(struct b44 *bp)
{
	u32 cur, cons;

	cur  = br32(bp, B44_DMATX_STAT) & DMATX_STAT_CDMASK;
	cur /= sizeof(struct dma_desc);

	/* XXX needs updating when NETIF_F_SG is supported */
	for (cons = bp->tx_cons; cons != cur; cons = NEXT_TX(cons)) {
		struct ring_info *rp = &bp->tx_buffers[cons];
		struct sk_buff *skb = rp->skb;

		BUG_ON(skb == NULL);

		dma_unmap_single(bp->sdev->dev,
				 rp->mapping,
				 skb->len,
				 DMA_TO_DEVICE);
		rp->skb = NULL;
		dev_kfree_skb_irq(skb);
	}

	bp->tx_cons = cons;
	if (netif_queue_stopped(bp->dev) &&
	    TX_BUFFS_AVAIL(bp) > B44_TX_WAKEUP_THRESH)
		netif_wake_queue(bp->dev);

	bw32(bp, B44_GPTIMER, 0);
}

/* Works like this.  This chip writes a 'struct rx_header" 30 bytes
 * before the DMA address you give it.  So we allocate 30 more bytes
 * for the RX buffer, DMA map all of it, skb_reserve the 30 bytes, then
 * point the chip at 30 bytes past where the rx_header will go.
 */
static int b44_alloc_rx_skb(struct b44 *bp, int src_idx, u32 dest_idx_unmasked)
{
	struct dma_desc *dp;
	struct ring_info *src_map, *map;
	struct rx_header *rh;
	struct sk_buff *skb;
	dma_addr_t mapping;
	int dest_idx;
	u32 ctrl;

	src_map = NULL;
	if (src_idx >= 0)
		src_map = &bp->rx_buffers[src_idx];
	dest_idx = dest_idx_unmasked & (B44_RX_RING_SIZE - 1);
	map = &bp->rx_buffers[dest_idx];
	skb = netdev_alloc_skb(bp->dev, RX_PKT_BUF_SZ);
	if (skb == NULL)
		return -ENOMEM;

	mapping = dma_map_single(bp->sdev->dev, skb->data,
				 RX_PKT_BUF_SZ,
				 DMA_FROM_DEVICE);

	/* Hardware bug work-around, the chip is unable to do PCI DMA
	   to/from anything above 1GB :-( */
	if (dma_mapping_error(mapping) ||
		mapping + RX_PKT_BUF_SZ > DMA_30BIT_MASK) {
		/* Sigh... */
		if (!dma_mapping_error(mapping))
			dma_unmap_single(bp->sdev->dev, mapping,
					RX_PKT_BUF_SZ, DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
		skb = __netdev_alloc_skb(bp->dev, RX_PKT_BUF_SZ, GFP_ATOMIC|GFP_DMA);
		if (skb == NULL)
			return -ENOMEM;
		mapping = dma_map_single(bp->sdev->dev, skb->data,
					 RX_PKT_BUF_SZ,
					 DMA_FROM_DEVICE);
		if (dma_mapping_error(mapping) ||
			mapping + RX_PKT_BUF_SZ > DMA_30BIT_MASK) {
			if (!dma_mapping_error(mapping))
				dma_unmap_single(bp->sdev->dev, mapping, RX_PKT_BUF_SZ,DMA_FROM_DEVICE);
			dev_kfree_skb_any(skb);
			return -ENOMEM;
		}
	}

	rh = (struct rx_header *) skb->data;
	skb_reserve(skb, RX_PKT_OFFSET);

	rh->len = 0;
	rh->flags = 0;

	map->skb = skb;
	map->mapping = mapping;

	if (src_map != NULL)
		src_map->skb = NULL;

	ctrl  = (DESC_CTRL_LEN & (RX_PKT_BUF_SZ - RX_PKT_OFFSET));
	if (dest_idx == (B44_RX_RING_SIZE - 1))
		ctrl |= DESC_CTRL_EOT;

	dp = &bp->rx_ring[dest_idx];
	dp->ctrl = cpu_to_le32(ctrl);
	dp->addr = cpu_to_le32((u32) mapping + RX_PKT_OFFSET + bp->dma_offset);

	if (bp->flags & B44_FLAG_RX_RING_HACK)
		b44_sync_dma_desc_for_device(bp->sdev, bp->rx_ring_dma,
			                    dest_idx * sizeof(dp),
			                    DMA_BIDIRECTIONAL);

	return RX_PKT_BUF_SZ;
}

static void b44_recycle_rx(struct b44 *bp, int src_idx, u32 dest_idx_unmasked)
{
	struct dma_desc *src_desc, *dest_desc;
	struct ring_info *src_map, *dest_map;
	struct rx_header *rh;
	int dest_idx;
	__le32 ctrl;

	dest_idx = dest_idx_unmasked & (B44_RX_RING_SIZE - 1);
	dest_desc = &bp->rx_ring[dest_idx];
	dest_map = &bp->rx_buffers[dest_idx];
	src_desc = &bp->rx_ring[src_idx];
	src_map = &bp->rx_buffers[src_idx];

	dest_map->skb = src_map->skb;
	rh = (struct rx_header *) src_map->skb->data;
	rh->len = 0;
	rh->flags = 0;
	dest_map->mapping = src_map->mapping;

	if (bp->flags & B44_FLAG_RX_RING_HACK)
		b44_sync_dma_desc_for_cpu(bp->sdev, bp->rx_ring_dma,
			                 src_idx * sizeof(src_desc),
			                 DMA_BIDIRECTIONAL);

	ctrl = src_desc->ctrl;
	if (dest_idx == (B44_RX_RING_SIZE - 1))
		ctrl |= cpu_to_le32(DESC_CTRL_EOT);
	else
		ctrl &= cpu_to_le32(~DESC_CTRL_EOT);

	dest_desc->ctrl = ctrl;
	dest_desc->addr = src_desc->addr;

	src_map->skb = NULL;

	if (bp->flags & B44_FLAG_RX_RING_HACK)
		b44_sync_dma_desc_for_device(bp->sdev, bp->rx_ring_dma,
					     dest_idx * sizeof(dest_desc),
					     DMA_BIDIRECTIONAL);

	dma_sync_single_for_device(bp->sdev->dev, le32_to_cpu(src_desc->addr),
				   RX_PKT_BUF_SZ,
				   DMA_FROM_DEVICE);
}

static int b44_rx(struct b44 *bp, int budget)
{
	int received;
	u32 cons, prod;

	received = 0;
	prod  = br32(bp, B44_DMARX_STAT) & DMARX_STAT_CDMASK;
	prod /= sizeof(struct dma_desc);
	cons = bp->rx_cons;

	while (cons != prod && budget > 0) {
		struct ring_info *rp = &bp->rx_buffers[cons];
		struct sk_buff *skb = rp->skb;
		dma_addr_t map = rp->mapping;
		struct rx_header *rh;
		u16 len;

		dma_sync_single_for_cpu(bp->sdev->dev, map,
					    RX_PKT_BUF_SZ,
					    DMA_FROM_DEVICE);
		rh = (struct rx_header *) skb->data;
		len = le16_to_cpu(rh->len);
		if ((len > (RX_PKT_BUF_SZ - RX_PKT_OFFSET)) ||
		    (rh->flags & cpu_to_le16(RX_FLAG_ERRORS))) {
		drop_it:
			b44_recycle_rx(bp, cons, bp->rx_prod);
		drop_it_no_recycle:
			bp->stats.rx_dropped++;
			goto next_pkt;
		}

		if (len == 0) {
			int i = 0;

			do {
				udelay(2);
				barrier();
				len = le16_to_cpu(rh->len);
			} while (len == 0 && i++ < 5);
			if (len == 0)
				goto drop_it;
		}

		/* Omit CRC. */
		len -= 4;

		if (len > RX_COPY_THRESHOLD) {
			int skb_size;
			skb_size = b44_alloc_rx_skb(bp, cons, bp->rx_prod);
			if (skb_size < 0)
				goto drop_it;
			dma_unmap_single(bp->sdev->dev, map,
					 skb_size, DMA_FROM_DEVICE);
			/* Leave out rx_header */
                	skb_put(skb, len + RX_PKT_OFFSET);
            	        skb_pull(skb, RX_PKT_OFFSET);
		} else {
			struct sk_buff *copy_skb;

			b44_recycle_rx(bp, cons, bp->rx_prod);
			copy_skb = dev_alloc_skb(len + 2);
			if (copy_skb == NULL)
				goto drop_it_no_recycle;

			skb_reserve(copy_skb, 2);
			skb_put(copy_skb, len);
			/* DMA sync done above, copy just the actual packet */
			skb_copy_from_linear_data_offset(skb, RX_PKT_OFFSET,
							 copy_skb->data, len);
			skb = copy_skb;
		}
		skb->ip_summed = CHECKSUM_NONE;
		skb->protocol = eth_type_trans(skb, bp->dev);
		netif_receive_skb(skb);
		bp->dev->last_rx = jiffies;
		received++;
		budget--;
	next_pkt:
		bp->rx_prod = (bp->rx_prod + 1) &
			(B44_RX_RING_SIZE - 1);
		cons = (cons + 1) & (B44_RX_RING_SIZE - 1);
	}

	bp->rx_cons = cons;
	bw32(bp, B44_DMARX_PTR, cons * sizeof(struct dma_desc));

	return received;
}

static int b44_poll(struct napi_struct *napi, int budget)
{
	struct b44 *bp = container_of(napi, struct b44, napi);
	struct net_device *netdev = bp->dev;
	int work_done;

	spin_lock_irq(&bp->lock);

	if (bp->istat & (ISTAT_TX | ISTAT_TO)) {
		/* spin_lock(&bp->tx_lock); */
		b44_tx(bp);
		/* spin_unlock(&bp->tx_lock); */
	}
	spin_unlock_irq(&bp->lock);

	work_done = 0;
	if (bp->istat & ISTAT_RX)
		work_done += b44_rx(bp, budget);

	if (bp->istat & ISTAT_ERRORS) {
		unsigned long flags;

		spin_lock_irqsave(&bp->lock, flags);
		b44_halt(bp);
		b44_init_rings(bp);
		b44_init_hw(bp, B44_FULL_RESET_SKIP_PHY);
		netif_wake_queue(bp->dev);
		spin_unlock_irqrestore(&bp->lock, flags);
		work_done = 0;
	}

	if (work_done < budget) {
		netif_rx_complete(netdev, napi);
		b44_enable_ints(bp);
	}

	return work_done;
}

static irqreturn_t b44_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct b44 *bp = netdev_priv(dev);
	u32 istat, imask;
	int handled = 0;

	spin_lock(&bp->lock);

	istat = br32(bp, B44_ISTAT);
	imask = br32(bp, B44_IMASK);

	/* The interrupt mask register controls which interrupt bits
	 * will actually raise an interrupt to the CPU when set by hw/firmware,
	 * but doesn't mask off the bits.
	 */
	istat &= imask;
	if (istat) {
		handled = 1;

		if (unlikely(!netif_running(dev))) {
			printk(KERN_INFO "%s: late interrupt.\n", dev->name);
			goto irq_ack;
		}

		if (netif_rx_schedule_prep(dev, &bp->napi)) {
			/* NOTE: These writes are posted by the readback of
			 *       the ISTAT register below.
			 */
			bp->istat = istat;
			__b44_disable_ints(bp);
			__netif_rx_schedule(dev, &bp->napi);
		} else {
			printk(KERN_ERR PFX "%s: Error, poll already scheduled\n",
			       dev->name);
		}

irq_ack:
		bw32(bp, B44_ISTAT, istat);
		br32(bp, B44_ISTAT);
	}
	spin_unlock(&bp->lock);
	return IRQ_RETVAL(handled);
}

static void b44_tx_timeout(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);

	printk(KERN_ERR PFX "%s: transmit timed out, resetting\n",
	       dev->name);

	spin_lock_irq(&bp->lock);

	b44_halt(bp);
	b44_init_rings(bp);
	b44_init_hw(bp, B44_FULL_RESET);

	spin_unlock_irq(&bp->lock);

	b44_enable_ints(bp);

	netif_wake_queue(dev);
}

static int b44_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);
	int rc = NETDEV_TX_OK;
	dma_addr_t mapping;
	u32 len, entry, ctrl;

	len = skb->len;
	spin_lock_irq(&bp->lock);

	/* This is a hard error, log it. */
	if (unlikely(TX_BUFFS_AVAIL(bp) < 1)) {
		netif_stop_queue(dev);
		printk(KERN_ERR PFX "%s: BUG! Tx Ring full when queue awake!\n",
		       dev->name);
		goto err_out;
	}

	mapping = dma_map_single(bp->sdev->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(mapping) || mapping + len > DMA_30BIT_MASK) {
		struct sk_buff *bounce_skb;

		/* Chip can't handle DMA to/from >1GB, use bounce buffer */
		if (!dma_mapping_error(mapping))
			dma_unmap_single(bp->sdev->dev, mapping, len,
					DMA_TO_DEVICE);

		bounce_skb = __dev_alloc_skb(len, GFP_ATOMIC | GFP_DMA);
		if (!bounce_skb)
			goto err_out;

		mapping = dma_map_single(bp->sdev->dev, bounce_skb->data,
					 len, DMA_TO_DEVICE);
		if (dma_mapping_error(mapping) || mapping + len > DMA_30BIT_MASK) {
			if (!dma_mapping_error(mapping))
				dma_unmap_single(bp->sdev->dev, mapping,
					 len, DMA_TO_DEVICE);
			dev_kfree_skb_any(bounce_skb);
			goto err_out;
		}

		skb_copy_from_linear_data(skb, skb_put(bounce_skb, len), len);
		dev_kfree_skb_any(skb);
		skb = bounce_skb;
	}

	entry = bp->tx_prod;
	bp->tx_buffers[entry].skb = skb;
	bp->tx_buffers[entry].mapping = mapping;

	ctrl  = (len & DESC_CTRL_LEN);
	ctrl |= DESC_CTRL_IOC | DESC_CTRL_SOF | DESC_CTRL_EOF;
	if (entry == (B44_TX_RING_SIZE - 1))
		ctrl |= DESC_CTRL_EOT;

	bp->tx_ring[entry].ctrl = cpu_to_le32(ctrl);
	bp->tx_ring[entry].addr = cpu_to_le32((u32) mapping+bp->dma_offset);

	if (bp->flags & B44_FLAG_TX_RING_HACK)
		b44_sync_dma_desc_for_device(bp->sdev, bp->tx_ring_dma,
			                    entry * sizeof(bp->tx_ring[0]),
			                    DMA_TO_DEVICE);

	entry = NEXT_TX(entry);

	bp->tx_prod = entry;

	wmb();

	bw32(bp, B44_DMATX_PTR, entry * sizeof(struct dma_desc));
	if (bp->flags & B44_FLAG_BUGGY_TXPTR)
		bw32(bp, B44_DMATX_PTR, entry * sizeof(struct dma_desc));
	if (bp->flags & B44_FLAG_REORDER_BUG)
		br32(bp, B44_DMATX_PTR);

	if (TX_BUFFS_AVAIL(bp) < 1)
		netif_stop_queue(dev);

	dev->trans_start = jiffies;

out_unlock:
	spin_unlock_irq(&bp->lock);

	return rc;

err_out:
	rc = NETDEV_TX_BUSY;
	goto out_unlock;
}

static int b44_change_mtu(struct net_device *dev, int new_mtu)
{
	struct b44 *bp = netdev_priv(dev);

	if (new_mtu < B44_MIN_MTU || new_mtu > B44_MAX_MTU)
		return -EINVAL;

	if (!netif_running(dev)) {
		/* We'll just catch it later when the
		 * device is up'd.
		 */
		dev->mtu = new_mtu;
		return 0;
	}

	spin_lock_irq(&bp->lock);
	b44_halt(bp);
	dev->mtu = new_mtu;
	b44_init_rings(bp);
	b44_init_hw(bp, B44_FULL_RESET);
	spin_unlock_irq(&bp->lock);

	b44_enable_ints(bp);

	return 0;
}

/* Free up pending packets in all rx/tx rings.
 *
 * The chip has been shut down and the driver detached from
 * the networking, so no interrupts or new tx packets will
 * end up in the driver.  bp->lock is not held and we are not
 * in an interrupt context and thus may sleep.
 */
static void b44_free_rings(struct b44 *bp)
{
	struct ring_info *rp;
	int i;

	for (i = 0; i < B44_RX_RING_SIZE; i++) {
		rp = &bp->rx_buffers[i];

		if (rp->skb == NULL)
			continue;
		dma_unmap_single(bp->sdev->dev, rp->mapping, RX_PKT_BUF_SZ,
					DMA_FROM_DEVICE);
		dev_kfree_skb_any(rp->skb);
		rp->skb = NULL;
	}

	/* XXX needs changes once NETIF_F_SG is set... */
	for (i = 0; i < B44_TX_RING_SIZE; i++) {
		rp = &bp->tx_buffers[i];

		if (rp->skb == NULL)
			continue;
		dma_unmap_single(bp->sdev->dev, rp->mapping, rp->skb->len,
					DMA_TO_DEVICE);
		dev_kfree_skb_any(rp->skb);
		rp->skb = NULL;
	}
}

/* Initialize tx/rx rings for packet processing.
 *
 * The chip has been shut down and the driver detached from
 * the networking, so no interrupts or new tx packets will
 * end up in the driver.
 */
static void b44_init_rings(struct b44 *bp)
{
	int i;

	b44_free_rings(bp);

	memset(bp->rx_ring, 0, B44_RX_RING_BYTES);
	memset(bp->tx_ring, 0, B44_TX_RING_BYTES);

	if (bp->flags & B44_FLAG_RX_RING_HACK)
		dma_sync_single_for_device(bp->sdev->dev, bp->rx_ring_dma,
			                  DMA_TABLE_BYTES,
			                  DMA_BIDIRECTIONAL);

	if (bp->flags & B44_FLAG_TX_RING_HACK)
		dma_sync_single_for_device(bp->sdev->dev, bp->tx_ring_dma,
			                  DMA_TABLE_BYTES,
			                  DMA_TO_DEVICE);

	for (i = 0; i < bp->rx_pending; i++) {
		if (b44_alloc_rx_skb(bp, -1, i) < 0)
			break;
	}
}

/*
 * Must not be invoked with interrupt sources disabled and
 * the hardware shutdown down.
 */
static void b44_free_consistent(struct b44 *bp)
{
	kfree(bp->rx_buffers);
	bp->rx_buffers = NULL;
	kfree(bp->tx_buffers);
	bp->tx_buffers = NULL;
	if (bp->rx_ring) {
		if (bp->flags & B44_FLAG_RX_RING_HACK) {
			dma_unmap_single(bp->sdev->dev, bp->rx_ring_dma,
					DMA_TABLE_BYTES,
					DMA_BIDIRECTIONAL);
			kfree(bp->rx_ring);
		} else
			dma_free_coherent(bp->sdev->dev, DMA_TABLE_BYTES,
					    bp->rx_ring, bp->rx_ring_dma);
		bp->rx_ring = NULL;
		bp->flags &= ~B44_FLAG_RX_RING_HACK;
	}
	if (bp->tx_ring) {
		if (bp->flags & B44_FLAG_TX_RING_HACK) {
			dma_unmap_single(bp->sdev->dev, bp->tx_ring_dma,
					DMA_TABLE_BYTES,
					DMA_TO_DEVICE);
			kfree(bp->tx_ring);
		} else
			dma_free_coherent(bp->sdev->dev, DMA_TABLE_BYTES,
					    bp->tx_ring, bp->tx_ring_dma);
		bp->tx_ring = NULL;
		bp->flags &= ~B44_FLAG_TX_RING_HACK;
	}
}

/*
 * Must not be invoked with interrupt sources disabled and
 * the hardware shutdown down.  Can sleep.
 */
static int b44_alloc_consistent(struct b44 *bp, gfp_t gfp)
{
	int size;

	size  = B44_RX_RING_SIZE * sizeof(struct ring_info);
	bp->rx_buffers = kzalloc(size, gfp);
	if (!bp->rx_buffers)
		goto out_err;

	size = B44_TX_RING_SIZE * sizeof(struct ring_info);
	bp->tx_buffers = kzalloc(size, gfp);
	if (!bp->tx_buffers)
		goto out_err;

	size = DMA_TABLE_BYTES;
	bp->rx_ring = dma_alloc_coherent(bp->sdev->dev, size, &bp->rx_ring_dma, gfp);
	if (!bp->rx_ring) {
		/* Allocation may have failed due to pci_alloc_consistent
		   insisting on use of GFP_DMA, which is more restrictive
		   than necessary...  */
		struct dma_desc *rx_ring;
		dma_addr_t rx_ring_dma;

		rx_ring = kzalloc(size, gfp);
		if (!rx_ring)
			goto out_err;

		rx_ring_dma = dma_map_single(bp->sdev->dev, rx_ring,
			                    DMA_TABLE_BYTES,
			                    DMA_BIDIRECTIONAL);

		if (dma_mapping_error(rx_ring_dma) ||
			rx_ring_dma + size > DMA_30BIT_MASK) {
			kfree(rx_ring);
			goto out_err;
		}

		bp->rx_ring = rx_ring;
		bp->rx_ring_dma = rx_ring_dma;
		bp->flags |= B44_FLAG_RX_RING_HACK;
	}

	bp->tx_ring = dma_alloc_coherent(bp->sdev->dev, size, &bp->tx_ring_dma, gfp);
	if (!bp->tx_ring) {
		/* Allocation may have failed due to dma_alloc_coherent
		   insisting on use of GFP_DMA, which is more restrictive
		   than necessary...  */
		struct dma_desc *tx_ring;
		dma_addr_t tx_ring_dma;

		tx_ring = kzalloc(size, gfp);
		if (!tx_ring)
			goto out_err;

		tx_ring_dma = dma_map_single(bp->sdev->dev, tx_ring,
			                    DMA_TABLE_BYTES,
			                    DMA_TO_DEVICE);

		if (dma_mapping_error(tx_ring_dma) ||
			tx_ring_dma + size > DMA_30BIT_MASK) {
			kfree(tx_ring);
			goto out_err;
		}

		bp->tx_ring = tx_ring;
		bp->tx_ring_dma = tx_ring_dma;
		bp->flags |= B44_FLAG_TX_RING_HACK;
	}

	return 0;

out_err:
	b44_free_consistent(bp);
	return -ENOMEM;
}

/* bp->lock is held. */
static void b44_clear_stats(struct b44 *bp)
{
	unsigned long reg;

	bw32(bp, B44_MIB_CTRL, MIB_CTRL_CLR_ON_READ);
	for (reg = B44_TX_GOOD_O; reg <= B44_TX_PAUSE; reg += 4UL)
		br32(bp, reg);
	for (reg = B44_RX_GOOD_O; reg <= B44_RX_NPAUSE; reg += 4UL)
		br32(bp, reg);
}

/* bp->lock is held. */
static void b44_chip_reset(struct b44 *bp)
{
	struct ssb_device *sdev = bp->sdev;

	if (ssb_device_is_enabled(bp->sdev)) {
		bw32(bp, B44_RCV_LAZY, 0);
		bw32(bp, B44_ENET_CTRL, ENET_CTRL_DISABLE);
		b44_wait_bit(bp, B44_ENET_CTRL, ENET_CTRL_DISABLE, 200, 1);
		bw32(bp, B44_DMATX_CTRL, 0);
		bp->tx_prod = bp->tx_cons = 0;
		if (br32(bp, B44_DMARX_STAT) & DMARX_STAT_EMASK) {
			b44_wait_bit(bp, B44_DMARX_STAT, DMARX_STAT_SIDLE,
				     100, 0);
		}
		bw32(bp, B44_DMARX_CTRL, 0);
		bp->rx_prod = bp->rx_cons = 0;
	} else
		ssb_pcicore_dev_irqvecs_enable(&sdev->bus->pcicore, sdev);

	ssb_device_enable(bp->sdev, 0);
	b44_clear_stats(bp);

	switch (sdev->bus->bustype) {
	case SSB_BUSTYPE_SSB:
		bw32(bp, B44_MDIO_CTRL, (MDIO_CTRL_PREAMBLE |
		     (((ssb_clockspeed(sdev->bus) + (B44_MDC_RATIO / 2)) / B44_MDC_RATIO)
		     & MDIO_CTRL_MAXF_MASK)));
		break;
	case SSB_BUSTYPE_PCI:
	case SSB_BUSTYPE_PCMCIA:
		bw32(bp, B44_MDIO_CTRL, (MDIO_CTRL_PREAMBLE |
		     (0x0d & MDIO_CTRL_MAXF_MASK)));
		break;
	}

	br32(bp, B44_MDIO_CTRL);

	if (!(br32(bp, B44_DEVCTRL) & DEVCTRL_IPP)) {
		bw32(bp, B44_ENET_CTRL, ENET_CTRL_EPSEL);
		br32(bp, B44_ENET_CTRL);
		bp->flags &= ~B44_FLAG_INTERNAL_PHY;
	} else {
		u32 val = br32(bp, B44_DEVCTRL);

		if (val & DEVCTRL_EPR) {
			bw32(bp, B44_DEVCTRL, (val & ~DEVCTRL_EPR));
			br32(bp, B44_DEVCTRL);
			udelay(100);
		}
		bp->flags |= B44_FLAG_INTERNAL_PHY;
	}
}

/* bp->lock is held. */
static void b44_halt(struct b44 *bp)
{
	b44_disable_ints(bp);
	b44_chip_reset(bp);
}

/* bp->lock is held. */
static void __b44_set_mac_addr(struct b44 *bp)
{
	bw32(bp, B44_CAM_CTRL, 0);
	if (!(bp->dev->flags & IFF_PROMISC)) {
		u32 val;

		__b44_cam_write(bp, bp->dev->dev_addr, 0);
		val = br32(bp, B44_CAM_CTRL);
		bw32(bp, B44_CAM_CTRL, val | CAM_CTRL_ENABLE);
	}
}

static int b44_set_mac_addr(struct net_device *dev, void *p)
{
	struct b44 *bp = netdev_priv(dev);
	struct sockaddr *addr = p;
	u32 val;

	if (netif_running(dev))
		return -EBUSY;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EINVAL;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);

	spin_lock_irq(&bp->lock);

	val = br32(bp, B44_RXCONFIG);
	if (!(val & RXCONFIG_CAM_ABSENT))
		__b44_set_mac_addr(bp);

	spin_unlock_irq(&bp->lock);

	return 0;
}

/* Called at device open time to get the chip ready for
 * packet processing.  Invoked with bp->lock held.
 */
static void __b44_set_rx_mode(struct net_device *);
static void b44_init_hw(struct b44 *bp, int reset_kind)
{
	u32 val;

	b44_chip_reset(bp);
	if (reset_kind == B44_FULL_RESET) {
		b44_phy_reset(bp);
		b44_setup_phy(bp);
	}

	/* Enable CRC32, set proper LED modes and power on PHY */
	bw32(bp, B44_MAC_CTRL, MAC_CTRL_CRC32_ENAB | MAC_CTRL_PHY_LEDCTRL);
	bw32(bp, B44_RCV_LAZY, (1 << RCV_LAZY_FC_SHIFT));

	/* This sets the MAC address too.  */
	__b44_set_rx_mode(bp->dev);

	/* MTU + eth header + possible VLAN tag + struct rx_header */
	bw32(bp, B44_RXMAXLEN, bp->dev->mtu + ETH_HLEN + 8 + RX_HEADER_LEN);
	bw32(bp, B44_TXMAXLEN, bp->dev->mtu + ETH_HLEN + 8 + RX_HEADER_LEN);

	bw32(bp, B44_TX_WMARK, 56); /* XXX magic */
	if (reset_kind == B44_PARTIAL_RESET) {
		bw32(bp, B44_DMARX_CTRL, (DMARX_CTRL_ENABLE |
				      (RX_PKT_OFFSET << DMARX_CTRL_ROSHIFT)));
	} else {
		bw32(bp, B44_DMATX_CTRL, DMATX_CTRL_ENABLE);
		bw32(bp, B44_DMATX_ADDR, bp->tx_ring_dma + bp->dma_offset);
		bw32(bp, B44_DMARX_CTRL, (DMARX_CTRL_ENABLE |
				      (RX_PKT_OFFSET << DMARX_CTRL_ROSHIFT)));
		bw32(bp, B44_DMARX_ADDR, bp->rx_ring_dma + bp->dma_offset);

		bw32(bp, B44_DMARX_PTR, bp->rx_pending);
		bp->rx_prod = bp->rx_pending;

		bw32(bp, B44_MIB_CTRL, MIB_CTRL_CLR_ON_READ);
	}

	val = br32(bp, B44_ENET_CTRL);
	bw32(bp, B44_ENET_CTRL, (val | ENET_CTRL_ENABLE));
}

static int b44_open(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);
	int err;

	err = b44_alloc_consistent(bp, GFP_KERNEL);
	if (err)
		goto out;

	napi_enable(&bp->napi);

	b44_init_rings(bp);
	b44_init_hw(bp, B44_FULL_RESET);

	b44_check_phy(bp);

	err = request_irq(dev->irq, b44_interrupt, IRQF_SHARED, dev->name, dev);
	if (unlikely(err < 0)) {
		napi_disable(&bp->napi);
		b44_chip_reset(bp);
		b44_free_rings(bp);
		b44_free_consistent(bp);
		goto out;
	}

	init_timer(&bp->timer);
	bp->timer.expires = jiffies + HZ;
	bp->timer.data = (unsigned long) bp;
	bp->timer.function = b44_timer;
	add_timer(&bp->timer);

	b44_enable_ints(bp);
	netif_start_queue(dev);
out:
	return err;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/*
 * Polling receive - used by netconsole and other diagnostic tools
 * to allow network i/o with interrupts disabled.
 */
static void b44_poll_controller(struct net_device *dev)
{
	disable_irq(dev->irq);
	b44_interrupt(dev->irq, dev);
	enable_irq(dev->irq);
}
#endif

static void bwfilter_table(struct b44 *bp, u8 *pp, u32 bytes, u32 table_offset)
{
	u32 i;
	u32 *pattern = (u32 *) pp;

	for (i = 0; i < bytes; i += sizeof(u32)) {
		bw32(bp, B44_FILT_ADDR, table_offset + i);
		bw32(bp, B44_FILT_DATA, pattern[i / sizeof(u32)]);
	}
}

static int b44_magic_pattern(u8 *macaddr, u8 *ppattern, u8 *pmask, int offset)
{
	int magicsync = 6;
	int k, j, len = offset;
	int ethaddr_bytes = ETH_ALEN;

	memset(ppattern + offset, 0xff, magicsync);
	for (j = 0; j < magicsync; j++)
		set_bit(len++, (unsigned long *) pmask);

	for (j = 0; j < B44_MAX_PATTERNS; j++) {
		if ((B44_PATTERN_SIZE - len) >= ETH_ALEN)
			ethaddr_bytes = ETH_ALEN;
		else
			ethaddr_bytes = B44_PATTERN_SIZE - len;
		if (ethaddr_bytes <=0)
			break;
		for (k = 0; k< ethaddr_bytes; k++) {
			ppattern[offset + magicsync +
				(j * ETH_ALEN) + k] = macaddr[k];
			len++;
			set_bit(len, (unsigned long *) pmask);
		}
	}
	return len - 1;
}

/* Setup magic packet patterns in the b44 WOL
 * pattern matching filter.
 */
static void b44_setup_pseudo_magicp(struct b44 *bp)
{

	u32 val;
	int plen0, plen1, plen2;
	u8 *pwol_pattern;
	u8 pwol_mask[B44_PMASK_SIZE];

	pwol_pattern = kzalloc(B44_PATTERN_SIZE, GFP_KERNEL);
	if (!pwol_pattern) {
		printk(KERN_ERR PFX "Memory not available for WOL\n");
		return;
	}

	/* Ipv4 magic packet pattern - pattern 0.*/
	memset(pwol_mask, 0, B44_PMASK_SIZE);
	plen0 = b44_magic_pattern(bp->dev->dev_addr, pwol_pattern, pwol_mask,
				  B44_ETHIPV4UDP_HLEN);

   	bwfilter_table(bp, pwol_pattern, B44_PATTERN_SIZE, B44_PATTERN_BASE);
   	bwfilter_table(bp, pwol_mask, B44_PMASK_SIZE, B44_PMASK_BASE);

	/* Raw ethernet II magic packet pattern - pattern 1 */
	memset(pwol_pattern, 0, B44_PATTERN_SIZE);
	memset(pwol_mask, 0, B44_PMASK_SIZE);
	plen1 = b44_magic_pattern(bp->dev->dev_addr, pwol_pattern, pwol_mask,
				  ETH_HLEN);

   	bwfilter_table(bp, pwol_pattern, B44_PATTERN_SIZE,
		       B44_PATTERN_BASE + B44_PATTERN_SIZE);
  	bwfilter_table(bp, pwol_mask, B44_PMASK_SIZE,
		       B44_PMASK_BASE + B44_PMASK_SIZE);

	/* Ipv6 magic packet pattern - pattern 2 */
	memset(pwol_pattern, 0, B44_PATTERN_SIZE);
	memset(pwol_mask, 0, B44_PMASK_SIZE);
	plen2 = b44_magic_pattern(bp->dev->dev_addr, pwol_pattern, pwol_mask,
				  B44_ETHIPV6UDP_HLEN);

   	bwfilter_table(bp, pwol_pattern, B44_PATTERN_SIZE,
		       B44_PATTERN_BASE + B44_PATTERN_SIZE + B44_PATTERN_SIZE);
  	bwfilter_table(bp, pwol_mask, B44_PMASK_SIZE,
		       B44_PMASK_BASE + B44_PMASK_SIZE + B44_PMASK_SIZE);

	kfree(pwol_pattern);

	/* set these pattern's lengths: one less than each real length */
	val = plen0 | (plen1 << 8) | (plen2 << 16) | WKUP_LEN_ENABLE_THREE;
	bw32(bp, B44_WKUP_LEN, val);

	/* enable wakeup pattern matching */
	val = br32(bp, B44_DEVCTRL);
	bw32(bp, B44_DEVCTRL, val | DEVCTRL_PFE);

}

#ifdef CONFIG_B44_PCI
static void b44_setup_wol_pci(struct b44 *bp)
{
	u16 val;

	if (bp->sdev->bus->bustype != SSB_BUSTYPE_SSB) {
		bw32(bp, SSB_TMSLOW, br32(bp, SSB_TMSLOW) | SSB_TMSLOW_PE);
		pci_read_config_word(bp->sdev->bus->host_pci, SSB_PMCSR, &val);
		pci_write_config_word(bp->sdev->bus->host_pci, SSB_PMCSR, val | SSB_PE);
	}
}
#else
static inline void b44_setup_wol_pci(struct b44 *bp) { }
#endif /* CONFIG_B44_PCI */

static void b44_setup_wol(struct b44 *bp)
{
	u32 val;

	bw32(bp, B44_RXCONFIG, RXCONFIG_ALLMULTI);

	if (bp->flags & B44_FLAG_B0_ANDLATER) {

		bw32(bp, B44_WKUP_LEN, WKUP_LEN_DISABLE);

		val = bp->dev->dev_addr[2] << 24 |
			bp->dev->dev_addr[3] << 16 |
			bp->dev->dev_addr[4] << 8 |
			bp->dev->dev_addr[5];
		bw32(bp, B44_ADDR_LO, val);

		val = bp->dev->dev_addr[0] << 8 |
			bp->dev->dev_addr[1];
		bw32(bp, B44_ADDR_HI, val);

		val = br32(bp, B44_DEVCTRL);
		bw32(bp, B44_DEVCTRL, val | DEVCTRL_MPM | DEVCTRL_PFE);

 	} else {
 		b44_setup_pseudo_magicp(bp);
 	}
	b44_setup_wol_pci(bp);
}

static int b44_close(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);

	netif_stop_queue(dev);

	napi_disable(&bp->napi);

	del_timer_sync(&bp->timer);

	spin_lock_irq(&bp->lock);

	b44_halt(bp);
	b44_free_rings(bp);
	netif_carrier_off(dev);

	spin_unlock_irq(&bp->lock);

	free_irq(dev->irq, dev);

	if (bp->flags & B44_FLAG_WOL_ENABLE) {
		b44_init_hw(bp, B44_PARTIAL_RESET);
		b44_setup_wol(bp);
	}

	b44_free_consistent(bp);

	return 0;
}

static struct net_device_stats *b44_get_stats(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);
	struct net_device_stats *nstat = &bp->stats;
	struct b44_hw_stats *hwstat = &bp->hw_stats;

	/* Convert HW stats into netdevice stats. */
	nstat->rx_packets = hwstat->rx_pkts;
	nstat->tx_packets = hwstat->tx_pkts;
	nstat->rx_bytes   = hwstat->rx_octets;
	nstat->tx_bytes   = hwstat->tx_octets;
	nstat->tx_errors  = (hwstat->tx_jabber_pkts +
			     hwstat->tx_oversize_pkts +
			     hwstat->tx_underruns +
			     hwstat->tx_excessive_cols +
			     hwstat->tx_late_cols);
	nstat->multicast  = hwstat->tx_multicast_pkts;
	nstat->collisions = hwstat->tx_total_cols;

	nstat->rx_length_errors = (hwstat->rx_oversize_pkts +
				   hwstat->rx_undersize);
	nstat->rx_over_errors   = hwstat->rx_missed_pkts;
	nstat->rx_frame_errors  = hwstat->rx_align_errs;
	nstat->rx_crc_errors    = hwstat->rx_crc_errs;
	nstat->rx_errors        = (hwstat->rx_jabber_pkts +
				   hwstat->rx_oversize_pkts +
				   hwstat->rx_missed_pkts +
				   hwstat->rx_crc_align_errs +
				   hwstat->rx_undersize +
				   hwstat->rx_crc_errs +
				   hwstat->rx_align_errs +
				   hwstat->rx_symbol_errs);

	nstat->tx_aborted_errors = hwstat->tx_underruns;
#if 0
	/* Carrier lost counter seems to be broken for some devices */
	nstat->tx_carrier_errors = hwstat->tx_carrier_lost;
#endif

	return nstat;
}

static int __b44_load_mcast(struct b44 *bp, struct net_device *dev)
{
	struct dev_mc_list *mclist;
	int i, num_ents;

	num_ents = min_t(int, dev->mc_count, B44_MCAST_TABLE_SIZE);
	mclist = dev->mc_list;
	for (i = 0; mclist && i < num_ents; i++, mclist = mclist->next) {
		__b44_cam_write(bp, mclist->dmi_addr, i + 1);
	}
	return i+1;
}

static void __b44_set_rx_mode(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);
	u32 val;

	val = br32(bp, B44_RXCONFIG);
	val &= ~(RXCONFIG_PROMISC | RXCONFIG_ALLMULTI);
	if ((dev->flags & IFF_PROMISC) || (val & RXCONFIG_CAM_ABSENT)) {
		val |= RXCONFIG_PROMISC;
		bw32(bp, B44_RXCONFIG, val);
	} else {
		unsigned char zero[6] = {0, 0, 0, 0, 0, 0};
		int i = 1;

		__b44_set_mac_addr(bp);

		if ((dev->flags & IFF_ALLMULTI) ||
		    (dev->mc_count > B44_MCAST_TABLE_SIZE))
			val |= RXCONFIG_ALLMULTI;
		else
			i = __b44_load_mcast(bp, dev);

		for (; i < 64; i++)
			__b44_cam_write(bp, zero, i);

		bw32(bp, B44_RXCONFIG, val);
        	val = br32(bp, B44_CAM_CTRL);
	        bw32(bp, B44_CAM_CTRL, val | CAM_CTRL_ENABLE);
	}
}

static void b44_set_rx_mode(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);

	spin_lock_irq(&bp->lock);
	__b44_set_rx_mode(dev);
	spin_unlock_irq(&bp->lock);
}

static u32 b44_get_msglevel(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);
	return bp->msg_enable;
}

static void b44_set_msglevel(struct net_device *dev, u32 value)
{
	struct b44 *bp = netdev_priv(dev);
	bp->msg_enable = value;
}

static void b44_get_drvinfo (struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct b44 *bp = netdev_priv(dev);
	struct ssb_bus *bus = bp->sdev->bus;

	strncpy(info->driver, DRV_MODULE_NAME, sizeof(info->driver));
	strncpy(info->version, DRV_MODULE_VERSION, sizeof(info->driver));
	switch (bus->bustype) {
	case SSB_BUSTYPE_PCI:
		strncpy(info->bus_info, pci_name(bus->host_pci), sizeof(info->bus_info));
		break;
	case SSB_BUSTYPE_PCMCIA:
	case SSB_BUSTYPE_SSB:
		strncpy(info->bus_info, "SSB", sizeof(info->bus_info));
		break;
	}
}

static int b44_nway_reset(struct net_device *dev)
{
	struct b44 *bp = netdev_priv(dev);
	u32 bmcr;
	int r;

	spin_lock_irq(&bp->lock);
	b44_readphy(bp, MII_BMCR, &bmcr);
	b44_readphy(bp, MII_BMCR, &bmcr);
	r = -EINVAL;
	if (bmcr & BMCR_ANENABLE) {
		b44_writephy(bp, MII_BMCR,
			     bmcr | BMCR_ANRESTART);
		r = 0;
	}
	spin_unlock_irq(&bp->lock);

	return r;
}

static int b44_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct b44 *bp = netdev_priv(dev);

	cmd->supported = (SUPPORTED_Autoneg);
	cmd->supported |= (SUPPORTED_100baseT_Half |
			  SUPPORTED_100baseT_Full |
			  SUPPORTED_10baseT_Half |
			  SUPPORTED_10baseT_Full |
			  SUPPORTED_MII);

	cmd->advertising = 0;
	if (bp->flags & B44_FLAG_ADV_10HALF)
		cmd->advertising |= ADVERTISED_10baseT_Half;
	if (bp->flags & B44_FLAG_ADV_10FULL)
		cmd->advertising |= ADVERTISED_10baseT_Full;
	if (bp->flags & B44_FLAG_ADV_100HALF)
		cmd->advertising |= ADVERTISED_100baseT_Half;
	if (bp->flags & B44_FLAG_ADV_100FULL)
		cmd->advertising |= ADVERTISED_100baseT_Full;
	cmd->advertising |= ADVERTISED_Pause | ADVERTISED_Asym_Pause;
	cmd->speed = (bp->flags & B44_FLAG_100_BASE_T) ?
		SPEED_100 : SPEED_10;
	cmd->duplex = (bp->flags & B44_FLAG_FULL_DUPLEX) ?
		DUPLEX_FULL : DUPLEX_HALF;
	cmd->port = 0;
	cmd->phy_address = bp->phy_addr;
	cmd->transceiver = (bp->flags & B44_FLAG_INTERNAL_PHY) ?
		XCVR_INTERNAL : XCVR_EXTERNAL;
	cmd->autoneg = (bp->flags & B44_FLAG_FORCE_LINK) ?
		AUTONEG_DISABLE : AUTONEG_ENABLE;
	if (cmd->autoneg == AUTONEG_ENABLE)
		cmd->advertising |= ADVERTISED_Autoneg;
	if (!netif_running(dev)){
		cmd->speed = 0;
		cmd->duplex = 0xff;
	}
	cmd->maxtxpkt = 0;
	cmd->maxrxpkt = 0;
	return 0;
}

static int b44_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct b44 *bp = netdev_priv(dev);

	/* We do not support gigabit. */
	if (cmd->autoneg == AUTONEG_ENABLE) {
		if (cmd->advertising &
		    (ADVERTISED_1000baseT_Half |
		     ADVERTISED_1000baseT_Full))
			return -EINVAL;
	} else if ((cmd->speed != SPEED_100 &&
		    cmd->speed != SPEED_10) ||
		   (cmd->duplex != DUPLEX_HALF &&
		    cmd->duplex != DUPLEX_FULL)) {
			return -EINVAL;
	}

	spin_lock_irq(&bp->lock);

	if (cmd->autoneg == AUTONEG_ENABLE) {
		bp->flags &= ~(B44_FLAG_FORCE_LINK |
			       B44_FLAG_100_BASE_T |
			       B44_FLAG_FULL_DUPLEX |
			       B44_FLAG_ADV_10HALF |
			       B44_FLAG_ADV_10FULL |
			       B44_FLAG_ADV_100HALF |
			       B44_FLAG_ADV_100FULL);
		if (cmd->advertising == 0) {
			bp->flags |= (B44_FLAG_ADV_10HALF |
				      B44_FLAG_ADV_10FULL |
				      B44_FLAG_ADV_100HALF |
				      B44_FLAG_ADV_100FULL);
		} else {
			if (cmd->advertising & ADVERTISED_10baseT_Half)
				bp->flags |= B44_FLAG_ADV_10HALF;
			if (cmd->advertising & ADVERTISED_10baseT_Full)
				bp->flags |= B44_FLAG_ADV_10FULL;
			if (cmd->advertising & ADVERTISED_100baseT_Half)
				bp->flags |= B44_FLAG_ADV_100HALF;
			if (cmd->advertising & ADVERTISED_100baseT_Full)
				bp->flags |= B44_FLAG_ADV_100FULL;
		}
	} else {
		bp->flags |= B44_FLAG_FORCE_LINK;
		bp->flags &= ~(B44_FLAG_100_BASE_T | B44_FLAG_FULL_DUPLEX);
		if (cmd->speed == SPEED_100)
			bp->flags |= B44_FLAG_100_BASE_T;
		if (cmd->duplex == DUPLEX_FULL)
			bp->flags |= B44_FLAG_FULL_DUPLEX;
	}

	if (netif_running(dev))
		b44_setup_phy(bp);

	spin_unlock_irq(&bp->lock);

	return 0;
}

static void b44_get_ringparam(struct net_device *dev,
			      struct ethtool_ringparam *ering)
{
	struct b44 *bp = netdev_priv(dev);

	ering->rx_max_pending = B44_RX_RING_SIZE - 1;
	ering->rx_pending = bp->rx_pending;

	/* XXX ethtool lacks a tx_max_pending, oops... */
}

static int b44_set_ringparam(struct net_device *dev,
			     struct ethtool_ringparam *ering)
{
	struct b44 *bp = netdev_priv(dev);

	if ((ering->rx_pending > B44_RX_RING_SIZE - 1) ||
	    (ering->rx_mini_pending != 0) ||
	    (ering->rx_jumbo_pending != 0) ||
	    (ering->tx_pending > B44_TX_RING_SIZE - 1))
		return -EINVAL;

	spin_lock_irq(&bp->lock);

	bp->rx_pending = ering->rx_pending;
	bp->tx_pending = ering->tx_pending;

	b44_halt(bp);
	b44_init_rings(bp);
	b44_init_hw(bp, B44_FULL_RESET);
	netif_wake_queue(bp->dev);
	spin_unlock_irq(&bp->lock);

	b44_enable_ints(bp);

	return 0;
}

static void b44_get_pauseparam(struct net_device *dev,
				struct ethtool_pauseparam *epause)
{
	struct b44 *bp = netdev_priv(dev);

	epause->autoneg =
		(bp->flags & B44_FLAG_PAUSE_AUTO) != 0;
	epause->rx_pause =
		(bp->flags & B44_FLAG_RX_PAUSE) != 0;
	epause->tx_pause =
		(bp->flags & B44_FLAG_TX_PAUSE) != 0;
}

static int b44_set_pauseparam(struct net_device *dev,
				struct ethtool_pauseparam *epause)
{
	struct b44 *bp = netdev_priv(dev);

	spin_lock_irq(&bp->lock);
	if (epause->autoneg)
		bp->flags |= B44_FLAG_PAUSE_AUTO;
	else
		bp->flags &= ~B44_FLAG_PAUSE_AUTO;
	if (epause->rx_pause)
		bp->flags |= B44_FLAG_RX_PAUSE;
	else
		bp->flags &= ~B44_FLAG_RX_PAUSE;
	if (epause->tx_pause)
		bp->flags |= B44_FLAG_TX_PAUSE;
	else
		bp->flags &= ~B44_FLAG_TX_PAUSE;
	if (bp->flags & B44_FLAG_PAUSE_AUTO) {
		b44_halt(bp);
		b44_init_rings(bp);
		b44_init_hw(bp, B44_FULL_RESET);
	} else {
		__b44_set_flow_ctrl(bp, bp->flags);
	}
	spin_unlock_irq(&bp->lock);

	b44_enable_ints(bp);

	return 0;
}

static void b44_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	switch(stringset) {
	case ETH_SS_STATS:
		memcpy(data, *b44_gstrings, sizeof(b44_gstrings));
		break;
	}
}

static int b44_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(b44_gstrings);
	default:
		return -EOPNOTSUPP;
	}
}

static void b44_get_ethtool_stats(struct net_device *dev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct b44 *bp = netdev_priv(dev);
	u32 *val = &bp->hw_stats.tx_good_octets;
	u32 i;

	spin_lock_irq(&bp->lock);

	b44_stats_update(bp);

	for (i = 0; i < ARRAY_SIZE(b44_gstrings); i++)
		*data++ = *val++;

	spin_unlock_irq(&bp->lock);
}

static void b44_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct b44 *bp = netdev_priv(dev);

	wol->supported = WAKE_MAGIC;
	if (bp->flags & B44_FLAG_WOL_ENABLE)
		wol->wolopts = WAKE_MAGIC;
	else
		wol->wolopts = 0;
	memset(&wol->sopass, 0, sizeof(wol->sopass));
}

static int b44_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct b44 *bp = netdev_priv(dev);

	spin_lock_irq(&bp->lock);
	if (wol->wolopts & WAKE_MAGIC)
		bp->flags |= B44_FLAG_WOL_ENABLE;
	else
		bp->flags &= ~B44_FLAG_WOL_ENABLE;
	spin_unlock_irq(&bp->lock);

	return 0;
}

static const struct ethtool_ops b44_ethtool_ops = {
	.get_drvinfo		= b44_get_drvinfo,
	.get_settings		= b44_get_settings,
	.set_settings		= b44_set_settings,
	.nway_reset		= b44_nway_reset,
	.get_link		= ethtool_op_get_link,
	.get_wol		= b44_get_wol,
	.set_wol		= b44_set_wol,
	.get_ringparam		= b44_get_ringparam,
	.set_ringparam		= b44_set_ringparam,
	.get_pauseparam		= b44_get_pauseparam,
	.set_pauseparam		= b44_set_pauseparam,
	.get_msglevel		= b44_get_msglevel,
	.set_msglevel		= b44_set_msglevel,
	.get_strings		= b44_get_strings,
	.get_sset_count		= b44_get_sset_count,
	.get_ethtool_stats	= b44_get_ethtool_stats,
};

static int b44_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mii_ioctl_data *data = if_mii(ifr);
	struct b44 *bp = netdev_priv(dev);
	int err = -EINVAL;

	if (!netif_running(dev))
		goto out;

	spin_lock_irq(&bp->lock);
	err = generic_mii_ioctl(&bp->mii_if, data, cmd, NULL);
	spin_unlock_irq(&bp->lock);
out:
	return err;
}

static int __devinit b44_get_invariants(struct b44 *bp)
{
	struct ssb_device *sdev = bp->sdev;
	int err = 0;
	u8 *addr;

	bp->dma_offset = ssb_dma_translation(sdev);

	if (sdev->bus->bustype == SSB_BUSTYPE_SSB &&
	    instance > 1) {
		addr = sdev->bus->sprom.r1.et1mac;
		bp->phy_addr = sdev->bus->sprom.r1.et1phyaddr;
	} else {
		addr = sdev->bus->sprom.r1.et0mac;
		bp->phy_addr = sdev->bus->sprom.r1.et0phyaddr;
	}
	memcpy(bp->dev->dev_addr, addr, 6);

	if (!is_valid_ether_addr(&bp->dev->dev_addr[0])){
		printk(KERN_ERR PFX "Invalid MAC address found in EEPROM\n");
		return -EINVAL;
	}

	memcpy(bp->dev->perm_addr, bp->dev->dev_addr, bp->dev->addr_len);

	bp->imask = IMASK_DEF;

	/* XXX - really required?
	   bp->flags |= B44_FLAG_BUGGY_TXPTR;
	*/

	if (bp->sdev->id.revision >= 7)
		bp->flags |= B44_FLAG_B0_ANDLATER;

	return err;
}

static int __devinit b44_init_one(struct ssb_device *sdev,
				  const struct ssb_device_id *ent)
{
	static int b44_version_printed = 0;
	struct net_device *dev;
	struct b44 *bp;
	int err;
	DECLARE_MAC_BUF(mac);

	instance++;

	if (b44_version_printed++ == 0)
		printk(KERN_INFO "%s", version);


	dev = alloc_etherdev(sizeof(*bp));
	if (!dev) {
		dev_err(sdev->dev, "Etherdev alloc failed, aborting.\n");
		err = -ENOMEM;
		goto out;
	}

	SET_NETDEV_DEV(dev, sdev->dev);

	/* No interesting netdevice features in this card... */
	dev->features |= 0;

	bp = netdev_priv(dev);
	bp->sdev = sdev;
	bp->dev = dev;

	bp->msg_enable = netif_msg_init(b44_debug, B44_DEF_MSG_ENABLE);

	spin_lock_init(&bp->lock);

	bp->rx_pending = B44_DEF_RX_RING_PENDING;
	bp->tx_pending = B44_DEF_TX_RING_PENDING;

	dev->open = b44_open;
	dev->stop = b44_close;
	dev->hard_start_xmit = b44_start_xmit;
	dev->get_stats = b44_get_stats;
	dev->set_multicast_list = b44_set_rx_mode;
	dev->set_mac_address = b44_set_mac_addr;
	dev->do_ioctl = b44_ioctl;
	dev->tx_timeout = b44_tx_timeout;
	netif_napi_add(dev, &bp->napi, b44_poll, 64);
	dev->watchdog_timeo = B44_TX_TIMEOUT;
#ifdef CONFIG_NET_POLL_CONTROLLER
	dev->poll_controller = b44_poll_controller;
#endif
	dev->change_mtu = b44_change_mtu;
	dev->irq = sdev->irq;
	SET_ETHTOOL_OPS(dev, &b44_ethtool_ops);

	netif_carrier_off(dev);

	err = ssb_bus_powerup(sdev->bus, 0);
	if (err) {
		dev_err(sdev->dev,
			"Failed to powerup the bus\n");
		goto err_out_free_dev;
	}
	err = ssb_dma_set_mask(sdev, DMA_30BIT_MASK);
	if (err) {
		dev_err(sdev->dev,
			"Required 30BIT DMA mask unsupported by the system.\n");
		goto err_out_powerdown;
	}
	err = b44_get_invariants(bp);
	if (err) {
		dev_err(sdev->dev,
			"Problem fetching invariants of chip, aborting.\n");
		goto err_out_powerdown;
	}

	bp->mii_if.dev = dev;
	bp->mii_if.mdio_read = b44_mii_read;
	bp->mii_if.mdio_write = b44_mii_write;
	bp->mii_if.phy_id = bp->phy_addr;
	bp->mii_if.phy_id_mask = 0x1f;
	bp->mii_if.reg_num_mask = 0x1f;

	/* By default, advertise all speed/duplex settings. */
	bp->flags |= (B44_FLAG_ADV_10HALF | B44_FLAG_ADV_10FULL |
		      B44_FLAG_ADV_100HALF | B44_FLAG_ADV_100FULL);

	/* By default, auto-negotiate PAUSE. */
	bp->flags |= B44_FLAG_PAUSE_AUTO;

	err = register_netdev(dev);
	if (err) {
		dev_err(sdev->dev, "Cannot register net device, aborting.\n");
		goto err_out_powerdown;
	}

	ssb_set_drvdata(sdev, dev);

	/* Chip reset provides power to the b44 MAC & PCI cores, which
	 * is necessary for MAC register access.
	 */
	b44_chip_reset(bp);

	printk(KERN_INFO "%s: Broadcom 44xx/47xx 10/100BaseT Ethernet %s\n",
	       dev->name, print_mac(mac, dev->dev_addr));

	return 0;

err_out_powerdown:
	ssb_bus_may_powerdown(sdev->bus);

err_out_free_dev:
	free_netdev(dev);

out:
	return err;
}

static void __devexit b44_remove_one(struct ssb_device *sdev)
{
	struct net_device *dev = ssb_get_drvdata(sdev);

	unregister_netdev(dev);
	ssb_bus_may_powerdown(sdev->bus);
	free_netdev(dev);
	ssb_set_drvdata(sdev, NULL);
}

static int b44_suspend(struct ssb_device *sdev, pm_message_t state)
{
	struct net_device *dev = ssb_get_drvdata(sdev);
	struct b44 *bp = netdev_priv(dev);

	if (!netif_running(dev))
		return 0;

	del_timer_sync(&bp->timer);

	spin_lock_irq(&bp->lock);

	b44_halt(bp);
	netif_carrier_off(bp->dev);
	netif_device_detach(bp->dev);
	b44_free_rings(bp);

	spin_unlock_irq(&bp->lock);

	free_irq(dev->irq, dev);
	if (bp->flags & B44_FLAG_WOL_ENABLE) {
		b44_init_hw(bp, B44_PARTIAL_RESET);
		b44_setup_wol(bp);
	}

	return 0;
}

static int b44_resume(struct ssb_device *sdev)
{
	struct net_device *dev = ssb_get_drvdata(sdev);
	struct b44 *bp = netdev_priv(dev);
	int rc = 0;

	rc = ssb_bus_powerup(sdev->bus, 0);
	if (rc) {
		dev_err(sdev->dev,
			"Failed to powerup the bus\n");
		return rc;
	}

	if (!netif_running(dev))
		return 0;

	rc = request_irq(dev->irq, b44_interrupt, IRQF_SHARED, dev->name, dev);
	if (rc) {
		printk(KERN_ERR PFX "%s: request_irq failed\n", dev->name);
		return rc;
	}

	spin_lock_irq(&bp->lock);

	b44_init_rings(bp);
	b44_init_hw(bp, B44_FULL_RESET);
	netif_device_attach(bp->dev);
	spin_unlock_irq(&bp->lock);

	b44_enable_ints(bp);
	netif_wake_queue(dev);

	mod_timer(&bp->timer, jiffies + 1);

	return 0;
}

static struct ssb_driver b44_ssb_driver = {
	.name		= DRV_MODULE_NAME,
	.id_table	= b44_ssb_tbl,
	.probe		= b44_init_one,
	.remove		= __devexit_p(b44_remove_one),
	.suspend	= b44_suspend,
	.resume		= b44_resume,
};

static inline int b44_pci_init(void)
{
	int err = 0;
#ifdef CONFIG_B44_PCI
	err = ssb_pcihost_register(&b44_pci_driver);
#endif
	return err;
}

static inline void b44_pci_exit(void)
{
#ifdef CONFIG_B44_PCI
	ssb_pcihost_unregister(&b44_pci_driver);
#endif
}

static int __init b44_init(void)
{
	unsigned int dma_desc_align_size = dma_get_cache_alignment();
	int err;

	/* Setup paramaters for syncing RX/TX DMA descriptors */
	dma_desc_align_mask = ~(dma_desc_align_size - 1);
	dma_desc_sync_size = max_t(unsigned int, dma_desc_align_size, sizeof(struct dma_desc));

	err = b44_pci_init();
	if (err)
		return err;
	err = ssb_driver_register(&b44_ssb_driver);
	if (err)
		b44_pci_exit();
	return err;
}

static void __exit b44_cleanup(void)
{
	ssb_driver_unregister(&b44_ssb_driver);
	b44_pci_exit();
}

module_init(b44_init);
module_exit(b44_cleanup);

