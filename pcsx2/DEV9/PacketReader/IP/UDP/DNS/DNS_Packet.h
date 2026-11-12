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

#ifndef DEV9_DNS_PACKET_H
#define DEV9_DNS_PACKET_H

/* DNS messages as the internal DNS server reads and writes them, bounded
 * by the packet: a name that runs past its end, or loops through its
 * compression pointers, is malformed rather than read. */

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_PORT 53
/* A reply the server sends, and so the query it takes, fits UDP's 512. */
#define DNS_MAX_UDP 512
#define DNS_HEADER_SIZE 12
/* A name in dotted form: 253 characters and its NUL, rounded. */
#define DNS_NAME_MAX 256
#define DNS_MAX_QUESTIONS 16

enum dns_opcode
{
	DNS_OPCODE_QUERY = 0,
	DNS_OPCODE_IQUERY = 1,
	DNS_OPCODE_STATUS = 2,
	DNS_OPCODE_NOTIFY = 4,
	DNS_OPCODE_UPDATE = 5
};

enum dns_rcode
{
	DNS_RCODE_NOERROR = 0,
	DNS_RCODE_FORMERR = 1,
	DNS_RCODE_SERVFAIL = 2,
	DNS_RCODE_NXDOMAIN = 3,
	DNS_RCODE_NOTIMP = 4,
	DNS_RCODE_REFUSED = 5
};

/* Header flags: the first byte, then the second, as on the wire. */
#define DNS_F1_QR 0x80
#define DNS_F1_OPCODE(f1) (((f1) >> 3) & 0xF)
#define DNS_F1_AA 0x04
#define DNS_F1_TC 0x02
#define DNS_F1_RD 0x01
#define DNS_F2_RA 0x80
#define DNS_F2_Z 0x40
#define DNS_F2_AD 0x20
#define DNS_F2_CD 0x10
#define DNS_F2_RCODE(f2) ((f2) & 0xF)

#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1

typedef struct dns_question
{
	u16 name; /* offset of the name in the packet */
	u16 type;
	u16 klass;
} dns_question;

typedef struct dns_query
{
	u16 id;
	u8 flags1;
	u8 flags2;
	u16 qdcount;
	u16 end; /* just past the question section */
	dns_question q[DNS_MAX_QUESTIONS];
} dns_query;

/* The header and questions of pkt; false when any of it is malformed or
 * there are more than DNS_MAX_QUESTIONS. */
bool dns_parse_query(const u8* pkt, int len, dns_query* query);

/* The name at offset in dotted form into out (NUL-terminated); the
 * offset just past it in the packet, or -1 when it is malformed or does
 * not fit out_size. A compression pointer must point before the name it
 * is part of, so a chain of them always ends. */
int dns_read_name(const u8* pkt, int len, int offset, char* out, int out_size);

/* Every field of a DNS message, through log_cb, as far as it is whole. */
void dns_log(const u8* pkt, int len);

#ifdef __cplusplus
}
#endif

#endif
