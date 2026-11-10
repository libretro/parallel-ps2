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

#pragma once

#include "common/Pcsx2Defs.h"
#include <retro_atomic.h>
#include <chrono>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#elif defined(__POSIX__)
#define INVALID_SOCKET -1
#endif

#include "DEV9/SimpleQueue.h"
#include "DEV9/Sessions/BaseSession.h"
#include "DEV9/PacketReader/IP/TCP/TCP_Packet.h"

namespace Sessions
{
	class TCP_Session : public BaseSession
	{
	private:
		enum struct TCP_State
		{
			None,
			SendingSYN_ACK,
			SentSYN_ACK,
			Connected,
			Closing_ClosedByPS2,
			Closing_ClosedByPS2ThenRemote_WaitingForAck,
			Closing_ClosedByRemote,
			Closing_ClosedByRemoteThenPS2_WaitingForAck,
			CloseCompletedFlushBuffer, //Packets in recvBuff to send
			CloseCompleted,
		};
		enum struct NumCheckResult
		{
			OK,
			GotOldData,
			Bad
		};

		SimpleQueue<PacketReader::IP::TCP::TCP_Packet*> _recvBuff;

#ifdef _WIN32
		SOCKET client = INVALID_SOCKET;
#elif defined(__POSIX__)
		int client = INVALID_SOCKET;
#endif

		/* The state and the next sequence number the PS2 is to send are
		 * set on the EE thread (Send) and read on the receive thread (Recv),
		 * and Recv sets the state too: atomics, as windowSize is. */
		retro_atomic_int_t stateWord = RETRO_ATOMIC_INT_INITIALIZER((int)TCP_State::None);
		TCP_State GetState() { return (TCP_State)retro_atomic_load_acquire_int(&stateWord); }
		void SetState(TCP_State s) { retro_atomic_store_release_int(&stateWord, (int)s); }

		u16 srcPort = 0;
		u16 destPort = 0;

		u16 maxSegmentSize = 1460; //Accesed By Both In and Out Threads, but set only on Connect Thread
		int windowScale = 0;
		retro_atomic_int_t windowSize = RETRO_ATOMIC_INT_INITIALIZER(1460);

		u32 lastRecivedTimeStamp; //Accesed By Both In and Out Threads
		std::chrono::steady_clock::time_point timeStampStart; //Set By In on connect, read by In and Out Threads, unsure as to correct C++ type
		bool sendTimeStamps = false; //Accesed By Out Thread Only

		const int receivedPS2SeqNumberCount = 5;
		retro_atomic_int_t expectedSeqWord = RETRO_ATOMIC_INT_INITIALIZER(0); /* set by Out, read by In too */
		u32 GetExpectedSeq() { return (u32)retro_atomic_load_acquire_int(&expectedSeqWord); }
		void SetExpectedSeq(u32 v) { retro_atomic_store_release_int(&expectedSeqWord, (int)v); }
		std::vector<u32> receivedPS2SeqNumbers; //Accesed By Out Thread Only

		const int oldMyNumCount = 2;
		u32 _MySequenceNumber = 1;
		std::vector<u32> _OldMyNumbers;
		retro_atomic_int_t myNumberACKed = RETRO_ATOMIC_INT_INITIALIZER(1);

	public:
		TCP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP);

		virtual PacketReader::IP::IP_Payload* Recv();
		virtual bool Send(PacketReader::IP::IP_Payload* payload);
		virtual void Reset();
		virtual bool PollInterest(uintptr_t* fd, int* events);

		virtual ~TCP_Session();

	private:
		//Async stuff

		void PushRecvBuff(PacketReader::IP::TCP::TCP_Packet* tcp);
		PacketReader::IP::TCP::TCP_Packet* PopRecvBuff();

		void IncrementMyNumber(u32 amount);
		u32 GetMyNumber();
		std::tuple<u32, std::vector<u32>> GetAllMyNumbers();
		void ResetMyNumbers();

		NumCheckResult CheckRepeatSYNNumbers(PacketReader::IP::TCP::TCP_Packet* tcp);
		NumCheckResult CheckNumbers(PacketReader::IP::TCP::TCP_Packet* tcp);
		u32 GetDelta(u32 parExpectedSeq, u32 parGotSeq);
		//Returns true if errored
		bool ErrorOnNonEmptyPacket(PacketReader::IP::TCP::TCP_Packet* tcp);

		//PS2 sent SYN
		PacketReader::IP::TCP::TCP_Packet* ConnectTCPComplete(bool success);
		bool SendConnect(PacketReader::IP::TCP::TCP_Packet* tcp);
		bool SendConnected(PacketReader::IP::TCP::TCP_Packet* tcp);

		bool SendData(PacketReader::IP::TCP::TCP_Packet* tcp);
		bool SendNoData(PacketReader::IP::TCP::TCP_Packet* tcp);

		//On Close by PS2
		//S1: PS2 Sends FIN+ACK
		//S2: CloseByPS2Stage1_2 sends ACK, state set to Closing_ClosedByPS2
		//S3: When server closes socket, we send FIN in CloseByPS2Stage3
		//and set state to Closing_ClosedByPS2ThenRemote_WaitingForAck
		//S4: PS2 then Sends ACK

		bool CloseByPS2Stage1_2(PacketReader::IP::TCP::TCP_Packet* tcp);
		PacketReader::IP::TCP::TCP_Packet* CloseByPS2Stage3();
		bool CloseByPS2Stage4(PacketReader::IP::TCP::TCP_Packet* tcp);

		//On Close By Server
		//S1: CloseByRemoteStage1 sends FIN+ACK, state set to Closing_ClosedByRemote
		//S2: PS2 Will then sends ACK, this is only checked after stage4
		//S3: PS2 Will send FIN, possible in the previous ACK packet
		//S4: CloseByRemoteStage3_4 sends ACK, state set to
		//Closing_ClosedByRemoteThenPS2_WaitingForAck
		//We Then Check if S3 has been compleated

		PacketReader::IP::TCP::TCP_Packet* CloseByRemoteStage1();
		bool CloseByRemoteStage2_ButAfter4(PacketReader::IP::TCP::TCP_Packet* tcp);
		bool CloseByRemoteStage3_4(PacketReader::IP::TCP::TCP_Packet* tcp);

		//Error on sending data
		void CloseByRemoteRST();

		//Returned TCP_Packet Takes ownership of data
		PacketReader::IP::TCP::TCP_Packet* CreateBasePacket(PacketReader::PayloadData* data = nullptr);

		void CloseSocket();
	};
} // namespace Sessions
