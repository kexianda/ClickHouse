#pragma once

#include <map>
#include <mutex>

#include <ext/shared_ptr_helper.h>

#include <Storages/StorageFactory.h>
#include <Storages/IStorage.h>
#include <Storages/StorageMergeTree.h>


namespace DB
{

using Table = std::map<Tuple, Tuple>;

class StorageHTAP final : public ext::shared_ptr_helper<StorageHTAP>, public IStorage {
  public:
    StorageHTAP(const StorageFactory::Arguments& args);

    String getName() const override { return "HTAP"; }

    void startup() override;
    void shutdown() override;

    Pipes read(
        const Names& column_names,
        const SelectQueryInfo& query_info,
        const Context& context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        unsigned num_streams) override;

    BlockOutputStreamPtr write(const ASTPtr& query, const Context& context) override;

    bool optimize(const ASTPtr& query,
                  const ASTPtr& partition,
                  bool final,
                  bool deduplicate,
                  const Context& context) override;

    void mutate(const MutationCommands& commands, const Context& context) override;

    void drop() override;

    void truncate(const ASTPtr&, const Context&, TableStructureWriteLockHolder&) override;

    std::optional<UInt64> totalRows() const override;
    std::optional<UInt64> totalBytes() const override { return 1024UL; }

    mutable std::mutex mutex;

    Table table;

    std::shared_ptr<IStorage> base_storage;

    uint64_t table_size;
};

}
