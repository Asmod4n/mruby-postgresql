#include <libpq-fe.h>
#include <mruby.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/array.h>
#include <mruby/string.h>
#include <mruby/class.h>
#include <mruby/error.h>
#include <mruby/variable.h>
#include <mruby/throw.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#if (MRB_INT_BIT < 64)
#error "mruby integer type must be 64 bit"
#endif
#ifdef MRB_USE_FLOAT
#error "mruby musn't be compiled with MRB_USE_FLOAT"
#endif
#ifdef MRB_WITHOUT_FLOAT
#error "needs mruby with floating point"
#endif

#ifndef E_IO_ERROR
#define E_IO_ERROR (mrb_exc_get(mrb, "IOError"))
#endif

#if (__GNUC__ >= 3) || (__INTEL_COMPILER >= 800) || defined(__clang__)
# define likely(x) __builtin_expect(!!(x), 1)
# define unlikely(x) __builtin_expect(!!(x), 0)
#else
# define likely(x) (x)
# define unlikely(x) (x)
#endif

static void
mrb_gc_PQfinish(mrb_state *mrb, void *conn)
{
  PQfinish((PGconn *) conn);
}

static const struct mrb_data_type mrb_PGconn_type = {
  "$i_mrb_PGconn", mrb_gc_PQfinish
};

static void
mrb_gc_PQclear(mrb_state *mrb, void *res)
{
  PQclear((PGresult *) res);
}

static const struct mrb_data_type mrb_PGresult_type = {
  "$i_mrb_PGresult", mrb_gc_PQclear
};

typedef struct {
  mrb_state *mrb;
  struct RClass *pq_result_class;
  mrb_value block;
} mrb_PQnoticeReceiver_arg;

static const struct mrb_data_type mrb_PQnoticeReceiver_type = {
  "$i_mrb_PQnoticeReceiver", mrb_free
};

static void
mrb_pq_handle_connection_error(mrb_state *mrb, mrb_value self, const PGconn *conn)
{
  if (errno) mrb_sys_fail(mrb, PQerrorMessage(conn));
  mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "ConnectionError"), PQerrorMessage(conn));
}
