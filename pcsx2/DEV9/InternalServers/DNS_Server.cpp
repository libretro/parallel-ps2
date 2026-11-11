/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include <chrono>
#include <algorithm>
#include <cctype>

#include "common/Pcsx2Defs.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#elif defined(__POSIX__)
//Note that getaddrinfo_a() exists which allows asynchronous operation
//however, that function is not standard POSIX, and is instead part of glibc
//So we will run with getaddrinfo() in a thread ourself
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
/* sockaddr_in is used below. Every platform but FreeBSD used to get this
 * transitively -- on Android it arrived via the system <ifaddrs.h> that
 * DNS_Server.h pulled in, which is now <compat/ifaddrs.h> and includes no
 * such thing. Declare the dependency here instead of relying on any of it. */
#include <netinet/in.h>
#endif

#include <retro_timers.h>
#include "DNS_Server.h"
#include "DEV9/PacketReader/IP/UDP/UDP_Packet.h"
#include "DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.h"

#include "DEV9/DEV9.h"
#include "DEV9/AdapterUtils.h"
#include <rthreads/rthreads.h>

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::UDP;
using namespace PacketReader::IP::UDP::DNS;

namespace InternalServers
{
	DNS_Server::DNS_State::DNS_State(int count, std::vector<std::string> dnsQuestions, DNS_Packet* dnsPacket, u16 port)
	{
		dns = dnsPacket;
		retro_atomic_store_release_int(&counter, count);
		questions = dnsQuestions;
		clientPort = port;

		//Prefill unordered_map, allowing use to modify it from seperate threads
		//See https://en.cppreference.com/w/cpp/container#Thread_safety
		//Different elements in the same container can be modified concurrently by different threads
		for (size_t i = 0; i < dnsQuestions.size(); i++)
			answers[dnsQuestions[i]] = {};
	}

	int DNS_Server::DNS_State::AddAnswer(const std::string& answer, IP_Address address)
	{
		answers[answer] = address;
		return retro_atomic_fetch_sub_int(&counter, 1) - 1;
	}
	int DNS_Server::DNS_State::AddNoAnswer(const std::string& answer)
	{
		return retro_atomic_fetch_sub_int(&counter, 1) - 1;
	}

	std::unordered_map<std::string, IP_Address> DNS_Server::DNS_State::GetAnswers()
	{
		return answers;
	}

	DNS_Server::DNS_Server()
	{
#ifdef _WIN32
		/* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
		WORD wVersionRequested = MAKEWORD(2, 2);

		WSADATA wsaData{0};
		const int err = WSAStartup(wVersionRequested, &wsaData);
		if (err != 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: WSAStartup failed with error: %d\n\n", err);
		else
			wsa_init = true;
#endif
	}

	//We remap 127.0.0.1 to the PC's IP address
	//We specificly use the address assigned to
	//the adapter we are using
#ifdef _WIN32
	void DNS_Server::Init(PIP_ADAPTER_ADDRESSES adapter)
#elif defined(__POSIX__)
	void DNS_Server::Init(ifaddrs* adapter)
#endif
	{
		localhostIP = {{{127, 0, 0, 1}}};

		//Find IPv4 Address
		std::optional<IP_Address> adapterIP = AdapterUtils::GetAdapterIP(adapter);
		if (adapterIP.has_value())
			localhostIP = adapterIP.value();
		else
			log_cb(RETRO_LOG_ERROR, "DEV9: Failed To Get Adapter IP\n");

		LoadHostList();
	}

	void DNS_Server::LoadHostList()
	{
		/* No host entries are configured: every name goes to the
		 * real DNS server. */
		hosts.clear();
	}

	UDP_Packet* DNS_Server::Recv()
	{
		UDP_Packet* retPay;
		if (dnsQueue.Dequeue(&retPay))
		{
			retro_atomic_dec_int(&outstandingQueries);
			return retPay;
		}
		return nullptr;
	}

	bool DNS_Server::Send(UDP_Packet* payload)
	{
		PayloadPtr* udpPayload = static_cast<PayloadPtr*>(payload->GetPayload());
		DNS_Packet dns(udpPayload->data, udpPayload->GetLength());

		if (dns.GetOpCode() == (u8)DNS_OPCode::Query && dns.questions.size() > 0 && dns.GetQR() == false)
		{
			std::vector<std::string> reqs;

			for (size_t i = 0; i < dns.questions.size(); i++)
			{
				DNS_QuestionEntry q = dns.questions[i];
				if (q.entryType == 1 && q.entryClass == 1)
					reqs.push_back(q.name);
				else
					log_cb(RETRO_LOG_ERROR, "DEV9: Unexpected question type of class, T: %d C: %d\n", q.entryType, q.entryClass);
			}
			if (reqs.size() == 0)
				return true;
			if (dns.GetTC() == true)
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: Truncated DNS packet Not Supported\n");
				return true;
			}

			DNS_Packet* ret = new DNS_Packet();
			ret->id = dns.id; //TODO, drop duplicate requests based on ID
			ret->SetQR(true);
			ret->SetOpCode((u8)DNS_OPCode::Query);
			ret->SetAA(false);
			ret->SetTC(false);
			ret->SetRD(true);
			ret->SetRA(true);
			ret->SetAD(false);
			ret->SetCD(false);
			ret->SetRCode((u8)DNS_RCode::NoError);
			//Counts
			ret->questions = dns.questions;

			DNS_State* state = new DNS_State(reqs.size(), reqs, ret, payload->sourcePort);
			retro_atomic_inc_int(&outstandingQueries);

			for (size_t i = 0; i < reqs.size(); i++)
			{
				if (CheckHostList(reqs[i], state))
					continue;
				GetHost(reqs[i], state);
			}
			return true;
		}
		else
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: Unexpected DNS OPCode, Code: %u\n", (unsigned)dns.GetOpCode());
			return true;
		}
	}

	bool DNS_Server::CheckHostList(std::string url, DNS_State* state)
	{
		std::transform(url.begin(), url.end(), url.begin(),
			[](unsigned char c) { return tolower(c); });

		auto f = hosts.find(url);
		if (f != hosts.end())
		{
			const int remaining = state->AddAnswer(url, hosts[url]);
			log_cb(RETRO_LOG_INFO, "DEV9: DNS: %s found in hosts\n", url.c_str());
			if (remaining == 0)
				FinaliseDNS(state);
			return true;
		}
		return false;
	}

	void DNS_Server::FinaliseDNS(DNS_State* state)
	{
		DNS_Packet* retPay = state->dns;
		std::vector<std::string> reqs = state->questions;
		std::unordered_map<std::string, IP_Address> answers = state->GetAnswers();

		for (size_t i = 0; i < reqs.size(); i++)
		{
			IP_Address ans = answers[reqs[i]];
			if (ans.integer != 0)
			{
				//TODO, might not be effective on pcap
				const IP_Address local{{{127, 0, 0, 1}}};
				if (ans == local)
					ans = localhostIP;

				std::vector<u8> ansVector;
				ansVector.resize(4);
				*(IP_Address*)&ansVector[0] = ans;
				DNS_ResponseEntry ansEntry(reqs[i], 1, 1, ansVector, 10800);
				retPay->answers.push_back(ansEntry);
			}
			else
				retPay->SetRCode(2); //ServerFailure
		}

		const u16 clientPort = state->clientPort;
		delete state;

		if (retPay->GetLength() > 512)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: Generated DNS response too large, dropping\n");
			delete retPay;
			retro_atomic_dec_int(&outstandingQueries);
			return;
		}

		UDP_Packet* retUdp = new UDP_Packet(retPay);
		retUdp->sourcePort = 53;
		retUdp->destinationPort = clientPort;
		dnsQueue.Enqueue(retUdp);
	}

	DNS_Server::~DNS_Server()
	{
		//Block untill DNS finished &
		//Delete entries in queue
		while (retro_atomic_load_acquire_int(&outstandingQueries) != 0)
		{
			UDP_Packet* retPay = nullptr;
			if (!dnsQueue.Dequeue(&retPay))
			{
				using namespace std::chrono_literals;
				retro_sleep(10);
				continue;
			}

			delete retPay;
			retro_atomic_dec_int(&outstandingQueries);
		}

#ifdef _WIN32
		if (wsa_init)
			WSACleanup();
#endif
	}

#ifdef _WIN32
	void DNS_Server::GetHost(std::string url, DNS_State* state)
	{
		//Need to convert to UTF16
		const int size = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
		std::vector<wchar_t> converted_string(size);
		MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, converted_string.data(), converted_string.size());

		ADDRINFOEXW hints{0};
		hints.ai_family = AF_INET;

		GetAddrInfoExCallbackData* data = new GetAddrInfoExCallbackData();
		data->state = state;
		data->session = this;
		data->url = url;

		int ret = GetAddrInfoExW(converted_string.data(), nullptr, NS_ALL, 0, &hints, (ADDRINFOEXW**)&data->result, nullptr, &data->overlapped, (LPLOOKUPSERVICE_COMPLETION_ROUTINE)&DNS_Server::GetAddrInfoExCallback, &data->cancelHandle);

		if (ret == WSA_IO_PENDING)
			return;
		else
			GetAddrInfoExCallback(ret, -1, &data->overlapped);
	}

	void __stdcall DNS_Server::GetAddrInfoExCallback(DWORD dwError, DWORD dwBytes, OVERLAPPED* lpOverlapped)
	{
		GetAddrInfoExCallbackData* data = reinterpret_cast<GetAddrInfoExCallbackData*>(lpOverlapped);

		int remaining = -1;
		switch (dwError)
		{
			case NO_ERROR:
			{
				ADDRINFOEXW* addrInfo = (ADDRINFOEXW*)data->result;
				while (addrInfo != nullptr && addrInfo->ai_family != AF_INET)
					addrInfo = addrInfo->ai_next;

				if (addrInfo == nullptr)
				{
					log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s\n", data->url.c_str());
					log_cb(RETRO_LOG_ERROR, "DEV9: with unexpected error code %d\n", -1);
					remaining = data->state->AddNoAnswer(data->url);
					break;
				}

				sockaddr_in* sockaddr = (sockaddr_in*)addrInfo->ai_addr;
				remaining = data->state->AddAnswer(data->url, *(IP_Address*)&sockaddr->sin_addr);
				break;
			}
			case WSAHOST_NOT_FOUND:
			case WSATRY_AGAIN: //Nonauthoritative host not found
				log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s\n", data->url.c_str());
				remaining = data->state->AddNoAnswer(data->url);
				break;
			default:
				log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s\n", data->url.c_str());
				log_cb(RETRO_LOG_ERROR, "DEV9: with unexpected error code %d\n", dwError);
				remaining = data->state->AddNoAnswer(data->url);
				break;
		}

		if (remaining == 0)
			data->session->FinaliseDNS(data->state);

		//cleanup
		if (data->result != nullptr)
			FreeAddrInfoExW((ADDRINFOEXW*)data->result);
		delete data;
	}
#elif defined(__POSIX__)
	void DNS_Server::GetHostThreadEntry(void* arg)
	{
		GetHostCtx* ctx = static_cast<GetHostCtx*>(arg);
		ctx->self->GetAddrInfoThread(ctx->url, ctx->state);
		delete ctx;
	}

	void DNS_Server::GetHost(std::string url, DNS_State* state)
	{
		/* The thread takes its arguments through a heap context it frees
		 * itself, and is detached: completion is signalled through state,
		 * not by joining. */
		GetHostCtx* ctx = new GetHostCtx{this, std::move(url), state};
		sthread_t* t = sthread_create(GetHostThreadEntry, ctx);
		if (t)
			sthread_detach(t);
		else
		{
			/* No thread: resolve on this one so the request still completes. */
			GetAddrInfoThread(ctx->url, ctx->state);
			delete ctx;
		}
	}

	void DNS_Server::GetAddrInfoThread(std::string url, DNS_State* state)
	{
		addrinfo hints{0};
		hints.ai_family = AF_INET;
		addrinfo* result = nullptr;

		int error = getaddrinfo(url.c_str(), nullptr, &hints, &result);
		int remaining = -1;
		switch (error)
		{
			case 0:
			{
				addrinfo* retInfo = result;
				while (retInfo != nullptr && retInfo->ai_family != AF_INET)
					retInfo = retInfo->ai_next;

				if (retInfo == nullptr)
				{
					log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s\n", url.c_str());
					log_cb(RETRO_LOG_ERROR, "DEV9: with unexpected error code %d\n", -1);
					remaining = state->AddNoAnswer(url);
					break;
				}

				sockaddr_in* sockaddr = (sockaddr_in*)retInfo->ai_addr;
				remaining = state->AddAnswer(url, *(IP_Address*)&sockaddr->sin_addr);
				break;
			}
			case EAI_NONAME:
			case EAI_AGAIN: //Nonauthoritative host not found
				log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s\n", url.c_str());
				remaining = state->AddNoAnswer(url);
				break;
			default:
				log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s\n", url.c_str());
				log_cb(RETRO_LOG_ERROR, "DEV9: with unexpected error code %d\n", error);
				remaining = state->AddNoAnswer(url);
				break;
		}

		if (remaining == 0)
			FinaliseDNS(state);

		//cleanup
		if (result != nullptr)
			freeaddrinfo(result);
	}
#endif
} // namespace InternalServers
