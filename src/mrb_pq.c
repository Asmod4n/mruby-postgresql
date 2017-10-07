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

static mrb_value
mrb_PQconnectdb(mrb_state *mrb, mrb_value self)
{
  const char *conninfo = "";
  mrb_get_args(mrb, "|z", &conninfo);

  struct mrb_jmpbuf* prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  PGconn *conn;
  MRB_TRY(&c_jmp)
  {
      mrb->jmp = &c_jmp;
      errno = 0;
      conn = PQconnectdb(conninfo);
      if (PQstatus(conn) != CONNECTION_OK) {
        if (errno) mrb_sys_fail(mrb, PQerrorMessage(conn));
        mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Error"), PQerrorMessage(conn));
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
    mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Error"), PQerrorMessage(conn));
  }

  return self;
}

static mrb_value
mrb_PQsocket(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(PQsocket((const PGconn *) DATA_PTR(self)));
}

static mrb_value
mrb_PQexecParams(mrb_state *mrb, mrb_value self)
{
  const char *command;
  mrb_value *paramValues_val = NULL;
  mrb_int nParams = 0;
  mrb_get_args(mrb, "z|*", &command, &paramValues_val, &nParams);

  PGresult *res;
  errno = 0;
  if (nParams) {
    const char *paramValues[nParams];
    for (mrb_int i = 0; i < nParams; i++) {
      switch(mrb_type(paramValues_val[i])) {
        case MRB_TT_TRUE:
          paramValues[i] = "t";
        break;
        case MRB_TT_FALSE: {
          if (!mrb_fixnum(paramValues_val[i])) {
            paramValues[i] = NULL;
          } else {
            paramValues[i] = "f";
          }
        } break;
        default:
          paramValues[i] = mrb_string_value_cstr(mrb, &paramValues_val[i]);
      }
    }
    res = PQexecParams((PGconn *) DATA_PTR(self), command, nParams, NULL, paramValues, NULL, NULL, 0);
  } else {
    res = PQexecParams((PGconn *) DATA_PTR(self), command, nParams, NULL, NULL, NULL, NULL, 0);
  }
  if (res) {
    struct mrb_jmpbuf* prev_jmp = mrb->jmp;
    struct mrb_jmpbuf c_jmp;

    MRB_TRY(&c_jmp)
    {
        mrb->jmp = &c_jmp;
        switch(PQresultStatus(res)) {
          case PGRES_TUPLES_OK:
          case PGRES_SINGLE_TUPLE: {
            mrb_value res_val = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Result")));
            mrb_data_init(res_val, res, &mrb_PGresult_type);
            return res_val;
          } break;
          case PGRES_EMPTY_QUERY:
          case PGRES_BAD_RESPONSE:
          case PGRES_NONFATAL_ERROR:
          case PGRES_FATAL_ERROR: {
            if (errno) mrb_sys_fail(mrb, PQresultErrorMessage(res));
            mrb_raise(mrb, mrb_class_get_under(mrb, mrb_obj_class(mrb, self), "Error"), PQresultErrorMessage(res));
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

    return self;
  } else if (errno) {
    mrb_sys_fail(mrb, PQresultErrorMessage(res));
  } else {
    return mrb_nil_value();
  }

  return self;
}

static mrb_value
mrb_PQsetSingleRowMode(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(PQsetSingleRowMode((PGconn *) DATA_PTR(self)));
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

  char *fname = PQfname((const PGresult *) DATA_PTR(self), column_number);
  if (fname) {
    return mrb_str_new_cstr(mrb, fname);
  } else {
    mrb_raise(mrb, E_RANGE_ERROR, "column_number is out of range");
  }
}

static mrb_value
mrb_PQfnumber(mrb_state *mrb, mrb_value self)
{
  const char *column_name;
  mrb_get_args(mrb, "z", &column_name);

  return mrb_fixnum_value(PQfnumber((const PGresult *) DATA_PTR(self), column_name));
}

static mrb_value
mrb_PQftable(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  Oid foo = PQftable((const PGresult *) DATA_PTR(self), column_number);
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

  int foo = PQftablecol((const PGresult *) DATA_PTR(self), column_number);
  if (foo == 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Column number is out of range, or the specified column is not a simple reference to a table column, or using pre-3.0 protocol");
  }

  return mrb_fixnum_value(foo);
}

static mrb_value
mrb_PQgetvalue(mrb_state *mrb, mrb_value self)
{
  mrb_int row_number, column_number;
  mrb_get_args(mrb, "ii", &row_number, &column_number);
  mrb_assert_int_fit(mrb_int, row_number, int, INT_MAX);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);
  const PGresult *result = (const PGresult *) DATA_PTR(self);

  errno = 0;
  char *value = PQgetvalue(result, row_number, column_number);
  if (value) {
    if (strlen(value) == 0 && PQgetisnull(result, row_number, column_number)) {
      return mrb_symbol_value(mrb_intern_lit(mrb, "NULL"));
    } else {
      switch(PQftype(result, column_number)) {
        case 16: { // boolean
          return mrb_bool_value(value[0] == 't');
        } break;
        case 20: // Integers
        case 21:
        case 23:
        case 26: {
          return mrb_str_to_inum(mrb, mrb_str_new_static(mrb, value, PQgetlength(result, row_number, column_number)), 0, TRUE);
        } break;
        case 700: // Floats
        case 701: {
          return mrb_float_value(mrb, mrb_str_to_dbl(mrb, mrb_str_new_static(mrb, value, PQgetlength(result, row_number, column_number)), TRUE));
        } break;
        default: {
          return mrb_str_new(mrb, value, PQgetlength(result, row_number, column_number));
        }
      }
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

  return mrb_bool_value(PQgetisnull((const PGresult *) DATA_PTR(self), row_number, column_number));
}

static mrb_value
mrb_PQftype(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);

  return mrb_fixnum_value(PQftype((const PGresult *) DATA_PTR(self), column_number));
}

void
mrb_mruby_postgresql_gem_init(mrb_state *mrb)
{
  struct RClass *pq_class, *pq_result_class;
  pq_class = mrb_define_class(mrb, "Pq", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_class, MRB_TT_DATA);
  mrb_define_method(mrb, pq_class, "initialize",  mrb_PQconnectdb, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, pq_class, "exec",  mrb_PQexecParams, MRB_ARGS_REQ(1)|MRB_ARGS_REST());
  mrb_define_method(mrb, pq_class, "reset",  mrb_PQreset, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_class, "socket",  mrb_PQsocket, MRB_ARGS_NONE());
  mrb_define_alias (mrb, pq_class, "to_i", "socket");
  mrb_define_method(mrb, pq_class, "set_single_row_mode",  mrb_PQsetSingleRowMode, MRB_ARGS_NONE());
  pq_result_class = mrb_define_class_under(mrb, pq_class, "Result", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_result_class, MRB_TT_DATA);
  mrb_define_method(mrb, pq_result_class, "ntuples", mrb_PQntuples, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_class, "nfields", mrb_PQnfields, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_class, "fname", mrb_PQfname, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "fnumber", mrb_PQfnumber, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "ftable", mrb_PQftable, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "ftablecol", mrb_PQftablecol, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_class, "getvalue", mrb_PQgetvalue, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_result_class, "getisnull", mrb_PQgetisnull, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_result_class, "ftype", mrb_PQftype, MRB_ARGS_REQ(1));
}

void mrb_mruby_postgresql_gem_final(mrb_state *mrb) {}
