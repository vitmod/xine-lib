dnl Configure paths for XINE
dnl
dnl Copyright (C) 2001 Daniel Caujolle-Bert <segfault@club-internet.fr>
dnl  
dnl This program is free software; you can redistribute it and/or modify
dnl it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation; either version 2 of the License, or
dnl (at your option) any later version.
dnl  
dnl This program is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl GNU General Public License for more details.
dnl  
dnl You should have received a copy of the GNU General Public License
dnl along with this program; if not, write to the Free Software
dnl Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
dnl  
dnl  
dnl As a special exception to the GNU General Public License, if you
dnl distribute this file as part of a program that contains a configuration
dnl script generated by Autoconf, you may include it under the same
dnl distribution terms that you use for the rest of that program.
dnl  

dnl AM_PATH_XINE([MINIMUM-VERSION, [ACTION-IF-FOUND [, ACTION-IF-NOT-FOUND ]]])
dnl Test for XINE, and define XINE_CFLAGS and XINE_LIBS
dnl
AC_DEFUN([AM_PATH_XINE],
[dnl 
dnl Get the cflags and libraries from the xine-config script
dnl
AC_ARG_WITH(xine-prefix,
    [  --with-xine-prefix=PFX  Prefix where XINE is installed (optional)],
            xine_config_prefix="$withval", xine_config_prefix="")
AC_ARG_WITH(xine-exec-prefix,
    [  --with-xine-exec-prefix=PFX                                                                             Exec prefix where XINE is installed (optional)],
            xine_config_exec_prefix="$withval", xine_config_exec_prefix="")
AC_ARG_ENABLE(xinetest, 
    [  --disable-xinetest      Do not try to compile and run a test XINE program],, enable_xinetest=yes)

  if test x$xine_config_exec_prefix != x ; then
     xine_config_args="$xine_config_args --exec-prefix=$xine_config_exec_prefix"
     if test x${XINE_CONFIG+set} != xset ; then
        XINE_CONFIG=$xine_config_exec_prefix/bin/xine-config
     fi
  fi
  if test x$xine_config_prefix != x ; then
     xine_config_args="$xine_config_args --prefix=$xine_config_prefix"
     if test x${XINE_CONFIG+set} != xset ; then
        XINE_CONFIG=$xine_config_prefix/bin/xine-config
     fi
  fi

  min_xine_version=ifelse([$1], ,0.5.0,$1)
  if test "x$enable_xinetest" != "xyes" ; then
    AC_MSG_CHECKING([for XINE-LIB version >= $min_xine_version])
  else
    AC_PATH_PROG(XINE_CONFIG, xine-config, no)
    AC_MSG_CHECKING([for XINE-LIB version >= $min_xine_version])
    no_xine=""
    if test "$XINE_CONFIG" = "no" ; then
      no_xine=yes
    else
      XINE_CFLAGS=`$XINE_CONFIG $xine_config_args --cflags`
      XINE_LIBS=`$XINE_CONFIG $xine_config_args --libs`
      xine_config_major_version=`$XINE_CONFIG $xine_config_args --version | \
             sed 's/\([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\1/'`
      xine_config_minor_version=`$XINE_CONFIG $xine_config_args --version | \
             sed 's/\([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\2/'`
      xine_config_sub_version=`$XINE_CONFIG $xine_config_args --version | \
             sed 's/\([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\3/'`
      xine_data_dir=`$XINE_CONFIG $xine_config_args --datadir`
      xine_logo_mrl=`$XINE_CONFIG $xine_config_args --logomrl`
      xine_script_dir=`$XINE_CONFIG $xine_config_args --scriptdir`
      xine_desktop_dir=`$XINE_CONFIG $xine_config_args --desktopdir`
      xine_plugin_dir=`$XINE_CONFIG $xine_config_args --plugindir`
      xine_locale_dir=`$XINE_CONFIG $xine_config_args --localedir`
      dnl    if test "x$enable_xinetest" = "xyes" ; then
      ac_save_CFLAGS="$CFLAGS"
      ac_save_LIBS="$LIBS"
      CFLAGS="$CFLAGS $XINE_CFLAGS"
      LIBS="$XINE_LIBS $LIBS"
dnl
dnl Now check if the installed XINE is sufficiently new. (Also sanity
dnl checks the results of xine-config to some extent
dnl
      AC_LANG_SAVE()
      AC_LANG_C()
      rm -f conf.xinetest
      AC_TRY_RUN([
#include <xine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int 
main ()
{
  int major, minor, sub;
   char *tmp_version;

  system ("touch conf.xinetest");

  /* HP/UX 9 (%@#!) writes to sscanf strings */
  tmp_version = (char *) strdup("$min_xine_version");
  if (sscanf(tmp_version, "%d.%d.%d", &major, &minor, &sub) != 3) {
     printf("%s, bad version string\n", "$min_xine_version");
     exit(1);
   }

  if ((XINE_MAJOR_VERSION != $xine_config_major_version) ||
      (XINE_MINOR_VERSION != $xine_config_minor_version) ||
      (XINE_SUB_VERSION != $xine_config_sub_version))
    {
      printf("\n*** 'xine-config --version' returned %d.%d.%d, but XINE (%d.%d.%d)\n", 
             $xine_config_major_version, $xine_config_minor_version, $xine_config_sub_version,
             XINE_MAJOR_VERSION, XINE_MINOR_VERSION, XINE_SUB_VERSION);
      printf ("*** was found! If xine-config was correct, then it is best\n");
      printf ("*** to remove the old version of XINE. You may also be able to fix the error\n");
      printf("*** by modifying your LD_LIBRARY_PATH enviroment variable, or by editing\n");
      printf("*** /etc/ld.so.conf. Make sure you have run ldconfig if that is\n");
      printf("*** required on your system.\n");
      printf("*** If xine-config was wrong, set the environment variable XINE_CONFIG\n");
      printf("*** to point to the correct copy of xine-config, and remove the file config.cache\n");
      printf("*** before re-running configure\n");
    } 
  else
    {
      if ((XINE_MAJOR_VERSION > major) ||
        ((XINE_MAJOR_VERSION == major) && (XINE_MINOR_VERSION > minor)) ||
        ((XINE_MAJOR_VERSION == major) && (XINE_MINOR_VERSION == minor) && (XINE_SUB_VERSION >= sub)))
      {
        return 0;
       }
     else
      {
        printf("\n*** An old version of XINE (%d.%d.%d) was found.\n",
               XINE_MAJOR_VERSION, XINE_MINOR_VERSION, XINE_SUB_VERSION);
        printf("*** You need a version of XINE newer than %d.%d.%d. The latest version of\n",
	       major, minor, sub);
        printf("*** XINE is always available from:\n");
        printf("***        http://xine.sourceforge.net\n");
        printf("***\n");
        printf("*** If you have already installed a sufficiently new version, this error\n");
        printf("*** probably means that the wrong copy of the xine-config shell script is\n");
        printf("*** being found. The easiest way to fix this is to remove the old version\n");
        printf("*** of XINE, but you can also set the XINE_CONFIG environment to point to the\n");
        printf("*** correct copy of xine-config. (In this case, you will have to\n");
        printf("*** modify your LD_LIBRARY_PATH enviroment variable, or edit /etc/ld.so.conf\n");
        printf("*** so that the correct libraries are found at run-time))\n");
      }
    }
  return 1;
}
],, no_xine=yes,[echo $ac_n "cross compiling; assumed OK... $ac_c"])
       CFLAGS="$ac_save_CFLAGS"
       LIBS="$ac_save_LIBS"
     fi
    fi
    if test "x$no_xine" = x ; then
       AC_MSG_RESULT(yes)
       ifelse([$2], , :, [$2])     
    else
      AC_MSG_RESULT(no)
      if test "$XINE_CONFIG" = "no" ; then
        echo "*** The xine-config script installed by XINE could not be found"
        echo "*** If XINE was installed in PREFIX, make sure PREFIX/bin is in"
        echo "*** your path, or set the XINE_CONFIG environment variable to the"
        echo "*** full path to xine-config."
      else
        if test -f conf.xinetest ; then
          :
        else
          echo "*** Could not run XINE test program, checking why..."
          CFLAGS="$CFLAGS $XINE_CFLAGS"
          LIBS="$LIBS $XINE_LIBS"
          AC_TRY_LINK([
#include <xine.h>
#include <stdio.h>
],      [ return ((XINE_MAJOR_VERSION) || (XINE_MINOR_VERSION) || (XINE_SUB_VERSION)); ],
        [ echo "*** The test program compiled, but did not run. This usually means"
          echo "*** that the run-time linker is not finding XINE or finding the wrong"
          echo "*** version of XINE. If it is not finding XINE, you'll need to set your"
          echo "*** LD_LIBRARY_PATH environment variable, or edit /etc/ld.so.conf to point"
          echo "*** to the installed location  Also, make sure you have run ldconfig if that"
          echo "*** is required on your system"
	  echo "***"
          echo "*** If you have an old version installed, it is best to remove it, although"
          echo "*** you may also be able to get things to work by modifying LD_LIBRARY_PATH"
          echo "***"],
        [ echo "*** The test program failed to compile or link. See the file config.log for the"
          echo "*** exact error that occured. This usually means XINE was incorrectly installed"
          echo "*** or that you have moved XINE since it was installed. In the latter case, you"
          echo "*** may want to edit the xine-config script: $XINE_CONFIG" ])
          CFLAGS="$ac_save_CFLAGS"
          LIBS="$ac_save_LIBS"
        fi
      fi
    XINE_CFLAGS=""
    XINE_LIBS=""
    ifelse([$3], , :, [$3])
  fi
  AC_SUBST(XINE_CFLAGS)
  AC_SUBST(XINE_LIBS)
  AC_LANG_RESTORE()
  rm -f conf.xinetest
])
