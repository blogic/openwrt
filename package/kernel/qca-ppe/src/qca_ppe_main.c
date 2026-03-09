// SPDX-License-Identifier: GPL-2.0-or-later OR MIT

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/if_bridge.h>

#include <qca_uniphy.h>

#include "qca_ppe.h"

static void ppe_port_mac_set(struct qca_ppe_priv *priv, int port,
			     bool tx_en, bool rx_en)
{
	int gmac = port - 1;
	u32 val = 0;

	if (port < 1 || port >= priv->ppe.data->num_ports)
		return;

	if (tx_en)
		val |= PPE_MAC_ENABLE_TXMAC_EN;
	if (rx_en)
		val |= PPE_MAC_ENABLE_RXMAC_EN;
	ppe_m32(&priv->ppe, PPE_GMAC_ENABLE(gmac),
		PPE_MAC_ENABLE_TXMAC_EN | PPE_MAC_ENABLE_RXMAC_EN, val);
}

static void ppe_port_bridge_txmac_set(struct qca_ppe_priv *priv, int port,
				      bool enable)
{
	ppe_m32(&priv->ppe, PPE_PORT_BRIDGE_CTRL(port),
		PPE_PORT_BRIDGE_CTRL_TXMAC_EN,
		enable ? PPE_PORT_BRIDGE_CTRL_TXMAC_EN : 0);
}

static void ppe_gmac_link_up(struct qca_ppe_priv *priv, int port,
			     int speed, int duplex,
			     bool tx_pause, bool rx_pause)
{
	int gmac = port - 1;
	u32 val;

	val = ppe_r32(&priv->ppe, PPE_GMAC_SPEED(gmac));
	val &= ~PPE_GMAC_SPEED_MASK;
	switch (speed) {
	case SPEED_100:
		val |= FIELD_PREP(PPE_GMAC_SPEED_MASK, 1);
		break;
	case SPEED_1000:
	case SPEED_2500:
		val |= FIELD_PREP(PPE_GMAC_SPEED_MASK, 2);
		break;
	}
	ppe_w32(&priv->ppe, PPE_GMAC_SPEED(gmac), val);

	val = 0;
	if (duplex == DUPLEX_FULL)
		val |= PPE_MAC_ENABLE_DUPLEX;
	if (tx_pause)
		val |= PPE_MAC_ENABLE_TX_FLOW_EN;
	if (rx_pause)
		val |= PPE_MAC_ENABLE_RX_FLOW_EN;
	ppe_m32(&priv->ppe, PPE_GMAC_ENABLE(gmac),
		PPE_MAC_ENABLE_DUPLEX | PPE_MAC_ENABLE_TX_FLOW_EN |
		PPE_MAC_ENABLE_RX_FLOW_EN, val);
}

static void ppe_port_cnt_enable(struct qca_ppe_priv *priv, int port)
{
	ppe_m32(&priv->ppe, PPE_MRU_MTU_CTRL(port) + 4,
		PPE_MRU_MTU_CTRL_RX_CNT_EN | PPE_MRU_MTU_CTRL_TX_CNT_EN,
		PPE_MRU_MTU_CTRL_RX_CNT_EN | PPE_MRU_MTU_CTRL_TX_CNT_EN);

	ppe_m32(&priv->ppe, PPE_MC_MTU_CTRL(port),
		PPE_MC_MTU_CTRL_TX_CNT_EN, PPE_MC_MTU_CTRL_TX_CNT_EN);

	ppe_m32(&priv->ppe, PPE_PORT_EG_VLAN(port),
		PPE_PORT_EG_VLAN_TX_CNT_EN, PPE_PORT_EG_VLAN_TX_CNT_EN);
}

int ppe_vsi_alloc(struct qca_ppe_priv *priv)
{
	int vsi;

	vsi = find_first_zero_bit(priv->vsi_bitmap, PPE_VSI_MAX);
	if (vsi >= PPE_VSI_MAX)
		return -ENOSPC;

	set_bit(vsi, priv->vsi_bitmap);

	ppe_w32(&priv->ppe, PPE_VSI_TBL(vsi), 0);
	ppe_w32(&priv->ppe, PPE_VSI_TBL(vsi) + 4,
		PPE_VSI_TBL_NEW_ADDR_LRN_EN | PPE_VSI_TBL_STA_MOVE_LRN_EN);

	return vsi;
}

void ppe_vsi_free(struct qca_ppe_priv *priv, u32 vsi)
{
	ppe_w32(&priv->ppe, PPE_VSI_TBL(vsi), 0);
	ppe_w32(&priv->ppe, PPE_VSI_TBL(vsi) + 4, 0);
	clear_bit(vsi, priv->vsi_bitmap);
}

void ppe_vsi_member_set(struct qca_ppe_priv *priv, u32 vsi,
			       u32 portmask)
{
	u32 val;

	val = FIELD_PREP(PPE_VSI_TBL_MEMBER, portmask) |
	      FIELD_PREP(PPE_VSI_TBL_UUC, portmask) |
	      FIELD_PREP(PPE_VSI_TBL_UMC, portmask) |
	      FIELD_PREP(PPE_VSI_TBL_BC, portmask);
	ppe_w32(&priv->ppe, PPE_VSI_TBL(vsi), val);
	ppe_w32(&priv->ppe, PPE_VSI_TBL(vsi) + 4,
		PPE_VSI_TBL_NEW_ADDR_LRN_EN | PPE_VSI_TBL_STA_MOVE_LRN_EN);
}

static void ppe_port_vsi_set(struct qca_ppe_priv *priv, int port, u32 vsi)
{
	u32 val;

	val = ppe_r32(&priv->ppe, PPE_L3_VP_PORT_TBL(port) + 4);
	val &= ~(PPE_L3_VP_VSI_VALID | PPE_L3_VP_VSI);
	if (vsi != PPE_VSI_INVALID) {
		val |= PPE_L3_VP_VSI_VALID;
		val |= FIELD_PREP(PPE_L3_VP_VSI, vsi);
	}
	ppe_w32(&priv->ppe, PPE_L3_VP_PORT_TBL(port) + 4, val);
}

static int ppe_fdb_op_wait(struct qca_ppe_priv *priv, u32 rslt_reg,
			   u32 cmd_id)
{
	u32 val;
	int i;

	for (i = 0; i < 100; i++) {
		val = ppe_r32(&priv->ppe, rslt_reg);
		if (FIELD_GET(PPE_FDB_RSLT_CMD_ID, val) == cmd_id)
			return 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

static void ppe_fdb_encode(const unsigned char *addr, int port, u16 vid,
			   bool is_static, u32 *data0, u32 *data1, u32 *data2)
{
	*data0 = (addr[2] << 24) | (addr[3] << 16) | (addr[4] << 8) | addr[5];

	*data1 = (addr[0] << 8) | addr[1];
	*data1 |= PPE_FDB_DATA1_VALID | PPE_FDB_DATA1_LKP_VALID;
	*data1 |= FIELD_PREP(PPE_FDB_DATA1_VSI, vid);
	*data1 |= FIELD_PREP(PPE_FDB_DATA1_DST_LO, port);

	*data2 = FIELD_PREP(PPE_FDB_DATA2_DST_TYPE, PPE_FDB_DST_PORT) |
		 FIELD_PREP(PPE_FDB_DATA2_HIT_AGE,
			    is_static ? PPE_FDB_AGE_STATIC : 2);
}

static int ppe_fdb_op(struct qca_ppe_priv *priv, const unsigned char *addr,
		      int port, u16 vid, u32 op_type)
{
	u32 data0, data1, data2;
	int ret;

	ppe_fdb_encode(addr, port, vid, op_type == PPE_FDB_OP_ADD,
		       &data0, &data1, &data2);

	spin_lock_bh(&priv->fdb_lock);

	ppe_w32(&priv->ppe, PPE_FDB_OP_DATA0, data0);
	ppe_w32(&priv->ppe, PPE_FDB_OP_DATA1, data1);
	ppe_w32(&priv->ppe, PPE_FDB_OP_DATA2, data2);
	ppe_w32(&priv->ppe, PPE_FDB_OP,
		FIELD_PREP(PPE_FDB_OP_TYPE, op_type) |
		FIELD_PREP(PPE_FDB_OP_HASH_BLOCK, 3));

	ret = ppe_fdb_op_wait(priv, PPE_FDB_OP_RSLT, 0);

	spin_unlock_bh(&priv->fdb_lock);

	return ret;
}

static int ppe_fdb_read_entry(struct qca_ppe_priv *priv, u32 index,
			      unsigned char *addr, u16 *vid, int *port,
			      bool *is_static)
{
	u32 data0, data1, data2, cmd_id, val;
	int ret;

	cmd_id = index % 15;

	spin_lock_bh(&priv->fdb_lock);

	ppe_w32(&priv->ppe, PPE_FDB_RD_OP_DATA0, 0);
	ppe_w32(&priv->ppe, PPE_FDB_RD_OP_DATA1, 0);
	ppe_w32(&priv->ppe, PPE_FDB_RD_OP_DATA2, 0);

	val = FIELD_PREP(PPE_FDB_OP_CMD_ID, cmd_id) |
	      FIELD_PREP(PPE_FDB_OP_TYPE, PPE_FDB_OP_GET) |
	      FIELD_PREP(PPE_FDB_OP_HASH_BLOCK, 3) |
	      PPE_FDB_OP_MODE |
	      FIELD_PREP(PPE_FDB_OP_ENTRY_IDX, index);
	ppe_w32(&priv->ppe, PPE_FDB_RD_OP, val);

	ret = ppe_fdb_op_wait(priv, PPE_FDB_RD_OP_RSLT, cmd_id);
	if (ret)
		goto unlock;

	data0 = ppe_r32(&priv->ppe, PPE_FDB_RD_RSLT_DATA0);
	data1 = ppe_r32(&priv->ppe, PPE_FDB_RD_RSLT_DATA1);
	data2 = ppe_r32(&priv->ppe, PPE_FDB_RD_RSLT_DATA2);

unlock:
	spin_unlock_bh(&priv->fdb_lock);

	if (ret)
		return ret;

	if (!(data1 & PPE_FDB_DATA1_VALID))
		return -ENOENT;

	if (FIELD_GET(PPE_FDB_DATA2_DST_TYPE, data2) != PPE_FDB_DST_PORT)
		return -ENOENT;

	addr[2] = (data0 >> 24) & 0xff;
	addr[3] = (data0 >> 16) & 0xff;
	addr[4] = (data0 >> 8) & 0xff;
	addr[5] = data0 & 0xff;
	addr[0] = (data1 >> 8) & 0xff;
	addr[1] = data1 & 0xff;

	*vid = FIELD_GET(PPE_FDB_DATA1_VSI, data1);
	*port = FIELD_GET(PPE_FDB_DATA1_DST_LO, data1) |
		(FIELD_GET(PPE_FDB_DATA2_DST_HI, data2) << 9);
	*is_static = FIELD_GET(PPE_FDB_DATA2_HIT_AGE, data2) == PPE_FDB_AGE_STATIC;

	return 0;
}

static int ppe_fdb_flush(struct qca_ppe_priv *priv)
{
	int ret;

	spin_lock_bh(&priv->fdb_lock);

	ppe_w32(&priv->ppe, PPE_FDB_OP,
		FIELD_PREP(PPE_FDB_OP_TYPE, PPE_FDB_OP_FLUSH));

	ret = ppe_fdb_op_wait(priv, PPE_FDB_OP_RSLT, 0);

	spin_unlock_bh(&priv->fdb_lock);

	return ret;
}

static void ppe_fdb_encode_mcast(const unsigned char *addr, u32 portmap,
				 u16 vid, u32 *data0, u32 *data1, u32 *data2)
{
	*data0 = (addr[2] << 24) | (addr[3] << 16) | (addr[4] << 8) | addr[5];

	*data1 = (addr[0] << 8) | addr[1];
	*data1 |= PPE_FDB_DATA1_VALID | PPE_FDB_DATA1_LKP_VALID;
	*data1 |= FIELD_PREP(PPE_FDB_DATA1_VSI, vid);
	*data1 |= FIELD_PREP(PPE_FDB_DATA1_DST_LO, portmap);

	*data2 = FIELD_PREP(PPE_FDB_DATA2_DST_HI, portmap >> 9) |
		 FIELD_PREP(PPE_FDB_DATA2_DST_TYPE, PPE_FDB_DST_PORTMAP) |
		 FIELD_PREP(PPE_FDB_DATA2_HIT_AGE, PPE_FDB_AGE_STATIC);
}

static int ppe_fdb_lookup(struct qca_ppe_priv *priv,
			  const unsigned char *addr, u16 vid, u32 *portmap)
{
	u32 data1, data2;
	int ret;

	spin_lock_bh(&priv->fdb_lock);

	ppe_w32(&priv->ppe, PPE_FDB_RD_OP_DATA0,
		(addr[2] << 24) | (addr[3] << 16) | (addr[4] << 8) | addr[5]);
	ppe_w32(&priv->ppe, PPE_FDB_RD_OP_DATA1,
		((addr[0] << 8) | addr[1]) |
		FIELD_PREP(PPE_FDB_DATA1_VSI, vid));
	ppe_w32(&priv->ppe, PPE_FDB_RD_OP_DATA2, 0);

	ppe_w32(&priv->ppe, PPE_FDB_RD_OP,
		FIELD_PREP(PPE_FDB_OP_TYPE, PPE_FDB_OP_GET) |
		FIELD_PREP(PPE_FDB_OP_HASH_BLOCK, 3));

	ret = ppe_fdb_op_wait(priv, PPE_FDB_RD_OP_RSLT, 0);
	if (ret)
		goto out;

	data1 = ppe_r32(&priv->ppe, PPE_FDB_RD_RSLT_DATA1);
	data2 = ppe_r32(&priv->ppe, PPE_FDB_RD_RSLT_DATA2);

	if (!(data1 & PPE_FDB_DATA1_VALID)) {
		ret = -ENOENT;
		goto out;
	}

	*portmap = FIELD_GET(PPE_FDB_DATA1_DST_LO, data1) |
		   (FIELD_GET(PPE_FDB_DATA2_DST_HI, data2) << 9);

out:
	spin_unlock_bh(&priv->fdb_lock);
	return ret;
}

static int ppe_fdb_mcast_op(struct qca_ppe_priv *priv,
			    const unsigned char *addr, u32 portmap,
			    u16 vid, u32 op_type)
{
	u32 data0, data1, data2;
	int ret;

	ppe_fdb_encode_mcast(addr, portmap, vid, &data0, &data1, &data2);

	spin_lock_bh(&priv->fdb_lock);

	ppe_w32(&priv->ppe, PPE_FDB_OP_DATA0, data0);
	ppe_w32(&priv->ppe, PPE_FDB_OP_DATA1, data1);
	ppe_w32(&priv->ppe, PPE_FDB_OP_DATA2, data2);
	ppe_w32(&priv->ppe, PPE_FDB_OP,
		FIELD_PREP(PPE_FDB_OP_TYPE, op_type) |
		FIELD_PREP(PPE_FDB_OP_HASH_BLOCK, 3));

	ret = ppe_fdb_op_wait(priv, PPE_FDB_OP_RSLT, 0);

	spin_unlock_bh(&priv->fdb_lock);

	return ret;
}

static enum dsa_tag_protocol
qca_ppe_get_tag_protocol(struct dsa_switch *ds, int port,
			     enum dsa_tag_protocol mprot)
{
	return DSA_TAG_PROTO_OOB;
}

static int qca_ppe_setup(struct dsa_switch *ds)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	int num_ports = ds->num_ports;
	u32 port_mask;
	u32 val;
	int i;

	port_mask = BIT(num_ports) - 1;

	for (i = 0; i < num_ports; i++)
		priv->port_vsi[i] = PPE_VSI_INVALID;

	ppe_w32(&priv->ppe, PPE_FDB_OP, 0);

	for (i = 0; i < num_ports; i++) {
		ppe_w32(&priv->ppe, PPE_CST_STATE(i), PPE_STP_FORWARDING);

		ppe_w32(&priv->ppe, PPE_MRU_MTU_CTRL(i),
			PPE_DEFAULT_MTU | (PPE_DEFAULT_MTU << PPE_MTU_SHIFT));

		if (i >= 1)
			ppe_w32(&priv->ppe, PPE_GMAC_MIB_CTRL(i - 1),
				PPE_MIB_EN);

		val = PPE_BRIDGE_NEW_LRN_EN |
		      PPE_BRIDGE_STA_MOVE_EN |
		      FIELD_PREP(PPE_BRIDGE_PORT_ISOL, port_mask);
		if (dsa_is_cpu_port(ds, i))
			val |= PPE_PORT_BRIDGE_CTRL_TXMAC_EN;
		ppe_m32(&priv->ppe, PPE_PORT_BRIDGE_CTRL(i),
			PPE_BRIDGE_NEW_LRN_EN |
			PPE_BRIDGE_STA_MOVE_EN |
			PPE_BRIDGE_PORT_ISOL |
			PPE_PORT_BRIDGE_CTRL_TXMAC_EN,
			val);

		ppe_port_cnt_enable(priv, i);
	}

	qca_ppe_vlan_setup(ds);

	set_bit(0, priv->vsi_bitmap);
	val = FIELD_PREP(PPE_VSI_TBL_MEMBER,
			 dsa_user_ports(ds) | BIT(QCA_PPE_CPU_PORT)) |
	      FIELD_PREP(PPE_VSI_TBL_UUC, BIT(QCA_PPE_CPU_PORT)) |
	      FIELD_PREP(PPE_VSI_TBL_UMC, BIT(QCA_PPE_CPU_PORT)) |
	      FIELD_PREP(PPE_VSI_TBL_BC, BIT(QCA_PPE_CPU_PORT));
	ppe_w32(&priv->ppe, PPE_VSI_TBL(0), val);
	ppe_w32(&priv->ppe, PPE_VSI_TBL(0) + 4,
		PPE_VSI_TBL_NEW_ADDR_LRN_EN | PPE_VSI_TBL_STA_MOVE_LRN_EN);

	for (i = 1; i < num_ports; i++)
		ppe_port_vsi_set(priv, i, 0);

	ppe_fdb_flush(priv);

	ppe_m32(&priv->ppe, PPE_L2_GLOBAL_CONF,
		PPE_L2_LRN_EN | PPE_L2_AGE_EN,
		PPE_L2_LRN_EN | PPE_L2_AGE_EN);

	ds->ageing_time_min = PPE_AGE_UNIT_MS;
	ds->ageing_time_max = (unsigned int)min_t(u64,
		(u64)PPE_AGE_UNIT_MS * PPE_AGE_TIMER_MASK, U32_MAX);
	ds->assisted_learning_on_cpu_port = true;

	return 0;
}

static int qca_ppe_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	u32 timer = msecs / PPE_AGE_UNIT_MS;

	ppe_m32(&priv->ppe, PPE_AGE_TIMER, PPE_AGE_TIMER_MASK,
		FIELD_PREP(PPE_AGE_TIMER_MASK, timer));

	return 0;
}

static int qca_ppe_port_enable(struct dsa_switch *ds, int port,
				   struct phy_device *phy)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);

	ppe_port_mac_set(priv, port, true, true);

	return 0;
}

static void qca_ppe_port_disable(struct dsa_switch *ds, int port)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);

	ppe_port_mac_set(priv, port, false, false);
}

static struct qca_ppe_bridge_vsi *
bridge_vsi_find(struct qca_ppe_priv *priv, struct net_device *br_dev)
{
	int i;

	for (i = 0; i < QCA_PPE_MAX_BRIDGES; i++)
		if (priv->bridges[i].br_dev == br_dev)
			return &priv->bridges[i];

	return NULL;
}

static struct qca_ppe_bridge_vsi *
bridge_vsi_alloc(struct qca_ppe_priv *priv, struct net_device *br_dev)
{
	int vsi, i;

	vsi = ppe_vsi_alloc(priv);
	if (vsi < 0)
		return NULL;

	for (i = 0; i < QCA_PPE_MAX_BRIDGES; i++) {
		if (priv->bridges[i].br_dev)
			continue;

		priv->bridges[i].br_dev = br_dev;
		priv->bridges[i].vsi = vsi;
		priv->bridges[i].refcount = 0;
		return &priv->bridges[i];
	}

	ppe_vsi_free(priv, vsi);
	return NULL;
}

static void bridge_vsi_put(struct qca_ppe_priv *priv,
			   struct qca_ppe_bridge_vsi *bvsi)
{
	bvsi->refcount--;
	if (bvsi->refcount > 0)
		return;

	ppe_vsi_free(priv, bvsi->vsi);
	bvsi->br_dev = NULL;
	bvsi->vsi = 0;
}

static void bridge_vsi_members_update(struct qca_ppe_priv *priv,
				      struct qca_ppe_bridge_vsi *bvsi)
{
	u32 portmask = 0;
	int i;

	for (i = 0; i < priv->ds.num_ports; i++)
		if (priv->port_vsi[i] != PPE_VSI_INVALID &&
		    priv->port_vsi[i] == bvsi->vsi)
			portmask |= BIT(i);

	portmask |= BIT(QCA_PPE_CPU_PORT);

	ppe_vsi_member_set(priv, bvsi->vsi, portmask);
}

static int qca_ppe_port_bridge_join(struct dsa_switch *ds, int port,
					struct dsa_bridge bridge,
					bool *tx_fwd_offload,
					struct netlink_ext_ack *extack)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	struct qca_ppe_bridge_vsi *bvsi;

	bvsi = bridge_vsi_find(priv, bridge.dev);
	if (!bvsi) {
		bvsi = bridge_vsi_alloc(priv, bridge.dev);
		if (!bvsi)
			return -ENOSPC;
	}

	bvsi->refcount++;
	priv->port_vsi[port] = bvsi->vsi;
	priv->port_br_dev[port] = bridge.dev;

	ppe_port_vsi_set(priv, port, bvsi->vsi);
	bridge_vsi_members_update(priv, bvsi);

	return 0;
}

static void qca_ppe_port_bridge_leave(struct dsa_switch *ds, int port,
					  struct dsa_bridge bridge)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	struct qca_ppe_bridge_vsi *bvsi;

	bvsi = bridge_vsi_find(priv, bridge.dev);
	if (!bvsi)
		return;

	priv->port_vsi[port] = PPE_VSI_INVALID;
	priv->port_br_dev[port] = NULL;
	ppe_port_vsi_set(priv, port, PPE_VSI_INVALID);
	bridge_vsi_members_update(priv, bvsi);
	bridge_vsi_put(priv, bvsi);
}

static int qca_ppe_port_fdb_add(struct dsa_switch *ds, int port,
				    const unsigned char *addr, u16 vid,
				    struct dsa_db db)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);

	return ppe_fdb_op(priv, addr, port, vid, PPE_FDB_OP_ADD);
}

static int qca_ppe_port_fdb_del(struct dsa_switch *ds, int port,
				    const unsigned char *addr, u16 vid,
				    struct dsa_db db)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);

	return ppe_fdb_op(priv, addr, port, vid, PPE_FDB_OP_DEL);
}

static int qca_ppe_port_fdb_dump(struct dsa_switch *ds, int port,
				     dsa_fdb_dump_cb_t *cb, void *data)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	unsigned char addr[ETH_ALEN];
	bool is_static;
	int fdb_port;
	u16 vid;
	u32 i;

	for (i = 0; i < PPE_FDB_TBL_NUM; i++) {
		if (ppe_fdb_read_entry(priv, i, addr, &vid, &fdb_port,
				       &is_static))
			continue;

		if (fdb_port != port)
			continue;

		if (cb(addr, vid, is_static, data))
			break;
	}

	return 0;
}

static int qca_ppe_port_mdb_add(struct dsa_switch *ds, int port,
				    const struct switchdev_obj_port_mdb *mdb,
				    struct dsa_db db)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	u32 portmap;
	int ret;

	ret = ppe_fdb_lookup(priv, mdb->addr, mdb->vid, &portmap);
	if (ret)
		portmap = BIT(QCA_PPE_CPU_PORT);

	portmap |= BIT(port);

	return ppe_fdb_mcast_op(priv, mdb->addr, portmap,
				mdb->vid, PPE_FDB_OP_ADD);
}

static int qca_ppe_port_mdb_del(struct dsa_switch *ds, int port,
				    const struct switchdev_obj_port_mdb *mdb,
				    struct dsa_db db)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	u32 portmap;
	int ret;

	ret = ppe_fdb_lookup(priv, mdb->addr, mdb->vid, &portmap);
	if (ret)
		return ret;

	portmap &= ~BIT(port);

	if (!portmap || portmap == BIT(QCA_PPE_CPU_PORT))
		return ppe_fdb_mcast_op(priv, mdb->addr, 0,
					mdb->vid, PPE_FDB_OP_DEL);

	return ppe_fdb_mcast_op(priv, mdb->addr, portmap,
				mdb->vid, PPE_FDB_OP_ADD);
}

static void qca_ppe_phylink_get_caps(struct dsa_switch *ds, int port,
					 struct phylink_config *config)
{
	switch (port) {
	case 0:
		config->mac_capabilities =
			MAC_1000FD | MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		break;
	case 1 ... 4:
		config->mac_capabilities =
			MAC_1000FD | MAC_100FD | MAC_10FD |
			MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

		__set_bit(PHY_INTERFACE_MODE_QSGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_RGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_RGMII_ID,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_RGMII_RXID,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_RGMII_TXID,
			  config->supported_interfaces);
		break;
	case 5 ... 6:
		config->mac_capabilities =
			MAC_10000FD | MAC_5000FD | MAC_2500FD |
			MAC_1000FD | MAC_100FD | MAC_10FD |
			MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_USXGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  config->supported_interfaces);
		break;
	}
}

static struct phylink_pcs *
qca_ppe_mac_select_pcs(struct phylink_config *config,
			    phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct qca_ppe_priv *priv = ds_to_priv(dp->ds);

	return priv->port_pcs[dp->index];
}

static void qca_ppe_mac_config(struct phylink_config *config,
				    unsigned int mode,
				    const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct qca_ppe_priv *priv = ds_to_priv(dp->ds);
	int port = dp->index;

	if (priv->port_rst[port]) {
		reset_control_assert(priv->port_rst[port]);
		msleep(150);
		reset_control_deassert(priv->port_rst[port]);
	}
}

static void qca_ppe_mac_link_down(struct phylink_config *config,
				       unsigned int mode,
				       phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct qca_ppe_priv *priv = ds_to_priv(dp->ds);
	int port = dp->index;

	ppe_port_bridge_txmac_set(priv, port, false);
	ppe_port_mac_set(priv, port, false, false);
}

static void qca_ppe_mac_link_up(struct phylink_config *config,
				     struct phy_device *phydev,
				     unsigned int mode,
				     phy_interface_t interface,
				     int speed, int duplex,
				     bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct qca_ppe_priv *priv = ds_to_priv(dp->ds);
	int port = dp->index;
	unsigned long rate;

	ppe_gmac_link_up(priv, port, speed, duplex, tx_pause, rx_pause);

	switch (speed) {
	case SPEED_10:
		rate = 2500000;
		break;
	case SPEED_100:
		rate = 25000000;
		break;
	case SPEED_1000:
		rate = 125000000;
		break;
	case SPEED_2500:
		rate = 312500000;
		break;
	default:
		rate = 125000000;
		break;
	}

	if (priv->port_rx_clk[port])
		clk_set_rate(priv->port_rx_clk[port], rate);
	if (priv->port_tx_clk[port])
		clk_set_rate(priv->port_tx_clk[port], rate);

	ppe_port_mac_set(priv, port, true, true);
	ppe_port_bridge_txmac_set(priv, port, true);
}

static const struct phylink_mac_ops qca_ppe_phylink_mac_ops = {
	.mac_select_pcs	= qca_ppe_mac_select_pcs,
	.mac_config	= qca_ppe_mac_config,
	.mac_link_down	= qca_ppe_mac_link_down,
	.mac_link_up	= qca_ppe_mac_link_up,
};

struct qca_ppe_mib_desc {
	unsigned int offset;
	unsigned int size;
	const char name[ETH_GSTRING_LEN];
};

#define MIB32(_off, _name)	{ .offset = (_off), .size = 1, .name = _name }
#define MIB64(_off, _name)	{ .offset = (_off), .size = 2, .name = _name }

static const struct qca_ppe_mib_desc qca_ppe_mib[] = {
	MIB32(PPE_MIB_RXBROAD,		"rx_broadcast"),
	MIB32(PPE_MIB_RXPAUSE,		"rx_pause"),
	MIB32(PPE_MIB_RXMULTI,		"rx_multicast"),
	MIB32(PPE_MIB_RXFCSERR,		"rx_fcs_error"),
	MIB32(PPE_MIB_RXALIGNERR,	"rx_align_error"),
	MIB32(PPE_MIB_RXRUNT,		"rx_runt"),
	MIB32(PPE_MIB_RXFRAG,		"rx_fragment"),
	MIB32(PPE_MIB_RXJUMBOFCSERR,	"rx_jumbo_fcs_error"),
	MIB32(PPE_MIB_RXJUMBOALIGNERR,	"rx_jumbo_align_error"),
	MIB32(PPE_MIB_RXPKT64,		"rx_64byte"),
	MIB32(PPE_MIB_RXPKT65TO127,	"rx_65_127byte"),
	MIB32(PPE_MIB_RXPKT128TO255,	"rx_128_255byte"),
	MIB32(PPE_MIB_RXPKT256TO511,	"rx_256_511byte"),
	MIB32(PPE_MIB_RXPKT512TO1023,	"rx_512_1023byte"),
	MIB32(PPE_MIB_RXPKT1024TO1518,	"rx_1024_1518byte"),
	MIB32(PPE_MIB_RXPKT1519TOX,	"rx_1519_maxbyte"),
	MIB32(PPE_MIB_RXTOOLONG,	"rx_too_long"),
	MIB64(PPE_MIB_RXGOODBYTE_L,	"rx_good_bytes"),
	MIB64(PPE_MIB_RXBADBYTE_L,	"rx_bad_bytes"),
	MIB32(PPE_MIB_RXUNI,		"rx_unicast"),
	MIB32(PPE_MIB_TXBROAD,		"tx_broadcast"),
	MIB32(PPE_MIB_TXPAUSE,		"tx_pause"),
	MIB32(PPE_MIB_TXMULTI,		"tx_multicast"),
	MIB32(PPE_MIB_TXUNDERRUN,	"tx_underrun"),
	MIB32(PPE_MIB_TXPKT64,		"tx_64byte"),
	MIB32(PPE_MIB_TXPKT65TO127,	"tx_65_127byte"),
	MIB32(PPE_MIB_TXPKT128TO255,	"tx_128_255byte"),
	MIB32(PPE_MIB_TXPKT256TO511,	"tx_256_511byte"),
	MIB32(PPE_MIB_TXPKT512TO1023,	"tx_512_1023byte"),
	MIB32(PPE_MIB_TXPKT1024TO1518,	"tx_1024_1518byte"),
	MIB32(PPE_MIB_TXPKT1519TOX,	"tx_1519_maxbyte"),
	MIB64(PPE_MIB_TXBYTE_L,		"tx_bytes"),
	MIB32(PPE_MIB_TXCOLLISIONS,	"tx_collisions"),
	MIB32(PPE_MIB_TXABORTCOL,	"tx_abort_collision"),
	MIB32(PPE_MIB_TXMULTICOL,	"tx_multi_collision"),
	MIB32(PPE_MIB_TXSINGLECOL,	"tx_single_collision"),
	MIB32(PPE_MIB_TXEXCESSIVEDEFER,	"tx_excessive_defer"),
	MIB32(PPE_MIB_TXDEFER,		"tx_defer"),
	MIB32(PPE_MIB_TXLATECOL,	"tx_late_collision"),
	MIB32(PPE_MIB_TXUNI,		"tx_unicast"),
};

static void qca_ppe_get_strings(struct dsa_switch *ds, int port,
				    u32 stringset, uint8_t *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(qca_ppe_mib); i++)
		ethtool_puts(&data, qca_ppe_mib[i].name);
}

static int qca_ppe_get_sset_count(struct dsa_switch *ds, int port,
				      int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(qca_ppe_mib);
}

static void qca_ppe_get_ethtool_stats(struct dsa_switch *ds, int port,
					  uint64_t *data)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	int gmac = port - 1;
	int i;

	if (port < 1 || port >= ds->num_ports) {
		memset(data, 0, sizeof(u64) * ARRAY_SIZE(qca_ppe_mib));
		return;
	}

	for (i = 0; i < ARRAY_SIZE(qca_ppe_mib); i++) {
		const struct qca_ppe_mib_desc *mib = &qca_ppe_mib[i];
		u64 val;

		val = ppe_r32(&priv->ppe, PPE_GMAC_MIB(gmac, mib->offset));
		if (mib->size == 2)
			val |= (u64)ppe_r32(&priv->ppe,
					     PPE_GMAC_MIB(gmac,
							  mib->offset + 4)) << 32;
		data[i] = val;
	}
}

static void qca_ppe_port_stp_state_set(struct dsa_switch *ds, int port,
					   u8 state)
{
	struct qca_ppe_priv *priv = ds_to_priv(ds);
	u32 stp_state;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = PPE_STP_DISABLED;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		stp_state = PPE_STP_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		stp_state = PPE_STP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = PPE_STP_FORWARDING;
		break;
	}

	ppe_m32(&priv->ppe, PPE_CST_STATE(port), PPE_STP_STATE_MASK, stp_state);
}

static const struct dsa_switch_ops qca_ppe_ops = {
	.get_tag_protocol	= qca_ppe_get_tag_protocol,
	.setup			= qca_ppe_setup,
	.set_ageing_time	= qca_ppe_set_ageing_time,
	.port_enable		= qca_ppe_port_enable,
	.port_disable		= qca_ppe_port_disable,
	.port_stp_state_set	= qca_ppe_port_stp_state_set,
	.port_bridge_join	= qca_ppe_port_bridge_join,
	.port_bridge_leave	= qca_ppe_port_bridge_leave,
	.port_fdb_add		= qca_ppe_port_fdb_add,
	.port_fdb_del		= qca_ppe_port_fdb_del,
	.port_fdb_dump		= qca_ppe_port_fdb_dump,
	.port_mdb_add		= qca_ppe_port_mdb_add,
	.port_mdb_del		= qca_ppe_port_mdb_del,
	.phylink_get_caps	= qca_ppe_phylink_get_caps,
	.port_vlan_filtering	= qca_ppe_port_vlan_filtering,
	.port_vlan_add		= qca_ppe_port_vlan_add,
	.port_vlan_del		= qca_ppe_port_vlan_del,
	.get_strings		= qca_ppe_get_strings,
	.get_sset_count		= qca_ppe_get_sset_count,
	.get_ethtool_stats	= qca_ppe_get_ethtool_stats,
};

static void ppe_vsi_init(struct qca_ppe_priv *priv)
{
	int i;

	/* All three words must be written back for the HW to latch the entry */
	for (i = 1; i < priv->ppe.data->num_ports; i++) {
		u32 val[3];

		val[0] = ppe_r32(&priv->ppe, PPE_L3_VP_PORT_TBL(i));
		val[1] = ppe_r32(&priv->ppe, PPE_L3_VP_PORT_TBL(i) + 4);
		val[2] = ppe_r32(&priv->ppe, PPE_L3_VP_PORT_TBL(i) + 8);

		val[1] &= ~(PPE_L3_VP_VSI_VALID | PPE_L3_VP_VSI);
		val[1] |= PPE_L3_VP_VSI_VALID;

		ppe_w32(&priv->ppe, PPE_L3_VP_PORT_TBL(i), val[0]);
		ppe_w32(&priv->ppe, PPE_L3_VP_PORT_TBL(i) + 4, val[1]);
		ppe_w32(&priv->ppe, PPE_L3_VP_PORT_TBL(i) + 8, val[2]);
	}
}

static void ppe_mac_hw_init(struct qca_ppe_priv *priv)
{
	const struct ppe_data *d = priv->ppe.data;
	int lpbk_gmac = d->loopback_port - 1;
	int gmac;

	for (gmac = 0; gmac < d->num_gmacs; gmac++) {
		ppe_m32(&priv->ppe, PPE_GMAC_CTRL2(gmac),
			PPE_GMAC_CTRL2_MAXFR | PPE_GMAC_CTRL2_CRS_SEL |
			PPE_GMAC_CTRL2_TX_THD,
			FIELD_PREP(PPE_GMAC_CTRL2_MAXFR, PPE_MAX_FRAME_SIZE) |
			FIELD_PREP(PPE_GMAC_CTRL2_TX_THD, 1));

		ppe_m32(&priv->ppe, PPE_GMAC_DBG_CTRL(gmac),
			PPE_GMAC_DBG_CTRL_HIHG_IPG,
			FIELD_PREP(PPE_GMAC_DBG_CTRL_HIHG_IPG, 0xc));

		ppe_w32(&priv->ppe, PPE_GMAC_JUMBO_SIZE(gmac), PPE_MAX_FRAME_SIZE);
	}

	ppe_m32(&priv->ppe, PPE_LPBK_PPS_CTRL(lpbk_gmac),
		PPE_LPBK_PPS_THRESHOLD,
		FIELD_PREP(PPE_LPBK_PPS_THRESHOLD, 21));
	ppe_w32(&priv->ppe, PPE_LPBK_ENABLE(lpbk_gmac),
		PPE_LPBK_EN | PPE_LPBK_CRC_STRIP_EN);
	msleep(100);
	ppe_port_bridge_txmac_set(priv, d->loopback_port, true);
}

static void ppe_ctrlpkt_init(struct qca_ppe_priv *priv)
{
	/* RFDB_TBL[31]: STP multicast MAC 01:80:c2:00:00:00 */
	ppe_w32(&priv->ppe, PPE_RFDB_TBL(31), 0xc2000000);
	ppe_w32(&priv->ppe, PPE_RFDB_TBL(31) + 4, 0x00010180);

	/* APP_CTRL[0]: match RFDB profile 31, bypass STP, redirect to CPU */
	ppe_w32(&priv->ppe, PPE_APP_CTRL(0), 0x00000003);
	ppe_w32(&priv->ppe, PPE_APP_CTRL(0) + 4, 0x00000002);
	ppe_w32(&priv->ppe, PPE_APP_CTRL(0) + 8, 0x000093fc);
}

static void ppe_pcs_teardown(struct qca_ppe_priv *priv)
{
	int i;

	for (i = 1; i < priv->ppe.data->num_ports; i++) {
		if (!priv->port_pcs[i])
			continue;

		qca_uniphy_pcs_put(priv->port_pcs[i]);
		priv->port_pcs[i] = NULL;
	}
}

static void ppe_pcs_mux_cppe(struct qca_ppe_priv *priv,
			     struct device_node *psgmii_uniphy,
			     struct device_node *port5_uniphy,
			     phy_interface_t port5_mode, int port3_ch)
{
	u32 mux = 0;

	if (port3_ch == 4) {
		mux |= FIELD_PREP(CPPE_PORT3_PCS_SEL, CPPE_PORT3_PCS0_CH4);
		mux |= CPPE_PCS0_CH4_SEL;
	}

	if (port5_uniphy && psgmii_uniphy &&
	    port5_uniphy != psgmii_uniphy) {
		mux |= FIELD_PREP(CPPE_PORT5_PCS_SEL, CPPE_PORT5_PCS1_CH0);
		if (port5_mode == PHY_INTERFACE_MODE_USXGMII)
			mux |= CPPE_PORT5_GMAC_SEL;
	}

	ppe_w32(&priv->ppe, PPE_PORT_MUX_CTRL, mux);
}

static void ppe_pcs_mux_hppe(struct qca_ppe_priv *priv,
			     struct device_node *psgmii_uniphy,
			     struct device_node *port5_uniphy,
			     struct device_node *port6_uniphy,
			     phy_interface_t port5_mode,
			     phy_interface_t port6_mode)
{
	u32 mux = 0;

	if (psgmii_uniphy)
		mux |= FIELD_PREP(HPPE_PORT4_PCS_SEL, HPPE_PORT4_PCS0);

	if (port5_uniphy) {
		if (psgmii_uniphy && port5_uniphy == psgmii_uniphy)
			mux |= FIELD_PREP(HPPE_PORT5_PCS_SEL, HPPE_PORT5_PCS0);
		else
			mux |= FIELD_PREP(HPPE_PORT5_PCS_SEL, HPPE_PORT5_PCS1);

		if (port5_mode != PHY_INTERFACE_MODE_USXGMII)
			mux |= FIELD_PREP(HPPE_PORT5_GMAC_SEL,
					  HPPE_PORT5_GMAC_SEL_GMAC);
	}

	if (port6_uniphy) {
		mux |= FIELD_PREP(HPPE_PORT6_PCS_SEL, HPPE_PORT6_PCS2);
		if (port6_mode != PHY_INTERFACE_MODE_USXGMII)
			mux |= FIELD_PREP(HPPE_PORT6_GMAC_SEL,
					  HPPE_PORT6_GMAC_SEL_GMAC);
	}

	ppe_w32(&priv->ppe, PPE_PORT_MUX_CTRL, mux);
}

static int ppe_pcs_setup(struct qca_ppe_priv *priv)
{
	const struct ppe_data *d = priv->ppe.data;
	struct device_node *ports_np, *port_np;
	struct device_node *psgmii_uniphy = NULL;
	struct device_node *port5_uniphy = NULL;
	struct device_node *port6_uniphy = NULL;
	phy_interface_t port5_mode = PHY_INTERFACE_MODE_NA;
	phy_interface_t port6_mode = PHY_INTERFACE_MODE_NA;
	struct of_phandle_args pcs_args;
	struct phylink_pcs *pcs;
	int port3_ch = -1;
	u32 port;
	int ret;

	ports_np = of_get_child_by_name(priv->ds.dev->of_node, "ports");
	if (!ports_np)
		return -ENODEV;

	for_each_available_child_of_node(ports_np, port_np) {
		ret = of_property_read_u32(port_np, "reg", &port);
		if (ret || port == 0 || port >= d->num_ports)
			continue;

		ret = of_parse_phandle_with_args(port_np, "pcs-handle",
						 "#pcs-cells", 0, &pcs_args);
		if (ret)
			continue;

		switch (port) {
		case 1 ... 4:
			if (!psgmii_uniphy)
				psgmii_uniphy = pcs_args.np;
			if (port == 3)
				port3_ch = pcs_args.args[0];
			break;
		case 5:
			port5_uniphy = pcs_args.np;
			of_get_phy_mode(port_np, &port5_mode);
			break;
		case 6:
			port6_uniphy = pcs_args.np;
			of_get_phy_mode(port_np, &port6_mode);
			break;
		}

		pcs = qca_uniphy_pcs_get(priv->ds.dev, pcs_args.np,
					   pcs_args.args[0]);
		of_node_put(pcs_args.np);
		if (IS_ERR(pcs)) {
			of_node_put(port_np);
			of_node_put(ports_np);
			ppe_pcs_teardown(priv);
			return dev_err_probe(priv->ds.dev, PTR_ERR(pcs),
					     "failed to get PCS for port %d\n",
					     port);
		}

		priv->port_pcs[port] = pcs;
	}

	of_node_put(ports_np);

	if (d->type == PPE_TYPE_IPQ8074)
		ppe_pcs_mux_hppe(priv, psgmii_uniphy, port5_uniphy,
				 port6_uniphy, port5_mode, port6_mode);
	else
		ppe_pcs_mux_cppe(priv, psgmii_uniphy, port5_uniphy,
				 port5_mode, port3_ch);

	return 0;
}

static int qca_ppe_probe(struct platform_device *pdev)
{
	const struct ppe_data *data;
	struct device_node *ports;
	struct qca_ppe_priv *priv;
	struct reset_control *rst;
	struct dsa_switch *ds;
	int ret, i;

	data = of_device_get_match_data(&pdev->dev);
	if (!data)
		return -ENODEV;

	ports = of_get_child_by_name(pdev->dev.of_node, "ports");
	if (!ports)
		return -ENODEV;
	of_node_put(ports);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ppe.data = data;

	priv->num_clks = devm_clk_bulk_get_all(&pdev->dev, &priv->clks);
	if (priv->num_clks < 0)
		return priv->num_clks;

	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret)
		return ret;

	priv->ppe.base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->ppe.base)) {
		ret = PTR_ERR(priv->ppe.base);
		goto err_clk;
	}

	rst = devm_reset_control_get(&pdev->dev, "ppe_rst");
	if (IS_ERR(rst)) {
		ret = PTR_ERR(rst);
		goto err_clk;
	}
	reset_control_assert(rst);
	msleep(100);
	reset_control_deassert(rst);
	msleep(100);

	spin_lock_init(&priv->fdb_lock);

	ds = &priv->ds;
	ds->dev = &pdev->dev;
	ds->num_ports = data->num_ports;
	ds->ops = &qca_ppe_ops;
	ds->priv = priv;
	ds->phylink_mac_ops = &qca_ppe_phylink_mac_ops;

	for (i = 1; i < data->num_ports; i++) {
		char name[32];

		snprintf(name, sizeof(name), "port%d_rx", i);
		priv->port_rx_clk[i] = devm_clk_get_optional(&pdev->dev, name);
		if (IS_ERR(priv->port_rx_clk[i])) {
			ret = PTR_ERR(priv->port_rx_clk[i]);
			goto err_clk;
		}

		snprintf(name, sizeof(name), "port%d_tx", i);
		priv->port_tx_clk[i] = devm_clk_get_optional(&pdev->dev, name);
		if (IS_ERR(priv->port_tx_clk[i])) {
			ret = PTR_ERR(priv->port_tx_clk[i]);
			goto err_clk;
		}

		snprintf(name, sizeof(name), "nss_port%d_rst", i);
		priv->port_rst[i] = devm_reset_control_get_optional_exclusive(
						&pdev->dev, name);
		if (IS_ERR(priv->port_rst[i])) {
			ret = PTR_ERR(priv->port_rst[i]);
			goto err_clk;
		}
	}

	ppe_vsi_init(priv);

	ppe_scheduler_init(&priv->ppe);

	ppe_mac_hw_init(priv);
	ppe_ctrlpkt_init(priv);

	ret = ppe_pcs_setup(priv);
	if (ret)
		goto err_clk;

	ret = dsa_register_switch(ds);
	if (ret)
		goto err_pcs;

	platform_set_drvdata(pdev, priv);

	return 0;

err_pcs:
	ppe_pcs_teardown(priv);
err_clk:
	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
	return ret;
}

static void qca_ppe_remove(struct platform_device *pdev)
{
	struct qca_ppe_priv *priv = platform_get_drvdata(pdev);

	dsa_unregister_switch(&priv->ds);
	ppe_pcs_teardown(priv);
	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
}

static const struct ppe_data ipq6018_ppe_data = {
	.type			= PPE_TYPE_IPQ6018,
	.num_ports		= 7,
	.num_gmacs		= 5,
	.loopback_port		= 6,
	.bm_phy_end		= 12,
	.bm_internal_start	= 13,
	.bm_group_buf		= 1024,
	.bm_ceiling		= 216,
	.qm_total_buf		= 1506,
	.qm_ceiling		= 216,
	.qm_green_max		= 144,
};

static const struct ppe_data ipq8074_ppe_data = {
	.type			= PPE_TYPE_IPQ8074,
	.num_ports		= 8,
	.num_gmacs		= 6,
	.loopback_port		= 7,
	.bm_phy_end		= 13,
	.bm_internal_start	= 14,
	.bm_group_buf		= 1400,
	.bm_ceiling		= 250,
	.qm_total_buf		= 2000,
	.qm_ceiling		= 400,
	.qm_green_max		= 250,
};

static const struct of_device_id qca_ppe_of_match[] = {
	{ .compatible = "qualcomm,ipq6018-ppe", .data = &ipq6018_ppe_data },
	{ .compatible = "qualcomm,ipq8074-ppe", .data = &ipq8074_ppe_data },
	{},
};
MODULE_DEVICE_TABLE(of, qca_ppe_of_match);

static struct platform_driver qca_ppe_driver = {
	.driver = {
		.name = "qca-ppe",
		.of_match_table = qca_ppe_of_match,
	},
	.probe = qca_ppe_probe,
	.remove = qca_ppe_remove,
};
module_platform_driver(qca_ppe_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for Qualcomm PPE switches");
