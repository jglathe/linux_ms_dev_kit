/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SPI_GENI_QCOM_BIOSREF_H
#define _LINUX_SPI_GENI_QCOM_BIOSREF_H

#include <linux/types.h>

struct spi_device;

/*
 * Execute the asymmetric FIFO/polled transaction recovered from Surface UEFI
 * SPIDxe protocol 0x09.  This is deliberately not representable as an ordinary
 * struct spi_transfer because the command has independent TX and RX lengths.
 */
int qcom_geni_spi_biosref_xfer(struct spi_device *spi,
			       const void *tx_buf, size_t tx_len,
			       void *rx_buf, size_t rx_len,
			       u32 speed_hz, unsigned int timeout_ms);

#endif
