/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __Q6APM_H__
#define __Q6APM_H__
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <sound/soc.h>
#include <linux/of_platform.h>
#include <linux/jiffies.h>
#include <linux/soc/qcom/apr.h>
#include "../common.h"
#include "audioreach.h"

#define APM_PORT_MAX		LPASS_MAX_PORT
#define APM_PORT_MAX_AUDIO_CHAN_CNT 8
#define PCM_CHANNEL_NULL 0
#define PCM_CHANNEL_FL    1	/* Front left channel. */
#define PCM_CHANNEL_FR    2	/* Front right channel. */
#define PCM_CHANNEL_FC    3	/* Front center channel. */
#define PCM_CHANNEL_LS   4	/* Left surround channel. */
#define PCM_CHANNEL_RS   5	/* Right surround channel. */
#define PCM_CHANNEL_LFE  6	/* Low frequency effect channel. */
#define PCM_CHANNEL_CS   7	/* Center surround channel; Rear center ch */
#define PCM_CHANNEL_LB   8	/* Left back channel; Rear left channel. */
#define PCM_CHANNEL_RB   9	/* Right back channel; Rear right channel. */
#define PCM_CHANNELS   10	/* Top surround channel. */

#define APM_TIMESTAMP_FLAG	0x80000000
#define FORMAT_LINEAR_PCM	0x0000
/* APM client callback events */
#define APM_CMD_EOS				0x0003
#define APM_CLIENT_EVENT_CMD_EOS_DONE		0x1003
#define APM_CMD_CLOSE				0x0004
#define APM_CLIENT_EVENT_CMD_CLOSE_DONE		0x1004
#define APM_CLIENT_EVENT_CMD_RUN_DONE		0x1008
#define APM_CLIENT_EVENT_DATA_WRITE_DONE	0x1009
#define APM_CLIENT_EVENT_DATA_READ_DONE		0x100a
#define APM_CLIENT_EVENT_WATERMARK_EVENT	0x100b
#define APM_WRITE_TOKEN_MASK                   GENMASK(15, 0)
#define APM_WRITE_TOKEN_LEN_MASK               GENMASK(31, 16)
#define APM_WRITE_TOKEN_LEN_SHIFT              16

#define APM_MAX_SESSIONS			8
#define APM_LAST_BUFFER_FLAG			BIT(30)
#define NO_TIMESTAMP				0xFF00

struct q6apm {
	struct device *dev;
	struct device *dma_dev;
	gpr_port_t *port;
	gpr_device_t *gdev;
	/* For Graph OPEN/START/STOP/CLOSE operations */
	wait_queue_head_t wait;
	struct gpr_ibasic_rsp_result_t result;
	u32 result_token;
	u32 pending_opcode;
	u32 pending_rsp_opcode;
	u32 pending_token;
	u32 cmd_token;
	bool cmd_pending;

	struct mutex cmd_lock;
	struct mutex lock;
	/* Serialize graph-client publication against service removal. */
	struct mutex client_lock;
	/* Protect command matching from asynchronous GPR responses. */
	spinlock_t result_lock;
	/* Protect graph lifetime during asynchronous GPR responses. */
	spinlock_t graph_lock;
	uint32_t state;

	struct list_head widget_list;
	struct list_head graph_client_list;
	bool removing;
	struct idr graph_idr;
	struct idr graph_info_idr;
	struct idr sub_graphs_idr;
	struct idr containers_idr;
	struct idr modules_idr;
};

struct audio_buffer {
	phys_addr_t phys;
	uint32_t size;		/* size of buffer */
};

struct audioreach_graph_data {
	struct audio_buffer *buf;
	uint32_t num_periods;
	uint32_t dsp_buf;
	atomic_t hw_ptr;
};

struct audioreach_graph {
	struct audioreach_graph_info *info;
	uint32_t id;
	int state;
	int start_count;
	bool initializing;
	struct device *dma_dev;
	void *oob_virt;
	dma_addr_t oob_dma;
	phys_addr_t oob_dsp_addr;
	size_t oob_size;
	u32 oob_mem_map_handle;
	u32 oob_token;
	/* Serialize use of the graph's shared OOB buffer. */
	struct mutex oob_lock;
	/* Serialize topology-derived protection state for this graph only. */
	struct mutex protection_lock;
	bool protection_profile;
	bool protection_malformed;
	bool protection_runtime;
	bool protection_available;
	bool protection_configured;
	bool protection_bypass_confirmed;
	bool protection_vi_ready;
	bool protection_cps_ready;
	bool protection_faulted;
	bool prepared;
	bool prepare_uncertain;
	bool close_confirmed;
	bool execution_uncertain;
	bool dma_quarantined;
	bool oob_map_uncertain;
	bool oob_transfer_uncertain;
	/* Cached Graph data */
	void *graph;
	struct kref refcount;
	struct q6apm *apm;
};

typedef void (*q6apm_cb) (uint32_t opcode, uint32_t token,
			  void *payload, void *priv);
struct q6apm_graph {
	void *priv;
	q6apm_cb cb;
	uint32_t id;
	uint32_t shm_iid;
	struct device *dev;
	struct q6apm *apm;
	struct list_head node;
	bool retained;
	bool protection_profile;
	bool protection_runtime;
	bool is_push_pull_mode;
	bool dying;
	bool detached;
	bool retain_dma_on_detach;
	unsigned int active_users;
	/* Protect admission and detach state for graph API users. */
	spinlock_t lifecycle_lock;
	wait_queue_head_t users_wait;
	struct completion detached_done;
	gpr_port_t *port;
	struct audioreach_graph_data rx_data;
	struct audioreach_graph_data tx_data;
	struct gpr_ibasic_rsp_result_t result;
	u32 result_token;
	u32 pending_opcode;
	u32 pending_rsp_opcode;
	u32 pending_token;
	u32 cmd_token;
	bool cmd_pending;
	wait_queue_head_t cmd_wait;
	/* Serialize synchronous commands without blocking data callbacks. */
	struct mutex cmd_lock;
	struct mutex lock;
	/* Protect command matching from asynchronous client responses. */
	spinlock_t result_lock;
	struct audioreach_graph *ar_graph;
	struct audioreach_graph_info *info;
};

/* Graph Operations */
struct q6apm_graph *q6apm_graph_open(struct device *dev, q6apm_cb cb,
				     void *priv, int graph_id, int dir,
				     bool enable_protection);
bool q6apm_graph_user_get(struct q6apm_graph *graph);
void q6apm_graph_user_put(struct q6apm_graph *graph);
int q6apm_graph_close(struct q6apm_graph *graph);
int q6apm_graph_prepare(struct q6apm_graph *graph);
int q6apm_graph_start(struct q6apm_graph *graph);
int q6apm_graph_stop(struct q6apm_graph *graph);
int q6apm_graph_flush(struct q6apm_graph *graph);
bool q6apm_graph_execution_uncertain(struct q6apm_graph *graph);
void q6apm_graph_quarantine_dma(struct q6apm_graph *graph);
bool q6apm_graph_dma_quarantined(struct device *dev, unsigned int graph_id);
bool q6apm_graph_id_has_protection(struct device *dev, unsigned int graph_id);

/* Media Format */
int q6apm_graph_media_format_pcm(struct q6apm_graph *graph,
				 struct audioreach_module_config *cfg);

int q6apm_graph_media_format_shmem(struct q6apm_graph *graph,
				   struct audioreach_module_config *cfg);

/* read/write related */
int q6apm_read(struct q6apm_graph *graph);
int q6apm_write_async(struct q6apm_graph *graph, uint32_t len, uint32_t msw_ts,
		      uint32_t lsw_ts, uint32_t wflags);

/* Memory Map related */
int q6apm_map_memory_fixed_region(struct device *dev,
			     unsigned int graph_id, phys_addr_t phys,
			     size_t sz);
int q6apm_map_pos_buffer(struct device *dev,
			     unsigned int graph_id, phys_addr_t phys,
			     size_t sz);
int q6apm_unmap_pos_buffer(struct device *dev, unsigned int graph_id);
int q6apm_alloc_fragments(struct q6apm_graph *graph,
			unsigned int dir, phys_addr_t phys,
			size_t period_sz, unsigned int periods);
int q6apm_free_fragments(struct q6apm_graph *graph, unsigned int dir);
int q6apm_unmap_memory_fixed_region(struct device *dev, unsigned int graph_id);
/* Helpers */
int q6apm_send_cmd_sync(struct q6apm *apm, struct gpr_pkt *pkt,
			uint32_t rsp_opcode);
int q6apm_send_oob_config(struct audioreach_graph *graph,
			  const void *data, size_t size);
int q6apm_send_graph_oob_config(struct q6apm_graph *graph,
				const void *data, size_t size);
int q6apm_graph_id_for_backend(struct device *dev, int backend_id);
bool q6apm_graph_has_protection(const struct q6apm_graph *graph);
int q6apm_graph_configure_protection(struct q6apm_graph *graph);

enum q6apm_protection_backend {
	Q6APM_PROTECTION_BACKEND_VI,
	Q6APM_PROTECTION_BACKEND_CPS,
};

int q6apm_set_protection_backend_ready(struct device *dev, int backend_id,
				       enum q6apm_protection_backend backend,
				       bool ready);

/* Callback for graph specific */
struct audioreach_module *q6apm_find_module_by_mid(struct q6apm_graph *graph,
						    uint32_t mid);
bool q6apm_is_adsp_ready(void);

int q6apm_enable_compress_module(struct device *dev, struct q6apm_graph *graph, bool en);
int q6apm_remove_initial_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples);
int q6apm_remove_trailing_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples);
int q6apm_set_real_module_id(struct device *dev, struct q6apm_graph *graph, uint32_t codec_id);
int q6apm_get_hw_pointer(struct q6apm_graph *graph, int dir);
bool q6apm_is_graph_in_push_pull_mode(struct q6apm_graph *graph);
bool q6apm_is_graph_in_push_pull_mode_from_id(struct device *dev, unsigned int graph_id, int dir);
int q6apm_push_pull_config(struct q6apm_graph *graph, phys_addr_t bphys,
			   phys_addr_t pphys, uint32_t size);

int q6apm_register_watermark_event(struct q6apm_graph *graph, int watermark_bytes, int num_levels);
#endif /* __APM_GRAPH_ */
