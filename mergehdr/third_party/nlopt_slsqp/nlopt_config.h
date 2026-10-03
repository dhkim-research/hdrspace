#ifndef MERGEHDR_NLOPT_CONFIG_H
#define MERGEHDR_NLOPT_CONFIG_H

#define HAVE_COPYSIGN 1
#define HAVE_FPCLASSIFY 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_ISFINITE 1
#define HAVE_ISINF 1
#define HAVE_ISNAN 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME 1
#define HAVE_UNISTD_H 1
#define SIZEOF_UNSIGNED_INT 4
#define SIZEOF_UNSIGNED_LONG 8
#define TIME_WITH_SYS_TIME 1

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#  define THREADLOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define THREADLOCAL __thread
#else
#  define THREADLOCAL
#endif

#endif
