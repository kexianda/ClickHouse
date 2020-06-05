#include <Common/Exception.h>

#include <DataStreams/IBlockOutputStream.h>

#include <Interpreters/Context.h>

#include <Parsers/ASTCreateQuery.h>

#include <Storages/StorageHTAP.h>
#include <Storages/StorageFactory.h>

#include <Processors/ISimpleTransform.h>
#include <Processors/Pipe.h>

namespace DB {

namespace ErrorCodes {
extern const int STORAGE_REQUIRES_PARAMETER;
}

static Block createOutputHeader(Block header, const Names& column_names) {
    Block output_header;
    for (const auto& col_name : column_names) {
        output_header.insert(header.getByName(col_name).cloneEmpty());
    }
    return output_header;
}

class TableMerger : public ISimpleTransform {
  public:
    TableMerger(const Block& header_,
                const Names& column_names_,
                std::shared_ptr<int> active_reader_count_,
                StorageHTAP* storage_)
        : ISimpleTransform(header_, createOutputHeader(header_, column_names_), false),
          column_names{column_names_}, active_reader_count{active_reader_count_},
          storage{storage_} {}

    String getName() const override { return "TableReader"; }

    Status prepare() override {
        Logger* logger = &Logger::get("TableReader");

        LOG_INFO(logger,
                 "Prepare reading HTAP table '{}'.",
                 storage->getStorageID().getFullTableName());

        /// Check can output.

        if (output.isFinished()) {
            input.close();
            return Status::Finished;
        }

        if (!output.canPush()) {
            input.setNotNeeded();
            return Status::PortFull;
        }

        /// Output if has data.
        if (has_output) {
            output.pushData(std::move(output_data));
            has_output = false;

            if (!no_more_data_needed)
                return Status::PortFull;
        }

        /// Stop if don't need more data.
        if (no_more_data_needed) {
            input.close();
            output.finish();
            return Status::Finished;
        }

        /// Check can input.
        if (!has_input) {
            if (input.isFinished()) {
                std::lock_guard lock(storage->mutex);
                (*active_reader_count)--;
                if (*active_reader_count == 0) {
                    // Last reader.  Now return table rows.
                    LOG_INFO(logger,
                             "Reading HTAP table '{}'. Last source pipe finished.",
                             storage->getStorageID().getFullTableName());
                    input_data.chunk = {};
                    input_data.exception = nullptr;
                    return Status::Ready;
                } else {
                    LOG_INFO(logger,
                             "Reading HTAP table '{}'. One source pipe finished. Remaining pipes: {}",
                             storage->getStorageID().getFullTableName(),
                             *active_reader_count);
                    output.finish();
                    return Status::Finished;
                }
            }

            input.setNeeded();

            if (!input.hasData())
                return Status::NeedData;

            input_data = input.pullData(set_input_not_needed_after_read);
            has_input = true;

            if (input_data.exception)
                /// No more data needed. Exception will be thrown (or swallowed) later.
                input.setNotNeeded();
        }

        /// Now transform.
        return Status::Ready;
    }

  protected:
    void transform(Chunk& chunk) override {
        Logger* logger = &Logger::get("TableReader");

        LOG_INFO(logger,
                 "Reading HTAP table '{}'.",
                 storage->getStorageID().getFullTableName());

        std::lock_guard lock(storage->mutex);

        MutableColumns columns;
        std::vector<uint32_t> column_positions;
        columns.reserve(column_names.size());
        column_positions.reserve(column_names.size());

        for (const std::string& col_name : column_names) {
            LOG_INFO(logger, "Read include column '{}'.", col_name);
            uint32_t pos = 0;
            for (const auto& col_name_type : storage->getColumns().getAllPhysical()) {
                if (col_name == col_name_type.name) {
                    columns.push_back(col_name_type.type->createColumn());
                    column_positions.push_back(pos);
                    break;
                }
                pos++;
            }
        }

        uint32_t row_count = 0;
        if (!has_input) {
            LOG_INFO(logger,
                     "Reading in-mem rows of HTAP table '{}' .",
                     storage->getStorageID().getFullTableName());

            for (const auto& row_pair : storage->table) {
                if (returned_keys.find(row_pair.first) == returned_keys.end()) {
                    const Tuple& row = row_pair.second;
                    for (size_t i = 0; i < columns.size(); i++) {
                        columns[i]->insert(row[column_positions[i]]);
                    }
                    row_count++;
                }
            }
        } else {
            LOG_INFO(logger,
                     "Reading MergeTree rows of HTAP table '{}' .",
                     storage->getStorageID().getFullTableName());

            Block raw_data =
                getInputPort().getHeader().cloneWithColumns(chunk.detachColumns());

            Columns pk_cols;
            for (const auto& pk_col_name : storage->getPrimaryKeyColumns()) {
                LOG_INFO(logger, "Primary key column: '{}'.", pk_col_name);
                pk_cols.push_back(raw_data.getByName(pk_col_name).column);
            }

            for (size_t i = 0; i < raw_data.rows(); i++) {
                Tuple key;
                for (auto& pk_c : pk_cols) {
                    key.push_back((*pk_c)[i]);
                }

                if (returned_keys.find(key) == returned_keys.end()) {
                    auto row_pair = storage->table.find(key);
                    if (row_pair == storage->table.end()) {
                        for (size_t j = 0; j < columns.size(); j++) {
                            ColumnPtr col = raw_data.getByName(column_names[j]).column;
                            columns[j]->insert((*col)[i]);
                        }
                    } else {
                        const Tuple& row = row_pair->second;
                        for (size_t j = 0; j < columns.size(); j++) {
                            columns[j]->insert(row[column_positions[j]]);
                        }
                    }
                    row_count++;
                    returned_keys.insert(key);
                }
            }
        }

        chunk.setColumns(std::move(columns), row_count);
    }

  private:
    Names column_names;
    std::shared_ptr<int> active_reader_count;
    StorageHTAP* storage;
    std::set<Tuple> returned_keys;
};

class TableReader : public SourceWithProgress {
  public:
    TableReader(const Names& column_names_, StorageHTAP* storage_)
        : SourceWithProgress{storage_->getSampleBlockForColumns(column_names_)},
          column_names{column_names_}, storage(storage_), generated{false} {}

    String getName() const override { return "Table"; }

  protected:
    Chunk generate() override {
        if (generated)
            return {};

        std::lock_guard lock(storage->mutex);

        MutableColumns columns;
        std::vector<uint32_t> column_positions;
        columns.reserve(column_names.size());
        column_positions.reserve(column_names.size());

        for (const std::string& col_name : column_names) {
            uint32_t pos = 0;
            for (const auto& col_name_type : storage->getColumns().getAllPhysical()) {
                if (col_name == col_name_type.name) {
                    columns.push_back(col_name_type.type->createColumn());
                    column_positions.push_back(pos);
                    break;
                }
                pos++;
            }
        }

        uint32_t row_count = 0;
        for (const auto& row_pair : storage->table) {
            const Tuple& row = row_pair.second;
            for (size_t i = 0; i < columns.size(); i++) {
                columns[i]->insert(row[column_positions[i]]);
            }
            row_count++;
        }

        generated = true;

        return Chunk(std::move(columns), row_count);
    }

  private:
    Names column_names;
    StorageHTAP* storage;
    bool generated;
};

class TableWriter : public IBlockOutputStream {
  public:
    explicit TableWriter(StorageHTAP* storage_, const Context& context_)
        : storage{storage_}, context{context_} {}

    Block getHeader() const override {
        return storage->getSampleBlock();
    }

    void write(const Block& block) override {
        Logger* logger = &Logger::get("TableWriter");

        LOG_INFO(logger,
                 "Writing HTAP table '{}'.",
                 storage->getStorageID().getFullTableName());

        storage->check(block, true);
        std::lock_guard lock(storage->mutex);

        Columns pk_cols;
        for (const auto& pk_col_name : storage->getPrimaryKeyColumns()) {
            LOG_INFO(logger, "Primary key column: '{}'.", pk_col_name);
            pk_cols.push_back(block.getByName(pk_col_name).column);
        }

        Columns cols;
        for (const auto& col_name_type : storage->getColumns().getAllPhysical()) {
            LOG_INFO(logger, "Column: '{}'.", col_name_type.name);
            cols.push_back(block.getByName(col_name_type.name).column);
        }

        for (size_t i = 0; i < block.rows(); i++) {
            Tuple key;
            for (auto& column : pk_cols) {
                key.push_back((*column)[i]);
            }
            Tuple row;
            for (auto& column : cols) {
                row.push_back((*column)[i]);
            }
            storage->table.insert_or_assign(key, row);
        }

        if ((++storage->table_size) % 2 == 0) {
            LOG_INFO(logger,
                     "Merging HTAP table '{}' with base storage.",
                     storage->getStorageID().getFullTableName());

            MutableColumns data_columns;
            for (const auto& col_name_type : storage->getColumns().getAllPhysical()) {
                data_columns.push_back(col_name_type.type->createColumn());
            }
            for (const auto& row_pair : storage->table) {
                const Tuple& row = row_pair.second;
                for (size_t i = 0; i < data_columns.size(); i++) {
                    data_columns[i]->insert(row[i]);
                }
            }

            Block batch_data = storage->getSampleBlock();
            batch_data.setColumns(std::move(data_columns));

            storage->base_storage->write(nullptr, context)->write(batch_data);
            storage->base_storage->optimize(nullptr, nullptr, true, false, context);

            LOG_INFO(logger,
                     "HTAP table '{}' merged with base storage.",
                     storage->getStorageID().getFullTableName());

            storage->table.clear();
        }
    }

  private:
    StorageHTAP* storage;
    const Context& context;
};

StorageHTAP::StorageHTAP(const StorageFactory::Arguments& args)
    : IStorage(args.table_id) {
    Logger* logger = &Logger::get("StorageHTAP");

    if (args.storage_def->primary_key == nullptr) {
        throw Exception("Primary key must be defined for HTAP tables.",
                        ErrorCodes::BAD_ARGUMENTS);
    }
    if (args.engine_args.size() != 1) {
        throw Exception("Missing version column. HTAP(ver).", ErrorCodes::BAD_ARGUMENTS);
    }

    StorageFactory::Arguments copy{
        .engine_name = "ReplacingMergeTree",
        .engine_args = args.engine_args,
        .storage_def = args.storage_def,
        .query = args.query,
        .relative_data_path = args.relative_data_path,
        .table_id = args.table_id,
        .local_context = args.local_context,
        .context = args.context,
        .columns = args.columns,
        .constraints = args.constraints,
        .attach = args.attach,
        .has_force_restore_data_flag = args.has_force_restore_data_flag
    };

    const auto& storages = StorageFactory::instance().getAllStorages();
    base_storage = (storages.at("ReplacingMergeTree").creator_fn(copy));

    setColumns(args.columns);
    setSortingKey(base_storage->getSortingKey());
    setPrimaryKey(base_storage->getPrimaryKey());

    for (const auto& pk_col : getPrimaryKeyColumns()) {
        LOG_INFO(logger, "HTAP Primary key column: '{}'.", pk_col);
    }

    table_size = 0;
}

void StorageHTAP::startup() {
    Logger* logger = &Logger::get("StorageHTAP");

    LOG_INFO(logger, "Starting up HTAP table '{}'.", getStorageID().getFullTableName());
    base_storage->startup();
    LOG_INFO(logger, "HTAP table '{}' started.", getStorageID().getFullTableName());
}

void StorageHTAP::shutdown() {
    Logger* logger = &Logger::get("StorageHTAP");

    LOG_INFO(logger, "Shutting down HTAP table '{}'.", getStorageID().getFullTableName());
    base_storage->shutdown();
    LOG_INFO(logger, "HTAP table '{}' shutdown.", getStorageID().getFullTableName());
}

Pipes StorageHTAP::read(const Names& column_names,
                        const SelectQueryInfo& query_info,
                        const Context& context,
                        QueryProcessingStage::Enum processed_stage,
                        size_t max_block_size,
                        unsigned) {
    Logger* logger = &Logger::get("StorageHTAP");

    check(column_names);

    std::lock_guard lock(mutex);

    Names column_names_with_pks = getPrimaryKeyColumns();
    for (const auto& col_name : column_names) {
        bool pk_col = false;
        for (const auto& pk_name : getPrimaryKeyColumns()) {
            if (col_name == pk_name) {
                pk_col = true;
                break;
            }
        }
        if (!pk_col) {
            column_names_with_pks.push_back(col_name);
        }
    }
    Pipes pipes = base_storage->read(column_names_with_pks,
                                     query_info,
                                     context,
                                     processed_stage,
                                     max_block_size,
                                     1);

    auto reader_count = std::make_shared<int>(pipes.size());
    LOG_INFO(logger, "Read pipe count {}", *reader_count);
    if (*reader_count == 0) {
        pipes.emplace_back(std::make_shared<TableReader>(column_names, this));
    } else {
        for (auto& pipe : pipes) {
            pipe.addSimpleTransform(std::make_shared<TableMerger>(pipe.getHeader(),
                                                                  column_names,
                                                                  reader_count,
                                                                  this));
        }
    }

    return pipes;
}

BlockOutputStreamPtr StorageHTAP::write(const ASTPtr&,
                                        const Context& context) {
    return std::make_shared<TableWriter>(this, context);
}

bool StorageHTAP::optimize(const DB::ASTPtr& query,
                           const DB::ASTPtr& partition,
                           bool,
                           bool,
                           const DB::Context& context) {
    Logger* logger = &Logger::get("StorageHTAP");

    LOG_INFO(logger, "Optimizing HTAP table '{}'.", getStorageID().getFullTableName());
    bool ret = base_storage->optimize(query, partition, true, true, context);
    LOG_INFO(logger, "HTAP table '{}' optimized.", getStorageID().getFullTableName());

    return ret;
}

void StorageHTAP::mutate(const MutationCommands& commands, const Context& query_context) {
    Logger* logger = &Logger::get("StorageHTAP");

    LOG_INFO(logger, "Mutating HTAP table '{}'.", getStorageID().getFullTableName());
    base_storage->mutate(commands, query_context);
    LOG_INFO(logger, "HTAP table '{}' mutated.", getStorageID().getFullTableName());
}

void StorageHTAP::drop() {
    Logger* logger = &Logger::get("StorageHTAP");

    std::lock_guard lock(mutex);
    LOG_INFO(logger, "Dropping HTAP table '{}'.", getStorageID().getFullTableName());
    table.clear();
    base_storage->drop();
    LOG_INFO(logger, "HTAP table '{}' dropped.", getStorageID().getFullTableName());
}

void StorageHTAP::truncate(const ASTPtr& astPtr,
                           const Context& context,
                           TableStructureWriteLockHolder& lockHolder) {
    Logger* logger = &Logger::get("StorageHTAP");

    std::lock_guard lock(mutex);
    LOG_INFO(logger, "Truncating HTAP table '{}'.", getStorageID().getFullTableName());
    table.clear();
    base_storage->truncate(astPtr, context, lockHolder);
    LOG_INFO(logger, "HTAP table '{}' truncated.", getStorageID().getFullTableName());
}

std::optional<UInt64> StorageHTAP::totalRows() const {
    std::lock_guard lock(mutex);
    return table.size();
}

static StoragePtr createHTAP(const StorageFactory::Arguments& args) {
    Logger* logger = &Logger::get("StorageHTAP");

    LOG_INFO(logger, "Create an HTAP table '{}'.", args.table_id.getFullTableName());

    return StorageHTAP::create(args);
}

void registerStorageHTAP(StorageFactory& factory) {
    StorageFactory::StorageFeatures features{
        .supports_settings = true,
        .supports_skipping_indices = true,
        .supports_sort_order = true,
        .supports_ttl = true,
    };

    factory.registerStorage("HTAP", createHTAP, features);
}

}
