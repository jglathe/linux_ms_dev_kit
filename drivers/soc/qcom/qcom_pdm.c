// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Protection Domain mapper
 *
 * Copyright (c) 2023 Linaro Ltd.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/soc/qcom/pd_mapper.h>
#include <linux/soc/qcom/qmi.h>

#include "qcom_pdm_msg.h"

#define TMS_SERVREG_SERVICE "tms/servreg"

struct qcom_pdm_domain {
	struct list_head list;
	const char *name;
	u32 instance_id;
};

struct qcom_pdm_service {
	struct list_head list;
	struct list_head domains;
	const char *name;
};

static LIST_HEAD(qcom_pdm_services);
static DEFINE_MUTEX(qcom_pdm_mutex);

static struct qcom_pdm_service *qcom_pdm_find_locked(const char *name)
{
	struct qcom_pdm_service *service;

	list_for_each_entry(service, &qcom_pdm_services, list) {
		if (!strcmp(service->name, name))
			return service;
	}

	return NULL;
}

static void qcom_pdm_del_service_domain_locked(const char *service_name, const char *domain_name)
{
	struct qcom_pdm_service *service;
	struct qcom_pdm_domain *domain, *temp;

	service = qcom_pdm_find_locked(service_name);
	if (WARN_ON(!service))
		return;

	list_for_each_entry_safe(domain, temp, &service->domains, list) {
		if (!strcmp(domain->name, domain_name)) {
			list_del(&domain->list);
			kfree(domain);

			if (list_empty(&service->domains)) {
				list_del(&service->list);
				kfree(service->name);
				kfree(service);
			}

			return;
		}
	}

	WARN(1, "domain not found");
}

static int qcom_pdm_add_service_domain_locked(const char *service_name,
					      const char *domain_name,
					      u32 instance_id)
{
	struct qcom_pdm_service *service;
	struct qcom_pdm_domain *domain;

	service = qcom_pdm_find_locked(service_name);
	if (service) {
		list_for_each_entry(domain, &service->domains, list) {
			if (!strcmp(domain->name, domain_name))
				return -EBUSY;
		}
	} else {
		service = kzalloc(sizeof(*service), GFP_KERNEL);
		if (!service)
			return -ENOMEM;

		INIT_LIST_HEAD(&service->domains);
		service->name = kstrdup(service_name, GFP_KERNEL);

		list_add_tail(&service->list, &qcom_pdm_services);
	}

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain) {
		if (list_empty(&service->domains)) {
			list_del(&service->list);
			kfree(service->name);
			kfree(service);
		}

		return -ENOMEM;
	}

	/*
	 * service name can outlive calling module and so it should be strdup'ed.
	 * domain name can not outlive the module, so there is no need to strdup it.
	 */
	domain->name = domain_name;
	domain->instance_id = instance_id;
	list_add(&domain->list, &service->domains);

	return 0;
}

int qcom_pdm_add_domain(const struct qcom_pdm_domain_data *data)
{
	int ret;
	int i;

	mutex_lock(&qcom_pdm_mutex);

	ret = qcom_pdm_add_service_domain_locked(TMS_SERVREG_SERVICE,
						 data->domain,
						 data->instance_id);

	for (i = 0; data->services[i]; i++) {
		ret = qcom_pdm_add_service_domain_locked(data->services[i],
							 data->domain,
							 data->instance_id);
		if (ret)
			goto err;
	}

	mutex_unlock(&qcom_pdm_mutex);

	return 0;

err:
	while (--i >= 0)
		qcom_pdm_del_service_domain_locked(data->services[i], data->domain);

	qcom_pdm_del_service_domain_locked(TMS_SERVREG_SERVICE, data->domain);

	mutex_unlock(&qcom_pdm_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_pdm_add_domain);

void qcom_pdm_del_domain(const struct qcom_pdm_domain_data *data)
{
	int i;

	mutex_lock(&qcom_pdm_mutex);

	for (i = 0; data->services[i]; i++)
		qcom_pdm_del_service_domain_locked(data->services[i], data->domain);

	qcom_pdm_del_service_domain_locked(TMS_SERVREG_SERVICE, data->domain);

	mutex_unlock(&qcom_pdm_mutex);
}
EXPORT_SYMBOL_GPL(qcom_pdm_del_domain);

static void qcom_pdm_get_domain_list(struct qmi_handle *qmi,
				     struct sockaddr_qrtr *sq,
				     struct qmi_txn *txn,
				     const void *decoded)
{
	const struct servreg_loc_get_domain_list_req *req = decoded;
	struct servreg_loc_get_domain_list_resp *rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	struct qcom_pdm_service *service;
	u32 offset;
	int ret;

	pr_info("PDM: service '%s' offset %d\n", req->name, req->offset_valid ? req->offset : -1);

	offset = req->offset_valid ? req->offset : 0;

	rsp->rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp->rsp.error = QMI_ERR_NONE_V01;

	rsp->db_revision_valid = true;
	rsp->db_revision = 1;

	rsp->total_domains_valid = 1;
	rsp->total_domains = 0;

	mutex_lock(&qcom_pdm_mutex);

	service = qcom_pdm_find_locked(req->name);
	if (service) {
		struct qcom_pdm_domain *domain;

		rsp->domain_list_valid = 1;
		rsp->domain_list_len = 0;

		list_for_each_entry(domain, &service->domains, list) {
			u32 i = rsp->total_domains++;

			if (i >= offset && i < SERVREG_LOC_MAX_DOMAINS) {
				u32 j = rsp->domain_list_len++;

				strlcpy(rsp->domain_list[j].name, domain->name,
					sizeof(rsp->domain_list[i].name));
				rsp->domain_list[j].instance_id = domain->instance_id;

				pr_info("PDM: returning %s / %d\n", domain->name,
					domain->instance_id);
			}
		}

	}

	mutex_unlock(&qcom_pdm_mutex);

	pr_info("PDM: service '%s' %d domains (of %d)\n", req->name, rsp->domain_list_len, rsp->total_domains);

	ret = qmi_send_response(qmi, sq, txn, SERVREG_LOC_GET_DOMAIN_LIST,
				2658,
				servreg_loc_get_domain_list_resp_ei, rsp);
	if (ret)
		pr_err("Error sending servreg response: %d\n", ret);

	kfree(rsp);
}

static void qcom_pdm_pfr(struct qmi_handle *qmi,
			 struct sockaddr_qrtr *sq,
			 struct qmi_txn *txn,
			 const void *decoded)
{
	const struct servreg_loc_pfr_req *req = decoded;
	struct servreg_loc_pfr_resp rsp = {};
	int ret;

	pr_warn_ratelimited("PDM: service '%s' crash: '%s'\n", req->service, req->reason);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, SERVREG_LOC_PFR,
				SERVREG_LOC_PFR_RESP_MSG_SIZE,
				servreg_loc_pfr_resp_ei, &rsp);
	if (ret)
		pr_err("Error sending servreg response: %d\n", ret);
}

static const struct qmi_msg_handler qcom_pdm_msg_handlers[] = {
	{
		.type = QMI_REQUEST,
		.msg_id = SERVREG_LOC_GET_DOMAIN_LIST,
		.ei = servreg_loc_get_domain_list_req_ei,
		.decoded_size = sizeof(struct servreg_loc_get_domain_list_req),
		.fn = qcom_pdm_get_domain_list,
	},
	{
		.type = QMI_REQUEST,
		.msg_id = SERVREG_LOC_PFR,
		.ei = servreg_loc_pfr_req_ei,
		.decoded_size = sizeof(struct servreg_loc_pfr_req),
		.fn = qcom_pdm_pfr,
	},
	{ },
};

static struct qmi_handle qcom_pdm_handle;

static int qcom_pdm_init(void)
{
	int ret;

	ret = qmi_handle_init(&qcom_pdm_handle, 1024,
			      NULL, qcom_pdm_msg_handlers);
	if (ret)
		return ret;

	ret = qmi_add_server(&qcom_pdm_handle, SERVREG_QMI_SERVICE,
			     SERVREG_QMI_VERSION, SERVREG_QMI_INSTANCE);
	if (ret) {
		qmi_handle_release(&qcom_pdm_handle);
		memset(&qcom_pdm_handle, 0, sizeof(qcom_pdm_handle));
	}

	return ret;
}

static void qcom_pdm_exit(void)
{
	qmi_handle_release(&qcom_pdm_handle);
	memset(&qcom_pdm_handle, 0, sizeof(qcom_pdm_handle));

	WARN_ON(!list_empty(&qcom_pdm_services));
}

module_init(qcom_pdm_init);
module_exit(qcom_pdm_exit);

MODULE_DESCRIPTION("Qualcomm Protection Domain Mapper");
MODULE_LICENSE("GPL");
