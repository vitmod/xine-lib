/*
 * Copyright (C) 2000-2002 the xine project
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
 * $Id: xineintl.h,v 1.6 2002/10/23 17:12:35 guenter Exp $
 *
 */

#ifndef HAVE_XINEINTL_H
#define HAVE_XINEINTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <locale.h>

#ifdef ENABLE_NLS
#    include <libintl.h>
#    define _(String) dgettext ("xine-lib", String)
#    ifdef gettext_noop
#        define N_(String) gettext_noop (String)
#    else
#        define N_(String) (String)
#    endif
#    ifndef HAVE_NGETTEXT
#        define ngettext(Singular, Plural, IsPlural) (Singular)
#    endif
#else
/* Stubs that do something close enough.  */
#    define textdomain(String) (String)
#    define gettext(String) (String)
#    define dgettext(Domain,Message) (Message)
#    define dcgettext(Domain,Message,Type) (Message)
#    define ngettext(Singular, Plural, IsPlural) (Singular)
#    define bindtextdomain(Domain,Directory) (Domain)
#    define _(String) (String)
#    define N_(String) (String)
#endif

#ifdef __cplusplus
}
#endif

#endif
