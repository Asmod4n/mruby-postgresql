#include "mrb_pq.h"

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
      if (unlikely(PQstatus(conn) != CONNECTION_OK)) {
        mrb_pq_handle_connection_error(mrb, self, conn);
      }
      mrb_data_init(self, conn, &mrb_PGconn_type);
#ifdef MRB_UTF8_STRING
      PQsetClientEncoding(conn, "UTF8");
#endif
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
mrb_PQfinish(mrb_state *mrb, mrb_value self)
{
  mrb_gc_PQfinish(mrb, DATA_PTR(self));
  mrb_data_init(self, NULL, NULL);

  return mrb_nil_value();
}

static mrb_value
mrb_PQreset(mrb_state *mrb, mrb_value self)
{
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  PQreset(conn);
  if (unlikely(PQstatus(conn) != CONNECTION_OK)) {
    mrb_pq_handle_connection_error(mrb, self, conn);
  }

  return self;
}

static mrb_value
mrb_PQsocket(mrb_state *mrb, mrb_value self)
{
  const PGconn *conn = (const PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  int socket = PQsocket(conn);
  if (unlikely(socket == -1)) {
    mrb_pq_handle_connection_error(mrb, self, conn);
  }
  return mrb_fixnum_value(socket);
}

static mrb_value
mrb_PQrequestCancel(mrb_state *mrb, mrb_value self)
{
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  int success = PQrequestCancel(conn);
  if (unlikely(!success)) {
    mrb_pq_handle_connection_error(mrb, self, conn);
  }

  return mrb_symbol_value(mrb_intern_lit(mrb, "cancel"));
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
mrb_pq_result_processor(mrb_state *mrb, struct RClass *pq_class, PGresult *res)
{
  mrb_value return_val;

  switch(PQresultStatus(res)) {
    case PGRES_EMPTY_QUERY: {
      return_val = mrb_exc_new_str(mrb, mrb_class_get_under(mrb, mrb_class_get_under(mrb, pq_class, "Result"), "EmptyQueryError"), mrb_str_new_cstr(mrb, PQresultErrorMessage(res)));
    } break;
    case PGRES_BAD_RESPONSE: {
      return_val = mrb_exc_new_str(mrb, mrb_class_get_under(mrb, mrb_class_get_under(mrb, pq_class, "Result"), "BadResponseError"), mrb_str_new_cstr(mrb, PQresultErrorMessage(res)));
    } break;
    case PGRES_NONFATAL_ERROR: {
      return_val = mrb_exc_new_str(mrb, mrb_class_get_under(mrb, mrb_class_get_under(mrb, pq_class, "Result"), "NonFatalError"), mrb_str_new_cstr(mrb, PQresultErrorMessage(res)));
    } break;
    case PGRES_FATAL_ERROR: {
      return_val = mrb_exc_new_str(mrb, mrb_class_get_under(mrb, mrb_class_get_under(mrb, pq_class, "Result"), "FatalError"), mrb_str_new_cstr(mrb, PQresultErrorMessage(res)));
    } break;
    default: {
      return_val = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, mrb_class_get_under(mrb, pq_class, "Result")));
      mrb_iv_set(mrb, return_val, mrb_intern_lit(mrb, "@status"), mrb_fixnum_value(PQresultStatus(res)));
    }
  }

  mrb_data_init(return_val, res, &mrb_PGresult_type);

  return return_val;
}

static void
mrb_pq_consume_each_row(mrb_state *mrb, mrb_value self, PGconn *conn, mrb_value block)
{
  int arena_index = mrb_gc_arena_save(mrb);
  struct mrb_jmpbuf* prev_jmp = mrb->jmp;
  struct RClass *pq_class = mrb_obj_class(mrb, self);
  struct mrb_jmpbuf c_jmp;

  PQsetSingleRowMode(conn);
  PGresult *res = PQgetResult(conn);

  MRB_TRY(&c_jmp)
  {
    mrb->jmp = &c_jmp;
    while (res) {
      if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        mrb_value ret = mrb_yield(mrb, block, mrb_pq_result_processor(mrb, pq_class, res));
        mrb_gc_arena_restore(mrb, arena_index);
        if (mrb_symbol_p(ret) && mrb_symbol(ret) == mrb_intern_lit(mrb, "cancel")) {
          while ((res = PQgetResult(conn))) {
            PQclear(res);
          }
          break;
        }
      } else {
        PQclear(res);
      }
      res = PQgetResult(conn);
    }
    mrb->jmp = prev_jmp;
  }
  MRB_CATCH(&c_jmp)
  {
    mrb->jmp = prev_jmp;
    while ((res = PQgetResult(conn))) {
      PQclear(res);
    }
    MRB_THROW(mrb->jmp);
  }
  MRB_END_EXC(&c_jmp);
}

static mrb_value
mrb_PQexec(mrb_state *mrb, mrb_value self)
{
  const char *command;
  mrb_value *paramValues_val = NULL;
  mrb_int nParams = 0;
  mrb_value block = mrb_nil_value();
  mrb_get_args(mrb, "z|*&", &command, &paramValues_val, &nParams, &block);
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  if (mrb_type(block) == MRB_TT_PROC) {
    int success = FALSE;
    errno = 0;
    if (nParams) {
      const char *paramValues[nParams];
      for (mrb_int i = 0; i < nParams; i++) {
        paramValues[i] = mrb_pq_encode_text_value(mrb, paramValues_val[i]);
      }
      success = PQsendQueryParams(conn, command, nParams, NULL, paramValues, NULL, NULL, 0);
    } else {
      success = PQsendQuery(conn, command);
    }
    if (likely(success)) {
      mrb_pq_consume_each_row(mrb, self, conn, block);
    } else {
      mrb_pq_handle_connection_error(mrb, self, conn);
    }
  } else {
    PGresult *res = NULL;
    errno = 0;
    if (nParams) {
      const char *paramValues[nParams];
      for (mrb_int i = 0; i < nParams; i++) {
        paramValues[i] = mrb_pq_encode_text_value(mrb, paramValues_val[i]);
      }
      res = PQexecParams(conn, command, nParams, NULL, paramValues, NULL, NULL, 0);
    } else {
      res = PQexec(conn, command);
    }
    if (likely(res)) {
      return mrb_pq_result_processor(mrb, mrb_obj_class(mrb, self), res);
    } else {
      mrb_sys_fail(mrb, PQresultErrorMessage(res));
    }
  }

  return self;
}

static mrb_value
mrb_PQprepare(mrb_state *mrb, mrb_value self)
{
  const char *stmtName, *query;
  mrb_get_args(mrb, "zz", &stmtName, &query);
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  PGresult *res = NULL;
  errno = 0;
  res = PQprepare(conn, stmtName, query, 0, NULL);
  if (likely(res)) {
    return mrb_pq_result_processor(mrb, mrb_obj_class(mrb, self), res);
  } else {
    mrb_sys_fail(mrb, PQresultErrorMessage(res));
  }

  return self;
}

static mrb_value
mrb_PQexecPrepared(mrb_state *mrb, mrb_value self)
{
  const char *stmtName;
  mrb_value *paramValues_val = NULL;
  mrb_int nParams = 0;
  mrb_value block = mrb_nil_value();
  mrb_get_args(mrb, "z|*&", &stmtName, &paramValues_val, &nParams, &block);
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  if (mrb_type(block) == MRB_TT_PROC) {
    int success = FALSE;
    errno = 0;
    if (nParams) {
      const char *paramValues[nParams];
      for (mrb_int i = 0; i < nParams; i++) {
        paramValues[i] = mrb_pq_encode_text_value(mrb, paramValues_val[i]);
      }
      success = PQsendQueryPrepared(conn, stmtName, nParams, paramValues, NULL, NULL, 0);
    } else {
      success = PQsendQueryPrepared(conn, stmtName, nParams, NULL, NULL, NULL, 0);
    }
    if (likely(success)) {
      mrb_pq_consume_each_row(mrb, self, conn, block);
    } else {
      mrb_pq_handle_connection_error(mrb, self, conn);
    }
  } else {
    PGresult *res = NULL;
    errno = 0;
    if (nParams) {
      const char *paramValues[nParams];
      for (mrb_int i = 0; i < nParams; i++) {
        paramValues[i] = mrb_pq_encode_text_value(mrb, paramValues_val[i]);
      }
      res = PQexecPrepared(conn, stmtName, nParams, paramValues, NULL, NULL, 0);
    } else {
      res = PQexecPrepared(conn, stmtName, nParams, NULL, NULL, NULL, 0);
    }
    if (likely(res)) {
      return mrb_pq_result_processor(mrb, mrb_obj_class(mrb, self), res);
    } else {
      mrb_sys_fail(mrb, PQresultErrorMessage(res));
    }
  }

  return self;

}

static mrb_value
mrb_PQdescribePrepared(mrb_state *mrb, mrb_value self)
{
  const char *stmtName = NULL;
  mrb_get_args(mrb, "|z!", &stmtName);
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  PGresult *res = PQdescribePrepared(conn, stmtName);
  if (likely(res)) {
    return mrb_pq_result_processor(mrb, mrb_obj_class(mrb, self), res);
  } else {
    mrb_sys_fail(mrb, PQresultErrorMessage(res));
  }

  return self;
}

static mrb_value
mrb_PQdescribePortal(mrb_state *mrb, mrb_value self)
{
  const char *portalName = NULL;
  mrb_get_args(mrb, "|z!", &portalName);
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  PGresult *res = PQdescribePortal(conn, portalName);
  if (likely(res)) {
    return mrb_pq_result_processor(mrb, mrb_obj_class(mrb, self), res);
  } else {
    mrb_sys_fail(mrb, PQresultErrorMessage(res));
  }

  return self;
}

static void
mrb_PQnoticeReceiver(void *arg_, const PGresult *res)
{
  mrb_PQnoticeReceiver_arg *arg = (mrb_PQnoticeReceiver_arg *) arg_;
  int arena_index = mrb_gc_arena_save(arg->mrb);
  mrb_yield(arg->mrb, arg->block, mrb_pq_result_processor(arg->mrb, arg->pq_class, res));
  mrb_gc_arena_restore(arg->mrb, arena_index);
}

static mrb_value
mrb_PQsetNoticeReceiver(mrb_state *mrb, mrb_value self)
{
  mrb_value block = mrb_nil_value();
  mrb_get_args(mrb, "&", &block);
  if (mrb_nil_p(block)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  if (mrb_type(block) != MRB_TT_PROC) {
    mrb_raise(mrb, E_TYPE_ERROR, "not a block");
  }
  PGconn *conn = (PGconn *) DATA_PTR(self);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  struct RClass *pq_class = mrb_obj_class(mrb, self);
  mrb_PQnoticeReceiver_arg *arg;
  struct RData *notice_receiver_data;
  Data_Make_Struct(mrb, mrb_class_get_under(mrb, pq_class, "NoticeReceiver"), mrb_PQnoticeReceiver_arg, &mrb_PQnoticeReceiver_type, arg, notice_receiver_data);
  arg->mrb = mrb;
  arg->pq_class = pq_class;
  arg->block = block;
  mrb_value notice_receiver = mrb_obj_value(notice_receiver_data);
  mrb_iv_set(mrb, notice_receiver, mrb_intern_lit(mrb, "block"), block);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "notice_receiver"), notice_receiver);

  PQsetNoticeReceiver(conn, mrb_PQnoticeReceiver, arg);

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
  if (fnumber != -1) {
    return mrb_fixnum_value(fnumber);
  } else {
    return mrb_nil_value();
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

  return mrb_fixnum_value(PQfformat((const PGresult *) DATA_PTR(self), (int) column_number));
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
        goto def;
      }
    } break;
    case 142: {
      if (mrb_class_defined(mrb, "XML")) {
        return mrb_funcall(mrb, mrb_obj_value(mrb_module_get(mrb, "XML")), "parse", 1, mrb_str_new_static(mrb, value, PQgetlength(result, row_number, column_number)));
      } else {
        goto def;
      }
    } break;
    case 700: { // float
      return mrb_float_value(mrb, strtof(value, NULL));
    } break;
    case 701: { // double
      return mrb_float_value(mrb, strtod(value, NULL));
    } break;
    default: {
def:
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
    if (PQgetisnull(result, (int) row_number, (int) column_number)) {
      return mrb_symbol_value(mrb_intern_lit(mrb, "NULL"));
    } else if (PQfformat(result, (int) row_number, (int) column_number) == 0) {
      return mrb_pq_decode_text_value(mrb, result, (int) row_number, (int) column_number, value);
    } else {
      return mrb_str_new(mrb, value, PQgetlength(result, (int) row_number, (int) column_number));
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

static mrb_value
mrb_PQnparams(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(PQnparams((const PGresult *) DATA_PTR(self)));
}

static mrb_value
mrb_PQparamtype(mrb_state *mrb, mrb_value self)
{
  mrb_int param_number;
  mrb_get_args(mrb, "i", &param_number);
  mrb_assert_int_fit(mrb_int, param_number, int, INT_MAX);

  return mrb_fixnum_value(PQparamtype((const PGresult *) DATA_PTR(self), (int) param_number));
}

static mrb_value
mrb_PQresultErrorField(mrb_state *mrb, mrb_value self)
{
  mrb_int fieldcode;
  mrb_get_args(mrb, "i", &fieldcode);
  mrb_assert_int_fit(mrb_int, fieldcode, int, INT_MAX);

  char *field = PQresultErrorField((const PGresult *) DATA_PTR(self), (int) fieldcode);
  if (field) {
    return mrb_str_new_cstr(mrb, field);
  } else {
    return mrb_nil_value();
  }
}

void
mrb_mruby_postgresql_gem_init(mrb_state *mrb)
{
  struct RClass *pq_class, *pq_error_class, *pq_result_mixins, *pq_result_class, *pq_result_error_class, *pq_notice_processor_class;
  pq_class = mrb_define_class(mrb, "Pq", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_class, MRB_TT_DATA);
  pq_error_class = mrb_define_class_under(mrb, pq_class, "Error", E_RUNTIME_ERROR);
  mrb_define_class_under(mrb, pq_class, "ConnectionError", pq_error_class);
  mrb_define_method(mrb, pq_class, "initialize",  mrb_PQconnectdb, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, pq_class, "finish",  mrb_PQfinish, MRB_ARGS_NONE());
  mrb_define_alias (mrb, pq_class, "close", "finish");
  mrb_define_method(mrb, pq_class, "exec",  mrb_PQexec, MRB_ARGS_REQ(1)|MRB_ARGS_REST()|MRB_ARGS_BLOCK());
  mrb_define_method(mrb, pq_class, "_prepare",  mrb_PQprepare, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_class, "exec_prepared",  mrb_PQexecPrepared, MRB_ARGS_REQ(1)|MRB_ARGS_REST()|MRB_ARGS_BLOCK());
  mrb_define_method(mrb, pq_class, "describe_prepared",  mrb_PQdescribePrepared, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, pq_class, "describe_portal",  mrb_PQdescribePortal, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, pq_class, "reset",  mrb_PQreset, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_class, "cancel",  mrb_PQrequestCancel, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_class, "socket",  mrb_PQsocket, MRB_ARGS_NONE());
  mrb_define_alias (mrb, pq_class, "to_i", "socket");
  mrb_define_method(mrb, pq_class, "notice_receiver",  mrb_PQsetNoticeReceiver, MRB_ARGS_BLOCK());
  pq_notice_processor_class = mrb_define_class_under(mrb, pq_class, "NoticeReceiver", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_notice_processor_class, MRB_TT_DATA);
  pq_result_mixins = mrb_define_module_under(mrb, pq_class, "ResultMixins");
  mrb_define_const(mrb, pq_result_mixins, "EMPTY_QUERY", mrb_fixnum_value(PGRES_EMPTY_QUERY));
  mrb_define_const(mrb, pq_result_mixins, "COMMAND_OK", mrb_fixnum_value(PGRES_COMMAND_OK));
  mrb_define_const(mrb, pq_result_mixins, "TUPLES_OK", mrb_fixnum_value(PGRES_TUPLES_OK));
  mrb_define_const(mrb, pq_result_mixins, "COPY_OUT", mrb_fixnum_value(PGRES_COPY_OUT));
  mrb_define_const(mrb, pq_result_mixins, "COPY_IN", mrb_fixnum_value(PGRES_COPY_IN));
  mrb_define_const(mrb, pq_result_mixins, "BAD_RESPONSE", mrb_fixnum_value(PGRES_BAD_RESPONSE));
  mrb_define_const(mrb, pq_result_mixins, "NONFATAL_ERROR", mrb_fixnum_value(PGRES_NONFATAL_ERROR));
  mrb_define_const(mrb, pq_result_mixins, "FATAL_ERROR", mrb_fixnum_value(PGRES_FATAL_ERROR));
  mrb_define_const(mrb, pq_result_mixins, "COPY_BOTH", mrb_fixnum_value(PGRES_COPY_BOTH));
  mrb_define_const(mrb, pq_result_mixins, "SINGLE_TUPLE", mrb_fixnum_value(PGRES_SINGLE_TUPLE));
  mrb_define_method(mrb, pq_result_mixins, "ntuples", mrb_PQntuples, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_mixins, "nfields", mrb_PQnfields, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_mixins, "fname", mrb_PQfname, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_mixins, "fnumber", mrb_PQfnumber, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_mixins, "ftable", mrb_PQftable, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_mixins, "ftablecol", mrb_PQftablecol, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_mixins, "fformat", mrb_PQfformat, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_mixins, "getvalue", mrb_PQgetvalue, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_result_mixins, "getisnull", mrb_PQgetisnull, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, pq_result_mixins, "nparams", mrb_PQnparams, MRB_ARGS_NONE());
  mrb_define_method(mrb, pq_result_mixins, "paramtype", mrb_PQparamtype, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, pq_result_mixins, "ftype", mrb_PQftype, MRB_ARGS_REQ(1));
  pq_result_class = mrb_define_class_under(mrb, pq_class, "Result", mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_result_class, MRB_TT_DATA);
  mrb_include_module(mrb, pq_result_class, pq_result_mixins);
  pq_result_error_class = mrb_define_class_under(mrb, pq_result_class, "Error", pq_error_class);
  MRB_SET_INSTANCE_TT(pq_result_error_class, MRB_TT_DATA);
  mrb_include_module(mrb, pq_result_error_class, pq_result_mixins);
  mrb_define_method(mrb, pq_result_error_class, "field", mrb_PQresultErrorField, MRB_ARGS_REQ(1));
  mrb_define_const(mrb, pq_result_error_class, "SEVERITY", mrb_fixnum_value(PG_DIAG_SEVERITY));
  mrb_define_const(mrb, pq_result_error_class, "SQLSTATE", mrb_fixnum_value(PG_DIAG_SQLSTATE));
  mrb_define_const(mrb, pq_result_error_class, "MESSAGE_PRIMARY", mrb_fixnum_value(PG_DIAG_MESSAGE_PRIMARY));
  mrb_define_const(mrb, pq_result_error_class, "MESSAGE_DETAIL", mrb_fixnum_value(PG_DIAG_MESSAGE_DETAIL));
  mrb_define_const(mrb, pq_result_error_class, "MESSAGE_HINT", mrb_fixnum_value(PG_DIAG_MESSAGE_HINT));
  mrb_define_const(mrb, pq_result_error_class, "STATEMENT_POSITION", mrb_fixnum_value(PG_DIAG_STATEMENT_POSITION));
  mrb_define_const(mrb, pq_result_error_class, "CONTEXT", mrb_fixnum_value(PG_DIAG_CONTEXT));
  mrb_define_const(mrb, pq_result_error_class, "SOURCE_FILE", mrb_fixnum_value(PG_DIAG_SOURCE_FILE));
  mrb_define_const(mrb, pq_result_error_class, "SOURCE_LINE", mrb_fixnum_value(PG_DIAG_SOURCE_LINE));
  mrb_define_const(mrb, pq_result_error_class, "SOURCE_FUNCTION", mrb_fixnum_value(PG_DIAG_SOURCE_FUNCTION));
#ifdef PG_DIAG_SEVERITY_NONLOCALIZED
  mrb_define_const(mrb, pq_result_error_class, "SEVERITY_NONLOCALIZED", mrb_fixnum_value(PG_DIAG_SEVERITY_NONLOCALIZED));
#endif
#ifdef PG_DIAG_INTERNAL_POSITION
  mrb_define_const(mrb, pq_result_error_class, "INTERNAL_POSITION", mrb_fixnum_value(PG_DIAG_INTERNAL_POSITION));
#endif
#ifdef PG_DIAG_INTERNAL_QUERY
  mrb_define_const(mrb, pq_result_error_class, "INTERNAL_QUERY", mrb_fixnum_value(PG_DIAG_INTERNAL_QUERY));
#endif
#ifdef PG_DIAG_SCHEMA_NAME
  mrb_define_const(mrb, pq_result_error_class, "SCHEMA_NAME", mrb_fixnum_value(PG_DIAG_SCHEMA_NAME));
#endif
#ifdef PG_DIAG_TABLE_NAME
  mrb_define_const(mrb, pq_result_error_class, "TABLE_NAME", mrb_fixnum_value(PG_DIAG_TABLE_NAME));
#endif
#ifdef PG_DIAG_COLUMN_NAME
  mrb_define_const(mrb, pq_result_error_class, "COLUMN_NAME", mrb_fixnum_value(PG_DIAG_COLUMN_NAME));
#endif
#ifdef PG_DIAG_DATATYPE_NAME
  mrb_define_const(mrb, pq_result_error_class, "DATATYPE_NAME", mrb_fixnum_value(PG_DIAG_DATATYPE_NAME));
#endif
#ifdef PG_DIAG_CONSTRAINT_NAME
  mrb_define_const(mrb, pq_result_error_class, "CONSTRAINT_NAME", mrb_fixnum_value(PG_DIAG_CONSTRAINT_NAME));
#endif
}

void mrb_mruby_postgresql_gem_final(mrb_state *mrb) {}
