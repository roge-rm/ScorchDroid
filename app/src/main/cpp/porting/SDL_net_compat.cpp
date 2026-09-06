// See SDL_net_compat.h for what this is and why.
// Adapted from SDL_net 1.2 (SDLnetTCP.c/SDLnetselect.c/SDLnet.c,
// https://github.com/SDL-mirror/SDL_net, zlib license), Windows paths
// dropped since Android is POSIX-only.

#include <SDL_net_compat.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *lastError = "";

int SDLNet_Init()
{
	return 0;
}

void SDLNet_Quit()
{
}

const char *SDLNet_GetError()
{
	return lastError;
}

static void setError(const char *msg)
{
	lastError = msg;
}

int SDLNet_ResolveHost(IPaddress *address, const char *host, Uint16 port)
{
	int retval = 0;

	if (host == nullptr)
	{
		address->host = INADDR_ANY;
	}
	else
	{
		address->host = inet_addr(host);
		if (address->host == INADDR_NONE)
		{
			struct hostent *hp = gethostbyname(host);
			if (hp)
			{
				memcpy(&address->host, hp->h_addr, hp->h_length);
			}
			else
			{
				retval = -1;
			}
		}
	}
	address->port = htons(port);
	return retval;
}

TCPsocket SDLNet_TCP_Open(IPaddress *ip)
{
	TCPsocket sock = (TCPsocket) malloc(sizeof(*sock));
	if (!sock)
	{
		setError("Out of memory");
		return nullptr;
	}
	memset(sock, 0, sizeof(*sock));

	sock->channel = socket(AF_INET, SOCK_STREAM, 0);
	if (sock->channel == -1)
	{
		setError("Couldn't create socket");
		SDLNet_TCP_Close(sock);
		return nullptr;
	}

	struct sockaddr_in sock_addr;
	memset(&sock_addr, 0, sizeof(sock_addr));

	if (ip->host != INADDR_NONE && ip->host != INADDR_ANY)
	{
		// Connecting to a remote host.
		sock_addr.sin_family = AF_INET;
		sock_addr.sin_addr.s_addr = ip->host;
		sock_addr.sin_port = ip->port;

		if (connect(sock->channel, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) == -1)
		{
			setError("Couldn't connect to remote host");
			SDLNet_TCP_Close(sock);
			return nullptr;
		}
		sock->sflag = 0;
	}
	else
	{
		// Binding locally (server socket).
		sock_addr.sin_family = AF_INET;
		sock_addr.sin_addr.s_addr = INADDR_ANY;
		sock_addr.sin_port = ip->port;

		int yes = 1;
		setsockopt(sock->channel, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

		if (bind(sock->channel, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) == -1)
		{
			setError("Couldn't bind to local port");
			SDLNet_TCP_Close(sock);
			return nullptr;
		}
		if (listen(sock->channel, 5) == -1)
		{
			setError("Couldn't listen to local port");
			SDLNet_TCP_Close(sock);
			return nullptr;
		}

		// Non-blocking accept() by default, matching upstream SDL_net -
		// NetBufferUtil::setBlockingIO() is what Scorched3D itself calls
		// afterwards to switch a socket back to blocking, same as on desktop.
		fcntl(sock->channel, F_SETFL, O_NONBLOCK);
		sock->sflag = 1;
	}
	sock->ready = 0;

	int nodelay = 1;
	setsockopt(sock->channel, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

	sock->remoteAddress.host = sock_addr.sin_addr.s_addr;
	sock->remoteAddress.port = sock_addr.sin_port;

	return sock;
}

TCPsocket SDLNet_TCP_Accept(TCPsocket server)
{
	if (!server->sflag)
	{
		setError("Only server sockets can accept()");
		return nullptr;
	}
	server->ready = 0;

	TCPsocket sock = (TCPsocket) malloc(sizeof(*sock));
	if (!sock)
	{
		setError("Out of memory");
		return nullptr;
	}
	memset(sock, 0, sizeof(*sock));

	struct sockaddr_in sock_addr;
	socklen_t           sock_alen = sizeof(sock_addr);
	sock->channel = accept(server->channel, (struct sockaddr *) &sock_addr, &sock_alen);
	if (sock->channel == -1)
	{
		setError("accept() failed");
		free(sock);
		return nullptr;
	}

	// Accepted sockets are blocking, matching upstream SDL_net.
	int flags = fcntl(sock->channel, F_GETFL, 0);
	fcntl(sock->channel, F_SETFL, flags & ~O_NONBLOCK);

	sock->remoteAddress.host = sock_addr.sin_addr.s_addr;
	sock->remoteAddress.port = sock_addr.sin_port;
	sock->sflag = 0;
	sock->ready = 0;

	return sock;
}

IPaddress *SDLNet_TCP_GetPeerAddress(TCPsocket sock)
{
	if (sock->sflag) return nullptr;
	return &sock->remoteAddress;
}

int SDLNet_TCP_Send(TCPsocket sock, const void *datap, int len)
{
	if (sock->sflag)
	{
		setError("Server sockets cannot send");
		return -1;
	}

	const Uint8 *data = (const Uint8 *) datap;
	int          sent = 0;
	int          left = len;
	errno = 0;
	do
	{
		int n = send(sock->channel, data, left, 0);
		if (n > 0)
		{
			sent += n;
			left -= n;
			data += n;
		}
		else if (n <= 0 && errno != EINTR)
		{
			break;
		}
	} while (left > 0);

	return sent;
}

int SDLNet_TCP_Recv(TCPsocket sock, void *data, int maxlen)
{
	if (sock->sflag)
	{
		setError("Server sockets cannot receive");
		return -1;
	}

	int len;
	do
	{
		errno = 0;
		len = (int) recv(sock->channel, data, maxlen, 0);
	} while (len == -1 && errno == EINTR);

	sock->ready = 0;
	return len;
}

void SDLNet_TCP_Close(TCPsocket sock)
{
	if (sock)
	{
		if (sock->channel != -1) close(sock->channel);
		free(sock);
	}
}

struct _SDLNet_SocketSet {
	int        numsockets;
	int        maxsockets;
	TCPsocket *sockets;
};

SDLNet_SocketSet SDLNet_AllocSocketSet(int maxsockets)
{
	SDLNet_SocketSet set = (SDLNet_SocketSet) malloc(sizeof(*set));
	if (!set) return nullptr;

	set->numsockets = 0;
	set->maxsockets = maxsockets;
	set->sockets    = (TCPsocket *) calloc(maxsockets, sizeof(TCPsocket));
	if (!set->sockets)
	{
		free(set);
		return nullptr;
	}
	return set;
}

int SDLNet_AddSocket(SDLNet_SocketSet set, TCPsocket sock)
{
	if (sock)
	{
		if (set->numsockets == set->maxsockets)
		{
			setError("socketset is full");
			return -1;
		}
		set->sockets[set->numsockets++] = sock;
	}
	return set->numsockets;
}

int SDLNet_DelSocket(SDLNet_SocketSet set, TCPsocket sock)
{
	if (sock)
	{
		int i;
		for (i = 0; i < set->numsockets; ++i)
		{
			if (set->sockets[i] == sock) break;
		}
		if (i == set->numsockets)
		{
			setError("socket not found in socketset");
			return -1;
		}
		--set->numsockets;
		for (; i < set->numsockets; ++i)
		{
			set->sockets[i] = set->sockets[i + 1];
		}
	}
	return set->numsockets;
}

int SDLNet_CheckSockets(SDLNet_SocketSet set, Uint32 timeoutMs)
{
	int maxfd = 0;
	for (int i = 0; i < set->numsockets; ++i)
	{
		if (set->sockets[i]->channel > maxfd) maxfd = set->sockets[i]->channel;
	}

	fd_set mask;
	int    retval;
	do
	{
		FD_ZERO(&mask);
		for (int i = 0; i < set->numsockets; ++i)
		{
			FD_SET(set->sockets[i]->channel, &mask);
		}

		struct timeval tv;
		tv.tv_sec  = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;

		errno  = 0;
		retval = select(maxfd + 1, &mask, nullptr, nullptr, &tv);
	} while (retval == -1 && errno == EINTR);

	if (retval > 0)
	{
		for (int i = 0; i < set->numsockets; ++i)
		{
			if (FD_ISSET(set->sockets[i]->channel, &mask))
			{
				set->sockets[i]->ready = 1;
			}
		}
	}
	return retval;
}

void SDLNet_FreeSocketSet(SDLNet_SocketSet set)
{
	if (set)
	{
		free(set->sockets);
		free(set);
	}
}

void SDLNet_Write16(Uint16 value, void *area)
{
	*(Uint16 *) area = htons(value);
}

void SDLNet_Write32(Uint32 value, void *area)
{
	*(Uint32 *) area = htonl(value);
}

Uint16 SDLNet_Read16(void *area)
{
	return ntohs(*(Uint16 *) area);
}

Uint32 SDLNet_Read32(void *area)
{
	return ntohl(*(Uint32 *) area);
}
