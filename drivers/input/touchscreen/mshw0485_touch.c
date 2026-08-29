// SPDX-License-Identifier: GPL-2.0
/*
 * Microsoft Surface G6 Touch (MSHW0485) touchscreen driver.
 */

#include <linux/acpi.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/g6ts_heat.h>
#include <linux/gpio/consumer.h>
#include <linux/hid.h>
#include <linux/hid-over-spi.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/poll.h>
#include <linux/unaligned.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "g6ts_classifier_profile.h"
#include "g6ts_lifecycle_profile.h"

#define G6TS_NAME			"mshw0485-touch"
#define G6TS_SPI_HZ			40000000U
#define G6TS_MAX_BODY			8192U
#define G6TS_HEADER_SYNC		0x5a
#define G6TS_HEADER_VERSION		0x03
#define G6TS_FEATURE_RESPONSE_LIMIT	64U
#define G6TS_IRQ_DRAIN_LIMIT		128U
#define G6TS_HEAT_QUEUE_CAPACITY		64U
#define G6TS_HEATMAP_REPORT_ID		0x12
#define G6TS_RAW_SIDEBAND_REPORT_07	0x07
#define G6TS_RAW_HEAT_REPORT_0B		0x0b
#define G6TS_RAW_HEAT_REPORT_0C		0x0c
#define G6TS_RAW_HEAT_REPORT_0D		0x0d
#define G6TS_RAW_HEAT_REPORT_1A		0x1a
#define G6TS_RAW_SIDEBAND_REPORT_6E	0x6e
#define G6TS_IPTS_MODE_REPORT_ID	0x05
#define G6TS_IPTS_METADATA_REPORT_ID	0x06
#define G6TS_IPTS_METADATA_LEN		119U
#define G6TS_PEN_REPORT_ID		0x01
#define G6TS_PEN_REPORT_LEN		15U
#define G6TS_PEN_X_MAX			9600U
#define G6TS_PEN_Y_MAX			7200U
#define G6TS_PEN_PRESSURE_MAX		4096U
#define G6TS_PEN_TILT_RAW_MAX		18000U
#define G6TS_PEN_TILT_CENTER		9000
#define G6TS_PEN_X_RESOLUTION		35U
#define G6TS_PEN_Y_RESOLUTION		39U
#define G6TS_PEN_TILT_RESOLUTION	5730U
#define G6TS_PEN_IN_RANGE		BIT(0)
#define G6TS_PEN_TIP_SWITCH		BIT(1)
#define G6TS_PEN_BARREL_SWITCH		BIT(2)
#define G6TS_PEN_INVERT			BIT(3)
#define G6TS_PEN_ERASER			BIT(4)
#define G6TS_MODE_ATTEMPT_LIMIT		3U
#define G6TS_RECOVERY_DELAY_MS		100U
#define G6TS_RECOVERY_RETRY_MS		500U
#define G6TS_RECOVERY_LIMIT		3U
#define G6TS_HID_REBIND_STABLE_MS	250U
#define G6TS_HID_REBIND_RETRY_MS	1000U
#define G6TS_HID_REBIND_RETRY_LIMIT	2U
#define G6TS_RESET_STORM_WINDOW_MS	5000U
#define G6TS_RESET_STORM_LIMIT		3U
#define G6TS_HEAT_ROWS			46U
#define G6TS_HEAT_COLS			68U
#define G6TS_HEAT_SAMPLES		(G6TS_HEAT_ROWS * G6TS_HEAT_COLS)
#define G6TS_HEAT_SECTION		0x0100
#define G6TS_METADATA_SECTION		0xff00
#define G6TS_NSR_BINS			16U
#define G6TS_NSR_CUTOFF			655U
/*
 * TouchPenProcessor0C83.dll converts a calibrated byte through a linear
 * lookup whose zero crossing is approximately 180.  The SP11 configuration
 * is inferred to start a candidate at raw <= 171.  Its scan-line union joins
 * only edge-adjacent cells.  One- and two-cell candidates survive only when
 * their peak is stronger than the secondary detector threshold (approximately
 * raw <= 162); all candidates of three or more cells continue downstream.
 */
#define G6TS_HEAT_SIGNAL_ZERO		180U
#define G6TS_HEAT_THRESHOLD		9U
#define G6TS_HEAT_ACTIVE_MAX		(G6TS_HEAT_SIGNAL_ZERO - G6TS_HEAT_THRESHOLD)
#define G6TS_HEAT_STRONG_MAX		162U
#define G6TS_HEAT_MIN_PIXELS		3U
#define G6TS_HEAT_PALM_PIXELS		48U
#define G6TS_HEAT_PALM_SPAN		12U
#define G6TS_MAX_CONTACTS		10U
#define G6TS_LOGICAL_MAX		32767U
#define G6TS_TRACK_MATCH_MAX		4096U
#define G6TS_CONTACT_HOLD_FRAMES	6U
#define G6TS_TRACK_CONFIRM_NORMAL	3U
#define G6TS_TRACK_CONFIRM_WEAK		5U
#define G6TS_TRACK_CONFIRM_SPLIT	8U
#define G6TS_TRACK_SPLIT_RADIUS		2048U
#define G6TS_BEHAVIOR_CONFIRM_NORMAL	2U
#define G6TS_WINDOWS_CENTROID_BASELINE	171U
#define G6TS_SMOOTH_STATIONARY_MAX	64U
#define G6TS_SMOOTH_SLOW_MAX		256U
#define G6TS_SIGNAL_INTERCEPT_Q24	6710886
#define G6TS_SIGNAL_STEP_Q24		37251
#define G6TS_SECONDARY_A_Q12		645663
#define G6TS_SECONDARY_NOISE_Q12	36895
#define G6TS_SECONDARY_SEED_Q12		225
#define G6TS_LOCAL_PEAK_FLOOR_Q12	164
#define G6TS_LOCAL_PEAK_CAPACITY	10U
#define G6TS_AXIS_SCALE_Q12		18919
#define G6TS_SPREAD_SCALE_Q12		25736
#define G6TS_HALO_RATIO_Q12		614
#define G6TS_HALO_UNAVAILABLE_Q12	(100U << G6TS_CLASSIFIER_SHIFT)
#define G6TS_ASSIGN_MAX			(G6TS_MAX_CONTACTS * 2U)
#define G6TS_ASSIGN_UNMATCHED_COST	1000000
#define G6TS_ASSIGN_INVALID_COST	3000000

/* Exact SP11 descriptor values observed in every complete Windows capture. */
#define G6TS_SP11_REPORT_DESCRIPTOR_LEN	1484U
#define G6TS_SP11_MAX_INPUT_LEN		8192U
#define G6TS_SP11_MAX_OUTPUT_LEN		512U
#define G6TS_SP11_MAX_FRAGMENT_LEN	8192U
#define G6TS_SP11_VENDOR_ID		0x045eU
#define G6TS_SP11_X1E_PRODUCT_ID	0x0c83U
#define G6TS_SP11_X1P_PRODUCT_ID	0x0c80U
#define G6TS_SP11_VERSION_ID		0x0004U
#define G6TS_SP11_DESCRIPTOR_FLAGS	0x0001U
#define G6TS_WINDOWS_FEEDBACK_LEN	63U
#define G6TS_WINDOWS_REPORT56_ID_LEN	6U
#define G6TS_WINDOWS_CONFIG_DELAY_MS	470U
#define G6TS_WINDOWS_CFU_DELAY_MS	825U
#define G6TS_WINDOWS_FINAL_CONFIG_DELAY_MS 600U
#define G6TS_WINDOWS_CFU_OFFER_LEN	16U
#define G6TS_WINDOWS_CFU_VERSION_LEN	60U
#define G6TS_WINDOWS_CFU_TOKEN		0xa0U
#define G6TS_WINDOWS_HEADER_BODY_MIN_US	490U
#define G6TS_WINDOWS_HEADER_BODY_MAX_US	550U

enum g6ts_initialization_stage {
	G6TS_INIT_IDLE,
	G6TS_INIT_WINDOWS_POWER_PS0,
	G6TS_INIT_WINDOWS_RESET_METHOD,
	G6TS_INIT_RESET_RESPONSE,
	G6TS_INIT_DEVICE_DESCRIPTOR,
	G6TS_INIT_REPORT_DESCRIPTOR,
	G6TS_INIT_WINDOWS_EARLY_FEATURE73,
	G6TS_INIT_WINDOWS_HEAT_CAPS06,
	G6TS_INIT_WINDOWS_FEEDBACK_REQUIRED,
	G6TS_INIT_WINDOWS_FEEDBACK_A1,
	G6TS_INIT_WINDOWS_FEEDBACK_A5,
	G6TS_INIT_WINDOWS_SET_FEATURE05,
	G6TS_INIT_WINDOWS_CONFIG_OWNER_REQUIRED,
	G6TS_INIT_WINDOWS_GET_FEATURE70,
	G6TS_INIT_WINDOWS_SET_FEATURE70,
	G6TS_INIT_WINDOWS_SET_FEATURE56,
	G6TS_INIT_WINDOWS_CFU_OWNER_REQUIRED,
	G6TS_INIT_WINDOWS_CFU_GET_VERSION,
	G6TS_INIT_WINDOWS_CFU_START_TRANSACTION,
	G6TS_INIT_WINDOWS_CFU_START_LIST,
	G6TS_INIT_WINDOWS_CFU_OFFER,
	G6TS_INIT_WINDOWS_CFU_BRANCH_REQUIRED,
	G6TS_INIT_WINDOWS_CFU_END_LIST,
	G6TS_INIT_WINDOWS_FINAL_FEATURE73,
	G6TS_INIT_WINDOWS_HEAT_OWNER_REQUIRED,
	G6TS_INIT_SET_FEATURE05,
	G6TS_INIT_GET_FEATURE70,
	G6TS_INIT_SET_FEATURE70,
	G6TS_INIT_SET_FEATURE56,
	G6TS_INIT_WAIT_HEAT,
};

#define G6TS_INIT_STAGE_COUNT		(G6TS_INIT_WAIT_HEAT + 1)

/*
 * Phase 70 keeps the hardware-validated Phase 68 policy as the default.  The
 * recovered Windows profile is opt-in until its provider-owned frame flags
 * have live ground truth.  Read-only prevents switching policy under active
 * contacts and mixing two incompatible track coordinate spaces.
 */
static bool g6ts_windows_orchestrator;
module_param_named(windows_orchestrator, g6ts_windows_orchestrator, bool, 0444);
MODULE_PARM_DESC(windows_orchestrator,
		 "Use the experimental recovered Windows frame profile (default: false)");

static bool g6ts_heat_feedback;
module_param_named(heat_feedback, g6ts_heat_feedback, bool, 0444);
MODULE_PARM_DESC(heat_feedback,
		 "Enable the per-heat-cycle Windows V06 feedback stream (report 0x09); "
		 "requires parity_fast_host_id=0..65535 (default: false)");

/*
 * Expose the native descriptor and DFT reports through a HIDRAW-only sibling
 * so the established iptsd decoder can provide the stylus input device.  The
 * custom driver remains the sole owner of panel mode and touch processing.
 */
static bool g6ts_ipts_hid_bridge = true;
module_param_named(ipts_hid_bridge, g6ts_ipts_hid_bridge, bool, 0444);
MODULE_PARM_DESC(ipts_hid_bridge,
		 "Expose a HIDRAW-only IPTS compatibility device (default: true)");

/*
 * Phase 76 isolates the behavior-only changes from the hardware-validated
 * Phase 75 transport and recovery path.  It enables three independently
 * proven ordinary-finger pieces without enabling the incomplete Windows
 * lifecycle: sensor-space assignment, direct output coordinates, and the
 * project-0x0c83 normal expanded-centroid baseline.  Strong contacts use the
 * previously tested two-frame admission window; weak and nearby split
 * candidates retain their longer gates.
 */
static bool g6ts_behavior_v2;
module_param_named(behavior_v2, g6ts_behavior_v2, bool, 0444);
MODULE_PARM_DESC(behavior_v2,
		 "Use Phase 76 low-latency Windows-derived contact behavior (default: false)");

/*
 * Phase 72 hardware-validated a coupled experiment: append the Linux panel's
 * GET_FEATURE 0x70 content to SET_FEATURE 0x70 and emit a short report 0x09.
 * A later transfer-length audit disproved the original claim that this mirrors
 * Windows: captured Windows SET_FEATURE 0x70 content is one byte and its report
 * 0x09 content is 63 bytes. Keep the proven Linux sequence until isolated
 * experiments identify which element prevents the reset storm.
 */
static bool g6ts_mode_config_fix = true;
module_param_named(mode_config_fix, g6ts_mode_config_fix, bool, 0444);
MODULE_PARM_DESC(mode_config_fix,
		 "Use validated Phase 72 SET 0x70 + short 0x09 sequence (default: true)");

/*
 * Phase 79 isolates the first known Phase 72 divergence. Windows declares
 * one logical SET_FEATURE 0x70 content byte (01); Linux appends the panel's
 * GET_FEATURE result (02). Keep the short Phase 72 report 0x09 unchanged so
 * this switch changes exactly one logical feature byte.
 */
static bool g6ts_feature70_one_byte;
module_param_named(feature70_one_byte, g6ts_feature70_one_byte, bool, 0444);
MODULE_PARM_DESC(feature70_one_byte,
		 "Use Windows-length one-byte SET_FEATURE 0x70 with short report 0x09 (default: false)");

/*
 * Phase 77 keeps cold initialization and the Phase 75 mode exchange intact,
 * but does not force a second hardware reset after the panel has already sent
 * RESET_RESPONSE.  The device-reset path re-enumerates in place, gates Linux
 * input until a complete Heat frame validates the restored stream, and falls
 * back to the cold hardware path if software recovery itself fails.
 */
static bool g6ts_reset_recovery_v2;
module_param_named(reset_recovery_v2, g6ts_reset_recovery_v2, bool, 0444);
MODULE_PARM_DESC(reset_recovery_v2,
		 "Use Phase 77 gated software recovery after panel reset (default: false)");

/*
 * Phase 78 bounds the Phase 77 fast path.  A real hardware run proved that
 * software re-enumeration can succeed repeatedly while the panel continues
 * issuing RESET_RESPONSE.  Three closely spaced resets therefore escalate
 * the current attempt to the existing cold hardware power/reset path.
 */
static bool g6ts_reset_storm_breaker;
module_param_named(reset_storm_breaker, g6ts_reset_storm_breaker, bool, 0444);
MODULE_PARM_DESC(reset_storm_breaker,
		 "Escalate a rapid Phase 77 reset loop to hardware recovery (default: false)");

/*
 * The July KDNET capture proves that Windows does not leave the collection
 * inert after a host-side HID-SPI timeout: it enters ResetDevice, observes the
 * reset response, and enumerates the descriptors again.  The Linux IRQ path
 * historically just broke out after a transport or framing failure, leaving
 * mode_enabled set with no recovery work queued.  Phase 80 makes that recovery
 * explicit and observable.  It uses the already proven cold hardware path;
 * it does not invent another panel command sequence.
 */
static bool g6ts_host_fault_recovery;
module_param_named(host_fault_recovery, g6ts_host_fault_recovery, bool, 0444);
MODULE_PARM_DESC(host_fault_recovery,
		 "Recover with a cold re-enumeration after an IRQ transport/protocol fault (default: false)");

/*
 * Phase 80 live evidence found invalid second headers immediately after valid
 * Heat bodies.  GPIO51 can remain logically ready until the completed body
 * transaction has propagated through the panel.  Phase 81 waits once, then
 * treats an invalid header as an empty trailing read only if ready deasserted;
 * a still-asserted line remains a real protocol fault handled by Phase 80.
 */
static bool g6ts_ready_quiesce;
module_param_named(ready_quiesce, g6ts_ready_quiesce, bool, 0444);
MODULE_PARM_DESC(ready_quiesce,
		 "Ignore one invalid trailing header after GPIO51 deasserts (default: false)");

/*
 * Evidence-only cold-attach path.  It follows the byte-exact Windows cold
 * sequence through each operation whose owner and contents are known. Dynamic
 * A1/A5 feedback and report-0x56 identity inputs are unavailable by default;
 * when explicitly supplied, the path advances through the device-config
 * owner and stops before independent CFU traffic. Heat remains disabled
 * unless the separate parity_heat_input gate is selected together with the
 * complete bounded CFU inventory path.
 */
static bool g6ts_windows_init_parity;
module_param_named(windows_init_parity, g6ts_windows_init_parity, bool, 0444);
MODULE_PARM_DESC(windows_init_parity,
		 "Run evidence-gated Windows cold initialization (default: false)");

/*
 * Isolate the ACPI-derived Windows _PS0 + _RST ordering from the remainder of
 * the parity chronology. Phase 90 selects the hardware-proven Phase 75 power
 * sequence while leaving every later Windows-parity operation unchanged.
 */
static bool g6ts_parity_linux_power;
module_param_named(parity_linux_power, g6ts_parity_linux_power, bool, 0444);
MODULE_PARM_DESC(parity_linux_power,
		 "Use the Phase 75 power/reset sequence in Windows parity mode");

/*
 * The stable Windows SPB trace never chains another header from the still-
 * asserted GPIO level. It waits about 500 us between header and body and
 * services one complete response per interrupt. Phase 91 isolates that
 * measured cadence from all initialization and contact-processing choices.
 */
static bool g6ts_windows_read_cadence;
module_param_named(windows_read_cadence, g6ts_windows_read_cadence, bool, 0444);
MODULE_PARM_DESC(windows_read_cadence,
		 "Use measured Windows header/body and one-response IRQ cadence");

/*
 * Provider inputs are deliberately invalid by default.  The Windows producer
 * obtains these from display/posture state and persistent storage; an isolated
 * parity boot may supply captured values for this exact machine, but the driver
 * must never silently manufacture them.
 */
static int g6ts_parity_display_bitmap = -1;
module_param_named(parity_display_bitmap, g6ts_parity_display_bitmap, int, 0444);
MODULE_PARM_DESC(parity_display_bitmap,
		 "Windows A1 display bitmap; required by windows_init_parity (-1: unavailable)");

static int g6ts_parity_stitching_flag = -1;
module_param_named(parity_stitching_flag, g6ts_parity_stitching_flag, int, 0444);
MODULE_PARM_DESC(parity_stitching_flag,
		 "Windows A1 stitching flag; required by windows_init_parity (-1: unavailable)");

static int g6ts_parity_hinge_angle = -1;
module_param_named(parity_hinge_angle, g6ts_parity_hinge_angle, int, 0444);
MODULE_PARM_DESC(parity_hinge_angle,
		 "Windows A1 hinge-angle value; required by windows_init_parity (-1: unavailable)");

static int g6ts_parity_fast_host_id = -1;
module_param_named(parity_fast_host_id, g6ts_parity_fast_host_id, int, 0444);
MODULE_PARM_DESC(parity_fast_host_id,
		 "Windows feedback FastHostId (0..65535, -1: unavailable)");

/*
 * Report 0x56 carries six bytes of device identity (Usage 0x03) and one
 * boolean (Usage 0x09).  The six bytes are repeated in GET_FEATURE 0x60 at
 * offsets 20,24,28,32,36,40, but Windows sends 0x56 before its CFU owner
 * requests 0x60.  Require the owning platform value rather than changing the
 * observed ordering or silently borrowing one machine's identity.
 */
static u8 g6ts_parity_report56_identity[G6TS_WINDOWS_REPORT56_ID_LEN];
static unsigned int g6ts_parity_report56_identity_count;
module_param_array_named(parity_report56_identity,
			 g6ts_parity_report56_identity, byte,
			 &g6ts_parity_report56_identity_count, 0444);
MODULE_PARM_DESC(parity_report56_identity,
		 "Six Windows report-0x56 Usage-0x03 identity bytes");

static int g6ts_parity_report56_flag = -1;
module_param_named(parity_report56_flag, g6ts_parity_report56_flag, int, 0444);
MODULE_PARM_DESC(parity_report56_flag,
		 "Windows report-0x56 Usage-0x09 boolean (-1: unavailable)");

/*
 * SurfaceCFUOverHid owns report 0x60/0x65 independently. Phase 84 leaves this
 * false and stops before CFU. A later laboratory entry may enable the bounded
 * inventory-only transaction after supplying the exact installed offer.
 * There is deliberately no firmware-payload parameter or implementation.
 */
static bool g6ts_parity_cfu_inventory;
module_param_named(parity_cfu_inventory, g6ts_parity_cfu_inventory, bool, 0444);
MODULE_PARM_DESC(parity_cfu_inventory,
		 "Run Windows CFU inventory/offer check without payload transfer");

static u8 g6ts_parity_cfu_offer[G6TS_WINDOWS_CFU_OFFER_LEN];
static unsigned int g6ts_parity_cfu_offer_count;
module_param_array_named(parity_cfu_offer, g6ts_parity_cfu_offer, byte,
			 &g6ts_parity_cfu_offer_count, 0444);
MODULE_PARM_DESC(parity_cfu_offer,
		 "Exact 16-byte installed Windows CFU offer (token byte is provider-owned)");

/*
 * Phase 86 is the first end-to-end laboratory entry.  It may admit Heat only
 * after the evidence-gated Windows cold attach and bounded CFU inventory have
 * both completed through the captured final report-0x73 boundary.  Keeping
 * this separate from windows_init_parity preserves Phase 84/85 as diagnostic
 * stop points and prevents a partial attach from being mistaken for input
 * readiness.
 */
static bool g6ts_parity_heat_input;
module_param_named(parity_heat_input, g6ts_parity_heat_input, bool, 0444);
MODULE_PARM_DESC(parity_heat_input,
		 "Admit Heat/input only after complete Windows init+CFU parity");

enum g6ts_recovery_path {
	G6TS_RECOVERY_HARDWARE,
	G6TS_RECOVERY_SOFTWARE,
};

static const u8 g6ts_header_cmd[8] = {
	0xeb, 0x00, 0x10, 0x00, 0xff, 0xff, 0xff, 0xff,
};

static const u8 g6ts_body_cmd[8] = {
	0xeb, 0x00, 0x10, 0x04, 0xff, 0xff, 0xff, 0xff,
};

/* Windows HID-over-SPI DEVICE_DESCRIPTOR request captured in ETW. */
static const u8 g6ts_device_descriptor_cmd[8] = {
	0xe2, 0x00, 0x20, 0x00, DEVICE_DESCRIPTOR, 0x00, 0x00, 0x00,
};

static const u8 g6ts_report_descriptor_cmd[8] = {
	0xe2, 0x00, 0x20, 0x00, REPORT_DESCRIPTOR, 0x00, 0x00, 0x00,
};

static const u8 g6ts_mode_enable[] = { 0x01 };
/* SHA-256 fc5772d4...0bd58af; metadata only, never a firmware payload. */
static const u8 g6ts_sp11_cfu_offer[G6TS_WINDOWS_CFU_OFFER_LEN] = {
	0x00, 0x00, 0x12, 0x00, 0x89, 0x14, 0x00, 0x3f,
	0xff, 0xff, 0xff, 0xff, 0x04, 0x04, 0x75, 0x00,
};

static const u8 g6ts_mode_handshake[] = {
	0xbc, 0xe6, 0x4a, 0x2e, 0x86, 0x78, 0x00,
};

/* TouchPenProcessor project-0x0c83 sensor-row to NSR-bin mapping. */
static const u8 g6ts_nsr_row_to_bin[G6TS_HEAT_ROWS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

struct g6ts_heat_frame {
	struct g6ts_heat_record_header header;
	u8 content[G6TS_HEAT_MAX_CONTENT_SIZE];
};

static_assert(sizeof(struct g6ts_heat_record_header) ==
	      G6TS_HEAT_RECORD_HEADER_SIZE);
static_assert(sizeof(struct g6ts_heat_info) == 48);
static_assert(sizeof(struct g6ts_heat_stats) == 112);

struct g6ts_heat_device {
	struct miscdevice miscdev;
	/* Protects queue data and device lifetime state. */
	struct mutex lock;
	wait_queue_head_t read_wait;
	struct kref ref;
	struct g6ts_heat_frame *frames;
	unsigned int head;
	unsigned int count;
	u32 generation;
	u32 sequence;
	u64 records_enqueued;
	u64 records_dropped;
	u64 queue_flushes;
	u64 oversize_drops;
	u64 report_counts[4];
	atomic_t opened;
	bool dead;
};

struct g6ts_a5_live_state {
	u16 fast_host_id;
	u8 sequence;
	bool fast_host_id_valid;
	bool cycle_saw_0x0d;
};

struct g6ts_contact {
	u64 weighted_x;
	u64 weighted_y;
	u32 strength;
	u32 sensor_x_q24;
	u32 sensor_y_q24;
	u16 pixels;
	u16 x;
	u16 y;
	u16 output_x;
	u16 output_y;
	u8 peak_value;
	u8 min_col;
	u8 max_col;
	u8 min_row;
	u8 max_row;
	s32 features_q12[G6TS_FEATURE_COUNT];
	s64 scores_q24[G6TS_CLASS_COUNT];
	u8 local_peak_count;
	u8 strong_local_peak_count;
	u8 shape_class;
	bool shape_allowed;
};

struct g6ts_track {
	u32 strength;
	u16 raw_x;
	u16 raw_y;
	u16 output_x;
	u16 output_y;
	u16 pixels;
	u32 sensor_x_q24;
	u32 sensor_y_q24;
	s32 sensor_velocity_x_q24;
	s32 sensor_velocity_y_q24;
	s16 velocity_x;
	s16 velocity_y;
	u16 age;
	s64 score_history_q24[G6TS_WINDOWS_HISTORY_CAPACITY][G6TS_CLASS_COUNT];
	u8 missed;
	u8 evidence;
	u8 required_evidence;
	u8 shape_class;
	u8 windows_class;
	u8 score_history_count;
	bool active;
	bool confirmed;
};

struct g6ts_assignment_workspace {
	int cost[G6TS_ASSIGN_MAX][G6TS_ASSIGN_MAX];
	int u[G6TS_ASSIGN_MAX + 1];
	int v[G6TS_ASSIGN_MAX + 1];
	int p[G6TS_ASSIGN_MAX + 1];
	int way[G6TS_ASSIGN_MAX + 1];
	int minv[G6TS_ASSIGN_MAX + 1];
	bool used[G6TS_ASSIGN_MAX + 1];
	int active_slots[G6TS_MAX_CONTACTS];
};

struct g6ts {
	struct spi_device *spi;
	struct input_dev *input;
	struct g6ts_heat_device *heat;
	struct hid_device *ipts_hid;
	struct input_dev *pen_input;
	struct touchscreen_properties prop;
	struct touchscreen_properties pen_prop;
	/* Serializes transport, initialization, recovery, and power changes. */
	struct mutex io_lock;
	struct gpio_desc *interrupt_gpio;
	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;
	u8 *body;
	u8 heatmap[G6TS_HEAT_SAMPLES];
	u8 heat_seen[G6TS_HEAT_SAMPLES];
	u8 heat_component[G6TS_HEAT_SAMPLES];
	u8 heat_work[G6TS_HEAT_SAMPLES];
	u16 heat_queue[G6TS_HEAT_SAMPLES];
	u16 nsr_bins[G6TS_NSR_BINS];
	struct g6ts_contact contacts[G6TS_MAX_CONTACTS];
	struct g6ts_track tracks[G6TS_MAX_CONTACTS];
	struct g6ts_assignment_workspace assignment;
	struct delayed_work recovery_work;
	struct delayed_work hid_rebind_work;
	u8 last_header[HIDSPI_INPUT_HEADER_SIZE];
	u8 report_descriptor[G6TS_SP11_REPORT_DESCRIPTOR_LEN];
	u8 ipts_metadata[G6TS_IPTS_METADATA_LEN];
	u8 last_class;
	u16 last_content_len;
	u16 expected_report_descriptor_len;
	u16 hid_product_id;
	u8 last_content_id;
	u32 hid_target_generation;
	u32 hid_bound_generation;
	u8 hid_rebind_failures;
	/* Phase 72 Linux response content used by the combined experimental path. */
	u8 mode_config[G6TS_FEATURE_RESPONSE_LIMIT];
	u8 mode_config_len;
	bool mode_config_valid;
	int interrupt_irq;
	atomic64_t interrupt_edges;
	s64 handled_interrupt_edges;
	u64 reset_notifications;
	u64 recovery_successes;
	u64 recovery_failures;
	u64 hardware_recovery_attempts;
	u64 software_recovery_attempts;
	u64 software_recovery_fallbacks;
	u64 reset_storm_escalations;
	u64 host_fault_recoveries;
	u64 irq_transport_errors;
	u64 irq_protocol_errors;
	u64 irq_drain_overflows;
	u64 quiesced_empty_reads;
	u64 cadence_single_response_irqs;
	u64 ready_heat_frames;
	u64 ready_verification_failures;
	u64 report_counts[U8_MAX + 1];
	struct g6ts_a5_live_state a5;
	u64 heat_feedback_sent;
	u64 heat_feedback_phase_a;
	u64 heat_feedback_phase_b;
	u64 heat_frames;
	u64 heat_errors;
	u64 component_total;
	u64 contact_total;
	u64 weak_rejections;
	u64 nsr_rejections;
	u64 palm_rejections;
	u64 assignment_matches;
	u64 new_tracks;
	u64 output_frames;
	u64 output_contacts;
	u64 processing_ns_total;
	u64 processing_ns_max;
	unsigned long last_reset_jiffies;
	u8 nsr_bin_count;
	enum g6ts_initialization_stage initialization_stage;
	u8 recovery_fail_streak;
	u8 rapid_reset_streak;
	int last_host_fault;
	enum g6ts_recovery_path recovery_path;
	bool nsr_valid;
	bool report_descriptor_valid;
	bool ipts_metadata_valid;
	bool ipts_hid_ready;
	bool awaiting_ready_heat;
	bool mode_enabled;
	bool fatal_transport_error;
	bool stopping;
	bool quiescing;
	bool parity_feedback_required;
	bool parity_config_owner_required;
	bool parity_cfu_owner_required;
	bool parity_cfu_branch_required;
	bool parity_heat_owner_required;
	u8 parity_feature73_early[2];
	u8 parity_feature73_late[2];
	u8 parity_feature06_prefix[16];
	u8 parity_cfu_version_prefix[12];
	u8 parity_cfu_offer_response[G6TS_WINDOWS_CFU_OFFER_LEN];
	bool init_get_feature_dumped[G6TS_INIT_STAGE_COUNT];
};

static bool g6ts_is_quiescing(const struct g6ts *ts)
{
	return READ_ONCE(ts->stopping) || READ_ONCE(ts->quiescing);
}

static bool g6ts_is_raw_export_report(u8 report_id)
{
	switch (report_id) {
	case G6TS_RAW_SIDEBAND_REPORT_07:
	case G6TS_RAW_HEAT_REPORT_0B:
	case G6TS_RAW_HEAT_REPORT_0C:
	case G6TS_RAW_HEAT_REPORT_0D:
	case G6TS_RAW_HEAT_REPORT_1A:
	case G6TS_RAW_SIDEBAND_REPORT_6E:
		return true;
	default:
		return false;
	}
}

/*
 * iptsd consumes only the four core DFT reports. Keep sideband reports on the
 * diagnostic ABI: report 0x6e can contain a device or pen identifier and is
 * not needed by the stylus decoder.
 */
static bool g6ts_is_ipts_hid_report(u8 report_id)
{
	switch (report_id) {
	case G6TS_RAW_HEAT_REPORT_0B:
	case G6TS_RAW_HEAT_REPORT_0C:
	case G6TS_RAW_HEAT_REPORT_0D:
	case G6TS_RAW_HEAT_REPORT_1A:
		return true;
	default:
		return false;
	}
}

/* io_lock is held by every transport-generation caller. */
static void g6ts_hid_generation_boundary(struct g6ts *ts)
{
	ts->hid_target_generation++;
	if (!ts->hid_target_generation)
		ts->hid_target_generation = 1;
	ts->hid_rebind_failures = 0;

	/* Never destroy hidraw from a feature ioctl or IRQ/reset call stack. */
	mod_delayed_work(system_wq, &ts->hid_rebind_work, 0);
}

static void g6ts_heat_device_release(struct kref *ref)
{
	struct g6ts_heat_device *heat =
		container_of(ref, struct g6ts_heat_device, ref);

	kvfree(heat->frames);
	kfree(heat);
}

static void g6ts_heat_enqueue_locked(struct g6ts_heat_device *heat,
				     u8 report_id, u8 flags,
				     const u8 *content, u16 content_len)
{
	struct g6ts_heat_frame *frame;
	unsigned int tail;

	if (heat->count == G6TS_HEAT_QUEUE_CAPACITY) {
		heat->head = (heat->head + 1) % G6TS_HEAT_QUEUE_CAPACITY;
		heat->count--;
		heat->records_dropped++;
	}

	tail = (heat->head + heat->count) % G6TS_HEAT_QUEUE_CAPACITY;
	frame = &heat->frames[tail];
	frame->header.magic = cpu_to_le32(G6TS_HEAT_RECORD_MAGIC);
	frame->header.abi_version = cpu_to_le16(G6TS_HEAT_ABI_VERSION);
	frame->header.header_len = cpu_to_le16(sizeof(frame->header));
	frame->header.record_len =
		cpu_to_le32(sizeof(frame->header) + content_len);
	frame->header.generation = cpu_to_le32(heat->generation);
	frame->header.timestamp_ns = cpu_to_le64(ktime_get_ns());
	frame->header.sequence = cpu_to_le32(heat->sequence++);
	frame->header.content_len = cpu_to_le16(content_len);
	frame->header.report_id = report_id;
	frame->header.flags = flags;
	if (content_len)
		memcpy(frame->content, content, content_len);

	heat->count++;
	heat->records_enqueued++;
	switch (report_id) {
	case G6TS_RAW_HEAT_REPORT_0B:
		heat->report_counts[0]++;
		break;
	case G6TS_RAW_HEAT_REPORT_0C:
		heat->report_counts[1]++;
		break;
	case G6TS_RAW_HEAT_REPORT_0D:
		heat->report_counts[2]++;
		break;
	case G6TS_RAW_HEAT_REPORT_1A:
		heat->report_counts[3]++;
		break;
	}
}

static void g6ts_heat_enqueue(struct g6ts *ts, u8 report_id,
			      const u8 *content, u16 content_len)
{
	struct g6ts_heat_device *heat = ts->heat;

	if (!heat)
		return;

	mutex_lock(&heat->lock);
	if (heat->dead) {
		mutex_unlock(&heat->lock);
		return;
	}
	if (content_len > G6TS_HEAT_MAX_CONTENT_SIZE) {
		heat->oversize_drops++;
		mutex_unlock(&heat->lock);
		return;
	}
	g6ts_heat_enqueue_locked(heat, report_id, 0, content, content_len);
	mutex_unlock(&heat->lock);
	wake_up_interruptible(&heat->read_wait);
}

static void g6ts_heat_generation_boundary(struct g6ts *ts, u8 flags)
{
	struct g6ts_heat_device *heat = ts->heat;

	ts->a5.cycle_saw_0x0d = false;
	g6ts_hid_generation_boundary(ts);
	if (!heat)
		return;

	mutex_lock(&heat->lock);
	if (heat->dead) {
		mutex_unlock(&heat->lock);
		return;
	}
	heat->generation++;
	if (!heat->generation)
		heat->generation = 1;
	heat->head = 0;
	heat->count = 0;
	heat->queue_flushes++;
	g6ts_heat_enqueue_locked(heat, 0, flags, NULL, 0);
	mutex_unlock(&heat->lock);
	wake_up_interruptible(&heat->read_wait);
}

static bool g6ts_heat_read_ready(struct g6ts_heat_device *heat)
{
	return READ_ONCE(heat->count) || READ_ONCE(heat->dead);
}

static int g6ts_heat_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct g6ts_heat_device *heat =
		container_of(miscdev, struct g6ts_heat_device, miscdev);

	if (!kref_get_unless_zero(&heat->ref))
		return -ENODEV;
	if (atomic_cmpxchg(&heat->opened, 0, 1)) {
		kref_put(&heat->ref, g6ts_heat_device_release);
		return -EBUSY;
	}

	mutex_lock(&heat->lock);
	if (heat->dead) {
		mutex_unlock(&heat->lock);
		atomic_set(&heat->opened, 0);
		kref_put(&heat->ref, g6ts_heat_device_release);
		return -ENODEV;
	}
	file->private_data = heat;
	mutex_unlock(&heat->lock);
	return nonseekable_open(inode, file);
}

static int g6ts_heat_release(struct inode *inode, struct file *file)
{
	struct g6ts_heat_device *heat = file->private_data;

	atomic_set(&heat->opened, 0);
	kref_put(&heat->ref, g6ts_heat_device_release);
	return 0;
}

static ssize_t g6ts_heat_read(struct file *file, char __user *buffer,
			      size_t size, loff_t *offset)
{
	struct g6ts_heat_device *heat = file->private_data;
	struct g6ts_heat_frame *frame;
	size_t content_len;
	size_t record_len;
	int ret;

	for (;;) {
		if (!(file->f_flags & O_NONBLOCK)) {
			ret = wait_event_interruptible(heat->read_wait,
						       g6ts_heat_read_ready(heat));
			if (ret)
				return ret;
		}

		mutex_lock(&heat->lock);
		if (heat->count)
			break;
		if (heat->dead) {
			mutex_unlock(&heat->lock);
			return 0;
		}
		mutex_unlock(&heat->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
	}

	frame = &heat->frames[heat->head];
	record_len = le32_to_cpu(frame->header.record_len);
	content_len = le16_to_cpu(frame->header.content_len);
	if (size < record_len) {
		ret = -EMSGSIZE;
		goto out_unlock;
	}
	if (copy_to_user(buffer, &frame->header, sizeof(frame->header)) ||
	    (content_len &&
	     copy_to_user(buffer + sizeof(frame->header), frame->content,
			  content_len))) {
		ret = -EFAULT;
		goto out_unlock;
	}

	heat->head = (heat->head + 1) % G6TS_HEAT_QUEUE_CAPACITY;
	heat->count--;
	mutex_unlock(&heat->lock);
	return record_len;

out_unlock:
	mutex_unlock(&heat->lock);
	return ret;
}

static __poll_t g6ts_heat_poll(struct file *file, poll_table *wait)
{
	struct g6ts_heat_device *heat = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &heat->read_wait, wait);
	mutex_lock(&heat->lock);
	if (heat->count)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (heat->dead)
		mask |= EPOLLHUP;
	mutex_unlock(&heat->lock);
	return mask;
}

static long g6ts_heat_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct g6ts_heat_device *heat = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case G6TS_HEAT_IOC_GET_INFO: {
		struct g6ts_heat_info info = {
			.abi_version = cpu_to_le16(G6TS_HEAT_ABI_VERSION),
			.struct_size = cpu_to_le16(sizeof(info)),
			.record_header_size =
				cpu_to_le16(sizeof(struct g6ts_heat_record_header)),
			.max_content_size =
				cpu_to_le32(G6TS_HEAT_MAX_CONTENT_SIZE),
			.queue_capacity =
				cpu_to_le32(G6TS_HEAT_QUEUE_CAPACITY),
			.supported_record_flags =
				cpu_to_le64(G6TS_HEAT_RECORD_F_RESET |
					    G6TS_HEAT_RECORD_F_SUSPEND |
					    G6TS_HEAT_RECORD_F_TRANSPORT_FAULT),
		};

		return copy_to_user(argp, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case G6TS_HEAT_IOC_GET_STATS: {
		struct g6ts_heat_stats stats = {
			.abi_version = cpu_to_le16(G6TS_HEAT_ABI_VERSION),
			.struct_size = cpu_to_le16(sizeof(stats)),
			.queue_capacity =
				cpu_to_le32(G6TS_HEAT_QUEUE_CAPACITY),
		};

		mutex_lock(&heat->lock);
		stats.generation = cpu_to_le32(heat->generation);
		stats.queued_records = cpu_to_le32(heat->count);
		stats.records_enqueued = cpu_to_le64(heat->records_enqueued);
		stats.records_dropped = cpu_to_le64(heat->records_dropped);
		stats.queue_flushes = cpu_to_le64(heat->queue_flushes);
		stats.oversize_drops = cpu_to_le64(heat->oversize_drops);
		stats.report_0b = cpu_to_le64(heat->report_counts[0]);
		stats.report_0c = cpu_to_le64(heat->report_counts[1]);
		stats.report_0d = cpu_to_le64(heat->report_counts[2]);
		stats.report_1a = cpu_to_le64(heat->report_counts[3]);
		mutex_unlock(&heat->lock);

		return copy_to_user(argp, &stats, sizeof(stats)) ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations g6ts_heat_fops = {
	.owner = THIS_MODULE,
	.open = g6ts_heat_open,
	.release = g6ts_heat_release,
	.read = g6ts_heat_read,
	.poll = g6ts_heat_poll,
	.unlocked_ioctl = g6ts_heat_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static void g6ts_heat_unregister(void *data)
{
	struct g6ts_heat_device *heat = data;

	misc_deregister(&heat->miscdev);
	mutex_lock(&heat->lock);
	heat->dead = true;
	heat->head = 0;
	heat->count = 0;
	mutex_unlock(&heat->lock);
	wake_up_interruptible(&heat->read_wait);
	kref_put(&heat->ref, g6ts_heat_device_release);
}

static int g6ts_heat_register(struct g6ts *ts)
{
	struct g6ts_heat_device *heat;
	int ret;

	heat = kzalloc_obj(*heat);
	if (!heat)
		return -ENOMEM;
	heat->frames = kvcalloc(G6TS_HEAT_QUEUE_CAPACITY,
				sizeof(*heat->frames), GFP_KERNEL);
	if (!heat->frames) {
		kfree(heat);
		return -ENOMEM;
	}

	mutex_init(&heat->lock);
	init_waitqueue_head(&heat->read_wait);
	kref_init(&heat->ref);
	atomic_set(&heat->opened, 0);
	heat->generation = 1;
	heat->sequence = 1;
	heat->miscdev.minor = MISC_DYNAMIC_MINOR;
	heat->miscdev.name = "g6ts-heat";
	heat->miscdev.fops = &g6ts_heat_fops;
	heat->miscdev.parent = &ts->spi->dev;

	ret = misc_register(&heat->miscdev);
	if (ret) {
		kref_put(&heat->ref, g6ts_heat_device_release);
		return ret;
	}
	ts->heat = heat;
	ret = devm_add_action_or_reset(&ts->spi->dev, g6ts_heat_unregister,
				       heat);
	if (ret)
		ts->heat = NULL;
	return ret;
}

static const char *
g6ts_initialization_stage_name(enum g6ts_initialization_stage stage)
{
	switch (stage) {
	case G6TS_INIT_IDLE:
		return "idle";
	case G6TS_INIT_WINDOWS_POWER_PS0:
		return "windows-power-ps0";
	case G6TS_INIT_WINDOWS_RESET_METHOD:
		return "windows-reset-method";
	case G6TS_INIT_RESET_RESPONSE:
		return "reset-response";
	case G6TS_INIT_DEVICE_DESCRIPTOR:
		return "device-descriptor";
	case G6TS_INIT_REPORT_DESCRIPTOR:
		return "report-descriptor";
	case G6TS_INIT_WINDOWS_EARLY_FEATURE73:
		return "windows-early-feature73";
	case G6TS_INIT_WINDOWS_HEAT_CAPS06:
		return "windows-heat-caps06";
	case G6TS_INIT_WINDOWS_FEEDBACK_REQUIRED:
		return "windows-feedback-required";
	case G6TS_INIT_WINDOWS_FEEDBACK_A1:
		return "windows-feedback-a1";
	case G6TS_INIT_WINDOWS_FEEDBACK_A5:
		return "windows-feedback-a5";
	case G6TS_INIT_WINDOWS_SET_FEATURE05:
		return "windows-set-feature05";
	case G6TS_INIT_WINDOWS_CONFIG_OWNER_REQUIRED:
		return "windows-config-owner-required";
	case G6TS_INIT_WINDOWS_GET_FEATURE70:
		return "windows-get-feature70";
	case G6TS_INIT_WINDOWS_SET_FEATURE70:
		return "windows-set-feature70";
	case G6TS_INIT_WINDOWS_SET_FEATURE56:
		return "windows-set-feature56";
	case G6TS_INIT_WINDOWS_CFU_OWNER_REQUIRED:
		return "windows-cfu-owner-required";
	case G6TS_INIT_WINDOWS_CFU_GET_VERSION:
		return "windows-cfu-get-version";
	case G6TS_INIT_WINDOWS_CFU_START_TRANSACTION:
		return "windows-cfu-start-transaction";
	case G6TS_INIT_WINDOWS_CFU_START_LIST:
		return "windows-cfu-start-list";
	case G6TS_INIT_WINDOWS_CFU_OFFER:
		return "windows-cfu-offer";
	case G6TS_INIT_WINDOWS_CFU_BRANCH_REQUIRED:
		return "windows-cfu-branch-required";
	case G6TS_INIT_WINDOWS_CFU_END_LIST:
		return "windows-cfu-end-list";
	case G6TS_INIT_WINDOWS_FINAL_FEATURE73:
		return "windows-final-feature73";
	case G6TS_INIT_WINDOWS_HEAT_OWNER_REQUIRED:
		return "windows-heat-owner-required";
	case G6TS_INIT_SET_FEATURE05:
		return "set-feature05";
	case G6TS_INIT_GET_FEATURE70:
		return "get-feature70";
	case G6TS_INIT_SET_FEATURE70:
		return "set-feature70";
	case G6TS_INIT_SET_FEATURE56:
		return "set-feature56";
	case G6TS_INIT_WAIT_HEAT:
		return "wait-heat";
	}

	return "invalid";
}

static const char *g6ts_profile_name(void)
{
	if (g6ts_windows_init_parity)
		return "windows-init-parity";
	if (g6ts_ready_quiesce && g6ts_feature70_one_byte)
		return "phase82";
	if (g6ts_ready_quiesce)
		return "phase81";
	if (g6ts_host_fault_recovery)
		return "phase80";
	if (g6ts_feature70_one_byte)
		return "phase79";
	if (g6ts_reset_storm_breaker)
		return "phase78";
	if (g6ts_reset_recovery_v2)
		return "phase77";
	if (g6ts_behavior_v2)
		return "phase76";
	if (g6ts_windows_orchestrator)
		return "windows-orchestrator";
	return "phase75";
}

static ssize_t behavior_stats_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct g6ts *ts = dev_get_drvdata(dev);
	u64 average_ns;
	ssize_t length;

	mutex_lock(&ts->io_lock);
	average_ns = ts->heat_frames ?
		div64_u64(ts->processing_ns_total, ts->heat_frames) : 0;
	length = sysfs_emit(buf,
			    "profile=%s\n"
			    "parity_linux_power=%u\n"
			    "windows_read_cadence=%u\n"
			    "initialization_stage=%s\n"
			    "parity_feedback_required=%u\n"
			    "parity_config_owner_required=%u\n"
			    "parity_cfu_owner_required=%u\n"
			    "parity_cfu_branch_required=%u\n"
			    "parity_heat_owner_required=%u\n"
			    "parity_feature73_early=%*ph\n"
			    "parity_feature73_late=%*ph\n"
			    "parity_feature06_prefix=%*ph\n"
			    "parity_cfu_version_prefix=%*ph\n"
			    "parity_cfu_offer_response=%*ph\n"
			    "last_header=%4ph\n"
			    "last_class=%u\n"
			    "last_content_id=%u\n"
			    "last_content_len=%u\n"
			    "mode_enabled=%u\n"
			    "awaiting_ready_heat=%u\n"
			    "heat_frames=%llu\n"
			    "heat_errors=%llu\n"
			    "components=%llu\n"
			    "accepted_contacts=%llu\n"
			    "weak_rejections=%llu\n"
			    "nsr_rejections=%llu\n"
			    "palm_rejections=%llu\n"
			    "assignment_matches=%llu\n"
			    "new_tracks=%llu\n"
			    "output_frames=%llu\n"
			    "output_contacts=%llu\n"
			    "processing_average_ns=%llu\n"
			    "processing_max_ns=%llu\n"
			    "panel_resets=%llu\n"
			    "recovery_successes=%llu\n"
			    "recovery_failures=%llu\n"
			    "hardware_recovery_attempts=%llu\n"
			    "software_recovery_attempts=%llu\n"
			    "software_recovery_fallbacks=%llu\n"
			    "reset_storm_escalations=%llu\n"
			    "rapid_reset_streak=%u\n"
			    "host_fault_recoveries=%llu\n"
			    "irq_transport_errors=%llu\n"
			    "irq_protocol_errors=%llu\n"
			    "irq_drain_overflows=%llu\n"
			    "quiesced_empty_reads=%llu\n"
			    "cadence_single_response_irqs=%llu\n"
			    "last_host_fault=%d\n"
			    "ready_heat_frames=%llu\n"
			    "ready_verification_failures=%llu\n",
			    g6ts_profile_name(),
			    g6ts_parity_linux_power,
			    g6ts_windows_read_cadence,
			    g6ts_initialization_stage_name(ts->initialization_stage),
			    ts->parity_feedback_required,
			    ts->parity_config_owner_required,
			    ts->parity_cfu_owner_required,
			    ts->parity_cfu_branch_required,
			    ts->parity_heat_owner_required,
			    (int)sizeof(ts->parity_feature73_early),
			    ts->parity_feature73_early,
			    (int)sizeof(ts->parity_feature73_late),
			    ts->parity_feature73_late,
			    (int)sizeof(ts->parity_feature06_prefix),
			    ts->parity_feature06_prefix,
			    (int)sizeof(ts->parity_cfu_version_prefix),
			    ts->parity_cfu_version_prefix,
			    (int)sizeof(ts->parity_cfu_offer_response),
			    ts->parity_cfu_offer_response,
			    ts->last_header, ts->last_class,
			    ts->last_content_id, ts->last_content_len,
			    READ_ONCE(ts->mode_enabled), ts->awaiting_ready_heat,
			    ts->heat_frames, ts->heat_errors,
			    ts->component_total, ts->contact_total,
			    ts->weak_rejections, ts->nsr_rejections,
			    ts->palm_rejections, ts->assignment_matches,
			    ts->new_tracks, ts->output_frames,
			    ts->output_contacts, average_ns,
			    ts->processing_ns_max, ts->reset_notifications,
			    ts->recovery_successes, ts->recovery_failures,
			    ts->hardware_recovery_attempts,
			    ts->software_recovery_attempts,
			    ts->software_recovery_fallbacks,
			    ts->reset_storm_escalations,
			    ts->rapid_reset_streak,
			    ts->host_fault_recoveries,
			    ts->irq_transport_errors,
			    ts->irq_protocol_errors,
			    ts->irq_drain_overflows,
			    ts->quiesced_empty_reads,
			    ts->cadence_single_response_irqs,
			    ts->last_host_fault,
			    ts->ready_heat_frames,
			    ts->ready_verification_failures);
	mutex_unlock(&ts->io_lock);

	return length;
}
static DEVICE_ATTR_RO(behavior_stats);

static ssize_t report_counts_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct g6ts *ts = dev_get_drvdata(dev);
	ssize_t length = 0;
	unsigned int report_id;

	mutex_lock(&ts->io_lock);
	for (report_id = 0; report_id <= U8_MAX; report_id++) {
		if (!ts->report_counts[report_id])
			continue;
		length += sysfs_emit_at(buf, length, "0x%02x %llu\n",
					report_id, ts->report_counts[report_id]);
		if (length >= PAGE_SIZE - 1)
			break;
	}
	mutex_unlock(&ts->io_lock);

	return length;
}
static DEVICE_ATTR_RO(report_counts);

static ssize_t feedback_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct g6ts *ts = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&ts->io_lock);
	length = sysfs_emit(buf,
			    "seq %u\n"
			    "sent %llu phase_a %llu phase_b %llu\n",
			    ts->a5.sequence, ts->heat_feedback_sent,
			    ts->heat_feedback_phase_a,
			    ts->heat_feedback_phase_b);
	mutex_unlock(&ts->io_lock);

	return length;
}
static DEVICE_ATTR_RO(feedback_state);

static struct attribute *g6ts_attributes[] = {
	&dev_attr_behavior_stats.attr,
	&dev_attr_report_counts.attr,
	&dev_attr_feedback_state.attr,
	NULL,
};

static const struct attribute_group g6ts_attribute_group = {
	.attrs = g6ts_attributes,
};

static int g6ts_acpi_method(struct device *dev, const char *method)
{
	acpi_handle handle = ACPI_HANDLE(dev);
	acpi_status status;

	if (!handle)
		return -ENODEV;
	status = acpi_evaluate_object(handle, (char *)method, NULL, NULL);
	return ACPI_FAILURE(status) ? -EIO : 0;
}

static int g6ts_power_on(struct g6ts *ts)
{
	int ret;

	if (ts->power_gpio && ts->reset_gpio) {
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		gpiod_set_value_cansleep(ts->power_gpio, 1);
		msleep(500);
		gpiod_set_value_cansleep(ts->reset_gpio, 1);
		msleep(300);
		return 0;
	}

	ret = g6ts_acpi_method(&ts->spi->dev, "_PS0");
	if (ret)
		return dev_err_probe(&ts->spi->dev, ret,
				     "ACPI _PS0 failed\n");

	ret = g6ts_acpi_method(&ts->spi->dev, "_RST");
	if (ret) {
		g6ts_acpi_method(&ts->spi->dev, "_PS3");
		return dev_err_probe(&ts->spi->dev, ret,
				     "ACPI _RST failed\n");
	}

	return 0;
}

/*
 * Reproduce the GPIO effects and delays in the SP11 GTCH _PS0 followed by
 * _RST. The production helper above retains its hardware-validated sequence;
 * only the input-disabled parity path uses this ACPI-derived ordering.
 */
static int g6ts_windows_power_on(struct g6ts *ts)
{
	int ret;

	if (!ts->power_gpio || !ts->reset_gpio) {
		ts->initialization_stage = G6TS_INIT_WINDOWS_POWER_PS0;
		ret = g6ts_acpi_method(&ts->spi->dev, "_PS0");
		if (ret)
			return dev_err_probe(&ts->spi->dev, ret,
					     "ACPI _PS0 failed\n");

		ts->initialization_stage = G6TS_INIT_WINDOWS_RESET_METHOD;
		ret = g6ts_acpi_method(&ts->spi->dev, "_RST");
		if (ret) {
			g6ts_acpi_method(&ts->spi->dev, "_PS3");
			return dev_err_probe(&ts->spi->dev, ret,
					     "ACPI _RST failed\n");
		}
		return 0;
	}

	ts->initialization_stage = G6TS_INIT_WINDOWS_POWER_PS0;
	gpiod_set_value_cansleep(ts->power_gpio, 1);
	msleep(500);
	gpiod_set_value_cansleep(ts->reset_gpio, 1);

	ts->initialization_stage = G6TS_INIT_WINDOWS_RESET_METHOD;
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(300);
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	return 0;
}

static int g6ts_power_off(struct g6ts *ts)
{
	if (ts->power_gpio && ts->reset_gpio) {
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		usleep_range(10000, 12000);
		gpiod_set_value_cansleep(ts->power_gpio, 0);
		return 0;
	}

	return g6ts_acpi_method(&ts->spi->dev, "_PS3");
}

/*
 * Submit RX first and TX second through the controller's SP11 QSPI pairing
 * path. The ordering and 1-4-4 lane selection match the Windows GPI capture.
 */
static int g6ts_dma_read_pair(struct g6ts *ts, const u8 cmd[8],
			      void *rx, size_t rx_len)
{
	struct spi_transfer xfers[2] = {
		{
			.tx_buf = cmd,
			.len = 8,
			.speed_hz = G6TS_SPI_HZ,
			.bits_per_word = 8,
			.tx_nbits = SPI_NBITS_QUAD,
		}, {
			.rx_buf = rx,
			.len = rx_len,
			.speed_hz = G6TS_SPI_HZ,
			.bits_per_word = 8,
			.rx_nbits = SPI_NBITS_QUAD,
		},
	};
	struct spi_message msg;

	spi_message_init_with_transfers(&msg, xfers, ARRAY_SIZE(xfers));
	return spi_sync(ts->spi, &msg);
}

static int g6ts_dma_output(struct g6ts *ts, const void *buf, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = buf,
		.len = len,
		.speed_hz = G6TS_SPI_HZ,
		.bits_per_word = 8,
		.tx_nbits = SPI_NBITS_QUAD,
	};
	struct spi_message msg;

	spi_message_init_with_transfers(&msg, &xfer, 1);
	return spi_sync(ts->spi, &msg);
}

static int g6ts_dma_hidspi_output(struct g6ts *ts, u8 report_type,
				  u8 content_id, const u8 *content,
				  size_t content_len)
{
	u8 packet[72] = { 0xe2, 0x00, 0x20, 0x00 };
	size_t packet_len;

	if (content_len > sizeof(packet) - 8)
		return -E2BIG;

	packet[4] = report_type;
	put_unaligned_le16(content_len, &packet[5]);
	packet[7] = content_id;
	if (content_len)
		memcpy(&packet[8], content, content_len);
	packet_len = round_up(8 + content_len, 4);

	dev_dbg(&ts->spi->dev,
		"G6TS DMA output type=%u id=%#02x content_len=%zu wire=%*ph\n",
		report_type, content_id, content_len, (int)packet_len, packet);
	return g6ts_dma_output(ts, packet, packet_len);
}

static int g6ts_pending(struct g6ts *ts)
{
	return gpiod_get_value_cansleep(ts->interrupt_gpio);
}

static bool g6ts_has_unread_response(struct g6ts *ts)
{
	int pending = g6ts_pending(ts);

	return pending > 0 ||
	       atomic64_read(&ts->interrupt_edges) > ts->handled_interrupt_edges;
}

static irqreturn_t g6ts_interrupt_edge(int irq, void *data)
{
	struct g6ts *ts = data;

	atomic64_inc(&ts->interrupt_edges);
	return IRQ_WAKE_THREAD;
}

static int g6ts_wait_pending(struct g6ts *ts, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int value;

	do {
		value = g6ts_pending(ts);
		if (value < 0 || value ||
		    atomic64_read(&ts->interrupt_edges) >
		    ts->handled_interrupt_edges)
			return value < 0 ? value : 0;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));

	return -ETIMEDOUT;
}

static void g6ts_clear_last_response(struct g6ts *ts)
{
	ts->last_class = 0xff;
	ts->last_content_len = 0;
	ts->last_content_id = 0xff;
}

static int g6ts_extract_nsr_metadata(struct g6ts *ts, const u8 *section,
				     size_t section_len)
{
	size_t position = 7;

	/* Windows starts its nested metadata dispatcher at section byte seven. */
	while (position < section_len) {
		size_t payload_len, record_end;
		const u8 *record;
		unsigned int count, i;

		if (section_len - position < 4)
			return -EPROTO;
		record = section + position;
		payload_len = get_unaligned_le16(record + 2);
		if (payload_len > section_len - position - 4)
			return -EPROTO;
		record_end = position + 4 + payload_len;

		if (record[0] == 0x04) {
			if (ts->nsr_valid || payload_len < 4)
				return -EPROTO;
			count = record[4];
			if (count > G6TS_NSR_BINS ||
			    count > (payload_len - 4) / 4)
				return -EPROTO;
			memset(ts->nsr_bins, 0, sizeof(ts->nsr_bins));
			for (i = 0; i < count; i++) {
				u16 value = get_unaligned_le16(record + 8 + i * 4);

				ts->nsr_bins[i] = value;
			}
			ts->nsr_bin_count = count;
			ts->nsr_valid = true;
		}
		position = record_end;
	}

	return 0;
}

static int g6ts_extract_heatmap(struct g6ts *ts, const u8 *content,
				size_t content_len)
{
	size_t container_end, offset;
	u32 container_len;
	bool found = false;

	/*
	 * Report 0x12 begins with a two-byte scan time, followed by a Heat
	 * container.  Its seven-byte container header is followed by length-
	 * prefixed sections.  Windows asks IHeatFrameRawData for section 0x0100.
	 */
	if (content_len < 9)
		return -EMSGSIZE;
	container_len = get_unaligned_le32(content + 2);
	if (container_len < 7 || container_len > content_len - 2)
		return -EPROTO;
	container_end = 2 + container_len;
	offset = 9;
	ts->nsr_valid = false;
	ts->nsr_bin_count = 0;

	while (offset < container_end) {
		const u8 *section = content + offset;
		size_t section_end, position;
		u32 section_len;
		u16 section_type;
		u32 written = 0;

		if (container_end - offset < 8)
			return -EPROTO;
		section_len = get_unaligned_le32(section);
		if (section_len < 8 || section_len > container_end - offset)
			return -EPROTO;
		section_end = offset + section_len;
		section_type = get_unaligned_le16(section + 4);

		if (section_type == G6TS_METADATA_SECTION) {
			int ret = g6ts_extract_nsr_metadata(ts, section,
							    section_len);

			if (ret)
				return ret;
			offset = section_end;
			continue;
		}
		if (section_type != G6TS_HEAT_SECTION) {
			offset = section_end;
			continue;
		}
		if (found || section[6] != 1 || section[7] != 8)
			return -EPROTO;

		memset(ts->heatmap, 0, sizeof(ts->heatmap));
		memset(ts->heat_seen, 0, sizeof(ts->heat_seen));
		position = offset + 8;
		while (position < section_end) {
			u32 destination, count;
			u32 i;

			if (section_end - position < 8)
				return -EPROTO;
			destination = get_unaligned_le32(content + position);
			count = get_unaligned_le32(content + position + 4);
			position += 8;
			if (count > section_end - position ||
			    count > G6TS_HEAT_SAMPLES ||
			    destination > G6TS_HEAT_SAMPLES - count)
				return -EPROTO;
			for (i = 0; i < count; i++) {
				if (ts->heat_seen[destination + i])
					return -EPROTO;
				ts->heat_seen[destination + i] = 1;
			}
			memcpy(ts->heatmap + destination, content + position, count);
			written += count;
			position += count;
		}
		if (written != G6TS_HEAT_SAMPLES)
			return -EPROTO;
		found = true;
		offset = section_end;
	}

	return found ? 0 : -ENOENT;
}

static bool g6ts_heat_active(const struct g6ts *ts, unsigned int index)
{
	return ts->heatmap[index] <= G6TS_HEAT_ACTIVE_MAX;
}

static void g6ts_store_contact(struct g6ts *ts,
			       const struct g6ts_contact *contact,
			       unsigned int *contact_count)
{
	unsigned int weakest = 0;
	unsigned int i;

	if (*contact_count < G6TS_MAX_CONTACTS) {
		ts->contacts[(*contact_count)++] = *contact;
		return;
	}

	for (i = 1; i < G6TS_MAX_CONTACTS; i++)
		if (ts->contacts[i].strength < ts->contacts[weakest].strength)
			weakest = i;
	if (contact->strength > ts->contacts[weakest].strength)
		ts->contacts[weakest] = *contact;
}

static s32 g6ts_signal_q12(u8 value)
{
	s32 signal_q24 = G6TS_SIGNAL_INTERCEPT_Q24 -
			 value * G6TS_SIGNAL_STEP_Q24;

	return (signal_q24 + BIT(G6TS_CLASSIFIER_SHIFT - 1)) >>
		G6TS_CLASSIFIER_SHIFT;
}

/*
 * FUN_180047078 recomputes the normal output centroid relative to project
 * baseline index 171.  On the ordinary zero-context path captured for project
 * 0x0c83, empty cells outside the thresholded component cannot have positive
 * weight against that baseline.  The expanded-window traversal therefore
 * reduces exactly to this component-local weighted centroid.  Keep the
 * detector centroid in contact->x/y for assignment and place this later
 * output-stage position in contact->output_x/y.
 */
static void g6ts_phase76_output_centroid(struct g6ts *ts,
					 struct g6ts_contact *contact)
{
	u64 weighted_x = 0, weighted_y = 0;
	u64 weight_total = 0;
	unsigned int index;

	for (index = 0; index < G6TS_HEAT_SAMPLES; index++) {
		u8 value;
		u32 weight;

		if (!ts->heat_component[index])
			continue;
		value = ts->heatmap[index];
		if (value >= G6TS_WINDOWS_CENTROID_BASELINE)
			continue;
		weight = G6TS_WINDOWS_CENTROID_BASELINE - value;
		weight_total += weight;
		weighted_x += (u64)(index % G6TS_HEAT_COLS) * weight;
		weighted_y += (u64)(index / G6TS_HEAT_COLS) * weight;
	}
	if (!weight_total)
		return;

	contact->output_x = div_u64(weighted_x * G6TS_LOGICAL_MAX,
				    weight_total * (G6TS_HEAT_COLS - 1));
	contact->output_y = div_u64(weighted_y * G6TS_LOGICAL_MAX,
				    weight_total * (G6TS_HEAT_ROWS - 1));
}

/*
 * Mirror FUN_180040438's candidate +0x4d local-maximum producer and
 * FUN_180041fd8's +0x4e strict 0.04 filter.  The DLL's equality epsilon is
 * 0.0001 while adjacent byte lookup values differ by about 0.00222, so Q12
 * ordering is identical for this byte-input path.
 */
static void g6ts_local_peak_counts(const struct g6ts *ts,
				   struct g6ts_contact *contact)
{
	static const s8 neighbours[][2] = {
		{ -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
	};
	unsigned int index;

	contact->local_peak_count = 0;
	contact->strong_local_peak_count = 0;
	for (index = 0; index < G6TS_HEAT_SAMPLES; index++) {
		unsigned int row, col, equal_index = UINT_MAX;
		s32 current_signal, equal_signal = 0;
		unsigned int lower = 0, near_equal = 0, n;
		bool selected;

		if (!ts->heat_component[index])
			continue;
		row = index / G6TS_HEAT_COLS;
		col = index % G6TS_HEAT_COLS;
		current_signal = g6ts_signal_q12(ts->heatmap[index]);
		for (n = 0; n < ARRAY_SIZE(neighbours); n++) {
			int neighbour_col = col + neighbours[n][0];
			int neighbour_row = row + neighbours[n][1];
			unsigned int neighbour_index;
			s32 neighbour_signal;

			if (neighbour_row < 0 || neighbour_row >= G6TS_HEAT_ROWS ||
			    neighbour_col < 0 || neighbour_col >= G6TS_HEAT_COLS) {
				neighbour_index = UINT_MAX;
				neighbour_signal = 0;
			} else {
				neighbour_index = neighbour_row * G6TS_HEAT_COLS +
						  neighbour_col;
				neighbour_signal =
					g6ts_signal_q12(ts->heatmap[neighbour_index]);
			}
			if (current_signal > neighbour_signal) {
				lower++;
			} else if (current_signal == neighbour_signal) {
				near_equal++;
				equal_index = neighbour_index;
				equal_signal = neighbour_signal;
			}
		}

		selected = lower == 4;
		if (lower == 3 && near_equal == 1)
			selected = equal_signal < current_signal ||
				   (equal_signal == current_signal &&
				    index < equal_index);
		if (!selected ||
		    contact->local_peak_count >= G6TS_LOCAL_PEAK_CAPACITY)
			continue;
		contact->local_peak_count++;
		if (current_signal > G6TS_LOCAL_PEAK_FLOOR_Q12)
			contact->strong_local_peak_count++;
	}
}

static u8 g6ts_secondary_cutoff(u8 peak_value, unsigned int pass)
{
	static const u16 fractions_q12[] = { 2048, 3072, 3584 };
	u32 fraction = fractions_q12[pass];
	u64 cutoff_q24;
	u32 cutoff_q12;

	cutoff_q24 = (u64)((1U << G6TS_CLASSIFIER_SHIFT) - fraction) *
		      G6TS_SECONDARY_A_Q12;
	cutoff_q24 += (u64)fraction *
		       ((peak_value << G6TS_CLASSIFIER_SHIFT) +
			G6TS_SECONDARY_NOISE_Q12);
	cutoff_q12 = (cutoff_q24 + BIT(G6TS_CLASSIFIER_SHIFT - 1)) >>
		      G6TS_CLASSIFIER_SHIFT;
	return min_t(u32, U8_MAX,
		     (cutoff_q12 + BIT(G6TS_CLASSIFIER_SHIFT - 1)) >>
		     G6TS_CLASSIFIER_SHIFT);
}

static bool g6ts_secondary_neighbour(const struct g6ts *ts, int row, int col,
				     u8 cutoff)
{
	unsigned int index;

	if (row < 0 || row >= G6TS_HEAT_ROWS ||
	    col < 0 || col >= G6TS_HEAT_COLS)
		return false;
	index = row * G6TS_HEAT_COLS + col;
	return ts->heat_component[index] && !ts->heat_work[index] &&
	       ts->heatmap[index] <= cutoff;
}

static void g6ts_secondary_features(struct g6ts *ts,
				    struct g6ts_contact *contact)
{
	static const s8 neighbours[][2] = {
		{ -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 },
	};
	unsigned int area = (contact->max_col - contact->min_col + 1) *
			    (contact->max_row - contact->min_row + 1);
	unsigned int pass;

	for (pass = 0; pass < 3; pass++) {
		contact->features_q12[1 + pass] =
			contact->pixels << G6TS_CLASSIFIER_SHIFT;
		contact->features_q12[4 + pass] = 1U << G6TS_CLASSIFIER_SHIFT;
	}
	/* FUN_180041fd8 only reruns bounded candidates above 0.05 + 0.02. */
	if (area >= 0x4e3 || g6ts_signal_q12(contact->peak_value) <= 287)
		return;

	for (pass = 0; pass < 3; pass++) {
		u8 cutoff = g6ts_secondary_cutoff(contact->peak_value, pass);
		unsigned int accepted = 0, largest = 0, start;

		memset(ts->heat_work, 0, sizeof(ts->heat_work));
		for (start = 0; start < G6TS_HEAT_SAMPLES; start++) {
			unsigned int head = 0, tail = 0, pixels = 0;
			u8 peak = U8_MAX;

			if (!ts->heat_component[start] || ts->heat_work[start] ||
			    ts->heatmap[start] > cutoff)
				continue;
			ts->heat_work[start] = 1;
			ts->heat_queue[tail++] = start;
			while (head < tail) {
				unsigned int index = ts->heat_queue[head++];
				unsigned int row = index / G6TS_HEAT_COLS;
				unsigned int col = index % G6TS_HEAT_COLS;
				unsigned int n;

				pixels++;
				peak = min_t(u8, peak, ts->heatmap[index]);
				for (n = 0; n < ARRAY_SIZE(neighbours); n++) {
					int nr = row + neighbours[n][1];
					int nc = col + neighbours[n][0];
					unsigned int neighbour;

					if (!g6ts_secondary_neighbour(ts, nr, nc, cutoff))
						continue;
					neighbour = nr * G6TS_HEAT_COLS + nc;
					ts->heat_work[neighbour] = 1;
					ts->heat_queue[tail++] = neighbour;
				}
			}
			if (pixels <= 2 &&
			    g6ts_signal_q12(peak) <= G6TS_SECONDARY_SEED_Q12)
				continue;
			accepted++;
			largest = max(largest, pixels);
		}
		contact->features_q12[1 + pass] =
			largest << G6TS_CLASSIFIER_SHIFT;
		contact->features_q12[4 + pass] =
			accepted << G6TS_CLASSIFIER_SHIFT;
	}
}

static void g6ts_geometry_features(struct g6ts *ts,
				   struct g6ts_contact *contact)
{
	u64 sum = 0, sum_x = 0, sum_y = 0;
	u64 sum_xx = 0, sum_yy = 0, sum_xy = 0;
	s64 var_x, var_y, covariance, trace, delta, discriminant;
	u64 denominator;
	u32 major_axis, minor_axis;
	unsigned int index;

	for (index = 0; index < G6TS_HEAT_SAMPLES; index++) {
		u32 row, col;
		s32 signal;

		if (!ts->heat_component[index])
			continue;
		row = index / G6TS_HEAT_COLS;
		col = index % G6TS_HEAT_COLS;
		signal = g6ts_signal_q12(ts->heatmap[index]);
		if (signal <= 0)
			continue;
		sum += signal;
		sum_x += (u64)signal * col;
		sum_y += (u64)signal * row;
		sum_xx += (u64)signal * col * col;
		sum_yy += (u64)signal * row * row;
		sum_xy += (u64)signal * col * row;
	}
	if (!sum) {
		contact->features_q12[7] = 1U << G6TS_CLASSIFIER_SHIFT;
		contact->features_q12[8] = 1U << G6TS_CLASSIFIER_SHIFT;
		return;
	}

	denominator = sum * sum;
	var_x = div64_s64(((s64)(sum_xx * sum) - (s64)(sum_x * sum_x)) *
			    (1U << G6TS_CLASSIFIER_SHIFT), denominator);
	var_y = div64_s64(((s64)(sum_yy * sum) - (s64)(sum_y * sum_y)) *
			    (1U << G6TS_CLASSIFIER_SHIFT), denominator);
	covariance = div64_s64(((s64)(sum_xy * sum) - (s64)(sum_x * sum_y)) *
				 (1U << G6TS_CLASSIFIER_SHIFT), denominator);
	trace = max_t(s64, 0, var_x + var_y);
	delta = var_x - var_y;
	discriminant = int_sqrt64(delta * delta + 4 * covariance * covariance);
	major_axis = int_sqrt64(((trace + discriminant) / 2) <<
				 G6TS_CLASSIFIER_SHIFT);
	minor_axis = int_sqrt64(max_t(s64, 0, (trace - discriminant) / 2) <<
				 G6TS_CLASSIFIER_SHIFT);
	major_axis = max_t(u32, 1U << G6TS_CLASSIFIER_SHIFT,
			   ((u64)major_axis * G6TS_AXIS_SCALE_Q12) >>
			   G6TS_CLASSIFIER_SHIFT);
	minor_axis = max_t(u32, 1U << G6TS_CLASSIFIER_SHIFT,
			   ((u64)minor_axis * G6TS_AXIS_SCALE_Q12) >>
			   G6TS_CLASSIFIER_SHIFT);
	contact->features_q12[7] = div_u64((u64)major_axis <<
					   G6TS_CLASSIFIER_SHIFT, minor_axis);
	if (contact->pixels < 2) {
		contact->features_q12[8] = 1U << G6TS_CLASSIFIER_SHIFT;
	} else {
		s64 spread = trace * G6TS_SPREAD_SCALE_Q12;
		u32 samples = (1U << G6TS_CLASSIFIER_SHIFT) *
			      (contact->pixels - 1);

		contact->features_q12[8] = div64_s64(spread, samples);
	}
}

static s32 g6ts_halo_feature(struct g6ts *ts,
			     const struct g6ts_contact *contact)
{
	static const s8 neighbours[][2] = {
		{ -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 },
	};
	u8 state[16][16];
	int origin_col = contact->min_col - 3;
	int origin_row = contact->min_row - 3;
	s32 peak = g6ts_signal_q12(contact->peak_value);
	s32 peak_threshold = peak / 4;
	s64 halo = 0;
	unsigned int radius, n;
	int row, col;

	if (contact->max_col - contact->min_col + 1 >= 11 ||
	    contact->max_row - contact->min_row + 1 >= 11 || peak <= 0)
		return G6TS_HALO_UNAVAILABLE_Q12;
	memset(state, 5, sizeof(state));
	for (row = max(0, origin_row); row <=
		     min_t(int, G6TS_HEAT_ROWS - 1, contact->max_row + 3); row++) {
		for (col = max(0, origin_col); col <=
		     min_t(int, G6TS_HEAT_COLS - 1, contact->max_col + 3); col++) {
			unsigned int index = row * G6TS_HEAT_COLS + col;

			if (ts->heat_component[index])
				state[row - origin_row][col - origin_col] = 0;
			else if (!g6ts_heat_active(ts, index))
				state[row - origin_row][col - origin_col] = 4;
		}
	}
	for (row = contact->min_row; row <= contact->max_row; row++) {
		for (col = contact->min_col; col <= contact->max_col; col++) {
			if (state[row - origin_row][col - origin_col])
				continue;
			for (n = 0; n < ARRAY_SIZE(neighbours); n++) {
				int nc = col + neighbours[n][0];
				int nr = row + neighbours[n][1];

				if (nr < 0 || nr >= G6TS_HEAT_ROWS || nc < 0 ||
				    nc >= G6TS_HEAT_COLS ||
				    state[nr - origin_row][nc - origin_col] != 4)
					continue;
				if (g6ts_signal_q12(ts->heatmap[nr * G6TS_HEAT_COLS + nc]) >
				    peak_threshold)
					state[nr - origin_row][nc - origin_col] = 1;
			}
		}
	}
	for (radius = 1; radius <= 2; radius++) {
		for (row = max_t(int, 0, contact->min_row - radius); row <=
		     min_t(int, G6TS_HEAT_ROWS - 1, contact->max_row + radius); row++) {
			for (col = max_t(int, 0, contact->min_col - radius); col <=
			     min_t(int, G6TS_HEAT_COLS - 1, contact->max_col + radius); col++) {
				unsigned int current_index;
				s32 current_signal;

				if (state[row - origin_row][col - origin_col] > radius)
					continue;
				current_index = row * G6TS_HEAT_COLS + col;
				current_signal = g6ts_signal_q12(ts->heatmap[current_index]);
				for (n = 0; n < ARRAY_SIZE(neighbours); n++) {
					int nc = col + neighbours[n][0];
					int nr = row + neighbours[n][1];
					unsigned int next_index;
					s32 next;

					if (nr < 0 || nr >= G6TS_HEAT_ROWS || nc < 0 ||
					    nc >= G6TS_HEAT_COLS ||
					    state[nr - origin_row][nc - origin_col] != 4)
						continue;
					next_index = nr * G6TS_HEAT_COLS + nc;
					next = g6ts_signal_q12(ts->heatmap[next_index]);
					if (next < peak_threshold && current_signal > next &&
					    current_signal > 0 &&
					    (s64)next << G6TS_CLASSIFIER_SHIFT >
						(s64)current_signal * G6TS_HALO_RATIO_Q12) {
						state[nr - origin_row][nc - origin_col] =
							radius + 1;
						halo += next;
					}
				}
			}
		}
	}
	return div64_s64(halo << G6TS_CLASSIFIER_SHIFT, peak);
}

static u8 g6ts_classify_contact(struct g6ts_contact *contact)
{
	s64 best_score = S64_MIN;
	u8 best_class = 0;
	unsigned int class, row, col;

	for (class = 0; class < ARRAY_SIZE(g6ts_classifier_models); class++) {
		const struct g6ts_classifier_model *model =
			&g6ts_classifier_models[class];
		s64 distance = 0;

		contact->scores_q24[class] =
			-9999LL * (1LL << G6TS_WINDOWS_SCORE_SHIFT);
		if (contact->pixels > model->max_points)
			continue;
		for (row = 0; row < G6TS_FEATURE_COUNT; row++) {
			s64 transformed = 0;

			for (col = row; col < G6TS_FEATURE_COUNT; col++) {
				s32 residual;

				if (col == 9 && contact->features_q12[col] ==
						G6TS_HALO_UNAVAILABLE_Q12)
					residual = 0;
				else
					residual = contact->features_q12[col] -
						   model->means_q12[col];
				transformed += (s64)residual *
					       model->transform_q12[row][col];
			}
			transformed >>= G6TS_CLASSIFIER_SHIFT;
			if (transformed > INT_MAX || transformed < -INT_MAX ||
			    distance > S64_MAX - transformed * transformed) {
				distance = S64_MAX;
				break;
			}
			distance += transformed * transformed;
		}
		if (distance != S64_MAX) {
			s64 score = ((s64)model->score_offset_q12 <<
				     G6TS_CLASSIFIER_SHIFT) - distance / 2 +
				    G6TS_WINDOWS_RUNTIME_OFFSET_Q24;

			contact->scores_q24[class] = score;
		}
	}
	if (g6ts_windows_orchestrator) {
		/* FUN_180049638 applies these in strict if/else-if order. */
		if (contact->local_peak_count == 1)
			contact->scores_q24[3] -=
				G6TS_WINDOWS_SCORE3_PRIMARY_Q24;
		else if (contact->strong_local_peak_count == 1)
			contact->scores_q24[3] -=
				G6TS_WINDOWS_SCORE3_SINGLE_Q24;
	}
	for (class = 0; class < G6TS_CLASS_COUNT; class++) {
		if (contact->scores_q24[class] > best_score) {
			best_score = contact->scores_q24[class];
			best_class = class;
		}
	}
	return best_class;
}

static unsigned int g6ts_find_contacts(struct g6ts *ts)
{
	unsigned int contact_count = 0;
	unsigned int start;

	memset(ts->heat_seen, 0, sizeof(ts->heat_seen));

	for (start = 0; start < G6TS_HEAT_SAMPLES; start++) {
		struct g6ts_contact contact = {
			.min_col = G6TS_HEAT_COLS - 1,
			.min_row = G6TS_HEAT_ROWS - 1,
			.peak_value = U8_MAX,
		};
		unsigned int head = 0, tail = 0;

		if (ts->heat_seen[start] || !g6ts_heat_active(ts, start))
			continue;
		ts->heat_seen[start] = 1;
		ts->heat_queue[tail++] = start;

		while (head < tail) {
			unsigned int index = ts->heat_queue[head++];
			unsigned int row = index / G6TS_HEAT_COLS;
			unsigned int col = index % G6TS_HEAT_COLS;
			unsigned int strength = G6TS_HEAT_SIGNAL_ZERO -
						ts->heatmap[index];
			int dr, dc;

			contact.pixels++;
			contact.strength += strength;
			contact.weighted_x += (u64)col * strength;
			contact.weighted_y += (u64)row * strength;
			contact.min_col = min_t(u8, contact.min_col, col);
			contact.max_col = max_t(u8, contact.max_col, col);
			contact.min_row = min_t(u8, contact.min_row, row);
			contact.max_row = max_t(u8, contact.max_row, row);
			contact.peak_value = min_t(u8, contact.peak_value,
						   ts->heatmap[index]);

			for (dr = -1; dr <= 1; dr++) {
				for (dc = -1; dc <= 1; dc++) {
					int neighbour_row = row + dr;
					int neighbour_col = col + dc;
					unsigned int neighbour;

					if (abs(dr) + abs(dc) != 1 ||
					    neighbour_row < 0 ||
					    neighbour_row >= G6TS_HEAT_ROWS ||
					    neighbour_col < 0 ||
					    neighbour_col >= G6TS_HEAT_COLS)
						continue;
					neighbour = neighbour_row * G6TS_HEAT_COLS +
						    neighbour_col;
					if (ts->heat_seen[neighbour] ||
					    !g6ts_heat_active(ts, neighbour))
						continue;
					ts->heat_seen[neighbour] = 1;
					ts->heat_queue[tail++] = neighbour;
				}
			}
		}

		ts->component_total++;
		if (contact.pixels < G6TS_HEAT_MIN_PIXELS &&
		    contact.peak_value > G6TS_HEAT_STRONG_MAX) {
			ts->weak_rejections++;
			continue;
		}
		if (ts->nsr_valid) {
			unsigned int sensor_row = div_u64(contact.weighted_y +
							     contact.strength / 2,
							     contact.strength);

			if (sensor_row < ARRAY_SIZE(g6ts_nsr_row_to_bin)) {
				u8 bin = g6ts_nsr_row_to_bin[sensor_row];

				if (bin < ts->nsr_bin_count &&
				    ts->nsr_bins[bin] > G6TS_NSR_CUTOFF) {
					ts->nsr_rejections++;
					continue;
				}
			}
		}
		if (contact.pixels > G6TS_HEAT_PALM_PIXELS ||
		    contact.max_col - contact.min_col + 1 > G6TS_HEAT_PALM_SPAN ||
		    contact.max_row - contact.min_row + 1 > G6TS_HEAT_PALM_SPAN) {
			ts->palm_rejections++;
			continue;
		}
		contact.x = div_u64(contact.weighted_x * G6TS_LOGICAL_MAX,
				    (u64)contact.strength * (G6TS_HEAT_COLS - 1));
		contact.y = div_u64(contact.weighted_y * G6TS_LOGICAL_MAX,
				    (u64)contact.strength * (G6TS_HEAT_ROWS - 1));
		contact.output_x = contact.x;
		contact.output_y = contact.y;
		contact.sensor_x_q24 = div_u64(contact.weighted_x << 24,
					       contact.strength);
		contact.sensor_y_q24 = div_u64(contact.weighted_y << 24,
					       contact.strength);
		memset(ts->heat_component, 0, sizeof(ts->heat_component));
		while (tail)
			ts->heat_component[ts->heat_queue[--tail]] = 1;
		if (g6ts_behavior_v2)
			g6ts_phase76_output_centroid(ts, &contact);
		g6ts_local_peak_counts(ts, &contact);
		contact.features_q12[0] =
			contact.pixels << G6TS_CLASSIFIER_SHIFT;
		g6ts_secondary_features(ts, &contact);
		g6ts_geometry_features(ts, &contact);
		contact.features_q12[9] = g6ts_halo_feature(ts, &contact);
		contact.shape_class = g6ts_classify_contact(&contact);
		/* FUN_180049458 permits the classifier's classes zero and two. */
		contact.shape_allowed = contact.shape_class == 0 ||
					contact.shape_class == 2;
		dev_dbg(&ts->spi->dev,
			"candidate class=%u allowed=%u pixels=%u peaks=%u/%u halo_q12=%d scores=[%lld,%lld,%lld,%lld]\n",
			contact.shape_class, contact.shape_allowed, contact.pixels,
			contact.local_peak_count, contact.strong_local_peak_count,
			contact.features_q12[9], contact.scores_q24[0],
			contact.scores_q24[1], contact.scores_q24[2],
			contact.scores_q24[3]);
		memset(ts->heat_component, 0, sizeof(ts->heat_component));
		g6ts_store_contact(ts, &contact, &contact_count);
	}

	ts->contact_total += contact_count;
	return contact_count;
}

static bool
g6ts_transition_current_passes(const struct g6ts_transition_rule *rule,
			       u8 candidate,
			       const s64 scores[G6TS_CLASS_COUNT])
{
	s64 selected = scores[candidate];
	unsigned int class;

	for (class = 0; class < G6TS_CLASS_COUNT; class++) {
		s64 margin;

		if (class == candidate)
			continue;
		margin = (s64)rule->current_margins[class] *
			 (1LL << G6TS_WINDOWS_SCORE_SHIFT);
		if (selected - scores[class] < margin)
			return false;
	}
	return selected >= (s64)rule->absolute_minimum *
			   (1LL << G6TS_WINDOWS_SCORE_SHIFT);
}

static bool
g6ts_transition_history_passes(const struct g6ts_track *track,
			       const struct g6ts_transition_rule *rule,
			       u8 candidate)
{
	unsigned int sample, class;

	if (rule->history_depth > G6TS_WINDOWS_HISTORY_CAPACITY ||
	    track->score_history_count < rule->history_depth)
		return false;
	for (sample = 0; sample < rule->history_depth; sample++) {
		s64 selected = track->score_history_q24[sample][candidate];

		for (class = 0; class < G6TS_CLASS_COUNT; class++) {
			s64 margin;

			if (class == candidate)
				continue;
			margin = (s64)rule->history_margins[class] *
				 (1LL << G6TS_WINDOWS_SCORE_SHIFT);
			if (selected - track->score_history_q24[sample][class] <
			    margin)
				return false;
		}
		if (selected < (s64)rule->absolute_minimum *
			       (1LL << G6TS_WINDOWS_SCORE_SHIFT))
			return false;
	}
	return true;
}

/*
 * Bounded ordinary-finger subset of FUN_180041150.  Context bias, the
 * candidate +0x4d/+0x4e score-three producer, and the later output override
 * remain outside this opt-in path until their live provider fields exist.
 */
static void g6ts_windows_update_class(struct g6ts_track *track,
				      const struct g6ts_contact *contact)
{
	const struct g6ts_transition_rule *rule;
	unsigned int sample, class;
	u8 candidate = 0;
	bool accepted = false;

	for (sample = min_t(unsigned int, track->score_history_count,
			    G6TS_WINDOWS_HISTORY_CAPACITY - 1);
	     sample > 0; sample--)
		memcpy(track->score_history_q24[sample],
		       track->score_history_q24[sample - 1],
		       sizeof(track->score_history_q24[sample]));
	memcpy(track->score_history_q24[0], contact->scores_q24,
	       sizeof(track->score_history_q24[0]));
	if (track->score_history_count < G6TS_WINDOWS_HISTORY_CAPACITY)
		track->score_history_count++;

	for (class = 1; class < G6TS_CLASS_COUNT; class++) {
		if (contact->scores_q24[candidate] < contact->scores_q24[class])
			candidate = class;
	}
	if (track->windows_class == candidate) {
		accepted = true;
	} else {
		rule = &g6ts_transition_rules[track->windows_class *
					      G6TS_CLASS_COUNT + candidate];
		if (track->age == 1 || track->age <= rule->initial_age_limit)
			accepted = g6ts_transition_current_passes(rule, candidate,
								  contact->scores_q24);
		if (!accepted && track->age > 1)
			accepted = g6ts_transition_history_passes(track, rule,
								  candidate);
		if (accepted &&
		    (contact->pixels < g6ts_class_point_minimums[candidate] ||
		     contact->pixels > g6ts_class_point_maximums[candidate]))
			accepted = false;
	}
	if (accepted)
		track->windows_class = candidate;
	track->confirmed = track->windows_class == 0 ||
			   track->windows_class == 2;
}

static u16 g6ts_windows_assignment_coordinate(u32 position_q24, u32 scale_q24)
{
	u64 scaled = (u64)position_q24 * scale_q24;

	return min_t(u64, (scaled + BIT_ULL(47)) >> 48, S16_MAX);
}

static u16 g6ts_filter_coordinate(u16 previous, u16 sample)
{
	unsigned int delta = previous > sample ? previous - sample :
						 sample - previous;

	/*
	 * TouchPenProcessor uses output = alpha * previous +
	 * (1 - alpha) * sample.  The project-tuning table selecting alpha is
	 * not recovered, so these two bands are explicit SP11 fits: suppress
	 * stationary sensor jitter, reduce lag for slow motion, and pass fast
	 * motion through unchanged.
	 */
	if (delta <= G6TS_SMOOTH_STATIONARY_MAX)
		return (3U * previous + sample + 2U) / 4U;
	if (delta <= G6TS_SMOOTH_SLOW_MAX)
		return (previous + 3U * sample + 2U) / 4U;
	return sample;
}

static unsigned int g6ts_track_distance(const struct g6ts_track *track,
					const struct g6ts_contact *contact)
{
	if (g6ts_windows_orchestrator || g6ts_behavior_v2) {
		const u32 x_scale = G6TS_WINDOWS_ASSIGN_X_SCALE_Q24;
		const u32 y_scale = G6TS_WINDOWS_ASSIGN_Y_SCALE_Q24;
		s64 predicted_x = clamp_t(s64,
			(s64)track->sensor_x_q24 + track->sensor_velocity_x_q24,
			0, (s64)(G6TS_HEAT_COLS - 1) << 24);
		s64 predicted_y = clamp_t(s64,
			(s64)track->sensor_y_q24 + track->sensor_velocity_y_q24,
			0, (s64)(G6TS_HEAT_ROWS - 1) << 24);
		s32 track_x = g6ts_windows_assignment_coordinate(predicted_x,
							      x_scale);
		s32 track_y = g6ts_windows_assignment_coordinate(predicted_y,
							      y_scale);
		s32 candidate_x = g6ts_windows_assignment_coordinate(contact->sensor_x_q24,
								  x_scale);
		s32 candidate_y = g6ts_windows_assignment_coordinate(contact->sensor_y_q24,
								  y_scale);
		s32 dx = track_x - candidate_x;
		s32 dy = track_y - candidate_y;
		u32 squared = dx * dx + dy * dy;

		if (squared >= G6TS_WINDOWS_ASSIGN_RADIUS *
			       G6TS_WINDOWS_ASSIGN_RADIUS)
			return G6TS_ASSIGN_INVALID_COST;
		return squared;
	}

	s32 predicted_x = clamp_t(s32, (s32)track->raw_x + track->velocity_x,
				    0, G6TS_LOGICAL_MAX);
	s32 predicted_y = clamp_t(s32, (s32)track->raw_y + track->velocity_y,
				    0, G6TS_LOGICAL_MAX);
	s32 dx = predicted_x - contact->x;
	s32 dy = predicted_y - contact->y;
	u64 squared = (s64)dx * dx + (s64)dy * dy;

	if (squared > (u64)G6TS_TRACK_MATCH_MAX * G6TS_TRACK_MATCH_MAX)
		return G6TS_ASSIGN_INVALID_COST;
	return int_sqrt64(squared);
}

static u8 g6ts_confirmation_requirement(const struct g6ts *ts,
					const struct g6ts_contact *contact)
{
	unsigned int i;
	u8 required = g6ts_behavior_v2 ? G6TS_BEHAVIOR_CONFIRM_NORMAL :
					     G6TS_TRACK_CONFIRM_NORMAL;

	/*
	 * TouchPenProcessor does not expose a blob immediately.  Its recovered
	 * lifecycle keeps a per-track class history and uses transition-specific
	 * evidence windows.  The proprietary four-class score coefficients are
	 * intentionally not copied here; use observable component quality to
	 * select the same short, medium, and long lifecycle windows instead.
	 */
	if (contact->pixels < G6TS_HEAT_MIN_PIXELS)
		required = G6TS_TRACK_CONFIRM_WEAK;
	if (!contact->shape_allowed)
		required = max_t(u8, required, G6TS_TRACK_CONFIRM_WEAK);

	for (i = 0; i < G6TS_MAX_CONTACTS; i++) {
		const struct g6ts_track *track = &ts->tracks[i];
		s32 dx, dy;
		u64 squared;

		if (!track->active || !track->confirmed || track->missed)
			continue;
		dx = (s32)track->raw_x - contact->x;
		dy = (s32)track->raw_y - contact->y;
		squared = (s64)dx * dx + (s64)dy * dy;
		if (squared > (u64)G6TS_TRACK_SPLIT_RADIUS *
				      G6TS_TRACK_SPLIT_RADIUS)
			continue;

		/*
		 * A new, smaller island beside an established finger is commonly a
		 * transient split of that finger.  Preserve real close multitouch by
		 * accepting it after sustained evidence rather than deleting it.
		 */
		required = max_t(u8, required, G6TS_TRACK_CONFIRM_WEAK);
		if ((u64)contact->strength * 2 <= track->strength ||
		    (u32)contact->pixels * 2 <= track->pixels)
			required = G6TS_TRACK_CONFIRM_SPLIT;
	}

	return required;
}

/*
 * Windows builds a predicted-position Euclidean cost matrix and solves a
 * global assignment.  Use a square matrix with explicit dummy rows/columns
 * so a gated-out pairing loses to closing one track and opening another.
 */
static void g6ts_assign_tracks(struct g6ts *ts, unsigned int count,
			       int contact_slots[G6TS_MAX_CONTACTS])
{
	struct g6ts_assignment_workspace *work = &ts->assignment;
	unsigned int active_count = 0;
	unsigned int n, row, col, i, j;

	memset(work, 0, sizeof(*work));
	for (i = 0; i < G6TS_MAX_CONTACTS; i++)
		contact_slots[i] = -1;

	for (i = 0; i < G6TS_MAX_CONTACTS; i++) {
		if (ts->tracks[i].active)
			work->active_slots[active_count++] = i;
	}
	if (!active_count || !count)
		return;

	n = active_count + count;
	for (row = 0; row < n; row++) {
		for (col = 0; col < n; col++) {
			if (row < active_count && col < count) {
				struct g6ts_track *track;

				track = &ts->tracks[work->active_slots[row]];
				work->cost[row][col] =
					g6ts_track_distance(track,
							    &ts->contacts[col]);
			} else if (row < active_count || col < count) {
				work->cost[row][col] = G6TS_ASSIGN_UNMATCHED_COST;
			} else {
				work->cost[row][col] = 0;
			}
		}
	}

	/* Hungarian minimum-cost assignment, using one-based work arrays. */
	for (i = 1; i <= n; i++) {
		int j0 = 0;

		work->p[0] = i;
		for (j = 0; j <= n; j++) {
			work->minv[j] = INT_MAX;
			work->used[j] = false;
		}
		do {
			int i0, delta = INT_MAX, j1 = 0;

			work->used[j0] = true;
			i0 = work->p[j0];
			for (j = 1; j <= n; j++) {
				int reduced_cost;

				if (work->used[j])
					continue;
				reduced_cost = work->cost[i0 - 1][j - 1] -
					       work->u[i0] - work->v[j];
				if (reduced_cost < work->minv[j]) {
					work->minv[j] = reduced_cost;
					work->way[j] = j0;
				}
				if (work->minv[j] < delta) {
					delta = work->minv[j];
					j1 = j;
				}
			}
			for (j = 0; j <= n; j++) {
				if (work->used[j]) {
					work->u[work->p[j]] += delta;
					work->v[j] -= delta;
				} else if (j) {
					work->minv[j] -= delta;
				}
			}
			j0 = j1;
		} while (work->p[j0]);

		do {
			int j1 = work->way[j0];

			work->p[j0] = work->p[j1];
			j0 = j1;
		} while (j0);
	}

	for (j = 1; j <= n; j++) {
		row = work->p[j] - 1;
		col = j - 1;
		if (row < active_count && col < count &&
		    work->cost[row][col] < G6TS_ASSIGN_INVALID_COST &&
		    (g6ts_windows_orchestrator || g6ts_behavior_v2 ||
		     work->cost[row][col] <= G6TS_TRACK_MATCH_MAX))
			contact_slots[col] = work->active_slots[row];
	}
	for (i = 0; i < count; i++)
		if (contact_slots[i] >= 0)
			ts->assignment_matches++;
}

static void g6ts_update_track(struct g6ts_track *track,
			      const struct g6ts_contact *contact)
{
	u16 old_x = track->raw_x;
	u16 old_y = track->raw_y;
	u32 old_sensor_x_q24 = track->sensor_x_q24;
	u32 old_sensor_y_q24 = track->sensor_y_q24;

	track->raw_x = contact->x;
	track->raw_y = contact->y;
	track->velocity_x = (s32)contact->x - old_x;
	track->velocity_y = (s32)contact->y - old_y;
	track->sensor_x_q24 = contact->sensor_x_q24;
	track->sensor_y_q24 = contact->sensor_y_q24;
	track->sensor_velocity_x_q24 = (s32)((s64)contact->sensor_x_q24 -
						 old_sensor_x_q24);
	track->sensor_velocity_y_q24 = (s32)((s64)contact->sensor_y_q24 -
						 old_sensor_y_q24);
	if (g6ts_windows_orchestrator) {
		/* FUN_18004a330 stores X/Y directly; its alpha blends another scalar. */
		track->output_x = contact->x;
		track->output_y = contact->y;
	} else if (g6ts_behavior_v2) {
		track->output_x = contact->output_x;
		track->output_y = contact->output_y;
	} else {
		track->output_x = g6ts_filter_coordinate(track->output_x,
							 contact->x);
		track->output_y = g6ts_filter_coordinate(track->output_y,
							 contact->y);
	}
	track->strength = contact->strength;
	track->pixels = contact->pixels;
	track->shape_class = contact->shape_class;
	if (track->age < U16_MAX)
		track->age++;
	if (g6ts_windows_orchestrator) {
		g6ts_windows_update_class(track, contact);
	} else if (!track->confirmed && contact->shape_allowed &&
	    track->evidence < U8_MAX) {
		track->evidence++;
		if (track->evidence >= track->required_evidence)
			track->confirmed = true;
	}
	track->missed = 0;
}

static int g6ts_new_track(struct g6ts *ts,
			  const struct g6ts_contact *contact)
{
	unsigned int slot;

	for (slot = 0; slot < G6TS_MAX_CONTACTS; slot++) {
		struct g6ts_track *track = &ts->tracks[slot];

		if (track->active)
			continue;
		memset(track, 0, sizeof(*track));
		track->raw_x = contact->x;
		track->raw_y = contact->y;
		track->output_x = g6ts_behavior_v2 ? contact->output_x : contact->x;
		track->output_y = g6ts_behavior_v2 ? contact->output_y : contact->y;
		track->sensor_x_q24 = contact->sensor_x_q24;
		track->sensor_y_q24 = contact->sensor_y_q24;
		track->strength = contact->strength;
		track->pixels = contact->pixels;
		track->shape_class = contact->shape_class;
		track->age = 1;
		if (g6ts_windows_orchestrator) {
			track->windows_class = G6TS_WINDOWS_UNCLASSIFIED;
			g6ts_windows_update_class(track, contact);
		} else {
			track->evidence = contact->shape_allowed ? 1 : 0;
			track->required_evidence =
				g6ts_confirmation_requirement(ts, contact);
			track->confirmed = track->required_evidence <= 1;
		}
		track->active = true;
		ts->new_tracks++;
		return slot;
	}
	return -ENOSPC;
}

static void g6ts_advance_unmatched_tracks(struct g6ts *ts,
					  unsigned long current_slots)
{
	unsigned int i;

	for (i = 0; i < G6TS_MAX_CONTACTS; i++) {
		struct g6ts_track *track = &ts->tracks[i];

		if (!track->active || (current_slots & BIT(i)))
			continue;
		if (track->missed < G6TS_CONTACT_HOLD_FRAMES) {
			track->missed++;
			track->velocity_x /= 2;
			track->velocity_y /= 2;
			track->sensor_velocity_x_q24 /= 2;
			track->sensor_velocity_y_q24 /= 2;
		} else {
			memset(track, 0, sizeof(*track));
		}
	}
}

static unsigned long
g6ts_create_unmatched_tracks(struct g6ts *ts, unsigned int count,
			     const int contact_slots[G6TS_MAX_CONTACTS],
			     unsigned long current_slots)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		int slot;

		if (contact_slots[i] >= 0)
			continue;
		slot = g6ts_new_track(ts, &ts->contacts[i]);
		if (slot >= 0)
			current_slots |= BIT(slot);
	}
	return current_slots;
}

static void g6ts_collect_linux_contacts(struct g6ts *ts,
					unsigned long current_slots)
{
	unsigned int reported = 0;
	unsigned int i;

	for (i = 0; i < G6TS_MAX_CONTACTS; i++) {
		struct g6ts_track *track = &ts->tracks[i];

		/* Retained tracks remain assignable but never become ghost contacts. */
		if (!track->active || !track->confirmed ||
		    !(current_slots & BIT(i)))
			continue;
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(ts->input, &ts->prop, track->output_x,
				       track->output_y, true);
		reported++;
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	ts->output_frames++;
	ts->output_contacts += reported;
}

static int g6ts_report_heat_contacts(struct g6ts *ts, const u8 *content,
				     size_t content_len)
{
	u64 started_ns = ktime_get_ns();
	unsigned long current_slots = 0;
	int contact_slots[G6TS_MAX_CONTACTS];
	unsigned int count, i;
	int ret;

	ret = g6ts_extract_heatmap(ts, content, content_len);
	if (ret) {
		ts->heat_errors++;
		return ret;
	}
	count = g6ts_find_contacts(ts);
	g6ts_assign_tracks(ts, count, contact_slots);

	for (i = 0; i < count; i++) {
		int slot = contact_slots[i];

		if (slot < 0)
			continue;
		g6ts_update_track(&ts->tracks[slot], &ts->contacts[i]);
		current_slots |= BIT(slot);
	}

	/*
	 * The frame transaction is deliberately explicit and single-threaded:
	 * assignment -> matched updates -> unmatched lifecycle -> new tracks ->
	 * final Linux collection.  No input event escapes an intermediate stage.
	 */
	g6ts_advance_unmatched_tracks(ts, current_slots);
	current_slots = g6ts_create_unmatched_tracks(ts, count, contact_slots,
						     current_slots);
	g6ts_collect_linux_contacts(ts, current_slots);
	ts->heat_frames++;
	started_ns = ktime_get_ns() - started_ns;
	ts->processing_ns_total += started_ns;
	ts->processing_ns_max = max(ts->processing_ns_max, started_ns);

	return 0;
}

static void g6ts_release_contacts(struct g6ts *ts)
{
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	memset(ts->tracks, 0, sizeof(ts->tracks));
}

static void g6ts_release_pen(struct g6ts *ts)
{
	input_report_abs(ts->pen_input, ABS_PRESSURE, 0);
	input_report_key(ts->pen_input, BTN_TOUCH, false);
	input_report_key(ts->pen_input, BTN_STYLUS, false);
	input_report_key(ts->pen_input, BTN_TOOL_PEN, false);
	input_report_key(ts->pen_input, BTN_TOOL_RUBBER, false);
	input_sync(ts->pen_input);
}

static void g6ts_release_inputs(struct g6ts *ts)
{
	g6ts_release_contacts(ts);
	g6ts_release_pen(ts);
}

static void g6ts_report_pen(struct g6ts *ts, const u8 *content)
{
	struct input_dev *input = ts->pen_input;
	u8 flags = content[0];
	bool in_range = flags & G6TS_PEN_IN_RANGE;
	bool touching = flags & (G6TS_PEN_TIP_SWITCH | G6TS_PEN_ERASER);
	bool barrel = flags & G6TS_PEN_BARREL_SWITCH;
	bool rubber = flags & (G6TS_PEN_INVERT | G6TS_PEN_ERASER);
	u16 x, y, pressure, raw_tilt_x, raw_tilt_y;
	s16 tilt_x, tilt_y;

	/* Layout and logical ranges come from the report-0x01 HID collection. */
	x = min_t(u16, get_unaligned_le16(&content[1]), G6TS_PEN_X_MAX);
	y = min_t(u16, get_unaligned_le16(&content[3]), G6TS_PEN_Y_MAX);
	pressure = min_t(u16, get_unaligned_le16(&content[5]),
			 G6TS_PEN_PRESSURE_MAX);
	raw_tilt_x = min_t(u16, get_unaligned_le16(&content[7]),
			   G6TS_PEN_TILT_RAW_MAX);
	raw_tilt_y = min_t(u16, get_unaligned_le16(&content[9]),
			   G6TS_PEN_TILT_RAW_MAX);
	/*
	 * HID maps logical 0..18000 to physical -9000..9000 at a -2
	 * degree exponent.  Linux therefore sees signed hundredths of a
	 * degree, centered at zero, while this dormant native path stays a
	 * guarded fallback.
	 */
	tilt_x = raw_tilt_x - G6TS_PEN_TILT_CENTER;
	tilt_y = raw_tilt_y - G6TS_PEN_TILT_CENTER;

	touchscreen_report_pos(input, &ts->pen_prop, x, y, false);
	input_report_abs(input, ABS_PRESSURE, pressure);
	input_report_abs(input, ABS_TILT_X, tilt_x);
	input_report_abs(input, ABS_TILT_Y, tilt_y);
	input_report_key(input, BTN_TOUCH, touching);
	input_report_key(input, BTN_STYLUS, barrel);
	input_report_key(input, BTN_TOOL_PEN, in_range && !rubber);
	input_report_key(input, BTN_TOOL_RUBBER, in_range && rubber);
	input_sync(input);
}

static void g6ts_send_heat_feedback_a5(struct g6ts *ts,
				       bool after_heat_group);

static void g6ts_handle_data_report(struct g6ts *ts)
{
	const u8 *payload = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	u16 x, y;
	bool active;
	int ret;

	if (ts->last_class != DATA)
		return;
	ts->report_counts[ts->last_content_id]++;
	/* Preserve raw pen measurements before touch-readiness can discard them. */
	if (g6ts_is_raw_export_report(ts->last_content_id))
		g6ts_heat_enqueue(ts, ts->last_content_id, payload,
				  ts->last_content_len);
	/*
	 * HID-SPI stores the report ID immediately before content, so body + 3
	 * is the exact hidraw packet expected by iptsd. The full allocation size
	 * is supplied because report 0x1a is 4,350 bytes including its ID.
	 */
	if (ts->ipts_hid_ready &&
	    g6ts_is_ipts_hid_report(ts->last_content_id)) {
		ret = hid_safe_input_report(ts->ipts_hid, HID_INPUT_REPORT,
					    ts->body + 3, G6TS_MAX_BODY - 3,
					    ts->last_content_len + 1, 1);
		if (ret && ret != -ENODEV && ret != -EBUSY)
			dev_warn_ratelimited(&ts->spi->dev,
					     "failed to relay DFT report 0x%02x: %d\n",
					     ts->last_content_id, ret);
	}
	switch (ts->last_content_id) {
	case 0x1a:
		g6ts_send_heat_feedback_a5(ts, true);
		break;
	case 0x0d:
		ts->a5.cycle_saw_0x0d = true;
		break;
	case 0x0b:
		if (ts->a5.cycle_saw_0x0d) {
			ts->a5.cycle_saw_0x0d = false;
			g6ts_send_heat_feedback_a5(ts, false);
		}
		break;
	}
	/*
	 * Heat is demand-driven on this panel: the first frame may not exist until
	 * a finger reaches the glass.  Phase 77 therefore opens the response path
	 * after the mode handshake, but suppresses every non-Heat input report
	 * until a complete Heat frame has passed the normal structural parser.
	 * Pen input is an independent HID collection and remains usable while
	 * touch readiness is being established.
	 */
	if (ts->awaiting_ready_heat &&
	    ts->last_content_id != G6TS_HEATMAP_REPORT_ID &&
	    ts->last_content_id != G6TS_PEN_REPORT_ID)
		return;

	if (ts->last_content_id == G6TS_PEN_REPORT_ID) {
		if (ts->last_content_len != G6TS_PEN_REPORT_LEN) {
			dev_warn_ratelimited(&ts->spi->dev,
					     "malformed pen report length: %u\n",
					     ts->last_content_len);
			return;
		}
		g6ts_report_pen(ts, payload);
		return;
	}

	if (ts->last_content_id == 0x40 && ts->last_content_len == 5) {
		active = payload[0] & BIT(0);
		x = get_unaligned_le16(&payload[1]);
		y = get_unaligned_le16(&payload[3]);

		input_report_key(ts->input, BTN_TOUCH, active);
		if (active)
			touchscreen_report_pos(ts->input, &ts->prop, x, y, false);
		input_sync(ts->input);
		return;
	}

	if (ts->last_content_id == G6TS_HEATMAP_REPORT_ID &&
	    ts->last_content_len + 1 <= G6TS_MAX_BODY) {
		int ret;

		ret = g6ts_report_heat_contacts(ts, payload, ts->last_content_len);
		if (ret) {
			if (ts->awaiting_ready_heat)
				ts->ready_verification_failures++;
			g6ts_release_contacts(ts);
			dev_warn_ratelimited(&ts->spi->dev,
					     "malformed Heat frame: %d\n", ret);
		} else if (ts->awaiting_ready_heat) {
			ts->awaiting_ready_heat = false;
			ts->ready_heat_frames++;
			dev_info(&ts->spi->dev,
				 "touch input ready after first valid Heat frame\n");
		}
	}
}

/* Read one complete pending HID-over-SPI response, and never retry. */
static int g6ts_dma_read_response(struct g6ts *ts)
{
	const struct hidspi_dev_descriptor *descriptor;
	s64 interrupt_edges;
	size_t body_len;
	u16 words;
	int pending;
	int ret;

	g6ts_clear_last_response(ts);
	pending = g6ts_pending(ts);
	interrupt_edges = atomic64_read(&ts->interrupt_edges);
	if (pending <= 0 &&
	    interrupt_edges <= ts->handled_interrupt_edges) {
		return pending < 0 ? pending : -EAGAIN;
	}
	ts->handled_interrupt_edges = interrupt_edges;

	ret = g6ts_dma_read_pair(ts, g6ts_header_cmd, ts->last_header,
				 sizeof(ts->last_header));
	if (ret) {
		ts->fatal_transport_error = true;
		return ret;
	}

	if ((ts->last_header[0] & 0x0f) != G6TS_HEADER_VERSION ||
	    ts->last_header[3] != G6TS_HEADER_SYNC) {
		if (g6ts_ready_quiesce) {
			/* Let the panel retire the body transaction before rechecking. */
			usleep_range(100, 200);
			pending = g6ts_pending(ts);
			if (!pending) {
				ts->quiesced_empty_reads++;
				return -EAGAIN;
			}
			if (pending < 0)
				return pending;
		}
		dev_warn_ratelimited(&ts->spi->dev,
				     "invalid HID-SPI header=%4ph ready=%d\n",
				     ts->last_header, g6ts_pending(ts));
		return -EPROTO;
	}

	words = get_unaligned_le16(&ts->last_header[1]) & 0x3fff;
	body_len = (size_t)words * 4;
	if (body_len < HIDSPI_INPUT_BODY_HEADER_SIZE ||
	    body_len > G6TS_MAX_BODY)
		return -EMSGSIZE;
	if (g6ts_windows_read_cadence)
		usleep_range(G6TS_WINDOWS_HEADER_BODY_MIN_US,
			     G6TS_WINDOWS_HEADER_BODY_MAX_US);

	ret = g6ts_dma_read_pair(ts, g6ts_body_cmd, ts->body, body_len);
	if (ret) {
		ts->fatal_transport_error = true;
		return ret;
	}

	ts->last_class = ts->body[0];
	ts->last_content_len = get_unaligned_le16(&ts->body[1]);
	ts->last_content_id = ts->body[3];
	if (ts->last_content_len > body_len - HIDSPI_INPUT_BODY_HEADER_SIZE)
		return -EPROTO;

	if (ts->last_class == DEVICE_DESCRIPTOR_RESPONSE &&
	    ts->last_content_len == HIDSPI_DEVICE_DESCRIPTOR_SIZE) {
		descriptor = (const struct hidspi_dev_descriptor *)
			     (ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE);
		ts->expected_report_descriptor_len =
			le16_to_cpu(descriptor->rep_desc_len);
	}
	/* Recovery consumes and validates replies before reopening Linux input. */
	if (!g6ts_is_quiescing(ts) && READ_ONCE(ts->mode_enabled))
		g6ts_handle_data_report(ts);
	return 0;
}

/* io_lock is held by both the IRQ thread and the recovery worker. */
static void g6ts_note_panel_reset_locked(struct g6ts *ts,
					 bool schedule_recovery)
{
	unsigned long now = jiffies;
	unsigned long interval_ms = 0;

	if (ts->last_reset_jiffies)
		interval_ms = jiffies_to_msecs(now - ts->last_reset_jiffies);
	ts->last_reset_jiffies = now;
	ts->reset_notifications++;
	ts->mode_enabled = false;
	ts->awaiting_ready_heat = false;
	ts->recovery_path = g6ts_reset_recovery_v2 ?
		G6TS_RECOVERY_SOFTWARE : G6TS_RECOVERY_HARDWARE;
	if (g6ts_reset_storm_breaker) {
		if (interval_ms && interval_ms <= G6TS_RESET_STORM_WINDOW_MS)
			ts->rapid_reset_streak++;
		else
			ts->rapid_reset_streak = 1;

		if (ts->rapid_reset_streak >= G6TS_RESET_STORM_LIMIT) {
			ts->recovery_path = G6TS_RECOVERY_HARDWARE;
			ts->reset_storm_escalations++;
			dev_warn(&ts->spi->dev,
				 "rapid reset circuit breaker #%llu after %u resets\n",
				 ts->reset_storm_escalations,
				 ts->rapid_reset_streak);
		}
	}
	g6ts_release_inputs(ts);
	g6ts_heat_generation_boundary(ts, G6TS_HEAT_RECORD_F_RESET);
	dev_warn(&ts->spi->dev,
		 "panel reset notification #%llu interval=%lums\n",
		 ts->reset_notifications, interval_ms);

	if (schedule_recovery && !g6ts_is_quiescing(ts))
		schedule_delayed_work(&ts->recovery_work,
				      msecs_to_jiffies(G6TS_RECOVERY_DELAY_MS));
}

/* io_lock is held and the caller has observed one complete IRQ read failure. */
static void g6ts_note_host_fault_locked(struct g6ts *ts, int error)
{
	if (error == -EPROTO || error == -EMSGSIZE)
		ts->irq_protocol_errors++;
	else
		ts->irq_transport_errors++;
	ts->last_host_fault = error;
	g6ts_heat_generation_boundary(ts,
				      G6TS_HEAT_RECORD_F_TRANSPORT_FAULT);

	if (!g6ts_host_fault_recovery) {
		dev_warn_ratelimited(&ts->spi->dev,
				     "IRQ response read failed: %d\n", error);
		return;
	}

	/*
	 * A fresh hardware recovery is allowed to use the transport again.  A new
	 * fatal error during that attempt will still stop the worker, preserving
	 * the existing bounded-failure behavior.
	 */
	ts->fatal_transport_error = false;
	ts->mode_enabled = false;
	ts->awaiting_ready_heat = false;
	ts->recovery_path = G6TS_RECOVERY_HARDWARE;
	ts->rapid_reset_streak = 0;
	ts->host_fault_recoveries++;
	g6ts_release_inputs(ts);
	dev_warn(&ts->spi->dev,
		 "host HID-SPI fault #%llu ret=%d; scheduling cold recovery\n",
		 ts->host_fault_recoveries, error);

	if (!g6ts_is_quiescing(ts))
		schedule_delayed_work(&ts->recovery_work,
				      msecs_to_jiffies(G6TS_RECOVERY_DELAY_MS));
}

static irqreturn_t g6ts_interrupt_thread(int irq, void *data)
{
	struct g6ts *ts = data;
	unsigned int i;
	int ret;

	if (g6ts_is_quiescing(ts) || !READ_ONCE(ts->mode_enabled))
		return IRQ_HANDLED;

	mutex_lock(&ts->io_lock);
	for (i = 0; i < G6TS_IRQ_DRAIN_LIMIT; i++) {
		if (!g6ts_has_unread_response(ts))
			break;
		ret = g6ts_dma_read_response(ts);
		if (ret) {
			if (ret != -EAGAIN)
				g6ts_note_host_fault_locked(ts, ret);
			break;
		}
		if (ts->last_class == RESET_RESPONSE) {
			g6ts_note_panel_reset_locked(ts, true);
			break;
		}
		if (g6ts_windows_read_cadence) {
			ts->cadence_single_response_irqs++;
			break;
		}
	}
	/*
	 * This IRQ is edge-triggered.  Returning while GPIO51 still advertises an
	 * unread packet can strand the stream because no second falling edge is
	 * guaranteed.  Bound the drain for safety, then use the same observable
	 * host-fault recovery as a timed-out Windows transfer.
	 */
	if (i == G6TS_IRQ_DRAIN_LIMIT && g6ts_has_unread_response(ts)) {
		ts->irq_drain_overflows++;
		g6ts_note_host_fault_locked(ts, -EOVERFLOW);
	}
	mutex_unlock(&ts->io_lock);
	return IRQ_HANDLED;
}

static int __g6ts_dma_feature_exchange(struct g6ts *ts, u8 report_type,
				       u8 content_id, const u8 *content,
				       size_t content_len, bool dump_response)
{
	char dump_prefix[64];
	u8 expected_class;
	unsigned int response_index;
	int ret;

	if (report_type == SET_FEATURE)
		expected_class = SET_FEATURE_RESPONSE;
	else if (report_type == GET_FEATURE)
		expected_class = GET_FEATURE_RESPONSE;
	else
		return -EINVAL;

	ret = g6ts_dma_hidspi_output(ts, report_type, content_id,
				     content, content_len);
	if (ret) {
		ts->fatal_transport_error = true;
		return ret;
	}

	for (response_index = 0;
	     response_index < G6TS_FEATURE_RESPONSE_LIMIT;
	     response_index++) {
		ret = g6ts_wait_pending(ts, 1000);
		if (ret)
			return ret;

		ret = g6ts_dma_read_response(ts);
		if (ret)
			return ret;

		if (ts->last_class == expected_class &&
		    ts->last_content_id == content_id) {
			if (dump_response && report_type == GET_FEATURE &&
			    !ts->init_get_feature_dumped[ts->initialization_stage]) {
				ts->init_get_feature_dumped[ts->initialization_stage] = true;
				scnprintf(dump_prefix, sizeof(dump_prefix),
					  "%s init GET_FEATURE 0x%02x: ",
					  dev_name(&ts->spi->dev), content_id);
				dev_info(&ts->spi->dev,
					 "init GET_FEATURE response stage=%s id=0x%02x len=%u\n",
					 g6ts_initialization_stage_name(ts->initialization_stage),
					 content_id, ts->last_content_len);
				print_hex_dump(KERN_INFO, dump_prefix,
					       DUMP_PREFIX_OFFSET, 16, 1,
					       ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE,
					       ts->last_content_len, false);
			}
			return 0;
		}
		if (ts->last_class == RESET_RESPONSE) {
			g6ts_note_panel_reset_locked(ts, false);
			return -EPIPE;
		}

		/* Streaming data can precede a solicited feature response. */
		if (ts->last_class == DATA)
			continue;
		if (ts->last_class == OUTPUT_REPORT_RESPONSE &&
		    ts->last_content_id == 0x09)
			continue;

		return -EPROTO;
	}

	return -EOVERFLOW;
}

static int g6ts_dma_feature_exchange(struct g6ts *ts, u8 report_type,
				     u8 content_id, const u8 *content,
				     size_t content_len)
{
	return __g6ts_dma_feature_exchange(ts, report_type, content_id, content,
					   content_len, true);
}

static int g6ts_cache_ipts_metadata_locked(struct g6ts *ts)
{
	const u8 *metadata =
		ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;

	if (ts->last_class != GET_FEATURE_RESPONSE ||
	    ts->last_content_id != G6TS_IPTS_METADATA_REPORT_ID ||
	    ts->last_content_len != G6TS_IPTS_METADATA_LEN)
		return -EPROTO;
	if (ts->ipts_metadata_valid &&
	    memcmp(ts->ipts_metadata, metadata, G6TS_IPTS_METADATA_LEN)) {
		dev_err(&ts->spi->dev,
			"IPTS metadata changed across transport generation\n");
		return -EPROTO;
	}

	memcpy(ts->ipts_metadata, metadata, G6TS_IPTS_METADATA_LEN);
	ts->ipts_metadata_valid = true;
	return 0;
}

static int g6ts_ipts_hid_ll_parse(struct hid_device *hid)
{
	struct g6ts *ts = hid_get_drvdata(hid);

	if (!READ_ONCE(ts->report_descriptor_valid))
		return -ENODEV;

	return hid_parse_report(hid, ts->report_descriptor,
				G6TS_SP11_REPORT_DESCRIPTOR_LEN);
}

static int g6ts_ipts_hid_ll_start(struct hid_device *hid)
{
	return 0;
}

static void g6ts_ipts_hid_ll_stop(struct hid_device *hid)
{
}

static int g6ts_ipts_hid_ll_open(struct hid_device *hid)
{
	return 0;
}

static void g6ts_ipts_hid_ll_close(struct hid_device *hid)
{
}

/*
 * The custom transport owns mode selection.  iptsd's report-0x05 writes are
 * therefore acknowledged without reaching the panel; in particular, its
 * shutdown write must not switch the shared controller back to singletouch.
 * The metadata GET is proxied once and then served from the stable cache.
 */
static int g6ts_ipts_hid_ll_raw_request(struct hid_device *hid,
					unsigned char reportnum, u8 *buf,
					size_t len, unsigned char rtype,
					int reqtype)
{
	struct g6ts *ts = hid_get_drvdata(hid);
	size_t metadata_len;
	int ret;

	if (!buf || len < 2)
		return -EINVAL;
	if (rtype != HID_FEATURE_REPORT)
		return -EOPNOTSUPP;

	mutex_lock(&ts->io_lock);
	if (g6ts_is_quiescing(ts) || ts->ipts_hid != hid) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (reqtype == HID_REQ_SET_REPORT) {
		if (reportnum != G6TS_IPTS_MODE_REPORT_ID ||
		    buf[0] != reportnum || len != 2 || buf[1] > 1) {
			ret = -EOPNOTSUPP;
			goto out_unlock;
		}
		ret = len;
		goto out_unlock;
	}

	if (reqtype != HID_REQ_GET_REPORT ||
	    reportnum != G6TS_IPTS_METADATA_REPORT_ID) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (!READ_ONCE(ts->mode_enabled)) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}

	if (!ts->ipts_metadata_valid) {
		ret = __g6ts_dma_feature_exchange(ts, GET_FEATURE,
						  G6TS_IPTS_METADATA_REPORT_ID,
						  NULL, 0, false);
		if (!ret)
			ret = g6ts_cache_ipts_metadata_locked(ts);
		if (ret) {
			if (ret == -EPIPE && !g6ts_is_quiescing(ts))
				schedule_delayed_work(&ts->recovery_work,
						      msecs_to_jiffies(G6TS_RECOVERY_DELAY_MS));
			goto out_unlock;
		}
	}

	metadata_len = min_t(size_t, len - 1, G6TS_IPTS_METADATA_LEN);
	buf[0] = reportnum;
	memcpy(buf + 1, ts->ipts_metadata, metadata_len);
	ret = metadata_len + 1;

out_unlock:
	mutex_unlock(&ts->io_lock);
	return ret;
}

static const struct hid_ll_driver g6ts_ipts_hid_ll_driver = {
	.parse = g6ts_ipts_hid_ll_parse,
	.start = g6ts_ipts_hid_ll_start,
	.stop = g6ts_ipts_hid_ll_stop,
	.open = g6ts_ipts_hid_ll_open,
	.close = g6ts_ipts_hid_ll_close,
	.raw_request = g6ts_ipts_hid_ll_raw_request,
	.max_buffer_size = G6TS_MAX_BODY,
};

static bool g6ts_ipts_hid_desired_locked(const struct g6ts *ts)
{
	return g6ts_ipts_hid_bridge && !g6ts_is_quiescing(ts) &&
	       READ_ONCE(ts->mode_enabled) &&
	       ts->report_descriptor_valid && ts->hid_product_id;
}

static struct hid_device *g6ts_ipts_hid_detach_locked(struct g6ts *ts)
{
	struct hid_device *hid = ts->ipts_hid;

	ts->ipts_hid = NULL;
	ts->ipts_hid_ready = false;
	ts->hid_bound_generation = 0;
	return hid;
}

static struct hid_device *g6ts_ipts_hid_allocate(struct g6ts *ts,
						 u16 product_id)
{
	struct hid_device *hid;

	hid = hid_allocate_device();
	if (IS_ERR(hid))
		return hid;

	hid_set_drvdata(hid, ts);
	hid->ll_driver = &g6ts_ipts_hid_ll_driver;
	hid->dev.parent = &ts->spi->dev;
	hid->bus = BUS_SPI;
	hid->group = HID_GROUP_MSHW0485_IPTS;
	hid->vendor = G6TS_SP11_VENDOR_ID;
	hid->product = product_id;
	hid->version = G6TS_SP11_VERSION_ID;
	snprintf(hid->name, sizeof(hid->name),
		 "Microsoft Surface G6 IPTS %04X:%04X",
		 G6TS_SP11_VENDOR_ID, product_id);
	snprintf(hid->phys, sizeof(hid->phys), "%s/ipts",
		 dev_name(&ts->spi->dev));

	return hid;
}

static bool g6ts_ipts_hid_retry_locked(struct g6ts *ts, u32 generation)
{
	if (!g6ts_ipts_hid_desired_locked(ts) || ts->ipts_hid ||
	    ts->hid_target_generation != generation ||
	    ts->hid_rebind_failures >= G6TS_HID_REBIND_RETRY_LIMIT)
		return false;

	ts->hid_rebind_failures++;
	return true;
}

/*
 * HID teardown may take hidraw's minors_rwsem for write.  Never destroy while
 * io_lock is held: a concurrent feature ioctl takes that rwsem for read before
 * entering our raw_request callback and acquiring io_lock.
 */
static void g6ts_ipts_hid_rebind_work(struct work_struct *work)
{
	struct g6ts *ts = container_of(to_delayed_work(work), struct g6ts,
				      hid_rebind_work);
	struct hid_device *old_hid = NULL;
	struct hid_device *hid;
	bool create = false;
	bool retry = false;
	u32 generation = 0;
	u16 product_id = 0;
	int ret;

	mutex_lock(&ts->io_lock);
	if (ts->ipts_hid &&
	    (!g6ts_ipts_hid_desired_locked(ts) ||
	     ts->hid_bound_generation != ts->hid_target_generation))
		old_hid = g6ts_ipts_hid_detach_locked(ts);
	if (!ts->ipts_hid && g6ts_ipts_hid_desired_locked(ts)) {
		generation = ts->hid_target_generation;
		product_id = ts->hid_product_id;
		create = true;
	}
	mutex_unlock(&ts->io_lock);

	if (old_hid)
		hid_destroy_device(old_hid);
	if (!create)
		return;

	hid = g6ts_ipts_hid_allocate(ts, product_id);
	if (IS_ERR(hid)) {
		ret = PTR_ERR(hid);
		mutex_lock(&ts->io_lock);
		retry = g6ts_ipts_hid_retry_locked(ts, generation);
		mutex_unlock(&ts->io_lock);
		dev_err(&ts->spi->dev,
			"failed to allocate IPTS HID device: %d%s\n", ret,
			retry ? "; retrying" : "");
		if (retry)
			mod_delayed_work(system_wq, &ts->hid_rebind_work,
					 msecs_to_jiffies(G6TS_HID_REBIND_RETRY_MS));
		return;
	}

	mutex_lock(&ts->io_lock);
	if (!g6ts_ipts_hid_desired_locked(ts) || ts->ipts_hid ||
	    ts->hid_target_generation != generation ||
	    ts->hid_product_id != product_id) {
		mutex_unlock(&ts->io_lock);
		hid_destroy_device(hid);
		return;
	}
	ts->ipts_hid = hid;
	ts->ipts_hid_ready = false;
	ts->hid_bound_generation = generation;
	mutex_unlock(&ts->io_lock);

	ret = hid_add_device(hid);
	if (!ret && hid->claimed != HID_CLAIMED_HIDRAW)
		ret = -ENODEV;

	mutex_lock(&ts->io_lock);
	if (!ret && ts->ipts_hid == hid &&
	    g6ts_ipts_hid_desired_locked(ts) &&
	    ts->hid_target_generation == generation) {
		ts->ipts_hid_ready = true;
		ts->hid_rebind_failures = 0;
		mutex_unlock(&ts->io_lock);
		dev_info(&ts->spi->dev,
			 "IPTS HIDRAW bridge ready generation=%u product=%04x\n",
			 generation, product_id);
		return;
	}
	if (ts->ipts_hid == hid)
		g6ts_ipts_hid_detach_locked(ts);
	if (ret)
		retry = g6ts_ipts_hid_retry_locked(ts, generation);
	mutex_unlock(&ts->io_lock);

	if (ret)
		dev_err(&ts->spi->dev,
			"failed to register HIDRAW-only IPTS device: %d%s\n",
			ret, retry ? "; retrying" : "");
	hid_destroy_device(hid);
	if (retry)
		mod_delayed_work(system_wq, &ts->hid_rebind_work,
				 msecs_to_jiffies(G6TS_HID_REBIND_RETRY_MS));
}

static int g6ts_expect_response(struct g6ts *ts, u8 response_class,
				u8 content_id, size_t min_content_len)
{
	if (ts->last_class != response_class ||
	    ts->last_content_id != content_id ||
	    ts->last_content_len < min_content_len)
		return -EPROTO;

	return 0;
}

static int g6ts_validate_sp11_device_descriptor(struct g6ts *ts)
{
	const struct hidspi_dev_descriptor *descriptor;
	u16 product_id;

	if (ts->last_class != DEVICE_DESCRIPTOR_RESPONSE ||
	    ts->last_content_id != 0 ||
	    ts->last_content_len != HIDSPI_DEVICE_DESCRIPTOR_SIZE)
		return -EPROTO;

	descriptor = (const struct hidspi_dev_descriptor *)
		(ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE);
	if (le16_to_cpu(descriptor->dev_desc_len) !=
			HIDSPI_DEVICE_DESCRIPTOR_SIZE ||
	    le16_to_cpu(descriptor->bcd_ver) != 0x0300 ||
	    le16_to_cpu(descriptor->rep_desc_len) !=
			G6TS_SP11_REPORT_DESCRIPTOR_LEN ||
	    le16_to_cpu(descriptor->max_input_len) != G6TS_SP11_MAX_INPUT_LEN ||
	    le16_to_cpu(descriptor->max_output_len) != G6TS_SP11_MAX_OUTPUT_LEN ||
	    le16_to_cpu(descriptor->max_frag_len) != G6TS_SP11_MAX_FRAGMENT_LEN ||
	    le16_to_cpu(descriptor->vendor_id) != G6TS_SP11_VENDOR_ID ||
	    (le16_to_cpu(descriptor->product_id) != G6TS_SP11_X1E_PRODUCT_ID &&
	     le16_to_cpu(descriptor->product_id) != G6TS_SP11_X1P_PRODUCT_ID) ||
	    le16_to_cpu(descriptor->version_id) != G6TS_SP11_VERSION_ID ||
	    le16_to_cpu(descriptor->flags) != G6TS_SP11_DESCRIPTOR_FLAGS ||
	    le32_to_cpu(descriptor->reserved) != 0)
		return -ENODEV;

	product_id = le16_to_cpu(descriptor->product_id);
	if (ts->hid_product_id && ts->hid_product_id != product_id)
		return -EPROTO;
	ts->hid_product_id = product_id;
	return 0;
}

static int g6ts_recovery_read_expected(struct g6ts *ts, u8 response_class,
				       u8 content_id, size_t min_content_len);

static bool g6ts_windows_feedback_provider_valid(void)
{
	return g6ts_parity_display_bitmap >= 0 &&
	       g6ts_parity_display_bitmap <= U8_MAX &&
	       (g6ts_parity_stitching_flag == 0 ||
		g6ts_parity_stitching_flag == 1) &&
	       g6ts_parity_hinge_angle >= 0 &&
	       g6ts_parity_fast_host_id >= 0 &&
	       g6ts_parity_fast_host_id <= U16_MAX;
}

static bool g6ts_windows_config_provider_valid(void)
{
	return g6ts_parity_report56_identity_count ==
			G6TS_WINDOWS_REPORT56_ID_LEN &&
	       (g6ts_parity_report56_flag == 0 ||
		g6ts_parity_report56_flag == 1);
}

static void g6ts_build_windows_feedback_a1(u8 content[G6TS_WINDOWS_FEEDBACK_LEN])
{
	memset(content, 0, G6TS_WINDOWS_FEEDBACK_LEN);
	content[0] = 0x8e;
	content[1] = 0xa1;
	content[2] = g6ts_parity_display_bitmap;
	content[3] = g6ts_parity_stitching_flag;
	put_unaligned_le32(g6ts_parity_hinge_angle, &content[4]);
	put_unaligned_le16(g6ts_parity_fast_host_id, &content[40]);
}

static void g6ts_build_windows_feedback_a5(struct g6ts *ts,
					   u8 content[G6TS_WINDOWS_FEEDBACK_LEN])
{
	memset(content, 0, G6TS_WINDOWS_FEEDBACK_LEN);
	content[0] = 0x8e;
	content[1] = 0xa5;
	/* Initial V06 sequence and current-feedback flags with no pen record. */
	content[2] = 0;
	content[3] = BIT(1);
	put_unaligned_le16(ts->a5.fast_host_id, &content[39]);
	/* The V06 sender unconditionally adds validity bit 0x0040. */
	put_unaligned_le16(0x0040, &content[46]);
}

static void g6ts_send_heat_feedback_a5(struct g6ts *ts,
				       bool after_heat_group)
{
	/*
	 * This bounded encoder preserves the hardware-proven V06 timing and wire
	 * template; only sequence, cycle phase, and configured FastHostId are
	 * validated live state.  Offsets 8..38 and the validity maps remain
	 * unmapped, so never replay a captured body or accept an opaque userspace
	 * payload in their place.
	 */
	u8 content[G6TS_WINDOWS_FEEDBACK_LEN] = { 0 };
	int ret;

	if (!g6ts_heat_feedback)
		return;
	if (!ts->a5.fast_host_id_valid) {
		dev_warn_once(&ts->spi->dev,
			      "heat feedback disabled: parity_fast_host_id is unavailable or out of range\n");
		return;
	}

	content[0] = 0x8e;
	content[1] = 0xa5;
	content[2] = ts->a5.sequence;
	content[3] = 0x02;
	put_unaligned_le16(0x1770, &content[12]);
	content[34] = 0xff;
	content[35] = 0x4c;
	put_unaligned_le16(ts->a5.fast_host_id, &content[39]);
	content[41] = 0x01;
	content[43] = after_heat_group ? 0x04 : 0x02;
	content[44] = 0xff;
	content[46] = 0x52;
	content[47] = 0x43;
	content[48] = 0xff;
	content[49] = 0x02;
	content[50] = 0x01;

	ret = g6ts_dma_hidspi_output(ts, OUTPUT_REPORT, 0x09, content,
				     sizeof(content));
	if (ret) {
		dev_warn_ratelimited(&ts->spi->dev,
				     "failed to send heat feedback phase %#02x: %d\n",
				     content[43], ret);
		return;
	}

	/* Advance only after a successful write; u8 wrap is the wire behavior. */
	ts->a5.sequence++;
	ts->heat_feedback_sent++;
	if (after_heat_group)
		ts->heat_feedback_phase_a++;
	else
		ts->heat_feedback_phase_b++;
}

static bool g6ts_windows_cfu_provider_valid(void)
{
	/* Force-immediate and force-ignore-version are development-only bits. */
	return g6ts_parity_cfu_offer_count == G6TS_WINDOWS_CFU_OFFER_LEN &&
	       !(g6ts_parity_cfu_offer[1] & (BIT(6) | BIT(7))) &&
	       !memcmp(g6ts_parity_cfu_offer, g6ts_sp11_cfu_offer,
		       G6TS_WINDOWS_CFU_OFFER_LEN);
}

static void
g6ts_build_windows_cfu_info(u8 content[G6TS_WINDOWS_CFU_OFFER_LEN],
			    u8 information_code)
{
	memset(content, 0, G6TS_WINDOWS_CFU_OFFER_LEN);
	content[0] = information_code;
	content[2] = 0xff;
	content[3] = G6TS_WINDOWS_CFU_TOKEN;
}

static bool g6ts_windows_cfu_response_well_formed(const u8 *content)
{
	return content[0] == 0 && content[1] == 0 && content[2] == 0 &&
	       content[3] == G6TS_WINDOWS_CFU_TOKEN &&
	       get_unaligned_le32(&content[4]) == 0 &&
	       content[9] == 0 && content[10] == 0 && content[11] == 0 &&
	       content[13] == 0 && content[14] == 0 && content[15] == 0;
}

static int g6ts_windows_cfu_send_locked(struct g6ts *ts, const u8 *content)
{
	int ret;

	ret = g6ts_dma_hidspi_output(ts, OUTPUT_REPORT, 0x65, content,
				     G6TS_WINDOWS_CFU_OFFER_LEN);
	if (ret)
		return ret;

	ret = g6ts_recovery_read_expected(ts, DATA, 0x65,
					  G6TS_WINDOWS_CFU_OFFER_LEN);
	if (ret || ts->last_content_len != G6TS_WINDOWS_CFU_OFFER_LEN)
		return ret ? ret : -EPROTO;

	content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	if (!g6ts_windows_cfu_response_well_formed(content))
		return -EPROTO;

	return 0;
}

static int g6ts_windows_cfu_expect_info_accept(struct g6ts *ts)
{
	const u8 *content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;

	/* The target returns 0xff in the don't-care reject-reason byte. */
	if (content[8] != 0xff || content[12] != 0x01)
		return -EPROTO;

	return 0;
}

static int g6ts_windows_cfu_inventory_locked(struct g6ts *ts)
{
	u8 transaction[G6TS_WINDOWS_CFU_OFFER_LEN];
	const u8 *content;
	int ret;

	/* Captured gap between the config owner and CFU collection attach. */
	msleep(G6TS_WINDOWS_CFU_DELAY_MS);

	ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_GET_VERSION;
	ret = g6ts_dma_feature_exchange(ts, GET_FEATURE, 0x60, NULL, 0);
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, GET_FEATURE_RESPONSE, 0x60,
				   G6TS_WINDOWS_CFU_VERSION_LEN);
	if (ret || ts->last_content_len != G6TS_WINDOWS_CFU_VERSION_LEN)
		return ret ? ret : -EPROTO;
	content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	memcpy(ts->parity_cfu_version_prefix, content,
	       sizeof(ts->parity_cfu_version_prefix));
	/*
	 * Decode only the header and the one declared component. Bytes after the
	 * component-count boundary are not inputs to the CFU decision.
	 */
	if (content[0] != 1 || content[1] != 0 || content[2] != 0 ||
	    content[3] != 0x04 ||
	    memcmp(&content[4], &g6ts_parity_cfu_offer[4], 4) ||
	    content[9] != g6ts_parity_cfu_offer[2])
		return -EPROTO;

	ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_START_TRANSACTION;
	g6ts_build_windows_cfu_info(transaction, 0);
	ret = g6ts_windows_cfu_send_locked(ts, transaction);
	if (ret)
		return ret;
	ret = g6ts_windows_cfu_expect_info_accept(ts);
	if (ret)
		return ret;

	ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_START_LIST;
	g6ts_build_windows_cfu_info(transaction, 1);
	ret = g6ts_windows_cfu_send_locked(ts, transaction);
	if (ret)
		return ret;
	ret = g6ts_windows_cfu_expect_info_accept(ts);
	if (ret)
		return ret;

	ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_OFFER;
	memcpy(transaction, g6ts_parity_cfu_offer, sizeof(transaction));
	transaction[3] = G6TS_WINDOWS_CFU_TOKEN;
	ret = g6ts_windows_cfu_send_locked(ts, transaction);
	if (ret)
		return ret;
	content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	memcpy(ts->parity_cfu_offer_response, content,
	       sizeof(ts->parity_cfu_offer_response));
	if (content[8] != 0 || content[12] != 0x02) {
		/*
		 * Windows branches into payload, replay, or busy handling here. This
		 * driver intentionally implements none of those firmware-update paths.
		 */
		ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_BRANCH_REQUIRED;
		ts->parity_cfu_branch_required = true;
		dev_notice(&ts->spi->dev,
			   "Windows CFU branch required: reject_reason=%#x status=%#x; no payload sent\n",
			   content[8], content[12]);
		return 0;
	}

	ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_END_LIST;
	g6ts_build_windows_cfu_info(transaction, 2);
	ret = g6ts_windows_cfu_send_locked(ts, transaction);
	if (ret)
		return ret;
	ret = g6ts_windows_cfu_expect_info_accept(ts);
	if (ret)
		return ret;

	/* Captured gap before the device-config owner's post-CFU query. */
	msleep(G6TS_WINDOWS_FINAL_CONFIG_DELAY_MS);
	ts->initialization_stage = G6TS_INIT_WINDOWS_FINAL_FEATURE73;
	ret = g6ts_dma_feature_exchange(ts, GET_FEATURE, 0x73, NULL, 0);
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, GET_FEATURE_RESPONSE, 0x73, 2);
	if (ret || ts->last_content_len != 2)
		return ret ? ret : -EPROTO;
	content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	memcpy(ts->parity_feature73_late, content,
	       sizeof(ts->parity_feature73_late));

	if (!g6ts_parity_heat_input) {
		ts->initialization_stage = G6TS_INIT_WINDOWS_HEAT_OWNER_REQUIRED;
		ts->parity_heat_owner_required = true;
		dev_notice(&ts->spi->dev,
			   "Windows cold chronology reached Heat boundary: early73=%2ph late73=%2ph; input remains disabled\n",
			   ts->parity_feature73_early,
			   ts->parity_feature73_late);
		return 0;
	}

	/*
	 * SET_FEATURE 0x05 already activated the Heat collection in the captured
	 * owner order.  Do not send another mode command here: merely open the
	 * response consumer and require the first complete report 0x12 to pass the
	 * normal structural parser before declaring input ready.
	 */
	ts->initialization_stage = G6TS_INIT_WAIT_HEAT;
	ts->awaiting_ready_heat = true;
	ts->mode_enabled = true;
	dev_notice(&ts->spi->dev,
		   "Windows init+CFU chronology complete: early73=%2ph late73=%2ph; waiting for first valid Heat frame\n",
		   ts->parity_feature73_early, ts->parity_feature73_late);
	return 0;
}

/*
 * Reproduce only the proven, serialized part of the Windows cold collection
 * attach. Dynamic values must come from their owning provider. Returning
 * success with mode_enabled clear intentionally leaves this diagnostic driver
 * bound and observable without claiming that normal touch mode was reached.
 */
static int g6ts_windows_cold_attach_locked(struct g6ts *ts)
{
	u8 feedback[G6TS_WINDOWS_FEEDBACK_LEN];
	u8 report56[G6TS_WINDOWS_REPORT56_ID_LEN + 1];
	const u8 *content;
	int ret;

	ts->initialization_stage = G6TS_INIT_WINDOWS_EARLY_FEATURE73;
	ret = g6ts_dma_feature_exchange(ts, GET_FEATURE, 0x73, NULL, 0);
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, GET_FEATURE_RESPONSE, 0x73, 2);
	if (ret || ts->last_content_len != 2)
		return ret ? ret : -EPROTO;
	content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	memcpy(ts->parity_feature73_early, content,
	       sizeof(ts->parity_feature73_early));
	if (content[0] != 0xfe || content[1] != 0xff)
		return -EPROTO;

	ts->initialization_stage = G6TS_INIT_WINDOWS_HEAT_CAPS06;
	ret = g6ts_dma_feature_exchange(ts, GET_FEATURE,
					G6TS_IPTS_METADATA_REPORT_ID, NULL, 0);
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, GET_FEATURE_RESPONSE,
				   G6TS_IPTS_METADATA_REPORT_ID,
				   G6TS_IPTS_METADATA_LEN);
	if (ret || ts->last_content_len != G6TS_IPTS_METADATA_LEN)
		return ret ? ret : -EPROTO;
	ret = g6ts_cache_ipts_metadata_locked(ts);
	if (ret)
		return ret;
	content = ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE;
	memcpy(ts->parity_feature06_prefix, content,
	       sizeof(ts->parity_feature06_prefix));

	if (!g6ts_windows_feedback_provider_valid()) {
		ts->initialization_stage = G6TS_INIT_WINDOWS_FEEDBACK_REQUIRED;
		ts->parity_feedback_required = true;
		ts->mode_enabled = false;
		dev_notice(&ts->spi->dev,
			   "Windows parity boundary reached: early feature73=%2ph; A1/A5 provider inputs are required\n",
			   ts->parity_feature73_early);
		return 0;
	}

	ts->initialization_stage = G6TS_INIT_WINDOWS_FEEDBACK_A1;
	g6ts_build_windows_feedback_a1(feedback);
	ret = g6ts_dma_hidspi_output(ts, OUTPUT_REPORT, 0x09, feedback,
				     sizeof(feedback));
	if (ret)
		return ret;
	/* The captured SP11 cold attach acknowledges A1 with DATA report A0={01}. */
	ret = g6ts_recovery_read_expected(ts, DATA, 0xa0, 1);
	if (ret || ts->last_content_len != 1 ||
	    ts->body[HIDSPI_INPUT_BODY_HEADER_SIZE] != 0x01)
		return ret ? ret : -EPROTO;

	ts->initialization_stage = G6TS_INIT_WINDOWS_FEEDBACK_A5;
	g6ts_build_windows_feedback_a5(ts, feedback);
	ret = g6ts_dma_hidspi_output(ts, OUTPUT_REPORT, 0x09, feedback,
				     sizeof(feedback));
	if (ret)
		return ret;

	ts->initialization_stage = G6TS_INIT_WINDOWS_SET_FEATURE05;
	ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x05,
					g6ts_mode_enable,
					sizeof(g6ts_mode_enable));
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, SET_FEATURE_RESPONSE, 0x05, 0);
	if (ret || ts->last_content_len != 0)
		return ret ? ret : -EPROTO;

	if (!g6ts_windows_config_provider_valid()) {
		ts->initialization_stage = G6TS_INIT_WINDOWS_CONFIG_OWNER_REQUIRED;
		ts->parity_config_owner_required = true;
		ts->mode_enabled = false;
		dev_notice(&ts->spi->dev,
			   "Windows parity boundary reached: report56 platform identity and flag are required\n");
		return 0;
	}

	/*
	 * The complete cold KDNET trace places 476 ms between the SET_FEATURE 0x05
	 * response and the independent config owner's GET_FEATURE 0x70.  Preserve
	 * a conservative 470 ms owner boundary; this is not a panel retry delay.
	 */
	msleep(G6TS_WINDOWS_CONFIG_DELAY_MS);

	ts->initialization_stage = G6TS_INIT_WINDOWS_GET_FEATURE70;
	ret = g6ts_dma_feature_exchange(ts, GET_FEATURE, 0x70, NULL, 0);
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, GET_FEATURE_RESPONSE, 0x70, 1);
	if (ret || ts->last_content_len != 1 ||
	    ts->body[HIDSPI_INPUT_BODY_HEADER_SIZE] != 0x02)
		return ret ? ret : -EPROTO;

	ts->initialization_stage = G6TS_INIT_WINDOWS_SET_FEATURE70;
	ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x70,
					g6ts_mode_enable,
					sizeof(g6ts_mode_enable));
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, SET_FEATURE_RESPONSE, 0x70, 0);
	if (ret || ts->last_content_len != 0)
		return ret ? ret : -EPROTO;

	memcpy(report56, g6ts_parity_report56_identity,
	       G6TS_WINDOWS_REPORT56_ID_LEN);
	report56[G6TS_WINDOWS_REPORT56_ID_LEN] = g6ts_parity_report56_flag;
	ts->initialization_stage = G6TS_INIT_WINDOWS_SET_FEATURE56;
	ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x56,
					report56, sizeof(report56));
	if (ret)
		return ret;
	ret = g6ts_expect_response(ts, SET_FEATURE_RESPONSE, 0x56, 0);
	if (ret || ts->last_content_len != 0)
		return ret ? ret : -EPROTO;

	/* CFU collection attach is deliberately separate from touch activation. */
	if (!g6ts_parity_cfu_inventory ||
	    !g6ts_windows_cfu_provider_valid()) {
		ts->initialization_stage = G6TS_INIT_WINDOWS_CFU_OWNER_REQUIRED;
		ts->parity_cfu_owner_required = true;
		ts->mode_enabled = false;
		dev_notice(&ts->spi->dev,
			   "Windows parity reached the CFU-owner boundary; inventory opt-in and exact offer are required\n");
		return 0;
	}

	return g6ts_windows_cfu_inventory_locked(ts);
}

static int g6ts_recovery_read_expected(struct g6ts *ts, u8 response_class,
				       u8 content_id, size_t min_content_len)
{
	unsigned int response_index;
	int ret;

	for (response_index = 0;
	     response_index < G6TS_FEATURE_RESPONSE_LIMIT;
	     response_index++) {
		ret = g6ts_wait_pending(ts, 1000);
		if (ret)
			return ret;

		ret = g6ts_dma_read_response(ts);
		if (ret)
			return ret;
		if (ts->last_class == response_class &&
		    ts->last_content_id == content_id &&
		    ts->last_content_len >= min_content_len)
			return 0;
		if (ts->last_class == RESET_RESPONSE) {
			g6ts_note_panel_reset_locked(ts, false);
			return -EPIPE;
		}

		/* Stale input can precede the solicited reply. */
		if (ts->last_class == DATA ||
		    (ts->last_class == OUTPUT_REPORT_RESPONSE &&
		     ts->last_content_id == 0x09))
			continue;

		return -EPROTO;
	}

	return -EOVERFLOW;
}

/*
 * Cold startup requires the validated power/reset sequence and its resulting
 * RESET_RESPONSE.  A panel-originated reset has already completed that part
 * and its response was consumed by the IRQ thread, so the Phase 77 path starts
 * at host re-enumeration instead of forcing another hardware reset.
 */
static int g6ts_full_reinitialize_locked(struct g6ts *ts,
					 enum g6ts_recovery_path path)
{
	int ret;

	ts->mode_enabled = false;
	ts->awaiting_ready_heat = false;
	ts->expected_report_descriptor_len = 0;
	ts->initialization_stage = G6TS_INIT_IDLE;
	ts->parity_feedback_required = false;
	ts->parity_config_owner_required = false;
	ts->parity_cfu_owner_required = false;
	ts->parity_cfu_branch_required = false;
	ts->parity_heat_owner_required = false;
	memset(ts->parity_feature73_early, 0,
	       sizeof(ts->parity_feature73_early));
	memset(ts->parity_feature73_late, 0,
	       sizeof(ts->parity_feature73_late));
	memset(ts->parity_feature06_prefix, 0,
	       sizeof(ts->parity_feature06_prefix));
	memset(ts->parity_cfu_version_prefix, 0,
	       sizeof(ts->parity_cfu_version_prefix));
	memset(ts->parity_cfu_offer_response, 0,
	       sizeof(ts->parity_cfu_offer_response));

	if (path == G6TS_RECOVERY_HARDWARE) {
		ts->hardware_recovery_attempts++;
		ret = g6ts_power_off(ts);
		if (ret)
			return ret;
		msleep(100);
		ret = g6ts_windows_init_parity && !g6ts_parity_linux_power ?
			g6ts_windows_power_on(ts) : g6ts_power_on(ts);
		if (ret)
			return ret;

		ts->initialization_stage = G6TS_INIT_RESET_RESPONSE;
		ret = g6ts_wait_pending(ts, 1000);
		if (ret)
			goto out;
		ret = g6ts_dma_read_response(ts);
		if (ret)
			goto out;
		ret = g6ts_expect_response(ts, RESET_RESPONSE, 0, 0);
		if (ret)
			goto out;
	} else {
		ts->software_recovery_attempts++;
		ts->initialization_stage = G6TS_INIT_RESET_RESPONSE;
	}

	ts->initialization_stage = G6TS_INIT_DEVICE_DESCRIPTOR;
	ret = g6ts_dma_output(ts, g6ts_device_descriptor_cmd,
			      sizeof(g6ts_device_descriptor_cmd));
	if (ret) {
		ts->fatal_transport_error = true;
		goto out;
	}
	ret = g6ts_recovery_read_expected(ts, DEVICE_DESCRIPTOR_RESPONSE, 0,
					  HIDSPI_DEVICE_DESCRIPTOR_SIZE);
	if (ret)
		goto out;
	ret = g6ts_validate_sp11_device_descriptor(ts);
	if (ret)
		goto out;
	if (!ts->expected_report_descriptor_len ||
	    ts->expected_report_descriptor_len >
		    G6TS_MAX_BODY - HIDSPI_INPUT_BODY_HEADER_SIZE) {
		ret = -EPROTO;
		goto out;
	}

	ts->initialization_stage = G6TS_INIT_REPORT_DESCRIPTOR;
	ret = g6ts_dma_output(ts, g6ts_report_descriptor_cmd,
			      sizeof(g6ts_report_descriptor_cmd));
	if (ret) {
		ts->fatal_transport_error = true;
		goto out;
	}
	ret = g6ts_recovery_read_expected(ts, REPORT_DESCRIPTOR_RESPONSE, 0,
					  ts->expected_report_descriptor_len);
	if (ret || ts->last_content_len != ts->expected_report_descriptor_len) {
		if (!ret)
			ret = -EPROTO;
		goto out;
	}
	if (ts->report_descriptor_valid) {
		if (memcmp(ts->report_descriptor,
			   ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE,
			   G6TS_SP11_REPORT_DESCRIPTOR_LEN)) {
			dev_err(&ts->spi->dev,
				"report descriptor changed across transport generation\n");
			ret = -EPROTO;
			goto out;
		}
	} else {
		memcpy(ts->report_descriptor,
		       ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE,
		       G6TS_SP11_REPORT_DESCRIPTOR_LEN);
		ts->report_descriptor_valid = true;
	}

	if (g6ts_windows_init_parity) {
		/* A software reset has a different, multi-owner Windows ordering. */
		if (path != G6TS_RECOVERY_HARDWARE) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		ret = g6ts_windows_cold_attach_locked(ts);
		if (ret)
			goto out;
		return 0;
	}

	/*
	 * Enter the panel's streaming personality with the smallest sequence
	 * proven by the Phase 55 through Phase 65 cold boots and reset recoveries.
	 * Reports 0x60, 0x65, 0x06, 0x09, and 0x73 in the Windows ETW capture
	 * belong to collection/application setup. Replaying them here prevented
	 * Heat from starting in both the Phase 66 and Phase 67 hardware trials.
	 */
	ts->initialization_stage = G6TS_INIT_SET_FEATURE05;
	ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x05,
					g6ts_mode_enable,
					sizeof(g6ts_mode_enable));
	if (ret)
		goto out;
	ret = g6ts_expect_response(ts, SET_FEATURE_RESPONSE, 0x05, 0);
	if (ret)
		goto out;

	ts->initialization_stage = G6TS_INIT_GET_FEATURE70;
	ts->mode_config_valid = false;
	ts->mode_config_len = 0;
	ret = g6ts_dma_feature_exchange(ts, GET_FEATURE, 0x70, NULL, 0);
	if (ret)
		goto out;
	ret = g6ts_expect_response(ts, GET_FEATURE_RESPONSE, 0x70, 1);
	if (ret)
		goto out;
	/*
	 * Capture the Linux panel's GET_FEATURE 0x70 content now, before another
	 * response overwrites ts->body. Phase 72 used it in the empirically stable
	 * combined sequence; this is not a byte-for-byte Windows feature exchange.
	 * Body layout: [0]=class [1..2]=len [3]=id [4..]=content.
	 */
	if (g6ts_mode_config_fix) {
		size_t cfg_len = ts->last_content_len;

		if (cfg_len > sizeof(ts->mode_config))
			cfg_len = sizeof(ts->mode_config);
		memcpy(ts->mode_config,
		       ts->body + HIDSPI_INPUT_BODY_HEADER_SIZE, cfg_len);
		ts->mode_config_len = cfg_len;
		ts->mode_config_valid = cfg_len > 0;
		dev_info(&ts->spi->dev,
			 "phase72: GET_FEATURE 0x70 config len=%zu bytes=%*ph\n",
			 cfg_len, (int)cfg_len, ts->mode_config);
	}

	ts->initialization_stage = G6TS_INIT_SET_FEATURE70;
	if (g6ts_feature70_one_byte) {
		dev_info(&ts->spi->dev,
			 "feature70-one-byte: SET_FEATURE 0x70 len=1 bytes=01\n");
		ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x70,
						g6ts_mode_enable,
						sizeof(g6ts_mode_enable));
	} else if (g6ts_mode_config_fix && ts->mode_config_valid) {
		/*
		 * Phase 72 Linux sequence: SET_FEATURE 0x70 is 0x01 followed by the
		 * panel's GET_FEATURE 0x70 content. Windows declares one logical content
		 * byte for its captured SET_FEATURE 0x70 writes.
		 */
		u8 mode_setup[1 + sizeof(ts->mode_config)];
		size_t setup_len = 1 + ts->mode_config_len;

		mode_setup[0] = 0x01;
		memcpy(&mode_setup[1], ts->mode_config, ts->mode_config_len);
		dev_info(&ts->spi->dev,
			 "phase72: SET_FEATURE 0x70 derived len=%zu bytes=%*ph\n",
			 setup_len, (int)setup_len, mode_setup);
		ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x70,
						mode_setup, setup_len);
	} else {
		ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x70,
						g6ts_mode_enable,
						sizeof(g6ts_mode_enable));
	}
	if (ret)
		goto out;
	ret = g6ts_expect_response(ts, SET_FEATURE_RESPONSE, 0x70, 0);
	if (ret)
		goto out;

	/*
	 * Phase 72 emits a short OUTPUT_REPORT 0x09 containing 0x8e followed by the
	 * Linux GET_FEATURE 0x70 content. KDNET confirms that Windows uses report
	 * 0x09 here, but with 63-byte content rather than this short form. This is
	 * best-effort: the read path already skips OUTPUT_REPORT_RESPONSE, so a
	 * missing or late acknowledgment does not abort initialization.
	 */
	if (g6ts_mode_config_fix && ts->mode_config_valid) {
		u8 report09[1 + sizeof(ts->mode_config)];
		size_t report_len = 1 + ts->mode_config_len;

		report09[0] = 0x8e;
		memcpy(&report09[1], ts->mode_config, ts->mode_config_len);
		dev_info(&ts->spi->dev,
			 "phase72: OUTPUT_REPORT 0x09 len=%zu bytes=%*ph\n",
			 report_len, (int)report_len, report09);
		ret = g6ts_dma_hidspi_output(ts, OUTPUT_REPORT, 0x09,
					     report09, report_len);
		if (ret) {
			ts->fatal_transport_error = true;
			goto out;
		}
	}

	ts->initialization_stage = G6TS_INIT_SET_FEATURE56;
	ret = g6ts_dma_feature_exchange(ts, SET_FEATURE, 0x56,
					g6ts_mode_handshake,
					sizeof(g6ts_mode_handshake));
	if (ret)
		goto out;
	ret = g6ts_expect_response(ts, SET_FEATURE_RESPONSE, 0x56, 0);
	if (ret)
		goto out;

	if (g6ts_reset_recovery_v2) {
		ts->initialization_stage = G6TS_INIT_WAIT_HEAT;
		/*
		 * Do not synchronously wait here.  The panel produces Heat on touch,
		 * so waiting before enabling the IRQ response path deadlocks startup.
		 * The first frame is parsed and admitted by g6ts_handle_data_report().
		 */
		ts->awaiting_ready_heat = true;
	}

	ts->mode_enabled = true;
	return 0;
out:
	ts->mode_enabled = false;
	return ret;
}

static void g6ts_recovery_work(struct work_struct *work)
{
	struct g6ts *ts = container_of(to_delayed_work(work), struct g6ts,
				      recovery_work);
	enum g6ts_recovery_path path;
	bool retry = false;
	int ret;

	mutex_lock(&ts->io_lock);
	if (g6ts_is_quiescing(ts)) {
		mutex_unlock(&ts->io_lock);
		return;
	}

	ts->mode_enabled = false;
	g6ts_release_inputs(ts);
	path = ts->recovery_path;
	ret = g6ts_full_reinitialize_locked(ts, path);
	if (!ret) {
		ts->recovery_fail_streak = 0;
		ts->recovery_successes++;
		if (path == G6TS_RECOVERY_HARDWARE)
			ts->rapid_reset_streak = 0;
		ts->recovery_path = G6TS_RECOVERY_HARDWARE;
		if (ts->parity_feedback_required ||
		    ts->parity_config_owner_required ||
		    ts->parity_cfu_owner_required ||
		    ts->parity_cfu_branch_required ||
		    ts->parity_heat_owner_required)
			dev_info(&ts->spi->dev,
				 "Windows parity attach paused at %s; touch input intentionally disabled\n",
				 g6ts_initialization_stage_name(ts->initialization_stage));
		else
			dev_info(&ts->spi->dev,
				 "touch controller initialized path=%s recoveries=%llu resets=%llu\n",
				 path == G6TS_RECOVERY_SOFTWARE ?
					 "software" : "hardware",
				 ts->recovery_successes,
				 ts->reset_notifications);
	} else {
		ts->recovery_failures++;
		ts->recovery_fail_streak++;
		if (path == G6TS_RECOVERY_SOFTWARE &&
		    !ts->fatal_transport_error) {
			ts->software_recovery_fallbacks++;
			ts->recovery_path = G6TS_RECOVERY_HARDWARE;
			retry = true;
		} else {
			retry = !ts->fatal_transport_error &&
				ts->recovery_fail_streak < G6TS_RECOVERY_LIMIT;
		}
		dev_warn(&ts->spi->dev,
			 "touch controller initialization failed path=%s stage=%s ret=%d failures=%llu%s\n",
			 path == G6TS_RECOVERY_SOFTWARE ? "software" : "hardware",
			 g6ts_initialization_stage_name(ts->initialization_stage),
			 ret, ts->recovery_failures,
			 retry ? "; retrying" : "");
	}
	mutex_unlock(&ts->io_lock);
	if (!ret && READ_ONCE(ts->mode_enabled))
		mod_delayed_work(system_wq, &ts->hid_rebind_work,
				 msecs_to_jiffies(G6TS_HID_REBIND_STABLE_MS));
	else
		mod_delayed_work(system_wq, &ts->hid_rebind_work, 0);

	if (retry && !g6ts_is_quiescing(ts))
		schedule_delayed_work(&ts->recovery_work,
				      msecs_to_jiffies(G6TS_RECOVERY_RETRY_MS));
}

static int g6ts_probe(struct spi_device *spi)
{
	struct g6ts *ts;
	int ret;

	if (g6ts_behavior_v2 && g6ts_windows_orchestrator)
		return dev_err_probe(&spi->dev, -EINVAL,
				     "behavior_v2 and windows_orchestrator are mutually exclusive\n");
	if (g6ts_reset_storm_breaker && !g6ts_reset_recovery_v2)
		return dev_err_probe(&spi->dev, -EINVAL,
				     "reset_storm_breaker requires reset_recovery_v2\n");
	if (g6ts_ready_quiesce && !g6ts_host_fault_recovery)
		return dev_err_probe(&spi->dev, -EINVAL,
				     "ready_quiesce requires host_fault_recovery\n");
	if (g6ts_parity_heat_input &&
	    (!g6ts_windows_init_parity || !g6ts_parity_cfu_inventory))
		return dev_err_probe(&spi->dev, -EINVAL,
				     "parity_heat_input requires windows_init_parity and parity_cfu_inventory\n");
	if (g6ts_windows_read_cadence && !g6ts_windows_init_parity)
		return dev_err_probe(&spi->dev, -EINVAL,
				     "windows_read_cadence requires windows_init_parity\n");

	ts = devm_kzalloc(&spi->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->body = devm_kmalloc(&spi->dev, G6TS_MAX_BODY, GFP_KERNEL);
	if (!ts->body)
		return -ENOMEM;
	ts->spi = spi;
	/*
	 * Initial attach A5 carries sequence zero.  Streaming starts at one and
	 * deliberately survives transport boundaries until evidence says otherwise.
	 */
	ts->a5.sequence = 1;
	if (g6ts_parity_fast_host_id >= 0 &&
	    g6ts_parity_fast_host_id <= U16_MAX) {
		ts->a5.fast_host_id = g6ts_parity_fast_host_id;
		ts->a5.fast_host_id_valid = true;
	}
	ts->interrupt_gpio = devm_gpiod_get(&spi->dev, "interrupt", GPIOD_IN);
	if (IS_ERR(ts->interrupt_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(ts->interrupt_gpio),
				     "failed to get interrupt GPIO\n");
	ts->interrupt_irq = gpiod_to_irq(ts->interrupt_gpio);
	if (ts->interrupt_irq < 0)
		return dev_err_probe(&spi->dev, ts->interrupt_irq,
				     "failed to map interrupt GPIO\n");
	ts->power_gpio =
		devm_gpiod_get_optional(&spi->dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(ts->power_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(ts->power_gpio),
				     "failed to get power GPIO\n");
	ts->reset_gpio =
		devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(ts->reset_gpio),
				     "failed to get reset GPIO\n");
	if (!!ts->power_gpio != !!ts->reset_gpio)
		return dev_err_probe(&spi->dev, -EINVAL,
				     "power and reset GPIOs must be paired\n");

	mutex_init(&ts->io_lock);
	INIT_DELAYED_WORK(&ts->recovery_work, g6ts_recovery_work);
	INIT_DELAYED_WORK(&ts->hid_rebind_work,
			  g6ts_ipts_hid_rebind_work);
	ts->hid_target_generation = 1;
	spi_set_drvdata(spi, ts);
	ret = g6ts_heat_register(ts);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to register raw Heat device\n");
	ret = devm_request_threaded_irq(&spi->dev, ts->interrupt_irq,
					g6ts_interrupt_edge,
					g6ts_interrupt_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					G6TS_NAME, ts);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to request interrupt\n");
	ret = devm_device_add_group(&spi->dev, &g6ts_attribute_group);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to create diagnostic attributes\n");
	spi->max_speed_hz = G6TS_SPI_HZ;
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0 | SPI_TX_QUAD | SPI_RX_QUAD;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup failed\n");

	ts->input = devm_input_allocate_device(&spi->dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "Microsoft Surface G6 Touch";
	ts->input->id.bustype = BUS_SPI;
	ts->input->dev.parent = &spi->dev;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
			     G6TS_LOGICAL_MAX, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0,
			     G6TS_LOGICAL_MAX, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);
	ret = input_mt_init_slots(ts->input, G6TS_MAX_CONTACTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED |
				  INPUT_MT_TRACK);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to initialize touch slots\n");
	ret = input_register_device(ts->input);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to register touch input\n");

	ts->pen_input = devm_input_allocate_device(&spi->dev);
	if (!ts->pen_input)
		return -ENOMEM;
	ts->pen_input->name = "Microsoft Surface G6 Pen";
	ts->pen_input->id.bustype = BUS_SPI;
	ts->pen_input->dev.parent = &spi->dev;
	input_set_capability(ts->pen_input, EV_KEY, BTN_TOUCH);
	input_set_capability(ts->pen_input, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(ts->pen_input, EV_KEY, BTN_TOOL_RUBBER);
	input_set_capability(ts->pen_input, EV_KEY, BTN_STYLUS);
	input_set_abs_params(ts->pen_input, ABS_X, 0, G6TS_PEN_X_MAX, 0, 0);
	input_set_abs_params(ts->pen_input, ABS_Y, 0, G6TS_PEN_Y_MAX, 0, 0);
	input_set_abs_params(ts->pen_input, ABS_PRESSURE, 0,
			     G6TS_PEN_PRESSURE_MAX, 0, 0);
	input_set_abs_params(ts->pen_input, ABS_TILT_X,
			     -G6TS_PEN_TILT_CENTER, G6TS_PEN_TILT_CENTER, 0, 0);
	input_set_abs_params(ts->pen_input, ABS_TILT_Y,
			     -G6TS_PEN_TILT_CENTER, G6TS_PEN_TILT_CENTER, 0, 0);
	touchscreen_parse_properties(ts->pen_input, false, &ts->pen_prop);
	input_abs_set_res(ts->pen_input, ABS_X, ts->pen_prop.swap_x_y ?
			  G6TS_PEN_Y_RESOLUTION : G6TS_PEN_X_RESOLUTION);
	input_abs_set_res(ts->pen_input, ABS_Y, ts->pen_prop.swap_x_y ?
			  G6TS_PEN_X_RESOLUTION : G6TS_PEN_Y_RESOLUTION);
	input_abs_set_res(ts->pen_input, ABS_TILT_X,
			  G6TS_PEN_TILT_RESOLUTION);
	input_abs_set_res(ts->pen_input, ABS_TILT_Y,
			  G6TS_PEN_TILT_RESOLUTION);
	__set_bit(INPUT_PROP_DIRECT, ts->pen_input->propbit);
	ret = input_register_device(ts->pen_input);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to register pen input\n");

	ret = g6ts_power_on(ts);
	if (ret)
		return ret;

	g6ts_clear_last_response(ts);
	ts->recovery_path = G6TS_RECOVERY_HARDWARE;
	schedule_delayed_work(&ts->recovery_work,
			      msecs_to_jiffies(G6TS_RECOVERY_DELAY_MS));
	dev_info(&spi->dev, "touch controller initialization scheduled profile=%s\n",
		 g6ts_profile_name());
	return 0;
}

static void g6ts_remove(struct spi_device *spi)
{
	struct g6ts *ts = spi_get_drvdata(spi);
	struct hid_device *hid;

	WRITE_ONCE(ts->stopping, true);
	WRITE_ONCE(ts->quiescing, true);
	WRITE_ONCE(ts->mode_enabled, false);
	disable_irq(ts->interrupt_irq);
	cancel_delayed_work_sync(&ts->recovery_work);
	cancel_delayed_work_sync(&ts->hid_rebind_work);
	mutex_lock(&ts->io_lock);
	hid = g6ts_ipts_hid_detach_locked(ts);
	g6ts_release_inputs(ts);
	mutex_unlock(&ts->io_lock);
	/* Drain generation work queued by an in-flight transport caller. */
	cancel_delayed_work_sync(&ts->hid_rebind_work);
	if (hid)
		hid_destroy_device(hid);
	(void)g6ts_power_off(ts);
}

static int g6ts_suspend(struct device *dev)
{
	struct g6ts *ts = dev_get_drvdata(dev);
	struct hid_device *hid;
	int ret;

	WRITE_ONCE(ts->quiescing, true);
	WRITE_ONCE(ts->mode_enabled, false);
	disable_irq(ts->interrupt_irq);
	cancel_delayed_work_sync(&ts->recovery_work);
	cancel_delayed_work_sync(&ts->hid_rebind_work);

	mutex_lock(&ts->io_lock);
	WRITE_ONCE(ts->mode_enabled, false);
	g6ts_heat_generation_boundary(ts,
				      G6TS_HEAT_RECORD_F_SUSPEND);
	hid = g6ts_ipts_hid_detach_locked(ts);
	g6ts_release_inputs(ts);
	mutex_unlock(&ts->io_lock);
	/* The generation boundary queued a teardown pass; the child is detached. */
	cancel_delayed_work_sync(&ts->hid_rebind_work);
	if (hid)
		hid_destroy_device(hid);

	mutex_lock(&ts->io_lock);
	ret = g6ts_power_off(ts);
	if (ret) {
		WRITE_ONCE(ts->quiescing, false);
		ts->recovery_path = G6TS_RECOVERY_HARDWARE;
	}
	mutex_unlock(&ts->io_lock);

	if (ret) {
		enable_irq(ts->interrupt_irq);
		schedule_delayed_work(&ts->recovery_work,
				      msecs_to_jiffies(G6TS_RECOVERY_DELAY_MS));
	}

	return ret;
}

static int g6ts_resume(struct device *dev)
{
	struct g6ts *ts = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ts->io_lock);
	ts->fatal_transport_error = false;
	ts->recovery_fail_streak = 0;
	ts->recovery_path = G6TS_RECOVERY_HARDWARE;
	ret = g6ts_power_on(ts);
	if (!ret)
		WRITE_ONCE(ts->quiescing, false);
	mutex_unlock(&ts->io_lock);
	if (ret)
		return ret;

	enable_irq(ts->interrupt_irq);
	schedule_delayed_work(&ts->recovery_work,
			      msecs_to_jiffies(G6TS_RECOVERY_DELAY_MS));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(g6ts_pm_ops, g6ts_suspend, g6ts_resume);

static int g6ts_ipts_hid_probe(struct hid_device *hid,
			       const struct hid_device_id *id)
{
	int ret;

	ret = hid_parse(hid);
	if (ret)
		return ret;

	return hid_hw_start(hid, HID_CONNECT_HIDRAW);
}

static void g6ts_ipts_hid_remove(struct hid_device *hid)
{
	hid_hw_stop(hid);
}

static const struct hid_device_id g6ts_ipts_hid_ids[] = {
	{ HID_DEVICE(BUS_SPI, HID_GROUP_MSHW0485_IPTS,
		     G6TS_SP11_VENDOR_ID, G6TS_SP11_X1E_PRODUCT_ID) },
	{ HID_DEVICE(BUS_SPI, HID_GROUP_MSHW0485_IPTS,
		     G6TS_SP11_VENDOR_ID, G6TS_SP11_X1P_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, g6ts_ipts_hid_ids);

static struct hid_driver g6ts_ipts_hid_driver = {
	.name = "mshw0485-ipts",
	.id_table = g6ts_ipts_hid_ids,
	.probe = g6ts_ipts_hid_probe,
	.remove = g6ts_ipts_hid_remove,
};

static const struct acpi_device_id g6ts_acpi_match[] = {
	{ "MSHW0485", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, g6ts_acpi_match);

static const struct of_device_id g6ts_of_match[] = {
	{ .compatible = "microsoft,mshw0485" },
	{ }
};
MODULE_DEVICE_TABLE(of, g6ts_of_match);

static const struct spi_device_id g6ts_spi_id[] = {
	{ "mshw0485" },
	{ }
};
MODULE_DEVICE_TABLE(spi, g6ts_spi_id);

static struct spi_driver g6ts_driver = {
	.driver = {
		.name = G6TS_NAME,
		.acpi_match_table = ACPI_PTR(g6ts_acpi_match),
		.of_match_table = g6ts_of_match,
		.pm = pm_sleep_ptr(&g6ts_pm_ops),
	},
	.id_table = g6ts_spi_id,
	.probe = g6ts_probe,
	.remove = g6ts_remove,
};

static int __init g6ts_init(void)
{
	int ret;

	ret = hid_register_driver(&g6ts_ipts_hid_driver);
	if (ret)
		return ret;

	ret = spi_register_driver(&g6ts_driver);
	if (ret)
		hid_unregister_driver(&g6ts_ipts_hid_driver);

	return ret;
}
module_init(g6ts_init);

static void __exit g6ts_exit(void)
{
	/* Destroy every child HID device before its special driver disappears. */
	spi_unregister_driver(&g6ts_driver);
	hid_unregister_driver(&g6ts_ipts_hid_driver);
}
module_exit(g6ts_exit);

MODULE_DESCRIPTION("Microsoft Surface G6 MSHW0485 touch and IPTS pen bridge");
MODULE_AUTHOR("SP11 reverse-engineering project");
MODULE_LICENSE("GPL");
