/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_PROTOCOL_H
#define REDFISH_ENUM_PROTOCOL_H

#include <stddef.h>

/* Protocol.Protocol */
enum rf_protocol {
	PROTOCOL_INVALID = 0,
	PROTOCOL_PCIE = 1,
	PROTOCOL_AHCI = 2,
	PROTOCOL_UHCI = 3,
	PROTOCOL_SAS = 4,
	PROTOCOL_SATA = 5,
	PROTOCOL_USB = 6,
	PROTOCOL_NVME = 7,
	PROTOCOL_FC = 8,
	PROTOCOL_ISCSI = 9,
	PROTOCOL_FCOE = 10,
	PROTOCOL_FCP = 11,
	PROTOCOL_FICON = 12,
	PROTOCOL_NVME_OVER_FABRICS = 13,
	PROTOCOL_SMB = 14,
	PROTOCOL_NF_SV3 = 15,
	PROTOCOL_NF_SV4 = 16,
	PROTOCOL_HTTP = 17,
	PROTOCOL_HTTPS = 18,
	PROTOCOL_FTP = 19,
	PROTOCOL_SFTP = 20,
	PROTOCOL_I_WARP = 21,
	PROTOCOL_RO_CE = 22,
	PROTOCOL_RO_C_EV2 = 23,
	PROTOCOL_I2_C = 24,
	PROTOCOL_TCP = 25,
	PROTOCOL_UDP = 26,
	PROTOCOL_TFTP = 27,
	PROTOCOL_GEN_Z = 28,
	PROTOCOL_MULTI_PROTOCOL = 29,
	PROTOCOL_INFINIBAND = 30,
	PROTOCOL_ETHERNET = 31,
	PROTOCOL_NV_LINK = 32,
	PROTOCOL_OEM = 33,
	PROTOCOL_DISPLAY_PORT = 34,
	PROTOCOL_HDMI = 35,
	PROTOCOL_VGA = 36,
	PROTOCOL_DVI = 37,
	PROTOCOL_CXL = 38,
	PROTOCOL_UPI = 39,
	PROTOCOL_QPI = 40,
	PROTOCOL_E_MMC = 41,
	PROTOCOL_UET = 42,
	PROTOCOL_UA_LINK = 43,
};

static inline const char *rf_protocol_to_str(enum rf_protocol val)
{
	switch (val) {
	case PROTOCOL_INVALID: return "Invalid";
	case PROTOCOL_PCIE: return "PCIe";
	case PROTOCOL_AHCI: return "AHCI";
	case PROTOCOL_UHCI: return "UHCI";
	case PROTOCOL_SAS: return "SAS";
	case PROTOCOL_SATA: return "SATA";
	case PROTOCOL_USB: return "USB";
	case PROTOCOL_NVME: return "NVMe";
	case PROTOCOL_FC: return "FC";
	case PROTOCOL_ISCSI: return "iSCSI";
	case PROTOCOL_FCOE: return "FCoE";
	case PROTOCOL_FCP: return "FCP";
	case PROTOCOL_FICON: return "FICON";
	case PROTOCOL_NVME_OVER_FABRICS: return "NVMeOverFabrics";
	case PROTOCOL_SMB: return "SMB";
	case PROTOCOL_NF_SV3: return "NFSv3";
	case PROTOCOL_NF_SV4: return "NFSv4";
	case PROTOCOL_HTTP: return "HTTP";
	case PROTOCOL_HTTPS: return "HTTPS";
	case PROTOCOL_FTP: return "FTP";
	case PROTOCOL_SFTP: return "SFTP";
	case PROTOCOL_I_WARP: return "iWARP";
	case PROTOCOL_RO_CE: return "RoCE";
	case PROTOCOL_RO_C_EV2: return "RoCEv2";
	case PROTOCOL_I2_C: return "I2C";
	case PROTOCOL_TCP: return "TCP";
	case PROTOCOL_UDP: return "UDP";
	case PROTOCOL_TFTP: return "TFTP";
	case PROTOCOL_GEN_Z: return "GenZ";
	case PROTOCOL_MULTI_PROTOCOL: return "MultiProtocol";
	case PROTOCOL_INFINIBAND: return "InfiniBand";
	case PROTOCOL_ETHERNET: return "Ethernet";
	case PROTOCOL_NV_LINK: return "NVLink";
	case PROTOCOL_OEM: return "OEM";
	case PROTOCOL_DISPLAY_PORT: return "DisplayPort";
	case PROTOCOL_HDMI: return "HDMI";
	case PROTOCOL_VGA: return "VGA";
	case PROTOCOL_DVI: return "DVI";
	case PROTOCOL_CXL: return "CXL";
	case PROTOCOL_UPI: return "UPI";
	case PROTOCOL_QPI: return "QPI";
	case PROTOCOL_E_MMC: return "eMMC";
	case PROTOCOL_UET: return "UET";
	case PROTOCOL_UA_LINK: return "UALink";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_PROTOCOL_H */
