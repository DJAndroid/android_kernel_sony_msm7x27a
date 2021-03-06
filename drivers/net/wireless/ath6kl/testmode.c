/*
 * Copyright (c) 2010-2011 Atheros Communications Inc.
 * Copyright (c) 2011 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "testmode.h"
#include "debug.h"
#include "cfg80211.h"

#include <net/netlink.h>
#include "wmiconfig.h"

enum ath6kl_tm_attr {
	__ATH6KL_TM_ATTR_INVALID	= 0,
	ATH6KL_TM_ATTR_CMD		= 1,
	ATH6KL_TM_ATTR_DATA		= 2,

	/* keep last */
	__ATH6KL_TM_ATTR_AFTER_LAST,
	ATH6KL_TM_ATTR_MAX		= __ATH6KL_TM_ATTR_AFTER_LAST - 1,
};

enum ath6kl_tm_cmd {
	ATH6KL_TM_CMD_TCMD		= 0,
	ATH6KL_TM_CMD_RX_REPORT		= 1,	/* not used anymore */
	ATH6KL_TM_CMD_WMI_CMD		= 0xF000,
	
	ATH6KL_TM_CMD_PKT_FILTER,
};

#define ATH6KL_TM_DATA_MAX_LEN		5000
#define PKT_FILTER_MAX_SIZE		64

static const struct nla_policy ath6kl_tm_policy[ATH6KL_TM_ATTR_MAX + 1] = {
	[ATH6KL_TM_ATTR_CMD]		= { .type = NLA_U32 },
	[ATH6KL_TM_ATTR_DATA]		= { .type = NLA_BINARY,
					    .len = ATH6KL_TM_DATA_MAX_LEN },
};


void ath6kl_tm_rx_event(struct ath6kl *ar, void *buf, size_t buf_len)
{
	struct sk_buff *skb;

	if (!buf || buf_len == 0)
		return;

	skb = cfg80211_testmode_alloc_event_skb(ar->wiphy, buf_len, GFP_KERNEL);
	if (!skb) {
		ath6kl_warn("failed to allocate testmode rx skb!\n");
		return;
	}
	NLA_PUT_U32(skb, ATH6KL_TM_ATTR_CMD, ATH6KL_TM_CMD_TCMD);
	NLA_PUT(skb, ATH6KL_TM_ATTR_DATA, buf_len, buf);
	cfg80211_testmode_event(skb, GFP_KERNEL);
	return;

nla_put_failure:
	kfree_skb(skb);
	ath6kl_warn("nla_put failed on testmode rx skb!\n");
}

static bool ath6kl_tm_ready(struct ath6kl_vif *vif)
{
	struct ath6kl *ar = vif->ar;

	if (!test_bit(WMI_READY, &ar->flag)) {
		ath6kl_err("wmi is not ready\n");
		return false;
	}

	if (!test_bit(WLAN_ENABLED, &vif->flags)) {
		ath6kl_err("wlan disabled\n");
		return false;
	}

	return true;
}

int ath6kl_tm_cmd(struct wiphy *wiphy, void *data, int len)
{
	struct ath6kl *ar = wiphy_priv(wiphy);
	struct nlattr *tb[ATH6KL_TM_ATTR_MAX + 1];
	struct ath6kl_vif *vif;
	int err, buf_len;
	void *buf;
	u8 *buf_ptr;
	u8 filter[PKT_FILTER_MAX_SIZE], mask[PKT_FILTER_MAX_SIZE];
	u8 size;
	u32 wmi_cmd;
	struct sk_buff *skb;

	vif = ath6kl_vif_first(ar);
	if (!vif)
		return -EIO;

	if (!ath6kl_cfg80211_ready(vif))
		return -EIO;

	err = nla_parse(tb, ATH6KL_TM_ATTR_MAX, data, len,
			ath6kl_tm_policy);
	if (err)
		return err;

	if (!tb[ATH6KL_TM_ATTR_CMD])
		return -EINVAL;

	switch (nla_get_u32(tb[ATH6KL_TM_ATTR_CMD])) {
	case ATH6KL_TM_CMD_WMI_CMD:
		if (!tb[ATH6KL_TM_ATTR_DATA])
			return -EINVAL;

		buf = nla_data(tb[ATH6KL_TM_ATTR_DATA]);
		buf_len = nla_len(tb[ATH6KL_TM_ATTR_DATA]);

		/* First four bytes hold the wmi_cmd and the rest is the data */
		skb = ath6kl_wmi_get_buf(buf_len-4);
		if (!skb)
			return -ENOMEM;

		memcpy(&wmi_cmd, buf, sizeof(wmi_cmd));
		memcpy(skb->data, (u32 *)buf + 1, buf_len - 4);
		ath6kl_wmi_cmd_send(ar->wmi, 0, skb, wmi_cmd, NO_SYNC_WMIFLAG);

		return 0;

		break;
	case ATH6KL_TM_CMD_TCMD:
		if (!tb[ATH6KL_TM_ATTR_DATA])
			return -EINVAL;

		buf = nla_data(tb[ATH6KL_TM_ATTR_DATA]);
		buf_len = nla_len(tb[ATH6KL_TM_ATTR_DATA]);

		ath6kl_wmi_test_cmd(ar->wmi, buf, buf_len);

		return 0;

		break;
	case ATH6KL_TM_CMD_PKT_FILTER:
		vif = ath6kl_vif_first(ar);
		if (!vif)
			return -EIO;

		if (!ath6kl_tm_ready(vif))
			return -EOPNOTSUPP;

		if (!tb[ATH6KL_TM_ATTR_DATA])
			return -EINVAL;

		buf = nla_data(tb[ATH6KL_TM_ATTR_DATA]);
		buf_len = nla_len(tb[ATH6KL_TM_ATTR_DATA]);
		buf_ptr = buf;

		if (buf_ptr[0]) {
			size = buf_ptr[3];
			if (size > PKT_FILTER_MAX_SIZE)
				return -EINVAL;

			memcpy(filter, &buf_ptr[5], size);
			memcpy(mask, &buf_ptr[5] + size, size);
			
			ath6kl_wmi_add_pkt_filter_pattern_cmd(
				ar->wmi, 0, buf_ptr[1], buf_ptr[2],
				buf_ptr[3], buf_ptr[4], filter, mask);
		} else {
			ath6kl_wmi_del_pkt_filter_pattern_cmd(
				ar->wmi, 0, buf_ptr[1]);
		}
		return 0;
	case ATH6KL_TM_CMD_RX_REPORT:
	default:
		return -EOPNOTSUPP;
	}
}
