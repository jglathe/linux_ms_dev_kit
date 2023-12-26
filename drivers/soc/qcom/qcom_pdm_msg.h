// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2018, Linaro Ltd.
 * Copyright (c) 2016, Bjorn Andersson
 */

#ifndef __QMI_SERVREG_LOC_H__
#define __QMI_SERVREG_LOC_H__

#include <linux/types.h>
#include <linux/soc/qcom/qmi.h>

#define SERVREG_QMI_SERVICE 64
#define SERVREG_QMI_VERSION 257
#define SERVREG_QMI_INSTANCE 0
#define QMI_RESULT_SUCCESS 0
#define QMI_RESULT_FAILURE 1
#define QMI_ERR_NONE 0
#define QMI_ERR_INTERNAL 1
#define QMI_ERR_MALFORMED_MSG 2
#define SERVREG_LOC_GET_DOMAIN_LIST 33
#define SERVREG_LOC_PFR 36

struct servreg_loc_domain_list_entry {
	char name[65];
	u32 instance_id;
	u8 service_data_valid;
	u32 service_data;
};

struct servreg_loc_get_domain_list_req {
	char name[65];
	u8 offset_valid;
	u32 offset;
};

#define SERVREG_LOC_MAX_DOMAINS 32

struct servreg_loc_get_domain_list_resp {
	struct qmi_response_type_v01 rsp;
	u8 total_domains_valid;
	u16 total_domains;
	u8 db_revision_valid;
	u16 db_revision;
	u8 domain_list_valid;
	u32 domain_list_len;
	struct servreg_loc_domain_list_entry domain_list[SERVREG_LOC_MAX_DOMAINS];
};

struct servreg_loc_pfr_req {
	char service[65];
	char reason[257];
};

struct servreg_loc_pfr_resp {
	struct qmi_response_type_v01 rsp;
};

#define SERVREG_LOC_PFR_RESP_MSG_SIZE 10

extern struct qmi_elem_info servreg_loc_get_domain_list_req_ei[];
extern struct qmi_elem_info servreg_loc_get_domain_list_resp_ei[];
extern struct qmi_elem_info servreg_loc_pfr_req_ei[];
extern struct qmi_elem_info servreg_loc_pfr_resp_ei[];

#endif
