/*
 * Copyright (C) 2002 the xine project
 * 
 * This file is part of xine, a free video player.
 * 
 * xine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * xine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id: mms.h,v 1.5 2002/05/06 21:40:02 f1rmb Exp $
 *
 * libmms public header
 */

#ifndef HAVE_MMS_H
#define HAVE_MMS_H

#include <inttypes.h>

typedef struct mms_s mms_t;

char*    mms_connect_common(int *s ,int *port, char *url, char **host, char **path, char **file);
mms_t*   mms_connect (char *url);

int      mms_read (mms_t *this, char *data, int len);
uint32_t mms_get_length (mms_t *this);
void     mms_close (mms_t *this);

int      mms_peek_header (mms_t *this, char *data);

#endif

