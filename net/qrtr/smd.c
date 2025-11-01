// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Sony Mobile Communications Inc.
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/rpmsg.h>

#include "qrtr.h"

struct qrtr_smd_dev {
	struct qrtr_endpoint ep;
	struct rpmsg_endpoint *channel;
	struct device *dev;

	/* Protect against channel open/close while sending */
	struct mutex send_lock;
};

/* from smd to qrtr */
static int qcom_smd_qrtr_callback(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv, u32 addr)
{
	struct qrtr_smd_dev *qdev = priv;
	int rc;

	rc = qrtr_endpoint_post(&qdev->ep, data, len);
	if (rc == -EINVAL) {
		dev_err(qdev->dev, "invalid ipcrouter packet\n");
		/* return 0 to let smd drop the packet */
		rc = 0;
	}

	return rc;
}

/* from qrtr to smd */
static int qcom_smd_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb)
{
	struct qrtr_smd_dev *qdev = container_of(ep, struct qrtr_smd_dev, ep);
	int rc;

	rc = skb_linearize(skb);
	if (rc)
		goto out;

	scoped_guard(mutex, &qdev->send_lock) {
		if (qdev->channel)
			rc = rpmsg_send(qdev->channel, skb->data, skb->len);
		else
			rc = -ENODEV;
	}

out:
	if (rc)
		kfree_skb(skb);
	else
		consume_skb(skb);
	return rc;
}

static int qcom_smd_qrtr_probe(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qdev;
	int rc;

	qdev = devm_kzalloc(&rpdev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	qdev->dev = &rpdev->dev;
	qdev->ep.xmit = qcom_smd_qrtr_send;

	rc = devm_mutex_init(&rpdev->dev, &qdev->send_lock);
	if (rc)
		return rc;

	/* Block sending until we have fully opened the channel */
	guard(mutex)(&qdev->send_lock);

	rc = qrtr_endpoint_register(&qdev->ep, QRTR_EP_NID_AUTO);
	if (rc)
		return rc;

	qdev->channel = rpmsg_dev_open_ept(rpdev, qcom_smd_qrtr_callback, qdev);
	if (!qdev->channel) {
		qrtr_endpoint_unregister(&qdev->ep);
		return -EREMOTEIO;
	}

	dev_set_drvdata(&rpdev->dev, qdev);

	dev_dbg(&rpdev->dev, "Qualcomm SMD QRTR driver probed\n");

	return 0;
}

static void qcom_smd_qrtr_remove(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qdev = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_endpoint *ept;

	/* We are about to close the channel, so stop all sending now */
	scoped_guard(mutex, &qdev->send_lock) {
		ept = qdev->channel;
		qdev->channel = NULL;
	}

	rpmsg_destroy_ept(ept);
	qrtr_endpoint_unregister(&qdev->ep);
}

static const struct rpmsg_device_id qcom_smd_qrtr_smd_match[] = {
	{ "IPCRTR" },
	{}
};

static struct rpmsg_driver qcom_smd_qrtr_driver = {
	.probe = qcom_smd_qrtr_probe,
	.remove = qcom_smd_qrtr_remove,
	.id_table = qcom_smd_qrtr_smd_match,
	.drv = {
		.name = "qcom_smd_qrtr",
	},
};

module_rpmsg_driver(qcom_smd_qrtr_driver);

MODULE_ALIAS("rpmsg:IPCRTR");
MODULE_DESCRIPTION("Qualcomm IPC-Router SMD interface driver");
MODULE_LICENSE("GPL v2");
