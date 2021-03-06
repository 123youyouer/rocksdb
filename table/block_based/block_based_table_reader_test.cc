//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/block_based/block_based_table_reader.h"

#include "db/table_properties_collector.h"
#include "options/options_helper.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "table/block_based/block_based_table_builder.h"
#include "table/block_based/block_based_table_factory.h"
#include "table/format.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"

namespace ROCKSDB_NAMESPACE {

class BlockBasedTableReaderTest
    : public testing::Test,
      public testing::WithParamInterface<std::tuple<CompressionType, bool>> {
 protected:
  CompressionType compression_type_;
  bool use_direct_reads_;

  void SetUp() override {
    std::tie(compression_type_, use_direct_reads_) = GetParam();

    test::SetupSyncPointsToMockDirectIO();
    test_dir_ = test::PerThreadDBPath("block_based_table_reader_test");
    env_ = Env::Default();
    fs_ = FileSystem::Default();
    ASSERT_OK(fs_->CreateDir(test_dir_, IOOptions(), nullptr));
  }

  void TearDown() override { EXPECT_OK(test::DestroyDir(env_, test_dir_)); }

  // Creates a table with the specificied key value pairs (kv).
  void CreateTable(const std::string& table_name,
                   const CompressionType& compression_type,
                   const std::map<std::string, std::string>& kv) {
    std::unique_ptr<WritableFileWriter> writer;
    NewFileWriter(table_name, &writer);

    // Create table builder.
    Options options;
    ImmutableCFOptions ioptions(options);
    InternalKeyComparator comparator(options.comparator);
    ColumnFamilyOptions cf_options;
    MutableCFOptions moptions(cf_options);
    std::vector<std::unique_ptr<IntTblPropCollectorFactory>> factories;
    std::unique_ptr<TableBuilder> table_builder(table_factory_.NewTableBuilder(
        TableBuilderOptions(ioptions, moptions, comparator, &factories,
                            compression_type, 0 /* sample_for_compression */,
                            CompressionOptions(), false /* skip_filters */,
                            kDefaultColumnFamilyName, -1 /* level */),
        0 /* column_family_id */, writer.get()));

    // Build table.
    for (auto it = kv.begin(); it != kv.end(); it++) {
      std::string k = ToInternalKey(it->first);
      std::string v = it->second;
      table_builder->Add(k, v);
    }
    ASSERT_OK(table_builder->Finish());
  }

  void NewBlockBasedTableReader(const FileOptions& foptions,
                                const ImmutableCFOptions& ioptions,
                                const InternalKeyComparator& comparator,
                                const std::string& table_name,
                                std::unique_ptr<BlockBasedTable>* table) {
    std::unique_ptr<RandomAccessFileReader> file;
    NewFileReader(table_name, foptions, &file);

    uint64_t file_size = 0;
    ASSERT_OK(env_->GetFileSize(Path(table_name), &file_size));

    std::unique_ptr<TableReader> table_reader;
    ASSERT_OK(BlockBasedTable::Open(ioptions, EnvOptions(),
                                    table_factory_.table_options(), comparator,
                                    std::move(file), file_size, &table_reader));

    table->reset(reinterpret_cast<BlockBasedTable*>(table_reader.release()));
  }

 private:
  std::string test_dir_;
  Env* env_;
  std::shared_ptr<FileSystem> fs_;
  BlockBasedTableFactory table_factory_;

  std::string Path(const std::string& fname) { return test_dir_ + "/" + fname; }

  void WriteToFile(const std::string& content, const std::string& filename) {
    std::unique_ptr<FSWritableFile> f;
    ASSERT_OK(fs_->NewWritableFile(Path(filename), FileOptions(), &f, nullptr));
    ASSERT_OK(f->Append(content, IOOptions(), nullptr));
    ASSERT_OK(f->Close(IOOptions(), nullptr));
  }

  void NewFileWriter(const std::string& filename,
                     std::unique_ptr<WritableFileWriter>* writer) {
    std::string path = Path(filename);
    EnvOptions env_options;
    FileOptions foptions;
    std::unique_ptr<FSWritableFile> file;
    ASSERT_OK(fs_->NewWritableFile(path, foptions, &file, nullptr));
    writer->reset(new WritableFileWriter(std::move(file), path, env_options));
  }

  void NewFileReader(const std::string& filename, const FileOptions& opt,
                     std::unique_ptr<RandomAccessFileReader>* reader) {
    std::string path = Path(filename);
    std::unique_ptr<FSRandomAccessFile> f;
    ASSERT_OK(fs_->NewRandomAccessFile(path, opt, &f, nullptr));
    reader->reset(new RandomAccessFileReader(std::move(f), path, env_));
  }

  std::string ToInternalKey(const std::string& key) {
    InternalKey internal_key(key, 0, ValueType::kTypeValue);
    return internal_key.Encode().ToString();
  }
};

// Tests MultiGet in both direct IO and non-direct IO mode.
// The keys should be in cache after MultiGet.
TEST_P(BlockBasedTableReaderTest, MultiGet) {
  // Prepare key-value pairs to occupy multiple blocks.
  // Each value is 256B, every 16 pairs constitute 1 block.
  // Adjacent blocks contain values with different compression complexity:
  // human readable strings are easier to compress than random strings.
  std::map<std::string, std::string> kv;
  {
    Random rnd(101);
    uint32_t key = 0;
    for (int block = 0; block < 100; block++) {
      for (int i = 0; i < 16; i++) {
        char k[9] = {0};
        // Internal key is constructed directly from this key,
        // and internal key size is required to be >= 8 bytes,
        // so use %08u as the format string.
        sprintf(k, "%08u", key);
        std::string v;
        if (block % 2) {
          v = test::RandomHumanReadableString(&rnd, 256);
        } else {
          test::RandomString(&rnd, 256, &v);
        }
        kv[std::string(k)] = v;
        key++;
      }
    }
  }

  // Prepare keys, values, and statuses for MultiGet.
  autovector<Slice, MultiGetContext::MAX_BATCH_SIZE> keys;
  autovector<PinnableSlice, MultiGetContext::MAX_BATCH_SIZE> values;
  autovector<Status, MultiGetContext::MAX_BATCH_SIZE> statuses;
  {
    const int step =
        static_cast<int>(kv.size()) / MultiGetContext::MAX_BATCH_SIZE;
    auto it = kv.begin();
    for (int i = 0; i < MultiGetContext::MAX_BATCH_SIZE; i++) {
      keys.emplace_back(it->first);
      values.emplace_back();
      statuses.emplace_back();
      std::advance(it, step);
    }
  }

  std::string table_name =
      "BlockBasedTableReaderTest" + CompressionTypeToString(compression_type_);
  CreateTable(table_name, compression_type_, kv);

  std::unique_ptr<BlockBasedTable> table;
  Options options;
  ImmutableCFOptions ioptions(options);
  FileOptions foptions;
  foptions.use_direct_reads = use_direct_reads_;
  InternalKeyComparator comparator(options.comparator);
  NewBlockBasedTableReader(foptions, ioptions, comparator, table_name, &table);

  // Ensure that keys are not in cache before MultiGet.
  for (auto& key : keys) {
    ASSERT_FALSE(table->TEST_KeyInCache(ReadOptions(), key));
  }

  // Prepare MultiGetContext.
  autovector<GetContext, MultiGetContext::MAX_BATCH_SIZE> get_context;
  autovector<KeyContext, MultiGetContext::MAX_BATCH_SIZE> key_context;
  autovector<KeyContext*, MultiGetContext::MAX_BATCH_SIZE> sorted_keys;
  for (size_t i = 0; i < keys.size(); ++i) {
    get_context.emplace_back(
        BytewiseComparator(), nullptr, nullptr, nullptr, GetContext::kNotFound,
        keys[i], &values[i], nullptr, nullptr, nullptr, true /* do_merge */,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    key_context.emplace_back(nullptr, keys[i], &values[i], nullptr,
                             &statuses.back());
    key_context.back().get_context = &get_context.back();
  }
  for (auto& key_ctx : key_context) {
    sorted_keys.emplace_back(&key_ctx);
  }
  MultiGetContext ctx(&sorted_keys, 0, sorted_keys.size(), 0, ReadOptions());

  // Execute MultiGet.
  MultiGetContext::Range range = ctx.GetMultiGetRange();
  table->MultiGet(ReadOptions(), &range, nullptr);

  for (const Status& status : statuses) {
    ASSERT_OK(status);
  }
  // Check that keys are in cache after MultiGet.
  for (size_t i = 0; i < keys.size(); i++) {
    ASSERT_TRUE(table->TEST_KeyInCache(ReadOptions(), keys[i]));
    ASSERT_EQ(values[i].ToString(), kv[keys[i].ToString()]);
  }
}

// Param 1: compression type
// Param 2: whether to use direct reads
#ifdef ROCKSDB_LITE
// Skip direct I/O tests in lite mode since direct I/O is unsupported.
INSTANTIATE_TEST_SUITE_P(
    MultiGet, BlockBasedTableReaderTest,
    ::testing::Combine(::testing::ValuesIn(GetSupportedCompressions()),
                       ::testing::Values(false)));
#else   // ROCKSDB_LITE
INSTANTIATE_TEST_SUITE_P(
    MultiGet, BlockBasedTableReaderTest,
    ::testing::Combine(::testing::ValuesIn(GetSupportedCompressions()),
                       ::testing::Bool()));
#endif  // ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
