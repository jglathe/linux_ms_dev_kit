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
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include "audioreach.h"
#include "q6apm.h"

/* Graph Management */
#define Q6APM_OOB_MAP_TOKEN	BIT(31)
#define Q6APM_POSITION_MAP_TOKEN	BIT(30)
#define Q6APM_OOB_BUFFER_SIZE	SZ_16K
#define Q6APM_DSP_SID_MASK	GENMASK(3, 0)

/* Windows DEFAULT speaker SOFT_PAUSE lifecycle, evidence-locked on SP11. */
#define SP11_MODULE_ID_SOFT_PAUSE		0x07001019
#define SP11_SOFT_PAUSE_IID			0x0000466b
#define SP11_PARAM_SOFT_PAUSE_PAUSE		0x0800102e
#define SP11_PARAM_SOFT_PAUSE_RESUME		0x0800102f

struct apm_graph_mgmt_cmd {
	struct apm_module_param_data param_data;
	uint32_t num_sub_graphs;
	uint32_t sub_graph_id_list[];
} __packed;

#define APM_GRAPH_MGMT_PSIZE(p, n) ALIGN(struct_size(p, sub_graph_id_list, n), 8)

static struct q6apm *g_apm;
static bool sp11_vi_ready;
static bool sp11_cps_ready;

void q6apm_sp11_set_vi_ready(bool ready)
{
	WRITE_ONCE(sp11_vi_ready, ready);
}
EXPORT_SYMBOL_GPL(q6apm_sp11_set_vi_ready);

bool q6apm_sp11_vi_ready(void)
{
	return READ_ONCE(sp11_vi_ready);
}
EXPORT_SYMBOL_GPL(q6apm_sp11_vi_ready);

void q6apm_sp11_set_cps_ready(bool ready)
{
	WRITE_ONCE(sp11_cps_ready, ready);
}
EXPORT_SYMBOL_GPL(q6apm_sp11_set_cps_ready);

bool q6apm_sp11_cps_ready(void)
{
	return READ_ONCE(sp11_cps_ready);
}
EXPORT_SYMBOL_GPL(q6apm_sp11_cps_ready);

static int audioreach_graph_mgmt_cmd(struct audioreach_graph *graph,
				     uint32_t opcode);
static int audioreach_graph_client_mgmt_cmd(struct q6apm_graph *graph,
					    uint32_t opcode);

static phys_addr_t q6apm_dsp_addr(struct device *dev, dma_addr_t dma_addr)
{
	struct of_phandle_args args;
	phys_addr_t dsp_addr = dma_addr;

	if (!dev->of_node ||
	    of_parse_phandle_with_fixed_args(dev->of_node, "iommus", 1, 0,
					     &args))
		return dsp_addr;

	dsp_addr |= (phys_addr_t)(args.args[0] & Q6APM_DSP_SID_MASK) << 32;
	of_node_put(args.np);

	return dsp_addr;
}

int q6apm_send_cmd_sync(struct q6apm *apm, const struct gpr_pkt *pkt,
			uint32_t rsp_opcode)
{
	gpr_device_t *gdev = apm->gdev;

	return audioreach_send_cmd_sync(&gdev->dev, gdev, &apm->result, &apm->lock,
					NULL, &apm->wait, pkt, rsp_opcode);
}

static int q6apm_map_oob_buffer(struct audioreach_graph *graph)
{
	struct apm_shared_map_region_payload *region;
	struct apm_cmd_shared_mem_map_regions *cmd;
	struct q6apm *apm = graph->apm;
	u32 token = Q6APM_OOB_MAP_TOKEN | graph->id;
	int payload_size = sizeof(*cmd) + sizeof(*region);
	void *p;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(payload_size,
					    APM_CMD_SHARED_MEM_MAP_REGIONS,
					    token);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	graph->oob_size = Q6APM_OOB_BUFFER_SIZE;
	graph->oob_virt = dma_alloc_coherent(graph->dma_dev, graph->oob_size,
					     &graph->oob_phys, GFP_KERNEL);
	if (!graph->oob_virt)
		return -ENOMEM;
	graph->oob_dsp_addr = q6apm_dsp_addr(graph->dma_dev, graph->oob_phys);

	p = (u8 *)pkt + GPR_HDR_SIZE;
	cmd = p;
	cmd->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = 1;
	cmd->property_flag = 0;
	region = p + sizeof(*cmd);
	region->shm_addr_lsw = lower_32_bits(graph->oob_dsp_addr);
	region->shm_addr_msw = upper_32_bits(graph->oob_dsp_addr);
	region->mem_size_bytes = graph->oob_size;

	int ret = q6apm_send_cmd_sync(apm, pkt,
				      APM_CMD_RSP_SHARED_MEM_MAP_REGIONS);
	if (ret) {
		dma_free_coherent(graph->dma_dev, graph->oob_size,
				  graph->oob_virt, graph->oob_phys);
		graph->oob_virt = NULL;
		graph->oob_phys = 0;
		graph->oob_dsp_addr = 0;
		graph->oob_size = 0;
	} else {
		dev_info(graph->dma_dev,
			 "protection OOB map DMA %pad DSP %pap handle %#x\n",
			 &graph->oob_phys, &graph->oob_dsp_addr,
			 graph->oob_mem_map_handle);
	}
	return ret;
}

static int q6apm_map_position_buffer(struct audioreach_graph *graph)
{
	struct apm_shared_map_region_payload *region;
	struct apm_cmd_shared_mem_map_regions *cmd;
	u32 token = Q6APM_POSITION_MAP_TOKEN | graph->id;
	int payload_size = sizeof(*cmd) + sizeof(*region);
	void *p;
	int ret;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(payload_size,
					    APM_CMD_SHARED_MEM_MAP_REGIONS,
					    token);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	graph->position_size = PAGE_SIZE;
	graph->position_virt =
		dma_alloc_coherent(graph->dma_dev, graph->position_size,
				   &graph->position_phys, GFP_KERNEL);
	if (!graph->position_virt)
		return -ENOMEM;
	memset(graph->position_virt, 0, graph->position_size);
	graph->position_dsp_addr =
		q6apm_dsp_addr(graph->dma_dev, graph->position_phys);

	p = (u8 *)pkt + GPR_HDR_SIZE;
	cmd = p;
	cmd->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = 1;
	/*
	 * The pull-mode position structure is shared live between the DSP and
	 * HLOS.  AudioReach updates it without cache maintenance and therefore
	 * requires this mapping to be uncached.
	 */
	cmd->property_flag = APM_MEMORY_MAP_FLAG_UNCACHED;
	region = p + sizeof(*cmd);
	region->shm_addr_lsw = lower_32_bits(graph->position_dsp_addr);
	region->shm_addr_msw = upper_32_bits(graph->position_dsp_addr);
	region->mem_size_bytes = graph->position_size;

	ret = q6apm_send_cmd_sync(graph->apm, pkt,
				  APM_CMD_RSP_SHARED_MEM_MAP_REGIONS);
	if (ret) {
		dma_free_coherent(graph->dma_dev, graph->position_size,
				  graph->position_virt, graph->position_phys);
		graph->position_virt = NULL;
		graph->position_phys = 0;
		graph->position_dsp_addr = 0;
		graph->position_size = 0;
		return ret;
	}

	dev_info(graph->dma_dev,
		 "pull position map DMA %pad DSP %pap handle %#x\n",
		 &graph->position_phys, &graph->position_dsp_addr,
		 graph->position_mem_map_handle);
	return 0;
}

static int q6apm_unmap_position_buffer(struct audioreach_graph *graph)
{
	struct apm_cmd_shared_mem_unmap_regions *cmd;
	u32 token = Q6APM_POSITION_MAP_TOKEN | graph->id;
	int ret = 0;

	if (graph->position_mem_map_handle) {
		struct gpr_pkt *pkt __free(kfree) =
			audioreach_alloc_apm_cmd_pkt(
				sizeof(*cmd),
				APM_CMD_SHARED_MEM_UNMAP_REGIONS, token);

		if (IS_ERR(pkt)) {
			ret = PTR_ERR(pkt);
		} else {
			cmd = (void *)pkt + GPR_HDR_SIZE;
			cmd->mem_map_handle = graph->position_mem_map_handle;
			ret = q6apm_send_cmd_sync(
				graph->apm, pkt,
				APM_CMD_SHARED_MEM_UNMAP_REGIONS);
		}
	}

	if (graph->position_virt) {
		dma_free_coherent(graph->dma_dev, graph->position_size,
				  graph->position_virt, graph->position_phys);
		graph->position_virt = NULL;
		graph->position_phys = 0;
		graph->position_dsp_addr = 0;
		graph->position_size = 0;
	}
	return ret;
}

static int q6apm_unmap_oob_buffer(struct audioreach_graph *graph)
{
	struct q6apm *apm = graph->apm;
	u32 token = Q6APM_OOB_MAP_TOKEN | graph->id;
	struct apm_cmd_shared_mem_unmap_regions *cmd;
	int ret = 0;

	if (graph->oob_mem_map_handle) {
		struct gpr_pkt *pkt __free(kfree) =
			audioreach_alloc_apm_cmd_pkt(
				sizeof(*cmd),
				APM_CMD_SHARED_MEM_UNMAP_REGIONS, token);

		if (IS_ERR(pkt)) {
			ret = PTR_ERR(pkt);
		} else {
			cmd = (void *)pkt + GPR_HDR_SIZE;
			cmd->mem_map_handle = graph->oob_mem_map_handle;
			ret = q6apm_send_cmd_sync(
				apm, pkt, APM_CMD_SHARED_MEM_UNMAP_REGIONS);
		}
	}

	if (graph->oob_virt) {
		dma_free_coherent(graph->dma_dev, graph->oob_size,
				  graph->oob_virt, graph->oob_phys);
		graph->oob_virt = NULL;
		graph->oob_phys = 0;
		graph->oob_dsp_addr = 0;
		graph->oob_size = 0;
	}
	return ret;
}

int q6apm_send_oob_config(struct audioreach_graph *graph,
			   const void *data, size_t size)
{
	struct apm_cmd_header *cmd;
	int ret;

	if (!graph->oob_virt || !graph->oob_mem_map_handle ||
	    !size || size > graph->oob_size)
		return -EINVAL;

	memset(graph->oob_virt, 0, graph->oob_size);
	memcpy(graph->oob_virt, data, size);
	dma_wmb();

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(0, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	cmd = (void *)pkt + GPR_HDR_SIZE;
	cmd->payload_address_lsw = lower_32_bits(graph->oob_dsp_addr);
	cmd->payload_address_msw = upper_32_bits(graph->oob_dsp_addr);
	cmd->mem_map_handle = graph->oob_mem_map_handle;
	cmd->payload_size = size;
	ret = q6apm_send_cmd_sync(graph->apm, pkt, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_send_oob_config);

int q6apm_send_graph_oob_config(struct q6apm_graph *graph,
				const void *data, size_t size)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	struct apm_cmd_header *cmd;

	if (!ar_graph || !ar_graph->oob_virt ||
	    !ar_graph->oob_mem_map_handle || !size ||
	    size > ar_graph->oob_size)
		return -EINVAL;

	memset(ar_graph->oob_virt, 0, ar_graph->oob_size);
	memcpy(ar_graph->oob_virt, data, size);
	dma_wmb();

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(0, APM_CMD_SET_CFG, 0,
					graph->port->id,
					APM_MODULE_INSTANCE_ID);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	cmd = (void *)pkt + GPR_HDR_SIZE;
	cmd->payload_address_lsw =
		lower_32_bits(ar_graph->oob_dsp_addr);
	cmd->payload_address_msw =
		upper_32_bits(ar_graph->oob_dsp_addr);
	cmd->mem_map_handle = ar_graph->oob_mem_map_handle;
	cmd->payload_size = size;

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}
EXPORT_SYMBOL_GPL(q6apm_send_graph_oob_config);

int q6apm_send_inband_config(struct audioreach_graph *graph,
			     const void *data, size_t size)
{
	void *payload;

	if (!data || !size)
		return -EINVAL;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	payload = (u8 *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	memcpy(payload, data, size);

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}
EXPORT_SYMBOL_GPL(q6apm_send_inband_config);

static struct audioreach_graph *
q6apm_get_audioreach_graph(struct q6apm *apm, struct device *dma_dev,
			   uint32_t graph_id)
{
	struct audioreach_graph_info *info;
	struct audioreach_graph *graph;
	int id;

	mutex_lock(&apm->lock);
	graph = idr_find(&apm->graph_idr, graph_id);
	mutex_unlock(&apm->lock);

	if (graph) {
		kref_get(&graph->refcount);
		return graph;
	}

	info = idr_find(&apm->graph_info_idr, graph_id);

	if (!info)
		return ERR_PTR(-ENODEV);

	graph = kzalloc_obj(*graph);
	if (!graph)
		return ERR_PTR(-ENOMEM);

	graph->apm = apm;
	graph->info = info;
	graph->id = graph_id;
	graph->dma_dev = dma_dev;
	mutex_init(&graph->protection_lock);

	graph->graph = audioreach_alloc_graph_pkt(apm, info);
	if (IS_ERR(graph->graph)) {
		void *err = graph->graph;

		kfree(graph);
		return ERR_CAST(err);
	}

	mutex_lock(&apm->lock);
	id = idr_alloc(&apm->graph_idr, graph, graph_id, graph_id + 1, GFP_KERNEL);
	if (id < 0) {
		dev_err(apm->dev, "Unable to allocate graph id (%d)\n", graph_id);
		kfree(graph->graph);
		kfree(graph);
		mutex_unlock(&apm->lock);
		return ERR_PTR(id);
	}
	mutex_unlock(&apm->lock);

	kref_init(&graph->refcount);

	if (audioreach_graph_has_protected_calibration(info)) {
		id = q6apm_map_position_buffer(graph);
		if (id)
			goto remove_graph;
	}

	id = q6apm_send_cmd_sync(apm, graph->graph, 0);
	if (id)
		goto unmap_position;
	if (audioreach_graph_has_protected_calibration(info)) {
		id = q6apm_map_oob_buffer(graph);
		if (id)
			goto close_graph;
		id = audioreach_send_protected_graph_calibration(graph);
		if (id)
			goto unmap_oob;
	}

	return graph;

unmap_oob:
	q6apm_unmap_oob_buffer(graph);
close_graph:
	audioreach_graph_mgmt_cmd(graph, APM_CMD_GRAPH_CLOSE);
unmap_position:
	q6apm_unmap_position_buffer(graph);
remove_graph:
	mutex_lock(&apm->lock);
	idr_remove(&apm->graph_idr, graph->id);
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

/*
 * The SP11 Windows transaction addresses run-state commands through the
 * graph's client port and lists the render graph before its two protection
 * subgraphs.  Keep this separate from the generic APM management path used
 * by existing AudioReach topologies.
 */
static int audioreach_graph_client_mgmt_cmd(struct q6apm_graph *graph,
					    uint32_t opcode)
{
	static const u32 sp11_sub_graph_order[] = {
		0xb0000001, 0xb000007f, 0xb000007e,
	};
	struct audioreach_graph_info *info = graph->info;
	struct apm_module_param_data *param_data;
	struct apm_graph_mgmt_cmd *mgmt_cmd;
	struct audioreach_sub_graph *sg;
	int payload_size;
	int i;

	if (!graph->pull_mode)
		return audioreach_graph_mgmt_cmd(graph->ar_graph, opcode);

	if (info->num_sub_graphs != ARRAY_SIZE(sp11_sub_graph_order))
		return -EINVAL;

	payload_size = APM_GRAPH_MGMT_PSIZE(mgmt_cmd,
					   ARRAY_SIZE(sp11_sub_graph_order));
	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, opcode, 0,
					graph->port->id,
					APM_MODULE_INSTANCE_ID);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	mgmt_cmd = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	mgmt_cmd->num_sub_graphs = ARRAY_SIZE(sp11_sub_graph_order);

	param_data = &mgmt_cmd->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_SUB_GRAPH_LIST;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;

	for (i = 0; i < ARRAY_SIZE(sp11_sub_graph_order); i++) {
		bool found = false;

		list_for_each_entry(sg, &info->sg_list, node) {
			if (sg->sub_graph_id != sp11_sub_graph_order[i])
				continue;
			mgmt_cmd->sub_graph_id_list[i] = sg->sub_graph_id;
			found = true;
			break;
		}
		if (!found)
			return -ENODEV;
	}

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

static void q6apm_put_audioreach_graph(struct kref *ref)
{
	struct audioreach_graph *graph;
	struct q6apm *apm;

	graph = container_of(ref, struct audioreach_graph, refcount);
	apm = graph->apm;

	q6apm_unmap_oob_buffer(graph);
	audioreach_graph_mgmt_cmd(graph, APM_CMD_GRAPH_CLOSE);
	q6apm_unmap_position_buffer(graph);

	mutex_lock(&apm->lock);
	graph = idr_remove(&apm->graph_idr, graph->id);
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
	if (g_apm)
		return q6apm_get_apm_state(g_apm);

	return false;
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

static struct audioreach_module *
__q6apm_find_module_by_iid(struct audioreach_graph_info *info, uint32_t iid)
{
	struct audioreach_container *container;
	struct audioreach_sub_graph *sg;
	struct audioreach_module *module;

	list_for_each_entry(sg, &info->sg_list, node)
		list_for_each_entry(container, &sg->container_list, node)
			list_for_each_entry(module, &container->modules_list, node)
				if (iid == module->instance_id)
					return module;

	return NULL;
}

bool q6apm_graph_is_pull_mode(struct device *dev, unsigned int graph_id)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph_info *info;

	if (!apm)
		return false;

	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info)
		return false;

	return __q6apm_find_module_by_mid(apm, info,
					 MODULE_ID_SH_MEM_PULL_MODE) != NULL;
}
EXPORT_SYMBOL_GPL(q6apm_graph_is_pull_mode);

int q6apm_graph_media_format_shmem(struct q6apm_graph *graph,
				   struct audioreach_module_config *cfg)
{
	struct audioreach_module *module;

	if (cfg->direction == SNDRV_PCM_STREAM_CAPTURE)
		module = q6apm_find_module_by_mid(graph, MODULE_ID_RD_SHARED_MEM_EP);
	else if (audioreach_graph_has_protected_calibration(graph->info))
		module = q6apm_find_module_by_mid(
			graph, MODULE_ID_SH_MEM_PULL_MODE);
	else
		module = q6apm_find_module_by_mid(graph, MODULE_ID_WR_SHARED_MEM_EP);

	if (!module)
		return -ENODEV;

	return audioreach_set_media_format(graph, module, cfg);

}
EXPORT_SYMBOL_GPL(q6apm_graph_media_format_shmem);

static int q6apm_register_module_event(struct q6apm_graph *graph, u32 iid,
					u32 event_id, const void *config,
					size_t config_size)
{
	struct apm_module_register_events *event;
	size_t payload_size = ALIGN(sizeof(*event) + config_size, 8);
	void *p;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size,
					APM_CMD_REGISTER_MODULE_EVENTS, 0,
					graph->port->id, iid);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (u8 *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	memset(p, 0, payload_size);
	event = p;
	event->module_instance_id = iid;
	event->event_id = event_id;
	event->is_register = 1;
	event->event_config_payload_size = config_size;
	if (config_size)
		memcpy(event + 1, config, config_size);

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

int q6apm_graph_configure_pull(struct q6apm_graph *graph,
			      struct audioreach_module_config *media_cfg,
			      phys_addr_t ring_addr, size_t ring_size,
			      size_t period_size)
{
	struct sh_mem_pull_push_mode_cfg *cfg;
	struct apm_module_param_data *param;
	struct audioreach_module *pull, *pause;
	struct {
		u32 num_water_mark_levels;
		u32 levels[2];
	} __packed watermarks;
	size_t payload_size;
	void *p;
	int ret;

	if (!graph->pull_mode || !graph->ar_graph ||
	    !graph->ar_graph->position_virt ||
	    !graph->ar_graph->position_mem_map_handle ||
	    !graph->info->mem_map_handle)
		return -ENODEV;
	if (ring_size != 3840 || period_size != 1920 ||
	    media_cfg->sample_rate != 48000 || media_cfg->bit_width != 16 ||
	    media_cfg->num_channels != 2)
		return -EINVAL;

	pull = __q6apm_find_module_by_iid(graph->info, 0x4660);
	pause = __q6apm_find_module_by_iid(graph->info, 0x466b);
	if (!pull || pull->module_id != MODULE_ID_SH_MEM_PULL_MODE ||
	    !pause)
		return -ENODEV;

	payload_size = ALIGN(APM_MODULE_PARAM_DATA_SIZE + sizeof(*cfg), 8);
	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0,
					graph->port->id, pull->instance_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (u8 *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	memset(p, 0, payload_size);
	param = p;
	param->module_instance_id = pull->instance_id;
	param->param_id = PARAM_ID_SH_MEM_PULL_PUSH_MODE_CFG;
	param->param_size = sizeof(*cfg);
	cfg = p + APM_MODULE_PARAM_DATA_SIZE;
	cfg->shared_circ_buf_addr_lsw = lower_32_bits(ring_addr);
	cfg->shared_circ_buf_addr_msw = upper_32_bits(ring_addr);
	cfg->shared_circ_buf_size = ring_size;
	cfg->circ_buf_mem_map_handle = graph->info->mem_map_handle;
	cfg->shared_pos_buf_addr_lsw =
		lower_32_bits(graph->ar_graph->position_dsp_addr);
	cfg->shared_pos_buf_addr_msw =
		upper_32_bits(graph->ar_graph->position_dsp_addr);
	cfg->pos_buf_mem_map_handle =
		graph->ar_graph->position_mem_map_handle;

	ret = audioreach_graph_send_cmd_sync(graph, pkt, 0);
	if (ret) {
		dev_err(graph->dev,
			"SP11 stage pull-ring-config iid %#x param %#x failed: %d\n",
			pull->instance_id, PARAM_ID_SH_MEM_PULL_PUSH_MODE_CFG,
			ret);
		return ret;
	}
	dev_info(graph->dev,
		 "SP11 stage pull-ring-config accepted: ring=%zu period=%zu\n",
		 ring_size, period_size);

	watermarks.num_water_mark_levels = 2;
	watermarks.levels[0] = period_size;
	watermarks.levels[1] = ring_size;
	ret = q6apm_register_module_event(
		graph, pull->instance_id,
		EVENT_ID_SH_MEM_PULL_PUSH_MODE_WATERMARK,
		&watermarks, sizeof(watermarks));
	if (ret)
		return ret;
	ret = q6apm_register_module_event(
		graph, pause->instance_id,
		EVENT_ID_SOFT_PAUSE_PAUSE_COMPLETE, NULL, 0);
	if (ret)
		return ret;
	ret = q6apm_register_module_event(
		graph, pause->instance_id,
		EVENT_ID_SOFT_PAUSE_RESUME_COMPLETE, NULL, 0);
	if (ret)
		return ret;
	dev_info(graph->dev,
		 "SP11 stages pull-watermarks and soft-pause events accepted\n");

	ret = audioreach_set_media_format(graph, pull, media_cfg);
	if (ret)
		dev_err(graph->dev,
			"SP11 stage pull-media-format iid %#x param %#x failed: %d\n",
			pull->instance_id, PARAM_ID_MEDIA_FORMAT, ret);
	else
		dev_info(graph->dev,
			 "SP11 stage pull-media-format accepted\n");
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_configure_pull);

int q6apm_map_memory_fixed_region(struct device *dev, unsigned int graph_id, phys_addr_t phys,
				  size_t sz)
{
	struct audioreach_graph_info *info;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct apm_shared_map_region_payload *mregions;
	struct apm_cmd_shared_mem_map_regions *cmd;
	int payload_size = sizeof(*cmd) + (sizeof(*mregions));
	uint32_t buf_sz;
	void *p;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size,
						APM_CMD_SHARED_MEM_MAP_REGIONS, graph_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info)
		return -ENODEV;

	if (info->mem_map_handle)
		return 0;

	/* DSP expects size should be aligned to 4K */
	buf_sz = ALIGN(sz, 4096);

	p = (void *)pkt + GPR_HDR_SIZE;
	cmd = p;
	cmd->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = 1;
	cmd->property_flag = 0x0;

	mregions = p + sizeof(*cmd);

	mregions->shm_addr_lsw = lower_32_bits(phys);
	mregions->shm_addr_msw = upper_32_bits(phys);
	mregions->mem_size_bytes = buf_sz;

	return q6apm_send_cmd_sync(apm, pkt, APM_CMD_RSP_SHARED_MEM_MAP_REGIONS);
}
EXPORT_SYMBOL_GPL(q6apm_map_memory_fixed_region);

int q6apm_alloc_fragments(struct q6apm_graph *graph, unsigned int dir, phys_addr_t phys,
				size_t period_sz, unsigned int periods)
{
	struct audioreach_graph_data *data;
	struct audio_buffer *buf;
	int cnt;

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

int q6apm_unmap_memory_fixed_region(struct device *dev, unsigned int graph_id)
{
	struct apm_cmd_shared_mem_unmap_regions *cmd;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph_info *info;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(sizeof(*cmd),
						APM_CMD_SHARED_MEM_UNMAP_REGIONS, graph_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info)
		return -ENODEV;

	if (!info->mem_map_handle)
		return 0;

	cmd = (void *)pkt + GPR_HDR_SIZE;
	cmd->mem_map_handle = info->mem_map_handle;

	return q6apm_send_cmd_sync(apm, pkt, APM_CMD_SHARED_MEM_UNMAP_REGIONS);
}
EXPORT_SYMBOL_GPL(q6apm_unmap_memory_fixed_region);

int q6apm_free_fragments(struct q6apm_graph *graph, unsigned int dir)
{
	audioreach_graph_free_buf(graph);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_free_fragments);

int q6apm_remove_initial_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_REMOVE_INITIAL_SILENCE, samples);
}
EXPORT_SYMBOL_GPL(q6apm_remove_initial_silence);

int q6apm_remove_trailing_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_REMOVE_TRAILING_SILENCE, samples);
}
EXPORT_SYMBOL_GPL(q6apm_remove_trailing_silence);

int q6apm_enable_compress_module(struct device *dev, struct q6apm_graph *graph, bool en)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_MODULE_ENABLE, en);
}
EXPORT_SYMBOL_GPL(q6apm_enable_compress_module);

int q6apm_set_real_module_id(struct device *dev, struct q6apm_graph *graph,
			     uint32_t codec_id)
{
	struct audioreach_module *module;
	uint32_t module_id;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
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
	struct audioreach_graph_info *info = graph->info;
	struct audioreach_sub_graph *sgs;
	struct audioreach_container *container;
	struct audioreach_module *module;
	int ret;

	if (audioreach_graph_has_protected_calibration(info)) {
		struct audioreach_module *pcm, *mfc;

		if (cfg->direction != SNDRV_PCM_STREAM_PLAYBACK ||
		    cfg->sample_rate != 48000 || cfg->bit_width != 16 ||
		    cfg->num_channels != 2)
			return -EINVAL;

		pcm = __q6apm_find_module_by_iid(info, 0x465f);
		mfc = __q6apm_find_module_by_iid(info, 0x466a);
		if (!pcm || pcm->module_id != MODULE_ID_PCM_CNV ||
		    !mfc || mfc->module_id != MODULE_ID_MFC)
			return -ENODEV;
		ret = audioreach_set_media_format(graph, pcm, cfg);
		if (ret)
			return ret;
		dev_info(graph->dev,
			 "SP11 stage PCM_CNV media format accepted\n");
		ret = audioreach_set_media_format(graph, mfc, cfg);
		if (ret)
			return ret;
		dev_info(graph->dev,
			 "SP11 stage MFC media format accepted\n");
		return audioreach_configure_protection(graph);
	}

	list_for_each_entry(sgs, &info->sg_list, node) {
		list_for_each_entry(container, &sgs->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if ((module->module_id == MODULE_ID_WR_SHARED_MEM_EP) ||
					(module->module_id == MODULE_ID_RD_SHARED_MEM_EP))
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

static int q6apm_graph_get_tx_shmem_module_iid(struct q6apm_graph *graph)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_RD_SHARED_MEM_EP);
	if (!module)
		return -ENODEV;

	return module->instance_id;

}

int q6apm_graph_get_rx_shmem_module_iid(struct q6apm_graph *graph)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_WR_SHARED_MEM_EP);
	if (!module && audioreach_graph_has_protected_calibration(graph->info))
		module = q6apm_find_module_by_mid(
			graph, MODULE_ID_SH_MEM_PULL_MODE);
	if (!module)
		return -ENODEV;

	return module->instance_id;

}
EXPORT_SYMBOL_GPL(q6apm_graph_get_rx_shmem_module_iid);

int q6apm_write_async(struct q6apm_graph *graph, uint32_t len, uint32_t msw_ts,
		      uint32_t lsw_ts, uint32_t wflags)
{
	struct apm_data_cmd_wr_sh_mem_ep_data_buffer_v2 *write_buffer;
	struct audio_buffer *ab;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_pkt(sizeof(*write_buffer),
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

	return gpr_send_port_pkt(graph->port, pkt);
}
EXPORT_SYMBOL_GPL(q6apm_write_async);

int q6apm_read(struct q6apm_graph *graph)
{
	struct data_cmd_rd_sh_mem_ep_data_buffer_v2 *read_buffer;
	struct audioreach_graph_data *port;
	struct audio_buffer *ab;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_pkt(sizeof(*read_buffer),
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

	return gpr_send_port_pkt(graph->port, pkt);
}
EXPORT_SYMBOL_GPL(q6apm_read);

int q6apm_get_hw_pointer(struct q6apm_graph *graph, int dir)
{
	struct audioreach_graph_data *data;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	return (int)atomic_read(&data->hw_ptr);
}
EXPORT_SYMBOL_GPL(q6apm_get_hw_pointer);

snd_pcm_uframes_t q6apm_get_pull_hw_pointer(struct q6apm_graph *graph,
					    struct snd_pcm_runtime *runtime)
{
	struct sh_mem_pull_push_mode_position_buffer *position;
	u32 before, after, index;
	int retries = 4;

	if (!graph->pull_mode || !graph->ar_graph ||
	    !graph->ar_graph->position_virt)
		return 0;

	position = graph->ar_graph->position_virt;
	do {
		before = READ_ONCE(position->frame_counter);
		dma_rmb();
		index = READ_ONCE(position->index);
		dma_rmb();
		after = READ_ONCE(position->frame_counter);
	} while ((!before || before != after) && --retries);

	if (!before || before != after ||
	    index >= frames_to_bytes(runtime, runtime->buffer_size))
		index = atomic_read(&graph->rx_data.hw_ptr);
	else
		atomic_set(&graph->rx_data.hw_ptr, index);

	return bytes_to_frames(runtime, index);
}
EXPORT_SYMBOL_GPL(q6apm_get_pull_hw_pointer);

static int graph_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	struct data_cmd_rsp_rd_sh_mem_ep_data_buffer_done_v2 *rd_done;
	struct data_cmd_rsp_wr_sh_mem_ep_data_buffer_done_v2 *done;
	const struct gpr_ibasic_rsp_result_t *result;
	struct q6apm_graph *graph = priv;
	const struct gpr_hdr *hdr = &data->hdr;
	struct device *dev = graph->dev;
	uint32_t client_event;
	phys_addr_t phys;
	size_t dump_len;
	int token;

	result = data->payload;

	switch (hdr->opcode) {
	case APM_CMD_RSP_GET_CFG:
		graph->result.opcode = hdr->opcode;
		if (data->payload_size < sizeof(u32)) {
			dev_err(dev,
				"SP11 GET_CFG response token=%#x is truncated: %d bytes\n",
				hdr->token, data->payload_size);
			graph->result.status = -EPROTO;
			wake_up(&graph->cmd_wait);
			break;
		}

		graph->result.status = *(const u32 *)data->payload;
		dump_len = min_t(size_t, data->payload_size, 512);
		dev_info(dev,
			 "SP11 GET_CFG response token=%#x bytes=%d status=%#x%s\n",
			 hdr->token, data->payload_size, graph->result.status,
			 data->payload_size > dump_len ? " (dump truncated)" : "");
		print_hex_dump(KERN_INFO, "sp11-getcfg: ", DUMP_PREFIX_OFFSET,
			       16, 1, data->payload, dump_len, false);
		wake_up(&graph->cmd_wait);
		break;
	case APM_EVENT_MODULE_TO_CLIENT: {
		const struct apm_module_event *event = data->payload;

		if (data->payload_size < sizeof(*event)) {
			dev_warn(dev, "SP11 truncated module event: %d bytes\n",
				 data->payload_size);
			break;
		}

		/*
		 * SP11 2026-08-01: log every module->client event.
		 *
		 * Upstream drops anything that is not the pull-mode watermark, so
		 * if the DSP reports speaker-protection activity nobody sees it.
		 * Windows registers ADSP callbacks (EVENT_ID_VI_CALIBRATION,
		 * EVENT_ID_SPv5_SPEAKER_DIAGNOSTICS per Ghidra of qcadcm8380.sys)
		 * and persists the resulting R0Cal/T0Cal to the registry. Linux has
		 * no equivalent return path, which is why protection can be shown
		 * CONFIGURED but never shown to be ACTING.
		 *
		 * The SPv5 diagnostics event id is NOT known: prior analysis found
		 * 0x0800138C belongs to instance 0x4ac1 in an unrelated graph, and
		 * 0x08001511 is used by the Windows driver directly via GSL and is
		 * absent from the CDDE. So do not guess an id. Log whatever
		 * arrives, with its source instance, and let evidence name it.
		 */
		if (event->event_id != EVENT_ID_SH_MEM_PULL_PUSH_MODE_WATERMARK)
			dev_info(graph->dev,
				 "SP11 module event: src %#x event %#x payload %u\n",
				 data->hdr.src_port, event->event_id,
				 event->event_payload_size);

		if (!graph->pull_mode)
			break;

		if (data->hdr.src_port == SP11_SOFT_PAUSE_IID && graph->cb) {
			if (event->event_id == EVENT_ID_SOFT_PAUSE_PAUSE_COMPLETE) {
				graph->cb(APM_CLIENT_EVENT_SOFT_PAUSE_COMPLETE, 0,
					  (u8 *)data->payload + sizeof(*event),
					  graph->priv);
				break;
			}
			if (event->event_id == EVENT_ID_SOFT_PAUSE_RESUME_COMPLETE) {
				graph->cb(APM_CLIENT_EVENT_SOFT_RESUME_COMPLETE, 0,
					  (u8 *)data->payload + sizeof(*event),
					  graph->priv);
				break;
			}
		}

		if (event->event_id != EVENT_ID_SH_MEM_PULL_PUSH_MODE_WATERMARK)
			break;
		if (graph->cb)
			graph->cb(APM_CLIENT_EVENT_PULL_WATERMARK, 0,
				  (u8 *)data->payload + sizeof(*event),
				  graph->priv);
		break;
	}
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
			graph->result.opcode = hdr->opcode;
			graph->result.status = done->status;
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
			graph->result.opcode = hdr->opcode;
			graph->result.status = rd_done->status;
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
		case APM_CMD_SET_CFG:
		case APM_CMD_GET_CFG:
		case APM_CMD_REGISTER_MODULE_EVENTS:
		case APM_CMD_GRAPH_START:
		case APM_CMD_GRAPH_STOP:
		case APM_CMD_GRAPH_FLUSH:
			graph->result.opcode = result->opcode;
			graph->result.status = result->status;
			if (result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n",
					result->status, result->opcode);
			wake_up(&graph->cmd_wait);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

struct q6apm_graph *q6apm_graph_open(struct device *dev, q6apm_cb cb,
				     void *priv, int graph_id, int dir)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph *ar_graph;
	struct q6apm_graph *graph;
	int ret;

	ar_graph = q6apm_get_audioreach_graph(apm, dev, graph_id);
	if (IS_ERR(ar_graph)) {
		dev_err(dev, "No graph found with id %d\n", graph_id);
		return ERR_CAST(ar_graph);
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
	graph->dev = dev;
	graph->pull_mode =
		dir == SNDRV_PCM_STREAM_PLAYBACK &&
		audioreach_graph_has_protected_calibration(graph->info);

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		graph->shm_iid = q6apm_graph_get_rx_shmem_module_iid(graph);
	else
		graph->shm_iid = q6apm_graph_get_tx_shmem_module_iid(graph);


	mutex_init(&graph->lock);
	init_waitqueue_head(&graph->cmd_wait);

	graph->port = gpr_alloc_port(apm->gdev, dev, graph_callback, graph);
	if (IS_ERR(graph->port)) {
		ret = PTR_ERR(graph->port);
		goto free_graph;
	}

	return graph;

free_graph:
	kfree(graph);
put_ar_graph:
	kref_put(&ar_graph->refcount, q6apm_put_audioreach_graph);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6apm_graph_open);

int q6apm_graph_close(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;

	graph->ar_graph = NULL;
	kref_put(&ar_graph->refcount, q6apm_put_audioreach_graph);
	gpr_free_port(graph->port);
	kfree(graph);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_graph_close);

int q6apm_graph_prepare(struct q6apm_graph *graph)
{
	if (graph->pull_mode)
		return 0;

	return audioreach_graph_mgmt_cmd(graph->ar_graph, APM_CMD_GRAPH_PREPARE);
}
EXPORT_SYMBOL_GPL(q6apm_graph_prepare);

int q6apm_graph_start(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	int ret = 0;

	if (ar_graph->start_count == 0) {
		ret = audioreach_graph_client_mgmt_cmd(graph,
						      APM_CMD_GRAPH_START);
		if (!ret && graph->pull_mode)
			dev_info(graph->dev,
				 "SP11 stage GRAPH_START accepted\n");
	}

	if (!ret)
		ar_graph->start_count++;

	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_start);

int q6apm_graph_stop(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;

	if (--ar_graph->start_count > 0)
		return 0;

	return audioreach_graph_client_mgmt_cmd(graph, APM_CMD_GRAPH_STOP);
}
EXPORT_SYMBOL_GPL(q6apm_graph_stop);

int q6apm_graph_flush(struct q6apm_graph *graph)
{
	return audioreach_graph_client_mgmt_cmd(graph, APM_CMD_GRAPH_FLUSH);
}
EXPORT_SYMBOL_GPL(q6apm_graph_flush);

/*
 * Reproduce qcadcm SetStreamPause() for the SP11 protected pull graph.
 * REV_0D tag 0x04010008 / key 0x01000021 resolves to a zero-length
 * parameter command on iid 0x466b: value 1 -> 0x0800102e (pause),
 * value 0 -> 0x0800102f (resume/release).  The Aug-10 common-GPR Windows
 * capture independently observed both exact SET_CFG headers.
 */
int q6apm_graph_sp11_soft_pause(struct q6apm_graph *graph, bool pause)
{
	struct audioreach_module *module;
	struct apm_module_param_data param = { 0 };

	if (!graph || !graph->pull_mode || !graph->ar_graph)
		return -EOPNOTSUPP;

	module = q6apm_find_module_by_mid(graph, SP11_MODULE_ID_SOFT_PAUSE);
	if (!module || module->instance_id != SP11_SOFT_PAUSE_IID)
		return -EOPNOTSUPP;

	param.module_instance_id = module->instance_id;
	param.param_id = pause ? SP11_PARAM_SOFT_PAUSE_PAUSE :
				 SP11_PARAM_SOFT_PAUSE_RESUME;
	param.param_size = 0;
	param.error_code = 0;

	return q6apm_send_inband_config(graph->ar_graph, &param, sizeof(param));
}
EXPORT_SYMBOL_GPL(q6apm_graph_sp11_soft_pause);

/*
 * SP11 MSIIR parameter injection control (added 2026-08-01).
 *
 * Mirrors what DolbyAPOvlldp150 does on Windows: a user-space processor
 * analyses the stream, computes MSIIR coefficients, and injects them into the
 * DSP module that is already in the graph. The DSP filters; user space only
 * decides the coefficients. Audio never leaves the DSP path, so speaker
 * protection still sees the processed signal exactly as it does on Windows.
 *
 * Write format (little-endian):
 *
 *     u32 instance_id
 *     u32 param_id
 *     u32 payload_size
 *     u8  payload[payload_size]
 *
 * Both instance_id and param_id are allowlisted in
 * audioreach_sp11_inject_module_param(). A write while no graph is running
 * will be rejected by the DSP, typically with -22, because MSIIR gates its
 * tuning parameters behind CAPI initialisation.
 */
#define SND_SOC_TLV_HDR		(2 * sizeof(u32))
#define SP11_INJECT_CTL_HDR	(3 * sizeof(u32))
#define SP11_INJECT_CTL_MAX	1152

#define SP11_VOLUME_TRANSACTION_NORMAL_SIZE	216
#define SP11_VOLUME_TRANSACTION_LARGE_SIZE	272
#define SP11_VOLUME_TRANSACTION_PAYLOAD_MAX	(2 * sizeof(u32) + \
						 SP11_VOLUME_TRANSACTION_LARGE_SIZE)
#define SP11_VOLUME_TRANSACTION_CONTROL_MAX	(SND_SOC_TLV_HDR + \
						 SP11_VOLUME_TRANSACTION_PAYLOAD_MAX)
#define SP11_GAINSTEP_IID			0x489e
#define SP11_VOLUME_ONLY_CONTROL_MAX		(SND_SOC_TLV_HDR + 2 * sizeof(u32))
#define SP11_ENDPOINT_MUTE_CONTROL_MAX		(SND_SOC_TLV_HDR + sizeof(u32))

static DEFINE_MUTEX(sp11_volume_transaction_lock);

static int q6apm_sp11_validate_gainstep_delta(const u8 *data, size_t size)
{
	static const u32 expected_param_ids[] = {
		0x08001020, 0x08001021, 0x08001022, 0x08001026,
	};
	static const u32 expected_param_sizes[] = { 28, 16, 0, 4 };
	size_t offset = 0;
	u32 coeff_size;
	int i;

	if (size == SP11_VOLUME_TRANSACTION_NORMAL_SIZE)
		coeff_size = 96;
	else if (size == SP11_VOLUME_TRANSACTION_LARGE_SIZE)
		coeff_size = 152;
	else
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(expected_param_ids); i++) {
		size_t frame_size, padding;
		u32 param_size;

		if (size - offset < APM_MODULE_PARAM_DATA_SIZE)
			return -EINVAL;
		if (get_unaligned_le32(data + offset) != SP11_GAINSTEP_IID ||
		    get_unaligned_le32(data + offset + sizeof(u32)) !=
			    expected_param_ids[i] ||
		    get_unaligned_le32(data + offset + 3 * sizeof(u32)) != 0)
			return -EPERM;

		param_size = get_unaligned_le32(data + offset + 2 * sizeof(u32));
		if (param_size != (i == 2 ? coeff_size : expected_param_sizes[i]))
			return -EINVAL;

		frame_size = ALIGN(APM_MODULE_PARAM_DATA_SIZE + param_size, 8);
		if (frame_size > size - offset)
			return -EINVAL;
		padding = frame_size - APM_MODULE_PARAM_DATA_SIZE - param_size;
		if (padding && memchr_inv(data + offset + APM_MODULE_PARAM_DATA_SIZE +
					  param_size, 0, padding))
			return -EINVAL;
		offset += frame_size;
	}

	return offset == size ? 0 : -EINVAL;
}

static struct audioreach_graph *
q6apm_sp11_get_running_protected_graph(struct q6apm *apm)
{
	struct audioreach_graph *graph;
	int id;

	mutex_lock(&apm->lock);
	idr_for_each_entry(&apm->graph_idr, graph, id) {
		if (!audioreach_graph_has_protected_calibration(graph->info) ||
		    READ_ONCE(graph->start_count) <= 0)
			continue;
		kref_get(&graph->refcount);
		mutex_unlock(&apm->lock);
		return graph;
	}
	mutex_unlock(&apm->lock);

	return ERR_PTR(-ENODEV);
}

static int q6apm_sp11_apply_volume_transaction(struct q6apm *apm, u32 left_q28,
					       u32 right_q28, const u8 *delta,
					       size_t delta_size)
{
	struct audioreach_graph *graph;
	int ret;

	ret = q6apm_sp11_validate_gainstep_delta(delta, delta_size);
	if (ret)
		return ret;

	graph = q6apm_sp11_get_running_protected_graph(apm);
	if (IS_ERR(graph))
		return PTR_ERR(graph);

	mutex_lock(&sp11_volume_transaction_lock);
	ret = audioreach_sp11_set_final_volume_q28(apm, left_q28, right_q28);
	if (!ret)
		ret = q6apm_send_oob_config(graph, delta, delta_size);
	mutex_unlock(&sp11_volume_transaction_lock);

	if (ret)
		dev_dbg(apm->dev,
			"SP11 volume transaction L %#x R %#x delta %zu -> %d\n",
			left_q28, right_q28, delta_size, ret);

	kref_put(&graph->refcount, q6apm_put_audioreach_graph);
	return ret;
}

static int
q6apm_sp11_volume_only_put(struct snd_kcontrol *kcontrol,
			   const unsigned int __user *bytes,
			   unsigned int size)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6apm *apm = dev_get_drvdata(c->dev);
	struct audioreach_graph *graph;
	u32 gain_q28[2];
	u32 left_q28, right_q28;
	int ret;

	if (size != SP11_VOLUME_ONLY_CONTROL_MAX)
		return -EINVAL;

	bytes += 2;
	if (copy_from_user(gain_q28, bytes, sizeof(gain_q28)))
		return -EFAULT;
	left_q28 = le32_to_cpu(gain_q28[0]);
	right_q28 = le32_to_cpu(gain_q28[1]);

	graph = q6apm_sp11_get_running_protected_graph(apm);
	if (IS_ERR(graph))
		return PTR_ERR(graph);

	mutex_lock(&sp11_volume_transaction_lock);
	ret = audioreach_sp11_set_final_volume_q28(apm, left_q28, right_q28);
	mutex_unlock(&sp11_volume_transaction_lock);

	if (ret)
		dev_dbg(apm->dev,
			"SP11 volume-only L %#x R %#x -> %d\n",
			left_q28, right_q28, ret);

	kref_put(&graph->refcount, q6apm_put_audioreach_graph);
	return ret;
}

static int
q6apm_sp11_endpoint_mute_put(struct snd_kcontrol *kcontrol,
				     const unsigned int __user *bytes,
				     unsigned int size)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6apm *apm = dev_get_drvdata(c->dev);
	struct audioreach_graph *graph;
	u32 mute;
	int ret;

	if (size != SP11_ENDPOINT_MUTE_CONTROL_MAX)
		return -EINVAL;

	bytes += 2;
	if (copy_from_user(&mute, bytes, sizeof(mute)))
		return -EFAULT;
	mute = le32_to_cpu(mute);
	if (mute > 1)
		return -EINVAL;

	graph = q6apm_sp11_get_running_protected_graph(apm);
	if (IS_ERR(graph))
		return PTR_ERR(graph);

	mutex_lock(&sp11_volume_transaction_lock);
	ret = audioreach_sp11_set_final_mute(apm, mute != 0);
	mutex_unlock(&sp11_volume_transaction_lock);

	if (ret)
		dev_dbg(apm->dev, "SP11 endpoint mute %u -> %d\n", mute, ret);

	kref_put(&graph->refcount, q6apm_put_audioreach_graph);
	return ret;
}

static int q6apm_sp11_inject_put(struct snd_kcontrol *kcontrol,
				 const unsigned int __user *bytes,
				 unsigned int size)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6apm *apm = dev_get_drvdata(c->dev);
	u32 hdr[3], iid, param_id, psize;
	void *payload;
	int ret;

	/*
	 * snd_soc_bytes_tlv_callback() hands us the whole TLV buffer, which
	 * begins with the two-word ALSA TLV header (type, length). Skip it;
	 * our own header starts after that.
	 */
	if (size < SND_SOC_TLV_HDR + SP11_INJECT_CTL_HDR ||
	    size > SND_SOC_TLV_HDR + SP11_INJECT_CTL_MAX)
		return -EINVAL;

	bytes += 2;
	size -= SND_SOC_TLV_HDR;

	if (copy_from_user(hdr, bytes, sizeof(hdr)))
		return -EFAULT;

	iid = le32_to_cpu(hdr[0]);
	param_id = le32_to_cpu(hdr[1]);
	psize = le32_to_cpu(hdr[2]);

	if (psize == 0 || psize > size - SP11_INJECT_CTL_HDR)
		return -EINVAL;

	payload = kzalloc(psize, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	if (copy_from_user(payload,
			   (const u8 __user *)bytes + SP11_INJECT_CTL_HDR,
			   psize)) {
		kfree(payload);
		return -EFAULT;
	}

	ret = audioreach_sp11_inject_module_param(apm, iid, param_id,
						  payload, psize);
	if (ret)
		dev_dbg(apm->dev,
			"SP11 inject iid %#x param %#x size %u -> %d\n",
			iid, param_id, psize, ret);

	kfree(payload);
	return ret;
}

static int
q6apm_sp11_volume_transaction_put(struct snd_kcontrol *kcontrol,
				  const unsigned int __user *bytes,
				  unsigned int size)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6apm *apm = dev_get_drvdata(c->dev);
	u8 data[SP11_VOLUME_TRANSACTION_PAYLOAD_MAX];
	size_t payload_size, gain_size, delta_size;
	u32 left_q28, right_q28;

	if (size < SND_SOC_TLV_HDR + sizeof(left_q28))
		return -EINVAL;
	payload_size = size - SND_SOC_TLV_HDR;

	/*
	 * Preserve the deployed one-Q28 ABI while adding the exact Windows master
	 * volume form: left Q28 + right Q28 + one GainStep delta.  The extended
	 * form permits the intermediate L=new/R=old body observed by KDNET.
	 */
	if (payload_size == sizeof(left_q28) + SP11_VOLUME_TRANSACTION_NORMAL_SIZE ||
	    payload_size == sizeof(left_q28) + SP11_VOLUME_TRANSACTION_LARGE_SIZE)
		gain_size = sizeof(left_q28);
	else if (payload_size == 2 * sizeof(left_q28) +
			 SP11_VOLUME_TRANSACTION_NORMAL_SIZE ||
		 payload_size == 2 * sizeof(left_q28) +
			 SP11_VOLUME_TRANSACTION_LARGE_SIZE)
		gain_size = 2 * sizeof(left_q28);
	else
		return -EINVAL;

	bytes += 2;
	if (copy_from_user(data, bytes, payload_size))
		return -EFAULT;
	left_q28 = get_unaligned_le32(data);
	right_q28 = gain_size == 2 * sizeof(left_q28) ?
		get_unaligned_le32(data + sizeof(left_q28)) : left_q28;
	delta_size = payload_size - gain_size;

	return q6apm_sp11_apply_volume_transaction(apm, left_q28, right_q28,
			data + gain_size, delta_size);
}

static const struct snd_kcontrol_new q6apm_sp11_controls[] = {
	SND_SOC_BYTES_TLV("SP11 MSIIR Inject", SP11_INJECT_CTL_MAX,
			  NULL, q6apm_sp11_inject_put),
	SND_SOC_BYTES_TLV("SP11 Windows Volume Transaction",
			  SP11_VOLUME_TRANSACTION_CONTROL_MAX, NULL,
			  q6apm_sp11_volume_transaction_put),
	SND_SOC_BYTES_TLV("SP11 Windows Volume Only",
			  SP11_VOLUME_ONLY_CONTROL_MAX, NULL,
			  q6apm_sp11_volume_only_put),
	SND_SOC_BYTES_TLV("SP11 Windows Endpoint Mute",
			  SP11_ENDPOINT_MUTE_CONTROL_MAX, NULL,
			  q6apm_sp11_endpoint_mute_put),
};

static int q6apm_audio_probe(struct snd_soc_component *component)
{
	int ret;

	ret = snd_soc_add_component_controls(component,
					     q6apm_sp11_controls,
					     ARRAY_SIZE(q6apm_sp11_controls));
	if (ret)
		return ret;

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
	apm->dev = dev;
	apm->gdev = gdev;
	init_waitqueue_head(&apm->wait);

	INIT_LIST_HEAD(&apm->widget_list);
	idr_init(&apm->graph_idr);
	idr_init(&apm->graph_info_idr);
	idr_init(&apm->sub_graphs_idr);
	idr_init(&apm->containers_idr);

	idr_init(&apm->modules_idr);

	g_apm = apm;

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

static void apm_remove(gpr_device_t *gdev)
{
	of_platform_depopulate(&gdev->dev);
	snd_soc_unregister_component(&gdev->dev);
}

struct audioreach_module *q6apm_find_module_by_mid(struct q6apm_graph *graph, uint32_t mid)
{
	struct audioreach_graph_info *info = graph->info;
	struct q6apm *apm = graph->apm;

	return __q6apm_find_module_by_mid(apm, info, mid);

}

int q6apm_graph_id_for_backend(struct device *dev, int backend_id)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph_info *info;
	struct audioreach_container *container;
	struct audioreach_sub_graph *sg;
	struct audioreach_module *module;
	int id;

	if (!apm)
		return -ENODEV;

	mutex_lock(&apm->lock);
	idr_for_each_entry(&apm->graph_info_idr, info, id) {
		list_for_each_entry(sg, &info->sg_list, node) {
			list_for_each_entry(container, &sg->container_list, node) {
				list_for_each_entry(module, &container->modules_list, node) {
					if (module->integrated_backend_id == backend_id) {
						mutex_unlock(&apm->lock);
						return info->id;
					}
				}
			}
		}
	}
	mutex_unlock(&apm->lock);

	return backend_id;
}
EXPORT_SYMBOL_GPL(q6apm_graph_id_for_backend);

static int apm_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	gpr_device_t *gdev = priv;
	struct audioreach_graph *graph;
	struct audioreach_graph_info *info;
	struct q6apm *apm = dev_get_drvdata(&gdev->dev);
	struct apm_cmd_rsp_shared_mem_map_regions *rsp;
	struct device *dev = &gdev->dev;
	struct gpr_ibasic_rsp_result_t *result;
	const struct gpr_hdr *hdr = &data->hdr;

	result = data->payload;

	switch (hdr->opcode) {
	case APM_CMD_RSP_GET_SPF_STATE:
		apm->result.opcode = hdr->opcode;
		apm->result.status = 0;
		/* First word of result it state */
		apm->state = result->opcode;
		wake_up(&apm->wait);
		break;
	case GPR_BASIC_RSP_RESULT:
		switch (result->opcode) {
		case APM_CMD_SHARED_MEM_MAP_REGIONS:
		case APM_CMD_GRAPH_START:
		case APM_CMD_GRAPH_OPEN:
		case APM_CMD_GRAPH_PREPARE:
		case APM_CMD_GRAPH_CLOSE:
		case APM_CMD_GRAPH_FLUSH:
		case APM_CMD_GRAPH_STOP:
		case APM_CMD_SET_CFG:
			apm->result.opcode = result->opcode;
			apm->result.status = result->status;
			if (result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n", result->status,
					result->opcode);
			wake_up(&apm->wait);
			break;
		case APM_CMD_SHARED_MEM_UNMAP_REGIONS:
			apm->result.opcode = result->opcode;
			apm->result.status = 0;
			rsp = data->payload;

			if (hdr->token & Q6APM_OOB_MAP_TOKEN) {
				graph = idr_find(
					&apm->graph_idr,
					hdr->token & ~Q6APM_OOB_MAP_TOKEN);
				if (graph)
					graph->oob_mem_map_handle = 0;
			} else if (hdr->token & Q6APM_POSITION_MAP_TOKEN) {
				graph = idr_find(
					&apm->graph_idr,
					hdr->token & ~Q6APM_POSITION_MAP_TOKEN);
				if (graph)
					graph->position_mem_map_handle = 0;
			} else {
				info = idr_find(&apm->graph_info_idr,
						hdr->token);
				if (info)
					info->mem_map_handle = 0;
			}
			if (!(hdr->token &
			      (Q6APM_OOB_MAP_TOKEN | Q6APM_POSITION_MAP_TOKEN)) &&
			    !info)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n", result->status,
					result->opcode);

			wake_up(&apm->wait);
			break;
		default:
			break;
		}
		break;
	case APM_CMD_RSP_SHARED_MEM_MAP_REGIONS:
		apm->result.opcode = hdr->opcode;
		apm->result.status = 0;
		rsp = data->payload;

		if (hdr->token & Q6APM_OOB_MAP_TOKEN) {
			graph = idr_find(&apm->graph_idr,
					 hdr->token & ~Q6APM_OOB_MAP_TOKEN);
			if (graph)
				graph->oob_mem_map_handle = rsp->mem_map_handle;
		} else if (hdr->token & Q6APM_POSITION_MAP_TOKEN) {
			graph = idr_find(
				&apm->graph_idr,
				hdr->token & ~Q6APM_POSITION_MAP_TOKEN);
			if (graph)
				graph->position_mem_map_handle =
					rsp->mem_map_handle;
		} else {
			info = idr_find(&apm->graph_info_idr, hdr->token);
			if (info)
				info->mem_map_handle = rsp->mem_map_handle;
		}
		if (!(hdr->token &
		      (Q6APM_OOB_MAP_TOKEN | Q6APM_POSITION_MAP_TOKEN)) &&
		    !info)
			dev_err(dev, "Error (%d) Processing 0x%08x cmd\n", result->status,
				result->opcode);

		wake_up(&apm->wait);
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
