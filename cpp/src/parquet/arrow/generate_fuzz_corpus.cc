// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// A command line executable that generates a bunch of valid Parquet files
// containing example record batches.  Those are used as fuzzing seeds
// to make fuzzing more efficient.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "arrow/array.h"
#include "arrow/io/file.h"
#include "arrow/record_batch.h"
#include "arrow/result.h"
#include "arrow/scalar.h"
#include "arrow/table.h"
#include "arrow/testing/random.h"
#include "arrow/util/io_util.h"
#include "arrow/util/key_value_metadata.h"
#include "parquet/arrow/writer.h"

namespace arrow {

using ::arrow::internal::CreateDir;
using ::arrow::internal::PlatformFilename;
using ::parquet::WriterProperties;

static constexpr int32_t kBatchSize = 1000;
static constexpr int32_t kChunkSize = kBatchSize * 3 / 8;

std::shared_ptr<WriterProperties> GetWriterProperties() {
  WriterProperties::Builder builder{};
  builder.disable_dictionary("no_dict");
  return builder.build();
}

Result<std::shared_ptr<RecordBatch>> ExampleBatch1() {
  random::RandomArrayGenerator gen(42);
  std::shared_ptr<Array> a, b, c, d, e, no_dict;
  a = gen.Int16(kBatchSize, -10000, 10000, /*null_probability=*/0.2);
  b = gen.Float64(kBatchSize, -1e10, 1e10, /*null_probability=*/0.0);
  // A column of tiny strings that will hopefully trigger dict encoding
  c = gen.String(kBatchSize, 0, 3, /*null_probability=*/0.2);
  {
    auto values = gen.Int64(kBatchSize * 10, -10000, 10000, /*null_probability=*/0.2);
    auto offsets = gen.Offsets(kBatchSize + 1, 0, static_cast<int32_t>(values->length()));
    RETURN_NOT_OK(ListArray::FromArrays(*offsets, *values, default_memory_pool(), &d));
  }
  // A column of a repeated constant that will hopefully trigger RLE encoding
  RETURN_NOT_OK(MakeArrayFromScalar(Int16Scalar(42), kBatchSize, &e));
  // A non-dict-encoded column
  no_dict = gen.String(kBatchSize, 0, 30, /*null_probability=*/0.2);

  auto schema = ::arrow::schema(
      {field("a", a->type()), field("b", b->type()), field("c", c->type()),
       field("d", d->type()), field("e", e->type()), field("no_dict", no_dict->type())});
  auto md = key_value_metadata({"key1", "key2"}, {"value1", ""});
  schema = schema->WithMetadata(md);
  return RecordBatch::Make(schema, kBatchSize, {a, b, c, d, e, no_dict});
}

Result<std::vector<std::shared_ptr<RecordBatch>>> Batches() {
  std::vector<std::shared_ptr<RecordBatch>> batches;
  ARROW_ASSIGN_OR_RAISE(auto batch, ExampleBatch1());
  batches.push_back(batch);
  return batches;
}

Status DoMain(const std::string& out_dir) {
  ARROW_ASSIGN_OR_RAISE(auto dir_fn, PlatformFilename::FromString(out_dir));
  RETURN_NOT_OK(CreateDir(dir_fn));

  int sample_num = 1;
  auto sample_name = [&]() -> std::string {
    return "pq-table-" + std::to_string(sample_num++);
  };

  ARROW_ASSIGN_OR_RAISE(auto batches, Batches());

  auto writer_properties = GetWriterProperties();

  for (const auto& batch : batches) {
    RETURN_NOT_OK(batch->ValidateFull());
    std::shared_ptr<Table> table;
    RETURN_NOT_OK(Table::FromRecordBatches({batch}, &table));

    ARROW_ASSIGN_OR_RAISE(auto sample_fn, dir_fn.Join(sample_name()));
    std::cerr << sample_fn.ToString() << std::endl;
    ARROW_ASSIGN_OR_RAISE(auto file, io::FileOutputStream::Open(sample_fn.ToString()));
    RETURN_NOT_OK(::parquet::arrow::WriteTable(*table, default_memory_pool(), file,
                                               kChunkSize, writer_properties));
    RETURN_NOT_OK(file->Close());
  }
  return Status::OK();
}

ARROW_NORETURN void Usage() {
  std::cerr << "Usage: parquet-arrow-generate-fuzz-corpus "
            << "<output directory>" << std::endl;
  std::exit(2);
}

int Main(int argc, char** argv) {
  if (argc != 2) {
    Usage();
  }
  auto out_dir = std::string(argv[1]);

  Status st = DoMain(out_dir);
  if (!st.ok()) {
    std::cerr << st.ToString() << std::endl;
    return 1;
  }
  return 0;
}

}  // namespace arrow

int main(int argc, char** argv) { return arrow::Main(argc, argv); }