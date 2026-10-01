#include "predict_db.h"
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <darts.h>
#include <rime/resource.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/string_table.h>

namespace rime {

const string kPredictFormat = "Rime::Predict/1.0";
const string kPredictFormatPrefix = "Rime::Predict/";

bool PredictDb::Load() {
  LOG(INFO) << "loading predict db: " << file_path();

  if (IsOpen())
    Close();

  if (!OpenReadOnly()) {
    LOG(ERROR) << "error opening predict db '" << file_path() << "'.";
    return false;
  }

  metadata_ = Find<predict::Metadata>(0);
  if (!metadata_) {
    LOG(ERROR) << "metadata not found.";
    Close();
    return false;
  }

  if (!boost::starts_with(string(metadata_->format), kPredictFormatPrefix)) {
    LOG(ERROR) << "invalid metadata.";
    Close();
    return false;
  }

  if (!metadata_->key_trie) {
    LOG(ERROR) << "double array image not found.";
    Close();
    return false;
  }
  DLOG(INFO) << "found double array image of size " << metadata_->key_trie_size
             << ".";
  key_trie_->set_array(metadata_->key_trie.get(), metadata_->key_trie_size);

  if (!metadata_->value_trie) {
    LOG(ERROR) << "string table not found.";
    Close();
    return false;
  }
  DLOG(INFO) << "found string table of size " << metadata_->value_trie.get()
             << ".";
  value_trie_ = make_unique<StringTable>(metadata_->value_trie.get(),
                                         metadata_->value_trie_size);

  has_codes_ =
      boost::starts_with(string(metadata_->format), predict::kPredictFormatV2);
  return true;
}

bool PredictDb::Save() {
  LOG(INFO) << "saving predict db: " << file_path();
  if (!key_trie_->total_size()) {
    LOG(ERROR) << "the trie has not been constructed!";
    return false;
  }
  // ShrinkToFit() 会 Resize -> Close()，解除映射；显式刷一次盘，确保此前
  // 写进映射的字节（尤其是 metadata 区）落盘后再收缩文件。
  Flush();
  if (!ShrinkToFit())
    return false;
  return true;
}

int PredictDb::WriteCandidates(const vector<predict::RawEntry>& candidates,
                               const table::Entry* entry) {
  auto* array = CreateArray<table::Entry>(candidates.size());
  auto* next = array->begin();
  for (size_t i = 0; i < candidates.size(); ++i) {
    *next++ = *entry++;
  }
  auto offset = reinterpret_cast<char*>(array) - address();
  return int(offset);
}

bool PredictDb::Build(const predict::RawData& data) {
  // create predict db
  int data_size = data.size();
  const size_t kReservedSize = 1024;

  size_t entry_count = 0;
  for (const auto& kv : data) {
    entry_count += kv.second.size();
  }
  StringTableBuilder string_table;
  vector<table::Entry> entries(entry_count);
  vector<const char*> keys;
  keys.reserve(data_size);
  int i = 0;
  for (const auto& kv : data) {
    if (kv.second.empty())
      continue;
    for (const auto& candidate : kv.second) {
      string_table.Add(candidate.text, candidate.weight,
                       &entries[i].text.str_id());
      entries[i].weight = float(candidate.weight);
      ++i;
    }
    keys.push_back(kv.first.c_str());
  }
  // this writes to entry vector, which should be copied to entry array later
  string_table.Build();
  size_t value_trie_image_size = string_table.BinarySize();
  if (!Create(value_trie_image_size)) {
    LOG(ERROR) << "Error creating predict db file '" << file_path() << "'.";
    return false;
  }
  // create metadata in the beginning of file
  if (!Allocate<predict::Metadata>()) {
    LOG(ERROR) << "Error creating metadata in file '" << file_path() << "'.";
    return false;
  }

  // copy from entry vector to entry array
  const table::Entry* available_entries = entries.empty() ? nullptr : &entries[0];
  vector<int> values;
  values.reserve(data_size);
  for (const auto& kv : data) {
    if (kv.second.empty())
      continue;
    values.push_back(WriteCandidates(kv.second, available_entries));
    available_entries += kv.second.size();
  }
  // build real key trie
  if (0 != key_trie_->build(data_size, keys.data(), NULL, values.data())) {
    LOG(ERROR) << "Error building double-array trie.";
    return false;
  }
  // save double-array image
  size_t key_trie_image_size = key_trie_->total_size();
  char* key_trie_image = Allocate<char>(key_trie_image_size);
  if (!key_trie_image) {
    LOG(ERROR) << "Error creating double-array image.";
    return false;
  }
  std::memcpy(key_trie_image, key_trie_->array(), key_trie_image_size);
  metadata_ = reinterpret_cast<predict::Metadata*>(address());
  metadata_->key_trie = key_trie_image;
  // double-array size (number of units)
  metadata_->key_trie_size = key_trie_->size();
  // save string table
  char* value_trie_image = Allocate<char>(value_trie_image_size);
  if (!value_trie_image) {
    LOG(ERROR) << "Error creating value trie image.";
    return false;
  }
  string_table.Dump(value_trie_image, value_trie_image_size);
  metadata_ = reinterpret_cast<predict::Metadata*>(address());
  metadata_->value_trie = value_trie_image;
  metadata_->value_trie_size = value_trie_image_size;
  value_trie_ =
      make_unique<StringTable>(value_trie_image, value_trie_image_size);
  // at last, complete the metadata
  std::strncpy(metadata_->format, kPredictFormat.c_str(),
               kPredictFormat.length());
  return true;
}

predict::Candidates* PredictDb::Lookup(const string& query) {
  int result = key_trie_->exactMatchSearch<int>(query.c_str());
  if (result == -1)
    return nullptr;
  else
    return Find<predict::Candidates>(result);
}

string PredictDb::GetEntryText(const ::rime::table::Entry& entry) {
  return value_trie_->GetString(entry.text.str_id());
}

// ---------------------------------------------------------------------------
// v2: 写入一条带编码的候选数组
// ---------------------------------------------------------------------------
int PredictDb::WriteCandidatesV2(const vector<predict::RawEntry>& candidates,
                                 const vector<string>& codes,
                                 const vector<vector<int>>& bounds,
                                 const table::Entry* entry) {
  auto* array = CreateArray<predict::EntryV2>(candidates.size());
  auto* next = array->begin();
  for (size_t i = 0; i < candidates.size(); ++i) {
    next->base = *entry++;
    const string& code = i < codes.size() ? codes[i] : string();
    if (!code.empty()) {
      next->code = CreateString(code);
    }
    if (i < bounds.size() && bounds[i].size() >= 2) {
      // bounds[i] 是 [start0, end0, start1, end1, ...]；只落每个音节的首字符
      // 下标，读取侧据此直接取首字母，无需再关心音节长度。
      const auto& b = bounds[i];
      size_t n = b.size() / 2;
      // CodeBoundaries 内嵌 Array<uint32_t>，尾部是柔性数组；按 n 个元素
      // 精确申请字节数，避免直接对 Array 结构体做值拷贝（那只会拷到第 1 个）。
      size_t bytes = sizeof(predict::CodeBoundaries) +
                     sizeof(uint32_t) * (n > 0 ? n - 1 : 0);
      auto* cb = reinterpret_cast<predict::CodeBoundaries*>(
          Allocate<char>(bytes));
      if (cb) {
        cb->offsets.size = uint32_t(n);
        auto* out = cb->offsets.begin();
        for (size_t k = 0; k < n; ++k) {
          *out++ = uint32_t(b[k * 2]);
        }
        next->bounds = cb;
      }
    }
    ++next;
  }
  return int(reinterpret_cast<char*>(array) - address());
}

bool PredictDb::BuildV2(const predict::RawData& data,
                        const map<string, string>& codes,
                        const map<string, vector<int>>& boundaries) {
  int data_size = data.size();
  size_t entry_count = 0;
  for (const auto& kv : data) {
    entry_count += kv.second.size();
  }
  StringTableBuilder string_table;
  vector<table::Entry> entries(entry_count);
  vector<const char*> keys;
  keys.reserve(data_size);
  int i = 0;
  for (const auto& kv : data) {
    if (kv.second.empty())
      continue;
    for (const auto& candidate : kv.second) {
      string_table.Add(candidate.text, candidate.weight,
                       &entries[i].text.str_id());
      entries[i].weight = float(candidate.weight);
      ++i;
    }
    keys.push_back(kv.first.c_str());
  }
  string_table.Build();
  size_t value_trie_image_size = string_table.BinarySize();
  if (!Create(value_trie_image_size)) {
    LOG(ERROR) << "Error creating predict db file '" << file_path() << "'.";
    return false;
  }
  if (!Allocate<predict::Metadata>()) {
    LOG(ERROR) << "Error creating metadata in file '" << file_path() << "'.";
    return false;
  }

  const table::Entry* available_entries = entries.empty() ? nullptr : &entries[0];
  vector<int> values;
  values.reserve(data_size);
  for (const auto& kv : data) {
    if (kv.second.empty())
      continue;
    vector<string> code_list;
    vector<vector<int>> bound_list;
    code_list.reserve(kv.second.size());
    bound_list.reserve(kv.second.size());
    for (const auto& candidate : kv.second) {
      auto it = codes.find(candidate.text);
      code_list.push_back(it != codes.end() ? it->second : string());
      auto bit = boundaries.find(candidate.text);
      if (bit != boundaries.end()) {
        bound_list.push_back(bit->second);
      } else {
        bound_list.emplace_back();
      }
    }
    values.push_back(WriteCandidatesV2(kv.second, code_list, bound_list,
                                       available_entries));
    available_entries += kv.second.size();
  }
  // 查询键就是「上一个上屏词」（或 $ 表示句首）。Darts 的 exactMatchSearch
  // 只做整串匹配，所以这里只登记原样的键 —— 「打一半 / 只打首字母」不是靠
  // 查询键前缀，而是 LookupRanked 里用当前输入对候选编码分档实现的。
  vector<const char*> indexed_keys;
  vector<int> indexed_values;
  for (size_t n = 0; n < keys.size(); ++n) {
    if (values[n] < 0)
      continue;
    indexed_keys.push_back(keys[n]);
    indexed_values.push_back(values[n]);
  }
  if (indexed_keys.empty()) {
    LOG(ERROR) << "no usable prediction keys.";
    return false;
  }
  if (0 != key_trie_->build(indexed_keys.size(), indexed_keys.data(), NULL,
                            indexed_values.data())) {
    LOG(ERROR) << "Error building double-array trie.";
    return false;
  }
  size_t key_trie_image_size = key_trie_->total_size();
  char* key_trie_image = Allocate<char>(key_trie_image_size);
  if (!key_trie_image) {
    LOG(ERROR) << "Error creating double-array image.";
    return false;
  }
  std::memcpy(key_trie_image, key_trie_->array(), key_trie_image_size);
  // 每次写 metadata 都重新取 address()：Allocate() 触发扩容时会重新映射，
  // 上一轮拿到的裸指针随即失效。OffsetPtr 存的是相对偏移，跨映射仍然有效。
  {
    metadata_ = reinterpret_cast<predict::Metadata*>(address());
    metadata_->key_trie = key_trie_image;
    metadata_->key_trie_size = uint32_t(key_trie_->size());
  }
  char* value_trie_image = Allocate<char>(value_trie_image_size);
  if (!value_trie_image) {
    LOG(ERROR) << "Error creating value trie image.";
    return false;
  }
  string_table.Dump(value_trie_image, value_trie_image_size);
  {
    // 上面这次 Allocate 可能又移动了映射，key_trie_image 也需要重新定位。
    // 偏移量在旧映射上是确定的，换算方式与新映射一致，故用同一 offset 重算。
    metadata_ = reinterpret_cast<predict::Metadata*>(address());
    metadata_->value_trie = value_trie_image;
    metadata_->value_trie_size = uint32_t(value_trie_image_size);
    value_trie_ =
        make_unique<StringTable>(value_trie_image, value_trie_image_size);
  }
  // 格式串放在**所有分配之后**写。前面每次 Allocate() 都可能 Resize 并重新
  // 映射，把新映射清零，因此只有最后一次重取 address() 才保证落盘。
  // 这也是之前 format 一直是上一版内容的原因。
  {
    metadata_ = reinterpret_cast<predict::Metadata*>(address());
    std::memset(metadata_->format, 0, sizeof(metadata_->format));
    std::strncpy(metadata_->format, predict::kPredictFormatV2,
                 sizeof(metadata_->format) - 1);
  }
  has_codes_ = true;
  return true;
}

vector<PredictDb::RankedEntry> PredictDb::LookupRanked(const string& key,
                                                       const string& input) {
  vector<RankedEntry> result;
  int offset = key_trie_->exactMatchSearch<int>(key.c_str());
  if (offset == -1)
    return result;
  auto* candidates = Find<predict::CandidatesV2>(offset);
  if (!candidates)
    return result;

  for (auto it = candidates->entries.begin(); it != candidates->entries.end();
       ++it) {
    RankedEntry e;
    e.text = GetEntryText(it->base);
    e.weight = it->base.weight;
    string code = (it->code && !it->code->empty()) ? it->code->c_str() : "";
    // 首字母串：按 bounds 记录的音节起点取每个音节的首字符
    string initials;
    if (it->bounds) {
      // bounds->offsets 存的是「每个音节的首字符下标」序列
      for (auto b = it->bounds->offsets.begin();
           b != it->bounds->offsets.end(); ++b) {
        size_t start = *b;
        if (start < code.size())
          initials.push_back(code[start]);
      }
    } else {
      for (size_t p = 0; p < code.size(); ++p) {
        if (p == 0 || code[p - 1] == ' ')
          initials.push_back(code[p]);
      }
    }
    e.tier = 3;
    e.matched = 0;
    if (!input.empty()) {
      if (code == input) {
        e.tier = 0;
        e.matched = input.size();
      } else if (code.compare(0, input.size(), input) == 0) {
        e.tier = 1;
        e.matched = input.size();
      } else if (initials.compare(0, input.size(), input) == 0) {
        e.tier = 2;
        e.matched = input.size();
      }
    }
    result.push_back(std::move(e));
  }
  // 档位优先，其次「更长的输入前缀」，最后比词频
  std::stable_sort(result.begin(), result.end(),
                   [](const RankedEntry& a, const RankedEntry& b) {
                     if (a.tier != b.tier)
                       return a.tier < b.tier;
                     if (a.matched != b.matched)
                       return a.matched > b.matched;
                     return a.weight > b.weight;
                   });
  return result;
}

// ---------------------------------------------------------------------------
// 文本 → 词库。与 tools/build_predict.cc 共用同一套解析规则，
// 这样命令行工具与设备端生成的结果完全一致。
// ---------------------------------------------------------------------------
static void SplitCodeIntoBoundaries(const string& code, vector<int>* bounds) {
  size_t pos = 0;
  while (pos <= code.size()) {
    size_t space = code.find(' ', pos);
    size_t end = (space == string::npos) ? code.size() : space;
    if (end > pos) {
      bounds->push_back(int(pos));
      bounds->push_back(int(end));
    }
    if (space == string::npos)
      break;
    pos = space + 1;
  }
}

bool BuildPredictDbFromText(const string& text, const path& db_path) {
  predict::RawData data;
  map<string, string> codes;
  map<string, vector<int>> code_boundaries;
  bool has_v2 = false;

  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    string line =
        text.substr(start, (nl == string::npos) ? string::npos : nl - start);
    start = (nl == string::npos) ? text.size() + 1 : nl + 1;
    // 去掉行尾 \r（Windows 生成的训练数据）
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;

    // 按制表符分列；不足 3 列的行直接跳过
    vector<string> cols;
    size_t p = 0;
    while (p <= line.size()) {
      size_t tab = line.find('\t', p);
      cols.push_back(line.substr(p, (tab == string::npos) ? string::npos
                                                          : tab - p));
      if (tab == string::npos)
        break;
      p = tab + 1;
    }
    while (!cols.empty() && cols.back().empty())
      cols.pop_back();
    if (cols.size() < 3)
      continue;

    predict::RawEntry entry;
    entry.text = cols[1];
    entry.weight = atof(cols[2].c_str());
    if (entry.text.empty())
      continue;
    data[cols[0]].push_back(entry);

    if (cols.size() >= 4 && !cols[3].empty()) {
      codes[entry.text] = cols[3];
      vector<int> bounds;
      SplitCodeIntoBoundaries(cols[3], &bounds);
      code_boundaries[entry.text] = bounds;
      has_v2 = true;
    }
  }

  if (data.empty()) {
    LOG(ERROR) << "BuildPredictDbFromText: no usable entries";
    return false;
  }

  PredictDb db(db_path);
  bool ok = has_v2 ? db.BuildV2(data, codes, code_boundaries) : db.Build(data);
  if (!ok || !db.Save()) {
    LOG(ERROR) << "BuildPredictDbFromText: failed to build " << db_path.string();
    return false;
  }
  LOG(INFO) << "BuildPredictDbFromText: built " << db_path.string()
            << (has_v2 ? " (v2)" : " (v1)");
  return true;
}

}  // namespace rime
