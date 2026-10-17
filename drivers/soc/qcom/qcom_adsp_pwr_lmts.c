// SPDX-License-Identifier: GPL-2.0
/*
 * Tell the ADSP when the system enters and leaves sleep, like Windows does.
 *
 * Windows' PEP (qcpep8380.sys) sends a MODERN_STANDBY_STATE message on the
 * APPS_ADSP_PWR_LMTS_GLINK_PORT channel when the platform enters Modern
 * Standby (state 1) and when it leaves it (state 0). On entry the ADSP stops
 * its power-limits timer, which otherwise wakes it ~100 times per second and
 * keeps DDR from staying in low-power mode during s2idle.
 *
 * The messages are sent from the system suspend/resume callbacks of the
 * rpmsg device. The glink edge is its parent, so it is still up here.
 */
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/rpmsg.h>

#define PWR_LMTS_MODERN_STANDBY_STATE	1

struct pwr_lmts_msg {
	__le32 size;
	__le32 type;
	__le32 count;
	__le32 state;
	__le32 reserved[2];
} __packed;

static int pwr_lmts_send_state(struct rpmsg_device *rpdev, u32 state)
{
	struct pwr_lmts_msg msg = {
		.size = cpu_to_le32(sizeof(msg)),
		.type = cpu_to_le32(PWR_LMTS_MODERN_STANDBY_STATE),
		.count = cpu_to_le32(1),
		.state = cpu_to_le32(state),
	};
	int ret;

	ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
	if (ret)
		dev_warn(&rpdev->dev, "failed to send standby state %u: %d\n", state, ret);

	return ret;
}

static int pwr_lmts_callback(struct rpmsg_device *rpdev, void *data, int len,
			     void *priv, u32 addr)
{
	return 0;
}

static int pwr_lmts_probe(struct rpmsg_device *rpdev)
{
	return 0;
}

static int pwr_lmts_suspend(struct device *dev)
{
	/* Do not block suspend if the ADSP does not take the message */
	pwr_lmts_send_state(container_of(dev, struct rpmsg_device, dev), 1);

	return 0;
}

static int pwr_lmts_resume(struct device *dev)
{
	pwr_lmts_send_state(container_of(dev, struct rpmsg_device, dev), 0);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pwr_lmts_pm_ops, pwr_lmts_suspend, pwr_lmts_resume);

static const struct rpmsg_device_id pwr_lmts_id_table[] = {
	{ .name = "APPS_ADSP_PWR_LMTS_GLINK_PORT" },
	{ }
};
MODULE_DEVICE_TABLE(rpmsg, pwr_lmts_id_table);

static struct rpmsg_driver pwr_lmts_driver = {
	.drv = {
		.name = "qcom_adsp_pwr_lmts",
		.pm = pm_sleep_ptr(&pwr_lmts_pm_ops),
	},
	.id_table = pwr_lmts_id_table,
	.probe = pwr_lmts_probe,
	.callback = pwr_lmts_callback,
};
module_rpmsg_driver(pwr_lmts_driver);

MODULE_DESCRIPTION("Qualcomm ADSP power-limits Modern Standby notification");
MODULE_LICENSE("GPL");
