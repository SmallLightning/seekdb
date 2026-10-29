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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/optimizer/ob_optimizer_util.h"
#include "sql/engine/basic/ob_file_reader.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        bool describe_handled = false;
        if (OB_FAIL(build_file_table_describe(*child_stmt, describe_handled))) {
          LOG_WARN("build file table describe failed", K(ret));
        } else if (describe_handled) {
          return ret;
        }
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

// DESCRIBE over a file table does not describe a table schema, it returns the
// schema infered from the file.  Build "select 'name', 'type', ... union all
// ..." for the client, so the output looks like DuckDB's DESCRIBE.
int ObExplainResolver::build_file_table_describe(ObStmt &child_stmt, bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  static const char *COL_NAMES[6] = {"column_name", "column_type", "null",
                                    "key", "default", "extra"};
  ObSelectStmt *select_stmt = NULL;
  const TableItem *table_item = NULL;
  const ObFileTableDef *def = NULL;
  ObSEArray<ObSelectStmt *, 32> branches;

  if (stmt::T_SELECT != child_stmt.get_stmt_type()) {
    // not a select stmt, keep the normal explain path
  } else if (OB_ISNULL(select_stmt = static_cast<ObSelectStmt *>(&child_stmt))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (1 != select_stmt->get_table_size() ||
             OB_ISNULL(table_item = select_stmt->get_table_item(0))) {
    // joins / multiple tables keep the normal explain path
  } else if (!table_item->is_file_table() || OB_ISNULL(def = table_item->file_table_def_)) {
    // not a file table
  } else if (0 == def->column_names_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t r = 0; OB_SUCC(ret) && r < def->column_names_.count(); ++r) {
      ObSelectStmt *branch = NULL;
      ObString values[6];
      values[0] = def->column_names_.at(r);
      values[1] = ObString::make_string(ob_file_type_name(def->column_types_.at(r)));
      values[2] = ObString::make_string(def->column_nullable_.at(r) ? "YES" : "NO");
      values[3] = ObString::make_string("NULL");
      values[4] = ObString::make_string("NULL");
      values[5] = ObString::make_string("NULL");
      if (OB_ISNULL(branch = create_stmt<ObSelectStmt>())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("create describe branch stmt failed", K(ret));
      } else {
        for (int64_t c = 0; OB_SUCC(ret) && c < 6; ++c) {
          ObConstRawExpr *expr = NULL;
          SelectItem item;
          if (OB_FAIL(ObRawExprUtils::build_const_string_expr(*params_.expr_factory_,
                                                             ObVarcharType, values[c],
                                                             CS_TYPE_UTF8MB4_GENERAL_CI,
                                                             expr))) {
            LOG_WARN("build const string expr failed", K(ret));
          } else if (OB_ISNULL(expr)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            item.expr_ = expr;
            item.alias_name_ = ObString::make_string(COL_NAMES[c]);
            item.expr_name_ = item.alias_name_;
            item.is_real_alias_ = true;
            if (OB_FAIL(branch->add_select_item(item))) {
              LOG_WARN("add describe select item failed", K(ret));
            }
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(branches.push_back(branch))) {
          LOG_WARN("push describe branch failed", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (1 == branches.count()) {
        // a union needs at least two branches; stmt_ already points to the
        // single branch created above (create_stmt updates it)
        handled = true;
      } else {
        ObSelectStmt *top = create_stmt<ObSelectStmt>();
        if (OB_ISNULL(top)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          top->assign_set_op(ObSelectStmt::UNION);
          for (int64_t i = 0; OB_SUCC(ret) && i < branches.count(); ++i) {
            if (OB_FAIL(top->add_set_query(branches.at(i)))) {
              LOG_WARN("add describe set query failed", K(ret));
            }
          }
          // the target list of a set stmt is derived from its branches
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(ObOptimizerUtil::gen_set_target_list(allocator_, session_info_,
                                                                 params_.expr_factory_, top))) {
            LOG_WARN("generate describe target list failed", K(ret));
          } else {
            handled = true;
          }
        }
      }
    }
  }
  return ret;
}
