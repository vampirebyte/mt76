/*
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt76x2.h"

struct beacon_bc_data {
	struct mt76x2_dev *dev;
	struct sk_buff_head q;
	struct sk_buff *tail[8];
};

void mt76x2_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	     struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct mt76x2_dev *dev = hw->priv;
	struct ieee80211_vif *vif = info->control.vif;
	struct mt76x2_vif *mvif = (struct mt76x2_vif *) vif->drv_priv;
	struct mt76x2_sta *msta = NULL;
	struct mt76_wcid *wcid = &mvif->group_wcid;
	struct mt76_queue *q;
	int qid = skb_get_queue_mapping(skb);

	if (WARN_ON(qid >= MT_TXQ_PSD)) {
		qid = MT_TXQ_BE;
		skb_set_queue_mapping(skb, qid);
	}

	if (control->sta) {
		msta = (struct mt76x2_sta *) control->sta->drv_priv;
		wcid = &msta->wcid;
	}

	if (!wcid->tx_rate_set)
		ieee80211_get_tx_rates(info->control.vif, control->sta, skb,
				       info->control.rates, 1);

	q = &dev->mt76.q_tx[qid];

	spin_lock_bh(&q->lock);
	mt76_tx_queue_skb(&dev->mt76, q, skb, wcid, control->sta);
	mt76_queue_kick(dev, q);

	if (q->queued > q->ndesc - 8)
		ieee80211_stop_queue(hw, skb_get_queue_mapping(skb));
	spin_unlock_bh(&q->lock);
}

void mt76x2_tx_complete(struct mt76x2_dev *dev, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct mt76_queue *q;
	int qid = skb_get_queue_mapping(skb);

	if (info->flags & IEEE80211_TX_CTL_AMPDU) {
		ieee80211_free_txskb(mt76_hw(dev), skb);
	} else {
		ieee80211_tx_info_clear_status(info);
		info->status.rates[0].idx = -1;
		info->flags |= IEEE80211_TX_STAT_ACK;
		ieee80211_tx_status(mt76_hw(dev), skb);
	}

	q = &dev->mt76.q_tx[qid];
	if (q->queued < q->ndesc - 8)
		ieee80211_wake_queue(mt76_hw(dev), qid);
}

static void
mt76x2_update_beacon_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct mt76x2_dev *dev = (struct mt76x2_dev *) priv;
	struct mt76x2_vif *mvif = (struct mt76x2_vif *) vif->drv_priv;
	struct ieee80211_tx_info *info;
	struct sk_buff *skb = NULL;

	if (!(dev->beacon_mask & BIT(mvif->idx)))
		return;

	skb = ieee80211_beacon_get(mt76_hw(dev), vif);
	if (!skb)
		return;

	info = IEEE80211_SKB_CB(skb);
	info->flags |= IEEE80211_TX_CTL_ASSIGN_SEQ;
	mt76x2_mac_set_beacon(dev, mvif->idx, skb);
}

static void
mt76x2_add_buffered_bc(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	struct beacon_bc_data *data = priv;
	struct mt76x2_dev *dev = data->dev;
	struct mt76x2_vif *mvif = (struct mt76x2_vif *) vif->drv_priv;
	struct ieee80211_tx_info *info;
	struct sk_buff *skb;

	if (!(dev->beacon_mask & BIT(mvif->idx)))
		return;

	skb = ieee80211_get_buffered_bc(mt76_hw(dev), vif);
	if (!skb)
		return;

	info = IEEE80211_SKB_CB(skb);
	info->control.vif = vif;
	info->flags |= IEEE80211_TX_CTL_ASSIGN_SEQ;
	mt76_skb_set_moredata(skb, true);
	__skb_queue_tail(&data->q, skb);
	data->tail[mvif->idx] = skb;
}

void mt76x2_pre_tbtt_tasklet(unsigned long arg)
{
	struct mt76x2_dev *dev = (struct mt76x2_dev *) arg;
	struct mt76_queue *q = &dev->mt76.q_tx[MT_TXQ_PSD];
	struct beacon_bc_data data = {};
	struct sk_buff *skb;
	int i, nframes;

	data.dev = dev;
	__skb_queue_head_init(&data.q);

	ieee80211_iterate_active_interfaces_atomic(mt76_hw(dev),
		IEEE80211_IFACE_ITER_RESUME_ALL,
		mt76x2_update_beacon_iter, dev);

	do {
		nframes = skb_queue_len(&data.q);
		ieee80211_iterate_active_interfaces_atomic(mt76_hw(dev),
			IEEE80211_IFACE_ITER_RESUME_ALL,
			mt76x2_add_buffered_bc, &data);
	} while (nframes != skb_queue_len(&data.q));

	if (!nframes)
		return;

	for (i = 0; i < ARRAY_SIZE(data.tail); i++) {
		if (!data.tail[i])
			continue;

		mt76_skb_set_moredata(data.tail[i], false);
	}

	spin_lock_bh(&q->lock);
	while ((skb = __skb_dequeue(&data.q)) != NULL) {
		struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
		struct ieee80211_vif *vif = info->control.vif;
		struct mt76x2_vif *mvif = (struct mt76x2_vif *) vif->drv_priv;

		mt76_tx_queue_skb(&dev->mt76, q, skb, &mvif->group_wcid, NULL);
	}
	spin_unlock_bh(&q->lock);
}

void mt76x2_txq_init(struct mt76x2_dev *dev, struct ieee80211_txq *txq)
{
	struct mt76_txq *mtxq;

	if (!txq)
		return;

	mtxq = (struct mt76_txq *) txq->drv_priv;
	if (txq->sta) {
		struct mt76x2_sta *sta = (struct mt76x2_sta *) txq->sta->drv_priv;
		mtxq->wcid = &sta->wcid;
	} else {
		struct mt76x2_vif *mvif = (struct mt76x2_vif *) txq->vif->drv_priv;
		mtxq->wcid = &mvif->group_wcid;
	}

	mt76_txq_init(&dev->mt76, txq);
}
