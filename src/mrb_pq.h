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
#include <mruby/dump.h>
#include <mruby/numeric.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>

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

MRB_INLINE mrb_value
mrb_pq_number_value(mrb_state *mrb, int64_t number)
{
  if (FIXABLE(number)) {
    return mrb_fixnum_value(number);
  } else {
#ifndef MRB_WITHOUT_FLOAT
    return mrb_float_value(mrb, number);
#else
    mrb_raise(mrb, E_RANGE_ERROR, "Number too big for Fixnum");
#endif
  }
}

static int mrb_pq_is_bigendian;

static const char *
mrb_pq_encode_fixnum(mrb_state *mrb, mrb_value value, Oid *paramType, int *paramLength)
{
  mrb_int number = mrb_fixnum(value);
  mrb_value str = mrb_str_new(mrb, NULL, sizeof(number));

#if (MRB_INT_BIT == 64)
  *paramType = 20;
#elif (MRB_INT_BIT == 32)
  *paramType = 23;
#elif (MRB_INT_BIT == 16)
  *paramType = 21;
#else
#error "mruby-postgresql: unknown MRB_INT_BIT found in <mruby/value.h>"
#endif
  *paramLength = sizeof(number);

  uint8_t *dst = (uint8_t *) RSTRING_PTR(str);
  if (mrb_pq_is_bigendian) {
    memcpy(dst, &number, sizeof(number));
  } else for (int i = sizeof(number) - 1;i >= 0; i--) {
    dst[i] = (uint8_t) number;
    number >>= 8;
  }

  return RSTRING_PTR(str);
}

#ifndef MRB_WITHOUT_FLOAT
#if (defined(MRB_USE_FLOAT) && MRB_INT_BIT != 32)||(MRB_INT_BIT != 64)
#error "mruby-postgresql: size of mrb_float and mrb_int differ, cannot pack floats. (define MRB_INT64 by default, MRB_INT32 when MRB_USE_FLOAT in <mrbconf.h>)"
#endif
static const char *
mrb_pq_encode_float(mrb_state *mrb, mrb_value value, Oid *paramType, int *paramLength)
{
  union {
    mrb_float f;
    mrb_int i;
  } swap;

  swap.f = mrb_float(value);
  mrb_value str = mrb_str_new(mrb, NULL, sizeof(swap));

#ifdef MRB_USE_FLOAT
  *paramType = 700;
#else
  *paramType = 701;
#endif
  *paramLength = sizeof(swap);

  uint8_t *dst = (uint8_t *) RSTRING_PTR(str);
  if (mrb_pq_is_bigendian) {
    memcpy(dst, &swap.i, sizeof(swap));
  } else for (int i = sizeof(swap) - 1;i >= 0; i--) {
    dst[i] = (uint8_t) swap.i;
    swap.i >>= 8;
  }

  return RSTRING_PTR(str);
}
#endif
