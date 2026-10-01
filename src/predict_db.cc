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

  has_codes_ = boost::starts_with(string(metadata_->format), kPredictFormatV2);
  return true;
}

bool PredictDb::Save() {
  LOG(INFO) << "saving predict db: " << file_path();
  if (!key_trie_->total_size()) {
    LOG(ERROR) << "the trie has not been constructed!";
    return false;
  }
  return ShrinkToFit();
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
  const table::Entry* available_entries = &entries[0];
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
      const auto& b = bounds[i];
      size_t n = b.size() / 2;
      auto* arr = CreateArray<uint32_t>(n);
      std::copy(b.begin(), b.end(), arr->begin());
      next->bounds = arr;
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

  const table::Entry* available_entries = &entries[0];
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
  // 用「词 + 其所有编码」建前缀树，这样查询既能整词命中也能按输入前缀命中
  vector<const char*> indexed_keys;
  vector<int> indexed_values;
  for (size_t n = 0; n < keys.size(); ++n) {
    if (values[n] < 0)
      continue;
    indexed_keys.push_back(keys[n]);
    indexed_values.push_back(values[n]);
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
  metadata_ = reinterpret_cast<predict::Metadata*>(address());
  metadata_->key_trie = key_trie_image;
  metadata_->key_trie_size = key_trie_->size();
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
  std::strncpy(metadata_->format, kPredictFormatV2.c_str(),
               sizeof(metadata_->format) - 1);
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
      for (auto b = it->bounds->begin(); b != it->bounds->end(); b += 2) {
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

}  // namespace rime
