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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/engine/basic/ob_file_reader.h"
#include "lib/charset/ob_charset.h"
#include "lib/allocator/page_arena.h"

#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), has_correlated_expr_(false)
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  bool has_correlated_expr_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_table_(NULL) 
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
  int (ObFunctionTableOp::*next_row_func_)();
};

// Physical spec of a local file scan produced by read_csv()/read_json()/...
class ObFileTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      file_name_(),
      column_exprs_(alloc),
      column_indexes_(alloc),
      column_types_(alloc),
      column_names_(alloc),
      format_(0),
      has_header_(true),
      field_delim_(','),
      field_quote_('"'),
      file_size_(0),
      file_mtime_(0)
  {}

  common::ObString file_name_;
  // exprs of the columns actually referenced by the query
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  // index of each referenced column in the file schema
  common::ObFixedArray<int32_t, common::ObIAllocator> column_indexes_;
  // full infered schema, kept to know when to stop reading fields
  common::ObFixedArray<int32_t, common::ObIAllocator> column_types_;
  // column names, used to map jsonl object keys
  common::ObFixedArray<common::ObString, common::ObIAllocator> column_names_;
  int64_t format_;
  bool has_header_;
  char field_delim_;
  char field_quote_;
  // snapshot taken at resolve time, used to detect a changed file
  int64_t file_size_;
  int64_t file_mtime_;
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      reader_(),
      opened_(false),
      row_alloc_()
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int convert_field(const common::ObString &field, const int32_t type, ObExpr *expr);
  int write_obj(ObExpr *expr, const common::ObObj &obj);
  int write_null(ObExpr *expr);

private:
  ObCsvFileReader reader_;
  bool opened_;
  common::ObArenaAllocator row_alloc_;
  // glob() results, filled once in inner_open
  std::vector<std::string> glob_files_;
  int64_t glob_idx_ = 0;
  // parquet columns, materialised in inner_open
  std::vector<ObParquetColumn> pq_cols_;
  int64_t pq_row_ = 0;
  int64_t pq_row_count_ = 0;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
