/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
#ifndef __QCOM_PM8008_H__
#define __QCOM_PM8008_H__

struct pm8008_data;
struct regmap *pm8008_get_regmap(const struct pm8008_data *chip);

#endif
