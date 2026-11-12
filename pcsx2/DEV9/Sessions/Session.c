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
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "Session.h"

void session_closed(net_session* s)
{
	if (s->closed)
		return;
	s->closed = true;
	/* A fixed port lives as long as one of its clients does. */
	if (s->parent && --s->parent->u.fixed.children == 0)
		session_closed(s->parent);
}

int net_sock_error(void)
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool net_sock_would_block(int err)
{
#ifdef _WIN32
	return err == WSAEWOULDBLOCK;
#else
	return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

void net_sock_close(net_sock sock)
{
	if (sock == NET_SOCK_BAD)
		return;
#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

bool net_sock_nonblocking(net_sock sock)
{
#ifdef _WIN32
	u_long on = 1;
	return ioctlsocket(sock, FIONBIO, &on) == 0;
#else
	const int flags = fcntl(sock, F_GETFL, 0);
	return flags != -1 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int net_sock_ready(net_sock sock, int events)
{
#ifdef _WIN32
	WSAPOLLFD p;
#else
	struct pollfd p;
#endif
	int ready = 0;
	p.fd = sock;
	p.events = (short)(((events & SESSION_POLL_READ) ? POLLIN : 0) | ((events & SESSION_POLL_WRITE) ? POLLOUT : 0));
	p.revents = 0;
#ifdef _WIN32
	if (WSAPoll(&p, 1, 0) <= 0)
		return 0;
#else
	if (poll(&p, 1, 0) <= 0)
		return 0;
#endif
	if (p.revents & POLLIN)
		ready |= SESSION_POLL_READ;
	if (p.revents & POLLOUT)
		ready |= SESSION_POLL_WRITE;
	if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
		ready |= NET_SOCK_FAILED;
	return ready;
}
