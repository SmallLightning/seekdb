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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_READER_H_

#include <cstdio>
#include <string>
#include <vector>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace common
{
class ObObj;
}
namespace sql
{

// Description of a local file exposed as a table by read_csv()/read_json()/...
struct ObFileTableDef
{
  enum FileFormat
  {
    FILE_FORMAT_CSV = 0,
    FILE_FORMAT_JSONL = 1,
    FILE_FORMAT_PARQUET = 2,
    FILE_FORMAT_GLOB = 3,
    FILE_FORMAT_MAX
  };

  ObFileTableDef()
    : format_(FILE_FORMAT_CSV),
      has_header_(true),
      field_delim_(','),
      field_quote_('"'),
      file_name_(),
      file_size_(0),
      file_mtime_(0),
      column_names_(),
      column_types_(),
      column_nullable_()
  {}

  int64_t format_;
  bool has_header_;
  char field_delim_;
  char field_quote_;
  common::ObString file_name_;
  int64_t file_size_;
  int64_t file_mtime_;
  // Union schema of the file.
  common::ObSEArray<common::ObString, 8, common::ModulePageAllocator, true> column_names_;
  common::ObSEArray<int32_t, 8, common::ModulePageAllocator, true> column_types_;
  common::ObSEArray<int32_t, 8, common::ModulePageAllocator, true> column_nullable_;
};

// Streaming CSV record reader: keeps a 64KB window and returns one record at a
// time, so a scan never materialises the whole file.
class ObCsvFileReader
{
public:
  ObCsvFileReader();
  ~ObCsvFileReader();
  // has_header: skip the first record when reading data.
  int open(const char *path, const bool has_header, const char delim, const char quote);
  int close();
  // Rewind to the first data record.
  int reset();
  // Returns OB_ITER_END when there is no more record.
  int next_record(common::ObIAllocator &alloc, common::ObIArray<common::ObString> &fields);
  // Raw line reader, used by the jsonl scan.
  int next_line(common::ObIAllocator &alloc, common::ObString &line);
  int64_t get_field_count() const { return field_count_; }
  bool is_open() const { return NULL != fp_; }

private:
  int get_char(char &c);
  int peek_char(char &c);
  int skip_bom();
  int skip_header();
  int read_field(std::string &field, bool &row_end);

private:
  FILE *fp_;
  char buf_[65536];
  int64_t buf_pos_;
  int64_t buf_len_;
  bool eof_;
  bool has_header_;
  char delim_;
  char quote_;
  int64_t field_count_;

  DISALLOW_COPY_AND_ASSIGN(ObCsvFileReader);
};

// Infer the CSV schema by streaming the whole file once.  Only one record is
// kept in memory at any time.
int ob_csv_infer_schema(common::ObIAllocator &alloc,
                        const common::ObString &path,
                        const char delim,
                        const char quote,
                        const bool has_header,
                        common::ObIArray<common::ObString> &names,
                        common::ObIArray<int32_t> &types,
                        common::ObIArray<int32_t> &nullable);

// Convert one textual field to the given ObObjType.
int ob_file_field_to_obj(const common::ObString &field, const int32_t type, common::ObObj &obj);

// Human readable type name used by DESCRIBE.
const char *ob_file_type_name(const int32_t type);

// Parse one flat JSON object line into key/value pairs.  kind: 0 string,
// 1 number, 2 bool, 3 null.
int ob_jsonl_parse_line(const common::ObString &line,
                        common::ObIAllocator &alloc,
                        common::ObIArray<common::ObString> &keys,
                        common::ObIArray<common::ObString> &values,
                        common::ObIArray<int32_t> &kinds);

// Infer the union schema of a jsonl file.
int ob_jsonl_infer_schema(common::ObIAllocator &alloc,
                          const common::ObString &path,
                          common::ObIArray<common::ObString> &names,
                          common::ObIArray<int32_t> &types,
                          common::ObIArray<int32_t> &nullable);

// Stream the csv file once and mark the columns that contain an empty field.
int ob_csv_scan_nullable(const common::ObString &path,
                         const char delim,
                         const char quote,
                         const bool has_header,
                         common::ObIArray<int32_t> &nullable);

// One decoded parquet column.  The value arrays are all aligned with the row
// index; a null row keeps a placeholder value and sets is_null_[row] to 1.
struct ObParquetColumn
{
  ObParquetColumn() : name_(), type_(0), nullable_(false), row_count_(0) {}
  common::ObString name_;
  int32_t type_;      // ObObjType
  bool nullable_;
  int64_t row_count_;
  std::vector<int64_t> ints_;
  std::vector<double> doubles_;
  std::vector<std::string> strings_;
  std::vector<uint8_t> is_null_;
};

// Minimal parquet reader: flat schema, any number of row groups, PLAIN and
// dictionary encoded pages, UNCOMPRESSED and SNAPPY codecs, optional columns.
// read_values = false only parses the footer and reports the schema.
int ob_parquet_read(const common::ObString &path,
                    common::ObIAllocator &alloc,
                    std::vector<ObParquetColumn> &columns,
                    const bool read_values);

// Resolve a path against secure_file_priv.  Both arguments must be canonical
// absolute paths; an empty priv means "no restriction".
int ob_check_secure_file_priv(const common::ObString &priv,
                              const common::ObString &canonical_path,
                              bool &allowed);

} // namespace sql
} // namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_READER_H_ */
