// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/soc/qcom/apr.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <dt-bindings/soc/qcom,gpr.h>
#include "q6apm.h"
#include "audioreach.h"

#define AUDIOREACH_DSP_EUNSUPPORTED	3

/* SubGraph Config */
struct apm_sub_graph_data {
	struct apm_sub_graph_cfg sub_graph_cfg;
	struct apm_prop_data perf_data;
	struct apm_sg_prop_id_perf_mode perf;
	struct apm_prop_data dir_data;
	struct apm_sg_prop_id_direction dir;
	struct apm_prop_data sid_data;
	struct apm_sg_prop_id_scenario_id sid;

} __packed;

#define APM_SUB_GRAPH_CFG_NPROP	3

struct apm_sub_graph_params  {
	struct apm_module_param_data param_data;
	uint32_t num_sub_graphs;
	struct apm_sub_graph_data sg_cfg[];
} __packed;

#define APM_SUB_GRAPH_PSIZE(p, n) ALIGN(struct_size(p, sg_cfg, n), 8)

/* container config */
struct apm_container_obj  {
	struct apm_container_cfg container_cfg;
	/* Capability ID list */
	struct apm_prop_data cap_data;
	uint32_t num_capability_id;
	uint32_t capability_id;

	/* Container graph Position */
	struct apm_prop_data pos_data;
	struct apm_cont_prop_id_graph_pos pos;

	/* Container Stack size */
	struct apm_prop_data stack_data;
	struct apm_cont_prop_id_stack_size stack;

	/* Container proc domain id */
	struct apm_prop_data domain_data;
	struct apm_cont_prop_id_domain domain;
} __packed;

struct apm_container_extended_obj {
	struct apm_container_obj common;

	struct apm_prop_data parent_data;
	struct apm_cont_prop_id_parent_container parent;

	struct apm_prop_data heap_data;
	struct apm_cont_prop_id_headp_id heap;
} __packed;

struct apm_container_params  {
	struct apm_module_param_data param_data;
	uint32_t num_containers;
	u8 cont_obj[];
} __packed;

/* Module List config */
struct apm_mod_list_obj {
	/* Modules list cfg */
	uint32_t sub_graph_id;
	uint32_t container_id;
	uint32_t num_modules;
	struct apm_module_obj mod_cfg[];
} __packed;

#define APM_MOD_LIST_OBJ_PSIZE(p, n) struct_size(p, mod_cfg, n)

struct apm_module_list_params {
	struct apm_module_param_data param_data;
	uint32_t num_modules_list;
	/* Module list config array */
	struct apm_mod_list_obj mod_list_obj[];
} __packed;


/* Module Properties */
struct apm_mod_prop_obj {
	u32 instance_id;
	u32 num_props;
	struct apm_prop_data prop_data_1;
	struct apm_module_prop_id_port_info prop_id_port;
} __packed;

struct apm_prop_list_params {
	struct apm_module_param_data param_data;
	u32 num_modules_prop_cfg;
	struct apm_mod_prop_obj mod_prop_obj[];

} __packed;

#define APM_MOD_PROP_PSIZE(p, n) ALIGN(struct_size(p, mod_prop_obj, n), 8)

/* Module Connections */
struct apm_mod_conn_list_params {
	struct apm_module_param_data param_data;
	u32 num_connections;
	struct apm_module_conn_obj conn_obj[];

} __packed;

#define APM_MOD_CONN_PSIZE(p, n) ALIGN(struct_size(p, conn_obj, n), 8)

struct apm_mod_ctrl_link_list_params {
	struct apm_module_param_data param_data;
	u32 num_ctrl_links;
	u8 ctrl_link_cfg[];
} __packed;

#define APM_MOD_CTRL_LINK_PSIZE(p, n) \
	ALIGN(struct_size(p, ctrl_link_cfg, n), 8)

struct apm_graph_open_params {
	struct apm_cmd_header *cmd_header;
	struct apm_sub_graph_params *sg_data;
	struct apm_container_params *cont_data;
	struct apm_module_list_params *mod_list_data;
	struct apm_prop_list_params *mod_prop_data;
	struct apm_mod_conn_list_params *mod_conn_list_data;
	struct apm_mod_ctrl_link_list_params *mod_ctrl_link_data;
} __packed;

struct apm_pcm_module_media_fmt_cmd {
	struct apm_module_param_data param_data;
	struct param_id_pcm_output_format_cfg header;
	struct payload_pcm_output_format_cfg media_cfg;
} __packed;

struct apm_rd_shmem_module_config_cmd {
	struct apm_module_param_data param_data;
	struct param_id_rd_sh_mem_cfg cfg;
} __packed;

struct apm_sh_module_media_fmt_cmd {
	struct media_format header;
	struct payload_media_fmt_pcm cfg;
} __packed;

#define APM_SHMEM_FMT_CFG_PSIZE(ch) ALIGN( \
				sizeof(struct apm_sh_module_media_fmt_cmd) + \
				ch * sizeof(uint8_t), 8)

/* num of channels as argument */
#define APM_PCM_MODULE_FMT_CMD_PSIZE(ch) ALIGN( \
				sizeof(struct apm_pcm_module_media_fmt_cmd) + \
				ch * sizeof(uint8_t), 8)

#define APM_PCM_OUT_FMT_CFG_PSIZE(p, n) ALIGN(struct_size(p, channel_mapping, n), 4)

struct apm_i2s_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_i2s_intf_cfg cfg;
} __packed;

#define APM_I2S_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_i2s_module_intf_cfg), 8)

struct apm_module_hw_ep_mf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_ep_mf mf;
} __packed;

#define APM_HW_EP_CFG_PSIZE ALIGN(sizeof(struct apm_module_hw_ep_mf_cfg), 8)

#define APM_MFC_CFG_PSIZE(p, n) ALIGN(struct_size(p, channel_mapping, n), 4)

struct apm_module_frame_size_factor_cfg {
	struct apm_module_param_data param_data;
	uint32_t frame_size_factor;
} __packed;

#define APM_FS_CFG_PSIZE ALIGN(sizeof(struct apm_module_frame_size_factor_cfg), 8)

struct apm_module_hw_ep_power_mode_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_ep_power_mode_cfg power_mode;
} __packed;

#define APM_HW_EP_PMODE_CFG_PSIZE ALIGN(sizeof(struct apm_module_hw_ep_power_mode_cfg),	8)

struct apm_module_hw_ep_dma_data_align_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_ep_dma_data_align align;
} __packed;

#define APM_HW_EP_DALIGN_CFG_PSIZE ALIGN(sizeof(struct apm_module_hw_ep_dma_data_align_cfg), 8)

struct apm_gain_module_cfg {
	struct apm_module_param_data param_data;
	struct param_id_gain_cfg gain_cfg;
} __packed;

#define APM_GAIN_CFG_PSIZE ALIGN(sizeof(struct apm_gain_module_cfg), 8)

struct apm_codec_dma_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_codec_dma_intf_cfg cfg;
} __packed;

#define APM_CDMA_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_codec_dma_module_intf_cfg), 8)

struct apm_display_port_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_display_port_intf_cfg cfg;
} __packed;
#define APM_DP_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_display_port_module_intf_cfg), 8)

struct apm_module_sp_vi_op_mode_cfg {
	struct apm_module_param_data param_data;
	struct param_id_sp_vi_op_mode_cfg cfg;
} __packed;

#define APM_SP_VI_OP_MODE_CFG_PSIZE(ch) ALIGN( \
				sizeof(struct apm_module_sp_vi_op_mode_cfg) + \
				(ch) * sizeof(uint32_t), 8)

struct apm_module_sp_vi_ex_mode_cfg {
	struct apm_module_param_data param_data;
	struct param_id_sp_vi_ex_mode_cfg cfg;
} __packed;

#define APM_SP_VI_EX_MODE_CFG_PSIZE ALIGN(sizeof(struct apm_module_sp_vi_ex_mode_cfg), 8)

struct apm_module_sp_vi_channel_map_cfg {
	struct apm_module_param_data param_data;
	struct param_id_sp_vi_channel_map_cfg cfg;
} __packed;

#define APM_SP_VI_CH_MAP_CFG_PSIZE(ch) ALIGN( \
				sizeof(struct apm_module_sp_vi_channel_map_cfg) + \
				(ch) * sizeof(uint32_t), 8)

static void *__audioreach_alloc_pkt(int payload_size, uint32_t opcode, uint32_t token,
				    uint32_t src_port, uint32_t dest_port, bool has_cmd_hdr)
{
	struct gpr_pkt *pkt;
	void *p;
	int pkt_size = GPR_HDR_SIZE + payload_size;

	if (has_cmd_hdr)
		pkt_size += APM_CMD_HDR_SIZE;

	p = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	pkt = p;
	pkt->hdr.version = GPR_PKT_VER;
	pkt->hdr.hdr_size = GPR_PKT_HEADER_WORD_SIZE;
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.dest_port = dest_port;
	pkt->hdr.src_port = src_port;

	pkt->hdr.dest_domain = GPR_DOMAIN_ID_ADSP;
	pkt->hdr.src_domain = GPR_DOMAIN_ID_APPS;
	pkt->hdr.token = token;
	pkt->hdr.opcode = opcode;

	if (has_cmd_hdr) {
		struct apm_cmd_header *cmd_header;

		p = p + GPR_HDR_SIZE;
		cmd_header = p;
		cmd_header->payload_size = payload_size;
	}

	return pkt;
}

void *audioreach_alloc_pkt(int payload_size, uint32_t opcode, uint32_t token,
			   uint32_t src_port, uint32_t dest_port)
{
	return __audioreach_alloc_pkt(payload_size, opcode, token, src_port, dest_port, false);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_pkt);

void *audioreach_alloc_apm_pkt(int pkt_size, uint32_t opcode, uint32_t token, uint32_t src_port)
{
	return __audioreach_alloc_pkt(pkt_size, opcode, token, src_port, APM_MODULE_INSTANCE_ID,
				      false);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_apm_pkt);

void *audioreach_alloc_cmd_pkt(int payload_size, uint32_t opcode, uint32_t token,
			       uint32_t src_port, uint32_t dest_port)
{
	return __audioreach_alloc_pkt(payload_size, opcode, token, src_port, dest_port, true);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_cmd_pkt);

void *audioreach_alloc_apm_cmd_pkt(int pkt_size, uint32_t opcode, uint32_t token)
{
	return __audioreach_alloc_pkt(pkt_size, opcode, token, GPR_APM_MODULE_IID,
				       APM_MODULE_INSTANCE_ID, true);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_apm_cmd_pkt);

void audioreach_set_default_channel_mapping(u8 *ch_map, int num_channels)
{
	if (num_channels == 1) {
		ch_map[0] =  PCM_CHANNEL_FL;
	} else if (num_channels == 2) {
		ch_map[0] =  PCM_CHANNEL_FL;
		ch_map[1] =  PCM_CHANNEL_FR;
	} else if (num_channels == 4) {
		ch_map[0] =  PCM_CHANNEL_FL;
		ch_map[1] =  PCM_CHANNEL_FR;
		ch_map[2] =  PCM_CHANNEL_LS;
		ch_map[3] =  PCM_CHANNEL_RS;
	}
}
EXPORT_SYMBOL_GPL(audioreach_set_default_channel_mapping);

static size_t apm_container_obj_size(const struct audioreach_container *cont)
{
	return cont->has_extended_properties ?
		sizeof(struct apm_container_extended_obj) :
		sizeof(struct apm_container_obj);
}

static void apm_populate_container_config(void *data,
					  const struct audioreach_container *cont)
{
	struct apm_container_obj *cfg = data;

	/* Container Config */
	cfg->container_cfg.container_id = cont->container_id;
	cfg->container_cfg.num_prop = cont->has_extended_properties ? 6 : 4;

	/* Capability list */
	cfg->cap_data.prop_id = APM_CONTAINER_PROP_ID_CAPABILITY_LIST;
	cfg->cap_data.prop_size = APM_CONTAINER_PROP_ID_CAPABILITY_SIZE;
	cfg->num_capability_id = 1;
	cfg->capability_id = cont->capability_id;

	/* Graph Position */
	cfg->pos_data.prop_id = APM_CONTAINER_PROP_ID_GRAPH_POS;
	cfg->pos_data.prop_size = sizeof(struct apm_cont_prop_id_graph_pos);
	cfg->pos.graph_pos = cont->graph_pos;

	/* Stack size */
	cfg->stack_data.prop_id = APM_CONTAINER_PROP_ID_STACK_SIZE;
	cfg->stack_data.prop_size = sizeof(struct apm_cont_prop_id_stack_size);
	cfg->stack.stack_size = cont->stack_size;

	/* Proc domain */
	cfg->domain_data.prop_id = APM_CONTAINER_PROP_ID_PROC_DOMAIN;
	cfg->domain_data.prop_size = sizeof(struct apm_cont_prop_id_domain);
	cfg->domain.proc_domain = cont->proc_domain;

	if (cont->has_extended_properties) {
		struct apm_container_extended_obj *ext = data;

		ext->parent_data.prop_id =
			APM_CONTAINER_PROP_ID_PARENT_CONTAINER_ID;
		ext->parent_data.prop_size =
			sizeof(struct apm_cont_prop_id_parent_container);
		ext->parent.parent_container_id = cont->parent_container_id;

		ext->heap_data.prop_id = APM_CONTAINER_PROP_ID_HEAP_ID;
		ext->heap_data.prop_size =
			sizeof(struct apm_cont_prop_id_headp_id);
		ext->heap.heap_id = cont->heap_id;
	}
}

static void apm_populate_sub_graph_config(struct apm_sub_graph_data *cfg,
					  const struct audioreach_sub_graph *sg)
{
	cfg->sub_graph_cfg.sub_graph_id = sg->sub_graph_id;
	cfg->sub_graph_cfg.num_sub_graph_prop = APM_SUB_GRAPH_CFG_NPROP;

	/* Perf Mode */
	cfg->perf_data.prop_id = APM_SUB_GRAPH_PROP_ID_PERF_MODE;
	cfg->perf_data.prop_size = APM_SG_PROP_ID_PERF_MODE_SIZE;
	cfg->perf.perf_mode = sg->perf_mode;

	/* Direction */
	cfg->dir_data.prop_id = APM_SUB_GRAPH_PROP_ID_DIRECTION;
	cfg->dir_data.prop_size = APM_SG_PROP_ID_DIR_SIZE;
	cfg->dir.direction = sg->direction;

	/* Scenario ID */
	cfg->sid_data.prop_id = APM_SUB_GRAPH_PROP_ID_SCENARIO_ID;
	cfg->sid_data.prop_size = APM_SG_PROP_ID_SID_SIZE;
	cfg->sid.scenario_id = sg->scenario_id;
}

static void apm_populate_module_prop_obj(struct apm_mod_prop_obj *obj,
					 const struct audioreach_module *module)
{

	obj->instance_id = module->instance_id;
	obj->num_props = 1;
	obj->prop_data_1.prop_id = APM_MODULE_PROP_ID_PORT_INFO;
	obj->prop_data_1.prop_size = APM_MODULE_PROP_ID_PORT_INFO_SZ;
	obj->prop_id_port.max_ip_port = module->max_ip_port;
	obj->prop_id_port.max_op_port = module->max_op_port;
}

static void apm_populate_module_list_obj(struct apm_mod_list_obj *obj,
					 const struct audioreach_container *container,
					 int sub_graph_id)
{
	struct audioreach_module *module;
	int i;

	obj->sub_graph_id = sub_graph_id;
	obj->container_id = container->container_id;
	obj->num_modules = container->num_modules;
	i = 0;
	list_for_each_entry(module, &container->modules_list, node) {
		obj->mod_cfg[i].module_id = module->module_id;
		obj->mod_cfg[i].instance_id = module->instance_id;
		i++;
	}
}

static void audioreach_populate_graph(struct q6apm *apm,
				      const struct audioreach_graph_info *info,
				      struct apm_graph_open_params *open,
				      const struct list_head *sg_list,
				      int num_sub_graphs)
{
	struct apm_mod_conn_list_params *mc_data = open->mod_conn_list_data;
	struct apm_module_list_params *ml_data = open->mod_list_data;
	struct apm_mod_ctrl_link_list_params *cl_data = open->mod_ctrl_link_data;
	u8 *ctrl_link_cfg = cl_data ? cl_data->ctrl_link_cfg : NULL;
	struct apm_prop_list_params *mp_data = open->mod_prop_data;
	struct apm_container_params *c_data = open->cont_data;
	struct apm_sub_graph_params *sg_data = open->sg_data;
	int ncontainer = 0, nmodule = 0, nconn = 0;
	struct apm_mod_prop_obj *module_prop_obj;
	struct audioreach_container *container;
	struct apm_module_conn_obj *conn_obj;
	struct audioreach_module *module;
	struct audioreach_sub_graph *sg;
	u8 *cobj = c_data->cont_obj;
	struct apm_mod_list_obj *mlobj;
	int i = 0;

	mlobj = &ml_data->mod_list_obj[0];


	if (!info->internal_vmixer_connection &&
	    info->dst_mod_inst_id && info->src_mod_inst_id) {
		conn_obj = &mc_data->conn_obj[nconn];
		conn_obj->src_mod_inst_id = info->src_mod_inst_id;
		conn_obj->src_mod_op_port_id = info->src_mod_op_port_id;
		conn_obj->dst_mod_inst_id = info->dst_mod_inst_id;
		conn_obj->dst_mod_ip_port_id = info->dst_mod_ip_port_id;
		nconn++;
	}

	list_for_each_entry(sg, sg_list, node) {
		struct apm_sub_graph_data *sg_cfg = &sg_data->sg_cfg[i++];

		apm_populate_sub_graph_config(sg_cfg, sg);

		list_for_each_entry(container, &sg->container_list, node) {
			apm_populate_container_config(cobj, container);
			apm_populate_module_list_obj(mlobj, container, sg->sub_graph_id);

			list_for_each_entry(module, &container->modules_list, node) {
				int pn;

				module_prop_obj = &mp_data->mod_prop_obj[nmodule++];
				apm_populate_module_prop_obj(module_prop_obj, module);

				if (!module->max_op_port)
					continue;

				for (pn = 0; pn < module->max_op_port; pn++) {
					if (module->dst_mod_inst_id[pn]) {
						conn_obj = &mc_data->conn_obj[nconn];
						conn_obj->src_mod_inst_id = module->instance_id;
						conn_obj->src_mod_op_port_id =
								module->src_mod_op_port_id[pn];
						conn_obj->dst_mod_inst_id =
								module->dst_mod_inst_id[pn];
						conn_obj->dst_mod_ip_port_id =
								module->dst_mod_ip_port_id[pn];
						nconn++;
					}
				}

				if (module->ctrl_link_data) {
					u32 size = le32_to_cpu(module->ctrl_link_data->size);

					/*
					 * GRAPH_OPEN has one aggregate count, so omit each
					 * topology block's leading num_ctrl_link_cfg.
					 */
					if (size > sizeof(u32)) {
						memcpy(ctrl_link_cfg,
						       (u8 *)module->ctrl_link_data->data + 4,
						       size - sizeof(u32));
						ctrl_link_cfg += size - sizeof(u32);
					}
				}
			}
			mlobj = (void *) mlobj + APM_MOD_LIST_OBJ_PSIZE(mlobj,
									container->num_modules);
			cobj += apm_container_obj_size(container);

			ncontainer++;
		}
	}
}

void *audioreach_alloc_graph_pkt(struct q6apm *apm,
				 const struct audioreach_graph_info *info)
{
	int payload_size, sg_sz, cont_sz, ml_sz, mp_sz, mc_sz, cl_sz = 0;
	struct apm_module_param_data  *param_data;
	struct audioreach_container *container;
	struct apm_sub_graph_params *sg_params;
	struct apm_mod_conn_list_params *mcon;
	struct apm_graph_open_params params;
	struct apm_prop_list_params *mprop;
	struct apm_mod_ctrl_link_list_params *mctrl;
	struct audioreach_module *module;
	struct audioreach_sub_graph *sgs;
	struct apm_mod_list_obj *mlobj;
	const struct list_head *sg_list;
	int num_connections = 0;
	int num_containers = 0;
	int num_sub_graphs = 0;
	int num_ctrl_links = 0;
	int ctrl_link_cfg_size = 0;
	int num_modules = 0;
	int num_modules_list;
	struct gpr_pkt *pkt;
	void *p;

	sg_list = &info->sg_list;
	ml_sz = 0;
	cont_sz = sizeof(struct apm_container_params);

	/* add FE-BE connections */
	if (!info->internal_vmixer_connection &&
	    info->dst_mod_inst_id && info->src_mod_inst_id)
		num_connections++;

	list_for_each_entry(sgs, sg_list, node) {
		num_sub_graphs++;
		list_for_each_entry(container, &sgs->container_list, node) {
			num_containers++;
			num_modules += container->num_modules;
			cont_sz += apm_container_obj_size(container);
			ml_sz = ml_sz + sizeof(struct apm_module_list_params) +
				APM_MOD_LIST_OBJ_PSIZE(mlobj, container->num_modules);

			list_for_each_entry(module, &container->modules_list, node) {
				num_connections += module->num_connections;
				if (module->ctrl_link_data) {
					u32 size = le32_to_cpu(module->ctrl_link_data->size);

					num_ctrl_links +=
						get_unaligned_le32(module->ctrl_link_data->data);
					ctrl_link_cfg_size += size - sizeof(u32);
				}
			}
		}
	}

	num_modules_list = num_containers;
	sg_sz = APM_SUB_GRAPH_PSIZE(sg_params, num_sub_graphs);
	cont_sz = ALIGN(cont_sz, 8);

	ml_sz = ALIGN(ml_sz, 8);

	mp_sz = APM_MOD_PROP_PSIZE(mprop, num_modules);
	mc_sz =	APM_MOD_CONN_PSIZE(mcon, num_connections);
	if (num_ctrl_links)
		cl_sz = APM_MOD_CTRL_LINK_PSIZE(mctrl, ctrl_link_cfg_size);

	payload_size = sg_sz + cont_sz + ml_sz + mp_sz + mc_sz + cl_sz;
	pkt = audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_GRAPH_OPEN, 0);
	if (IS_ERR(pkt))
		return pkt;

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	/* SubGraph */
	params.sg_data = p;
	param_data = &params.sg_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_SUB_GRAPH_CONFIG;
	param_data->param_size = sg_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.sg_data->num_sub_graphs = num_sub_graphs;
	p += sg_sz;

	/* Container */
	params.cont_data = p;
	param_data = &params.cont_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_CONTAINER_CONFIG;
	param_data->param_size = cont_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.cont_data->num_containers = num_containers;
	p += cont_sz;

	/* Module List*/
	params.mod_list_data = p;
	param_data = &params.mod_list_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_MODULE_LIST;
	param_data->param_size = ml_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.mod_list_data->num_modules_list = num_modules_list;
	p += ml_sz;

	/* Module Properties */
	params.mod_prop_data = p;
	param_data = &params.mod_prop_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_MODULE_PROP;
	param_data->param_size = mp_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.mod_prop_data->num_modules_prop_cfg = num_modules;
	p += mp_sz;

	/* Module Connections */
	params.mod_conn_list_data = p;
	param_data = &params.mod_conn_list_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_MODULE_CONN;
	param_data->param_size = mc_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.mod_conn_list_data->num_connections = num_connections;
	p += mc_sz;

	/* Module Control Links */
	if (num_ctrl_links) {
		params.mod_ctrl_link_data = p;
		param_data = &params.mod_ctrl_link_data->param_data;
		param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
		param_data->param_id = APM_PARAM_ID_MODULE_CTRL_LINK_CFG;
		/*
		 * Keep alignment bytes outside param_size.  The DSP payload is
		 * the exact aggregate count followed by the topology bodies.
		 */
		param_data->param_size = sizeof(u32) + ctrl_link_cfg_size;
		params.mod_ctrl_link_data->num_ctrl_links = num_ctrl_links;
		p += cl_sz;
	} else {
		params.mod_ctrl_link_data = NULL;
	}

	audioreach_populate_graph(apm, info, &params, sg_list, num_sub_graphs);

	return pkt;
}
EXPORT_SYMBOL_GPL(audioreach_alloc_graph_pkt);

int audioreach_send_cmd_sync(struct device *dev, gpr_device_t *gdev,
			     struct gpr_ibasic_rsp_result_t *result, struct mutex *cmd_lock,
			     gpr_port_t *port, wait_queue_head_t *cmd_wait,
			     const struct gpr_pkt *pkt, uint32_t rsp_opcode)
{

	const struct gpr_hdr *hdr = &pkt->hdr;
	int rc;

	mutex_lock(cmd_lock);
	result->opcode = 0;
	result->status = 0;

	if (port)
		rc = gpr_send_port_pkt(port, pkt);
	else if (gdev)
		rc = gpr_send_pkt(gdev, pkt);
	else
		rc = -EINVAL;

	if (rc < 0)
		goto err;

	if (rsp_opcode)
		rc = wait_event_timeout(*cmd_wait, (result->opcode == hdr->opcode) ||
					(result->opcode == rsp_opcode),	5 * HZ);
	else
		rc = wait_event_timeout(*cmd_wait, (result->opcode == hdr->opcode), 5 * HZ);

	if (!rc) {
		dev_err(dev, "CMD timeout for [%x] opcode\n", hdr->opcode);
		rc = -ETIMEDOUT;
	} else if (result->status > 0) {
		dev_err(dev, "DSP returned error[%x] %x\n", hdr->opcode, result->status);
		if (result->status == AUDIOREACH_DSP_EUNSUPPORTED)
			rc = -EOPNOTSUPP;
		else
			rc = -EINVAL;
	} else {
		/* DSP successfully finished the command */
		rc = 0;
	}

err:
	mutex_unlock(cmd_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(audioreach_send_cmd_sync);

int audioreach_graph_send_cmd_sync(struct q6apm_graph *graph, const struct gpr_pkt *pkt,
				   uint32_t rsp_opcode)
{

	return audioreach_send_cmd_sync(graph->dev, NULL,  &graph->result, &graph->lock,
					graph->port, &graph->cmd_wait, pkt, rsp_opcode);
}
EXPORT_SYMBOL_GPL(audioreach_graph_send_cmd_sync);

static int audioreach_display_port_set_media_format(struct q6apm_graph *graph,
						    const struct audioreach_module *module,
						    const struct audioreach_module_config *cfg)
{
	struct apm_display_port_module_intf_cfg *intf_cfg;
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_module_param_data *param_data;
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	int ic_sz = APM_DP_INTF_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int size = ic_sz + ep_sz + fs_sz;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	hw_cfg = p;
	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = cfg->sample_rate;
	hw_cfg->mf.bit_width = cfg->bit_width;
	hw_cfg->mf.num_channels = cfg->num_channels;
	hw_cfg->mf.data_format = module->data_format;
	p += ep_sz;

	fs_cfg = p;
	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	fs_cfg->frame_size_factor = 1;
	p += fs_sz;

	intf_cfg = p;
	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_DISPLAY_PORT_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.channel_allocation = cfg->channel_allocation;
	intf_cfg->cfg.mst_idx = 0;
	intf_cfg->cfg.dptx_idx = cfg->dp_idx;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

/* LPASS Codec DMA port Module Media Format Setup */
static int audioreach_codec_dma_set_media_format(struct q6apm_graph *graph,
						 const struct audioreach_module *module,
						 const struct audioreach_module_config *cfg)
{
	struct apm_codec_dma_module_intf_cfg *intf_cfg;
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_module_hw_ep_power_mode_cfg *pm_cfg;
	struct apm_module_param_data *param_data;
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	int ic_sz = APM_CDMA_INTF_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int pm_sz = APM_HW_EP_PMODE_CFG_PSIZE;
	int size = ic_sz + ep_sz + fs_sz + pm_sz;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	hw_cfg = p;
	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = cfg->sample_rate;
	hw_cfg->mf.bit_width = cfg->bit_width;
	hw_cfg->mf.num_channels = cfg->num_channels;
	hw_cfg->mf.data_format = module->data_format;
	p += ep_sz;

	fs_cfg = p;
	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	fs_cfg->frame_size_factor = 1;
	p += fs_sz;

	intf_cfg = p;
	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_CODEC_DMA_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.lpaif_type = module->hw_interface_type;
	intf_cfg->cfg.intf_index = module->hw_interface_idx;
	intf_cfg->cfg.active_channels_mask = (1 << cfg->num_channels) - 1;
	p += ic_sz;

	pm_cfg = p;
	param_data = &pm_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_POWER_MODE_CFG;
	param_data->param_size = pm_sz - APM_MODULE_PARAM_DATA_SIZE;
	pm_cfg->power_mode.power_mode = 0;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

int audioreach_send_u32_param(struct q6apm_graph *graph,
			      const struct audioreach_module *module,
			      uint32_t param_id, uint32_t param_val)
{
	struct apm_module_param_data *param_data;
	uint32_t *param;
	int payload_size = sizeof(uint32_t) + APM_MODULE_PARAM_DATA_SIZE;
	void *p;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0,
					graph->port->id, module->instance_id);
	if (IS_ERR(pkt))
		return -ENOMEM;

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = param_id;
	param_data->param_size = sizeof(uint32_t);

	p = p + APM_MODULE_PARAM_DATA_SIZE;
	param = p;
	*param = param_val;

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}
EXPORT_SYMBOL_GPL(audioreach_send_u32_param);

static int audioreach_sal_limiter_enable(struct q6apm_graph *graph,
					 const struct audioreach_module *module,
					 bool enable)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_SAL_LIMITER_ENABLE, enable);
}

static int audioreach_sal_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_SAL_OUTPUT_CFG,  cfg->bit_width);
}

static int audioreach_module_enable(struct q6apm_graph *graph,
				    const struct audioreach_module *module,
				    bool enable)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_MODULE_ENABLE, enable);
}

static int audioreach_sp11_set_protection_enabled(struct q6apm_graph *graph,
						  bool enable)
{
	const struct audioreach_module *sp = NULL, *spvi = NULL;
	struct audioreach_container *container;
	struct audioreach_sub_graph *sg;
	struct audioreach_module *module;
	int ret;

	list_for_each_entry(sg, &graph->info->sg_list, node) {
		list_for_each_entry(container, &sg->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if (module->module_id == MODULE_ID_SPEAKER_PROTECTION)
					sp = module;
				else if (module->module_id ==
					 MODULE_ID_SPEAKER_PROTECTION_VI)
					spvi = module;
			}
		}
	}

	if (!sp || !spvi)
		return -ENODEV;

	/* Bring feedback up before render; tear render down before feedback. */
	ret = audioreach_module_enable(graph, enable ? spvi : sp, enable);
	if (ret)
		return ret;

	return audioreach_module_enable(graph, enable ? sp : spvi, enable);
}

static int audioreach_gapless_set_media_format(struct q6apm_graph *graph,
					       const struct audioreach_module *module,
					       const struct audioreach_module_config *cfg)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_EARLY_EOS_DELAY,
					 EARLY_EOS_DELAY_MS);
}

static int audioreach_set_module_config(struct q6apm_graph *graph,
					const struct audioreach_module *module,
					const struct audioreach_module_config *cfg)
{
	int size = le32_to_cpu(module->data->size);
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	memcpy(p, module->data->data, size);

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static struct audioreach_module_priv_data *
audioreach_module_stage(const struct audioreach_module *module, u32 type)
{
	switch (type) {
	case SND_SOC_AR_TPLG_GRAPH_CAL_CFG_TYPE:
		return module->graph_cal_data;
	case SND_SOC_AR_TPLG_RENDER_EP_CFG_TYPE:
		return module->render_ep_data;
	case SND_SOC_AR_TPLG_SP_TAG_CFG_TYPE:
		return module->sp_tag_data;
	case SND_SOC_AR_TPLG_SPVI_TAG_CFG_TYPE:
		return module->spvi_tag_data;
	case SND_SOC_AR_TPLG_VI_EP_CFG_TYPE:
		return module->vi_ep_data;
	case SND_SOC_AR_TPLG_PROTECTION_DYNAMIC_CFG_TYPE:
		return module->protection_dynamic_data;
	case SND_SOC_AR_TPLG_VOLUME_GAIN_CFG_TYPE:
		return module->volume_gain_data;
	case SND_SOC_AR_TPLG_VOLUME_FILTER_CFG_TYPE:
		return module->volume_filter_data;
	case SND_SOC_AR_TPLG_VOLUME_MUTE_CFG_TYPE:
		return module->volume_mute_data;
	case SND_SOC_AR_TPLG_CHANNEL_MIXER_CFG_TYPE:
		return module->channel_mixer_data;
	default:
		return NULL;
	}
}

static struct audioreach_module_priv_data *
audioreach_find_stage(const struct audioreach_graph_info *info, u32 type)
{
	struct audioreach_module_priv_data *found = NULL;
	struct audioreach_container *container;
	struct audioreach_sub_graph *sg;
	struct audioreach_module *module;

	list_for_each_entry(sg, &info->sg_list, node) {
		list_for_each_entry(container, &sg->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				struct audioreach_module_priv_data *candidate;

				candidate = audioreach_module_stage(module, type);
				if (!candidate)
					continue;
				if (found)
					return ERR_PTR(-EEXIST);
				found = candidate;
			}
		}
	}

	return found;
}

bool audioreach_graph_has_protected_calibration(
	const struct audioreach_graph_info *info)
{
	return audioreach_find_stage(
		info, SND_SOC_AR_TPLG_GRAPH_CAL_CFG_TYPE) != NULL;
}
EXPORT_SYMBOL_GPL(audioreach_graph_has_protected_calibration);

static bool audioreach_graph_has_iid(const struct audioreach_graph_info *info,
				     u32 iid)
{
	struct audioreach_container *container;
	struct audioreach_sub_graph *sg;
	struct audioreach_module *module;

	list_for_each_entry(sg, &info->sg_list, node)
		list_for_each_entry(container, &sg->container_list, node)
			list_for_each_entry(module, &container->modules_list, node)
				if (module->instance_id == iid)
					return true;

	return false;
}

static int audioreach_validate_stage_modules(
	const struct audioreach_graph_info *info,
	const struct audioreach_module_priv_data *stage)
{
	size_t size = le32_to_cpu(stage->size);
	const u8 *cursor = (const u8 *)stage->data;
	const u8 *end = cursor + size;

	while (cursor < end) {
		size_t frame_size;
		u32 iid = get_unaligned_le32(cursor);
		u32 param_size = get_unaligned_le32(cursor + 2 * sizeof(u32));

		if (!audioreach_graph_has_iid(info, iid))
			return -ENOENT;
		frame_size = ALIGN(sizeof(struct apm_module_param_data) +
				   param_size, 8);
		cursor += frame_size;
	}

	return cursor == end ? 0 : -EINVAL;
}

static int audioreach_send_inband_frame(struct q6apm_graph *graph,
					const u8 *frame)
{
	u32 param_size = get_unaligned_le32(frame + 2 * sizeof(u32));
	size_t frame_size = ALIGN(sizeof(struct apm_module_param_data) +
				  param_size, 8);
	void *payload;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(
			frame_size, APM_CMD_SET_CFG, 0, graph->port->id,
			get_unaligned_le32(frame));
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	payload = (u8 *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	memcpy(payload, frame, frame_size);
	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

static int audioreach_send_get_cfg(struct q6apm_graph *graph, u32 iid,
				   u32 param_id, u32 param_size)
{
	struct apm_module_param_data *param;
	size_t payload_size =
		ALIGN(APM_MODULE_PARAM_DATA_SIZE + param_size, 8);
	void *payload;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_GET_CFG, 0,
					graph->port->id, iid);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	payload = (u8 *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	memset(payload, 0, payload_size);
	param = payload;
	param->module_instance_id = iid;
	param->param_id = param_id;
	param->param_size = param_size;

	return audioreach_graph_send_cmd_sync(
		graph, pkt, APM_CMD_RSP_GET_CFG);
}

static const u8 *audioreach_stage_frame(
	const struct audioreach_module_priv_data *stage, unsigned int index)
{
	size_t size = le32_to_cpu(stage->size);
	const u8 *cursor = (const u8 *)stage->data;
	const u8 *end = cursor + size;
	unsigned int frame_index = 0;

	while (cursor < end) {
		u32 param_size = get_unaligned_le32(cursor + 2 * sizeof(u32));
		size_t frame_size =
			ALIGN(sizeof(struct apm_module_param_data) + param_size, 8);

		if (frame_index++ == index)
			return cursor;
		cursor += frame_size;
	}

	return NULL;
}

static int audioreach_log_stage(struct q6apm_graph *graph,
				const char *name, int ret)
{
	if (ret)
		dev_err(graph->dev, "SP11 stage %s failed: %d\n", name, ret);
	else
		dev_info(graph->dev, "SP11 stage %s accepted\n", name);

	return ret;
}

static int audioreach_validate_protection_profile(
	const struct audioreach_graph_info *info,
	const struct audioreach_module_priv_data *graph_cal,
	const struct audioreach_module_priv_data *render_ep,
	const struct audioreach_module_priv_data *sp_tag,
	const struct audioreach_module_priv_data *spvi_tag,
	const struct audioreach_module_priv_data *vi_ep,
	const struct audioreach_module_priv_data *dynamic,
	const struct audioreach_module_priv_data *volume_gain,
	const struct audioreach_module_priv_data *volume_filter,
	const struct audioreach_module_priv_data *volume_mute,
	const struct audioreach_module_priv_data *channel_mixer)
{
	static const struct {
		u32 iid;
		u32 param_id;
		u32 param_size;
	} dynamic_layout[] = {
		{ 0x4027, 0x080011e9, 8 },
		{ 0x4024, 0x080011f5, 24 },
		{ 0x4024, 0x080011f4, 24 },
		{ 0x4024, 0x080011ff, 8 },
	};
	const struct audioreach_module_priv_data *stages[] = {
		graph_cal, render_ep, sp_tag, spvi_tag, vi_ep, dynamic,
		volume_gain, volume_filter, volume_mute, channel_mixer,
	};
	const size_t expected_sizes[] = {
		10464, 64, 1888, 1328, 64, 128, 120, 216, 120, 40,
	};
	const u8 *frame;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(stages); i++) {
		if (!stages[i] ||
		    le32_to_cpu(stages[i]->size) != expected_sizes[i])
			return -EINVAL;
		ret = audioreach_validate_stage_modules(info, stages[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(dynamic_layout); i++) {
		frame = audioreach_stage_frame(dynamic, i);
		if (!frame ||
		    get_unaligned_le32(frame) != dynamic_layout[i].iid ||
		    get_unaligned_le32(frame + sizeof(u32)) !=
			    dynamic_layout[i].param_id ||
		    get_unaligned_le32(frame + 2 * sizeof(u32)) !=
			    dynamic_layout[i].param_size)
			return -EINVAL;
	}
	if (audioreach_stage_frame(dynamic, ARRAY_SIZE(dynamic_layout)))
		return -EINVAL;

	/* Both static GET-response layouts and the dynamic stage select 2 speakers. */
	frame = audioreach_stage_frame(sp_tag, 0);
	if (!frame ||
	    get_unaligned_le32(frame) != 0x4027 ||
	    get_unaligned_le32(frame + sizeof(u32)) != 0x080011e8 ||
	    get_unaligned_le32(frame + sizeof(struct apm_module_param_data) + 8) != 2)
		return -EINVAL;
	frame = audioreach_stage_frame(spvi_tag, 0);
	if (!frame ||
	    get_unaligned_le32(frame) != 0x4024 ||
	    get_unaligned_le32(frame + sizeof(u32)) != 0x080011f6 ||
	    get_unaligned_le32(frame + sizeof(struct apm_module_param_data)) != 2)
		return -EINVAL;

	return 0;
}

static atomic_t sp11_frame_diag_done = ATOMIC_INIT(0);

static void audioreach_diagnose_oob_frames(struct audioreach_graph *graph,
					   const u8 *data, size_t size)
{
	const u8 *cursor = data;
	const u8 *end = data + size;
	int first_rejected = -1;
	unsigned int accepted = 0;
	unsigned int rejected = 0;
	unsigned int index = 0;

	if (atomic_xchg(&sp11_frame_diag_done, 1))
		return;

	while (cursor < end) {
		u32 iid = get_unaligned_le32(cursor);
		u32 param_id = get_unaligned_le32(cursor + sizeof(u32));
		u32 param_size = get_unaligned_le32(cursor + 2 * sizeof(u32));
		size_t frame_size =
			ALIGN(sizeof(struct apm_module_param_data) + param_size, 8);
		int ret;

		ret = q6apm_send_oob_config(graph, cursor, frame_size);
		if (ret) {
			if (first_rejected < 0)
				first_rejected = index;
			rejected++;
			dev_err(graph->apm->dev,
				"SP11 OOB frame %u offset %zu iid 0x%08x param 0x%08x size %u rejected: %d\n",
				index, cursor - data, iid, param_id,
				param_size, ret);
		} else {
			accepted++;
		}
		cursor += frame_size;
		index++;
	}

	pr_info("sp11 graph-cal frame diagnostic: frames=%u accepted=%u rejected=%u first-rejected=%d\n",
		index, accepted, rejected, first_rejected);
}

int audioreach_send_protected_graph_calibration(struct audioreach_graph *graph)
{
	const __le32 enable[] = {
		cpu_to_le32(0x00004027),
		cpu_to_le32(0x08001026),
		cpu_to_le32(sizeof(u32)),
		0,
		cpu_to_le32(1),
		0,
	};
	struct audioreach_module_priv_data *graph_cal;
	const u8 *cal;
	u32 cal_size;
	int ib, ob, tail, ret;

	graph_cal = audioreach_find_stage(graph->info,
					 SND_SOC_AR_TPLG_GRAPH_CAL_CFG_TYPE);
	if (IS_ERR(graph_cal))
		return PTR_ERR(graph_cal);
	if (!graph_cal)
		return 0;
	if (le32_to_cpu(graph_cal->size) != 10464)
		return -EINVAL;
	ret = audioreach_validate_stage_modules(graph->info, graph_cal);
	if (ret)
		return ret;

	cal = (const u8 *)graph_cal->data;
	cal_size = le32_to_cpu(graph_cal->size);
	ret = q6apm_send_oob_config(graph, cal, cal_size);
	if (ret == -EOPNOTSUPP) {
		/*
		 * Match Qualcomm GSL's graph-calibration policy. ACDB can
		 * include query-only parameters in this aggregate; SPF reports
		 * AR_EUNSUPPORTED after applying the supported records and GSL
		 * deliberately lets graph open continue.
		 */
		dev_warn(graph->apm->dev,
			 "graph calibration returned AR_EUNSUPPORTED; continuing as Qualcomm GSL does\n");

		/*
		 * 2026-07-31 DIAGNOSTIC KERNEL ONLY. Do not merge into a daily kernel.
		 *
		 * Nothing has ever established whether SPF applies the supported
		 * records of this aggregate or discards all of them. The frame walk
		 * below answers that, but it is NOT passive: it re-sends each of the
		 * ~111 records individually, costing that many extra SET_CFG round
		 * trips during graph setup, which could stall bring-up on a slow DSP.
		 * Guarded by sp11_frame_diag_done so it runs at most once per boot.
		 *
		 * Behaviour is otherwise unchanged: ret is still forced to 0 below,
		 * exactly as Qualcomm GSL does.
		 */
		audioreach_diagnose_oob_frames(graph, cal, cal_size);

		ret = 0;
	}
	if (ret) {
		ib = q6apm_send_inband_config(graph, enable, sizeof(enable));
		ob = q6apm_send_oob_config(graph, enable, sizeof(enable));
		tail = q6apm_send_oob_config(graph, cal + 24, cal_size - 24);
		pr_info("sp11 graph-cal diagnostic: original=%d inband-enable=%d oob-enable=%d aggregate-without-sal-sentinel=%d\n",
			ret, ib, ob, tail);
		audioreach_diagnose_oob_frames(graph, cal, cal_size);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(audioreach_send_protected_graph_calibration);

int audioreach_configure_protection(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	struct audioreach_module_priv_data *graph_cal, *render_ep, *sp_tag;
	struct audioreach_module_priv_data *spvi_tag, *vi_ep, *dynamic;
	struct audioreach_module_priv_data *volume_gain, *volume_filter;
	struct audioreach_module_priv_data *volume_mute, *channel_mixer;
	const u8 *frame;
	int ret;

	graph_cal = audioreach_find_stage(graph->info,
					 SND_SOC_AR_TPLG_GRAPH_CAL_CFG_TYPE);
	if (IS_ERR(graph_cal))
		return PTR_ERR(graph_cal);
	if (!graph_cal)
		return 0;

	mutex_lock(&ar_graph->protection_lock);
	if (ar_graph->protection_configured) {
		ret = 0;
		goto out;
	}

	render_ep = audioreach_find_stage(graph->info,
					 SND_SOC_AR_TPLG_RENDER_EP_CFG_TYPE);
	sp_tag = audioreach_find_stage(graph->info,
				      SND_SOC_AR_TPLG_SP_TAG_CFG_TYPE);
	spvi_tag = audioreach_find_stage(graph->info,
					SND_SOC_AR_TPLG_SPVI_TAG_CFG_TYPE);
	vi_ep = audioreach_find_stage(graph->info,
				     SND_SOC_AR_TPLG_VI_EP_CFG_TYPE);
	dynamic = audioreach_find_stage(
		graph->info, SND_SOC_AR_TPLG_PROTECTION_DYNAMIC_CFG_TYPE);
	volume_gain = audioreach_find_stage(
		graph->info, SND_SOC_AR_TPLG_VOLUME_GAIN_CFG_TYPE);
	volume_filter = audioreach_find_stage(
		graph->info, SND_SOC_AR_TPLG_VOLUME_FILTER_CFG_TYPE);
	volume_mute = audioreach_find_stage(
		graph->info, SND_SOC_AR_TPLG_VOLUME_MUTE_CFG_TYPE);
	channel_mixer = audioreach_find_stage(
		graph->info, SND_SOC_AR_TPLG_CHANNEL_MIXER_CFG_TYPE);
	if (IS_ERR(render_ep) || IS_ERR(sp_tag) || IS_ERR(spvi_tag) ||
	    IS_ERR(vi_ep) || IS_ERR(dynamic) || IS_ERR(volume_gain) ||
	    IS_ERR(volume_filter) || IS_ERR(volume_mute) ||
	    IS_ERR(channel_mixer)) {
		ret = -EEXIST;
		goto out;
	}

	ret = audioreach_validate_protection_profile(
		graph->info, graph_cal, render_ep, sp_tag, spvi_tag, vi_ep,
		dynamic, volume_gain, volume_filter, volume_mute,
		channel_mixer);
	if (ret)
		goto out;

	if (!q6apm_sp11_vi_ready() || !q6apm_sp11_cps_ready()) {
		ret = audioreach_sp11_set_protection_enabled(graph, false);
		audioreach_log_stage(graph, "SP/SPVI fail-safe bypass", ret);
		if (ret)
			goto out;
		dev_warn(graph->dev,
			 "SP11 protection feedback incomplete (VI=%s CPS=%s); speaker protection is bypassed\n",
			 q6apm_sp11_vi_ready() ? "ready" : "missing",
			 q6apm_sp11_cps_ready() ? "ready" : "missing");
		goto volume_config;
	}

	/* Exact Windows order, with topology-critical GET counts prevalidated. */
	frame = audioreach_stage_frame(dynamic, 0);
	ret = audioreach_send_inband_frame(graph, frame);
	audioreach_log_stage(graph, "SP operating mode", ret);
	if (ret)
		goto out;
	ret = q6apm_send_graph_oob_config(graph, sp_tag->data,
					 le32_to_cpu(sp_tag->size));
	audioreach_log_stage(graph, "SP tag calibration", ret);
	if (ret)
		goto out;
	ret = audioreach_send_get_cfg(graph, 0x4027, 0x080011e8, 68);
	audioreach_log_stage(graph, "SP configuration query", ret);
	if (ret)
		goto out;
	ret = audioreach_send_get_cfg(graph, 0x4024, 0x080011f6, 44);
	audioreach_log_stage(graph, "SPVI configuration query", ret);
	if (ret)
		goto out;
	for (int i = 1; i < 4; i++) {
		frame = audioreach_stage_frame(dynamic, i);
		ret = audioreach_send_inband_frame(graph, frame);
		audioreach_log_stage(
			graph, i == 1 ? "SPVI R0/T0" :
			       i == 2 ? "SPVI channel mode" :
					"SPVI processing mode",
			ret);
		if (ret)
			goto out;
	}
	ret = q6apm_send_graph_oob_config(graph, spvi_tag->data,
					 le32_to_cpu(spvi_tag->size));
	audioreach_log_stage(graph, "SPVI tag calibration", ret);
	if (ret)
		goto out;
	ret = q6apm_send_graph_oob_config(graph, render_ep->data,
					 le32_to_cpu(render_ep->size));
	audioreach_log_stage(graph, "render endpoint calibration", ret);
	if (ret)
		goto out;
	ret = q6apm_send_graph_oob_config(graph, vi_ep->data,
					 le32_to_cpu(vi_ep->size));
	audioreach_log_stage(graph, "VI endpoint calibration", ret);
	if (ret)
		goto out;
	ret = audioreach_sp11_set_protection_enabled(graph, true);
	audioreach_log_stage(graph, "SP/SPVI enabled with VI+CPS feedback", ret);
	if (ret)
		goto out;

volume_config:
	frame = audioreach_stage_frame(volume_gain, 0);
	ret = audioreach_send_inband_frame(graph, frame);
	audioreach_log_stage(graph, "VOL_CTRL gain", ret);
	if (ret)
		goto out;
	ret = q6apm_send_graph_oob_config(graph, volume_filter->data,
					 le32_to_cpu(volume_filter->size));
	audioreach_log_stage(graph, "full-volume MSIIR calibration", ret);
	if (ret)
		goto out;
	frame = audioreach_stage_frame(volume_mute, 0);
	ret = audioreach_send_inband_frame(graph, frame);
	audioreach_log_stage(graph, "VOL_CTRL mute", ret);
	if (ret)
		goto out;
	ret = q6apm_send_graph_oob_config(
		graph, channel_mixer->data,
		le32_to_cpu(channel_mixer->size));
	audioreach_log_stage(graph, "channel-mixer calibration", ret);
	if (ret)
		goto out;

	ar_graph->protection_configured = true;
out:
	mutex_unlock(&ar_graph->protection_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(audioreach_configure_protection);

static int audioreach_mfc_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	struct apm_module_param_data *param_data;
	struct param_id_mfc_media_format *media_format;
	uint32_t num_channels = cfg->num_channels;
	int payload_size = APM_MFC_CFG_PSIZE(media_format, num_channels) +
				APM_MODULE_PARAM_DATA_SIZE;
	int i;
	void *p;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0,
					graph->port->id, module->instance_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT;
	param_data->param_size = APM_MFC_CFG_PSIZE(media_format, num_channels);
	p = p + APM_MODULE_PARAM_DATA_SIZE;
	media_format = p;

	media_format->sample_rate = cfg->sample_rate;
	media_format->bit_width = cfg->bit_width;
	media_format->num_channels = cfg->num_channels;
	for (i = 0; i < num_channels; i++)
		media_format->channel_mapping[i] = cfg->channel_map[i];

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

static int audioreach_set_compr_media_format(struct media_format *media_fmt_hdr,
					     void *p,
					     const struct audioreach_module_config *mcfg)
{
	struct payload_media_fmt_aac_t *aac_cfg;
	struct payload_media_fmt_pcm *mp3_cfg;
	struct payload_media_fmt_flac_t *flac_cfg;
	struct payload_media_fmt_opus_t *opus_cfg;

	switch (mcfg->fmt) {
	case SND_AUDIOCODEC_MP3:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_MP3;
		media_fmt_hdr->payload_size = 0;
		p = p + sizeof(*media_fmt_hdr);
		mp3_cfg = p;
		mp3_cfg->sample_rate = mcfg->sample_rate;
		mp3_cfg->bit_width = mcfg->bit_width;
		mp3_cfg->alignment = PCM_LSB_ALIGNED;
		mp3_cfg->bits_per_sample = mcfg->bit_width;
		mp3_cfg->q_factor = mcfg->bit_width - 1;
		mp3_cfg->endianness = PCM_LITTLE_ENDIAN;
		mp3_cfg->num_channels = mcfg->num_channels;
		break;
	case SND_AUDIOCODEC_AAC:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_AAC;
		media_fmt_hdr->payload_size = sizeof(struct payload_media_fmt_aac_t);
		p = p + sizeof(*media_fmt_hdr);
		aac_cfg = p;
		aac_cfg->aac_fmt_flag = 0;
		aac_cfg->audio_obj_type = 5;
		aac_cfg->num_channels = mcfg->num_channels;
		aac_cfg->total_size_of_PCE_bits = 0;
		aac_cfg->sample_rate = mcfg->sample_rate;
		break;
	case SND_AUDIOCODEC_FLAC:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_FLAC;
		media_fmt_hdr->payload_size = sizeof(struct payload_media_fmt_flac_t);
		p = p + sizeof(*media_fmt_hdr);
		flac_cfg = p;
		flac_cfg->sample_size = mcfg->codec.options.flac_d.sample_size;
		flac_cfg->num_channels = mcfg->num_channels;
		flac_cfg->min_blk_size = mcfg->codec.options.flac_d.min_blk_size;
		flac_cfg->max_blk_size = mcfg->codec.options.flac_d.max_blk_size;
		flac_cfg->sample_rate = mcfg->sample_rate;
		flac_cfg->min_frame_size = mcfg->codec.options.flac_d.min_frame_size;
		flac_cfg->max_frame_size = mcfg->codec.options.flac_d.max_frame_size;
		break;
	case SND_AUDIOCODEC_OPUS_RAW:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_OPUS;
		media_fmt_hdr->payload_size = sizeof(*opus_cfg);
		p = p + sizeof(*media_fmt_hdr);
		opus_cfg = p;
		/* raw opus packets prepended with 4 bytes of length */
		opus_cfg->bitstream_format = 1;
		/*
		 * payload_type:
		 * 0 -- read metadata from opus stream;
		 * 1 -- metadata is provided by filling in the struct here.
		 */
		opus_cfg->payload_type = 1;
		opus_cfg->version = mcfg->codec.options.opus_d.version;
		opus_cfg->num_channels = mcfg->codec.options.opus_d.num_channels;
		opus_cfg->pre_skip = mcfg->codec.options.opus_d.pre_skip;
		opus_cfg->sample_rate = mcfg->codec.options.opus_d.sample_rate;
		opus_cfg->output_gain = mcfg->codec.options.opus_d.output_gain;
		opus_cfg->mapping_family = mcfg->codec.options.opus_d.mapping_family;
		opus_cfg->stream_count = mcfg->codec.options.opus_d.chan_map.stream_count;
		opus_cfg->coupled_count = mcfg->codec.options.opus_d.chan_map.coupled_count;
		memcpy(opus_cfg->channel_mapping, mcfg->codec.options.opus_d.chan_map.channel_map,
		       sizeof(opus_cfg->channel_mapping));
		opus_cfg->reserved[0] = opus_cfg->reserved[1] = opus_cfg->reserved[2] = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int audioreach_compr_set_param(struct q6apm_graph *graph,
			       const struct audioreach_module_config *mcfg)
{
	struct media_format *header;
	int rc;
	void *p;
	int iid = q6apm_graph_get_rx_shmem_module_iid(graph);
	int payload_size = sizeof(struct apm_sh_module_media_fmt_cmd);

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_cmd_pkt(payload_size,
					DATA_CMD_WR_SH_MEM_EP_MEDIA_FORMAT,
					0, graph->port->id, iid);
	if (IS_ERR(pkt))
		return -ENOMEM;

	p = (void *)pkt + GPR_HDR_SIZE;
	header = p;
	rc = audioreach_set_compr_media_format(header, p, mcfg);
	if (rc)
		return rc;

	return gpr_send_port_pkt(graph->port, pkt);
}
EXPORT_SYMBOL_GPL(audioreach_compr_set_param);

static int audioreach_i2s_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_module_param_data *param_data;
	struct apm_i2s_module_intf_cfg *intf_cfg;
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	int ic_sz = APM_I2S_INTF_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int size = ic_sz + ep_sz + fs_sz;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	intf_cfg = p;

	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_I2S_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.lpaif_type = module->hw_interface_type;
	intf_cfg->cfg.intf_idx = module->hw_interface_idx;
	intf_cfg->cfg.sd_line_idx = module->sd_line_idx;

	switch (cfg->fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		intf_cfg->cfg.ws_src = CONFIG_I2S_WS_SRC_INTERNAL;
		break;
	case SND_SOC_DAIFMT_BC_FC:
		/* CPU is slave */
		intf_cfg->cfg.ws_src = CONFIG_I2S_WS_SRC_EXTERNAL;
		break;
	default:
		break;
	}

	p += ic_sz;
	hw_cfg = p;
	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = cfg->sample_rate;
	hw_cfg->mf.bit_width = cfg->bit_width;
	hw_cfg->mf.num_channels = cfg->num_channels;
	hw_cfg->mf.data_format = module->data_format;

	p += ep_sz;
	fs_cfg = p;
	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	fs_cfg->frame_size_factor = 1;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_logging_set_media_format(struct q6apm_graph *graph,
					       const struct audioreach_module *module)
{
	struct apm_module_param_data *param_data;
	struct data_logging_config *cfg;
	int size = sizeof(*cfg) + APM_MODULE_PARAM_DATA_SIZE;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_DATA_LOGGING_CONFIG;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	p = p + APM_MODULE_PARAM_DATA_SIZE;
	cfg = p;
	cfg->log_code = module->log_code;
	cfg->log_tap_point_id = module->log_tap_point_id;
	cfg->mode = module->log_mode;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_pcm_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *mcfg)
{
	struct payload_pcm_output_format_cfg *media_cfg;
	uint32_t num_channels = mcfg->num_channels;
	struct apm_pcm_module_media_fmt_cmd *cfg;
	struct apm_module_param_data *param_data;
	int payload_size;

	if (num_channels > 4) {
		dev_err(graph->dev, "Error: Invalid channels (%d)!\n", num_channels);
		return -EINVAL;
	}

	payload_size = APM_PCM_MODULE_FMT_CMD_PSIZE(num_channels);

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0,
					graph->port->id, module->instance_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	cfg = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = &cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_PCM_OUTPUT_FORMAT_CFG;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;

	cfg->header.data_format = DATA_FORMAT_FIXED_POINT;
	cfg->header.fmt_id = MEDIA_FMT_ID_PCM;
	cfg->header.payload_size = APM_PCM_OUT_FMT_CFG_PSIZE(media_cfg, num_channels);

	media_cfg = &cfg->media_cfg;
	media_cfg->alignment = PCM_LSB_ALIGNED;
	media_cfg->bit_width = mcfg->bit_width;
	media_cfg->endianness = PCM_LITTLE_ENDIAN;
	media_cfg->interleaved = module->interleave_type;
	media_cfg->num_channels = mcfg->num_channels;
	media_cfg->q_factor = mcfg->bit_width - 1;
	media_cfg->bits_per_sample = mcfg->bit_width;
	memcpy(media_cfg->channel_mapping, mcfg->channel_map, mcfg->num_channels);

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

static int audioreach_shmem_set_media_format(struct q6apm_graph *graph,
					     const struct audioreach_module *module,
					     const struct audioreach_module_config *mcfg)
{
	uint32_t num_channels = mcfg->num_channels;
	struct apm_module_param_data *param_data;
	struct payload_media_fmt_pcm *cfg;
	struct media_format *header;
	int rc, payload_size;
	void *p;

	if (num_channels > 4) {
		dev_err(graph->dev, "Error: Invalid channels (%d)!\n", num_channels);
		return -EINVAL;
	}

	payload_size = APM_SHMEM_FMT_CFG_PSIZE(num_channels) + APM_MODULE_PARAM_DATA_SIZE;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0,
					 graph->port->id, module->instance_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_MEDIA_FORMAT;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;
	p = p + APM_MODULE_PARAM_DATA_SIZE;

	header = p;
	if (mcfg->fmt == SND_AUDIOCODEC_PCM) {
		header->data_format = DATA_FORMAT_FIXED_POINT;
		header->fmt_id =  MEDIA_FMT_ID_PCM;
		header->payload_size = payload_size - sizeof(*header);

		p = p + sizeof(*header);
		cfg = p;
		cfg->sample_rate = mcfg->sample_rate;
		cfg->bit_width = mcfg->bit_width;
		cfg->alignment = PCM_LSB_ALIGNED;
		cfg->bits_per_sample = mcfg->bit_width;
		cfg->q_factor = mcfg->bit_width - 1;
		cfg->endianness = PCM_LITTLE_ENDIAN;
		cfg->num_channels = mcfg->num_channels;
		memcpy(cfg->channel_mapping, mcfg->channel_map, mcfg->num_channels);
	} else {
		rc = audioreach_set_compr_media_format(header, p, mcfg);
		if (rc)
			return rc;
	}

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

int audioreach_gain_set_vol_ctrl(struct q6apm *apm,
				 const struct audioreach_module *module, int vol)
{
	struct param_id_vol_ctrl_master_gain *cfg;
	struct apm_module_param_data *param_data;
	int size = sizeof(*cfg) + APM_MODULE_PARAM_DATA_SIZE;
	void *p;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_VOL_CTRL_MASTER_GAIN;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	p = p + APM_MODULE_PARAM_DATA_SIZE;
	cfg = p;
	cfg->master_gain =  vol;
	return q6apm_send_cmd_sync(apm, pkt, 0);
}
EXPORT_SYMBOL_GPL(audioreach_gain_set_vol_ctrl);

/*
 * SP11 runtime module-parameter injection (added 2026-08-01).
 *
 * Windows does not filter audio in user space. DolbyAPOvlldp150 analyses the
 * stream, computes filter coefficients, and injects them into the MSIIR
 * modules that already live in the DSP graph, via gsl_set_custom_config.
 * The DSP does the filtering, ahead of speaker protection.
 *
 * Linux had no equivalent: every module parameter was only ever sent during
 * graph setup, so MSIIR sits at whatever the topology loaded (currently unity
 * on 0x489e) for the life of the stream. A user-space processor could compute
 * the right coefficients but had no way to deliver them.
 *
 * This provides that delivery path. It deliberately mirrors
 * audioreach_gain_set_vol_ctrl(): build an APM_CMD_SET_CFG packet addressed to
 * one module instance and send it to the live graph.
 *
 * SAFETY: the caller is user space, and the modules adjacent to these live in
 * the speaker-protection subgraph. Both the target instance and the parameter
 * id are checked against an allowlist below. Do not widen it without
 * understanding what the module does; a wrong write next to SP/SP_VI is not a
 * cosmetic bug.
 */

/* MSIIR instances present in the deployed SP11 topology. */
#define SP11_MSIIR_IID_A		0x489e
#define SP11_MSIIR_IID_B		0x48a1

/* MSIIR tuning parameters. Per firmware analysis these are gated behind the
 * module's CAPI-initialised flag, so they are only accepted after the graph
 * has started and media format has been applied.
 */
#define SP11_PARAM_MSIIR_ENABLE		0x08001020
#define SP11_PARAM_MSIIR_PREGAIN		0x08001021
#define SP11_PARAM_MSIIR_COEFFS		0x08001022

#define SP11_MAX_INJECT_PAYLOAD	1024

static bool sp11_inject_target_allowed(u32 iid, u32 param_id)
{
	if (iid != SP11_MSIIR_IID_A && iid != SP11_MSIIR_IID_B)
		return false;

	switch (param_id) {
	case SP11_PARAM_MSIIR_ENABLE:
	case SP11_PARAM_MSIIR_PREGAIN:
	case SP11_PARAM_MSIIR_COEFFS:
		return true;
	default:
		return false;
	}
}

/*
 * audioreach_sp11_inject_module_param() - push one parameter to a live module
 * @apm:the APM instance
 * @iid:target module instance id (allowlisted)
 * @param_id:parameter id (allowlisted)
 * @payload:parameter body
 * @size:length of @payload in bytes
 *
 * Returns 0 on success. -EPERM if the target is not allowlisted, -EINVAL on a
 * bad size, or the DSP's error if it rejects the parameter. A rejection of -22
 * typically means the module has not been initialised through the media-format
 * path yet, i.e. the graph is not running.
 */
static int
audioreach_sp11_send_module_param(struct q6apm *apm, u32 iid, u32 param_id,
				  const void *payload, size_t size)
{
	struct apm_module_param_data *param_data;
	int pkt_size;
	void *p;

	if (!apm || !payload || !size || size > SP11_MAX_INJECT_PAYLOAD)
		return -EINVAL;

	pkt_size = ALIGN(size, 8) + APM_MODULE_PARAM_DATA_SIZE;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(pkt_size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = iid;
	param_data->error_code = 0;
	param_data->param_id = param_id;
	param_data->param_size = ALIGN(size, 8);

	p += APM_MODULE_PARAM_DATA_SIZE;
	memcpy(p, payload, size);

	return q6apm_send_cmd_sync(apm, pkt, 0);
}

int audioreach_sp11_inject_module_param(struct q6apm *apm, u32 iid, u32 param_id,
					const void *payload, size_t size)
{
	if (!apm || !payload)
		return -EINVAL;

	if (!size || size > SP11_MAX_INJECT_PAYLOAD)
		return -EINVAL;

	if (!sp11_inject_target_allowed(iid, param_id)) {
		dev_warn(apm->dev,
			 "SP11 inject refused: iid %#x param %#x not allowlisted\n",
			 iid, param_id);
		return -EPERM;
	}

	return audioreach_sp11_send_module_param(apm, iid, param_id, payload, size);
}
EXPORT_SYMBOL_GPL(audioreach_sp11_inject_module_param);

#define SP11_FINAL_VOL_CTRL_IID		0x4a63
#define SP11_PARAM_VOL_CTRL_MULTICH_GAIN	0x08001038
#define SP11_PARAM_VOL_CTRL_MULTICH_MUTE	0x08001039
#define SP11_VOL_CTRL_Q28_UNITY		0x10000000
#define SP11_VOL_CTRL_NUM_CONFIGS		8

struct sp11_vol_ctrl_channel_gain {
	u32 channel_mask_lsw;
	u32 channel_mask_msw;
	u32 gain_q28;
} __packed;

struct sp11_vol_ctrl_multichannel_gain {
	u32 num_config;
	struct sp11_vol_ctrl_channel_gain channel[SP11_VOL_CTRL_NUM_CONFIGS];
	u32 padding;
} __packed;

static_assert(sizeof(struct sp11_vol_ctrl_multichannel_gain) == 0x68);

struct sp11_vol_ctrl_channel_mute {
	u32 channel_mask_lsw;
	u32 channel_mask_msw;
	u32 mute;
} __packed;

struct sp11_vol_ctrl_multichannel_mute {
	u32 num_config;
	struct sp11_vol_ctrl_channel_mute channel[SP11_VOL_CTRL_NUM_CONFIGS];
	u32 padding;
} __packed;

static_assert(sizeof(struct sp11_vol_ctrl_multichannel_mute) == 0x68);

int audioreach_sp11_set_final_volume_q28(struct q6apm *apm, u32 left_q28,
					 u32 right_q28)
{
	struct sp11_vol_ctrl_multichannel_gain cfg = {
		.num_config = SP11_VOL_CTRL_NUM_CONFIGS,
		.channel[0] = { .channel_mask_lsw = 0x2, .gain_q28 = left_q28 },
		.channel[1] = { .channel_mask_lsw = 0x4, .gain_q28 = right_q28 },
	};

	if (!apm || left_q28 > SP11_VOL_CTRL_Q28_UNITY ||
	    right_q28 > SP11_VOL_CTRL_Q28_UNITY)
		return -EINVAL;

	return audioreach_sp11_send_module_param(apm, SP11_FINAL_VOL_CTRL_IID,
			SP11_PARAM_VOL_CTRL_MULTICH_GAIN, &cfg, sizeof(cfg));
}
EXPORT_SYMBOL_GPL(audioreach_sp11_set_final_volume_q28);

int audioreach_sp11_set_final_mute(struct q6apm *apm, bool muted)
{
	struct sp11_vol_ctrl_multichannel_mute cfg = {
		.num_config = SP11_VOL_CTRL_NUM_CONFIGS,
		.channel[0] = { .channel_mask_lsw = 0x2, .mute = muted ? 1 : 0 },
		.channel[1] = { .channel_mask_lsw = 0x4, .mute = muted ? 1 : 0 },
	};

	if (!apm)
		return -EINVAL;

	return audioreach_sp11_send_module_param(apm, SP11_FINAL_VOL_CTRL_IID,
			SP11_PARAM_VOL_CTRL_MULTICH_MUTE, &cfg, sizeof(cfg));
}
EXPORT_SYMBOL_GPL(audioreach_sp11_set_final_mute);

static int audioreach_gain_set(struct q6apm_graph *graph,
			       const struct audioreach_module *module)
{
	struct apm_module_param_data *param_data;
	struct apm_gain_module_cfg *cfg;
	int size = APM_GAIN_CFG_PSIZE;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	cfg = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = &cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = APM_PARAM_ID_GAIN;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	cfg->gain_cfg.gain = module->gain;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_speaker_protection(struct q6apm_graph *graph,
					 const struct audioreach_module *module,
					 uint32_t operation_mode)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_SP_OP_MODE,
					 operation_mode);
}

static int audioreach_speaker_protection_vi(struct q6apm_graph *graph,
					    const struct audioreach_module *module,
					    const struct audioreach_module_config *mcfg)
{
	u32 num_channels = mcfg->num_channels;
	struct apm_module_sp_vi_op_mode_cfg *op_cfg;
	struct apm_module_sp_vi_channel_map_cfg *cm_cfg;
	struct apm_module_sp_vi_ex_mode_cfg *ex_cfg;
	int op_sz, cm_sz, ex_sz;
	struct apm_module_param_data *param_data;
	int rc, i, payload_size;
	struct gpr_pkt *pkt;
	void *p;

	if (num_channels > 2) {
		dev_err(graph->dev, "Error: Invalid channels (%d)!\n", num_channels);
		return -EINVAL;
	}

	op_sz = APM_SP_VI_OP_MODE_CFG_PSIZE(num_channels);
	/* Channel mapping for Isense and Vsense, thus twice number of speakers. */
	cm_sz = APM_SP_VI_CH_MAP_CFG_PSIZE(num_channels * 2);
	ex_sz = APM_SP_VI_EX_MODE_CFG_PSIZE;

	payload_size = op_sz + cm_sz + ex_sz;

	pkt = audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	op_cfg = p;
	param_data = &op_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SP_VI_OP_MODE_CFG;
	param_data->param_size = op_sz - APM_MODULE_PARAM_DATA_SIZE;

	op_cfg->cfg.num_channels = num_channels;
	op_cfg->cfg.operation_mode = PARAM_ID_SP_VI_OP_MODE_NORMAL;
	p += op_sz;

	cm_cfg = p;
	param_data = &cm_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SP_VI_CHANNEL_MAP_CFG;
	param_data->param_size = cm_sz - APM_MODULE_PARAM_DATA_SIZE;

	cm_cfg->cfg.num_channels = num_channels * 2;
	for (i = 0; i < num_channels; i++) {
		/*
		 * Map speakers into Vsense and then Isense of each channel.
		 * E.g. for PCM_CHANNEL_FL and PCM_CHANNEL_FR to:
		 * [1, 2, 3, 4]
		 */
		cm_cfg->cfg.channel_mapping[2 * i] = (mcfg->channel_map[i] - 1) * 2 + 1;
		cm_cfg->cfg.channel_mapping[2 * i + 1] = (mcfg->channel_map[i] - 1) * 2 + 2;
	}

	p += cm_sz;

	ex_cfg = p;
	param_data = &ex_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SP_VI_EX_MODE_CFG;
	param_data->param_size = ex_sz - APM_MODULE_PARAM_DATA_SIZE;

	ex_cfg->cfg.factory_mode = 0;

	rc = q6apm_send_cmd_sync(graph->apm, pkt, 0);

	kfree(pkt);

	return rc;
}

int audioreach_set_media_format(struct q6apm_graph *graph,
				const struct audioreach_module *module,
				const struct audioreach_module_config *cfg)
{
	int rc;

	switch (module->module_id) {
	case MODULE_ID_DATA_LOGGING:
		rc = audioreach_module_enable(graph, module, true);
		if (!rc)
			rc = audioreach_logging_set_media_format(graph, module);
		break;
	case MODULE_ID_PCM_DEC:
	case MODULE_ID_PCM_ENC:
	case MODULE_ID_PCM_CNV:
	case MODULE_ID_PLACEHOLDER_DECODER:
	case MODULE_ID_PLACEHOLDER_ENCODER:
		rc = audioreach_pcm_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_DISPLAY_PORT_SINK:
		rc = audioreach_display_port_set_media_format(graph, module, cfg);
		break;
	case  MODULE_ID_SMECNS_V2:
		rc = audioreach_set_module_config(graph, module, cfg);
		break;
	case MODULE_ID_I2S_SOURCE:
	case MODULE_ID_I2S_SINK:
		rc = audioreach_i2s_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_WR_SHARED_MEM_EP:
	case MODULE_ID_SH_MEM_PULL_MODE:
		rc = audioreach_shmem_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_GAIN:
		rc = audioreach_gain_set(graph, module);
		break;
	case MODULE_ID_CODEC_DMA_SINK:
	case MODULE_ID_CODEC_DMA_SOURCE:
		rc = audioreach_codec_dma_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_SAL:
		rc = audioreach_sal_set_media_format(graph, module, cfg);
		if (!rc)
			rc = audioreach_sal_limiter_enable(graph, module, true);
		break;
	case MODULE_ID_MFC:
		rc = audioreach_mfc_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_GAPLESS:
		rc = audioreach_gapless_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_SPEAKER_PROTECTION:
		if (module->speaker_protection_bypass)
			return 0;

		rc = audioreach_speaker_protection(graph, module,
						   PARAM_ID_SP_OP_MODE_NORMAL);
		if (!rc)
			rc = audioreach_module_enable(graph, module, true);

		break;
	case MODULE_ID_SPEAKER_PROTECTION_VI:
		if (module->speaker_protection_bypass)
			return 0;

		rc = audioreach_speaker_protection_vi(graph, module, cfg);
		if (!rc)
			rc = audioreach_module_enable(graph, module, true);
		break;

	default:
		rc = 0;
	}

	return rc;
}
EXPORT_SYMBOL_GPL(audioreach_set_media_format);

void audioreach_graph_free_buf(struct q6apm_graph *graph)
{
	struct audioreach_graph_data *port;

	mutex_lock(&graph->lock);
	port = &graph->rx_data;
	port->num_periods = 0;
	kfree(port->buf);
	port->buf = NULL;

	port = &graph->tx_data;
	port->num_periods = 0;
	kfree(port->buf);
	port->buf = NULL;
	mutex_unlock(&graph->lock);
}
EXPORT_SYMBOL_GPL(audioreach_graph_free_buf);

int audioreach_shared_memory_send_eos(struct q6apm_graph *graph)
{
	struct data_cmd_wr_sh_mem_ep_eos *eos;
	int iid = q6apm_graph_get_rx_shmem_module_iid(graph);
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_cmd_pkt(sizeof(*eos),
					DATA_CMD_WR_SH_MEM_EP_EOS, 0, graph->port->id, iid);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	eos = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	eos->policy = WR_SH_MEM_EP_EOS_POLICY_LAST;

	return gpr_send_port_pkt(graph->port, pkt);
}
EXPORT_SYMBOL_GPL(audioreach_shared_memory_send_eos);
