/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2014-2015, 2020 The Linux Foundation. All rights reserved.
 * Copyright 2015, 2021 Linaro Limited.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/pci.h>
#include "../../pci.h"
#include "pcie-designware.h"

#define GEN3_EQ_FMDC_MAX_PRE_CUSROR_DELTA_16GT_VAL   0x5
#define GEN3_EQ_FMDC_MAX_POST_CUSROR_DELTA_16GT_VAL  0x5
#define GEN3_EQ_FMDC_N_EVALS_16GT_VAL          0xD

int qcom_pcie_cmn_icc_get_resource(struct dw_pcie *pci, struct icc_path *icc_mem);
int qcom_pcie_cmn_icc_init(struct dw_pcie *pci, struct icc_path *icc_mem);
void qcom_pcie_cmn_icc_update(struct dw_pcie *pci, struct icc_path *icc_mem);
void qcom_pcie_cmn_set_16gt_eq_settings(struct dw_pcie *pci);
