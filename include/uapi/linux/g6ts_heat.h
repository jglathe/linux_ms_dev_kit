/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_G6TS_HEAT_H
#define _UAPI_LINUX_G6TS_HEAT_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define G6TS_HEAT_ABI_VERSION		1U
#define G6TS_HEAT_RECORD_MAGIC		0x31483647U /* "G6H1", little-endian */
#define G6TS_HEAT_RECORD_HEADER_SIZE	32U
#define G6TS_HEAT_MAX_CONTENT_SIZE	4349U

#define G6TS_HEAT_RECORD_F_RESET		(1U << 0)
#define G6TS_HEAT_RECORD_F_SUSPEND	(1U << 1)
#define G6TS_HEAT_RECORD_F_TRANSPORT_FAULT	(1U << 2)

/*
 * One read returns one complete header followed by content_len exact report
 * content bytes.  The HID report ID is carried separately in report_id.
 * All multibyte members are little-endian and this header has no padding.
 */
struct g6ts_heat_record_header {
	__le32 magic;
	__le16 abi_version;
	__le16 header_len;
	__le32 record_len;
	__le32 generation;
	__le64 timestamp_ns;
	__le32 sequence;
	__le16 content_len;
	__u8 report_id;
	__u8 flags;
} __attribute__((__packed__));

struct g6ts_heat_info {
	__le16 abi_version;
	__le16 struct_size;
	__le16 record_header_size;
	__le16 reserved0;
	__le32 max_content_size;
	__le32 queue_capacity;
	__le64 supported_record_flags;
	__le64 reserved[3];
};

struct g6ts_heat_stats {
	__le16 abi_version;
	__le16 struct_size;
	__le32 generation;
	__le32 queued_records;
	__le32 queue_capacity;
	__le64 records_enqueued;
	__le64 records_dropped;
	__le64 queue_flushes;
	__le64 oversize_drops;
	__le64 report_0b;
	__le64 report_0c;
	__le64 report_0d;
	__le64 report_1a;
	__le64 reserved[4];
};

#define G6TS_HEAT_IOC_MAGIC		'G'
#define G6TS_HEAT_IOC_GET_INFO		\
	_IOR(G6TS_HEAT_IOC_MAGIC, 0x00, struct g6ts_heat_info)
#define G6TS_HEAT_IOC_GET_STATS		\
	_IOR(G6TS_HEAT_IOC_MAGIC, 0x01, struct g6ts_heat_stats)

#endif /* _UAPI_LINUX_G6TS_HEAT_H */
