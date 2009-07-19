/*
 Copyright (c) 2004-2009 NFG Net Facilities Group BV support@nfg.nl

  This program is free software; you can redistribute it and/or 
  modify it under the terms of the GNU General Public License 
  as published by the Free Software Foundation; either 
  version 2 of the License, or (at your option) any later 
  version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef DM_TLS_H
#define DM_TLS_H

#include "dbmail.h"

SSL_CTX *tls_init(void);
void tls_load_certs(serverConfig_t *);
void tls_load_ciphers(serverConfig_t *);
char *tls_get_error(void);

#endif
