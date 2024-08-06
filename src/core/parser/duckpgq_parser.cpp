
#include "duckpgq/core/parser/duckpgq_parser.hpp"

#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include <duckdb/parser/parsed_data/create_table_info.hpp>
#include <duckdb/parser/parser.hpp>
#include <duckdb/parser/statement/copy_statement.hpp>
#include <duckdb/parser/statement/create_statement.hpp>
#include <duckdb/parser/statement/insert_statement.hpp>
#include <duckpgq/core/functions/table/create_property_graph.hpp>
#include <duckpgq_state.hpp>

#include <duckpgq/core/functions/table/describe_property_graph.hpp>
#include <duckpgq/core/functions/table/drop_property_graph.hpp>

namespace duckpgq {

namespace core {

ParserExtensionParseResult duckpgq_parse(ParserExtensionInfo *info,
                                         const std::string &query) {
  auto parse_info = (DuckPGQParserExtensionInfo &)(info);
  Parser parser;
  parser.ParseQuery((query[0] == '-') ? query.substr(1, query.length())
                                      : query);
  if (parser.statements.size() != 1) {
    throw Exception(ExceptionType::PARSER,
                    "More than one statement detected, please only give one.");
  }
  return ParserExtensionParseResult(
      make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(
          std::move(parser.statements[0])));
}



void duckpgq_find_match_function(TableRef *table_ref,
                                 DuckPGQState &duckpgq_state) {
  if (auto table_function_ref = dynamic_cast<TableFunctionRef *>(table_ref)) {
    // Handle TableFunctionRef case
    auto function =
        dynamic_cast<FunctionExpression *>(table_function_ref->function.get());
    if (function->function_name != "duckpgq_match") {
      return;
    }
    int32_t match_index = duckpgq_state.match_index++;
    duckpgq_state.transform_expression[match_index] =
        std::move(function->children[0]);
    function->children.pop_back();
    auto function_identifier =
        make_uniq<ConstantExpression>(Value::CreateValue(match_index));
    function->children.push_back(std::move(function_identifier));
  } else if (auto join_ref = dynamic_cast<JoinRef *>(table_ref)) {
    // Handle JoinRef case
    duckpgq_find_match_function(join_ref->left.get(), duckpgq_state);
    duckpgq_find_match_function(join_ref->right.get(), duckpgq_state);
  }
}


ParserExtensionPlanResult
duckpgq_handle_statement(SQLStatement *statement, DuckPGQState &duckpgq_state) {
  if (statement->type == StatementType::SELECT_STATEMENT) {
    const auto select_statement = dynamic_cast<SelectStatement *>(statement);
    const auto select_node =
        dynamic_cast<SelectNode *>(select_statement->node.get());
    const auto describe_node =
        dynamic_cast<ShowRef *>(select_node->from_table.get());
    if (describe_node) {
      ParserExtensionPlanResult result;
      result.function = DescribePropertyGraphFunction();
      result.requires_valid_transaction = true;
      result.return_type = StatementReturnType::QUERY_RESULT;
      return result;
    }
    auto cte_keys = select_node->cte_map.map.Keys();
    for (auto &key : cte_keys) {
      auto cte = select_node->cte_map.map.find(key);
      auto cte_select_statement = dynamic_cast<SelectStatement *>(cte->second->query.get());
      if (cte_select_statement == nullptr) {
        continue; // Skip non-select statements
      }
      auto cte_node = dynamic_cast<SelectNode *>(cte_select_statement->node.get());
      if (cte_node) {
        duckpgq_find_match_function(cte_node->from_table.get(), duckpgq_state);
      }
    }
    duckpgq_find_match_function(select_node->from_table.get(), duckpgq_state);
    throw Exception(ExceptionType::BINDER, "use duckpgq_bind instead");
  }
  if (statement->type == StatementType::CREATE_STATEMENT) {
    const auto &create_statement = statement->Cast<CreateStatement>();
    const auto create_property_graph =
        dynamic_cast<CreatePropertyGraphInfo *>(create_statement.info.get());
    if (create_property_graph) {
      ParserExtensionPlanResult result;
      result.function = CreatePropertyGraphFunction();
      result.requires_valid_transaction = true;
      result.return_type = StatementReturnType::QUERY_RESULT;
      return result;
    }
    const auto create_table =
        reinterpret_cast<CreateTableInfo *>(create_statement.info.get());
    duckpgq_handle_statement(create_table->query.get(), duckpgq_state);
  }
  if (statement->type == StatementType::DROP_STATEMENT) {
    ParserExtensionPlanResult result;
    result.function = DropPropertyGraphFunction();
    result.requires_valid_transaction = true;
    result.return_type = StatementReturnType::QUERY_RESULT;
    return result;
  }
  if (statement->type == StatementType::EXPLAIN_STATEMENT) {
    auto &explain_statement = statement->Cast<ExplainStatement>();
    duckpgq_handle_statement(explain_statement.stmt.get(), duckpgq_state);
  }
  if (statement->type == StatementType::COPY_STATEMENT) {
    const auto &copy_statement = statement->Cast<CopyStatement>();
    const auto select_node =
        dynamic_cast<SelectNode *>(copy_statement.info->select_statement.get());
    duckpgq_find_match_function(select_node->from_table.get(), duckpgq_state);
    throw Exception(ExceptionType::BINDER, "use duckpgq_bind instead");
  }
  if (statement->type == StatementType::INSERT_STATEMENT) {
    const auto &insert_statement = statement->Cast<InsertStatement>();
    duckpgq_handle_statement(insert_statement.select_statement.get(),
                             duckpgq_state);
  }

  throw Exception(ExceptionType::NOT_IMPLEMENTED,
                  StatementTypeToString(statement->type) +
                      "has not been implemented yet for DuckPGQ queries");
}

ParserExtensionPlanResult
duckpgq_plan(ParserExtensionInfo *, ClientContext &context,
             unique_ptr<ParserExtensionParseData> parse_data) {
  auto duckpgq_state_entry = context.registered_state.find("duckpgq");
  DuckPGQState *duckpgq_state;
  if (duckpgq_state_entry == context.registered_state.end()) {
    auto state = make_shared_ptr<DuckPGQState>(std::move(parse_data));
    context.registered_state["duckpgq"] = state;
    duckpgq_state = state.get();
  } else {
    duckpgq_state = (DuckPGQState *)duckpgq_state_entry->second.get();
    duckpgq_state->parse_data = std::move(parse_data);
  }
  auto duckpgq_parse_data =
      dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());

  if (!duckpgq_parse_data) {
    throw Exception(ExceptionType::BINDER, "No DuckPGQ parse data found");
  }

  auto statement = duckpgq_parse_data->statement.get();
  return duckpgq_handle_statement(statement, *duckpgq_state);
}


//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------
void CorePGQParser::RegisterPGQParserExtension(
    DatabaseInstance &db) {
  auto &config = DBConfig::GetConfig(db);
  DuckPGQParserExtension pgq_parser;
  config.parser_extensions.push_back(pgq_parser);
}

} // namespace core

} // namespace duckpgq