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
  mrb_value block;
} mrb_PQnoticeProcessor_arg;

static const struct mrb_data_type mrb_PQnoticeProcessor_type = {
  "$i_mrb_PQnoticeProcessor", mrb_free
};

static mrb_value
mrb_PQconnectdb(mrb_state *mrb, mrb_value self)
{
  const char *conninfo = "";
  mrb_get_args(mrb, "|z", &conninfo);

  struct mrb_jmpbuf* prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  PGconn *conn = NULL;
  MRB_TRY(&c_jmp)
  {
      mrb->jmp = &c_jmp;
      errno = 0;
      conn = PQconnectdb(conninfo);
      if (PQstatus(conn) != CONNECTION_OK) {
        if (errno) mrb_sys_fail(mrb, PQerrorMessage(conn));
        mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "ConnectionError"), PQerrorMessage(conn));
      }
      mrb_data_init(self, conn, &mrb_PGconn_type);
      mrb->jmp = prev_jmp;
  }
  MRB_CATCH(&c_jmp)
  {
      mrb->jmp = prev_jmp;
      PQfinish(conn);
      MRB_THROW(mrb->jmp);
  }
  MRB_END_EXC(&c_jmp);

  return self;
}

static mrb_value
mrb_PQreset(mrb_state *mrb, mrb_value self)
{
  errno = 0;
  PGconn *conn = (PGconn *) DATA_PTR(self);
  PQreset(conn);
  if (PQstatus(conn) != CONNECTION_OK) {
    if (errno) mrb_sys_fail(mrb, PQerrorMessage(conn));
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "ConnectionError"), PQerrorMessage(conn));
  }

  return self;
}

static mrb_value
mrb_PQsocket(mrb_state *mrb, mrb_value self)
{
  const PGconn *conn = (const PGconn *) DATA_PTR(self);
  int socket = PQsocket(conn);
  if (socket == -1) {
    if (errno) mrb_sys_fail(mrb, PQerrorMessage(conn));
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "ConnectionError"), PQerrorMessage(conn));
  }
  return mrb_fixnum_value(socket);
}

MRB_INLINE const char *
mrb_pq_encode_text_value(mrb_state *mrb, mrb_value value)
{
  switch(mrb_type(value)) {
    case MRB_TT_FALSE: {
      if (!mrb_fixnum(value)) {
        return NULL;
      } else {
        return "f";
      }
    } break;
    case MRB_TT_TRUE: {
      return "t";
    } break;
    default: {
      return mrb_string_value_cstr(mrb, &value);
    }
  }
}

static mrb_value
mrb_pq_result_processor(mrb_state *mrb, mrb_value self, PGresult *res)
{
  struct mrb_jmpbuf* prev_jmp = mrb->jmp;
  mrb_value return_val = self;
  struct mrb_jmpbuf c_jmp;

  MRB_TRY(&c_jmp)
  {
      mrb->jmp = &c_jmp;
      switch(PQresultStatus(res)) {
        case PGRES_TUPLES_OK:
        case PGRES_SINGLE_TUPLE: {
          return_val = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Result")));
          mrb_data_init(return_val, res, &mrb_PGresult_type);
        } break;
        case PGRES_EMPTY_QUERY:
        case PGRES_BAD_RESPONSE:
        case PGRES_NONFATAL_ERROR:
        case PGRES_FATAL_ERROR: {
          if (errno) mrb_sys_fail(mrb, PQresultErrorMessage(res));
          mrb_raise(mrb, mrb_class_get_under(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Result"), "Error"), PQresultErrorMessage(res));
        } break;
        default: {
          PQclear(res);
        }
      }
      mrb->jmp = prev_jmp;
  }
  MRB_CATCH(&c_jmp)
  {
      mrb->jmp = prev_jmp;
      PQclear(res);
      MRB_THROW(mrb->jmp);
  }
  MRB_END_EXC(&c_jmp);

  return return_val;
}

static mrb_value
mrb_PQexecParams(mrb_state *mrb, mrb_value self)
{
  const char *command;
  mrb_value *paramValues_val = NULL;
  mrb_int nParams = 0;
  mrb_get_args(mrb, "z|*", &command, &paramValues_val, &nParams);

  PGresult *res = NULL;
  errno = 0;
  if (nParams) {
    const char *paramValues[nParams];
    for (mrb_int i = 0; i < nParams; i++) {
      paramValues[i] = mrb_pq_encode_text_value(mrb, paramValues_val[i]);
    }
    res = PQexecParams((PGconn *) DATA_PTR(self), command, nParams, NULL, paramValues, NULL, NULL, 0);
  } else {
    res = PQexecParams((PGconn *) DATA_PTR(self), command, nParams, NULL, NULL, NULL, NULL, 0);
  }
  if (res) {
    return mrb_pq_result_processor(mrb, self, res);
  } else {
    mrb_sys_fail(mrb, PQresultErrorMessage(res));
  }

  return self;
}

static void
mrb_PQnoticeProcessor(void *arg_, const char *message)
{
  mrb_PQnoticeProcessor_arg *arg = (mrb_PQnoticeProcessor_arg *) arg_;
  int arena_index = mrb_gc_arena_save(arg->mrb);
  mrb_yield(arg->mrb, arg->block, mrb_str_new_cstr(arg->mrb, message));
  mrb_gc_arena_restore(arg->mrb, arena_index);
}

static mrb_value
mrb_PQsetNoticeProcessor(mrb_state *mrb, mrb_value self)
{
  mrb_value block = mrb_nil_value();
  mrb_get_args(mrb, "&", &block);
  if (mrb_nil_p(block)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  if (mrb_type(block) != MRB_TT_PROC) {
    mrb_raise(mrb, E_TYPE_ERROR, "not a block");
  }

  mrb_value notice_processor = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "NoticeProcessor")));
  mrb_PQnoticeProcessor_arg *arg = (mrb_PQnoticeProcessor_arg *) mrb_realloc(mrb, DATA_PTR(notice_processor), sizeof(*arg));
  mrb_data_init(notice_processor, arg, &mrb_PQnoticeProcessor_type);
  arg->mrb = mrb;
  arg->block = block;
  mrb_iv_set(mrb, notice_processor, mrb_intern_lit(mrb, "block"), block);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "notice_processor"), notice_processor);

  PQsetNoticeProcessor((PGconn *) DATA_PTR(self), mrb_PQnoticeProcessor, arg);

  return self;
}

static mrb_value
mrb_PQntuples(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(PQntuples((const PGresult *) DATA_PTR(self)));
}

static mrb_value
mrb_PQnfields(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(PQnfields((const PGresult *) DATA_PTR(self)));
}

static mrb_value
mrb_PQfname(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  char *fname = PQfname((const PGresult *) DATA_PTR(self), (int) column_number);
  if (fname) {
    return mrb_str_new_cstr(mrb, fname);
  } else {
    return mrb_nil_value();
  }
}

static mrb_value
mrb_PQfnumber(mrb_state *mrb, mrb_value self)
{
  const char *column_name;
  mrb_get_args(mrb, "z", &column_name);

  int fnumber = PQfnumber((const PGresult *) DATA_PTR(self), column_name);
  if (fnumber == -1) {
    return mrb_nil_value();
  } else {
    return mrb_fixnum_value(fnumber);
  }
}

static mrb_value
mrb_PQftable(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  Oid foo = PQftable((const PGresult *) DATA_PTR(self), (int) column_number);
  if (foo == InvalidOid) {
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "InvalidOid"), "Column number is out of range, or the specified column is not a simple reference to a table column, or using pre-3.0 protocol");
  }

  return mrb_fixnum_value(foo);
}

static mrb_value
mrb_PQftablecol(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  int foo = PQftablecol((const PGresult *) DATA_PTR(self), (int) column_number);
  if (foo == 0) {
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Error"), "Column number is out of range, or the specified column is not a simple reference to a table column, or using pre-3.0 protocol");
  }

  return mrb_fixnum_value(foo);
}

static mrb_value
mrb_PQfformat(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  int format = PQfformat((const PGresult *) DATA_PTR(self), (int) column_number);
  switch (format) {
    case 0: {
      return mrb_symbol_value(mrb_intern_lit(mrb, "text"));
    } break;
    case 1: {
      return mrb_symbol_value(mrb_intern_lit(mrb, "binary"));
    } break;
    default: {
      return mrb_fixnum_value(format);
    }
  }
}

static mrb_value
mrb_PQftype(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  return mrb_fixnum_value(PQftype((const PGresult *) DATA_PTR(self), (int) column_number));
}

static mrb_value
mrb_pq_decode_text_value(mrb_state *mrb, const PGresult *result, int row_number, int column_number, char *value)
{
  switch(PQftype(result, column_number)) {
    case 16: { // bool
      return mrb_bool_value(value[0] == 't');
    } break;
    case 20: { // int64_t
      return mrb_fixnum_value(strtoll(value, NULL, 0));
    } break;
    case 21: // int16_t
    case 23: { // int32_t
      return mrb_fixnum_value(strtol(value, NULL, 0));
    } break;
    case 114:
    case 3802: {
      if (mrb_class_defined(mrb, "JSON")) {
        return mrb_funcall(mrb, mrb_obj_value(mrb_module_get(mrb, "JSON")), "parse", 1, mrb_str_new_static(mrb, value, PQgetlength(result, row_number, column_number)));
      } else {
        return mrb_str_new(mrb, value, PQgetlength(result, row_number, column_number));
      }
    } break;
    case 142: {
      if (mrb_class_defined(mrb, "XML")) {
        return mrb_funcall(mrb, mrb_obj_value(mrb_module_get(mrb, "XML")), "parse", 1, mrb_str_new_static(mrb, value, PQgetlength(result, row_number, column_number)));
      } else {
        return mrb_str_new(mrb, value, PQgetlength(result, row_number, column_number));
      }
    } break;
    case 700: { // float
      return mrb_float_value(mrb, strtof(value, NULL));
    } break;
    case 701: { // double
      return mrb_float_value(mrb, strtod(value, NULL));
    } break;
    default: {
      return mrb_str_new(mrb, value, PQgetlength(result, row_number, column_number));
    }
  }
}

static mrb_value
mrb_PQgetvalue(mrb_state *mrb, mrb_value self)
{
  mrb_int row_number, column_number;
  mrb_get_args(mrb, "ii", &row_number, &column_number);
  mrb_assert_int_fit(mrb_int, row_number, int, INT_MAX);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);
  const PGresult *result = (const PGresult *) DATA_PTR(self);

  char *value = PQgetvalue(result, (int) row_number, (int) column_number);
  if (value) {
    if (strlen(value) == 0 && PQgetisnull(result, (int) row_number, (int) column_number)) {
      return mrb_symbol_value(mrb_intern_lit(mrb, "NULL"));
    } else {
      return mrb_pq_decode_text_value(mrb, result, (int) row_number, (int) column_number, value);
    }
  } else {
    return mrb_nil_value();
  }
}

static mrb_value
mrb_PQgetisnull(mrb_state *mrb, mrb_value self)
{
  mrb_int row_number, column_number;
  mrb_get_args(mrb, "ii", &row_number, &column_number);
  mrb_assert_int_fit(mrb_int, row_number, int, INT_MAX);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  return mrb_bool_value(PQgetisnull((const PGresult *) DATA_PTR(self), (int) row_number, (int) column_number));
}

void
mrb_mruby_postgresql_gem_init(mrb_state *mrb)
{
  struct RClass *pq_class, *pq_result_class, *pq_notice_processor_class;
  pq_class = mrb_define_class(mrb, "Pq", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_class, MRB_TT_DATA);
  mrb_define_method(mrb, pq_class, "initialize",  mrb_PQconnectdb, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, pq_class, "exec",  mrb_PQexecParams, MRB_ARGS_REQ(1)|MRB_ARGS_REST());
  mrb_define_method(mrb, pq_class, "reset",  mrb_PQreset, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_class, "socket",  mrb_PQsocket, MRB_ARGS_NONE());
  mrb_define_alias (mrb, pq_class, "to_i", "socket");
  mrb_define_method(mrb, pq_class, "notice_processor",  mrb_PQsetNoticeProcessor, MRB_ARGS_BLOCK());
  pq_notice_processor_class = mrb_define_class_under(mrb, pq_class, "NoticeProcessor", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_notice_processor_class, MRB_TT_DATA);
  pq_result_class = mrb_define_class_under(mrb, pq_class, "Result", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_result_class, MRB_TT_DATA);
  mrb_define_method(mrb, pq_result_class, "ntuples", mrb_PQntuples, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_class, "nfields", mrb_PQnfields, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_class, "fname", mrb_PQfname, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "fnumber", mrb_PQfnumber, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "ftable", mrb_PQftable, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "ftablecol", mrb_PQftablecol, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "fformat", mrb_PQfformat, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "getvalue", mrb_PQgetvalue, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_result_class, "getisnull", mrb_PQgetisnull, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_result_class, "ftype", mrb_PQftype, MRB_ARGS_REQ(1));
}

void mrb_mruby_postgresql_gem_final(mrb_state *mrb) {}
