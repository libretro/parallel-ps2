/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <memory>
#include <string/stdstring.h>
#include "common/Pcsx2Defs.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#include <iphlpapi.h>
#endif

#include <stdio.h>
#include "pcap_io.h"
#include "DEV9.h"
#include "AdapterUtils.h"
#include "net.h"
#include "PacketReader/EthernetFrame.h"
#include "PacketReader/EthernetFrameEditor.h"
#include "PacketReader/ARP/ARP_PacketEditor.h"
#ifndef PCAP_NETMASK_UNKNOWN
#define PCAP_NETMASK_UNKNOWN 0xffffffff
#endif

#ifdef _WIN32
/* The Windows adapter APIs return wide strings; the rest of the core is UTF-8. */
static bool WideStringToUTF8String(std::string& dest, const std::wstring_view& str)
{
	int mblen = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0, nullptr, nullptr);
	if (mblen < 0)
		return false;

	dest.resize(mblen);
	if (mblen > 0 && WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), dest.data(), mblen,
						 nullptr, nullptr) < 0)
	{
		return false;
	}

	return true;
}

static std::string WideStringToUTF8String(const std::wstring_view& str)
{
	std::string ret;
	if (!WideStringToUTF8String(ret, str))
		ret.clear();

	return ret;
}
#endif

#ifdef _WIN32
#define PCAPPREFIX "\\Device\\NPF_"
#endif

using namespace PacketReader;
using namespace PacketReader::ARP;
using namespace PacketReader::IP;

PCAPAdapter::PCAPAdapter()
	: NetAdapter()
{
	if (!EmuConfig.DEV9.EthEnable)
		return;
#ifdef _WIN32
	if (!load_pcap())
		return;
#endif

#ifdef _WIN32
	std::string pcapAdapter = std::string(PCAPPREFIX) + EmuConfig.DEV9.EthDevice;
#else
	std::string pcapAdapter = EmuConfig.DEV9.EthDevice;
#endif

	switched = EmuConfig.DEV9.EthApi == Pcsx2Config::DEV9Options::NetApi::PCAP_Switched;

	if (!InitPCAP(pcapAdapter, switched))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Can't open Device '%s'\n", EmuConfig.DEV9.EthDevice);
		return;
	}

	AdapterUtils::Adapter adapter;
	AdapterUtils::AdapterBuffer buffer;
	std::optional<MAC_Address> adMAC = std::nullopt;
	const bool foundAdapter = AdapterUtils::GetAdapter(EmuConfig.DEV9.EthDevice, &adapter, &buffer);
	if (foundAdapter)
		adMAC = AdapterUtils::GetAdapterMAC(&adapter);
	else
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get adapter information\n");

	if (adMAC.has_value())
	{
		hostMAC = adMAC.value();
		MAC_Address newMAC = ps2MAC;

		//Lets take the hosts last 2 bytes to make it unique on Xlink
		newMAC.bytes[5] = hostMAC.bytes[4];
		newMAC.bytes[4] = hostMAC.bytes[5];

		SetMACAddress(&newMAC);
	}
	else if (switched)
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get MAC address for adapter, proceeding with hardcoded MAC address\n");
	else
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get MAC address for adapter\n");
		pcap_close(hpcap);
		hpcap = nullptr;
		return;
	}

	if (switched && !SetMACSwitchedFilter(ps2MAC))
	{
		pcap_close(hpcap);
		hpcap = nullptr;
		log_cb(RETRO_LOG_ERROR, "DEV9: Can't open Device '%s'\n", EmuConfig.DEV9.EthDevice);
		return;
	}

	if (foundAdapter)
		InitInternalServer(&adapter);
	else
		InitInternalServer(nullptr);
}
AdapterOptions PCAPAdapter::GetAdapterOptions()
{
	return AdapterOptions::None;
}
bool PCAPAdapter::isInitialised()
{
	return hpcap != nullptr;
}
//gets a packet.rv :true success
bool PCAPAdapter::recv(NetPacket* pkt)
{
	pcap_pkthdr* header;
	const u_char* pkt_data;

	// pcap bridged will pick up packets not intended for us, returning false on those packets will incur a 1ms wait.
	// This delays getting packets we need, so instead loop untill a valid packet, or no packet, is returned from pcap_next_ex.
	while (pcap_next_ex(hpcap, &header, &pkt_data) > 0)
	{
		if (header->len > sizeof(pkt->buffer))
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: Dropped jumbo frame of size: %u\n", header->len);
			continue;
		}

		memcpy(pkt->buffer, pkt_data, header->len);
		pkt->size = (int)header->len;

		if (!switched)
			SetMACBridgedRecv(pkt);

		if (VerifyPkt(pkt, header->len))
		{
			InspectRecv(pkt);
			return true;
		}
		// continue.
	}

	return false;
}
//sends the packet .rv :true success
bool PCAPAdapter::send(NetPacket* pkt)
{
	InspectSend(pkt);
	if (NetAdapter::send(pkt))
		return true;

	// TODO: loopback broadcast packets to host pc in switched mode.
	if (!switched)
		SetMACBridgedSend(pkt);

	if (pcap_sendpacket(hpcap, (u_char*)pkt->buffer, pkt->size))
		return false;
	else
		return true;
}

void PCAPAdapter::reloadSettings()
{
	AdapterUtils::Adapter adapter;
	AdapterUtils::AdapterBuffer buffer;
	if (AdapterUtils::GetAdapter(EmuConfig.DEV9.EthDevice, &adapter, &buffer))
		ReloadInternalServer(&adapter);
	else
		ReloadInternalServer(nullptr);
}

PCAPAdapter::~PCAPAdapter()
{
	if (hpcap)
	{
		pcap_close(hpcap);
		hpcap = nullptr;
	}
}

std::vector<AdapterEntry> PCAPAdapter::GetAdapters()
{
	std::vector<AdapterEntry> nic;

#ifdef _WIN32
	if (!load_pcap())
		return nic;
#endif

	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_if_t* alldevs;
	pcap_if_t* d;

	if (pcap_findalldevs(&alldevs, errbuf) == -1)
		return nic;

	d = alldevs;
	while (d != NULL)
	{
		AdapterEntry entry;
		entry.type = Pcsx2Config::DEV9Options::NetApi::PCAP_Switched;
#ifdef _WIN32
		//guid
		if (!string_starts_with(d->name, PCAPPREFIX))
		{
			log_cb(RETRO_LOG_ERROR, "PCAP: Unexpected Device: \n", d->name);
			d = d->next;
			continue;
		}

		entry.guid = std::string(&d->name[strlen(PCAPPREFIX)]);

		IP_ADAPTER_ADDRESSES adapterInfo;
		std::unique_ptr<IP_ADAPTER_ADDRESSES[]> buffer;

		if (AdapterUtils::GetAdapter(entry.guid, &adapterInfo, &buffer))
			entry.name = WideStringToUTF8String(std::wstring(adapterInfo.FriendlyName));
		else
		{
			//have to use description
			//NPCAP 1.10 is using an version of pcap that dosn't
			//allow us to set it to use UTF8
			//see https://github.com/nmap/npcap/issues/276
			//We have to convert from ANSI to wstring, to then convert to UTF8
			const int len_desc = strlen(d->description) + 1;
			const int len_buf = MultiByteToWideChar(CP_ACP, 0, d->description, len_desc, nullptr, 0);

			std::unique_ptr<wchar_t[]> buf = std::make_unique<wchar_t[]>(len_buf);
			MultiByteToWideChar(CP_ACP, 0, d->description, len_desc, buf.get(), len_buf);

			entry.name = WideStringToUTF8String(std::wstring(buf.get()));
		}
#else
		entry.name = std::string(d->name);
		entry.guid = std::string(d->name);
#endif

		nic.push_back(entry);
		entry.type = Pcsx2Config::DEV9Options::NetApi::PCAP_Bridged;
		nic.push_back(entry);
		d = d->next;
	}

	return nic;
}

// Opens device for capture and sets non-blocking.
bool PCAPAdapter::InitPCAP(const std::string& adapter, bool promiscuous)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	log_cb(RETRO_LOG_INFO, "DEV9: Opening adapter '%s'...\n", adapter.c_str());

	// Open the adapter.
	if ((hpcap = pcap_open_live(adapter.c_str(), // Name of the device.
			 65536, // portion of the packet to capture.
			 // 65536 grants that the whole packet will be captured on all the MACs.
			 promiscuous ? 1 : 0,
			 1, // Read timeout.
			 errbuf // Error buffer.
			 )) == nullptr)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s\n", errbuf);
		log_cb(RETRO_LOG_ERROR, "DEV9: Unable to open the adapter. %s is not supported by pcap\n", adapter.c_str());
		return false;
	}

	if (pcap_setnonblock(hpcap, 1, errbuf) == -1)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Error setting non-blocking: %s\n", pcap_geterr(hpcap));
		log_cb(RETRO_LOG_ERROR, "DEV9: Continuing in blocking mode\n");
	}

	// Validate.
	const int dlt = pcap_datalink(hpcap);
	const char* dlt_name = pcap_datalink_val_to_name(dlt);

	log_cb(RETRO_LOG_ERROR, "DEV9: Device uses DLT %d: %s\n", dlt, dlt_name);
	switch (dlt)
	{
		case DLT_EN10MB:
			//case DLT_IEEE802_11:
			break;
		default:
			log_cb(RETRO_LOG_ERROR, "ERROR: Unsupported DataLink Type (%d): %s\n", dlt, dlt_name);
			pcap_close(hpcap);
			hpcap = nullptr;
			return false;
	}

	log_cb(RETRO_LOG_INFO, "DEV9: Adapter Ok.\n");
	return true;
}

bool PCAPAdapter::SetMACSwitchedFilter(MAC_Address mac)
{
	bpf_program fp;
	char filter[1024];

	snprintf(filter, sizeof(filter), "ether broadcast or ether dst %.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
		mac.bytes[0], mac.bytes[1], mac.bytes[2], mac.bytes[3], mac.bytes[4], mac.bytes[5]);

	if (pcap_compile(hpcap, &fp, filter, 1, PCAP_NETMASK_UNKNOWN) == -1)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Error calling pcap_compile: %s\n", pcap_geterr(hpcap));
		return false;
	}

	int setFilterRet;
	if ((setFilterRet = pcap_setfilter(hpcap, &fp)) == -1)
		log_cb(RETRO_LOG_ERROR, "DEV9: Error setting filter: %s\n", pcap_geterr(hpcap));

	pcap_freecode(&fp);
	return setFilterRet != -1;
}

void PCAPAdapter::SetMACBridgedRecv(NetPacket* pkt)
{
	EthernetFrameEditor frame(pkt);
	if (frame.GetProtocol() == (u16)EtherType::IPv4) // IP
	{
		// Compare DEST IP in IP with the PS2's IP, if they match, change DEST MAC to ps2MAC.
		PayloadPtr* payload = frame.GetPayload();
		IP_Packet ippkt(payload->data, payload->GetLength());
		if (ippkt.destinationIP == ps2IP)
			frame.SetDestinationMAC(ps2MAC);
	}
	if (frame.GetProtocol() == (u16)EtherType::ARP) // ARP
	{
		// Compare DEST IP in ARP with the PS2's IP, if they match, DEST MAC to ps2MAC on both ARP and ETH Packet headers.
		ARP_PacketEditor arpPkt(frame.GetPayload());
		if (*(IP_Address*)arpPkt.TargetProtocolAddress() == ps2IP)
		{
			frame.SetDestinationMAC(ps2MAC);
			*(MAC_Address*)arpPkt.TargetHardwareAddress() = ps2MAC;
		}
	}
}

void PCAPAdapter::SetMACBridgedSend(NetPacket* pkt)
{
	EthernetFrameEditor frame(pkt);
	if (frame.GetProtocol() == (u16)EtherType::IPv4) // IP
	{
		PayloadPtr* payload = frame.GetPayload();
		IP_Packet ippkt(payload->data, payload->GetLength());
		ps2IP = ippkt.sourceIP;
	}
	if (frame.GetProtocol() == (u16)EtherType::ARP) // ARP
	{
		ARP_PacketEditor arpPkt(frame.GetPayload());
		ps2IP = *(IP_Address*)arpPkt.SenderProtocolAddress();
		*(MAC_Address*)arpPkt.SenderHardwareAddress() = hostMAC;
	}
	frame.SetSourceMAC(hostMAC);
}
