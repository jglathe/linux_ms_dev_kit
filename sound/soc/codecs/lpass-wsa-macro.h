/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LPASS_WSA_MACRO_H__
#define __LPASS_WSA_MACRO_H__

#include <linux/kconfig.h>

struct snd_soc_component;

/*
 * Selects compander and smart boost settings
 * for a given speaker mode
 */
enum {
	WSA_MACRO_SPKR_MODE_DEFAULT,
	WSA_MACRO_SPKR_MODE_1, /* COMP Gain = 12dB, Smartboost Max = 5.5V */
};

int wsa_macro_set_spkr_mode(struct snd_soc_component *component, int mode);

#if IS_REACHABLE(CONFIG_SND_SOC_LPASS_WSA_MACRO)
bool wsa_macro_protection_pa_event(struct snd_soc_component *source,
				   bool enable);
#else
static inline bool
wsa_macro_protection_pa_event(struct snd_soc_component *source, bool enable)
{
	return false;
}
#endif

#endif /* __LPASS_WSA_MACRO_H__ */
