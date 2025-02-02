module;

export module column_index_merger;

import stl;
import memory_pool;
import file_writer;
import posting_decoder;
import posting_merger;
import posting_list_format;
import index_defines;
import term_meta;
import index_full_text;
import column_index_iterator;
import segment_term_posting;
import local_file_system;
import internal_types;

namespace infinity {
export class ColumnIndexMerger {
public:
    ColumnIndexMerger(const String &index_dir, optionflag_t flag, MemoryPool *memory_pool, RecyclePool *buffer_pool);
    ~ColumnIndexMerger();

    void Merge(const Vector<String> &base_names, const Vector<RowID> &base_rowids, const String &dst_base_name);

private:
    SharedPtr<PostingMerger> CreatePostingMerger();

    void MergeTerm(const String &term, TermMeta &term_meta, const Vector<SegmentTermPosting *> &merging_term_postings, const RowID &merge_base_rowid);

    String index_dir_;
    optionflag_t flag_;
    MemoryPool *memory_pool_{nullptr};
    RecyclePool *buffer_pool_{nullptr};
    SharedPtr<FileWriter> posting_file_writer_;
    LocalFileSystem fs_;

    // for column length info
    std::shared_mutex column_length_mutex_;
    Vector<u32> column_length_array_;
};
} // namespace infinity