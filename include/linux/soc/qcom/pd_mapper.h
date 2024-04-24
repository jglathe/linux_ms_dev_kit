/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Qualcomm Protection Domain mapper
 *
 * Copyright (c) 2023 Linaro Ltd.
 */
#ifndef __QCOM_PD_MAPPER__
#define __QCOM_PD_MAPPER__

#if IS_ENABLED(CONFIG_QCOM_PD_MAPPER)

int qcom_pdm_get(void);
void qcom_pdm_release(void);

#else

static inline int qcom_pdm_get(void)
{
	return 0;
}

static inline void qcom_pdm_release(void)
{
}

#endif

#endif
