/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/object/ob_obj_type.h"
#include "common/timezone/ob_time_convert.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <glob.h>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// local file table (read_csv()/read_json()/read_parquet()/glob())
// ---------------------------------------------------------------------------

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec), file_name_, column_exprs_, column_indexes_,
                    column_types_, column_names_, format_, has_header_, field_delim_,
                    field_quote_, file_size_, file_mtime_);

namespace
{

int ft_append_field(common::ObIAllocator &alloc, const std::string &src,
                    common::ObIArray<common::ObString> &fields)
{
  int ret = OB_SUCCESS;
  if (src.empty()) {
    common::ObString empty;
    ret = fields.push_back(empty);
  } else {
    char *buf = static_cast<char *>(alloc.alloc(static_cast<int64_t>(src.size()) + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, src.data(), src.size());
      buf[src.size()] = '\0';
      common::ObString str(static_cast<int32_t>(src.size()), buf);
      ret = fields.push_back(str);
    }
  }
  return ret;
}

bool ft_is_int_value(const std::string &s)
{
  bool result = !s.empty();
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i >= s.size()) {
    result = false;
  }
  for (; result && i < s.size(); ++i) {
    if (!isdigit(static_cast<unsigned char>(s[i]))) {
      result = false;
    }
  }
  return result;
}

bool ft_is_double_value(const std::string &s)
{
  bool result = false;
  if (!s.empty()) {
    char *end = NULL;
    strtod(s.c_str(), &end);
    result = (NULL != end && '\0' == *end && end != s.c_str());
  }
  return result;
}

bool ft_is_bool_value(const std::string &s)
{
  bool result = false;
  if (5 == s.size()) {
    result = 0 == strncasecmp(s.c_str(), "false", 5);
  } else if (4 == s.size()) {
    result = 0 == strncasecmp(s.c_str(), "true", 4);
  }
  return result;
}

bool ft_is_date_value(const std::string &s)
{
  int y = 0, m = 0, d = 0;
  return 10 == s.size() && 3 == sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) &&
         y > 0 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

bool ft_is_datetime_value(const std::string &s)
{
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  return s.size() >= 19 &&
         6 == sscanf(s.c_str(), "%4d-%2d-%2d%*[ T]%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) &&
         y > 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
         h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && se >= 0 && se <= 59;
}

} // namespace

namespace
{
// resource guards for local file scans
const int64_t MAX_FILE_FIELD_LEN = 16L * 1024 * 1024;
const int64_t MAX_FILE_COLUMN_CNT = 4096;
} // namespace

ObCsvFileReader::ObCsvFileReader()
  : fp_(NULL), buf_pos_(0), buf_len_(0), eof_(false), has_header_(true),
    delim_(','), quote_('"'), field_count_(0)
{
  buf_[0] = '\0';
}

ObCsvFileReader::~ObCsvFileReader()
{
  close();
}

int ObCsvFileReader::open(const char *path, const bool has_header, const char delim, const char quote)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(path)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (NULL != fp_) {
    ret = OB_INIT_TWICE;
  } else if (NULL == (fp_ = fopen(path, "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("open local file failed", K(ret), KCSTRING(path));
  } else {
    has_header_ = has_header;
    delim_ = delim;
    quote_ = quote;
    field_count_ = 0;
    if (OB_FAIL(reset())) {
      close();
    }
  }
  return ret;
}

int ObCsvFileReader::close()
{
  int ret = OB_SUCCESS;
  if (NULL != fp_) {
    if (0 != fclose(fp_)) {
      ret = OB_IO_ERROR;
    }
    fp_ = NULL;
  }
  buf_pos_ = 0;
  buf_len_ = 0;
  eof_ = false;
  field_count_ = 0;
  return ret;
}

int ObCsvFileReader::reset()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(fp_)) {
    ret = OB_NOT_INIT;
  } else if (0 != fseek(fp_, 0, SEEK_SET)) {
    ret = OB_IO_ERROR;
  } else {
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
    field_count_ = 0;
    buf_len_ = static_cast<int64_t>(fread(buf_, 1, sizeof(buf_), fp_));
    if (buf_len_ <= 0) {
      eof_ = true;
      buf_pos_ = 0;
      buf_len_ = 0;
    } else if (buf_len_ >= 3 &&
               static_cast<unsigned char>(buf_[0]) == 0xEF &&
               static_cast<unsigned char>(buf_[1]) == 0xBB &&
               static_cast<unsigned char>(buf_[2]) == 0xBF) {
      buf_pos_ = 3;
    }
    if (OB_SUCC(ret) && has_header_ && OB_FAIL(skip_header())) {
      LOG_WARN("skip csv header failed", K(ret));
    }
  }
  return ret;
}

int ObCsvFileReader::get_char(char &c)
{
  int ret = OB_SUCCESS;
  if (buf_pos_ >= buf_len_) {
    if (eof_) {
      ret = OB_ITER_END;
    } else {
      buf_len_ = static_cast<int64_t>(fread(buf_, 1, sizeof(buf_), fp_));
      buf_pos_ = 0;
      if (buf_len_ <= 0) {
        eof_ = true;
        buf_len_ = 0;
        ret = OB_ITER_END;
      } else {
        c = buf_[buf_pos_++];
      }
    }
  } else {
    c = buf_[buf_pos_++];
  }
  return ret;
}

int ObCsvFileReader::peek_char(char &c)
{
  int ret = OB_SUCCESS;
  if (buf_pos_ < buf_len_) {
    c = buf_[buf_pos_];
  } else if (eof_) {
    ret = OB_ITER_END;
  } else {
    buf_len_ = static_cast<int64_t>(fread(buf_, 1, sizeof(buf_), fp_));
    buf_pos_ = 0;
    if (buf_len_ <= 0) {
      eof_ = true;
      buf_len_ = 0;
      ret = OB_ITER_END;
    } else {
      c = buf_[0];
    }
  }
  return ret;
}

int ObCsvFileReader::skip_header()
{
  int ret = OB_SUCCESS;
  char c = 0;
  bool in_quotes = false;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (OB_FAIL(get_char(c))) {
      // a header only file is fine
      ret = OB_SUCCESS;
      done = true;
    } else if (in_quotes) {
      if (c == quote_) {
        char next = 0;
        if (OB_SUCC(peek_char(next)) && next == quote_) {
          char dummy = 0;
          get_char(dummy);
        } else {
          in_quotes = false;
        }
      }
    } else if (c == quote_) {
      in_quotes = true;
    } else if (c == '\n') {
      done = true;
    }
  }
  return ret;
}

int ObCsvFileReader::read_field(std::string &field, bool &row_end)
{
  int ret = OB_SUCCESS;
  char c = 0;
  bool in_quotes = false;
  bool done = false;
  field.clear();
  row_end = false;
  if (OB_SUCC(peek_char(c)) && c == quote_) {
    in_quotes = true;
    if (OB_FAIL(get_char(c))) {
      ret = OB_SUCCESS;
    }
  }
  while (OB_SUCC(ret) && !done) {
    if (OB_FAIL(get_char(c))) {
      // eof reached, the partial field is still valid
      break;
    } else if (field.size() > MAX_FILE_FIELD_LEN) {
      // a single field must not grow without bound
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("csv field is too long", K(ret), "len", (int64_t)field.size());
    } else if (in_quotes) {
      if (c == quote_) {
        char next = 0;
        if (OB_SUCC(peek_char(next)) && next == quote_) {
          char dummy = 0;
          get_char(dummy);
          field.push_back(quote_);
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(c);
      }
    } else if (c == delim_) {
      done = true;
    } else if (c == '\n') {
      row_end = true;
      done = true;
    } else if (c == '\r') {
      char next = 0;
      if (OB_SUCC(peek_char(next)) && next == '\n') {
        char dummy = 0;
        get_char(dummy);
      }
      row_end = true;
      done = true;
    } else {
      field.push_back(c);
    }
  }
  if (OB_ITER_END == ret) {
    // keep the data read so far, the caller decides whether a record exists
  }
  return ret;
}

int ObCsvFileReader::next_record(common::ObIAllocator &alloc,
                                 common::ObIArray<common::ObString> &fields)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  field_count_ = 0;
  bool first = true;
  while (OB_SUCC(ret)) {
    std::string field;
    bool row_end = false;
    int tmp_ret = read_field(field, row_end);
    if (OB_ITER_END == tmp_ret) {
      if (first && field.empty()) {
        ret = OB_ITER_END;
        break;
      } else if (OB_FAIL(ft_append_field(alloc, field, fields))) {
        break;
      } else {
        ++field_count_;
        break;
      }
    } else if (OB_FAIL(tmp_ret)) {
      ret = tmp_ret;
      break;
    } else if (OB_FAIL(ft_append_field(alloc, field, fields))) {
      break;
    } else {
      ++field_count_;
      first = false;
      if (row_end) {
        break;
      }
    }
  }
  return ret;
}

int ob_csv_infer_schema(common::ObIAllocator &alloc,
                        const common::ObString &path,
                        const char delim,
                        const char quote,
                        const bool has_header,
                        common::ObIArray<common::ObString> &names,
                        common::ObIArray<int32_t> &types,
                        common::ObIArray<int32_t> &nullable)
{
  int ret = OB_SUCCESS;
  std::string std_path(path.ptr(), static_cast<size_t>(path.length()));
  ObCsvFileReader reader;
  common::ObArenaAllocator tmp_alloc;
  common::ObSEArray<common::ObString, 32> fields;
  std::vector<std::string> raw_names;
  std::vector<bool> any, all_int, all_double, all_bool, all_date, all_dt, has_null;
  int64_t column_cnt = 0;

  if (OB_FAIL(reader.open(std_path.c_str(), false, delim, quote))) {
    LOG_WARN("open csv file failed", K(ret), K(path));
  } else {
    int tmp_ret = reader.next_record(tmp_alloc, fields);
    if (OB_ITER_END == tmp_ret) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("csv file is empty", K(path));
    } else if (OB_FAIL(tmp_ret)) {
      ret = tmp_ret;
    } else {
      column_cnt = fields.count();
      if (column_cnt > MAX_FILE_COLUMN_CNT) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("too many columns in csv file", K(column_cnt));
      }
      for (int64_t i = 0; i < column_cnt; ++i) {
        raw_names.push_back(has_header
            ? std::string(fields.at(i).ptr(), static_cast<size_t>(fields.at(i).length()))
            : std::string());
      }
      any.assign(column_cnt, false);
      all_int.assign(column_cnt, true);
      all_double.assign(column_cnt, true);
      all_bool.assign(column_cnt, true);
      all_date.assign(column_cnt, true);
      all_dt.assign(column_cnt, true);
      has_null.assign(column_cnt, false);
      // scan every data row so that type widening (BIGINT -> DOUBLE -> VARCHAR)
      // and nullability are decided from the whole file, not just the first row.
      // NOTE: OB_SUCC() assigns to `ret`, so compare tmp_ret with OB_SUCCESS.
      if (has_header) {
        tmp_alloc.reuse();
        tmp_ret = reader.next_record(tmp_alloc, fields);
      }
      while (OB_SUCC(ret) && OB_SUCCESS == tmp_ret) {
        for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
          std::string val;
          if (c < fields.count() && !fields.at(c).empty()) {
            val.assign(fields.at(c).ptr(), static_cast<size_t>(fields.at(c).length()));
          }
          if (val.empty()) {
            has_null[c] = true;
          } else {
            any[c] = true;
            if (!ft_is_int_value(val)) all_int[c] = false;
            if (!ft_is_double_value(val)) all_double[c] = false;
            if (!ft_is_bool_value(val)) all_bool[c] = false;
            if (!ft_is_date_value(val)) all_date[c] = false;
            if (!ft_is_datetime_value(val)) all_dt[c] = false;
          }
        }
        tmp_alloc.reuse();
        tmp_ret = reader.next_record(tmp_alloc, fields);
      }
      if (OB_ITER_END != tmp_ret && OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
    int close_ret = reader.close();
    if (OB_SUCC(ret) && OB_FAIL(close_ret)) {
      ret = close_ret;
    }
  }

  if (OB_SUCC(ret)) {
    std::map<std::string, int64_t> seen;
    for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
      std::string base = raw_names[c];
      if (base.empty()) {
        base = "column" + std::to_string(c);
      }
      std::string name = base;
      std::map<std::string, int64_t>::iterator iter = seen.find(base);
      if (seen.end() == iter) {
        seen[base] = 1;
      } else {
        ++iter->second;
        name = base + "__" + std::to_string(iter->second);
      }
      int32_t type = ObVarcharType;
      if (!any[c]) {
        type = ObVarcharType;
      } else if (all_bool[c]) {
        type = ObTinyIntType;
      } else if (all_int[c]) {
        type = ObIntType;
      } else if (all_double[c]) {
        type = ObDoubleType;
      } else if (all_date[c]) {
        type = ObDateType;
      } else if (all_dt[c]) {
        type = ObDateTimeType;
      } else {
        type = ObVarcharType;
      }
      char *buf = static_cast<char *>(alloc.alloc(static_cast<int64_t>(name.size()) + 1));
      common::ObString col_name;
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, name.data(), name.size());
        buf[name.size()] = '\0';
        col_name.assign_ptr(buf, static_cast<int32_t>(name.size()));
        if (OB_FAIL(names.push_back(col_name))) {
        } else if (OB_FAIL(types.push_back(type))) {
        } else if (OB_FAIL(nullable.push_back(has_null[c] ? 1 : 0))) {
        }
      }
    }
  }
  return ret;
}

int ObCsvFileReader::next_line(common::ObIAllocator &alloc, common::ObString &line)
{
  int ret = OB_SUCCESS;
  std::string buf;
  char c = 0;
  bool got = false;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(get_char(c))) {
      break;
    } else if ('\n' == c) {
      got = true;
      break;
    } else if ('\r' == c) {
      // ignore
    } else {
      buf.push_back(c);
      got = true;
    }
  }
  if (OB_ITER_END == ret) {
    if (!got) {
      return OB_ITER_END;
    }
    ret = OB_SUCCESS;
  }
  if (OB_SUCC(ret)) {
    if (buf.empty()) {
      line.reset();
    } else {
      char *mem = static_cast<char *>(alloc.alloc(static_cast<int64_t>(buf.size()) + 1));
      if (OB_ISNULL(mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(mem, buf.data(), buf.size());
        mem[buf.size()] = '\0';
        line.assign_ptr(mem, static_cast<int32_t>(buf.size()));
      }
    }
  }
  return ret;
}

namespace
{

void jsonl_skip_space(const char *&p, const char *end)
{
  while (p < end && (*p == ' ' || *p == '\t')) {
    ++p;
  }
}

bool jsonl_read_string(const char *&p, const char *end, std::string &out)
{
  bool ok = false;
  if (p < end && '"' == *p) {
    ++p;
    ok = true;
    while (p < end && '"' != *p) {
      if ('\\' == *p && p + 1 < end) {
        ++p;
        switch (*p) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          default: out.push_back(*p); break;
        }
        ++p;
      } else {
        out.push_back(*p++);
      }
    }
    if (p < end && '"' == *p) {
      ++p;
    } else {
      ok = false;
    }
  }
  return ok;
}

} // namespace

int ob_jsonl_parse_line(const common::ObString &line,
                        common::ObIAllocator &alloc,
                        common::ObIArray<common::ObString> &keys,
                        common::ObIArray<common::ObString> &values,
                        common::ObIArray<int32_t> &kinds)
{
  int ret = OB_SUCCESS;
  const char *p = line.ptr();
  const char *end = p + line.length();
  jsonl_skip_space(p, end);
  if (p >= end || '{' != *p) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ++p;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      std::string key;
      std::string value;
      int32_t kind = 0;
      jsonl_skip_space(p, end);
      if (p < end && '}' == *p) {
        done = true;
        break;
      }
      if (!jsonl_read_string(p, end, key)) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      jsonl_skip_space(p, end);
      if (p >= end || ':' != *p) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      ++p;
      jsonl_skip_space(p, end);
      if (p >= end) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      if ('"' == *p) {
        kind = 0;
        if (!jsonl_read_string(p, end, value)) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
      } else if (0 == strncmp(p, "true", 4)) {
        kind = 2;
        value = "true";
        p += 4;
      } else if (0 == strncmp(p, "false", 5)) {
        kind = 2;
        value = "false";
        p += 5;
      } else if (0 == strncmp(p, "null", 4)) {
        kind = 3;
        p += 4;
      } else {
        kind = 1;
        const char *start = p;
        while (p < end && ',' != *p && '}' != *p) {
          ++p;
        }
        value.assign(start, static_cast<size_t>(p - start));
      }
      char *key_mem = static_cast<char *>(alloc.alloc(static_cast<int64_t>(key.size()) + 1));
      char *val_mem = static_cast<char *>(alloc.alloc(static_cast<int64_t>(value.size()) + 1));
      if (OB_ISNULL(key_mem) || OB_ISNULL(val_mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(key_mem, key.data(), key.size());
        key_mem[key.size()] = '\0';
        MEMCPY(val_mem, value.data(), value.size());
        val_mem[value.size()] = '\0';
        if (OB_FAIL(keys.push_back(common::ObString(static_cast<int32_t>(key.size()), key_mem)))) {
        } else if (OB_FAIL(values.push_back(common::ObString(static_cast<int32_t>(value.size()), val_mem)))) {
        } else if (OB_FAIL(kinds.push_back(kind))) {
        }
      }
      jsonl_skip_space(p, end);
      if (OB_SUCC(ret) && p < end && ',' == *p) {
        ++p;
      }
    }
  }
  return ret;
}

int ob_jsonl_infer_schema(common::ObIAllocator &alloc,
                          const common::ObString &path,
                          common::ObIArray<common::ObString> &names,
                          common::ObIArray<int32_t> &types,
                          common::ObIArray<int32_t> &nullable)
{
  int ret = OB_SUCCESS;
  std::string std_path(path.ptr(), static_cast<size_t>(path.length()));
  ObCsvFileReader reader;
  common::ObArenaAllocator tmp_alloc;
  common::ObSEArray<common::ObString, 16> keys;
  common::ObSEArray<common::ObString, 16> values;
  common::ObSEArray<int32_t, 16> kinds;
  std::vector<std::string> col_names;
  std::vector<int> all_kind;   // -1 unknown, 0 string, 1 int, 2 double, 3 bool, 4 mixed
  std::vector<bool> has_null;

  if (OB_FAIL(reader.open(std_path.c_str(), false, ',', '"'))) {
  } else {
    common::ObString line;
    int tmp_ret = OB_SUCCESS;
    while (OB_SUCC(ret) && OB_SUCCESS == (tmp_ret = reader.next_line(tmp_alloc, line))) {
      if (line.empty()) {
        continue;
      }
      keys.reuse();
      values.reuse();
      kinds.reuse();
      if (OB_FAIL(ob_jsonl_parse_line(line, tmp_alloc, keys, values, kinds))) {
      } else {
        // columns first seen order
        for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
          int found = -1;
          for (size_t c = 0; c < col_names.size(); ++c) {
            if (col_names[c].size() == static_cast<size_t>(keys.at(i).length()) &&
                0 == strncasecmp(col_names[c].c_str(), keys.at(i).ptr(), col_names[c].size())) {
              found = static_cast<int>(c);
              break;
            }
          }
          if (found < 0) {
            col_names.push_back(std::string(keys.at(i).ptr(),
                                            static_cast<size_t>(keys.at(i).length())));
            all_kind.push_back(-1);
            has_null.push_back(false);
            found = static_cast<int>(col_names.size() - 1);
          }
          if (3 == kinds.at(i)) {
            has_null[found] = true;
          } else {
            int kind = 1;
            if (0 == kinds.at(i)) {
              std::string text(values.at(i).ptr(), static_cast<size_t>(values.at(i).length()));
              if (ft_is_date_value(text)) {
                kind = 5;
              } else if (ft_is_datetime_value(text)) {
                kind = 6;
              } else {
                kind = 0;
              }
            } else if (2 == kinds.at(i)) {
              kind = 3;
            } else {
              std::string text(values.at(i).ptr(), static_cast<size_t>(values.at(i).length()));
              kind = ft_is_int_value(text) ? 1 : 2;
            }
            int &cur = all_kind[found];
            if (-1 == cur) {
              cur = kind;
            } else if (cur != kind) {
              if ((1 == cur && 2 == kind) || (2 == cur && 1 == kind)) {
                cur = 2;
              } else {
                cur = 4;
              }
            }
          }
        }
      }
      tmp_alloc.reuse();
    }
    if (OB_SUCCESS != tmp_ret && OB_ITER_END != tmp_ret) {
      ret = tmp_ret;
    }
    int close_ret = reader.close();
    if (OB_SUCC(ret) && OB_FAIL(close_ret)) {
      ret = close_ret;
    }
  }

  if (OB_SUCC(ret)) {
    for (size_t c = 0; OB_SUCC(ret) && c < col_names.size(); ++c) {
      int32_t type = ObVarcharType;
      switch (all_kind[c]) {
        case 0: type = ObVarcharType; break;
        case 1: type = ObIntType; break;
        case 2: type = ObDoubleType; break;
        case 3: type = ObTinyIntType; break;
        case 5: type = ObDateType; break;
        case 6: type = ObDateTimeType; break;
        default: type = ObVarcharType; break;
      }
      char *mem = static_cast<char *>(alloc.alloc(static_cast<int64_t>(col_names[c].size()) + 1));
      if (OB_ISNULL(mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(mem, col_names[c].data(), col_names[c].size());
        mem[col_names[c].size()] = '\0';
        if (OB_FAIL(names.push_back(common::ObString(static_cast<int32_t>(col_names[c].size()), mem)))) {
        } else if (OB_FAIL(types.push_back(type))) {
        } else if (OB_FAIL(nullable.push_back(has_null[c] ? 1 : 0))) {
        }
      }
    }
  }
  return ret;
}

int ob_csv_scan_nullable(const common::ObString &path,
                         const char delim,
                         const char quote,
                         const bool has_header,
                         common::ObIArray<int32_t> &nullable)
{
  int ret = OB_SUCCESS;
  std::string std_path(path.ptr(), static_cast<size_t>(path.length()));
  ObCsvFileReader reader;
  common::ObArenaAllocator tmp_alloc;
  common::ObSEArray<common::ObString, 32> fields;
  if (OB_FAIL(reader.open(std_path.c_str(), has_header, delim, quote))) {
    LOG_WARN("open csv file failed", K(ret), K(path));
  } else {
    int tmp_ret = reader.next_record(tmp_alloc, fields);
    // NOTE: OB_SUCC(x) assigns to ret, never use it on a local result here
    while (OB_SUCC(ret) && OB_SUCCESS == tmp_ret) {
      for (int64_t c = 0; OB_SUCC(ret) && c < nullable.count(); ++c) {
        if (c >= fields.count() || fields.at(c).empty()) {
          nullable.at(c) = 1;
        }
      }
      tmp_alloc.reuse();
      tmp_ret = reader.next_record(tmp_alloc, fields);
    }
    if (OB_ITER_END != tmp_ret && OB_SUCC(ret)) {
      ret = tmp_ret;
    }
    int close_ret = reader.close();
    if (OB_SUCC(ret) && OB_FAIL(close_ret)) {
      ret = close_ret;
    }
  }
  return ret;
}

namespace
{

// ------------------------- minimal parquet support -------------------------
// thrift compact protocol reader
class PqThrift
{
public:
  PqThrift(const char *buf, int64_t len) : buf_(buf), len_(len), pos_(0) {}
  int64_t pos() const { return pos_; }
  void set_pos(int64_t p) { pos_ = p; }
  bool ok() const { return pos_ <= len_; }
  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ITER_END;
    } else {
      v = static_cast<uint8_t>(buf_[pos_++]);
    }
    return ret;
  }
  int read_varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    while (OB_SUCC(ret)) {
      uint8_t b = 0;
      if (OB_FAIL(read_byte(b))) {
      } else {
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (0 == (b & 0x80)) {
          break;
        }
        shift += 7;
        if (shift > 63) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
    return ret;
  }
  int read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    int ret = read_varint(u);
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    }
    return ret;
  }
  int read_bytes(const char *&p, int64_t &len)
  {
    uint64_t n = 0;
    int ret = read_varint(n);
    if (OB_SUCC(ret)) {
      if (pos_ < 0 || n > static_cast<uint64_t>(len_ - pos_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        p = buf_ + pos_;
        len = static_cast<int64_t>(n);
        pos_ += len;
      }
    }
    return ret;
  }
  int skip(int type_id)
  {
    int ret = OB_SUCCESS;
    int64_t v = 0;
    const char *p = NULL;
    int64_t len = 0;
    switch (type_id) {
      case 1: case 2: break;                     // bool value in the header
      case 3: { uint8_t b = 0; ret = read_byte(b); break; }
      case 4: case 5: case 6: ret = read_zigzag(v); break;
      case 7: pos_ += 8; break;
      case 8: ret = read_bytes(p, len); break;
      case 9: case 10: {
        uint8_t h = 0;
        if (OB_FAIL(read_byte(h))) {
        } else {
          int64_t n = h >> 4;
          const int et = h & 0x0f;
          if (15 == n) {
            uint64_t big = 0;
            if (OB_FAIL(read_varint(big))) {
            } else {
              n = static_cast<int64_t>(big);
            }
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
            ret = skip(et);
          }
        }
        break;
      }
      case 11: {
        uint64_t n = 0;
        if (OB_FAIL(read_varint(n))) {
        } else if (n > 0) {
          uint8_t h = 0;
          if (OB_FAIL(read_byte(h))) {
          } else {
            const int et = h & 0x0f;
            for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              ret = skip(et);
            }
          }
        }
        break;
      }
      case 12: {                                  // struct: read until stop
        bool stop = false;
        while (OB_SUCC(ret) && !stop) {
          uint8_t h = 0;
          if (OB_FAIL(read_byte(h))) {
          } else if (0 == h) {
            stop = true;
          } else {
            const int et = h & 0x0f;
            if (0 == (h >> 4)) {
              int64_t fid = 0;
              ret = read_zigzag(fid);
            }
            if (OB_SUCC(ret)) {
              ret = skip(et);
            }
          }
        }
        break;
      }
      default: ret = OB_ERR_UNEXPECTED; break;
    }
    if (OB_SUCC(ret) && pos_ > len_) {
      ret = OB_ERR_UNEXPECTED;
    }
    return ret;
  }
  // returns field id, type in `type_id`, and whether the struct ended
  int read_field_header(int64_t &field_id, int &type_id, bool &stop, int64_t &last_id)
  {
    int ret = OB_SUCCESS;
    uint8_t h = 0;
    stop = false;
    if (OB_FAIL(read_byte(h))) {
    } else if (0 == h) {
      stop = true;
    } else {
      type_id = h & 0x0f;
      if (0 != (h >> 4)) {
        field_id = last_id + (h >> 4);
      } else if (OB_FAIL(read_zigzag(field_id))) {
      }
      last_id = field_id;
    }
    return ret;
  }
private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

// compact protocol list header: high nibble is the size, 15 means an extended
// varint size follows (the same encoding is used for maps)
int pq_read_list(PqThrift &thr, int64_t &count, int &elem_type)
{
  int ret = OB_SUCCESS;
  uint8_t h = 0;
  count = 0;
  elem_type = 0;
  if (OB_FAIL(thr.read_byte(h))) {
  } else {
    count = h >> 4;
    elem_type = h & 0x0f;
    if (15 == count) {
      uint64_t big = 0;
      if (OB_FAIL(thr.read_varint(big))) {
      } else if (big > 100000000ULL) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        count = static_cast<int64_t>(big);
      }
    }
  }
  return ret;
}

// snappy block format decoder
int pq_snappy_decompress(const char *src, int64_t src_len, std::string &dst)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  uint64_t expect = 0;
  int shift = 0;
  while (pos < src_len) {
    const uint8_t b = static_cast<uint8_t>(src[pos++]);
    expect |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) break;
    shift += 7;
  }
  dst.clear();
  dst.reserve(static_cast<size_t>(expect));
  while (OB_SUCC(ret) && pos < src_len) {
    const uint8_t tag = static_cast<uint8_t>(src[pos++]);
    const int t = tag & 0x03;
    if (0 == t) {  // literal
      int64_t len = (tag >> 2) + 1;
      if (len >= 61) {
        const int extra = static_cast<int>(len - 60);
        len = 0;
        for (int i = 0; i < extra; ++i) {
          len |= static_cast<int64_t>(static_cast<uint8_t>(src[pos++])) << (8 * i);
        }
        len += 1;
      }
      if (pos + len > src_len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        dst.append(src + pos, static_cast<size_t>(len));
        pos += len;
      }
    } else {  // copy
      int64_t len = 0;
      int64_t off = 0;
      if (1 == t) {
        len = 4 + ((tag >> 2) & 0x07);
        off = (static_cast<int64_t>(tag >> 5) << 8) | static_cast<uint8_t>(src[pos++]);
      } else if (2 == t) {
        len = 1 + (tag >> 2);
        off = static_cast<uint8_t>(src[pos]) | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 1])) << 8);
        pos += 2;
      } else {
        len = 1 + (tag >> 2);
        off = static_cast<uint8_t>(src[pos]) | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 1])) << 8) |
              (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 2])) << 16) |
              (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 3])) << 24);
        pos += 4;
      }
      if (off <= 0 || off > static_cast<int64_t>(dst.size())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t i = 0; i < len; ++i) {
          dst.push_back(dst[dst.size() - static_cast<size_t>(off)]);
        }
      }
    }
  }
  return ret;
}

// parquet RLE / bit-packed hybrid decoder, used both for the definition levels
// and for the dictionary indices
int pq_decode_rle_bp(const char *src_in, int64_t src_len, int bit_width,
                     int64_t want, std::vector<int64_t> &out)
{
  int ret = OB_SUCCESS;
  const unsigned char *src = reinterpret_cast<const unsigned char *>(src_in);
  int64_t pos = 0;
  const int byte_width = (bit_width + 7) / 8;
  if (bit_width < 0 || bit_width > 32 || want < 0 || src_len < 0) {
    ret = OB_ERR_UNEXPECTED;
  }
  while (OB_SUCC(ret) && pos < src_len && static_cast<int64_t>(out.size()) < want) {
    uint64_t header = 0;
    int shift = 0;
    while (pos < src_len) {
      const unsigned char b = src[pos++];
      header |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) {
        break;
      }
      shift += 7;
    }
    if (header & 1) {  // bit-packed: groups of 8 values
      const int64_t groups = static_cast<int64_t>(header >> 1);
      const int64_t cnt = groups * 8;
      const int64_t bytes = groups * bit_width;
      if (groups < 0 || bytes < 0 || pos + bytes > src_len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int64_t bit = 0;
        for (int64_t i = 0; i < cnt && static_cast<int64_t>(out.size()) < want; ++i) {
          int64_t v = 0;
          for (int b = 0; b < bit_width; ++b, ++bit) {
            const int64_t byte = pos + bit / 8;
            const int64_t off = bit % 8;
            if (0 != (src[byte] & (1u << off))) {
              v |= (static_cast<int64_t>(1) << b);
            }
          }
          out.push_back(v);
        }
        pos += bytes;
      }
    } else {  // RLE run
      const int64_t cnt = static_cast<int64_t>(header >> 1);
      if (cnt < 0 || pos + byte_width > src_len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int64_t v = 0;
        for (int i = 0; i < byte_width; ++i) {
          v |= static_cast<int64_t>(src[pos + i]) << (8 * i);
        }
        pos += byte_width;
        for (int64_t i = 0; i < cnt && static_cast<int64_t>(out.size()) < want; ++i) {
          out.push_back(v);
        }
      }
    }
  }
  return ret;
}

// one column chunk of one row group
struct PqChunk
{
  PqChunk() : offset_(0), num_values_(0), codec_(0) {}
  int64_t offset_;      // first page to read (the dictionary page when present)
  int64_t num_values_;
  int32_t codec_;
};

// decoded column: the value arrays are aligned with the row index, a null row
// keeps a placeholder in the typed array and a 1 in is_null_
struct PqColumn
{
  PqColumn()
    : name_(), pq_type_(0), repetition_(0), type_(0), type_length_(0), nullable_(0),
      chunks_(), ints_(), doubles_(), strings_(), is_null_(), has_dict_(false),
      dict_ints_(), dict_doubles_(), dict_strings_() {}
  std::string name_;
  int32_t pq_type_;      // parquet physical type
  int32_t repetition_;   // 0 required, 1 optional, 2 repeated
  int32_t type_;         // ObObjType
  int32_t type_length_;  // FIXED_LEN_BYTE_ARRAY only
  int32_t nullable_;
  std::vector<PqChunk> chunks_;
  std::vector<int64_t> ints_;
  std::vector<double> doubles_;
  std::vector<std::string> strings_;
  std::vector<uint8_t> is_null_;
  bool has_dict_;
  std::vector<int64_t> dict_ints_;
  std::vector<double> dict_doubles_;
  std::vector<std::string> dict_strings_;
};

struct PqPageHeader
{
  PqPageHeader()
    : type_(-1), uncompressed_(0), compressed_(0), num_values_(0), encoding_(0),
      def_len_(0), rep_len_(0), is_compressed_(true) {}
  int64_t type_;
  int64_t uncompressed_;
  int64_t compressed_;
  int64_t num_values_;
  int64_t encoding_;
  int64_t def_len_;   // data page v2
  int64_t rep_len_;   // data page v2
  bool is_compressed_;
};

int64_t pq_u32le(const char *p)
{
  return static_cast<int64_t>(static_cast<uint8_t>(p[0])) |
      (static_cast<int64_t>(static_cast<uint8_t>(p[1])) << 8) |
      (static_cast<int64_t>(static_cast<uint8_t>(p[2])) << 16) |
      (static_cast<int64_t>(static_cast<uint8_t>(p[3])) << 24);
}

// map a parquet physical type to the ObObjType exposed to SQL
int32_t pq_obj_type(const int32_t pq_type)
{
  int32_t type = ObIntType;
  switch (pq_type) {
    case 0: type = ObTinyIntType; break;    // BOOLEAN
    case 1: type = ObIntType; break;        // INT32
    case 2: type = ObIntType; break;        // INT64
    case 3: type = ObDateTimeType; break;   // INT96 timestamp
    case 4: type = ObDoubleType; break;     // FLOAT
    case 5: type = ObDoubleType; break;     // DOUBLE
    case 6: type = ObVarcharType; break;    // BYTE_ARRAY
    case 7: type = ObVarcharType; break;    // FIXED_LEN_BYTE_ARRAY
    default: type = ObIntType; break;
  }
  return type;
}

int pq_decompress(const int32_t codec, const char *src, const int64_t len, std::string &out)
{
  int ret = OB_SUCCESS;
  if (len < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (0 == codec) {
    out.assign(src, static_cast<size_t>(len));
  } else if (1 == codec) {
    if (OB_FAIL(pq_snappy_decompress(src, len, out))) {
      LOG_WARN("snappy decompress failed", K(ret), K(len));
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported parquet codec", K(ret), K(codec));
  }
  return ret;
}

// PLAIN encoded BYTE_ARRAY: a 4-byte little-endian length then the bytes.
// FIXED_LEN_BYTE_ARRAY (fixed_width > 0) has no length prefix.
int pq_plain_strings(const char *src, const int64_t len, const int64_t want,
                     const int32_t fixed_width, std::vector<std::string> &out)
{
  int ret = OB_SUCCESS;
  int64_t p = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
    int64_t n = fixed_width;
    if (n <= 0) {
      if (p + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        n = pq_u32le(src + p);
        p += 4;
      }
    }
    if (OB_SUCC(ret)) {
      if (n < 0 || p + n > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        out.emplace_back(src + p, static_cast<size_t>(n));
        p += n;
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("decode parquet byte array failed", K(ret), K(want), K(fixed_width), K(len));
  }
  return ret;
}

// PLAIN encoded BOOLEAN: one bit per value, least significant bit first
int pq_plain_bools(const char *src, const int64_t len, const int64_t want,
                   std::vector<int64_t> &out)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
    const int64_t byte = i / 8;
    if (byte >= len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      out.push_back((static_cast<uint8_t>(src[byte]) >> (i % 8)) & 1 ? 1 : 0);
    }
  }
  return ret;
}

// INT96: 8 bytes of nanoseconds of the day + 4 bytes of julian day
int64_t pq_int96_to_us(const char *p)
{
  uint64_t nanos = 0;
  uint32_t julian = 0;
  MEMCPY(&nanos, p, 8);
  MEMCPY(&julian, p + 8, 4);
  const int64_t days = static_cast<int64_t>(julian) - 2440588;  // 1970-01-01
  return days * 86400000000LL + static_cast<int64_t>(nanos / 1000);
}

// decode `want` non-null values of one page into the typed arrays
int pq_decode_values(PqColumn &col, const char *src, const int64_t len,
                     const int64_t want, const int64_t encoding,
                     std::vector<int64_t> &ints, std::vector<double> &doubles,
                     std::vector<std::string> &strings)
{
  int ret = OB_SUCCESS;
  if (want < 0 || len < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (2 == encoding || 8 == encoding) {  // RLE_DICTIONARY / PLAIN_DICTIONARY
    if (!col.has_dict_ || len < 1) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("parquet dictionary page missing", K(ret), K(col.name_.c_str()));
    } else {
      const int bit_width = static_cast<uint8_t>(src[0]);
      std::vector<int64_t> idx;
      if (OB_FAIL(pq_decode_rle_bp(src + 1, len - 1, bit_width, want, idx))) {
        LOG_WARN("decode dictionary indices failed", K(ret), K(col.name_.c_str()), K(bit_width));
      } else if (static_cast<int64_t>(idx.size()) < want) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("dictionary index page too short", K(ret), K(col.name_.c_str()),
                 K(idx.size()), K(want));
      } else if (6 == col.pq_type_ || 7 == col.pq_type_) {
        for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
          const int64_t v = idx[static_cast<size_t>(i)];
          if (v < 0 || v >= static_cast<int64_t>(col.dict_strings_.size())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            strings.push_back(col.dict_strings_[static_cast<size_t>(v)]);
          }
        }
      } else if (4 == col.pq_type_ || 5 == col.pq_type_) {
        for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
          const int64_t v = idx[static_cast<size_t>(i)];
          if (v < 0 || v >= static_cast<int64_t>(col.dict_doubles_.size())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            doubles.push_back(col.dict_doubles_[static_cast<size_t>(v)]);
          }
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
          const int64_t v = idx[static_cast<size_t>(i)];
          if (v < 0 || v >= static_cast<int64_t>(col.dict_ints_.size())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            ints.push_back(col.dict_ints_[static_cast<size_t>(v)]);
          }
        }
      }
    }
  } else if (0 != encoding) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported parquet encoding", K(ret), K(encoding), K(col.name_.c_str()));
  } else if (0 == col.pq_type_) {  // BOOLEAN
    ret = pq_plain_bools(src, len, want, ints);
  } else if (6 == col.pq_type_ || 7 == col.pq_type_) {  // BYTE_ARRAY
    ret = pq_plain_strings(src, len, want,
                           7 == col.pq_type_ ? col.type_length_ : 0, strings);
  } else if (4 == col.pq_type_) {  // FLOAT, 4 bytes each
    for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
      if (i * 4 + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        float v = 0;
        MEMCPY(&v, src + i * 4, 4);
        doubles.push_back(static_cast<double>(v));
      }
    }
  } else if (5 == col.pq_type_) {  // DOUBLE, 8 bytes each
    for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
      if (i * 8 + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        double v = 0;
        MEMCPY(&v, src + i * 8, 8);
        doubles.push_back(v);
      }
    }
  } else if (1 == col.pq_type_) {  // INT32
    for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
      if (i * 4 + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int32_t v = 0;
        MEMCPY(&v, src + i * 4, 4);
        ints.push_back(v);
      }
    }
  } else if (3 == col.pq_type_) {  // INT96
    for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
      if (i * 12 + 12 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ints.push_back(pq_int96_to_us(src + i * 12));
      }
    }
  } else {  // INT64
    for (int64_t i = 0; OB_SUCC(ret) && i < want; ++i) {
      if (i * 8 + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int64_t v = 0;
        MEMCPY(&v, src + i * 8, 8);
        ints.push_back(v);
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("decode parquet page values failed", K(ret), K(col.name_.c_str()),
             K(col.pq_type_), K(want), K(encoding), K(len));
  }
  return ret;
}

// append one page worth of rows, keeping nulls aligned with the row index
int pq_append_rows(PqColumn &col, const std::vector<int64_t> &levels,
                   const std::vector<int64_t> &ints,
                   const std::vector<double> &doubles,
                   const std::vector<std::string> &strings)
{
  int ret = OB_SUCCESS;
  size_t vi = 0;
  size_t di = 0;
  size_t si = 0;
  for (size_t i = 0; OB_SUCC(ret) && i < levels.size(); ++i) {
    const bool present = (levels.at(i) > 0);
    col.is_null_.push_back(present ? 0 : 1);
    if (!present) {
      if (ObDoubleType == col.type_) {
        col.doubles_.push_back(0);
      } else if (ObVarcharType == col.type_) {
        col.strings_.emplace_back();
      } else {
        col.ints_.push_back(0);
      }
    } else if (ObDoubleType == col.type_) {
      if (di >= doubles.size()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        col.doubles_.push_back(doubles.at(di++));
      }
    } else if (ObVarcharType == col.type_) {
      if (si >= strings.size()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        col.strings_.push_back(strings.at(si++));
      }
    } else {
      if (vi >= ints.size()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        col.ints_.push_back(ints.at(vi++));
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("merge parquet page values failed", K(ret), K(col.name_.c_str()), K(levels.size()),
             K(ints.size()), K(doubles.size()), K(strings.size()));
  }
  return ret;
}

// decode the definition levels of a data page.  A flat optional column has
// max definition level 1, so the bit width is 1.
int pq_decode_levels(const char *src, const int64_t len, const int64_t want,
                     std::vector<int64_t> &levels)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(pq_decode_rle_bp(src, len, 1, want, levels))) {
    LOG_WARN("decode definition levels failed", K(ret), K(len), K(want));
  } else if (static_cast<int64_t>(levels.size()) < want) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("definition level section too short", K(ret), K(levels.size()), K(want));
  }
  return ret;
}

int pq_parse_page_header(const std::string &buf, int64_t &pos, PqPageHeader &h)
{
  int ret = OB_SUCCESS;
  PqThrift thr(buf.data(), static_cast<int64_t>(buf.size()));
  thr.set_pos(pos);
  int64_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    int64_t fid = 0;
    int tid = 0;
    if (OB_FAIL(thr.read_field_header(fid, tid, stop, last))) {
    } else if (stop) {
      break;
    } else if (1 == fid) {
      ret = thr.read_zigzag(h.type_);
    } else if (2 == fid) {
      ret = thr.read_zigzag(h.uncompressed_);
    } else if (3 == fid) {
      ret = thr.read_zigzag(h.compressed_);
    } else if (5 == fid && 12 == tid) {  // data_page_header (v1)
      int64_t d_last = 0;
      bool d_stop = false;
      while (OB_SUCC(ret) && !d_stop) {
        int64_t dfid = 0;
        int dtid = 0;
        if (OB_FAIL(thr.read_field_header(dfid, dtid, d_stop, d_last))) {
        } else if (d_stop) {
          break;
        } else if (1 == dfid) {
          ret = thr.read_zigzag(h.num_values_);
        } else if (2 == dfid) {
          ret = thr.read_zigzag(h.encoding_);
        } else {
          ret = thr.skip(dtid);
        }
      }
    } else if (7 == fid && 12 == tid) {  // dictionary_page_header
      int64_t d_last = 0;
      bool d_stop = false;
      while (OB_SUCC(ret) && !d_stop) {
        int64_t dfid = 0;
        int dtid = 0;
        if (OB_FAIL(thr.read_field_header(dfid, dtid, d_stop, d_last))) {
        } else if (d_stop) {
          break;
        } else if (1 == dfid) {
          ret = thr.read_zigzag(h.num_values_);
        } else if (2 == dfid) {
          ret = thr.read_zigzag(h.encoding_);
        } else {
          ret = thr.skip(dtid);
        }
      }
    } else if (8 == fid && 12 == tid) {  // data_page_header_v2
      int64_t d_last = 0;
      bool d_stop = false;
      while (OB_SUCC(ret) && !d_stop) {
        int64_t dfid = 0;
        int dtid = 0;
        if (OB_FAIL(thr.read_field_header(dfid, dtid, d_stop, d_last))) {
        } else if (d_stop) {
          break;
        } else if (1 == dfid) {
          ret = thr.read_zigzag(h.num_values_);
        } else if (4 == dfid) {
          ret = thr.read_zigzag(h.encoding_);
        } else if (5 == dfid) {
          ret = thr.read_zigzag(h.def_len_);
        } else if (6 == dfid) {
          ret = thr.read_zigzag(h.rep_len_);
        } else if (7 == dfid && 1 == dtid) {
          h.is_compressed_ = true;
        } else if (7 == dfid && 2 == dtid) {
          h.is_compressed_ = false;
        } else {
          ret = thr.skip(dtid);
        }
      }
    } else {
      ret = thr.skip(tid);
    }
  }
  if (OB_SUCC(ret)) {
    if (h.type_ < 0 || h.compressed_ < 0 || h.uncompressed_ < 0 ||
        h.num_values_ < 0 || h.rep_len_ < 0 || h.def_len_ < 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      pos = thr.pos();
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("parse parquet page header failed", K(ret), K(pos));
  }
  return ret;
}

// decode all pages of one column chunk, appending to the column arrays
int pq_decode_chunk(const std::string &buf, PqColumn &col, const PqChunk &chunk,
                    const int64_t remaining_rows)
{
  int ret = OB_SUCCESS;
  const int64_t total = chunk.num_values_ > 0 ? chunk.num_values_ : remaining_rows;
  int64_t pos = chunk.offset_;
  int64_t values_read = 0;
  col.has_dict_ = false;
  col.dict_ints_.clear();
  col.dict_doubles_.clear();
  col.dict_strings_.clear();
  while (OB_SUCC(ret) && values_read < total) {
    PqPageHeader h;
    if (pos <= 0 || pos + 8 >= static_cast<int64_t>(buf.size())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("parquet page offset out of file", K(ret), K(pos), K(col.name_.c_str()));
    } else if (OB_FAIL(pq_parse_page_header(buf, pos, h))) {
    } else if (h.compressed_ > static_cast<int64_t>(buf.size()) - pos) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("parquet page body out of file", K(ret), K(pos), K(h.compressed_));
    } else if (2 == h.type_) {  // DICTIONARY_PAGE
      std::string dict;
      if (OB_FAIL(pq_decompress(chunk.codec_, buf.data() + pos, h.compressed_, dict))) {
      } else if (0 != h.encoding_) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported dictionary encoding", K(ret), K(h.encoding_));
      } else {
        col.has_dict_ = true;
        if (OB_FAIL(pq_decode_values(col, dict.data(), static_cast<int64_t>(dict.size()),
                                     h.num_values_, 0, col.dict_ints_, col.dict_doubles_,
                                     col.dict_strings_))) {
        }
      }
      pos += h.compressed_;
    } else if (0 == h.type_ || 3 == h.type_) {  // DATA_PAGE v1 / DATA_PAGE_V2
      std::vector<int64_t> levels;
      std::vector<int64_t> ints;
      std::vector<double> doubles;
      std::vector<std::string> strings;
      std::string body;
      const char *values = NULL;
      int64_t values_len = 0;
      if (0 == h.type_) {  // v1: levels and values are compressed together
        if (OB_FAIL(pq_decompress(chunk.codec_, buf.data() + pos, h.compressed_, body))) {
        } else {
          const int64_t body_len = static_cast<int64_t>(body.size());
          int64_t p = 0;
          if (2 == col.repetition_) {
            if (p + 4 > body_len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              const int64_t rep_len = pq_u32le(body.data() + p);
              p += 4;
              if (p + rep_len > body_len) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                p += rep_len;
              }
            }
          }
          if (OB_SUCC(ret) && col.nullable_) {
            if (p + 4 > body_len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              const int64_t def_len = pq_u32le(body.data() + p);
              p += 4;
              if (p + def_len > body_len) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(pq_decode_levels(body.data() + p, def_len,
                                                  h.num_values_, levels))) {
              } else {
                p += def_len;
              }
            }
          }
          if (OB_SUCC(ret)) {
            values = body.data() + p;
            values_len = body_len - p;
            if (values_len < 0) {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        }
      } else {  // v2: levels are stored uncompressed in front of the values
        if (col.nullable_ &&
            OB_FAIL(pq_decode_levels(buf.data() + pos + h.rep_len_, h.def_len_,
                                     h.num_values_, levels))) {
        } else if (OB_SUCC(ret)) {
          const int64_t comp_values = h.compressed_ - h.rep_len_ - h.def_len_;
          const char *vsrc = buf.data() + pos + h.rep_len_ + h.def_len_;
          if (comp_values < 0) {
            ret = OB_ERR_UNEXPECTED;
          } else if (h.is_compressed_) {
            if (OB_FAIL(pq_decompress(chunk.codec_, vsrc, comp_values, body))) {
            } else {
              values = body.data();
              values_len = static_cast<int64_t>(body.size());
            }
          } else {
            values = vsrc;
            values_len = comp_values;
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (!col.nullable_) {
          levels.assign(static_cast<size_t>(h.num_values_), 1);
        }
        int64_t present = 0;
        for (size_t i = 0; i < levels.size(); ++i) {
          if (levels.at(i) > 0) {
            ++present;
          }
        }
        if (OB_FAIL(pq_decode_values(col, values, values_len, present, h.encoding_,
                                     ints, doubles, strings))) {
        } else if (OB_FAIL(pq_append_rows(col, levels, ints, doubles, strings))) {
        } else {
          values_read += h.num_values_;
        }
      }
      pos += h.compressed_;
    } else if (1 == h.type_) {  // INDEX_PAGE, no writer produces it for a flat file
      pos += h.compressed_;
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet page type", K(ret), K(h.type_));
    }
  }
  return ret;
}

} // namespace

int ob_parquet_read(const common::ObString &path,
                    common::ObIAllocator &alloc,
                    std::vector<ObParquetColumn> &out_columns,
                    const bool read_values)
{
  int ret = OB_SUCCESS;
  std::string file_name(path.ptr(), static_cast<size_t>(path.length()));
  std::string buf;
  FILE *fp = fopen(file_name.c_str(), "rb");
  int64_t num_rows = 0;
  std::vector<PqColumn> columns;
  if (NULL == fp) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    char tmp[65536];
    size_t n = 0;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
      buf.append(tmp, n);
    }
    fclose(fp);
    fp = NULL;
  }
  if (OB_FAIL(ret)) {
  } else if (buf.size() < 12 || 0 != buf.compare(0, 4, "PAR1") ||
             0 != buf.compare(buf.size() - 4, 4, "PAR1")) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not a parquet file", K(ret), KCSTRING(file_name.c_str()));
  } else {
    const int64_t meta_len = static_cast<int64_t>(
        static_cast<uint8_t>(buf[buf.size() - 8]) |
        (static_cast<uint8_t>(buf[buf.size() - 7]) << 8) |
        (static_cast<uint8_t>(buf[buf.size() - 6]) << 16) |
        (static_cast<uint8_t>(buf[buf.size() - 5]) << 24));
    const int64_t meta_pos = static_cast<int64_t>(buf.size()) - 8 - meta_len;
    if (meta_pos <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("bad parquet footer", K(ret), K(meta_len), K(meta_pos));
    } else {
      PqThrift thr(buf.data(), static_cast<int64_t>(buf.size()));
      thr.set_pos(meta_pos);
      int64_t last_id = 0;
      bool stop = false;
      while (OB_SUCC(ret) && !stop) {
        int64_t fid = 0;
        int tid = 0;
        if (OB_FAIL(thr.read_field_header(fid, tid, stop, last_id))) {
        } else if (stop) {
          break;
        } else if (2 == fid && (9 == tid || 10 == tid)) {  // schema
          int64_t n = 0;
          int et = 0;
          if (OB_FAIL(pq_read_list(thr, n, et))) {
          } else if (12 != et) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              PqColumn col;
              int64_t s_last = 0;
              bool s_stop = false;
              int32_t repetition = 0;
              bool has_type = false;
              while (OB_SUCC(ret) && !s_stop) {
                int64_t sfid = 0;
                int stid = 0;
                if (OB_FAIL(thr.read_field_header(sfid, stid, s_stop, s_last))) {
                } else if (s_stop) {
                  break;
                } else if (1 == sfid) {
                  int64_t v = 0;
                  if (OB_FAIL(thr.read_zigzag(v))) {
                  } else if (v < 0 || v > 7) {
                    ret = OB_NOT_SUPPORTED;
                    LOG_WARN("unknown parquet physical type", K(ret), K(v));
                  } else {
                    col.pq_type_ = static_cast<int32_t>(v);
                    has_type = true;
                  }
                } else if (2 == sfid) {
                  int64_t v = 0;
                  if (OB_FAIL(thr.read_zigzag(v))) {
                  } else {
                    col.type_length_ = static_cast<int32_t>(v);
                  }
                } else if (3 == sfid) {
                  int64_t v = 0;
                  if (OB_FAIL(thr.read_zigzag(v))) {
                  } else {
                    repetition = static_cast<int32_t>(v);
                  }
                } else if (4 == sfid) {
                  const char *p = NULL;
                  int64_t len = 0;
                  if (OB_FAIL(thr.read_bytes(p, len))) {
                  } else {
                    col.name_.assign(p, static_cast<size_t>(len));
                  }
                } else if (OB_FAIL(thr.skip(stid))) {
                }
              }
              if (OB_SUCC(ret)) {
                if (0 == i) {
                  // the root element describes the message itself
                } else if (!has_type) {
                  ret = OB_NOT_SUPPORTED;
                  LOG_WARN("nested parquet schema is not supported", K(ret));
                } else if (2 == repetition) {
                  ret = OB_NOT_SUPPORTED;
                  LOG_WARN("repeated parquet column is not supported", K(ret),
                           K(col.name_.c_str()));
                } else {
                  col.repetition_ = repetition;
                  col.nullable_ = 1 == repetition ? 1 : 0;
                  col.type_ = pq_obj_type(col.pq_type_);
                  columns.push_back(col);
                }
              }
            }
          }
        } else if (3 == fid) {  // num_rows
          if (OB_FAIL(thr.read_zigzag(num_rows))) {
          }
        } else if (4 == fid && (9 == tid || 10 == tid)) {  // row_groups
          int64_t rg_n = 0;
          int rg_et = 0;
          if (OB_FAIL(pq_read_list(thr, rg_n, rg_et))) {
          } else if (12 != rg_et) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            for (int64_t r = 0; OB_SUCC(ret) && r < rg_n; ++r) {
              int64_t rg_last = 0;
              bool rg_stop = false;
              while (OB_SUCC(ret) && !rg_stop) {
                int64_t rfid = 0;
                int rtid = 0;
                if (OB_FAIL(thr.read_field_header(rfid, rtid, rg_stop, rg_last))) {
                } else if (rg_stop) {
                  break;
                } else if (1 == rfid && (9 == rtid || 10 == rtid)) {  // columns
                  int64_t cn = 0;
                  int cet = 0;
                  if (OB_FAIL(pq_read_list(thr, cn, cet))) {
                  } else if (12 != cet) {
                    ret = OB_ERR_UNEXPECTED;
                  } else {
                    for (int64_t c = 0; OB_SUCC(ret) && c < cn; ++c) {
                      int64_t cc_last = 0;
                      bool cc_stop = false;
                      int64_t data_offset = -1;
                      int64_t dict_offset = -1;
                      int64_t num_values = 0;
                      int32_t codec = 0;
                      while (OB_SUCC(ret) && !cc_stop) {
                        int64_t cfid = 0;
                        int ctid = 0;
                        if (OB_FAIL(thr.read_field_header(cfid, ctid, cc_stop, cc_last))) {
                        } else if (cc_stop) {
                          break;
                        } else if (3 == cfid && 12 == ctid) {  // ColumnMetaData
                          int64_t md_last = 0;
                          bool md_stop = false;
                          while (OB_SUCC(ret) && !md_stop) {
                            int64_t mfid = 0;
                            int mtid = 0;
                            if (OB_FAIL(thr.read_field_header(mfid, mtid, md_stop, md_last))) {
                            } else if (md_stop) {
                              break;
                            } else if (4 == mfid) {
                              int64_t v = 0;
                              if (OB_FAIL(thr.read_zigzag(v))) {
                              } else {
                                codec = static_cast<int32_t>(v);
                              }
                            } else if (5 == mfid) {
                              if (OB_FAIL(thr.read_zigzag(num_values))) {
                              }
                            } else if (9 == mfid) {
                              if (OB_FAIL(thr.read_zigzag(data_offset))) {
                              }
                            } else if (11 == mfid) {
                              if (OB_FAIL(thr.read_zigzag(dict_offset))) {
                              }
                            } else if (OB_FAIL(thr.skip(mtid))) {
                            }
                          }
                        } else if (OB_FAIL(thr.skip(ctid))) {
                        }
                      }
                      if (OB_SUCC(ret) && c < static_cast<int64_t>(columns.size())) {
                        PqChunk chunk;
                        chunk.offset_ = (dict_offset > 0 && data_offset > 0 &&
                                         dict_offset < data_offset)
                                        ? dict_offset : data_offset;
                        chunk.num_values_ = num_values;
                        chunk.codec_ = codec;
                        columns[static_cast<size_t>(c)].chunks_.push_back(chunk);
                      }
                    }
                  }
                } else if (OB_FAIL(thr.skip(rtid))) {
                }
              }
            }
          }
        } else if (OB_FAIL(thr.skip(tid))) {
        }
      }
    }
  }

  if (OB_SUCC(ret) && read_values) {
    for (size_t c = 0; OB_SUCC(ret) && c < columns.size(); ++c) {
      PqColumn &col = columns[c];
      int64_t decoded = 0;
      for (size_t k = 0; OB_SUCC(ret) && k < col.chunks_.size(); ++k) {
        const int64_t remaining = num_rows - decoded;
        if (remaining <= 0) {
          break;
        } else if (OB_FAIL(pq_decode_chunk(buf, col, col.chunks_[k], remaining))) {
          LOG_WARN("decode parquet column failed", K(ret), K(c), K(k),
                   K(col.chunks_[k].offset_), K(col.chunks_[k].num_values_));
        } else {
          decoded = static_cast<int64_t>(col.is_null_.size());
        }
      }
      if (OB_SUCC(ret) && static_cast<int64_t>(col.is_null_.size()) != num_rows) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("parquet column row count mismatch", K(ret), K(col.name_.c_str()),
                 K(col.is_null_.size()), K(num_rows));
      }
    }
  }

  if (OB_SUCC(ret)) {
    out_columns.resize(columns.size());
    for (size_t c = 0; OB_SUCC(ret) && c < columns.size(); ++c) {
      PqColumn &col = columns[c];
      char *mem = static_cast<char *>(alloc.alloc(static_cast<int64_t>(col.name_.size()) + 1));
      if (OB_ISNULL(mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(mem, col.name_.data(), col.name_.size());
        mem[col.name_.size()] = '\0';
        ObParquetColumn &dst = out_columns[c];
        dst.name_.assign_ptr(mem, static_cast<int32_t>(col.name_.size()));
        dst.type_ = col.type_;
        dst.nullable_ = (0 != col.nullable_);
        dst.row_count_ = read_values ? static_cast<int64_t>(col.is_null_.size()) : num_rows;
        if (read_values) {
          dst.ints_.swap(col.ints_);
          dst.doubles_.swap(col.doubles_);
          dst.strings_.swap(col.strings_);
          dst.is_null_.swap(col.is_null_);
        }
      }
    }
  }
  return ret;
}

int ob_file_field_to_obj(const common::ObString &field, const int32_t type, common::ObObj &obj)
{
  int ret = OB_SUCCESS;
  std::string val(field.ptr(), static_cast<size_t>(field.length()));
  switch (type) {
    case ObIntType: {
      char *end = NULL;
      int64_t v = strtoll(val.c_str(), &end, 10);
      if (NULL == end || '\0' != *end || end == val.c_str()) {
        ret = OB_INVALID_NUMERIC;
      } else {
        obj.set_int(v);
      }
      break;
    }
    case ObDoubleType: {
      char *end = NULL;
      double v = strtod(val.c_str(), &end);
      if (NULL == end || '\0' != *end || end == val.c_str()) {
        ret = OB_INVALID_NUMERIC;
      } else {
        obj.set_double(v);
      }
      break;
    }
    case ObTinyIntType: {
      int8_t v = 0;
      if (0 == strncasecmp(val.c_str(), "true", 4) || 0 == strcmp(val.c_str(), "1")) {
        v = 1;
      } else if (0 == strncasecmp(val.c_str(), "false", 5) || 0 == strcmp(val.c_str(), "0")) {
        v = 0;
      } else {
        ret = OB_INVALID_NUMERIC;
      }
      if (OB_SUCC(ret)) {
        obj.set_tinyint(v);
      }
      break;
    }
    case ObDateType: {
      int32_t date = 0;
      if (OB_FAIL(common::ObTimeConverter::str_to_date(field, date))) {
        ret = OB_INVALID_DATE_VALUE;
      } else {
        obj.set_date(date);
      }
      break;
    }
    case ObDateTimeType: {
      int64_t datetime = 0;
      std::string normalized = val;
      if (normalized.size() >= 11 && 'T' == normalized[10]) {
        normalized[10] = ' ';
      }
      common::ObString str(static_cast<int32_t>(normalized.size()), normalized.data());
      common::ObTimeConvertCtx cvrt_ctx(NULL, false);
      if (OB_FAIL(common::ObTimeConverter::str_to_datetime(str, cvrt_ctx, datetime))) {
        ret = OB_INVALID_DATE_VALUE;
      } else {
        obj.set_datetime(datetime);
      }
      break;
    }
    case ObVarcharType:
      obj.set_varchar(field);
      obj.set_collation_type(common::CS_TYPE_UTF8MB4_GENERAL_CI);
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      break;
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("convert file field failed", K(ret), K(field), K(type));
  }
  return ret;
}

const char *ob_file_type_name(const int32_t type)
{
  switch (type) {
    case ObIntType: return "BIGINT";
    case ObDoubleType: return "DOUBLE";
    case ObTinyIntType: return "BOOLEAN";
    case ObDateType: return "DATE";
    case ObDateTimeType: return "DATETIME";
    default: return "VARCHAR";
  }
}

int ob_check_secure_file_priv(const common::ObString &priv,
                              const common::ObString &canonical_path,
                              bool &allowed)
{
  int ret = OB_SUCCESS;
  allowed = true;
  if (!priv.empty()) {
    std::string dir(priv.ptr(), static_cast<size_t>(priv.length()));
    while (dir.size() > 1 && '/' == dir[dir.size() - 1]) {
      dir.erase(dir.size() - 1);
    }
    std::string path(canonical_path.ptr(), static_cast<size_t>(canonical_path.length()));
    if ("/" == dir) {
      // the file system root allows everything
      allowed = true;
    } else {
      allowed = path.size() >= dir.size() &&
                0 == path.compare(0, dir.size(), dir) &&
                (path.size() == dir.size() || '/' == path[dir.size()]);
    }
  }
  return ret;
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &spec = MY_SPEC;
  std::string path(spec.file_name_.ptr(), static_cast<size_t>(spec.file_name_.length()));
  if (spec.file_name_.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table without file name", K(ret));
  } else if (3 == spec.format_) {  // GLOB
    glob_t glob_result;
    MEMSET(&glob_result, 0, sizeof(glob_result));
    int glob_ret = ::glob(path.c_str(), 0, NULL, &glob_result);
    if (0 != glob_ret && GLOB_NOMATCH != glob_ret) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("glob failed", K(ret), KCSTRING(path.c_str()), K(glob_ret));
    } else {
      for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
        glob_files_.push_back(std::string(glob_result.gl_pathv[i]));
      }
      glob_idx_ = 0;
      opened_ = true;
    }
    ::globfree(&glob_result);
  } else {
    struct stat file_stat;
    if (0 != stat(path.c_str(), &file_stat)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("stat file failed", K(ret), KCSTRING(path.c_str()));
    } else if (static_cast<int64_t>(file_stat.st_size) != spec.file_size_) {
      // the file changed after the schema was infered, refuse to scan it.
      // only the size is compared: re-writing the same content changes mtime and
      // would otherwise make the same statement fail when its plan is cached.
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("file changed after schema inference", K(ret),
               "plan_size", spec.file_size_, "now_size", (int64_t)file_stat.st_size,
               "plan_mtime", spec.file_mtime_, "now_mtime", (int64_t)file_stat.st_mtime);
    } else if (0 == spec.format_ || 1 == spec.format_) {  // CSV / JSONL
      // jsonl has no header line, never skip the first record there
      const bool skip_header = (0 == spec.format_) && spec.has_header_;
      if (OB_FAIL(reader_.open(path.c_str(), skip_header,
                               spec.field_delim_, spec.field_quote_))) {
      } else {
        opened_ = true;
      }
    } else if (2 == spec.format_) {  // PARQUET
      if (OB_FAIL(ob_parquet_read(spec.file_name_, ctx_.get_allocator(),
                                  pq_cols_, true))) {
        LOG_WARN("read parquet file failed", K(ret), KCSTRING(path.c_str()));
      } else if (pq_cols_.empty()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("parquet file has no column", K(ret), KCSTRING(path.c_str()));
      } else {
        pq_row_ = 0;
        pq_row_count_ = pq_cols_.at(0).row_count_;
        opened_ = true;
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (3 == MY_SPEC.format_) {
    glob_idx_ = 0;
  } else if (OB_FAIL(reader_.reset())) {
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(reader_.close())) {
  } else {
    opened_ = false;
  }
  return ret;
}

void ObFileTableOp::destroy()
{
  reader_.close();
  opened_ = false;
  ObOperator::destroy();
}

int ObFileTableOp::write_obj(ObExpr *expr, const common::ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    common::ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (OB_FAIL(datum.from_obj(obj, expr->obj_datum_map_))) {
      LOG_WARN("convert obj to datum failed", K(ret), K(obj));
    }
  }
  return ret;
}

int ObFileTableOp::write_null(ObExpr *expr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    expr->locate_datum_for_write(eval_ctx_).set_null();
  }
  return ret;
}

int ObFileTableOp::convert_field(const common::ObString &field, const int32_t type, ObExpr *expr)
{
  int ret = OB_SUCCESS;
  common::ObObj obj;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_file_field_to_obj(field, type, obj))) {
  } else {
    common::ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (OB_FAIL(datum.from_obj(obj, expr->obj_datum_map_))) {
      LOG_WARN("convert obj to datum failed", K(ret), K(obj), K(type));
    }
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &spec = MY_SPEC;
  common::ObSEArray<common::ObString, 32> fields;
  clear_evaluated_flag();
  row_alloc_.reuse();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (3 == spec.format_) {  // GLOB
    if (glob_idx_ >= static_cast<int64_t>(glob_files_.size())) {
      ret = OB_ITER_END;
    } else {
      const std::string &path = glob_files_.at(glob_idx_++);
      // count(*) does not reference the file column at all
      if (spec.column_exprs_.count() > 0) {
        common::ObString value(static_cast<int32_t>(path.size()), path.data());
        if (OB_FAIL(convert_field(value, ObVarcharType, spec.column_exprs_.at(0)))) {
        } else {
          spec.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  } else if (2 == spec.format_) {  // PARQUET
    if (pq_row_ >= pq_row_count_) {
      ret = OB_ITER_END;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i) {
        ObExpr *expr = spec.column_exprs_.at(i);
        const int32_t index = spec.column_indexes_.at(i);
        common::ObObj obj;
        if (index < 0 || index >= static_cast<int32_t>(pq_cols_.size())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("parquet column index out of range", K(ret), K(index),
                   K(pq_cols_.size()));
        } else {
          const ObParquetColumn &col = pq_cols_.at(static_cast<size_t>(index));
          const int64_t row = pq_row_;
          if (row >= col.row_count_ ||
              row >= static_cast<int64_t>(col.is_null_.size())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (0 != col.is_null_.at(static_cast<size_t>(row))) {
            if (OB_FAIL(write_null(expr))) {
            }
          } else if (ObDoubleType == col.type_) {
            obj.set_double(col.doubles_.at(static_cast<size_t>(row)));
          } else if (ObVarcharType == col.type_) {
            const std::string &str = col.strings_.at(static_cast<size_t>(row));
            obj.set_varchar(common::ObString(static_cast<int32_t>(str.size()), str.data()));
            obj.set_collation_type(common::CS_TYPE_UTF8MB4_GENERAL_CI);
          } else if (ObTinyIntType == col.type_) {
            obj.set_tinyint(static_cast<int8_t>(col.ints_.at(static_cast<size_t>(row))));
          } else if (ObDateType == col.type_) {
            obj.set_date(static_cast<int32_t>(col.ints_.at(static_cast<size_t>(row))));
          } else if (ObDateTimeType == col.type_) {
            obj.set_datetime(col.ints_.at(static_cast<size_t>(row)));
          } else {
            obj.set_int(col.ints_.at(static_cast<size_t>(row)));
          }
          if (OB_SUCC(ret) && OB_FAIL(write_obj(expr, obj))) {
            LOG_WARN("write parquet value failed", K(ret), K(index), K(col.type_));
          }
        }
        if (OB_SUCC(ret)) {
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
      if (OB_SUCC(ret)) {
        ++pq_row_;
      }
    }
  } else if (1 == spec.format_) {  // JSONL
    common::ObString line;
    common::ObSEArray<common::ObString, 16> keys;
    common::ObSEArray<common::ObString, 16> values;
    common::ObSEArray<int32_t, 16> kinds;
    if (OB_FAIL(reader_.next_line(row_alloc_, line))) {
    } else if (line.empty()) {
      // skip blank line and try the next one
      ret = inner_get_next_row();
    } else if (OB_FAIL(ob_jsonl_parse_line(line, row_alloc_, keys, values, kinds))) {
      LOG_WARN("parse jsonl line failed", K(ret), K(line));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i) {
        ObExpr *expr = spec.column_exprs_.at(i);
        const int32_t index = spec.column_indexes_.at(i);
        int64_t found = -1;
        for (int64_t k = 0; k < keys.count(); ++k) {
          if (index < spec.column_names_.count() &&
              keys.at(k).length() == spec.column_names_.at(index).length() &&
              0 == strncasecmp(keys.at(k).ptr(), spec.column_names_.at(index).ptr(),
                               static_cast<size_t>(keys.at(k).length()))) {
            found = k;
            break;
          }
        }
        if (found < 0 || 3 == kinds.at(found)) {
          if (OB_FAIL(write_null(expr))) {
          }
        } else {
          const int32_t type = index < spec.column_types_.count()
              ? spec.column_types_.at(index) : ObVarcharType;
          if (OB_FAIL(convert_field(values.at(found), type, expr))) {
            LOG_WARN("convert jsonl field failed", K(ret), K(index), K(type));
          }
        }
        if (OB_SUCC(ret)) {
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  } else if (OB_FAIL(reader_.next_record(row_alloc_, fields))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i) {
      ObExpr *expr = spec.column_exprs_.at(i);
      int32_t index = spec.column_indexes_.at(i);
      if (index < 0 || index >= fields.count() || fields.at(index).empty()) {
        if (OB_FAIL(write_null(expr))) {
        }
      } else {
        int32_t type = index < spec.column_types_.count() ? spec.column_types_.at(index) : ObVarcharType;
        if (OB_FAIL(convert_field(fields.at(index), type, expr))) {
          LOG_WARN("convert csv field failed", K(ret), "column_index", index, K(type));
        }
      }
      if (OB_SUCC(ret)) {
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
