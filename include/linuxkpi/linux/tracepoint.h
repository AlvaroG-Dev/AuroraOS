/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Tracepoints: la capa no tiene ftrace. TRACE_EVENT y compania generan una
 * trace_<nombre>() vacia (y trace_<nombre>_enabled() falso) para que los
 * ficheros trace.h de Linux (drm_trace.h ...) compilen tal cual.
 */
#ifndef _LKPI_LINUX_TRACEPOINT_H
#define _LKPI_LINUX_TRACEPOINT_H

#include <linux/types.h>
#include <linux/stringify.h>

#define TP_PROTO(args...)	args
#define TP_ARGS(args...)	args
#define TP_STRUCT__entry(args...)
#define TP_fast_assign(args...)
#define TP_printk(fmt, args...)
#define TP_CONDITION(args...)	args
#define __field(t, n)
#define __array(t, n, l)
#define __string(n, s)
#define __get_str(n) ""
#define __entry (&(int){0})
#define __print_flags(args...) 0
#define __print_symbolic(args...) 0

#define __LKPI_TP_NOARGS_void
#define DECLARE_TRACE(name, proto, args)				\
	static inline void trace_##name(proto) { }			\
	static inline bool trace_##name##_enabled(void) { return false; }

#define TRACE_EVENT(name, proto, args, tstruct, assign, print)		\
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(template, name, proto, args)			\
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_PRINT(template, name, proto, args, print)		\
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define PARAMS(args...) args

#define EXPORT_TRACEPOINT_SYMBOL(name)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name)
#define DEFINE_TRACE(name)

#endif
