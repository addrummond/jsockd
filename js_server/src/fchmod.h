#ifndef FCHMOD_H_
#define FCHMOD_H_

#include <sys/stat.h>

// fchmod on the socket fd works on Linux but not mac. But Mac is only
// used for local dev where the permissions don't matter, so NBD.

#ifdef LINUX
#define socket_fchmod(sock, perm) fchmod((sock), (perm))
#else
#define socket_fchmod(sock, perm) 0
#endif

#endif
