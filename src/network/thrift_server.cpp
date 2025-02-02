// Copyright(C) 2023 InfiniFlow, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module;

#include <memory>
#include <thrift/TToString.h>
#include <thrift/concurrency/ThreadFactory.h>
#include <thrift/concurrency/ThreadManager.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TNonblockingServer.h>
#include <thrift/server/TThreadPoolServer.h>
#include <thrift/server/TThreadedServer.h>
#include <thrift/transport/TNonblockingServerSocket.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>

#include "infinity_thrift/InfinityService.h"
#include "infinity_thrift/infinity_types.h"
#include "statement/explain_statement.h"
#include "statement/extra/extra_ddl_info.h"
#include "statement/statement_common.h"

module thrift_server;

import infinity;
import stl;
import infinity_exception;
import logger;
import query_result;
import column_vector;
import data_block;
import value;
import third_party;

import file_writer;
import table_def;
import file_system_type;
import file_system;
import local_file_system;
import infinity_context;
import config;
import data_block;
import query_options;
import status;
import logical_type;
import internal_types;
import embedding_info;
import constant_expr;
import column_expr;
import function_expr;
import knn_expr;
import match_expr;
import fusion_expr;
import parsed_expr;
import update_statement;
import search_expr;
import create_index_info;
import extra_ddl_info;
import column_def;
import statement_common;
import data_type;
import type_info;

using namespace apache::thrift;
using namespace apache::thrift::concurrency;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::server;

namespace infinity {

constexpr String kErrorMsgHeader = "[THRIFT ERROR]";

class InfinityServiceHandler final : public infinity_thrift_rpc::InfinityServiceIf {
public:
    InfinityServiceHandler() = default;

    void Connect(infinity_thrift_rpc::CommonResponse &response) final {
        auto infinity = Infinity::RemoteConnect();
        std::lock_guard<std::mutex> lock(infinity_session_map_mutex_);
        infinity_session_map_.emplace(infinity->GetSessionId(), infinity);
        response.__set_session_id(infinity->GetSessionId());
        response.__set_error_code((i64)(ErrorCode::kOk));
        LOG_TRACE(fmt::format("THRIFT: Connect success, new session {}", response.session_id));
    }

    void Disconnect(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::CommonRequest &request) final {
        auto status = GetAndRemoveSessionID(request.session_id);
        if (status.ok()) {
            response.__set_error_code((i64)(status.code()));
            LOG_TRACE(fmt::format("THRIFT: Disconnect session {} success", request.session_id));
        } else {
            response.__set_error_code((i64)(status.code()));
            response.__set_error_msg(status.message());
            LOG_TRACE(fmt::format("THRIFT: Disconnect session {} failed", request.session_id));
        }
    }

    void CreateDatabase(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::CreateDatabaseRequest &request) final {
        CreateDatabaseOptions create_database_opts;
        switch (request.create_option.conflict_type) {
            case infinity_thrift_rpc::CreateConflict::Ignore: {
                create_database_opts.conflict_type_ = ConflictType::kIgnore;
                break;
            }
            case infinity_thrift_rpc::CreateConflict::Error: {
                create_database_opts.conflict_type_ = ConflictType::kError;
                break;
            }
            case infinity_thrift_rpc::CreateConflict::Replace: {
                create_database_opts.conflict_type_ = ConflictType::kReplace;
                break;
            }
            default: {
                ProcessStatus(response, Status::InvalidConflictType());
                return;
            }
        }

        auto [infinity, status] = GetInfinityBySessionID(request.session_id);
        if (status.ok()) {
            auto result = infinity->CreateDatabase(request.db_name, create_database_opts);
            ProcessQueryResult(response, result);
        } else {
            ProcessStatus(response, status);
        }
    }

    void DropDatabase(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::DropDatabaseRequest &request) final {

        DropDatabaseOptions drop_database_opts;
        switch (request.drop_option.conflict_type) {
            case infinity_thrift_rpc::DropConflict::Ignore: {
                drop_database_opts.conflict_type_ = ConflictType::kIgnore;
                break;
            }
            case infinity_thrift_rpc::DropConflict::Error: {
                drop_database_opts.conflict_type_ = ConflictType::kError;
                break;
            }
            default: {
                ProcessStatus(response, Status::InvalidConflictType());
                return;
            }
        }

        auto [infinity, status] = GetInfinityBySessionID(request.session_id);
        if (status.ok()) {
            auto result = infinity->DropDatabase(request.db_name, drop_database_opts);
            ProcessQueryResult(response, result);
        } else {
            ProcessStatus(response, status);
        }
    }

    void CreateTable(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::CreateTableRequest &request) final {
        Vector<ColumnDef *> column_defs;

        for (auto &proto_column_def : request.column_defs) {
            auto [column_def, column_def_status] = GetColumnDefFromProto(proto_column_def);
            if (!column_def_status.ok()) {
                ProcessStatus(response, column_def_status);
                return;
            }
            column_defs.emplace_back(column_def);
        }

        CreateTableOptions create_table_opts;
        switch (request.create_option.conflict_type) {
            case infinity_thrift_rpc::CreateConflict::Ignore: {
                create_table_opts.conflict_type_ = ConflictType::kIgnore;
                break;
            }
            case infinity_thrift_rpc::CreateConflict::Error: {
                create_table_opts.conflict_type_ = ConflictType::kError;
                break;
            }
            case infinity_thrift_rpc::CreateConflict::Replace: {
                create_table_opts.conflict_type_ = ConflictType::kReplace;
                break;
            }
            default: {
                ProcessStatus(response, Status::InvalidConflictType());
                return;
            }
        }

        SizeT properties_count = request.create_option.properties.size();
        create_table_opts.properties_.reserve(properties_count);
        for (SizeT idx = 0; idx < properties_count; ++idx) {
            InitParameter *property = new InitParameter();
            property->param_name_ = request.create_option.properties[idx].key;
            property->param_value_ = request.create_option.properties[idx].value;
            create_table_opts.properties_.emplace_back(property);
        }

        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        auto result = infinity->CreateTable(request.db_name, request.table_name, column_defs, Vector<TableConstraint *>(), create_table_opts);
        ProcessQueryResult(response, result);
    }

    void DropTable(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::DropTableRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        DropTableOptions drop_table_opts;
        switch (request.drop_option.conflict_type) {
            case infinity_thrift_rpc::DropConflict::Ignore: {
                drop_table_opts.conflict_type_ = ConflictType::kIgnore;
                break;
            }
            case infinity_thrift_rpc::DropConflict::Error: {
                drop_table_opts.conflict_type_ = ConflictType::kError;
                break;
            }
            default: {
                ProcessStatus(response, Status::InvalidConflictType());
                return;
            }
        }

        auto result = infinity->DropTable(request.db_name, request.table_name, drop_table_opts);
        ProcessQueryResult(response, result);
    }

    void Insert(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::InsertRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        if (request.fields.empty()) {
            ProcessStatus(response, Status::InsertWithoutValues());
            return;
        }

        auto columns = new Vector<String>();
        columns->reserve(request.column_names.size());
        for (auto &column : request.column_names) {
            columns->emplace_back(column);
        }

        Status constant_status;

        Vector<Vector<ParsedExpr *> *> *values = new Vector<Vector<ParsedExpr *> *>();
        values->reserve(request.fields.size());
        for (auto &value : request.fields) {
            auto value_list = new Vector<ParsedExpr *>();
            value_list->reserve(value.parse_exprs.size());
            for (auto &expr : value.parse_exprs) {
                auto parsed_expr = GetConstantFromProto(constant_status, *expr.type.constant_expr);
                if (!constant_status.ok()) {
                    // Free values memory
                    if (values != nullptr) {
                        for (auto &value_array : *values) {
                            for (auto &value_ptr : *value_array) {
                                delete value_ptr;
                                value_ptr = nullptr;
                            }
                            delete value_array;
                            value_array = nullptr;
                        }
                        delete values;
                        values = nullptr;
                    }
                    // Free current value list memory
                    if (value_list != nullptr) {
                        for (auto &value_ptr : *value_list) {
                            delete value_ptr;
                            value_ptr = nullptr;
                        }
                        delete value_list;
                        value_list = nullptr;
                    }

                    if (parsed_expr != nullptr) {
                        delete parsed_expr;
                        parsed_expr = nullptr;
                    }

                    ProcessStatus(response, constant_status);
                    return;
                }
                value_list->emplace_back(parsed_expr);
            }
            values->emplace_back(value_list);
        }

        auto result = infinity->Insert(request.db_name, request.table_name, columns, values);
        ProcessQueryResult(response, result);
    }

    Tuple<CopyFileType, Status> GetCopyFileType(infinity_thrift_rpc::CopyFileType::type copy_file_type) {
        switch (copy_file_type) {
            case infinity_thrift_rpc::CopyFileType::CSV:
                return {CopyFileType::kCSV, Status::OK()};
            case infinity_thrift_rpc::CopyFileType::JSON:
                return {CopyFileType::kJSON, Status::OK()};
            case infinity_thrift_rpc::CopyFileType::JSONL:
                return {CopyFileType::kJSONL, Status::OK()};
            case infinity_thrift_rpc::CopyFileType::FVECS:
                return {CopyFileType::kFVECS, Status::OK()};
            default: {
                return {CopyFileType::kInvalid, Status::ImportFileFormatError("Not implemented yet")};
            }
        }
    }

    void Import(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::ImportRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        Path path(fmt::format("{}_{}_{}_{}",
                              *InfinityContext::instance().config()->temp_dir().get(),
                              request.db_name,
                              request.table_name,
                              request.file_name));

        auto [copy_file_type, status] = GetCopyFileType(request.import_option.copy_file_type);
        if (!status.ok()) {
            ProcessStatus(response, status);
        }

        ImportOptions import_options;
        import_options.copy_file_type_ = copy_file_type;
        auto &delimiter_string = request.import_option.delimiter;
        if (import_options.copy_file_type_ == CopyFileType::kCSV && delimiter_string.size() != 1) {
            ProcessStatus(response, Status::SyntaxError("CSV file delimiter isn't a char."));
        }
        import_options.delimiter_ = delimiter_string[0];

        const QueryResult result = infinity->Import(request.db_name, request.table_name, path.c_str(), import_options);
        ProcessQueryResult(response, result);
    }

    void UploadFileChunk(infinity_thrift_rpc::UploadResponse &response, const infinity_thrift_rpc::FileChunk &request) final {
        LocalFileSystem fs;
        Path path(fmt::format("{}_{}_{}_{}",
                              *InfinityContext::instance().config()->temp_dir().get(),
                              request.db_name,
                              request.table_name,
                              request.file_name));
        if (request.index != 0) {
            FileWriter file_writer(fs, path.c_str(), request.data.size(), FileFlags::WRITE_FLAG | FileFlags::APPEND_FLAG);
            file_writer.Write(request.data.data(), request.data.size());
            file_writer.Flush();
        } else {
            // Check file exist
            if (fs.Exists(path.c_str())) {
                auto exist_file_size = LocalFileSystem::GetFileSizeByPath(path.c_str());
                if ((i64)exist_file_size != request.total_size) {
                    LOG_TRACE(fmt::format("Exist file size: {} , request total size: {}", exist_file_size, request.total_size));
                    fs.DeleteFile(path.c_str());
                } else {
                    response.__set_error_code((i64)(ErrorCode::kOk));
                    response.__set_can_skip(true);
                    return;
                }
            }
            FileWriter file_writer(fs, path.c_str(), request.data.size());
            file_writer.Write(request.data.data(), request.data.size());
            file_writer.Flush();
        }
        response.__set_error_code((i64)(ErrorCode::kOk));
        response.__set_can_skip(false);
        LOG_TRACE(fmt::format("Upload file name: {} , index: {}", path.c_str(), request.index));
    }

    void Select(infinity_thrift_rpc::SelectResponse &response, const infinity_thrift_rpc::SelectRequest &request) final {
        // ++count_;
        // auto start1 = std::chrono::steady_clock::now();

        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        // auto end1 = std::chrono::steady_clock::now();
        //
        // phase_1_duration_ += end1 - start1;
        //
        // auto start2 = std::chrono::steady_clock::now();

        // select list
        if (request.__isset.select_list == false or request.select_list.empty()) {
            ProcessStatus(response, Status::EmptySelectFields());
            return;
        }

        Vector<ParsedExpr *> *output_columns = new Vector<ParsedExpr *>();
        output_columns->reserve(request.select_list.size());

        Status parsed_expr_status;
        for (auto &expr : request.select_list) {
            auto parsed_expr = GetParsedExprFromProto(parsed_expr_status, expr);
            if (!parsed_expr_status.ok()) {

                if (output_columns != nullptr) {
                    for (auto &expr_ptr : *output_columns) {
                        delete expr_ptr;
                    }
                    delete output_columns;
                    output_columns = nullptr;
                }

                if (parsed_expr != nullptr) {
                    delete parsed_expr;
                    parsed_expr = nullptr;
                }

                ProcessStatus(response, parsed_expr_status);
                return;
            }
            output_columns->emplace_back(parsed_expr);
        }

        // search expr
        SearchExpr *search_expr = nullptr;
        if (request.__isset.search_expr) {
            search_expr = new SearchExpr();
            auto search_expr_list = new Vector<ParsedExpr *>();
            SizeT knn_expr_count = request.search_expr.knn_exprs.size();
            SizeT match_expr_count = request.search_expr.match_exprs.size();
            bool fusion_expr_exists = request.search_expr.__isset.fusion_expr;
            SizeT total_expr_count = knn_expr_count + match_expr_count + fusion_expr_exists;
            search_expr_list->reserve(total_expr_count);
            for (SizeT idx = 0; idx < knn_expr_count; ++idx) {
                auto [knn_expr, knn_expr_status] = GetKnnExprFromProto(request.search_expr.knn_exprs[idx]);
                if (!knn_expr_status.ok()) {

                    if (output_columns != nullptr) {
                        for (auto &expr_ptr : *output_columns) {
                            delete expr_ptr;
                        }
                        delete output_columns;
                        output_columns = nullptr;
                    }

                    if (search_expr_list != nullptr) {
                        for (auto &expr_ptr : *search_expr_list) {
                            delete expr_ptr;
                        }
                        delete search_expr_list;
                        search_expr_list = nullptr;
                    }

                    if (knn_expr != nullptr) {
                        delete knn_expr;
                        knn_expr = nullptr;
                    }

                    if (search_expr != nullptr) {
                        delete search_expr;
                        search_expr = nullptr;
                    }

                    ProcessStatus(response, knn_expr_status);
                    return;
                }
                search_expr_list->emplace_back(knn_expr);
            }

            for (SizeT idx = 0; idx < match_expr_count; ++idx) {
                ParsedExpr *match_expr = GetMatchExprFromProto(request.search_expr.match_exprs[idx]);
                search_expr_list->emplace_back(match_expr);
            }

            if (fusion_expr_exists) {
                ParsedExpr *fusion_expr = GetFusionExprFromProto(request.search_expr.fusion_expr);
                search_expr_list->emplace_back(fusion_expr);
            }

            search_expr->SetExprs(search_expr_list);
        }

        // filter
        ParsedExpr *filter = nullptr;
        if (request.__isset.where_expr == true) {
            filter = GetParsedExprFromProto(parsed_expr_status, request.where_expr);
            if (!parsed_expr_status.ok()) {

                if (output_columns != nullptr) {
                    for (auto &expr_ptr : *output_columns) {
                        delete expr_ptr;
                    }
                    delete output_columns;
                    output_columns = nullptr;
                }

                if (search_expr != nullptr) {
                    delete search_expr;
                    search_expr = nullptr;
                }

                if (filter != nullptr) {
                    delete filter;
                    filter = nullptr;
                }

                ProcessStatus(response, parsed_expr_status);
                return;
            }
        }

        // TODO:
        //    ParsedExpr *offset;
        // offset = new ParsedExpr();

        // limit
        //        ParsedExpr *limit = nullptr;
        //        if (request.__isset.limit_expr == true) {
        //            limit = GetParsedExprFromProto(request.limit_expr);
        //        }

        // auto end2 = std::chrono::steady_clock::now();
        // phase_2_duration_ += end2 - start2;
        //
        // auto start3 = std::chrono::steady_clock::now();

        const QueryResult result = infinity->Search(request.db_name, request.table_name, search_expr, filter, output_columns);

        // auto end3 = std::chrono::steady_clock::now();
        //
        // phase_3_duration_ += end3 - start3;
        //
        // auto start4 = std::chrono::steady_clock::now();

        if (result.IsOk()) {
            auto &columns = response.column_fields;
            columns.resize(result.result_table_->ColumnCount());
            ProcessDataBlocks(result, response, columns);
        } else {
            ProcessQueryResult(response, result);
        }

        // auto end4 = std::chrono::steady_clock::now();
        // phase_4_duration_ += end4 - start4;
        //
        // if (count_ % 10000 == 0) {
        //     LOG_ERROR(fmt::format("Phase 1: {} Phase 2: {} Phase 3: {} Phase 4: {}  Total: {} seconds",
        //                      phase_1_duration_.count(),
        //                      phase_2_duration_.count(),
        //                      phase_3_duration_.count(),
        //                      phase_4_duration_.count(),
        //                      (phase_1_duration_ + phase_2_duration_ + phase_3_duration_ + phase_4_duration_).count()));
        //     phase_1_duration_ = std::chrono::duration<double>();
        //     phase_2_duration_ = std::chrono::duration<double>();
        //     phase_3_duration_ = std::chrono::duration<double>();
        //     phase_4_duration_ = std::chrono::duration<double>();
        // } else if (count_ % 1000 == 0) {
        //     LOG_ERROR(fmt::format("Phase 1: {} Phase 2: {} Phase 3: {} Phase 4: {}  Total: {} seconds",
        //                      phase_1_duration_.count(),
        //                      phase_2_duration_.count(),
        //                      phase_3_duration_.count(),
        //                      phase_4_duration_.count(),
        //                      (phase_1_duration_ + phase_2_duration_ + phase_3_duration_ + phase_4_duration_).count()));
        // }
    }

    void Explain(infinity_thrift_rpc::SelectResponse &response, const infinity_thrift_rpc::ExplainRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        if (request.__isset.select_list == false) {
            ProcessStatus(response, Status::EmptySelectFields());
            return;
        }

        Vector<ParsedExpr *> *output_columns = new Vector<ParsedExpr *>();
        output_columns->reserve(request.select_list.size());

        Status parsed_expr_status;
        for (auto &expr : request.select_list) {
            auto parsed_expr = GetParsedExprFromProto(parsed_expr_status, expr);
            if (!parsed_expr_status.ok()) {

                if (output_columns != nullptr) {
                    for (auto &expr_ptr : *output_columns) {
                        delete expr_ptr;
                    }
                    delete output_columns;
                    output_columns = nullptr;
                }

                if (parsed_expr != nullptr) {
                    delete parsed_expr;
                    parsed_expr = nullptr;
                }

                ProcessStatus(response, parsed_expr_status);
                return;
            }
            output_columns->emplace_back(parsed_expr);
        }

        // search expr
        SearchExpr *search_expr = nullptr;
        if (request.__isset.search_expr) {
            search_expr = new SearchExpr();
            auto search_expr_list = new Vector<ParsedExpr *>();
            SizeT knn_expr_count = request.search_expr.knn_exprs.size();
            SizeT match_expr_count = request.search_expr.match_exprs.size();
            bool fusion_expr_exists = request.search_expr.__isset.fusion_expr;
            SizeT total_expr_count = knn_expr_count + match_expr_count + fusion_expr_exists;
            search_expr_list->reserve(total_expr_count);
            for (SizeT idx = 0; idx < knn_expr_count; ++idx) {
                auto [knn_expr, knn_expr_status] = GetKnnExprFromProto(request.search_expr.knn_exprs[idx]);
                if (!knn_expr_status.ok()) {

                    if (output_columns != nullptr) {
                        for (auto &expr_ptr : *output_columns) {
                            delete expr_ptr;
                        }
                        delete output_columns;
                        output_columns = nullptr;
                    }

                    if (search_expr_list != nullptr) {
                        for (auto &expr_ptr : *search_expr_list) {
                            delete expr_ptr;
                        }
                        delete search_expr_list;
                        search_expr_list = nullptr;
                    }

                    if (knn_expr != nullptr) {
                        delete knn_expr;
                        knn_expr = nullptr;
                    }

                    ProcessStatus(response, knn_expr_status);
                    return;
                }
                search_expr_list->emplace_back(knn_expr);
            }

            for (SizeT idx = 0; idx < match_expr_count; ++idx) {
                ParsedExpr *match_expr = GetMatchExprFromProto(request.search_expr.match_exprs[idx]);
                search_expr_list->emplace_back(match_expr);
            }

            if (fusion_expr_exists) {
                ParsedExpr *fusion_expr = GetFusionExprFromProto(request.search_expr.fusion_expr);
                search_expr_list->emplace_back(fusion_expr);
            }

            search_expr->SetExprs(search_expr_list);
        }

        // filter
        ParsedExpr *filter = nullptr;
        if (request.__isset.where_expr == true) {
            filter = GetParsedExprFromProto(parsed_expr_status, request.where_expr);
            if (!parsed_expr_status.ok()) {

                if (output_columns != nullptr) {
                    for (auto &expr_ptr : *output_columns) {
                        delete expr_ptr;
                    }
                    delete output_columns;
                    output_columns = nullptr;
                }

                if (search_expr != nullptr) {
                    delete search_expr;
                    search_expr = nullptr;
                }

                if (filter != nullptr) {
                    delete filter;
                    filter = nullptr;
                }

                ProcessStatus(response, parsed_expr_status);
                return;
            }
        }

        // TODO:
        //    ParsedExpr *offset;
        // offset = new ParsedExpr();

        // limit
        //        ParsedExpr *limit = nullptr;
        //        if (request.__isset.limit_expr == true) {
        //            limit = GetParsedExprFromProto(request.limit_expr);
        //        }

        // Explain type
        auto explain_type = GetExplainTypeFromProto(request.explain_type);
        const QueryResult result = infinity->Explain(request.db_name, request.table_name, explain_type, search_expr, filter, output_columns);

        if (result.IsOk()) {
            auto &columns = response.column_fields;
            columns.resize(result.result_table_->ColumnCount());
            ProcessDataBlocks(result, response, columns);
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ShowVariable(infinity_thrift_rpc::SelectResponse &response, const infinity_thrift_rpc::ShowVariableRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        const QueryResult result = infinity->ShowVariable(request.variable_name);
        if (result.IsOk()) {
            auto &columns = response.column_fields;
            columns.resize(result.result_table_->ColumnCount());
            ProcessDataBlocks(result, response, columns);
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void Delete(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::DeleteRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        ParsedExpr *filter = nullptr;
        if (request.__isset.where_expr == true) {
            Status parsed_expr_status;
            filter = GetParsedExprFromProto(parsed_expr_status, request.where_expr);
            if (!parsed_expr_status.ok()) {
                ProcessStatus(response, parsed_expr_status);
                return;
            }
        }

        const QueryResult result = infinity->Delete(request.db_name, request.table_name, filter);
        ProcessQueryResult(response, result);
    };

    void Update(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::UpdateRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        ParsedExpr *filter = nullptr;
        if (request.__isset.where_expr == true) {
            Status parsed_expr_status;
            filter = GetParsedExprFromProto(parsed_expr_status, request.where_expr);
            if (!parsed_expr_status.ok()) {
                ProcessStatus(response, parsed_expr_status);
                return;
            }
        }

        std::vector<UpdateExpr *> *update_expr_array{nullptr};
        if (request.__isset.update_expr_array == true) {
            update_expr_array = new std::vector<UpdateExpr *>();
            update_expr_array->reserve(request.update_expr_array.size());
            for (auto &update_expr : request.update_expr_array) {
                auto [parsed_expr, update_expr_status] = GetUpdateExprFromProto(update_expr);
                if (!update_expr_status.ok()) {

                    if (update_expr_array != nullptr) {
                        for (auto update_expr : *update_expr_array) {
                            delete update_expr;
                        }

                        delete update_expr_array;
                        update_expr_array = nullptr;
                    }

                    if (parsed_expr != nullptr) {
                        delete parsed_expr;
                        parsed_expr = nullptr;
                    }

                    ProcessStatus(response, update_expr_status);
                    return;
                }
                update_expr_array->emplace_back(parsed_expr);
            }
        }

        const QueryResult result = infinity->Update(request.db_name, request.table_name, filter, update_expr_array);
        ProcessQueryResult(response, result);
    }

    void ListDatabase(infinity_thrift_rpc::ListDatabaseResponse &response, const infinity_thrift_rpc::ListDatabaseRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        auto result = infinity->ListDatabases();
        response.__set_error_code((i64)(result.ErrorCode()));
        if (result.IsOk()) {
            SharedPtr<DataBlock> data_block = result.result_table_->GetDataBlockById(0);
            auto row_count = data_block->row_count();
            for (int i = 0; i < row_count; ++i) {
                Value value = data_block->GetValue(0, i);
                const String &db_name = value.GetVarchar();
                response.db_names.emplace_back(db_name);
            }
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ListTable(infinity_thrift_rpc::ListTableResponse &response, const infinity_thrift_rpc::ListTableRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        auto result = infinity->ListTables(request.db_name);
        if (result.IsOk()) {
            SharedPtr<DataBlock> data_block = result.result_table_->GetDataBlockById(0);
            auto row_count = data_block->row_count();
            for (int i = 0; i < row_count; ++i) {
                Value value = data_block->GetValue(1, i);
                const String &table_name = value.GetVarchar();
                response.table_names.emplace_back(table_name);
            }
            response.__set_error_code((i64)(result.ErrorCode()));
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ShowDatabase(infinity_thrift_rpc::ShowDatabaseResponse &response, const infinity_thrift_rpc::ShowDatabaseRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }
        const QueryResult result = infinity->ShowDatabase(request.db_name);
        if (result.IsOk()) {
            SharedPtr<DataBlock> data_block = result.result_table_->GetDataBlockById(0);
            auto row_count = data_block->row_count();
            if (row_count != 3) {
                UnrecoverableError("ShowDatabase: query result is invalid.");
            }

            {
                Value value = data_block->GetValue(1, 0);
                response.database_name = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 1);
                response.store_dir = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 2);
                response.table_count = value.value_.big_int;
            }

            response.__set_error_code((i64)(result.ErrorCode()));
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ShowTable(infinity_thrift_rpc::ShowTableResponse &response, const infinity_thrift_rpc::ShowTableRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        const QueryResult result = infinity->ShowTable(request.db_name, request.table_name);
        if (result.IsOk()) {
            SharedPtr<DataBlock> data_block = result.result_table_->GetDataBlockById(0);
            auto row_count = data_block->row_count();
            if (row_count != 6) {
                UnrecoverableError("ShowTable: query result is invalid.");
            }

            {
                Value value = data_block->GetValue(1, 0);
                response.database_name = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 1);
                response.table_name = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 2);
                response.store_dir = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 3);
                response.column_count = value.value_.big_int;
            }

            {
                Value value = data_block->GetValue(1, 4);
                response.segment_count = value.value_.big_int;
            }

            {
                Value value = data_block->GetValue(1, 5);
                response.row_count = value.value_.big_int;
            }

            response.__set_error_code((i64)(result.ErrorCode()));
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ShowColumns(infinity_thrift_rpc::SelectResponse &response, const infinity_thrift_rpc::ShowColumnsRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        const QueryResult result = infinity->ShowColumns(request.db_name, request.table_name);
        if (result.IsOk()) {
            auto &columns = response.column_fields;
            columns.resize(result.result_table_->ColumnCount());
            ProcessDataBlocks(result, response, columns);
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ShowTables(infinity_thrift_rpc::SelectResponse &response, const infinity_thrift_rpc::ShowTablesRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        const QueryResult result = infinity->ShowTables(request.db_name);
        if (result.IsOk()) {
            auto &columns = response.column_fields;
            columns.resize(result.result_table_->ColumnCount());
            ProcessDataBlocks(result, response, columns);
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void GetDatabase(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::GetDatabaseRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        QueryResult result = infinity->GetDatabase(request.db_name);
        ProcessQueryResult(response, result);
    }

    void GetTable(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::GetTableRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        QueryResult result = infinity->GetTable(request.db_name, request.table_name);
        ProcessQueryResult(response, result);
    }

    void CreateIndex(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::CreateIndexRequest &request) final {
        CreateIndexOptions create_index_opts;
        switch (request.create_option.conflict_type) {
            case infinity_thrift_rpc::CreateConflict::Ignore: {
                create_index_opts.conflict_type_ = ConflictType::kIgnore;
                break;
            }
            case infinity_thrift_rpc::CreateConflict::Error: {
                create_index_opts.conflict_type_ = ConflictType::kError;
                break;
            }
            case infinity_thrift_rpc::CreateConflict::Replace: {
                create_index_opts.conflict_type_ = ConflictType::kReplace;
                break;
            }
            default: {
                ProcessStatus(response, Status::InvalidConflictType());
                return;
            }
        }

        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        auto *index_info_list_to_use = new Vector<IndexInfo *>();
        for (auto &index_info : request.index_info_list) {
            auto index_info_to_use = new IndexInfo();
            index_info_to_use->index_type_ = GetIndexTypeFromProto(index_info.index_type);
            if (index_info_to_use->index_type_ == IndexType::kInvalid) {

                if (index_info_list_to_use != nullptr) {
                    for (auto &index_info : *index_info_list_to_use) {
                        delete index_info;
                        index_info = nullptr;
                    }
                    delete index_info_list_to_use;
                    index_info_list_to_use = nullptr;
                }

                delete index_info_to_use;
                index_info_to_use = nullptr;

                ProcessStatus(response, Status::InvalidIndexType());
                return;
            }

            index_info_to_use->column_name_ = index_info.column_name;

            auto *index_param_list = new Vector<InitParameter *>();
            for (auto &index_param : index_info.index_param_list) {
                auto init_parameter = new InitParameter();
                init_parameter->param_name_ = index_param.param_name;
                init_parameter->param_value_ = index_param.param_value;
                index_param_list->emplace_back(init_parameter);
            }
            index_info_to_use->index_param_list_ = index_param_list;

            index_info_list_to_use->emplace_back(index_info_to_use);
        }

        QueryResult result =
            infinity->CreateIndex(request.db_name, request.table_name, request.index_name, index_info_list_to_use, create_index_opts);
        ProcessQueryResult(response, result);
    }

    void DropIndex(infinity_thrift_rpc::CommonResponse &response, const infinity_thrift_rpc::DropIndexRequest &request) final {
        DropIndexOptions drop_index_opts;
        switch (request.drop_option.conflict_type) {
            case infinity_thrift_rpc::DropConflict::type::Ignore: {
                drop_index_opts.conflict_type_ = ConflictType::kIgnore;
                break;
            }
            case infinity_thrift_rpc::DropConflict::type::Error: {
                drop_index_opts.conflict_type_ = ConflictType::kError;
                break;
            }
            default: {
                ProcessStatus(response, Status::InvalidConflictType());
                return;
            }
        }

        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        QueryResult result = infinity->DropIndex(request.db_name, request.table_name, request.index_name, drop_index_opts);
        ProcessQueryResult(response, result);
    }

    void ListIndex(infinity_thrift_rpc::ListIndexResponse &response, const infinity_thrift_rpc::ListIndexRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        auto result = infinity->ListTableIndexes(request.db_name, request.table_name);
        if (result.IsOk()) {
            SharedPtr<DataBlock> data_block = result.result_table_->GetDataBlockById(0);
            auto row_count = data_block->row_count();
            for (int i = 0; i < row_count; ++i) {
                Value value = data_block->GetValue(0, i);
                const String &index_name = value.GetVarchar();
                response.index_names.emplace_back(index_name);
            }
            response.__set_error_code((i64)(result.ErrorCode()));
        } else {
            ProcessQueryResult(response, result);
        }
    }

    void ShowIndex(infinity_thrift_rpc::ShowIndexResponse &response, const infinity_thrift_rpc::ShowIndexRequest &request) final {
        auto [infinity, infinity_status] = GetInfinityBySessionID(request.session_id);
        if (!infinity_status.ok()) {
            ProcessStatus(response, infinity_status);
            return;
        }

        auto result = infinity->ShowIndex(request.db_name, request.table_name, request.index_name);

        if (result.IsOk()) {
            SharedPtr<DataBlock> data_block = result.result_table_->GetDataBlockById(0);
            auto row_count = data_block->row_count();
            if (row_count != 9) {
                UnrecoverableError("ShowIndex: query result is invalid.");
            }

            {
                Value value = data_block->GetValue(1, 0);
                response.db_name = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 1);
                response.table_name = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 2);
                response.index_name = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 3);
                response.index_type = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 4);
                response.index_column_names = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 5);
                response.index_column_ids = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 6);
                response.other_parameters = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 7);
                response.store_dir = value.GetVarchar();
            }

            {
                Value value = data_block->GetValue(1, 8);
                response.segment_index_count = value.GetVarchar();
            }

            response.__set_error_code((i64)(result.ErrorCode()));
        } else {
            ProcessQueryResult(response, result);
        }
    }

private:
    std::mutex infinity_session_map_mutex_{};
    HashMap<u64, SharedPtr<Infinity>> infinity_session_map_{};

    // SizeT count_ = 0;
    // std::chrono::duration<double> phase_1_duration_{};
    // std::chrono::duration<double> phase_2_duration_{};
    // std::chrono::duration<double> phase_3_duration_{};
    // std::chrono::duration<double> phase_4_duration_{};

private:
    Tuple<Infinity *, Status> GetInfinityBySessionID(i64 session_id) {
        std::lock_guard<std::mutex> lock(infinity_session_map_mutex_);
        auto iter = infinity_session_map_.find(session_id);
        if (iter == infinity_session_map_.end()) {
            return {nullptr, Status::SessionNotFound(session_id)};
        }
        return {iter->second.get(), Status::OK()};
    }

    Status GetAndRemoveSessionID(i64 session_id) {
        std::lock_guard<std::mutex> lock(infinity_session_map_mutex_);
        auto iter = infinity_session_map_.find(session_id);
        if (iter == infinity_session_map_.end()) {
            return Status::SessionNotFound(session_id);
        }
        iter->second->RemoteDisconnect();
        infinity_session_map_.erase(session_id);
        return Status::OK();
    }

    static Tuple<ColumnDef *, Status> GetColumnDefFromProto(const infinity_thrift_rpc::ColumnDef &column_def) {
        auto column_def_data_type_ptr = GetColumnTypeFromProto(column_def.data_type);
        if (column_def_data_type_ptr->type() == infinity::LogicalType::kInvalid) {
            return {nullptr, Status::InvalidDataType()};
        }

        auto constraints = HashSet<ConstraintType>();

        for (auto constraint : column_def.constraints) {
            auto type = GetConstraintTypeFromProto(constraint);
            if (type == ConstraintType::kInvalid) {
                return {nullptr, Status::InvalidConstraintType()};
            }
            constraints.insert(type);
        }

        const auto &column_def_name = column_def.name;
        auto col_def = new ColumnDef(column_def.id, column_def_data_type_ptr, column_def_name, constraints);
        return {col_def, Status::OK()};
    }

    static SharedPtr<DataType> GetColumnTypeFromProto(const infinity_thrift_rpc::DataType &type) {
        switch (type.logic_type) {
            case infinity_thrift_rpc::LogicType::Boolean:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kBoolean);
            case infinity_thrift_rpc::LogicType::TinyInt:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kTinyInt);
            case infinity_thrift_rpc::LogicType::SmallInt:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kSmallInt);
            case infinity_thrift_rpc::LogicType::Integer:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kInteger);
            case infinity_thrift_rpc::LogicType::BigInt:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kBigInt);
            case infinity_thrift_rpc::LogicType::HugeInt:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kHugeInt);
            case infinity_thrift_rpc::LogicType::Decimal:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kDecimal);
            case infinity_thrift_rpc::LogicType::Float:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kFloat);
            case infinity_thrift_rpc::LogicType::Double:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kDouble);
            case infinity_thrift_rpc::LogicType::Embedding: {
                auto embedding_type = GetEmbeddingDataTypeFromProto(type.physical_type.embedding_type.element_type);
                if (embedding_type == EmbeddingDataType::kElemInvalid) {
                    return MakeShared<infinity::DataType>(infinity::LogicalType::kInvalid);
                }
                auto embedding_info = EmbeddingInfo::Make(embedding_type, type.physical_type.embedding_type.dimension);
                return MakeShared<infinity::DataType>(infinity::LogicalType::kEmbedding, embedding_info);
            };
            case infinity_thrift_rpc::LogicType::Varchar:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kVarchar);
            default:
                return MakeShared<infinity::DataType>(infinity::LogicalType::kInvalid);
        }
        return MakeShared<infinity::DataType>(infinity::LogicalType::kInvalid);
    }

    static ConstraintType GetConstraintTypeFromProto(infinity_thrift_rpc::Constraint::type constraint) {
        switch (constraint) {
            case infinity_thrift_rpc::Constraint::NotNull:
                return ConstraintType::kNotNull;
            case infinity_thrift_rpc::Constraint::Null:
                return ConstraintType::kNull;
            case infinity_thrift_rpc::Constraint::PrimaryKey:
                return ConstraintType::kPrimaryKey;
            case infinity_thrift_rpc::Constraint::Unique:
                return ConstraintType::kUnique;
            default:
                return ConstraintType::kInvalid;
        }
    }

    static EmbeddingDataType GetEmbeddingDataTypeFromProto(const infinity_thrift_rpc::ElementType::type &type) {
        switch (type) {
            case infinity_thrift_rpc::ElementType::ElementBit:
                return EmbeddingDataType::kElemBit;
            case infinity_thrift_rpc::ElementType::ElementInt8:
                return EmbeddingDataType::kElemInt8;
            case infinity_thrift_rpc::ElementType::ElementInt16:
                return EmbeddingDataType::kElemInt16;
            case infinity_thrift_rpc::ElementType::ElementInt32:
                return EmbeddingDataType::kElemInt32;
            case infinity_thrift_rpc::ElementType::ElementInt64:
                return EmbeddingDataType::kElemInt64;
            case infinity_thrift_rpc::ElementType::ElementFloat32:
                return EmbeddingDataType::kElemFloat;
            case infinity_thrift_rpc::ElementType::ElementFloat64:
                return EmbeddingDataType::kElemDouble;
            default:
                return EmbeddingDataType::kElemInvalid;
        }
    }

    static IndexType GetIndexTypeFromProto(const infinity_thrift_rpc::IndexType::type &type) {
        switch (type) {
            case infinity_thrift_rpc::IndexType::IVFFlat:
                return IndexType::kIVFFlat;
            case infinity_thrift_rpc::IndexType::Hnsw:
                return IndexType::kHnsw;
            case infinity_thrift_rpc::IndexType::FullText:
                return IndexType::kFullText;
            default:
                return IndexType::kInvalid;
        }
        return IndexType::kInvalid;
    }

    static ConstantExpr *GetConstantFromProto(Status &status, const infinity_thrift_rpc::ConstantExpr &expr) {
        switch (expr.literal_type) {
            case infinity_thrift_rpc::LiteralType::Boolean: {
                auto parsed_expr = new ConstantExpr(LiteralType::kBoolean);
                parsed_expr->bool_value_ = expr.bool_value;
                return parsed_expr;
            }
            case infinity_thrift_rpc::LiteralType::Double: {
                auto parsed_expr = new ConstantExpr(LiteralType::kDouble);
                parsed_expr->double_value_ = expr.f64_value;
                return parsed_expr;
            }
            case infinity_thrift_rpc::LiteralType::String: {
                auto parsed_expr = new ConstantExpr(LiteralType::kString);
                parsed_expr->str_value_ = strdup(expr.str_value.c_str());
                return parsed_expr;
            }
            case infinity_thrift_rpc::LiteralType::Int64: {
                auto parsed_expr = new ConstantExpr(LiteralType::kInteger);
                parsed_expr->integer_value_ = expr.i64_value;
                return parsed_expr;
            }
            case infinity_thrift_rpc::LiteralType::Null: {
                auto parsed_expr = new ConstantExpr(LiteralType::kNull);
                return parsed_expr;
            }
            case infinity_thrift_rpc::LiteralType::IntegerArray: {
                auto parsed_expr = new ConstantExpr(LiteralType::kIntegerArray);
                parsed_expr->long_array_.reserve(expr.i64_array_value.size());
                for (auto &value : expr.i64_array_value) {
                    parsed_expr->long_array_.emplace_back(value);
                }
                return parsed_expr;
            }
            case infinity_thrift_rpc::LiteralType::DoubleArray: {
                auto parsed_expr = new ConstantExpr(LiteralType::kDoubleArray);
                parsed_expr->double_array_.reserve(expr.f64_array_value.size());
                for (auto &value : expr.f64_array_value) {
                    parsed_expr->double_array_.emplace_back(value);
                }
                return parsed_expr;
            }
            default: {
                status = Status::InvalidConstantType();
            }
        }
    }

    static ColumnExpr *GetColumnExprFromProto(const infinity_thrift_rpc::ColumnExpr &column_expr) {
        auto parsed_expr = new ColumnExpr();

        for (auto &column_name : column_expr.column_name) {
            parsed_expr->names_.emplace_back(column_name);
        }

        parsed_expr->star_ = column_expr.star;
        return parsed_expr;
    }

    static FunctionExpr *GetFunctionExprFromProto(Status &status, const infinity_thrift_rpc::FunctionExpr &function_expr) {
        auto *parsed_expr = new FunctionExpr();
        parsed_expr->func_name_ = function_expr.function_name;
        Vector<ParsedExpr *> *arguments;
        arguments = new Vector<ParsedExpr *>();
        arguments->reserve(function_expr.arguments.size());

        for (auto &args : function_expr.arguments) {
            arguments->emplace_back(GetParsedExprFromProto(status, args));
            if (!status.ok()) {
                if (parsed_expr != nullptr) {
                    delete parsed_expr;
                    parsed_expr = nullptr;
                }
                if (arguments != nullptr) {
                    for (auto &argument : *arguments) {
                        delete argument;
                        argument = nullptr;
                    }
                    delete arguments;
                    arguments = nullptr;
                }
                return nullptr;
            }
        }

        parsed_expr->arguments_ = arguments;
        return parsed_expr;
    }

    static std::tuple<KnnExpr *, Status> GetKnnExprFromProto(const infinity_thrift_rpc::KnnExpr &expr) {
        auto knn_expr = new KnnExpr(false);
        knn_expr->column_expr_ = GetColumnExprFromProto(expr.column_expr);

        knn_expr->distance_type_ = GetDistanceTypeFormProto(expr.distance_type);
        if (knn_expr->distance_type_ == KnnDistanceType::kInvalid) {
            delete knn_expr;
            knn_expr = nullptr;
            return {nullptr, Status::InvalidKnnDistanceType()};
        }
        knn_expr->embedding_data_type_ = GetEmbeddingDataTypeFromProto(expr.embedding_data_type);
        if (knn_expr->embedding_data_type_ == EmbeddingDataType::kElemInvalid) {
            delete knn_expr;
            knn_expr = nullptr;
            return {nullptr, Status::InvalidEmbeddingDataType()};
        }

        auto [embedding_data_ptr, dimension, status] = GetEmbeddingDataTypeDataPtrFromProto(expr.embedding_data);
        knn_expr->embedding_data_ptr_ = embedding_data_ptr;
        knn_expr->dimension_ = dimension;
        if (!status.ok()) {
            if (knn_expr != nullptr) {
                delete knn_expr;
                knn_expr = nullptr;
            }
            return {nullptr, status};
        }

        knn_expr->topn_ = expr.topn;
        if (knn_expr->topn_ <= 0) {
            delete knn_expr;
            knn_expr = nullptr;
            String topn = std::to_string(expr.topn);
            return {nullptr, Status::InvalidParameterValue("topn", topn, "topn should be greater than 0")};
        }

        knn_expr->opt_params_ = new Vector<InitParameter *>();
        for (auto &param : expr.opt_params) {
            auto init_parameter = new InitParameter();
            init_parameter->param_name_ = param.param_name;
            init_parameter->param_value_ = param.param_value;
            knn_expr->opt_params_->emplace_back(init_parameter);
        }
        return {knn_expr, status};
    }

    static MatchExpr *GetMatchExprFromProto(const infinity_thrift_rpc::MatchExpr &expr) {
        auto match_expr = new MatchExpr();
        match_expr->fields_ = expr.fields;
        match_expr->matching_text_ = expr.matching_text;
        match_expr->options_text_ = expr.options_text;
        return match_expr;
    }

    static FusionExpr *GetFusionExprFromProto(const infinity_thrift_rpc::FusionExpr &expr) {
        auto fusion_expr = new FusionExpr();
        fusion_expr->method_ = expr.method;
        fusion_expr->SetOptions(expr.options_text);
        return fusion_expr;
    }

    static ParsedExpr *GetParsedExprFromProto(Status &status, const infinity_thrift_rpc::ParsedExpr &expr) {
        if (expr.type.__isset.column_expr == true) {
            auto parsed_expr = GetColumnExprFromProto(*expr.type.column_expr);
            return parsed_expr;
        } else if (expr.type.__isset.constant_expr == true) {
            auto parsed_expr = GetConstantFromProto(status, *expr.type.constant_expr);
            return parsed_expr;
        } else if (expr.type.__isset.function_expr == true) {
            auto parsed_expr = GetFunctionExprFromProto(status, *expr.type.function_expr);
            return parsed_expr;
        } else if (expr.type.__isset.knn_expr == true) {
            auto [parsed_expr, knn_expr_status] = GetKnnExprFromProto(*expr.type.knn_expr);
            status = knn_expr_status;
            return parsed_expr;
        } else if (expr.type.__isset.match_expr == true) {
            auto parsed_expr = GetMatchExprFromProto(*expr.type.match_expr);
            return parsed_expr;
        } else if (expr.type.__isset.fusion_expr == true) {
            auto parsed_expr = GetFusionExprFromProto(*expr.type.fusion_expr);
            return parsed_expr;
        } else {
            status = Status::InvalidParsedExprType();
        }
        return nullptr;
    }

    static KnnDistanceType GetDistanceTypeFormProto(const infinity_thrift_rpc::KnnDistanceType::type &type) {
        switch (type) {
            case infinity_thrift_rpc::KnnDistanceType::L2:
                return KnnDistanceType::kL2;
            case infinity_thrift_rpc::KnnDistanceType::Cosine:
                return KnnDistanceType::kCosine;
            case infinity_thrift_rpc::KnnDistanceType::InnerProduct:
                return KnnDistanceType::kInnerProduct;
            case infinity_thrift_rpc::KnnDistanceType::Hamming:
                return KnnDistanceType::kHamming;
            default:
                return KnnDistanceType::kInvalid;
        }
    }

    static ExplainType GetExplainTypeFromProto(const infinity_thrift_rpc::ExplainType::type &type) {
        switch (type) {
            case infinity_thrift_rpc::ExplainType::Analyze:
                return ExplainType::kAnalyze;
            case infinity_thrift_rpc::ExplainType::Ast:
                return ExplainType::kAst;
            case infinity_thrift_rpc::ExplainType::Physical:
                return ExplainType ::kPhysical;
            case infinity_thrift_rpc::ExplainType::Pipeline:
                return ExplainType ::kPipeline;
            case infinity_thrift_rpc::ExplainType::UnOpt:
                return ExplainType ::kUnOpt;
            case infinity_thrift_rpc::ExplainType::Opt:
                return ExplainType ::kOpt;
            case infinity_thrift_rpc::ExplainType::Fragment:
                return ExplainType ::kFragment;
            default:
                return ExplainType::kInvalid;
        }
    }

    static std::tuple<void *, int64_t, Status> GetEmbeddingDataTypeDataPtrFromProto(const infinity_thrift_rpc::EmbeddingData &embedding_data) {
        if (embedding_data.__isset.i8_array_value) {
            return {(void *)embedding_data.i8_array_value.data(), embedding_data.i8_array_value.size(), Status::OK()};
        } else if (embedding_data.__isset.i16_array_value) {
            return {(void *)embedding_data.i16_array_value.data(), embedding_data.i16_array_value.size(), Status::OK()};
        } else if (embedding_data.__isset.i32_array_value) {
            return {(void *)embedding_data.i32_array_value.data(), embedding_data.i32_array_value.size(), Status::OK()};
        } else if (embedding_data.__isset.i64_array_value) {
            return {(void *)embedding_data.i64_array_value.data(), embedding_data.i64_array_value.size(), Status::OK()};
        } else if (embedding_data.__isset.f32_array_value) {
            auto ptr_double = (double *)(embedding_data.f32_array_value.data());
            auto ptr_float = (float *)(embedding_data.f32_array_value.data());
            for (size_t i = 0; i < embedding_data.f32_array_value.size(); ++i) {
                ptr_float[i] = float(ptr_double[i]);
            }
            return {(void *)embedding_data.f32_array_value.data(), embedding_data.f32_array_value.size(), Status::OK()};
        } else if (embedding_data.__isset.f64_array_value) {
            return {(void *)embedding_data.f64_array_value.data(), embedding_data.f64_array_value.size(), Status::OK()};
        } else {
            return {nullptr, 0, Status::InvalidEmbeddingDataType()};
        }
    }

    static Tuple<UpdateExpr *, Status> GetUpdateExprFromProto(const infinity_thrift_rpc::UpdateExpr &update_expr) {
        Status status;
        auto up_expr = new UpdateExpr();
        up_expr->column_name = update_expr.column_name;
        up_expr->value = GetParsedExprFromProto(status, update_expr.value);
        return {up_expr, status};
    }

    static infinity_thrift_rpc::ColumnType::type DataTypeToProtoColumnType(const SharedPtr<DataType> &data_type) {
        switch (data_type->type()) {
            case LogicalType::kBoolean:
                return infinity_thrift_rpc::ColumnType::ColumnBool;
            case LogicalType::kTinyInt:
                return infinity_thrift_rpc::ColumnType::ColumnInt8;
            case LogicalType::kSmallInt:
                return infinity_thrift_rpc::ColumnType::ColumnInt16;
            case LogicalType::kInteger:
                return infinity_thrift_rpc::ColumnType::ColumnInt32;
            case LogicalType::kBigInt:
                return infinity_thrift_rpc::ColumnType::ColumnInt64;
            case LogicalType::kFloat:
                return infinity_thrift_rpc::ColumnType::ColumnFloat32;
            case LogicalType::kDouble:
                return infinity_thrift_rpc::ColumnType::ColumnFloat64;
            case LogicalType::kVarchar:
                return infinity_thrift_rpc::ColumnType::ColumnVarchar;
            case LogicalType::kEmbedding:
                return infinity_thrift_rpc::ColumnType::ColumnEmbedding;
            case LogicalType::kRowID:
                return infinity_thrift_rpc::ColumnType::ColumnRowID;
            default:
                // necessary cause it was internal error
                UnrecoverableError("Invalid data type");
        }
        return infinity_thrift_rpc::ColumnType::ColumnInvalid;
    }

    UniquePtr<infinity_thrift_rpc::DataType> DataTypeToProtoDataType(const SharedPtr<DataType> &data_type) {
        switch (data_type->type()) {
            case LogicalType::kBoolean: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::Boolean);
                return data_type_proto;
            }
            case LogicalType::kTinyInt: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::TinyInt);
                return data_type_proto;
            }
            case LogicalType::kSmallInt: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::SmallInt);
                return data_type_proto;
            }
            case LogicalType::kInteger: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::Integer);
                return data_type_proto;
            }
            case LogicalType::kBigInt: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::BigInt);
                return data_type_proto;
            }
            case LogicalType::kFloat: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::Float);
                return data_type_proto;
            }
            case LogicalType::kDouble: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::Double);
                return data_type_proto;
            }
            case LogicalType::kVarchar: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                infinity_thrift_rpc::VarcharType varchar_type;
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::Varchar);
                infinity_thrift_rpc::PhysicalType physical_type;
                physical_type.__set_varchar_type(varchar_type);
                data_type_proto->__set_physical_type(physical_type);
                return data_type_proto;
            }
            case LogicalType::kEmbedding: {
                auto data_type_proto = MakeUnique<infinity_thrift_rpc::DataType>();
                infinity_thrift_rpc::EmbeddingType embedding_type;
                auto embedding_info = static_cast<EmbeddingInfo *>(data_type->type_info().get());
                embedding_type.__set_dimension(embedding_info->Dimension());
                embedding_type.__set_element_type(EmbeddingDataTypeToProtoElementType(*embedding_info));
                data_type_proto->__set_logic_type(infinity_thrift_rpc::LogicType::Embedding);
                infinity_thrift_rpc::PhysicalType physical_type;
                physical_type.__set_embedding_type(embedding_type);
                data_type_proto->__set_physical_type(physical_type);
                return data_type_proto;
            }
            case LogicalType::kInvalid:
            default: {
                UnrecoverableError("Invalid data type");
            }
        }
        return nullptr;
    }

    infinity_thrift_rpc::ElementType::type EmbeddingDataTypeToProtoElementType(const EmbeddingInfo &embedding_info) {
        switch (embedding_info.Type()) {
            case EmbeddingDataType::kElemBit:
                return infinity_thrift_rpc::ElementType::ElementBit;
            case EmbeddingDataType::kElemInt8:
                return infinity_thrift_rpc::ElementType::ElementInt8;
            case EmbeddingDataType::kElemInt16:
                return infinity_thrift_rpc::ElementType::ElementInt16;
            case EmbeddingDataType::kElemInt32:
                return infinity_thrift_rpc::ElementType::ElementInt32;
            case EmbeddingDataType::kElemInt64:
                return infinity_thrift_rpc::ElementType::ElementInt64;
            case EmbeddingDataType::kElemFloat:
                return infinity_thrift_rpc::ElementType::ElementFloat32;
            case EmbeddingDataType::kElemDouble:
                return infinity_thrift_rpc::ElementType::ElementFloat64;
            case EmbeddingDataType::kElemInvalid: {
                UnrecoverableError("Invalid embedding element data type");
            }
        }
        return infinity_thrift_rpc::ElementType::ElementFloat32;
    }

    void
    ProcessDataBlocks(const QueryResult &result, infinity_thrift_rpc::SelectResponse &response, Vector<infinity_thrift_rpc::ColumnField> &columns) {
        SizeT blocks_count = result.result_table_->DataBlockCount();
        for (SizeT block_idx = 0; block_idx < blocks_count; ++block_idx) {
            auto data_block = result.result_table_->GetDataBlockById(block_idx);
            Status status = ProcessColumns(data_block, result.result_table_->ColumnCount(), columns);
            if (!status.ok()) {
                ProcessStatus(response, status);
                return;
            }
        }
        HandleColumnDef(response, result.result_table_->ColumnCount(), result.result_table_->definition_ptr_, columns);
    }

    Status ProcessColumns(const SharedPtr<DataBlock> &data_block, SizeT column_count, Vector<infinity_thrift_rpc::ColumnField> &columns) {
        auto row_count = data_block->row_count();
        for (SizeT col_index = 0; col_index < column_count; ++col_index) {
            auto &result_column_vector = data_block->column_vectors[col_index];
            infinity_thrift_rpc::ColumnField &output_column_field = columns[col_index];
            output_column_field.__set_column_type(DataTypeToProtoColumnType(result_column_vector->data_type()));
            Status status = ProcessColumnFieldType(output_column_field, row_count, result_column_vector);
            if (!status.ok()) {
                return status;
            }
        }
        return Status::OK();
    }

    void HandleColumnDef(infinity_thrift_rpc::SelectResponse &response,
                         SizeT column_count,
                         SharedPtr<TableDef> table_def,
                         Vector<infinity_thrift_rpc::ColumnField> &all_column_vectors) {
        if (column_count != all_column_vectors.size()) {
            ProcessStatus(response, Status::ColumnCountMismatch(fmt::format("expect: {}, actual: {}", column_count, all_column_vectors.size())));
            return;
        }
        for (SizeT col_index = 0; col_index < column_count; ++col_index) {
            auto column_def = table_def->columns()[col_index];
            infinity_thrift_rpc::ColumnDef proto_column_def;
            proto_column_def.__set_id(column_def->id());
            proto_column_def.__set_name(column_def->name());

            infinity_thrift_rpc::DataType proto_data_type;
            proto_column_def.__set_data_type(*DataTypeToProtoDataType(column_def->type()));

            response.column_defs.emplace_back(proto_column_def);
        }
        response.__set_error_code((i64)(ErrorCode::kOk));
    }

    Status
    ProcessColumnFieldType(infinity_thrift_rpc::ColumnField &output_column_field, SizeT row_count, const shared_ptr<ColumnVector> &column_vector) {
        switch (column_vector->data_type()->type()) {
            case LogicalType::kBoolean:
            case LogicalType::kTinyInt:
            case LogicalType::kSmallInt:
            case LogicalType::kInteger:
            case LogicalType::kBigInt:
            case LogicalType::kHugeInt:
            case LogicalType::kFloat:
            case LogicalType::kDouble: {
                HandlePodType(output_column_field, row_count, column_vector);
                break;
            }
            case LogicalType::kVarchar: {
                HandleVarcharType(output_column_field, row_count, column_vector);
                break;
            }
            case LogicalType::kEmbedding: {
                HandleEmbeddingType(output_column_field, row_count, column_vector);
                break;
            }
            case LogicalType::kRowID: {
                HandleRowIDType(output_column_field, row_count, column_vector);
                break;
            }
            default: {
                return Status::InvalidDataType();
            }
        }
        return Status::OK();
    }

    static void
    HandlePodType(infinity_thrift_rpc::ColumnField &output_column_field, SizeT row_count, const std::shared_ptr<ColumnVector> &column_vector) {
        auto size = column_vector->data_type()->Size() * row_count;
        String dst;
        dst.resize(size);
        std::memcpy(dst.data(), column_vector->data(), size);
        output_column_field.column_vectors.emplace_back(std::move(dst));
    }

    void
    HandleVarcharType(infinity_thrift_rpc::ColumnField &output_column_field, SizeT row_count, const std::shared_ptr<ColumnVector> &column_vector) {
        String dst;
        SizeT total_varchar_data_size = 0;
        for (SizeT index = 0; index < row_count; ++index) {
            VarcharT &varchar = ((VarcharT *)column_vector->data())[index];
            total_varchar_data_size += varchar.length_;
        }

        auto all_size = total_varchar_data_size + row_count * sizeof(i32);
        dst.resize(all_size);

        i32 current_offset = 0;
        for (SizeT index = 0; index < row_count; ++index) {
            VarcharT &varchar = ((VarcharT *)column_vector->data())[index];
            i32 length = varchar.length_;
            if (varchar.IsInlined()) {
                std::memcpy(dst.data() + current_offset, &length, sizeof(i32));
                std::memcpy(dst.data() + current_offset + sizeof(i32), varchar.short_.data_, varchar.length_);
            } else {
                auto varchar_ptr = MakeUnique<char[]>(varchar.length_ + 1);
                column_vector->buffer_->fix_heap_mgr_->ReadFromHeap(varchar_ptr.get(),
                                                                    varchar.vector_.chunk_id_,
                                                                    varchar.vector_.chunk_offset_,
                                                                    varchar.length_);
                std::memcpy(dst.data() + current_offset, &length, sizeof(i32));
                std::memcpy(dst.data() + current_offset + sizeof(i32), varchar_ptr.get(), varchar.length_);
            }
            current_offset += sizeof(i32) + varchar.length_;
        }

        output_column_field.column_vectors.emplace_back(std::move(dst));
        output_column_field.__set_column_type(DataTypeToProtoColumnType(column_vector->data_type()));
    }

    void
    HandleEmbeddingType(infinity_thrift_rpc::ColumnField &output_column_field, SizeT row_count, const std::shared_ptr<ColumnVector> &column_vector) {
        auto size = column_vector->data_type()->Size() * row_count;
        String dst;
        dst.resize(size);
        std::memcpy(dst.data(), column_vector->data(), size);
        output_column_field.column_vectors.emplace_back(std::move(dst));
        output_column_field.__set_column_type(DataTypeToProtoColumnType(column_vector->data_type()));
    }

    void HandleRowIDType(infinity_thrift_rpc::ColumnField &output_column_field, SizeT row_count, const std::shared_ptr<ColumnVector> &column_vector) {
        auto size = column_vector->data_type()->Size() * row_count;
        String dst;
        dst.resize(size);
        std::memcpy(dst.data(), column_vector->data(), size);
        output_column_field.column_vectors.emplace_back(std::move(dst));
        output_column_field.__set_column_type(DataTypeToProtoColumnType(column_vector->data_type()));
    }

    static void ProcessStatus(infinity_thrift_rpc::CommonResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void
    ProcessStatus(infinity_thrift_rpc::ShowDatabaseResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void ProcessStatus(infinity_thrift_rpc::ShowTableResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void ProcessStatus(infinity_thrift_rpc::ShowIndexResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void ProcessStatus(infinity_thrift_rpc::SelectResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void
    ProcessStatus(infinity_thrift_rpc::ListDatabaseResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void ProcessStatus(infinity_thrift_rpc::ListTableResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void ProcessStatus(infinity_thrift_rpc::ListIndexResponse &response, const Status &status, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(status.code()));
        if (!status.ok()) {
            response.__set_error_msg(status.message());
            LOG_ERROR(fmt::format("{}: {}", error_header, status.message()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::CommonResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::SelectResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::ListDatabaseResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::ListTableResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::ListIndexResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::ShowDatabaseResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::ShowTableResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }

    static void
    ProcessQueryResult(infinity_thrift_rpc::ShowIndexResponse &response, const QueryResult &result, const String error_header = kErrorMsgHeader) {
        response.__set_error_code((i64)(result.ErrorCode()));
        if (!result.IsOk()) {
            response.__set_error_msg(result.ErrorStr());
            LOG_ERROR(fmt::format("{}: {}", error_header, result.ErrorStr()));
        }
    }
};

class InfinityServiceCloneFactory final : public infinity_thrift_rpc::InfinityServiceIfFactory {
public:
    ~InfinityServiceCloneFactory() final = default;

    infinity_thrift_rpc::InfinityServiceIf *getHandler(const ::apache::thrift::TConnectionInfo &connInfo) final {
        SharedPtr<TSocket> sock = std::dynamic_pointer_cast<TSocket>(connInfo.transport);

        LOG_TRACE(fmt::format("Incoming connection, SocketInfo: {}, PeerHost: {}, PeerAddress: {}, PeerPort: {}",
                              sock->getSocketInfo(),
                              sock->getPeerHost(),
                              sock->getPeerAddress(),
                              sock->getPeerPort()));

        return new InfinityServiceHandler;
    }

    void releaseHandler(infinity_thrift_rpc::InfinityServiceIf *handler) final { delete handler; }
};

// Thrift server

void ThreadedThriftServer::Init(i32 port_no) {

    std::cout << "Thrift server listen on: 0.0.0.0:" << port_no << std::endl;
    server = MakeUnique<TThreadedServer>(MakeShared<infinity_thrift_rpc::InfinityServiceProcessorFactory>(MakeShared<InfinityServiceCloneFactory>()),
                                         MakeShared<TServerSocket>(port_no), // port
                                         MakeShared<TBufferedTransportFactory>(),
                                         MakeShared<TBinaryProtocolFactory>());
}

void ThreadedThriftServer::Start() { server->serve(); }

void ThreadedThriftServer::Shutdown() { server->stop(); }

void PoolThriftServer::Init(i32 port_no, i32 pool_size) {

    SharedPtr<ThreadFactory> threadFactory = MakeShared<ThreadFactory>();

    SharedPtr<ThreadManager> threadManager = ThreadManager::newSimpleThreadManager(pool_size);
    threadManager->threadFactory(threadFactory);
    threadManager->start();

    std::cout << "API server listen on: 0.0.0.0:" << port_no << ", thread pool: " << pool_size << std::endl;

    server =
        MakeUnique<TThreadPoolServer>(MakeShared<infinity_thrift_rpc::InfinityServiceProcessorFactory>(MakeShared<InfinityServiceCloneFactory>()),
                                      MakeShared<TServerSocket>(port_no),
                                      MakeShared<TBufferedTransportFactory>(),
                                      MakeShared<TBinaryProtocolFactory>(),
                                      threadManager);
}

void PoolThriftServer::Start() { server->serve(); }

void PoolThriftServer::Shutdown() { server->stop(); }

void NonBlockPoolThriftServer::Init(i32 port_no, i32 pool_size) {

    SharedPtr<ThreadFactory> thread_factory = MakeShared<ThreadFactory>();
    service_handler_ = MakeShared<InfinityServiceHandler>();
    SharedPtr<infinity_thrift_rpc::InfinityServiceProcessor> service_processor =
        MakeShared<infinity_thrift_rpc::InfinityServiceProcessor>(service_handler_);
    SharedPtr<TProtocolFactory> protocol_factory = MakeShared<TBinaryProtocolFactory>();

    SharedPtr<ThreadManager> threadManager = ThreadManager::newSimpleThreadManager(pool_size);
    threadManager->threadFactory(thread_factory);
    threadManager->start();

    std::cout << "Non-block pooled thrift server listen on: 0.0.0.0:" << port_no << ", pool size: " << pool_size << std::endl;

    SharedPtr<TNonblockingServerSocket> non_block_socket = MakeShared<TNonblockingServerSocket>(port_no);

    //    server_thread_ = thread_factory->newThread(std::shared_ptr<TServer>(
    //        new TNonblockingServer(serviceProcessor, protocolFactory, nbSocket1, threadManager)));

    server_thread_ = thread_factory->newThread(MakeShared<TNonblockingServer>(service_processor, protocol_factory, non_block_socket, threadManager));

    //    server = MakeUnique<TThreadPoolServer>(MakeShared<InfinityServiceProcessorFactory>(MakeShared<InfinityServiceCloneFactory>()),
    //                                           MakeShared<TServerSocket>(port_no),
    //                                           MakeShared<TBufferedTransportFactory>(),
    //                                           MakeShared<TBinaryProtocolFactory>(),
    //                                           threadManager);
}

void NonBlockPoolThriftServer::Start() { server_thread_->start(); }

void NonBlockPoolThriftServer::Shutdown() { server_thread_->join(); }

} // namespace infinity
