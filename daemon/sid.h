
/* NFSv4.1 client for Windows
 * Copyright � 2012 The Regents of the University of Michigan
 *
 * Olga Kornievskaia <aglo@umich.edu>
 * Casey Bodley <cbodley@umich.edu>
 * Roland Mainz <roland.mainz@nrubsig.org>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * without any warranty; without even the implied warranty of merchantability
 * or fitness for a particular purpose.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 */

#ifndef __NFS41_DAEMON_SID_H
#define __NFS41_DAEMON_SID_H 1

#include "nfs41_build_features.h"
#include "nfs41_daemon.h"
#include <stdbool.h>

typedef struct _sidcache sidcache;

extern sidcache user_sidcache;
extern sidcache group_sidcache;

/* prototypes */
int create_unknownsid(WELL_KNOWN_SID_TYPE type, PSID *sid, DWORD *sid_len);
void sidcache_init(void);
void sidcache_add(sidcache *cache, const char* win32name, PSID value);
PSID *sidcache_getcached_byname(sidcache *cache, const char *win32name);
bool sidcache_getcached_bysid(sidcache *cache, PSID sid, char *out_win32name);

int map_nfs4servername_2_sid(nfs41_daemon_globals *nfs41dg, int query, DWORD *sid_len, PSID *sid, LPCSTR name);

#endif /* !__NFS41_DAEMON_SID_H */
