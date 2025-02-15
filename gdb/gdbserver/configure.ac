dnl Autoconf configure script for GDB server.
dnl Copyright (C) 2000, 2002-2012 Free Software Foundation, Inc.
dnl
dnl This file is part of GDB.
dnl
dnl This program is free software; you can redistribute it and/or modify
dnl it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation; either version 3 of the License, or
dnl (at your option) any later version.
dnl
dnl This program is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl GNU General Public License for more details.
dnl
dnl You should have received a copy of the GNU General Public License
dnl along with this program.  If not, see <http://www.gnu.org/licenses/>.

dnl Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)dnl

AC_INIT(server.c)
AC_CONFIG_HEADER(config.h:config.in)
AC_CONFIG_LIBOBJ_DIR(../gnulib)

AC_ARG_WITH([sysroot],
  [AS_HELP_STRING([--with-sysroot=SYSROOT],
                  [use sysroot as the system root during the build])],
  [if test x"$withval" != x ; then
     CFLAGS="$CFLAGS --sysroot=$withval"
   fi],
  [])

AC_PROG_CC
AC_GNU_SOURCE

AC_CANONICAL_SYSTEM

AC_PROG_INSTALL

AC_ARG_PROGRAM

AC_HEADER_STDC
AC_HEADER_DIRENT

AC_FUNC_ALLOCA
AC_CHECK_HEADERS(sgtty.h termio.h termios.h sys/reg.h string.h dnl
		 proc_service.h sys/procfs.h thread_db.h linux/elf.h dnl
		 stdlib.h unistd.h dnl
		 errno.h fcntl.h signal.h sys/file.h malloc.h dnl
		 sys/ioctl.h netinet/in.h sys/socket.h netdb.h dnl
		 netinet/tcp.h arpa/inet.h sys/wait.h)
AC_CHECK_FUNCS(pread pwrite pread64)
AC_REPLACE_FUNCS(memmem vasprintf vsnprintf)

# Check for UST
ustlibs=""
ustinc=""

AC_ARG_WITH(ust, [  --with-ust=PATH       Specify prefix directory for the installed UST package
                          Equivalent to --with-ust-include=PATH/include
                          plus --with-ust-lib=PATH/lib])
AC_ARG_WITH(ust_include, [  --with-ust-include=PATH Specify directory for installed UST include files])
AC_ARG_WITH(ust_lib, [  --with-ust-lib=PATH   Specify the directory for the installed UST library])

case $with_ust in
  no)
    ustlibs=
    ustinc=
    ;;
  "" | yes)
    ustlibs=" -lust "
    ustinc=""
    ;;
  *)
    ustlibs="-L$with_ust/lib -lust"
    ustinc="-I$with_ust/include "
    ;;
esac
if test "x$with_ust_include" != x; then
  ustinc="-I$with_ust_include "
fi
if test "x$with_ust_lib" != x; then
  ustlibs="-L$with_ust_lib -lust"
fi

if test "x$with_ust" != "xno"; then
  saved_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS $ustinc"
  AC_MSG_CHECKING([for ust])
  AC_TRY_COMPILE([
#define CONFIG_UST_GDB_INTEGRATION
#include <ust/ust.h>
  ],[],
  [AC_MSG_RESULT([yes]); AC_DEFINE(HAVE_UST, 1, [Define if UST is available])],
  [AC_MSG_RESULT([no]); ustlibs= ; ustinc= ])
  CFLAGS="$saved_CFLAGS"
fi

# Flags needed for UST
AC_SUBST(ustlibs)
AC_SUBST(ustinc)

AC_ARG_ENABLE(werror,
  AS_HELP_STRING([--enable-werror], [treat compile warnings as errors]),
  [case "${enableval}" in
     yes | y) ERROR_ON_WARNING="yes" ;;
     no | n)  ERROR_ON_WARNING="no" ;;
     *) AC_MSG_ERROR(bad value ${enableval} for --enable-werror) ;;
   esac])

# Enable -Werror by default when using gcc
if test "${GCC}" = yes -a -z "${ERROR_ON_WARNING}" ; then
    ERROR_ON_WARNING=yes
fi

WERROR_CFLAGS=""
if test "${ERROR_ON_WARNING}" = yes ; then
    WERROR_CFLAGS="-Werror"
fi

build_warnings="-Wall -Wdeclaration-after-statement -Wpointer-arith \
-Wformat-nonliteral -Wno-char-subscripts"

WARN_CFLAGS=""
if test "x$GCC" = xyes
then
    AC_MSG_CHECKING(compiler warning flags)
    # Separate out the -Werror flag as some files just cannot be
    # compiled with it enabled.
    for w in ${build_warnings}; do
	case $w in
	-Werr*) WERROR_CFLAGS=-Werror ;;
	*) # Check that GCC accepts it
	    saved_CFLAGS="$CFLAGS"
	    CFLAGS="$CFLAGS $w"
	    AC_TRY_COMPILE([],[],WARN_CFLAGS="${WARN_CFLAGS} $w",)
	    CFLAGS="$saved_CFLAGS"
	esac
    done
    AC_MSG_RESULT(${WARN_CFLAGS} ${WERROR_CFLAGS})
fi
AC_SUBST(WARN_CFLAGS)
AC_SUBST(WERROR_CFLAGS)

dnl dladdr is glibc-specific.  It is used by thread-db.c but only for
dnl debugging messages.  It lives in -ldl which is handled below so we don't
dnl use AC_CHECK_LIB (or AC_SEARCH_LIBS) here.  Instead we just temporarily
dnl augment LIBS.
old_LIBS="$LIBS"
LIBS="$LIBS -ldl"
AC_CHECK_FUNCS(dladdr)
LIBS="$old_LIBS"

have_errno=no
AC_MSG_CHECKING(for errno)
AC_TRY_LINK([
#if HAVE_ERRNO_H
#include <errno.h>
#endif], [static int x; x = errno;],
  [AC_MSG_RESULT(yes - in errno.h); AC_DEFINE(HAVE_ERRNO, 1, [Define if errno is available]) have_errno=yes])
if test $have_errno = no; then
AC_TRY_LINK([
#if HAVE_ERRNO_H
#include <errno.h>
#endif], [extern int errno; static int x; x = errno;],
  [AC_MSG_RESULT(yes - must define); AC_DEFINE(HAVE_ERRNO, 1, [Define if errno is available]) AC_DEFINE(MUST_DEFINE_ERRNO, 1, [Checking if errno must be defined])],
  [AC_MSG_RESULT(no)])
fi

AC_CHECK_DECLS([strerror, perror, memmem, vasprintf, vsnprintf])

AC_CHECK_TYPES(socklen_t, [], [],
[#include <sys/types.h>
#include <sys/socket.h>
])

AC_CHECK_TYPES([Elf32_auxv_t, Elf64_auxv_t], [], [],
#include <elf.h>
)

ACX_PKGVERSION([GDB])
ACX_BUGURL([http://www.gnu.org/software/gdb/bugs/])
AC_DEFINE_UNQUOTED([PKGVERSION], ["$PKGVERSION"], [Additional package description])
AC_DEFINE_UNQUOTED([REPORT_BUGS_TO], ["$REPORT_BUGS_TO"], [Bug reporting address])

# Check for various supplementary target information (beyond the
# triplet) which might affect the choices in configure.srv.
case "${target}" in
changequote(,)dnl
  i[34567]86-*-linux*)
changequote([,])dnl
    AC_CACHE_CHECK([if building for x86-64], [gdb_cv_i386_is_x86_64],
      	           [save_CPPFLAGS="$CPPFLAGS"
                    CPPFLAGS="$CPPFLAGS $CFLAGS"
                    AC_EGREP_CPP([got it], [
#if __x86_64__
got it
#endif
                 ], [gdb_cv_i386_is_x86_64=yes],
                    [gdb_cv_i386_is_x86_64=no])
                    CPPFLAGS="$save_CPPFLAGS"])
    ;;
  m68k-*-*)
    AC_CACHE_CHECK([if building for Coldfire], [gdb_cv_m68k_is_coldfire],
      	           [save_CPPFLAGS="$CPPFLAGS"
                    CPPFLAGS="$CPPFLAGS $CFLAGS"
                    AC_EGREP_CPP([got it], [
#ifdef __mcoldfire__
got it
#endif
                 ], [gdb_cv_m68k_is_coldfire=yes],
                    [gdb_cv_m68k_is_coldfire=no])
                    CPPFLAGS="$save_CPPFLAGS"])
    ;;
esac

. ${srcdir}/configure.srv

if test "${srv_mingwce}" = "yes"; then
  LIBS="$LIBS -lws2"
elif test "${srv_mingw}" = "yes"; then
  LIBS="$LIBS -lws2_32"
elif test "${srv_qnx}" = "yes"; then
  LIBS="$LIBS -lsocket"
elif test "${srv_lynxos}" = "yes"; then
  LIBS="$LIBS -lnetinet"
fi

if test "${srv_mingw}" = "yes"; then
  AC_DEFINE(USE_WIN32API, 1,
	    [Define if we should use the Windows API, instead of the
	     POSIX API.  On Windows, we use the Windows API when
	     building for MinGW, but the POSIX API when building
	     for Cygwin.])
fi

if test "${srv_linux_usrregs}" = "yes"; then
  AC_DEFINE(HAVE_LINUX_USRREGS, 1,
	    [Define if the target supports PTRACE_PEEKUSR for register ]
	    [access.])
fi

if test "${srv_linux_regsets}" = "yes"; then
  AC_DEFINE(HAVE_LINUX_REGSETS, 1,
	    [Define if the target supports register sets.])

  AC_MSG_CHECKING(for PTRACE_GETREGS)
  AC_CACHE_VAL(gdbsrv_cv_have_ptrace_getregs,
  [AC_TRY_COMPILE([#include <sys/ptrace.h>],
		  [PTRACE_GETREGS;],
		  [gdbsrv_cv_have_ptrace_getregs=yes],
		  [gdbsrv_cv_have_ptrace_getregs=no])])
  AC_MSG_RESULT($gdbsrv_cv_have_ptrace_getregs)
  if test "${gdbsrv_cv_have_ptrace_getregs}" = "yes"; then
    AC_DEFINE(HAVE_PTRACE_GETREGS, 1,
	      [Define if the target supports PTRACE_GETREGS for register ]
	      [access.])
  fi

  AC_MSG_CHECKING(for PTRACE_GETFPXREGS)
  AC_CACHE_VAL(gdbsrv_cv_have_ptrace_getfpxregs,
  [AC_TRY_COMPILE([#include <sys/ptrace.h>],
		  [PTRACE_GETFPXREGS;],
		  [gdbsrv_cv_have_ptrace_getfpxregs=yes],
		  [gdbsrv_cv_have_ptrace_getfpxregs=no])])
  AC_MSG_RESULT($gdbsrv_cv_have_ptrace_getfpxregs)
  if test "${gdbsrv_cv_have_ptrace_getfpxregs}" = "yes"; then
    AC_DEFINE(HAVE_PTRACE_GETFPXREGS, 1,
	      [Define if the target supports PTRACE_GETFPXREGS for extended ]
	      [register access.])
  fi
fi

if test "$ac_cv_header_sys_procfs_h" = yes; then
  BFD_HAVE_SYS_PROCFS_TYPE(lwpid_t)
  BFD_HAVE_SYS_PROCFS_TYPE(psaddr_t)
  BFD_HAVE_SYS_PROCFS_TYPE(prgregset_t)
  BFD_HAVE_SYS_PROCFS_TYPE(elf_fpregset_t)
fi

case "$target" in
*android*)
    srv_android_thread_db="yes"
    thread_db_inc="-I$srcdir/../../android -fno-short-enums"
    ;;
*)
    dnl Check for libdl, but do not add it to LIBS as only gdbserver
    dnl needs it (and gdbreplay doesn't).
    old_LIBS="$LIBS"
    AC_CHECK_LIB(dl, dlopen)
    LIBS="$old_LIBS"
    ;;
esac
AC_SUBST(thread_db_inc)

srv_thread_depfiles=
srv_libs=
USE_THREAD_DB=

if test "$srv_linux_thread_db" = "yes"; then
  if test "$ac_cv_lib_dl_dlopen" = "yes"; then
    srv_libs="-ldl"
    AC_MSG_CHECKING(for the dynamic export flag)
    old_LDFLAGS="$LDFLAGS"
    # Older GNU ld supports --export-dynamic but --dynamic-list may not be
    # supported there.
    RDYNAMIC="-Wl,--dynamic-list=${srcdir}/proc-service.list"
    LDFLAGS="$LDFLAGS $RDYNAMIC"
    AC_TRY_LINK([], [],
		[found="-Wl,--dynamic-list"
		 RDYNAMIC='-Wl,--dynamic-list=$(srcdir)/proc-service.list'],
		[RDYNAMIC="-rdynamic"
		 LDFLAGS="$old_LDFLAGS $RDYNAMIC"
		 AC_TRY_LINK([], [],
			     [found="-rdynamic"],
			     [found="no"
			      RDYNAMIC=""])])
    AC_SUBST(RDYNAMIC)
    LDFLAGS="$old_LDFLAGS"
    AC_MSG_RESULT($found)
  elif test "$srv_android_thread_db" != "yes"; then
    srv_libs="-lthread_db"
  fi

  srv_thread_depfiles="thread-db.o proc-service.o"
  if test "$srv_android_thread_db" = "yes"; then
    srv_thread_depfiles="$srv_thread_depfiles libthread_db.o"
  fi

  USE_THREAD_DB="-DUSE_THREAD_DB"
  AC_CACHE_CHECK([for TD_VERSION], gdbsrv_cv_have_td_version,
  [AC_TRY_COMPILE([#include <thread_db.h>], [TD_VERSION;],
		  [gdbsrv_cv_have_td_version=yes],
		  [gdbsrv_cv_have_td_version=no])])
  if test $gdbsrv_cv_have_td_version = yes; then
    AC_DEFINE(HAVE_TD_VERSION, 1, [Define if TD_VERSION is available.])
  fi
fi

AC_ARG_WITH(libthread-db,
AS_HELP_STRING([--with-libthread-db=PATH], [use given libthread_db directly]),
[srv_libthread_db_path="${withval}"
  srv_libs="$srv_libthread_db_path"
])

if test "$srv_libs" != "" -a "$srv_libs" != "-ldl" -o "$srv_android_thread_db" = "yes"; then
  AC_DEFINE(USE_LIBTHREAD_DB_DIRECTLY, 1, [Define if we should use libthread_db directly.])
fi

if test "$srv_xmlfiles" != ""; then
  srv_xmlbuiltin="xml-builtin.o"
  AC_DEFINE(USE_XML, 1, [Define if an XML target description is available.])

  tmp_xmlfiles=$srv_xmlfiles
  srv_xmlfiles=""
  for f in $tmp_xmlfiles; do
    srv_xmlfiles="$srv_xmlfiles \$(XML_DIR)/$f"
  done
fi

GDBSERVER_DEPFILES="$srv_regobj $srv_tgtobj $srv_hostio_err_objs $srv_thread_depfiles"
GDBSERVER_LIBS="$srv_libs"

dnl Check whether the target supports __sync_*_compare_and_swap.
AC_CACHE_CHECK([whether the target supports __sync_*_compare_and_swap],
		gdbsrv_cv_have_sync_builtins, [
AC_TRY_LINK([], [int foo, bar; bar = __sync_val_compare_and_swap(&foo, 0, 1);],
		gdbsrv_cv_have_sync_builtins=yes,
		gdbsrv_cv_have_sync_builtins=no)])
if test $gdbsrv_cv_have_sync_builtins = yes; then
  AC_DEFINE(HAVE_SYNC_BUILTINS, 1,
    [Define to 1 if the target supports __sync_*_compare_and_swap])
fi

dnl Check for -fvisibility=hidden support in the compiler.
saved_cflags="$CFLAGS"
CFLAGS="$CFLAGS -fvisibility=hidden"
AC_COMPILE_IFELSE(AC_LANG_PROGRAM([]),
		        [gdbsrv_cv_have_visibility_hidden=yes],
	        	[gdbsrv_cv_have_visibility_hidden=no])
CFLAGS="$saved_cflags"

dnl Check if we can disable the virtual address space randomization.
dnl The functionality of setarch -R.
AC_CHECK_DECLS([ADDR_NO_RANDOMIZE],,, [#include <sys/personality.h>])
define([PERSONALITY_TEST], [AC_LANG_PROGRAM([#include <sys/personality.h>], [
#      if !HAVE_DECL_ADDR_NO_RANDOMIZE
#       define ADDR_NO_RANDOMIZE 0x0040000
#      endif
       /* Test the flag could be set and stays set.  */
       personality (personality (0xffffffff) | ADDR_NO_RANDOMIZE);
       if (!(personality (personality (0xffffffff)) & ADDR_NO_RANDOMIZE))
           return 1])])
AC_RUN_IFELSE([PERSONALITY_TEST],
              [gdbsrv_cv_have_personality=true],
              [gdbsrv_cv_have_personality=false],
              [AC_LINK_IFELSE([PERSONALITY_TEST],
                              [gdbsrv_cv_have_personality=true],
                              [gdbsrv_cv_have_personality=false])])
if $gdbsrv_cv_have_personality
then
    AC_DEFINE([HAVE_PERSONALITY], 1,
              [Define if you support the personality syscall.])
fi


IPA_DEPFILES=""
extra_libraries=""

# check whether to enable the inprocess agent
if test "$ipa_obj" != "" \
   -a "$gdbsrv_cv_have_sync_builtins" = yes \
   -a "$gdbsrv_cv_have_visibility_hidden" = yes; then
   have_ipa=true
else
   have_ipa=false
fi

AC_ARG_ENABLE(inprocess-agent,
AS_HELP_STRING([--enable-inprocess-agent], [inprocess agent]),
[case "$enableval" in
  yes) want_ipa=true ;;
  no) want_ipa=false ;;
  *) AC_MSG_ERROR([bad value $enableval for inprocess-agent]) ;;
esac],
[want_ipa=$have_ipa])

if $want_ipa ; then
   if $have_ipa ; then
     IPA_DEPFILES="$ipa_obj"
     extra_libraries="$extra_libraries libinproctrace.so"
   else
     AC_MSG_ERROR([inprocess agent not supported for this target])
   fi
fi

AC_SUBST(GDBSERVER_DEPFILES)
AC_SUBST(GDBSERVER_LIBS)
AC_SUBST(USE_THREAD_DB)
AC_SUBST(srv_xmlbuiltin)
AC_SUBST(srv_xmlfiles)
AC_SUBST(IPA_DEPFILES)
AC_SUBST(extra_libraries)

AC_OUTPUT(Makefile,
[case x$CONFIG_HEADERS in
xconfig.h:config.in)
echo > stamp-h ;;
esac
])
