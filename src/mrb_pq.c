#include "mrb_pq.h"

static mrb_value
mrb_PQconnectdb(mrb_state *mrb, mrb_value self)
{
  const char *conninfo = "";
  mrb_get_args(mrb, "|z", &conninfo);

  errno = 0;
  PGconn *conn = PQconnectdb(conninfo);
  if (unlikely(PQstatus(conn) != CONNECTION_OK)) {
    /* Build the exception before mrb_data_init so the GC finalizer won't
       double-free conn. ConnectionError inherits from RuntimeError and is
       MRB_TT_OBJECT, so mrb_exc_new_str is correct here. */
    mrb_value err = mrb_exc_new_str(mrb,
      mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(ConnectionError)),
      mrb_str_new_cstr(mrb, PQerrorMessage(conn)));
    PQfinish(conn);
    mrb_exc_raise(mrb, err);
  }
  mrb_data_init(self, conn, &mrb_PGconn_type);
#ifdef MRB_UTF8_STRING
  PQsetClientEncoding(conn, "UTF8");
#endif

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
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
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
  const PGconn *conn = (const PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  int socket = PQsocket(conn);
  if (unlikely(socket == -1)) {
    mrb_pq_handle_connection_error(mrb, self, conn);
  }

  return mrb_int_value(mrb, socket);
}

static mrb_value
mrb_PQrequestCancel(mrb_state *mrb, mrb_value self)
{
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  int success = PQrequestCancel(conn);
  if (unlikely(!success)) {
    mrb_pq_handle_connection_error(mrb, self, conn);
  }

  return mrb_symbol_value(MRB_SYM(cancel));
}

static const char *
mrb_pq_encode_value(mrb_state *mrb, mrb_value value, Oid *paramType, int *paramLength, int *paramFormat)
{
  switch(mrb_type(value)) {
    case MRB_TT_FALSE: {
      if (!mrb_integer(value)) {
        *paramType = 0;
        *paramLength = 0;
        *paramFormat = 0;
        return NULL;
      } else {
        *paramType = 16;
        *paramLength = 1;
        *paramFormat = 0;
        return "f";
      }
    } break;
    case MRB_TT_TRUE: {
      *paramType = 16;
      *paramLength = 1;
      *paramFormat = 0;
      return "t";
    } break;
    case MRB_TT_INTEGER: {
      *paramFormat = 1;
      return mrb_pq_encode_integer(mrb, value, paramType, paramLength);
    } break;
#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT: {
      *paramFormat = 1;
      return mrb_pq_encode_float(mrb, value, paramType, paramLength);
    } break;
#endif
    default: {
      mrb_gc_protect(mrb, value);
      value = mrb_str_to_str(mrb, value);
      mrb_gc_protect(mrb, value);
      *paramType = 0;
      *paramLength = RSTRING_LEN(value);
      *paramFormat = 0;
      return RSTRING_PTR(value);
    }
  }
}

static mrb_value
mrb_pq_make_error_result(mrb_state *mrb, struct RClass *klass, PGresult *res)
{
  mrb_value obj = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, klass));
  mrb_gc_protect(mrb, obj);
  mrb_iv_set(mrb, obj, MRB_IVSYM(mesg), mrb_str_new_cstr(mrb, PQresultErrorMessage(res)));
  mrb_iv_set(mrb, obj, MRB_IVSYM(status), mrb_int_value(mrb, PQresultStatus(res)));
  mrb_data_init(obj, res, &mrb_PGresult_type);
  return obj;
}

static mrb_value
mrb_pq_result_processor(mrb_state *mrb, struct RClass *pq_result_class, PGresult *res)
{
  switch(PQresultStatus(res)) {
    case PGRES_EMPTY_QUERY:
      return mrb_pq_make_error_result(mrb,
        mrb_class_get_under_id(mrb, pq_result_class, MRB_SYM(EmptyQueryError)), res);
    case PGRES_BAD_RESPONSE:
      return mrb_pq_make_error_result(mrb,
        mrb_class_get_under_id(mrb, pq_result_class, MRB_SYM(BadResponseError)), res);
    case PGRES_NONFATAL_ERROR:
      return mrb_pq_make_error_result(mrb,
        mrb_class_get_under_id(mrb, pq_result_class, MRB_SYM(NonFatalError)), res);
    case PGRES_FATAL_ERROR:
      return mrb_pq_make_error_result(mrb,
        mrb_class_get_under_id(mrb, pq_result_class, MRB_SYM(FatalError)), res);
    default: {
      mrb_value obj = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, pq_result_class));
      mrb_iv_set(mrb, obj, MRB_IVSYM(status), mrb_int_value(mrb, PQresultStatus(res)));
      mrb_data_init(obj, res, &mrb_PGresult_type);
      return obj;
    }
  }
}

typedef struct {
  PGconn *conn;
  mrb_value block;
  mrb_value self;
  struct RClass *pq_result_class;
} mrb_pq_each_row_arg;

static mrb_value
mrb_pq_each_row_body(mrb_state *mrb, mrb_value arg_val)
{
  mrb_pq_each_row_arg *arg = (mrb_pq_each_row_arg *) mrb_cptr(arg_val);
  int arena_index = mrb_gc_arena_save(mrb);
  PGresult *res;

  while ((res = PQgetResult(arg->conn))) {
    if (PQntuples(res) == 0) {
      PQclear(res);
      continue;
    }
    mrb_value ret = mrb_yield(mrb, arg->block,
      mrb_pq_result_processor(mrb, arg->pq_result_class, res));
    mrb_gc_arena_restore(mrb, arena_index);
    if (mrb_symbol_p(ret) && mrb_symbol(ret) == MRB_SYM(cancel)) {
      while ((res = PQgetResult(arg->conn))) {
        PQclear(res);
      }
      break;
    }
  }
  return arg->self;
}

static mrb_value
mrb_pq_consume_each_row(mrb_state *mrb, mrb_value self, PGconn *conn, mrb_value block)
{
  PQsetSingleRowMode(conn);

  mrb_pq_each_row_arg arg = {
    .conn = conn,
    .block = block,
    .self = self,
    .pq_result_class = mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Result)),
  };

  mrb_bool exc = FALSE;
  mrb_value result = mrb_protect(mrb, mrb_pq_each_row_body, mrb_cptr_value(mrb, &arg), &exc);

  if (exc) {
    PQrequestCancel(conn);
    PGresult *res;
    while ((res = PQgetResult(conn))) {
      PQclear(res);
    }
    mrb_exc_raise(mrb, result);
  }

  return self;
}

static mrb_bool
mrb_pq_encode_params(mrb_state *mrb, mrb_value *paramValues_val, mrb_int nParams,
  Oid **paramTypes, const char ***paramValues, int **paramLengths, int **paramFormats)
{
  if (nParams <= 0) return FALSE;

  if ((size_t)nParams > SIZE_MAX / sizeof(char *)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "too many parameters");
  }
  mrb_value paramTypes_val = mrb_str_new_capa(mrb, nParams * sizeof(Oid));
  mrb_gc_protect(mrb, paramTypes_val);
  mrb_value paramValue_val = mrb_str_new_capa(mrb, nParams * sizeof(char *));
  mrb_gc_protect(mrb, paramValue_val);
  mrb_value paramLengths_val = mrb_str_new_capa(mrb, nParams * sizeof(int));
  mrb_gc_protect(mrb, paramLengths_val);
  mrb_value paramFormats_val = mrb_str_new_capa(mrb, nParams * sizeof(int));
  mrb_gc_protect(mrb, paramFormats_val);

  *paramTypes   = (Oid *)        RSTRING_PTR(paramTypes_val);
  *paramValues  = (const char **) RSTRING_PTR(paramValue_val);
  *paramLengths = (int *)        RSTRING_PTR(paramLengths_val);
  *paramFormats = (int *)        RSTRING_PTR(paramFormats_val);

  for (mrb_int i = 0; i < nParams; i++) {
    (*paramValues)[i] = mrb_pq_encode_value(mrb, paramValues_val[i],
      &(*paramTypes)[i], &(*paramLengths)[i], &(*paramFormats)[i]);
  }
  return TRUE;
}

static mrb_value
mrb_PQexec(mrb_state *mrb, mrb_value self)
{
  const char *command;
  mrb_value *paramValues_val = NULL;
  mrb_int nParams = 0;
  mrb_value block = mrb_nil_value();
  mrb_get_args(mrb, "z|*&", &command, &paramValues_val, &nParams, &block);
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  Oid        *paramTypes   = NULL;
  const char **paramValues = NULL;
  int        *paramLengths = NULL;
  int        *paramFormats = NULL;
  int arena_index = mrb_gc_arena_save(mrb);
  mrb_bool has_params = mrb_pq_encode_params(mrb, paramValues_val, nParams,
    &paramTypes, &paramValues, &paramLengths, &paramFormats);

  errno = 0;
  if (mrb_type(block) == MRB_TT_PROC) {
    int success = has_params
      ? PQsendQueryParams(conn, command, nParams, paramTypes, paramValues, paramLengths, paramFormats, 0)
      : PQsendQuery(conn, command);
    mrb_gc_arena_restore(mrb, arena_index);
    if (likely(success)) {
      return mrb_pq_consume_each_row(mrb, self, conn, block);
    } else {
      mrb_pq_handle_connection_error(mrb, self, conn);
    }
  } else {
    PGresult *res = has_params
      ? PQexecParams(conn, command, nParams, paramTypes, paramValues, paramLengths, paramFormats, 0)
      : PQexec(conn, command);
    mrb_gc_arena_restore(mrb, arena_index);
    if (likely(res)) {
      return mrb_pq_result_processor(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Result)), res);
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
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  PGresult *res = PQprepare(conn, stmtName, query, 0, NULL);
  if (likely(res)) {
    return mrb_pq_result_processor(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Result)), res);
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
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  Oid        *paramTypes   = NULL;
  const char **paramValues = NULL;
  int        *paramLengths = NULL;
  int        *paramFormats = NULL;
  int arena_index = mrb_gc_arena_save(mrb);
  mrb_bool has_params = mrb_pq_encode_params(mrb, paramValues_val, nParams,
    &paramTypes, &paramValues, &paramLengths, &paramFormats);

  errno = 0;
  if (mrb_type(block) == MRB_TT_PROC) {
    int success = has_params
      ? PQsendQueryPrepared(conn, stmtName, nParams, paramValues, paramLengths, paramFormats, 0)
      : PQsendQueryPrepared(conn, stmtName, 0, NULL, NULL, NULL, 0);
    mrb_gc_arena_restore(mrb, arena_index);
    if (likely(success)) {
      return mrb_pq_consume_each_row(mrb, self, conn, block);
    } else {
      mrb_pq_handle_connection_error(mrb, self, conn);
    }
  } else {
    PGresult *res = has_params
      ? PQexecPrepared(conn, stmtName, nParams, paramValues, paramLengths, paramFormats, 0)
      : PQexecPrepared(conn, stmtName, 0, NULL, NULL, NULL, 0);
    mrb_gc_arena_restore(mrb, arena_index);
    if (likely(res)) {
      return mrb_pq_result_processor(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Result)), res);
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
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  PGresult *res = PQdescribePrepared(conn, stmtName);
  if (likely(res)) {
    return mrb_pq_result_processor(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Result)), res);
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
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  errno = 0;
  PGresult *res = PQdescribePortal(conn, portalName);
  if (likely(res)) {
    return mrb_pq_result_processor(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Result)), res);
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
  mrb_yield(arg->mrb, arg->block, mrb_pq_result_processor(arg->mrb, arg->pq_result_class, res));
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
  PGconn *conn = (PGconn *) mrb_data_check_get_ptr(mrb, self, &mrb_PGconn_type);
  if (!conn) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }

  struct RClass *pq_class = mrb_obj_class(mrb, self);
  struct RClass *pq_result_class = mrb_class_get_under_id(mrb, pq_class, MRB_SYM(Result));
  mrb_PQnoticeReceiver_arg *arg;
  struct RData *notice_receiver_data;
  Data_Make_Struct(mrb, mrb_class_get_under_id(mrb, pq_class, MRB_SYM(NoticeReceiver)), mrb_PQnoticeReceiver_arg,&mrb_PQnoticeReceiver_type, arg, notice_receiver_data);
  arg->mrb = mrb;
  arg->pq_result_class = pq_result_class;
  arg->block = block;
  mrb_value notice_receiver = mrb_obj_value(notice_receiver_data);
  mrb_iv_set(mrb, notice_receiver, MRB_IVSYM(block), block);
  mrb_iv_set(mrb, self, MRB_IVSYM(notice_receiver), notice_receiver);

  PQsetNoticeReceiver(conn, mrb_PQnoticeReceiver, arg);

  return self;
}

static mrb_value
mrb_PQntuples(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, PQntuples((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type)));
}

static mrb_value
mrb_PQnfields(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, PQnfields((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type)));
}

static mrb_value
mrb_PQfname(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  char *fname = PQfname((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) column_number);
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

  int fnumber = PQfnumber((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), column_name);
  if (fnumber != -1) {
    return mrb_int_value(mrb, fnumber);
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

  Oid foo = PQftable((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) column_number);
  if (foo == InvalidOid) {
    mrb_raise(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(InvalidOid)), "Column number is out of range, or the specified column is not a simple reference to a table column, or using pre-3.0 protocol");
  }

  return mrb_int_value(mrb, foo);
}

static mrb_value
mrb_PQftablecol(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  int foo = PQftablecol((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) column_number);
  if (foo == 0) {
    mrb_raise(mrb, mrb_class_get_under_id(mrb, mrb_obj_class(mrb, self), MRB_SYM(Error)), "Column number is out of range, or the specified column is not a simple reference to a table column, or using pre-3.0 protocol");
  }

  return mrb_int_value(mrb, foo);
}

static mrb_value
mrb_PQfformat(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  return mrb_int_value(mrb, PQfformat((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) column_number));
}

static mrb_value
mrb_PQftype(mrb_state *mrb, mrb_value self)
{
  mrb_int column_number;
  mrb_get_args(mrb, "i", &column_number);
  mrb_assert_int_fit(mrb_int, column_number, int, INT_MAX);

  return mrb_int_value(mrb, PQftype((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) column_number));
}

static mrb_value
mrb_pq_decode_text_value(mrb_state *mrb, const PGresult *result, int row_number, int column_number, char *value)
{
  switch(PQftype(result, column_number)) {
    case 16: { // bool
      return mrb_bool_value(value[0] == 't');
    } break;
    case 20: { // int64_t
      return mrb_int_value(mrb, strtoll(value, NULL, 0));
    } break;
    case 23: // int32_t
    case 21: // int16_t
      return mrb_int_value(mrb, strtol(value, NULL, 0));
    break;
    case 114:
    case 3802: {
      if (mrb_class_defined_id(mrb, MRB_SYM(JSON))) {
        return mrb_funcall_id(mrb, mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(JSON))), MRB_SYM(parse), 1, mrb_str_new(mrb, value, PQgetlength(result, row_number, column_number)));
      } else {
        goto def;
      }
    } break;
    case 142: {
      if (mrb_class_defined_id(mrb, MRB_SYM(XML))) {
        return mrb_funcall_id(mrb, mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(XML))), MRB_SYM(parse), 1, mrb_str_new(mrb, value, PQgetlength(result, row_number, column_number)));
      } else {
        goto def;
      }
    } break;
#ifndef MRB_WITHOUT_FLOAT
    case 700: { // float
      return mrb_float_value(mrb, strtof(value, NULL));
    } break;
#ifndef MRB_USE_FLOAT
    case 701: { // double
      return mrb_float_value(mrb, strtod(value, NULL));
#endif
    } break;
#endif
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
  const PGresult *result = (const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type);

  char *value = PQgetvalue(result, (int) row_number, (int) column_number);
  if (value) {
    if (PQgetisnull(result, (int) row_number, (int) column_number)) {
      return mrb_symbol_value(MRB_SYM(NULL));
    } else if (PQfformat(result, (int) column_number) == 0) {
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

  return mrb_bool_value(PQgetisnull((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) row_number, (int) column_number));
}

static mrb_value
mrb_PQnparams(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, PQnparams((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type)));
}

static mrb_value
mrb_PQparamtype(mrb_state *mrb, mrb_value self)
{
  mrb_int param_number;
  mrb_get_args(mrb, "i", &param_number);
  mrb_assert_int_fit(mrb_int, param_number, int, INT_MAX);

  return mrb_int_value(mrb, PQparamtype((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) param_number));
}

static mrb_value
mrb_PQresultErrorField(mrb_state *mrb, mrb_value self)
{
  mrb_int fieldcode;
  mrb_get_args(mrb, "i", &fieldcode);
  mrb_assert_int_fit(mrb_int, fieldcode, int, INT_MAX);

  char *field = PQresultErrorField((const PGresult *) mrb_data_check_get_ptr(mrb, self, &mrb_PGresult_type), (int) fieldcode);
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
  pq_class = mrb_define_class_id(mrb, MRB_SYM(Pq), mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_class, MRB_TT_DATA);
  pq_error_class = mrb_define_class_under_id(mrb, pq_class, MRB_SYM(Error), E_RUNTIME_ERROR);
  mrb_define_class_under_id(mrb, pq_class, MRB_SYM(ConnectionError), pq_error_class);
  mrb_define_method_id(mrb, pq_class, MRB_SYM(initialize), mrb_PQconnectdb, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, pq_class, MRB_SYM(finish), mrb_PQfinish, MRB_ARGS_NONE());
  mrb_define_alias_id(mrb, pq_class, MRB_SYM(close), MRB_SYM(finish));
  mrb_define_method_id(mrb, pq_class, MRB_SYM(exec), mrb_PQexec, MRB_ARGS_REQ(1)|MRB_ARGS_REST()|MRB_ARGS_BLOCK());
  mrb_define_method_id(mrb, pq_class, MRB_SYM(_prepare), mrb_PQprepare, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, pq_class, MRB_SYM(exec_prepared), mrb_PQexecPrepared, MRB_ARGS_REQ(1)|MRB_ARGS_REST()|MRB_ARGS_BLOCK());
  mrb_define_method_id(mrb, pq_class, MRB_SYM(describe_prepared), mrb_PQdescribePrepared, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, pq_class, MRB_SYM(describe_portal), mrb_PQdescribePortal, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, pq_class, MRB_SYM(reset), mrb_PQreset, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, pq_class, MRB_SYM(cancel), mrb_PQrequestCancel, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, pq_class, MRB_SYM(socket), mrb_PQsocket, MRB_ARGS_NONE());
  mrb_define_alias_id(mrb, pq_class, MRB_SYM(to_i), MRB_SYM(socket));
  mrb_define_method_id(mrb, pq_class, MRB_SYM(notice_receiver), mrb_PQsetNoticeReceiver, MRB_ARGS_BLOCK());
  pq_notice_processor_class = mrb_define_class_under_id(mrb, pq_class, MRB_SYM(NoticeReceiver), mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_notice_processor_class, MRB_TT_DATA);
  pq_result_mixins = mrb_define_module_under_id(mrb, pq_class, MRB_SYM(ResultMixins));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(EMPTY_QUERY), mrb_int_value(mrb, PGRES_EMPTY_QUERY));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(COMMAND_OK), mrb_int_value(mrb, PGRES_COMMAND_OK));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(TUPLES_OK), mrb_int_value(mrb, PGRES_TUPLES_OK));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(COPY_OUT), mrb_int_value(mrb, PGRES_COPY_OUT));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(COPY_IN), mrb_int_value(mrb, PGRES_COPY_IN));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(BAD_RESPONSE), mrb_int_value(mrb, PGRES_BAD_RESPONSE));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(NONFATAL_ERROR), mrb_int_value(mrb, PGRES_NONFATAL_ERROR));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(FATAL_ERROR), mrb_int_value(mrb, PGRES_FATAL_ERROR));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(COPY_BOTH), mrb_int_value(mrb, PGRES_COPY_BOTH));
  mrb_define_const_id(mrb, pq_result_mixins, MRB_SYM(SINGLE_TUPLE), mrb_int_value(mrb, PGRES_SINGLE_TUPLE));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(ntuples), mrb_PQntuples, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(nfields), mrb_PQnfields, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(fname), mrb_PQfname, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(fnumber), mrb_PQfnumber, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(ftable), mrb_PQftable, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(ftablecol), mrb_PQftablecol, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(fformat), mrb_PQfformat, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(getvalue), mrb_PQgetvalue, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(getisnull), mrb_PQgetisnull, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(nparams), mrb_PQnparams, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(paramtype), mrb_PQparamtype, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, pq_result_mixins, MRB_SYM(ftype), mrb_PQftype, MRB_ARGS_REQ(1));
  pq_result_class = mrb_define_class_under_id(mrb, pq_class, MRB_SYM(Result), mrb->object_class);
  MRB_SET_INSTANCE_TT(pq_result_class, MRB_TT_DATA);
  mrb_include_module(mrb, pq_result_class, pq_result_mixins);
  pq_result_error_class = mrb_define_class_under_id(mrb, pq_result_class, MRB_SYM(Error), pq_error_class);
  MRB_SET_INSTANCE_TT(pq_result_error_class, MRB_TT_DATA);
  mrb_include_module(mrb, pq_result_error_class, pq_result_mixins);
  mrb_define_method_id(mrb, pq_result_error_class, MRB_SYM(field), mrb_PQresultErrorField, MRB_ARGS_REQ(1));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SEVERITY), mrb_int_value(mrb, PG_DIAG_SEVERITY));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SQLSTATE), mrb_int_value(mrb, PG_DIAG_SQLSTATE));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(MESSAGE_PRIMARY), mrb_int_value(mrb, PG_DIAG_MESSAGE_PRIMARY));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(MESSAGE_DETAIL), mrb_int_value(mrb, PG_DIAG_MESSAGE_DETAIL));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(MESSAGE_HINT), mrb_int_value(mrb, PG_DIAG_MESSAGE_HINT));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(STATEMENT_POSITION), mrb_int_value(mrb, PG_DIAG_STATEMENT_POSITION));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(CONTEXT), mrb_int_value(mrb, PG_DIAG_CONTEXT));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SOURCE_FILE), mrb_int_value(mrb, PG_DIAG_SOURCE_FILE));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SOURCE_LINE), mrb_int_value(mrb, PG_DIAG_SOURCE_LINE));
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SOURCE_FUNCTION), mrb_int_value(mrb, PG_DIAG_SOURCE_FUNCTION));
#ifdef PG_DIAG_SEVERITY_NONLOCALIZED
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SEVERITY_NONLOCALIZED), mrb_int_value(mrb, PG_DIAG_SEVERITY_NONLOCALIZED));
#endif
#ifdef PG_DIAG_INTERNAL_POSITION
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(INTERNAL_POSITION), mrb_int_value(mrb, PG_DIAG_INTERNAL_POSITION));
#endif
#ifdef PG_DIAG_INTERNAL_QUERY
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(INTERNAL_QUERY), mrb_int_value(mrb, PG_DIAG_INTERNAL_QUERY));
#endif
#ifdef PG_DIAG_SCHEMA_NAME
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(SCHEMA_NAME), mrb_int_value(mrb, PG_DIAG_SCHEMA_NAME));
#endif
#ifdef PG_DIAG_TABLE_NAME
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(TABLE_NAME), mrb_int_value(mrb, PG_DIAG_TABLE_NAME));
#endif
#ifdef PG_DIAG_COLUMN_NAME
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(COLUMN_NAME), mrb_int_value(mrb, PG_DIAG_COLUMN_NAME));
#endif
#ifdef PG_DIAG_DATATYPE_NAME
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(DATATYPE_NAME), mrb_int_value(mrb, PG_DIAG_DATATYPE_NAME));
#endif
#ifdef PG_DIAG_CONSTRAINT_NAME
  mrb_define_const_id(mrb, pq_result_error_class, MRB_SYM(CONSTRAINT_NAME), mrb_int_value(mrb, PG_DIAG_CONSTRAINT_NAME));
#endif
}

void mrb_mruby_postgresql_gem_final(mrb_state *mrb) {}