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

#include <stdio.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "DNS_Packet.h"

static u16 rd16(const u8* p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static u32 rd32(const u8* p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int dns_read_name(const u8* pkt, int len, int offset, char* out, int out_size)
{
	int pos = offset;
	int seg = offset; /* where this run of labels began */
	int next = -1;    /* past the name in the packet, once known */
	int n = 0;

	if (out_size < 1)
		return -1;
	for (;;)
	{
		int c;
		if (pos < 0 || pos >= len)
			return -1;
		c = pkt[pos];
		if (c == 0)
		{
			if (next < 0)
				next = pos + 1;
			break;
		}
		if ((c & 0xC0) == 0xC0)
		{
			int target;
			if (pos + 1 >= len)
				return -1;
			target = ((c & 0x3F) << 8) | pkt[pos + 1];
			if (next < 0)
				next = pos + 2;
			/* Strictly back from where this run began: no loops. */
			if (target >= seg)
				return -1;
			pos = seg = target;
			continue;
		}
		if (c & 0xC0)
			return -1; /* the reserved label types */
		if (pos + 1 + c > len || n + (n ? 1 : 0) + c + 1 > out_size)
			return -1;
		if (memchr(pkt + pos + 1, 0, (size_t)c))
			return -1;
		if (n)
			out[n++] = '.';
		memcpy(out + n, pkt + pos + 1, (size_t)c);
		n += c;
		pos += 1 + c;
	}
	out[n] = '\0';
	return next;
}

bool dns_parse_query(const u8* pkt, int len, dns_query* query)
{
	char name[DNS_NAME_MAX];
	int pos = DNS_HEADER_SIZE;
	int i;

	if (len < DNS_HEADER_SIZE)
		return false;
	query->id = rd16(pkt);
	query->flags1 = pkt[2];
	query->flags2 = pkt[3];
	query->qdcount = rd16(pkt + 4);
	if (query->qdcount > DNS_MAX_QUESTIONS)
		return false;
	for (i = 0; i < query->qdcount; i++)
	{
		const int next = dns_read_name(pkt, len, pos, name, sizeof(name));
		if (next < 0 || next + 4 > len)
			return false;
		query->q[i].name = (u16)pos;
		query->q[i].type = rd16(pkt + next);
		query->q[i].klass = rd16(pkt + next + 2);
		pos = next + 4;
	}
	query->end = (u16)pos;
	return true;
}

static const char* opcode_name(int op)
{
	switch (op)
	{
		case DNS_OPCODE_QUERY: return "Query";
		case DNS_OPCODE_IQUERY: return "IQuery";
		case DNS_OPCODE_STATUS: return "Status";
		case 3: return "Reserved";
		case DNS_OPCODE_NOTIFY: return "Notify";
		case DNS_OPCODE_UPDATE: return "Update";
		default: return "Unknown";
	}
}

static const char* rcode_name(int rc)
{
	static const char* const names[] = {"NoError", "FormatError", "ServerFailure", "NameError",
		"NotImplemented", "Refused", "YXDomain", "YXRRSet", "NXRRSet", "NotAuth", "NotZone"};
	return rc < (int)(sizeof(names) / sizeof(names[0])) ? names[rc] : "Unknown";
}

/* One section's records, from *pos; false once one is not whole. */
static bool log_records(const u8* pkt, int len, int* pos, const char* tag, int count, bool question)
{
	char name[DNS_NAME_MAX];
	char data[200];
	int i;
	for (i = 0; i < count; i++)
	{
		int next = dns_read_name(pkt, len, *pos, name, sizeof(name));
		if (next < 0 || next + 4 > len)
			return false;
		log_cb(RETRO_LOG_INFO, "DEV9: DNS: %s%d Name %s\n", tag, i, name);
		log_cb(RETRO_LOG_INFO, "DEV9: DNS: %s%d Type %u\n", tag, i, (unsigned)rd16(pkt + next));
		log_cb(RETRO_LOG_INFO, "DEV9: DNS: %s%d Class %u\n", tag, i, (unsigned)rd16(pkt + next + 2));
		next += 4;
		if (!question)
		{
			int rdlen, j, n = 0;
			if (next + 6 > len)
				return false;
			rdlen = rd16(pkt + next + 4);
			if (next + 6 + rdlen > len)
				return false;
			log_cb(RETRO_LOG_INFO, "DEV9: DNS: %s%d TTL %u\n", tag, i, (unsigned)rd32(pkt + next));
			data[0] = '\0';
			/* As many bytes as the line holds, then an ellipsis. */
			for (j = 0; j < rdlen; j++)
			{
				if (n > (int)sizeof(data) - 8)
				{
					strcpy(data + n, "...");
					break;
				}
				n += sprintf(data + n, j ? ":%u" : "%u", (unsigned)pkt[next + 6 + j]);
			}
			log_cb(RETRO_LOG_INFO, "DEV9: DNS: %s%d Data %s\n", tag, i, data);
			next += 6 + rdlen;
		}
		*pos = next;
	}
	return true;
}

void dns_log(const u8* pkt, int len)
{
	int qd, an, ns, ar, pos = DNS_HEADER_SIZE;
	u8 f1, f2;
	if (len < DNS_HEADER_SIZE)
	{
		log_cb(RETRO_LOG_INFO, "DEV9: DNS: Truncated (%d bytes)\n", len);
		return;
	}
	f1 = pkt[2];
	f2 = pkt[3];
	qd = rd16(pkt + 4);
	an = rd16(pkt + 6);
	ns = rd16(pkt + 8);
	ar = rd16(pkt + 10);
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: ID %u\n", (unsigned)rd16(pkt));
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Is Response? %s\n", (f1 & DNS_F1_QR) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: OpCode %s (%d)\n", opcode_name(DNS_F1_OPCODE(f1)), DNS_F1_OPCODE(f1));
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Is Authoritative (not cached)? %s\n", (f1 & DNS_F1_AA) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Is Truncated? %s\n", (f1 & DNS_F1_TC) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Recursion Desired? %s\n", (f1 & DNS_F1_RD) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Recursion Available? %s\n", (f2 & DNS_F2_RA) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Zero %d\n", (f2 & DNS_F2_Z) ? 1 : 0);
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Authenticated Data? %s\n", (f2 & DNS_F2_AD) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Checking Disabled? %s\n", (f2 & DNS_F2_CD) ? "True" : "False");
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Result %s (%d)\n", rcode_name(DNS_F2_RCODE(f2)), DNS_F2_RCODE(f2));
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Question Count %d\n", qd);
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Answer Count %d\n", an);
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Authority Count %d\n", ns);
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Additional Count %d\n", ar);
	if (log_records(pkt, len, &pos, "Q", qd, true) &&
		log_records(pkt, len, &pos, "Ans", an, false) &&
		log_records(pkt, len, &pos, "Auth", ns, false) &&
		log_records(pkt, len, &pos, "Add", ar, false))
		return;
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Malformed past offset %d\n", pos);
}
