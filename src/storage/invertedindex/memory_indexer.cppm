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

#include <cstdio>

export module memory_indexer;
import stl;
import memory_pool;
import index_defines;
import posting_writer;
import column_vector;
import column_inverter;
import third_party;
import internal_types;
import ring;
import skiplist;
import internal_types;
import map_with_lock;

namespace infinity {

class FullTextColumnLengthFileHandler;

export class MemoryIndexer {
public:
    struct KeyComp {
        bool operator()(const String &lhs, const String &rhs) const;
    };

    using PostingPtr = SharedPtr<PostingWriter>;
    // using PostingTableStore = SkipList<String, PostingPtr, KeyComp>;
    using PostingTableStore = MapWithLock<String, PostingPtr>;

    struct PostingTable {
        PostingTable();
        PostingTableStore store_;
        MemoryPool byte_slice_pool_;
        RecyclePool buffer_pool_;
    };

    MemoryIndexer(const String &index_dir,
                  const String &base_name,
                  RowID base_row_id,
                  optionflag_t flag,
                  const String &analyzer,
                  MemoryPool &byte_slice_pool,
                  RecyclePool &buffer_pool,
                  ThreadPool &thread_pool);

    ~MemoryIndexer();

    // Insert is non-blocking. Caller must ensure there's no RowID gap between each call.
    void Insert(SharedPtr<ColumnVector> column_vector,
                u32 row_offset,
                u32 row_count,
                SharedPtr<FullTextColumnLengthFileHandler> fulltext_length_handler,
                bool offline = false);

    // Commit is non-blocking and thread-safe. There shall be a background thread which call this method regularly.
    void Commit(bool offline = false);

    // CommitSync is for online case. It gets a batch of ColumnInverter and commit them. Returns the size of the batch.
    SizeT CommitSync();

    // Dump is blocking and shall be called only once after inserting all documents.
    // WARN: Don't reuse MemoryIndexer after calling Dump!
    void Dump(bool offline = false, bool spill = false);

    // A MemoryIndexer is allow to load iff it's empty or spilled.
    void Load();

    SizeT GetInflightTasks() {
        std::unique_lock<std::mutex> lock(mutex_);
        return inflight_tasks_;
    }

    String GetBaseName() const { return base_name_; }

    RowID GetBaseRowId() const { return base_row_id_; }

    u32 GetDocCount() const { return doc_count_; }

    MemoryPool *GetPool() { return &byte_slice_pool_; }

    SharedPtr<PostingTable> GetPostingTable() { return posting_table_; }

    SharedPtr<PostingWriter> GetOrAddPosting(const String &term);

    void Reset();

private:
    void WaitInflightTasks() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return inflight_tasks_ == 0; });
    }

    // CommitOffline is for offline case. It spill a batch of ColumnInverter. Returns the size of the batch.
    SizeT CommitOffline(bool wait_if_empty = false);

    void OfflineDump();

    void FinalSpillFile();

    void PrepareSpillFile();

private:
    String index_dir_;
    String base_name_;
    RowID base_row_id_{INVALID_ROWID};
    optionflag_t flag_;
    String analyzer_;
    MemoryPool &byte_slice_pool_;
    RecyclePool &buffer_pool_;
    ThreadPool &thread_pool_;
    u32 doc_count_{0};
    SharedPtr<PostingTable> posting_table_;
    PostingPtr prepared_posting_{nullptr};
    Ring<SharedPtr<ColumnInverter>> ring_inverted_;
    Ring<SharedPtr<ColumnInverter>> ring_sorted_;
    u64 seq_inserted_{0};
    u64 inflight_tasks_{0};
    Atomic<bool> generating_{false};

    std::condition_variable cv_;
    std::mutex mutex_;

    u32 num_runs_{0};                  // For offline index building
    FILE *spill_file_handle_{nullptr}; // Temp file for offline external merge sort
    String spill_full_path_;           // Path of spill file
    u64 tuple_count_{0};               // Number of tuples for external merge sort

    bool is_spilled_{false};

    // for column length info
    std::shared_mutex column_length_mutex_;
    Vector<u32> column_length_array_;
};
} // namespace infinity
