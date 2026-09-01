// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/soc/qcom,gpr.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/wait.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include "audioreach.h"
#include "q6apm.h"
#include "q6dsp-errno.h"

/* Graph Management */
#define APM_MMAP_TOKEN_SEQ_SHIFT		17
#define APM_MMAP_TOKEN_SEQ_MASK		GENMASK(31, 17)

struct apm_graph_mgmt_cmd {
	struct apm_module_param_data param_data;
	uint32_t num_sub_graphs;
	uint32_t sub_graph_id_list[];
} __packed;

#define APM_GRAPH_MGMT_PSIZE(p, n) ALIGN(struct_size(p, sub_graph_id_list, n), 8)

static struct q6apm *g_apm;
static DEFINE_MUTEX(g_apm_lock);
static atomic_t q6apm_mmap_token_seq = ATOMIC_INIT(0);

bool q6apm_graph_user_get(struct q6apm_graph *graph)
{
	unsigned long flags;
	bool acquired = false;

	if (!graph)
		return false;

	spin_lock_irqsave(&graph->lifecycle_lock, flags);
	if (!graph->dying && !graph->detached) {
		graph->active_users++;
		acquired = true;
	}
	spin_unlock_irqrestore(&graph->lifecycle_lock, flags);

	return acquired;
}

void q6apm_graph_user_put(struct q6apm_graph *graph)
{
	unsigned long flags;
	bool wake = false;

	if (!graph)
		return;

	spin_lock_irqsave(&graph->lifecycle_lock, flags);
	if (WARN_ON(!graph->active_users)) {
		spin_unlock_irqrestore(&graph->lifecycle_lock, flags);
		return;
	}
	wake = --graph->active_users == 0;
	spin_unlock_irqrestore(&graph->lifecycle_lock, flags);

	if (wake)
		wake_up(&graph->users_wait);
}

DEFINE_FREE(q6apm_graph_user, struct q6apm_graph *, q6apm_graph_user_put(_T))

static void q6apm_graph_abort_cmd(struct q6apm_graph *graph)
{
	spin_lock(&graph->result_lock);
	graph->cmd_pending = false;
	spin_unlock(&graph->result_lock);
	wake_up(&graph->cmd_wait);
}

static void q6apm_graph_mark_dying(struct q6apm_graph *graph)
{
	unsigned long flags;

	spin_lock_irqsave(&graph->lifecycle_lock, flags);
	graph->dying = true;
	spin_unlock_irqrestore(&graph->lifecycle_lock, flags);
	q6apm_graph_abort_cmd(graph);
}

static void q6apm_graph_wait_users(struct q6apm_graph *graph)
{
	wait_event(graph->users_wait, !READ_ONCE(graph->active_users));
}

int q6apm_send_cmd_sync(struct q6apm *apm, struct gpr_pkt *pkt,
			uint32_t rsp_opcode)
{
	struct gpr_hdr *hdr = &pkt->hdr;
	gpr_device_t *gdev = apm->gdev;
	u32 result_status;
	bool aborted;
	int ret;

	mutex_lock(&apm->lock);
	if (READ_ONCE(apm->removing)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (!hdr->token) {
		do {
			hdr->token = ++apm->cmd_token;
		} while (!hdr->token);
	}
	spin_lock(&apm->result_lock);
	apm->result.opcode = 0;
	apm->result.status = 0;
	apm->result_token = U32_MAX;
	apm->pending_opcode = hdr->opcode;
	apm->pending_rsp_opcode = rsp_opcode;
	apm->pending_token = hdr->token;
	apm->cmd_pending = true;
	spin_unlock(&apm->result_lock);

	ret = gpr_send_pkt(gdev, pkt);
	if (ret >= 0)
		ret = wait_event_timeout(apm->wait,
					 !READ_ONCE(apm->cmd_pending) ||
					 (((READ_ONCE(apm->result.opcode) ==
					   hdr->opcode) ||
					  (rsp_opcode &&
					   READ_ONCE(apm->result.opcode) ==
					   rsp_opcode)) &&
					 READ_ONCE(apm->result_token) ==
					 hdr->token), 5 * HZ);

	spin_lock(&apm->result_lock);
	aborted = !apm->cmd_pending || apm->removing;
	if (!ret && apm->result_token == hdr->token &&
	    (apm->result.opcode == hdr->opcode ||
	     (rsp_opcode && apm->result.opcode == rsp_opcode)))
		ret = 1;
	apm->cmd_pending = false;
	result_status = apm->result.status;
	spin_unlock(&apm->result_lock);

	if (aborted) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (ret < 0)
		goto unlock;
	if (!ret) {
		dev_err(&gdev->dev, "CMD timeout for [%x] opcode\n",
			hdr->opcode);
		ret = -ETIMEDOUT;
	} else if (result_status > 0) {
		dev_err(&gdev->dev, "DSP returned error[%x] %x\n",
			hdr->opcode, result_status);
		ret = result_status == ADSP_EUNSUPPORTED ?
			-EOPNOTSUPP : -EINVAL;
	} else {
		ret = 0;
	}

unlock:
	mutex_unlock(&apm->lock);
	return ret;
}

static u32 q6apm_next_mmap_token(u32 graph_id, u32 map_type)
{
	u32 sequence;

	sequence = (u32)atomic_inc_return(&q6apm_mmap_token_seq) <<
		   APM_MMAP_TOKEN_SEQ_SHIFT;

	return (graph_id & APM_MMAP_TOKEN_GID_MASK) | map_type |
		(sequence & APM_MMAP_TOKEN_SEQ_MASK);
}

static struct audioreach_graph *
q6apm_get_audioreach_graph(struct q6apm *apm, u32 graph_id)
{
	struct audioreach_graph_info *info;
	struct audioreach_graph *graph;
	int id;

	mutex_lock(&apm->lock);
	spin_lock(&apm->graph_lock);
	graph = idr_find(&apm->graph_idr, graph_id);
	if (graph && (graph->initializing ||
		      !kref_get_unless_zero(&graph->refcount))) {
		spin_unlock(&apm->graph_lock);
		mutex_unlock(&apm->lock);
		return ERR_PTR(-EBUSY);
	}
	spin_unlock(&apm->graph_lock);
	info = idr_find(&apm->graph_info_idr, graph_id);
	mutex_unlock(&apm->lock);

	if (graph)
		return graph;

	if (!info)
		return ERR_PTR(-ENODEV);

	graph = kzalloc_obj(*graph);
	if (!graph)
		return ERR_PTR(-ENOMEM);

	graph->apm = apm;
	graph->info = info;
	graph->id = graph_id;
	graph->initializing = true;

	graph->graph = audioreach_alloc_graph_pkt(apm, info);
	if (IS_ERR(graph->graph)) {
		void *err = graph->graph;

		kfree(graph);
		return ERR_CAST(err);
	}

	kref_init(&graph->refcount);
	mutex_lock(&apm->lock);
	spin_lock(&apm->graph_lock);
	id = idr_alloc(&apm->graph_idr, graph, graph_id, graph_id + 1,
		       GFP_ATOMIC);
	spin_unlock(&apm->graph_lock);
	if (id < 0) {
		dev_err(apm->dev, "Unable to allocate graph id (%d)\n", graph_id);
		kfree(graph->graph);
		kfree(graph);
		mutex_unlock(&apm->lock);
		return ERR_PTR(id);
	}
	mutex_unlock(&apm->lock);

	id = q6apm_send_cmd_sync(apm, graph->graph, 0);
	if (id == -ETIMEDOUT) {
		dev_err(apm->dev,
			"retaining graph %u after uncertain GRAPH_OPEN\n",
			graph_id);
		return ERR_PTR(id);
	}
	if (id)
		goto remove_graph;

	spin_lock(&apm->graph_lock);
	graph->initializing = false;
	spin_unlock(&apm->graph_lock);

	return graph;

remove_graph:
	mutex_lock(&apm->lock);
	spin_lock(&apm->graph_lock);
	idr_remove(&apm->graph_idr, graph->id);
	spin_unlock(&apm->graph_lock);
	mutex_unlock(&apm->lock);
	kfree(graph->graph);
	kfree(graph);
	return ERR_PTR(id);
}

static int audioreach_graph_mgmt_cmd(struct audioreach_graph *graph, uint32_t opcode)
{
	struct audioreach_graph_info *info = graph->info;
	int num_sub_graphs = info->num_sub_graphs;
	struct apm_module_param_data *param_data;
	struct apm_graph_mgmt_cmd *mgmt_cmd;
	struct audioreach_sub_graph *sg;
	struct q6apm *apm = graph->apm;
	int i = 0, payload_size = APM_GRAPH_MGMT_PSIZE(mgmt_cmd, num_sub_graphs);

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size, opcode, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	mgmt_cmd = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	mgmt_cmd->num_sub_graphs = num_sub_graphs;

	param_data = &mgmt_cmd->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_SUB_GRAPH_LIST;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;

	list_for_each_entry(sg, &info->sg_list, node)
		mgmt_cmd->sub_graph_id_list[i++] = sg->sub_graph_id;

	return q6apm_send_cmd_sync(apm, pkt, 0);
}

static void q6apm_put_audioreach_graph(struct kref *ref)
{
	struct audioreach_graph *graph;
	struct q6apm *apm;

	graph = container_of(ref, struct audioreach_graph, refcount);
	apm = graph->apm;

	audioreach_graph_mgmt_cmd(graph, APM_CMD_GRAPH_CLOSE);

	mutex_lock(&apm->lock);
	spin_lock(&apm->graph_lock);
	graph = idr_remove(&apm->graph_idr, graph->id);
	spin_unlock(&apm->graph_lock);
	mutex_unlock(&apm->lock);

	kfree(graph->graph);
	kfree(graph);
}


static int q6apm_get_apm_state(struct q6apm *apm)
{
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(0,
								APM_CMD_GET_SPF_STATE, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	q6apm_send_cmd_sync(apm, pkt, APM_CMD_RSP_GET_SPF_STATE);

	return apm->state;
}

bool q6apm_is_adsp_ready(void)
{
	bool ready = false;

	mutex_lock(&g_apm_lock);
	if (g_apm)
		ready = q6apm_get_apm_state(g_apm);
	mutex_unlock(&g_apm_lock);

	return ready;
}
EXPORT_SYMBOL_GPL(q6apm_is_adsp_ready);

static struct audioreach_module *__q6apm_find_module_by_mid(struct q6apm *apm,
						    struct audioreach_graph_info *info,
						    uint32_t mid)
{
	struct audioreach_container *container;
	struct audioreach_sub_graph *sgs;
	struct audioreach_module *module;

	list_for_each_entry(sgs, &info->sg_list, node) {
		list_for_each_entry(container, &sgs->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if (mid == module->module_id)
					return module;
			}
		}
	}

	return NULL;
}

int q6apm_graph_media_format_shmem(struct q6apm_graph *graph,
				   struct audioreach_module_config *cfg)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_module *module;

	if (!active)
		return -ESHUTDOWN;
	if (cfg->direction == SNDRV_PCM_STREAM_CAPTURE) {
		module = __q6apm_find_module_by_mid(graph->apm,
						    graph->info,
						    MODULE_ID_SH_MEM_PUSH_MODE);
		if (!module)
			module = __q6apm_find_module_by_mid(graph->apm,
							    graph->info,
							    MODULE_ID_RD_SHARED_MEM_EP);
	} else {
		module = __q6apm_find_module_by_mid(graph->apm,
						    graph->info,
						    MODULE_ID_SH_MEM_PULL_MODE);
		if (!module)
			module = __q6apm_find_module_by_mid(graph->apm,
							    graph->info,
							    MODULE_ID_WR_SHARED_MEM_EP);
	}

	if (!module) {
		dev_err(graph->dev, "No SHMEM module found in graph\n");
		return -ENODEV;
	}

	return audioreach_set_media_format(graph, module, cfg);
}
EXPORT_SYMBOL_GPL(q6apm_graph_media_format_shmem);

static int __q6apm_map_memory_fixed_region(struct device *dev, unsigned int graph_id,
					   phys_addr_t phys, size_t sz, bool is_pos_buf)
{
	struct audioreach_graph_info *info;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct apm_shared_map_region_payload *mregions;
	struct apm_cmd_shared_mem_map_regions *cmd;
	int payload_size = sizeof(*cmd) + (sizeof(*mregions));
	uint32_t buf_sz;
	void *p;
	uint32_t pos_mask = is_pos_buf ? APM_MMAP_TOKEN_MAP_TYPE_POS_BUF : 0;
	u32 token = q6apm_next_mmap_token(graph_id, pos_mask);
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size,
					APM_CMD_SHARED_MEM_MAP_REGIONS, token);

	if (!apm)
		return -ENODEV;
	guard(mutex)(&apm->client_lock);
	if (apm->removing)
		return -ESHUTDOWN;
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info)
		return -ENODEV;

	if (is_pos_buf) {
		if (info->pos_buf_mem_map_handle)
			return 0;
	} else {
		if (info->mem_map_handle)
			return 0;
	}

	/* DSP expects size should be aligned to 4K */
	buf_sz = ALIGN(sz, 4096);

	p = (void *)pkt + GPR_HDR_SIZE;
	cmd = p;
	cmd->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = 1;
	if (is_pos_buf)
		cmd->property_flag = 0x2;
	else
		cmd->property_flag = 0x0;

	mregions = p + sizeof(*cmd);

	mregions->shm_addr_lsw = lower_32_bits(phys);
	mregions->shm_addr_msw = upper_32_bits(phys);
	mregions->mem_size_bytes = buf_sz;

	return q6apm_send_cmd_sync(apm, pkt, APM_CMD_RSP_SHARED_MEM_MAP_REGIONS);
}

int q6apm_map_pos_buffer(struct device *dev, unsigned int graph_id, phys_addr_t phys, size_t sz)
{
	return __q6apm_map_memory_fixed_region(dev, graph_id, phys, sz, true);
}
EXPORT_SYMBOL_GPL(q6apm_map_pos_buffer);

int q6apm_map_memory_fixed_region(struct device *dev, unsigned int graph_id,
				  phys_addr_t phys, size_t sz)
{
	return __q6apm_map_memory_fixed_region(dev, graph_id, phys, sz, false);
}
EXPORT_SYMBOL_GPL(q6apm_map_memory_fixed_region);

int q6apm_alloc_fragments(struct q6apm_graph *graph, unsigned int dir, phys_addr_t phys,
				size_t period_sz, unsigned int periods)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_graph_data *data;
	struct audio_buffer *buf;
	int cnt;

	if (!active)
		return -ESHUTDOWN;
	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	mutex_lock(&graph->lock);

	data->dsp_buf = 0;

	if (data->buf) {
		mutex_unlock(&graph->lock);
		return 0;
	}

	buf = kzalloc_objs(struct audio_buffer, periods);
	if (!buf) {
		mutex_unlock(&graph->lock);
		return -ENOMEM;
	}

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	data->buf = buf;

	buf[0].phys = phys;
	buf[0].size = period_sz;

	for (cnt = 1; cnt < periods; cnt++) {
		if (period_sz > 0) {
			buf[cnt].phys = buf[0].phys + (cnt * period_sz);
			buf[cnt].size = period_sz;
		}
	}
	data->num_periods = periods;

	mutex_unlock(&graph->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_alloc_fragments);

static int __q6apm_unmap_memory_fixed_region(struct device *dev, unsigned int graph_id,
					     bool is_pos_buf)
{
	struct apm_cmd_shared_mem_unmap_regions *cmd;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph_info *info;
	uint32_t mem_map_handle;
	u32 map_type = is_pos_buf ? APM_MMAP_TOKEN_MAP_TYPE_POS_BUF : 0;
	u32 token = q6apm_next_mmap_token(graph_id, map_type);
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(sizeof(*cmd),
						APM_CMD_SHARED_MEM_UNMAP_REGIONS, token);
	if (!apm)
		return -ENODEV;
	guard(mutex)(&apm->client_lock);
	if (apm->removing)
		return -ESHUTDOWN;
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info)
		return -ENODEV;

	if (is_pos_buf) {
		if (!info->pos_buf_mem_map_handle)
			return 0;
		mem_map_handle = info->pos_buf_mem_map_handle;
	} else {

		if (!info->mem_map_handle)
			return 0;
		mem_map_handle = info->mem_map_handle;
	}

	cmd = (void *)pkt + GPR_HDR_SIZE;
	cmd->mem_map_handle = mem_map_handle;

	return q6apm_send_cmd_sync(apm, pkt, APM_CMD_SHARED_MEM_UNMAP_REGIONS);
}

int q6apm_unmap_memory_fixed_region(struct device *dev, unsigned int graph_id)
{
	return __q6apm_unmap_memory_fixed_region(dev, graph_id, false);
}
EXPORT_SYMBOL_GPL(q6apm_unmap_memory_fixed_region);

int q6apm_unmap_pos_buffer(struct device *dev, unsigned int graph_id)
{
	return __q6apm_unmap_memory_fixed_region(dev, graph_id, true);
}
EXPORT_SYMBOL_GPL(q6apm_unmap_pos_buffer);

int q6apm_free_fragments(struct q6apm_graph *graph, unsigned int dir)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;

	if (!active)
		return -ESHUTDOWN;
	audioreach_graph_free_buf(graph);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_free_fragments);

int q6apm_remove_initial_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_module *module;

	if (!active)
		return -ESHUTDOWN;
	module = __q6apm_find_module_by_mid(graph->apm, graph->info,
					    MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_REMOVE_INITIAL_SILENCE, samples);
}
EXPORT_SYMBOL_GPL(q6apm_remove_initial_silence);

int q6apm_remove_trailing_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_module *module;

	if (!active)
		return -ESHUTDOWN;
	module = __q6apm_find_module_by_mid(graph->apm, graph->info,
					    MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_REMOVE_TRAILING_SILENCE, samples);
}
EXPORT_SYMBOL_GPL(q6apm_remove_trailing_silence);

int q6apm_enable_compress_module(struct device *dev, struct q6apm_graph *graph, bool en)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_module *module;

	if (!active)
		return -ESHUTDOWN;
	module = __q6apm_find_module_by_mid(graph->apm, graph->info,
					    MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_MODULE_ENABLE, en);
}
EXPORT_SYMBOL_GPL(q6apm_enable_compress_module);

int q6apm_set_real_module_id(struct device *dev, struct q6apm_graph *graph,
			     uint32_t codec_id)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_module *module;
	uint32_t module_id;

	if (!active)
		return -ESHUTDOWN;
	module = __q6apm_find_module_by_mid(graph->apm, graph->info,
					    MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	switch (codec_id) {
	case SND_AUDIOCODEC_MP3:
		module_id = MODULE_ID_MP3_DECODE;
		break;
	case SND_AUDIOCODEC_AAC:
		module_id = MODULE_ID_AAC_DEC;
		break;
	case SND_AUDIOCODEC_FLAC:
		module_id = MODULE_ID_FLAC_DEC;
		break;
	case SND_AUDIOCODEC_OPUS_RAW:
		module_id = MODULE_ID_OPUS_DEC;
		break;
	default:
		return -EINVAL;
	}

	return audioreach_send_u32_param(graph, module, PARAM_ID_REAL_MODULE_ID,
					 module_id);
}
EXPORT_SYMBOL_GPL(q6apm_set_real_module_id);

int q6apm_graph_media_format_pcm(struct q6apm_graph *graph, struct audioreach_module_config *cfg)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_graph_info *info;
	struct audioreach_sub_graph *sgs;
	struct audioreach_container *container;
	struct audioreach_module *module;
	int ret;

	if (!active)
		return -ESHUTDOWN;
	info = graph->info;

	list_for_each_entry(sgs, &info->sg_list, node) {
		list_for_each_entry(container, &sgs->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if ((module->module_id == MODULE_ID_WR_SHARED_MEM_EP) ||
					(module->module_id == MODULE_ID_RD_SHARED_MEM_EP) ||
					(module->module_id == MODULE_ID_SH_MEM_PULL_MODE) ||
					(module->module_id == MODULE_ID_SH_MEM_PUSH_MODE))
					continue;

				ret = audioreach_set_media_format(graph, module, cfg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;

}
EXPORT_SYMBOL_GPL(q6apm_graph_media_format_pcm);

int q6apm_write_async(struct q6apm_graph *graph, uint32_t len, uint32_t msw_ts,
		      uint32_t lsw_ts, uint32_t wflags)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct apm_data_cmd_wr_sh_mem_ep_data_buffer_v2 *write_buffer;
	struct audio_buffer *ab;
	int ret;

	struct gpr_pkt *pkt __free(kfree) = NULL;

	if (!active)
		return -ESHUTDOWN;
	pkt = audioreach_alloc_pkt(sizeof(*write_buffer),
				   DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2,
					graph->rx_data.dsp_buf | (len << APM_WRITE_TOKEN_LEN_SHIFT),
					graph->port->id, graph->shm_iid);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	write_buffer = (void *)pkt + GPR_HDR_SIZE;

	mutex_lock(&graph->lock);
	ab = &graph->rx_data.buf[graph->rx_data.dsp_buf];

	write_buffer->buf_addr_lsw = lower_32_bits(ab->phys);
	write_buffer->buf_addr_msw = upper_32_bits(ab->phys);
	write_buffer->buf_size = len;
	write_buffer->timestamp_lsw = lsw_ts;
	write_buffer->timestamp_msw = msw_ts;
	write_buffer->mem_map_handle = graph->info->mem_map_handle;
	write_buffer->flags = wflags;

	graph->rx_data.dsp_buf++;

	if (graph->rx_data.dsp_buf >= graph->rx_data.num_periods)
		graph->rx_data.dsp_buf = 0;

	mutex_unlock(&graph->lock);

	ret = gpr_send_port_pkt(graph->port, pkt);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_write_async);

int q6apm_read(struct q6apm_graph *graph)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct data_cmd_rd_sh_mem_ep_data_buffer_v2 *read_buffer;
	struct audioreach_graph_data *port;
	struct audio_buffer *ab;
	int ret;

	struct gpr_pkt *pkt __free(kfree) = NULL;

	if (!active)
		return -ESHUTDOWN;
	pkt = audioreach_alloc_pkt(sizeof(*read_buffer),
				   DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2,
					graph->tx_data.dsp_buf, graph->port->id, graph->shm_iid);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	read_buffer = (void *)pkt + GPR_HDR_SIZE;

	mutex_lock(&graph->lock);
	port = &graph->tx_data;
	ab = &port->buf[port->dsp_buf];

	read_buffer->buf_addr_lsw = lower_32_bits(ab->phys);
	read_buffer->buf_addr_msw = upper_32_bits(ab->phys);
	read_buffer->mem_map_handle = graph->info->mem_map_handle;
	read_buffer->buf_size = ab->size;

	port->dsp_buf++;

	if (port->dsp_buf >= port->num_periods)
		port->dsp_buf = 0;

	mutex_unlock(&graph->lock);

	ret = gpr_send_port_pkt(graph->port, pkt);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_read);

int q6apm_get_hw_pointer(struct q6apm_graph *graph, int dir)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_graph_data *data;

	if (!active)
		return 0;
	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	return (int)atomic_read(&data->hw_ptr);
}
EXPORT_SYMBOL_GPL(q6apm_get_hw_pointer);

static bool q6apm_graph_try_complete_cmd(struct q6apm_graph *graph,
					 const struct gpr_hdr *hdr, u32 opcode,
					 u32 status, bool basic_response)
{
	bool expected;

	spin_lock(&graph->result_lock);
	expected = graph->cmd_pending && graph->pending_token == hdr->token &&
		   (graph->pending_opcode == opcode ||
		    graph->pending_rsp_opcode == opcode);
	if (expected && basic_response && !status &&
	    graph->pending_rsp_opcode && graph->pending_rsp_opcode != opcode)
		expected = false;
	if (expected) {
		graph->result.status = status;
		graph->result_token = hdr->token;
		graph->result.opcode = opcode;
	}
	spin_unlock(&graph->result_lock);

	if (expected)
		wake_up(&graph->cmd_wait);

	return expected;
}

static int graph_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	struct data_cmd_rsp_rd_sh_mem_ep_data_buffer_done_v2 *rd_done;
	struct data_cmd_rsp_wr_sh_mem_ep_data_buffer_done_v2 *done;
	struct apm_module_event *event;
	const struct gpr_ibasic_rsp_result_t *result;
	struct q6apm_graph *graph = priv;
	const struct gpr_hdr *hdr = &data->hdr;
	struct device *dev;
	uint32_t client_event;
	bool expected;
	phys_addr_t phys;
	int token;

	if (!q6apm_graph_user_get(graph))
		return 0;

	dev = graph->dev;
	result = data->payload;

	switch (hdr->opcode) {
	case APM_EVENT_MODULE_TO_CLIENT:
		event = data->payload;
		switch (event->event_id) {
		case EVENT_ID_SH_MEM_PULL_PUSH_MODE_WATERMARK:
			client_event = APM_CLIENT_EVENT_WATERMARK_EVENT;
			graph->cb(client_event, hdr->token, data->payload, graph->priv);
			break;
		}

		break;
	case DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2:
		if (!graph->ar_graph)
			break;
		client_event = APM_CLIENT_EVENT_DATA_WRITE_DONE;
		mutex_lock(&graph->lock);
		token = hdr->token & APM_WRITE_TOKEN_MASK;

		done = data->payload;
		if (!graph->rx_data.buf) {
			mutex_unlock(&graph->lock);
			break;
		}
		phys = graph->rx_data.buf[token].phys;
		mutex_unlock(&graph->lock);
		/* token numbering starts at 0 */
		atomic_set(&graph->rx_data.hw_ptr, token + 1);
		if (lower_32_bits(phys) == done->buf_addr_lsw &&
		    upper_32_bits(phys) == done->buf_addr_msw) {
			if (graph->cb)
				graph->cb(client_event, hdr->token, data->payload, graph->priv);
		} else {
			dev_err(dev, "WR BUFF Unexpected addr %08x-%08x\n", done->buf_addr_lsw,
				done->buf_addr_msw);
		}

		break;
	case DATA_CMD_RSP_RD_SH_MEM_EP_DATA_BUFFER_V2:
		if (!graph->ar_graph)
			break;
		client_event = APM_CLIENT_EVENT_DATA_READ_DONE;
		mutex_lock(&graph->lock);
		rd_done = data->payload;
		if (!graph->tx_data.buf) {
			mutex_unlock(&graph->lock);
			break;
		}
		phys = graph->tx_data.buf[hdr->token].phys;
		mutex_unlock(&graph->lock);
		/* token numbering starts at 0 */
		atomic_set(&graph->tx_data.hw_ptr, hdr->token + 1);

		if (upper_32_bits(phys) == rd_done->buf_addr_msw &&
		    lower_32_bits(phys) == rd_done->buf_addr_lsw) {
			if (graph->cb)
				graph->cb(client_event, hdr->token, data->payload, graph->priv);
		} else {
			dev_err(dev, "RD BUFF Unexpected addr %08x-%08x\n", rd_done->buf_addr_lsw,
				rd_done->buf_addr_msw);
		}
		break;
	case DATA_CMD_WR_SH_MEM_EP_EOS_RENDERED:
		client_event = APM_CLIENT_EVENT_CMD_EOS_DONE;
		if (graph->cb)
			graph->cb(client_event, hdr->token, data->payload, graph->priv);
		break;
	case GPR_BASIC_RSP_RESULT:
		switch (result->opcode) {
		case APM_CMD_SHARED_MEM_MAP_REGIONS:
		case DATA_CMD_WR_SH_MEM_EP_MEDIA_FORMAT:
		case APM_CMD_REGISTER_MODULE_EVENTS:
		case APM_CMD_SET_CFG:
		case APM_CMD_GRAPH_START:
		case APM_CMD_GRAPH_STOP:
		case APM_CMD_GRAPH_FLUSH:
			expected = q6apm_graph_try_complete_cmd(graph, hdr,
								result->opcode,
								result->status, true);
			if (expected && result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n",
					result->status, result->opcode);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	q6apm_graph_user_put(graph);
	return 0;
}

int q6apm_register_watermark_event(struct q6apm_graph *graph, int water_mark_level_bytes,
				   int num_levels)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;

	if (!active)
		return -ESHUTDOWN;
	return audioreach_shmem_register_event(graph, water_mark_level_bytes, num_levels);
}
EXPORT_SYMBOL_GPL(q6apm_register_watermark_event);

int q6apm_push_pull_config(struct q6apm_graph *graph, phys_addr_t bphys,
			   phys_addr_t pphys, uint32_t size)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_graph_info *info;

	if (!active)
		return -ESHUTDOWN;
	info = graph->info;

	return audioreach_setup_push_pull(graph, bphys, pphys, info->mem_map_handle,
					  info->pos_buf_mem_map_handle, size);
}
EXPORT_SYMBOL_GPL(q6apm_push_pull_config);

bool q6apm_is_graph_in_push_pull_mode_from_id(struct device *dev, unsigned int graph_id, int dir)
{
	struct audioreach_graph_info *info;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_module *module;

	if (!apm)
		return false;
	guard(mutex)(&apm->client_lock);
	if (apm->removing)
		return false;
	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info)
		return false;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		module = __q6apm_find_module_by_mid(apm, info, MODULE_ID_SH_MEM_PULL_MODE);
	else
		module = __q6apm_find_module_by_mid(apm, info, MODULE_ID_SH_MEM_PUSH_MODE);

	return !!module;

}
EXPORT_SYMBOL_GPL(q6apm_is_graph_in_push_pull_mode_from_id);

bool q6apm_is_graph_in_push_pull_mode(struct q6apm_graph *graph)
{
	return graph && READ_ONCE(graph->is_push_pull_mode);
}
EXPORT_SYMBOL_GPL(q6apm_is_graph_in_push_pull_mode);

static int q6apm_graph_get_module_iid(struct q6apm_graph *graph, uint32_t mid)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, mid);
	if (!module)
		return -ENODEV;

	return module->instance_id;
}

struct q6apm_graph *q6apm_graph_open(struct device *dev, q6apm_cb cb,
				     void *priv, int graph_id, int dir)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph *ar_graph;
	struct q6apm_graph *graph;
	int ret, iid = 0;

	if (!apm)
		return ERR_PTR(-ENODEV);

	mutex_lock(&apm->client_lock);
	if (apm->removing) {
		ret = -ESHUTDOWN;
		goto unlock_clients;
	}

	ar_graph = q6apm_get_audioreach_graph(apm, graph_id);
	if (IS_ERR(ar_graph)) {
		dev_err(dev, "No graph found with id %d\n", graph_id);
		ret = PTR_ERR(ar_graph);
		goto unlock_clients;
	}

	graph = kzalloc_obj(*graph);
	if (!graph) {
		ret = -ENOMEM;
		goto put_ar_graph;
	}

	graph->apm = apm;
	graph->priv = priv;
	graph->cb = cb;
	graph->info = ar_graph->info;
	graph->ar_graph = ar_graph;
	graph->id = ar_graph->id;
	graph->dev = get_device(dev);
	spin_lock_init(&graph->lifecycle_lock);
	init_waitqueue_head(&graph->users_wait);
	init_completion(&graph->detached_done);

	if (dir == SNDRV_PCM_STREAM_PLAYBACK) {
		iid = q6apm_graph_get_module_iid(graph, MODULE_ID_SH_MEM_PULL_MODE);
		if (iid < 0)
			iid = q6apm_graph_get_module_iid(graph, MODULE_ID_WR_SHARED_MEM_EP);
		else
			graph->info->is_push_pull_mode = true;

	} else {
		iid = q6apm_graph_get_module_iid(graph, MODULE_ID_SH_MEM_PUSH_MODE);
		if (iid < 0)
			iid = q6apm_graph_get_module_iid(graph, MODULE_ID_RD_SHARED_MEM_EP);
		else
			graph->info->is_push_pull_mode = true;
	}
	graph->is_push_pull_mode = graph->info->is_push_pull_mode;

	if (iid > 0)
		graph->shm_iid = iid;

	mutex_init(&graph->lock);
	mutex_init(&graph->cmd_lock);
	spin_lock_init(&graph->result_lock);
	init_waitqueue_head(&graph->cmd_wait);

	graph->port = gpr_alloc_port(apm->gdev, dev, graph_callback, graph);
	if (IS_ERR(graph->port)) {
		ret = PTR_ERR(graph->port);
		goto free_graph;
	}
	list_add_tail(&graph->node, &apm->graph_client_list);
	mutex_unlock(&apm->client_lock);

	return graph;

free_graph:
	put_device(graph->dev);
	kfree(graph);
put_ar_graph:
	kref_put(&ar_graph->refcount, q6apm_put_audioreach_graph);
	mutex_unlock(&apm->client_lock);
	return ERR_PTR(ret);

unlock_clients:
	mutex_unlock(&apm->client_lock);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6apm_graph_open);

int q6apm_graph_close(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	struct q6apm *apm;
	unsigned long flags;
	bool detached;
	bool dying;

retry:
	spin_lock_irqsave(&graph->lifecycle_lock, flags);
	detached = graph->detached;
	dying = graph->dying;
	apm = graph->apm;
	spin_unlock_irqrestore(&graph->lifecycle_lock, flags);
	if (detached) {
		put_device(graph->dev);
		kfree(graph);
		return 0;
	}
	if (dying) {
		wait_for_completion(&graph->detached_done);
		goto retry;
	}
	if (!apm)
		return -ESHUTDOWN;

	mutex_lock(&apm->client_lock);
	spin_lock_irqsave(&graph->lifecycle_lock, flags);
	if (graph->dying || graph->detached) {
		spin_unlock_irqrestore(&graph->lifecycle_lock, flags);
		mutex_unlock(&apm->client_lock);
		goto retry;
	}
	spin_unlock_irqrestore(&graph->lifecycle_lock, flags);
	ar_graph = graph->ar_graph;

	q6apm_graph_mark_dying(graph);
	q6apm_graph_wait_users(graph);
	list_del_init(&graph->node);
	gpr_free_port(graph->port);
	graph->port = NULL;
	graph->ar_graph = NULL;
	kref_put(&ar_graph->refcount, q6apm_put_audioreach_graph);
	mutex_unlock(&apm->client_lock);
	put_device(graph->dev);
	kfree(graph);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_graph_close);

int q6apm_graph_prepare(struct q6apm_graph *graph)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;

	if (!active)
		return -ESHUTDOWN;
	return audioreach_graph_mgmt_cmd(graph->ar_graph, APM_CMD_GRAPH_PREPARE);
}
EXPORT_SYMBOL_GPL(q6apm_graph_prepare);

int q6apm_graph_start(struct q6apm_graph *graph)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_graph *ar_graph;
	int ret = 0;

	if (!active)
		return -ESHUTDOWN;
	ar_graph = graph->ar_graph;
	if (ar_graph->start_count == 0)
		ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_START);

	if (!ret)
		ar_graph->start_count++;

	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_start);

int q6apm_graph_stop(struct q6apm_graph *graph)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;
	struct audioreach_graph *ar_graph;
	int ret;

	if (!active)
		return -ESHUTDOWN;
	ar_graph = graph->ar_graph;
	if (ar_graph->start_count <= 0)
		return 0;
	if (--ar_graph->start_count > 0)
		return 0;

	ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_STOP);
	if (ret)
		ar_graph->start_count++;

	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_stop);

int q6apm_graph_flush(struct q6apm_graph *graph)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;

	if (!active)
		return -ESHUTDOWN;
	return audioreach_graph_mgmt_cmd(graph->ar_graph, APM_CMD_GRAPH_FLUSH);
}
EXPORT_SYMBOL_GPL(q6apm_graph_flush);

static int q6apm_audio_probe(struct snd_soc_component *component)
{
	return audioreach_tplg_init(component);
}

static void q6apm_audio_remove(struct snd_soc_component *component)
{
	/* remove topology */
	snd_soc_tplg_component_remove(component);
}

#define APM_AUDIO_DRV_NAME "q6apm-audio"

static const struct snd_soc_component_driver q6apm_audio_component = {
	.name		= APM_AUDIO_DRV_NAME,
	.probe		= q6apm_audio_probe,
	.remove		= q6apm_audio_remove,
	.remove_order   = SND_SOC_COMP_ORDER_LAST,
};

static int apm_probe(gpr_device_t *gdev)
{
	struct device *dev = &gdev->dev;
	struct q6apm *apm;
	int ret;

	apm = devm_kzalloc(dev, sizeof(*apm), GFP_KERNEL);
	if (!apm)
		return -ENOMEM;

	dev_set_drvdata(dev, apm);

	mutex_init(&apm->lock);
	mutex_init(&apm->client_lock);
	spin_lock_init(&apm->result_lock);
	spin_lock_init(&apm->graph_lock);
	apm->dev = dev;
	apm->gdev = gdev;
	init_waitqueue_head(&apm->wait);

	INIT_LIST_HEAD(&apm->widget_list);
	INIT_LIST_HEAD(&apm->graph_client_list);
	idr_init(&apm->graph_idr);
	idr_init(&apm->graph_info_idr);
	idr_init(&apm->sub_graphs_idr);
	idr_init(&apm->containers_idr);

	idr_init(&apm->modules_idr);

	mutex_lock(&g_apm_lock);
	g_apm = apm;
	mutex_unlock(&g_apm_lock);

	q6apm_get_apm_state(apm);

	ret = snd_soc_register_component(dev, &q6apm_audio_component, NULL, 0);
	if (ret < 0) {
		dev_err(dev, "failed to register q6apm: %d\n", ret);
		return ret;
	}

	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret)
		snd_soc_unregister_component(dev);

	return ret;
}

static void q6apm_abort_global_cmd(struct q6apm *apm)
{
	spin_lock(&apm->result_lock);
	apm->cmd_pending = false;
	spin_unlock(&apm->result_lock);
	wake_up(&apm->wait);
}

static void q6apm_detach_graph_clients(struct q6apm *apm)
{
	struct q6apm_graph *graph, *next;
	LIST_HEAD(detaching);

	mutex_lock(&apm->client_lock);
	apm->removing = true;
	list_for_each_entry(graph, &apm->graph_client_list, node)
		q6apm_graph_mark_dying(graph);
	list_splice_init(&apm->graph_client_list, &detaching);
	mutex_unlock(&apm->client_lock);
	q6apm_abort_global_cmd(apm);
	/* Drain the command thread awakened by the abort before teardown. */
	mutex_lock(&apm->lock);
	mutex_unlock(&apm->lock);

	list_for_each_entry_safe(graph, next, &detaching, node) {
		unsigned long flags;

		list_del_init(&graph->node);
		q6apm_graph_wait_users(graph);
		gpr_free_port(graph->port);

		spin_lock_irqsave(&graph->lifecycle_lock, flags);
		graph->port = NULL;
		graph->ar_graph = NULL;
		graph->info = NULL;
		graph->apm = NULL;
		graph->detached = true;
		spin_unlock_irqrestore(&graph->lifecycle_lock, flags);
		complete_all(&graph->detached_done);
		/* The ALSA owner releases the detached client on its close path. */
	}
}

static void q6apm_remove_graph_state(struct q6apm *apm)
{
	struct audioreach_graph *ar_graph;
	int id = 0;

	guard(mutex)(&apm->client_lock);
	while ((ar_graph = idr_get_next(&apm->graph_idr, &id))) {
		idr_remove(&apm->graph_idr, id);
		kfree(ar_graph->graph);
		kfree(ar_graph);
		id++;
	}
}

static void apm_remove(gpr_device_t *gdev)
{
	struct q6apm *apm = dev_get_drvdata(&gdev->dev);

	mutex_lock(&g_apm_lock);
	if (g_apm == apm)
		g_apm = NULL;
	mutex_unlock(&g_apm_lock);
	q6apm_detach_graph_clients(apm);
	of_platform_depopulate(&gdev->dev);
	snd_soc_unregister_component(&gdev->dev);
	q6apm_remove_graph_state(apm);
}

struct audioreach_module *q6apm_find_module_by_mid(struct q6apm_graph *graph, uint32_t mid)
{
	struct q6apm_graph *active __free(q6apm_graph_user) =
		q6apm_graph_user_get(graph) ? graph : NULL;

	if (!active)
		return NULL;

	return __q6apm_find_module_by_mid(graph->apm, graph->info, mid);
}

static bool __q6apm_cmd_response_expected(struct q6apm *apm,
					  const struct gpr_hdr *hdr,
					  u32 opcode, u32 status,
					  bool basic_response)
{
	bool expected;

	expected = apm->cmd_pending && apm->pending_token == hdr->token &&
		   (apm->pending_opcode == opcode ||
		    apm->pending_rsp_opcode == opcode);
	if (expected && basic_response && !status &&
	    apm->pending_rsp_opcode && apm->pending_rsp_opcode != opcode)
		expected = false;
	return expected;
}

static bool q6apm_try_complete_cmd(struct q6apm *apm,
				   const struct gpr_hdr *hdr, u32 opcode,
				   u32 status, bool basic_response)
{
	bool expected;

	spin_lock(&apm->result_lock);
	expected = __q6apm_cmd_response_expected(apm, hdr, opcode, status,
						 basic_response);
	if (expected) {
		apm->result.status = status;
		apm->result_token = hdr->token;
		apm->result.opcode = opcode;
	}
	spin_unlock(&apm->result_lock);

	if (expected)
		wake_up(&apm->wait);

	return expected;
}

static int apm_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	gpr_device_t *gdev = priv;
	struct audioreach_graph_info *info = NULL;
	struct q6apm *apm = dev_get_drvdata(&gdev->dev);
	struct apm_cmd_rsp_shared_mem_map_regions *rsp;
	struct device *dev = &gdev->dev;
	struct gpr_ibasic_rsp_result_t *result;
	const struct gpr_hdr *hdr = &data->hdr;
	bool expected;
	int graph_id, is_pos_buf;

	result = data->payload;

	switch (hdr->opcode) {
	case APM_CMD_RSP_GET_SPF_STATE:
		/* First word of result is the state. */
		spin_lock(&apm->result_lock);
		expected = __q6apm_cmd_response_expected(apm, hdr, hdr->opcode,
							 0, false);
		if (expected) {
			apm->state = result->opcode;
			apm->result.status = 0;
			apm->result_token = hdr->token;
			apm->result.opcode = hdr->opcode;
		}
		spin_unlock(&apm->result_lock);
		if (expected)
			wake_up(&apm->wait);
		break;
	case GPR_BASIC_RSP_RESULT:
		switch (result->opcode) {
		case APM_CMD_SHARED_MEM_MAP_REGIONS:
			expected = q6apm_try_complete_cmd(apm, hdr,
							  result->opcode,
							  result->status, true);
			if (expected && result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n",
					result->status, result->opcode);
			break;
		case APM_CMD_GRAPH_START:
		case APM_CMD_GRAPH_OPEN:
		case APM_CMD_GRAPH_PREPARE:
		case APM_CMD_GRAPH_CLOSE:
		case APM_CMD_GRAPH_FLUSH:
		case APM_CMD_GRAPH_STOP:
		case APM_CMD_SET_CFG:
			expected = q6apm_try_complete_cmd(apm, hdr,
							  result->opcode,
							  result->status, true);
			if (expected && result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n", result->status,
					result->opcode);
			break;
		case APM_CMD_SHARED_MEM_UNMAP_REGIONS:
			if (result->status) {
				expected = q6apm_try_complete_cmd(apm, hdr,
								  result->opcode,
								  result->status,
								  true);
				if (expected)
					dev_err(dev,
						"Error (%d) Processing 0x%08x cmd\n",
						result->status, result->opcode);
				break;
			}

			graph_id = hdr->token & APM_MMAP_TOKEN_GID_MASK;
			is_pos_buf =
				hdr->token & APM_MMAP_TOKEN_MAP_TYPE_POS_BUF;
			spin_lock(&apm->result_lock);
			expected = __q6apm_cmd_response_expected(apm, hdr,
								 result->opcode,
								 0, true);
			if (expected) {
				info = idr_find(&apm->graph_info_idr, graph_id);
				if (info && is_pos_buf)
					info->pos_buf_mem_map_handle = 0;
				else if (info)
					info->mem_map_handle = 0;
				apm->result.status = 0;
				apm->result_token = hdr->token;
				apm->result.opcode = result->opcode;
			}
			spin_unlock(&apm->result_lock);
			if (expected) {
				wake_up(&apm->wait);
				if (!info)
					dev_err(dev,
						"no mapping for 0x%08x response token 0x%08x\n",
						result->opcode, hdr->token);
			}
			break;
		default:
			break;
		}
		break;
	case APM_CMD_RSP_SHARED_MEM_MAP_REGIONS:
		rsp = data->payload;
		graph_id = hdr->token & APM_MMAP_TOKEN_GID_MASK;
		is_pos_buf = hdr->token & APM_MMAP_TOKEN_MAP_TYPE_POS_BUF;

		spin_lock(&apm->result_lock);
		expected = __q6apm_cmd_response_expected(apm, hdr, hdr->opcode,
							 0, false);
		if (expected)
			info = idr_find(&apm->graph_info_idr, graph_id);
		if (info) {
			if (is_pos_buf)
				info->pos_buf_mem_map_handle = rsp->mem_map_handle;
			else
				info->mem_map_handle = rsp->mem_map_handle;
		}

		if (expected) {
			apm->result.status = 0;
			apm->result_token = hdr->token;
			apm->result.opcode = hdr->opcode;
		}
		spin_unlock(&apm->result_lock);
		if (expected)
			wake_up(&apm->wait);
		if (expected && !info)
			dev_err(dev,
				"no graph for 0x%08x response token 0x%08x\n",
				hdr->opcode, hdr->token);
		break;
	default:
		break;
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id apm_device_id[]  = {
	{ .compatible = "qcom,q6apm" },
	{},
};
MODULE_DEVICE_TABLE(of, apm_device_id);
#endif

static gpr_driver_t apm_driver = {
	.probe = apm_probe,
	.remove = apm_remove,
	.gpr_callback = apm_callback,
	.driver = {
		.name = "qcom-apm",
		.of_match_table = of_match_ptr(apm_device_id),
	},
};

module_gpr_driver(apm_driver);
MODULE_DESCRIPTION("Audio Process Manager");
MODULE_LICENSE("GPL");
