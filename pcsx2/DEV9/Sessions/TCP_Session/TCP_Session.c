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

#include <string.h>

#include "common/Pcsx2Defs.h"
#ifdef _WIN32
#include <winsock2.h>
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#else
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include <features/features_cpu.h>

#include "DEV9/Sessions/Session.h"

/* The PS2's side of a TCP connection, terminated here: its segments are
 * acknowledged as they come and their data written to a host socket,
 * and what the host sends back is cut into segments for it. */

enum
{
	TCP_NONE,
	TCP_SENDING_SYN_ACK,
	TCP_SENT_SYN_ACK,
	TCP_CONNECTED,
	TCP_CLOSED_BY_PS2,
	TCP_CLOSED_BY_PS2_THEN_REMOTE_WAIT_ACK,
	TCP_CLOSED_BY_REMOTE,
	TCP_CLOSED_BY_REMOTE_THEN_PS2_WAIT_ACK,
	TCP_CLOSE_COMPLETED_FLUSH, /* segments still to send */
	TCP_CLOSE_COMPLETED
};

enum
{
	NUM_OK,
	NUM_OLD_DATA,
	NUM_BAD
};

/* The segment sizes the PS2 may ask for: one that leaves no room for
 * data, or none for its timestamps, is not taken. */
#define TCP_MSS_MIN 64
#define TCP_MSS_MAX 1460
#define TCP_TS_OPTIONS 12

static s64 now_usec(void)
{
	return (s64)cpu_features_get_time_usec();
}

static u32 ts_now(const tcp_state* t)
{
	return (u32)((now_usec() - t->ts_start) / 1000000);
}

static bool u32_in(const u32* v, int n, u32 x)
{
	int i;
	for (i = 0; i < n; i++)
		if (v[i] == x)
			return true;
	return false;
}

static void increment_my_number(tcp_state* t, u32 amount)
{
	t->my_old[0] = t->my_old[1];
	t->my_old[1] = t->my_seq;
	t->my_seq += amount;
}

static void reset_my_numbers(tcp_state* t)
{
	t->my_seq = 1;
	t->my_old[0] = t->my_old[1] = 1;
}

static void push_ps2_seq(tcp_state* t, u32 seq)
{
	memmove(t->ps2_seqs, t->ps2_seqs + 1, sizeof(t->ps2_seqs) - sizeof(t->ps2_seqs[0]));
	t->ps2_seqs[4] = seq;
}

/* A control segment for the PS2, as the numbers are now. */
static void push_segment(tcp_state* t, u8 flags)
{
	tcp_segment* seg;
	if (t->pending_count == TCP_PENDING)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: %d segments waiting, dropping one\n", TCP_PENDING);
		return;
	}
	seg = &t->pending[(t->pending_head + t->pending_count) % TCP_PENDING];
	seg->flags = flags;
	seg->seq = t->my_seq;
	seg->ack = t->expected_seq;
	t->pending_count++;
}

/* The timestamps option where the PS2 asked for them, at opt. */
static int put_ts(const tcp_state* t, u8* opt)
{
	if (!t->send_ts)
		return 0;
	opt[0] = 1;
	opt[1] = 1;
	opt[2] = 8;
	opt[3] = 10;
	net_wr32(opt + 4, ts_now(t));
	net_wr32(opt + 8, t->last_ts);
	return TCP_TS_OPTIONS;
}

/* A segment of len bytes of data already at frame + NET_TCP_DATA(the
 * options it has): its headers, and the length past NET_IP_DATA. */
static int finish(net_session* s, u8* frame, u32 seq, u32 ack, u8 flags, int optlen, int len)
{
	const tcp_state* t = &s->u.tcp;
	net_put_tcp(frame, s->remote_ip, s->ps2_ip, t->dst_port, t->src_port, seq, ack, flags,
		(u16)(2 * t->mss), optlen, len);
	return TCP_HEADER + optlen + len;
}

static int send_segment(net_session* s, u8* frame, const tcp_segment* seg)
{
	const int optlen = put_ts(&s->u.tcp, frame + NET_TCP_DATA(0));
	return finish(s, frame, seg->seq, seg->ack, seg->flags, optlen, 0);
}

static void close_socket(tcp_state* t)
{
	net_sock_close(t->sock);
	t->sock = NET_SOCK_BAD;
}

/* An error: RST to the PS2, and done once it is sent. */
static void close_by_remote_rst(net_session* s)
{
	tcp_state* t = &s->u.tcp;
	push_segment(t, TCP_RST);
	close_socket(t);
	t->state = TCP_CLOSE_COMPLETED_FLUSH;
}

static void complete(net_session* s)
{
	s->u.tcp.state = TCP_CLOSE_COMPLETED;
	session_closed(s);
}

static void log_socket_error(const tcp_state* t)
{
	int error = 0;
#ifdef _WIN32
	int len = sizeof(error);
#else
	socklen_t len = sizeof(error);
#endif
	if (getsockopt(t->sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Unkown TCP Connection Error (getsockopt Error: %d)\n", net_sock_error());
	else
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Send Error: %d\n", error);
}

/* ---- From the host -------------------------------------------------- */

static bool tcp_poll_interest(net_session* s, uintptr_t* fd, int* events)
{
	const tcp_state* t = &s->u.tcp;
	*events = 0;
	if (t->pending_count)
		return false;
	switch (t->state)
	{
		case TCP_SENDING_SYN_ACK:
			*fd = (uintptr_t)t->sock;
			*events = SESSION_POLL_WRITE;
			return true;
		case TCP_CLOSE_COMPLETED_FLUSH:
			return false;
		case TCP_CONNECTED:
		case TCP_CLOSED_BY_PS2:
			if (t->window != 0 && t->my_acked)
			{
				*fd = (uintptr_t)t->sock;
				*events = SESSION_POLL_READ;
			}
			return true;
		default:
			return true;
	}
}

/* The host's socket connected, or did not. */
static int connect_complete(net_session* s, u8* frame, bool success)
{
	tcp_state* t = &s->u.tcp;
	u8* opt = frame + NET_TCP_DATA(0);
	u32 seq;
	if (!success)
	{
		log_socket_error(t);
		complete(s);
		return 0;
	}
	t->state = TCP_SENT_SYN_ACK;
	seq = t->my_seq;
	increment_my_number(t, 1);
	/* MSS, NOP, window scale 0, then the timestamps */
	opt[0] = 2;
	opt[1] = 4;
	net_wr16(opt + 2, (u32)t->mss);
	opt[4] = 1;
	opt[5] = 3;
	opt[6] = 3;
	opt[7] = 0;
	return finish(s, frame, seq, t->expected_seq, TCP_SYN | TCP_ACK, 8 + put_ts(t, opt + 8), 0);
}

/* The host closed its side: FIN to the PS2. */
static int remote_fin(net_session* s, u8* frame, int next_state)
{
	tcp_state* t = &s->u.tcp;
	tcp_segment seg;
	seg.flags = TCP_FIN | TCP_ACK;
	seg.seq = t->my_seq;
	seg.ack = t->expected_seq;
	increment_my_number(t, 1);
	t->my_acked = false;
	t->state = next_state;
	return send_segment(s, frame, &seg);
}

static int tcp_recv(net_session* s, u8* frame)
{
	tcp_state* t = &s->u.tcp;
	int optlen, max, n;
	u32 seq;

	if (t->pending_count)
	{
		const tcp_segment seg = t->pending[t->pending_head];
		t->pending_head = (t->pending_head + 1) % TCP_PENDING;
		t->pending_count--;
		return send_segment(s, frame, &seg);
	}

	switch (t->state)
	{
		case TCP_SENDING_SYN_ACK:
		{
			const int ready = net_sock_ready(t->sock, SESSION_POLL_WRITE);
			if (ready & NET_SOCK_FAILED)
				return connect_complete(s, frame, false);
			if (ready & SESSION_POLL_WRITE)
				return connect_complete(s, frame, true);
			return 0;
		}
		case TCP_CLOSE_COMPLETED_FLUSH:
			/* Closed by the host, the last segment being ours */
			complete(s);
			return 0;
		case TCP_CONNECTED:
		case TCP_CLOSED_BY_PS2:
			break;
		default:
			return 0;
	}

	optlen = t->send_ts ? TCP_TS_OPTIONS : 0;
	max = t->mss - optlen;
	if (max > t->window)
		max = t->window;
	if (max > ETH_FRAME_MAX - NET_TCP_DATA(optlen))
		max = ETH_FRAME_MAX - NET_TCP_DATA(optlen);
	if (max <= 0 || !t->my_acked)
		return 0;

	/* Straight from the socket into the segment. */
	n = recv(t->sock, (char*)frame + NET_TCP_DATA(optlen), max, 0);
	if (n < 0)
	{
		const int err = net_sock_error();
		if (net_sock_would_block(err))
			return 0;
#ifdef _WIN32
		if (err == WSAEINVAL || err == WSAESHUTDOWN)
			return 0;
#else
		if (err == EINVAL || err == ESHUTDOWN)
			return 0; /* the PS2 reset it as this ran */
#endif
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Recv Error: %d\n", err);
		return 0;
	}
	if (n == 0)
	{
		if (shutdown(t->sock, SHUT_RD) != 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Shutdown SD_RECEIVE Error: %d\n", net_sock_error());
		if (t->state == TCP_CONNECTED)
			return remote_fin(s, frame, TCP_CLOSED_BY_REMOTE);
		return remote_fin(s, frame, TCP_CLOSED_BY_PS2_THEN_REMOTE_WAIT_ACK);
	}
	seq = t->my_seq;
	increment_my_number(t, (u32)n);
	t->my_acked = false;
	put_ts(t, frame + NET_TCP_DATA(0));
	return finish(s, frame, seq, t->expected_seq, TCP_ACK | TCP_PSH, optlen, n);
}

/* ---- From the PS2 --------------------------------------------------- */

static int check_repeat_syn(const tcp_state* t, const net_tcp* tcp)
{
	if (tcp->seq != t->expected_seq - 1)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] Sent Unexpected Sequence Number From Repeated SYN Packet, Got %u Expected %u\n",
			tcp->seq, t->expected_seq - 1);
		return NUM_BAD;
	}
	return NUM_OK;
}

static int check_numbers(tcp_state* t, const net_tcp* tcp)
{
	if (tcp->ack != t->my_seq)
	{
		if (!u32_in(t->my_old, 2, tcp->ack))
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] Sent Unexpected Acknowledgement Number, did not Match Old Numbers, Got %u Expected %u\n",
				tcp->ack, t->my_seq);
			return NUM_BAD;
		}
	}
	else
		t->my_acked = true;

	if (tcp->seq != t->expected_seq)
	{
		if (tcp->len == 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] Sent Unexpected Sequence Number From ACK Packet, Got %u Expected %u\n",
				tcp->seq, t->expected_seq);
		else if (!u32_in(t->ps2_seqs, 5, tcp->seq))
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] Sent an Old Seq Number on an Data packet, Got %u Expected %u\n",
				tcp->seq, t->expected_seq);
			return NUM_OLD_DATA;
		}
		else
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] Sent Unexpected Sequence Number From Data Packet, Got %u Expected %u\n",
				tcp->seq, t->expected_seq);
			return NUM_BAD;
		}
	}
	return NUM_OK;
}

/* How far into a segment at got the PS2's next byte is. */
static u32 seq_delta(u32 expected, u32 got)
{
	u32 delta = expected - got;
	if (delta > 0x7FFFFFFFu)
	{
		delta = 0xFFFFFFFFu - expected + got;
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] SequenceNumber Overflow Detected\n");
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: [PS2] New Data Offset: %u bytes\n", delta);
	}
	return delta;
}

static void take_options(tcp_state* t, const net_tcp* tcp)
{
	if (tcp->has_ts)
		t->last_ts = tcp->ts_val;
	if (tcp->unknown >= 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Got Unknown Option %d\n", tcp->unknown);
}

/* True when the segment was an error, and the connection reset. */
static bool error_on_non_empty(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	const int result = check_numbers(t, tcp);
	if (result == NUM_OLD_DATA)
		return false;
	if (result == NUM_BAD)
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Bad TCP Numbers Received\n");
		return true;
	}
	if (tcp->len > 0)
	{
		if (seq_delta(t->expected_seq, tcp->seq) == 0)
			return false;
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Invalid Packet, Packet Has Data\n");
		return true;
	}
	return false;
}

static bool send_connect(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	static const u8 any[4] = {0, 0, 0, 0};
	struct sockaddr_in sa;
	const int on = 1;
	int i, err;

	t->dst_port = tcp->dport;
	t->src_port = tcp->sport;
	if (!(tcp->flags & TCP_SYN))
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Attempt To Send Data On Non Connected Connection\n");
		return true;
	}
	t->expected_seq = tcp->seq + 1;
	for (i = 0; i < 5; i++)
		t->ps2_seqs[i] = tcp->seq;
	reset_my_numbers(t);

	if (tcp->mss >= 0)
		t->mss = tcp->mss < TCP_MSS_MIN ? TCP_MSS_MIN : tcp->mss > TCP_MSS_MAX ? TCP_MSS_MAX : tcp->mss;
	if (tcp->wscale >= 0)
	{
		/* RFC 7323: at most 14 */
		t->wscale = tcp->wscale > 14 ? 14 : tcp->wscale;
		if (t->wscale > 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Non-Zero WindowScale Option\n");
	}
	if (tcp->has_ts)
	{
		t->last_ts = tcp->ts_val;
		t->send_ts = true;
		t->ts_start = now_usec();
	}
	if (tcp->unknown >= 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Got Unknown Option %d\n", tcp->unknown);
	t->window = (int)tcp->window << t->wscale;

	close_socket(t);
	t->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (t->sock == NET_SOCK_BAD)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Failed to open socket. Error: %d\n", net_sock_error());
		session_closed(s);
		return false;
	}
	if (memcmp(s->adapter_ip, any, 4) != 0)
	{
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		memcpy(&sa.sin_addr, s->adapter_ip, 4);
		if (bind(t->sock, (const struct sockaddr*)&sa, sizeof(sa)) != 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Failed to bind socket. Error: %d\n", net_sock_error());
	}
	if (!net_sock_nonblocking(t->sock))
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Failed to set non blocking. Error: %d\n", net_sock_error());
	if (setsockopt(t->sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on)) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Failed to set TCP_NODELAY. Error: %d\n", net_sock_error());

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	memcpy(&sa.sin_addr, s->remote_ip, 4);
	sa.sin_port = htons(t->dst_port);
	if (connect(t->sock, (const struct sockaddr*)&sa, sizeof(sa)) != 0)
	{
		err = net_sock_error();
#ifdef _WIN32
		if (err != WSAEWOULDBLOCK)
#else
		if (!net_sock_would_block(err) && err != EINPROGRESS)
#endif
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Failed to connect socket. Error: %d\n", err);
			session_closed(s);
			return false;
		}
		/* Its completion is seen in recv */
	}
	t->state = TCP_SENDING_SYN_ACK;
	return true;
}

/* The PS2's ACK of our SYN-ACK */
static bool send_connected(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	if (tcp->flags & TCP_SYN)
	{
		if (check_repeat_syn(t, tcp) == NUM_BAD)
		{
			close_by_remote_rst(s);
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Invalid Repeated SYN (SentSYN_ACK)\n");
		}
		return true; /* a repeat, while we still connect */
	}
	if (check_numbers(t, tcp) == NUM_BAD)
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Bad TCP Numbers Received\n");
		return true;
	}
	take_options(t, tcp);
	t->state = TCP_CONNECTED;
	return true;
}

static bool send_data(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	int result;
	u32 delta;

	if (tcp->flags & TCP_SYN)
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Attempt to Connect to an open Port\n");
		return true;
	}
	if (tcp->flags & TCP_URG)
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Urgent Data Not Supported\n");
		return true;
	}
	take_options(t, tcp);
	t->window = (int)tcp->window << t->wscale;

	result = check_numbers(t, tcp);
	delta = seq_delta(t->expected_seq, tcp->seq);
	if (result == NUM_BAD)
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Bad TCP Numbers Received\n");
		return true;
	}
	if (tcp->len == 0)
		return true;
	/* What of it is new: from delta on. As much as the host takes now;
	 * the rest the PS2 sends again, unacknowledged. */
	if ((u32)tcp->len > delta)
	{
		const int fresh = tcp->len - (int)delta;
		int sent = send(t->sock, (const char*)tcp->data + delta, fresh, 0);
		if (sent < 0)
		{
			const int err = net_sock_error();
			if (!net_sock_would_block(err))
			{
				close_by_remote_rst(s);
				log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Send Error: %d\n", err);
				return true;
			}
			sent = 0;
		}
		if (sent > 0)
		{
			push_ps2_seq(t, t->expected_seq);
			t->expected_seq += (u32)sent;
		}
	}
	push_segment(t, TCP_ACK);
	return true;
}

static bool send_no_data(net_session* s, const net_tcp* tcp)
{
	if (tcp->flags & TCP_SYN)
	{
		close_by_remote_rst(s);
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Attempt to Connect to an open Port\n");
		return true;
	}
	take_options(&s->u.tcp, tcp);
	error_on_non_empty(s, tcp);
	return true;
}

/* The PS2 closes: FIN from it, ACK to it, and our FIN once the host is
 * done (remote_fin), which it ACKs. */
static bool close_by_ps2(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	if (error_on_non_empty(s, tcp))
		return true;
	push_ps2_seq(t, t->expected_seq);
	t->expected_seq++;
	t->state = TCP_CLOSED_BY_PS2;
	if (shutdown(t->sock, SHUT_WR) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Shutdown SD_SEND Error: %d\n", net_sock_error());
	push_segment(t, TCP_ACK);
	return true;
}

static bool close_by_ps2_last_ack(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	if (error_on_non_empty(s, tcp))
		return true;
	if (t->my_acked)
	{
		close_socket(t);
		complete(s);
	}
	return true;
}

/* The host closed first: our FIN, the PS2's ACK of it and its own FIN,
 * which we ACK. */
static bool close_by_remote_ack(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	if (error_on_non_empty(s, tcp))
		return true;
	if (t->my_acked)
	{
		close_socket(t);
		t->state = TCP_CLOSE_COMPLETED_FLUSH;
	}
	return true;
}

static bool close_by_remote_fin(net_session* s, const net_tcp* tcp)
{
	tcp_state* t = &s->u.tcp;
	if (error_on_non_empty(s, tcp))
		return true;
	push_ps2_seq(t, t->expected_seq);
	t->expected_seq++;
	if (shutdown(t->sock, SHUT_WR) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Shutdown SD_SEND Error: %d\n", net_sock_error());
	push_segment(t, TCP_ACK);
	if (t->my_acked)
	{
		close_socket(t);
		t->state = TCP_CLOSE_COMPLETED_FLUSH;
	}
	else
		t->state = TCP_CLOSED_BY_REMOTE_THEN_PS2_WAIT_ACK;
	return true;
}

static bool tcp_send(net_session* s, const net_ip* ip)
{
	tcp_state* t = &s->u.tcp;
	net_tcp tcp;

	if (!net_tcp_parse(ip->data, ip->len, &tcp))
		return false;
	if (t->dst_port != 0 && (tcp.dport != t->dst_port || tcp.sport != t->src_port))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Packet invalid for current session (Duplicate key?)\n");
		return false;
	}
	if (tcp.flags & TCP_RST)
	{
		/* The PS2 expects nothing more */
		if (t->sock == NET_SOCK_BAD)
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: RESET CLOSED CONNECTION\n");
		close_socket(t);
		complete(s);
		return true;
	}
	switch (t->state)
	{
		case TCP_NONE:
			return send_connect(s, &tcp);
		case TCP_SENDING_SYN_ACK:
			if (check_repeat_syn(t, &tcp) == NUM_BAD)
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Invalid Repeated SYN (SendingSYN_ACK)\n");
				return false;
			}
			return true; /* a repeat, while we still connect */
		case TCP_SENT_SYN_ACK:
			return send_connected(s, &tcp);
		case TCP_CONNECTED:
			if (tcp.flags & TCP_FIN)
				return close_by_ps2(s, &tcp);
			return send_data(s, &tcp);
		case TCP_CLOSED_BY_PS2:
			return send_no_data(s, &tcp);
		case TCP_CLOSED_BY_PS2_THEN_REMOTE_WAIT_ACK:
			return close_by_ps2_last_ack(s, &tcp);
		case TCP_CLOSED_BY_REMOTE:
			if (tcp.flags & TCP_FIN)
				return close_by_remote_fin(s, &tcp);
			return send_data(s, &tcp);
		case TCP_CLOSED_BY_REMOTE_THEN_PS2_WAIT_ACK:
			return close_by_remote_ack(s, &tcp);
		case TCP_CLOSE_COMPLETED:
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Attempt to send to a closed TCP connection\n");
			return false;
		default:
			close_by_remote_rst(s);
			log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Invalid TCP State\n");
			return true;
	}
}

static void tcp_reset(net_session* s)
{
	session_closed(s);
}

static void tcp_release(net_session* s)
{
	close_socket(&s->u.tcp);
}

static const net_session_ops tcp_ops = {
	tcp_recv, tcp_poll_interest, tcp_send, tcp_reset, tcp_release};

void tcp_session_init(net_session* s)
{
	tcp_state* t = &s->u.tcp;
	memset(t, 0, sizeof(*t));
	s->ops = &tcp_ops;
	t->sock = NET_SOCK_BAD;
	t->state = TCP_NONE;
	t->mss = TCP_MSS_MAX;
	t->window = TCP_MSS_MAX;
	t->my_acked = true;
	reset_my_numbers(t);
}
