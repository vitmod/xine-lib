/*
 * Sun mediaLib optimized DSP utils
 * Copyright (c) 2001 Juergen Keil.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include "dsputil.h"

#include <mlib_types.h>
#include <mlib_status.h>
#include <mlib_sys.h>
#include <mlib_video.h>


void ff_idct_mlib(DCTELEM *data)
{
    mlib_VideoIDCT8x8_S16_S16 (data, data);
}


void ff_fdct_mlib(DCTELEM *data)
{
    mlib_VideoDCT8x8_S16_S16 (data, data);
}
