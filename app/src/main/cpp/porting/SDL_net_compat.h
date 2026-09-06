#ifndef SCORCHDROID_SDL_NET_COMPAT_H
#define SCORCHDROID_SDL_NET_COMPAT_H

// Android build: a POSIX-socket-backed reimplementation of the small
// subset of the SDL_net 1.2 TCP API that upstream Scorched3D's
// src/common/net actually uses (SDL is not part of this build - see the
// porting plan). This lets NetServerTCP3*.cpp etc. keep calling the exact
// same API, unmodified, which matters: those files implement the wire
// protocol we want to stay byte-for-byte compatible with a PC build for
// cross-play, so preserving their logic untouched (rather than hand-
// patching 50+ call sites to raw POSIX calls) is the safer choice.
//
// Ported from SDL_net 1.2's SDLnetTCP.c/SDLnetselect.c/SDLnet.c
// (https://github.com/SDL-mirror/SDL_net, zlib license) with Windows-only
// code paths dropped (Android is POSIX-only) and IPaddress/TCPsocket kept
// binary-identical to upstream's own private struct layout - Scorched3D's
// NetBufferUtil.cpp already relies on that exact layout via its own
// "ripped from SDL_net" hack (see that file's comment), so this shim's
// struct _TCPsocket must stay in sync with it.

#include <SDL_types_compat.h>
#include <netinet/in.h>

typedef struct {
	Uint32 host;  // 32-bit IPv4 host address, network byte order
	Uint16 port;  // 16-bit protocol port, network byte order
} IPaddress;

#ifndef INADDR_NONE
#define INADDR_NONE 0xFFFFFFFF
#endif

struct _TCPsocket {
	int       ready;
	int       channel;
	IPaddress remoteAddress;
	IPaddress localAddress;
	int       sflag;
};
typedef struct _TCPsocket *TCPsocket;

struct _SDLNet_SocketSet;
typedef struct _SDLNet_SocketSet *SDLNet_SocketSet;

int         SDLNet_Init();
void        SDLNet_Quit();
const char *SDLNet_GetError();

// If host is null, resolves to INADDR_ANY (for opening a local server socket).
int SDLNet_ResolveHost(IPaddress *address, const char *host, Uint16 port);

// If ip->host is INADDR_ANY/INADDR_NONE, opens a listening server socket;
// otherwise connects to the given remote address.
TCPsocket  SDLNet_TCP_Open(IPaddress *ip);
TCPsocket  SDLNet_TCP_Accept(TCPsocket server);
IPaddress *SDLNet_TCP_GetPeerAddress(TCPsocket sock);
int        SDLNet_TCP_Send(TCPsocket sock, const void *data, int len);
int        SDLNet_TCP_Recv(TCPsocket sock, void *data, int maxlen);
void       SDLNet_TCP_Close(TCPsocket sock);

SDLNet_SocketSet SDLNet_AllocSocketSet(int maxsockets);
int              SDLNet_AddSocket(SDLNet_SocketSet set, TCPsocket sock);
int              SDLNet_DelSocket(SDLNet_SocketSet set, TCPsocket sock);
int              SDLNet_CheckSockets(SDLNet_SocketSet set, Uint32 timeoutMs);
void             SDLNet_FreeSocketSet(SDLNet_SocketSet set);

#define SDLNet_TCP_AddSocket(set, sock) SDLNet_AddSocket(set, sock)
#define SDLNet_TCP_DelSocket(set, sock) SDLNet_DelSocket(set, sock)
#define SDLNet_SocketReady(sock) ((sock) != nullptr && (sock)->ready)

void   SDLNet_Write16(Uint16 value, void *area);
void   SDLNet_Write32(Uint32 value, void *area);
Uint16 SDLNet_Read16(void *area);
Uint32 SDLNet_Read32(void *area);

#endif  // SCORCHDROID_SDL_NET_COMPAT_H
