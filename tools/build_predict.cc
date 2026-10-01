//
// Copyright RIME Developers
//
// 建联想词库（predict.db）。
//
// v1 输入格式（旧）：
//     <上文词>\t<候选词>\t<权重>
// v2 输入格式（带编码，支持「打一半」「只打首字母」）：
//     <上文词>\t<候选词>\t<权重>\t<拼音编码（音节空格分隔）>
// 两种行可以混着给；有编码的行会用 v2 段写入。只要出现过一条带编码的行，
// 整个词库就用 v2 格式落盘（v1 段与 v2 段不共存）。
//
#include <algorithm>
#include <iostream>
#include <sstream>
#include <rime/common.h>
#include "predict_db.h"

using namespace rime;

namespace {

// "beng tie" -> {"beng", "tie"}，同时给出每个音节在串里的起止下标
void SplitCode(const string& code,
               vector<string>* syllables,
               vector<int>* boundaries) {
  std::istringstream stream(code);
  string syllable;
  int pos = 0;
  while (stream >> syllable) {
    int start = pos;
    int end = pos + int(syllable.size());
    syllables->push_back(syllable);
    boundaries->push_back(start);
    boundaries->push_back(end);
    pos = end + 1;  // 跳过空格
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  rime::predict::RawData data;
  map<string, string> codes;                // 候选词 -> 拼音编码
  map<string, vector<int>> code_boundaries; // 候选词 -> 音节边界
  bool has_v2 = false;

  string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    // 允许制表符或空白分隔
    std::istringstream stream(line);
    string key;
    rime::predict::RawEntry entry;
    if (!(stream >> key >> entry.text >> entry.weight))
      continue;
    string code;
    stream >> code;
    if (!code.empty()) {
      codes[entry.text] = code;
      vector<string> syllables;
      vector<int> boundaries;
      SplitCode(code, &syllables, &boundaries);
      code_boundaries[entry.text] = boundaries;
      has_v2 = true;
    }
    data[key].push_back(std::move(entry));
  }

  if (data.empty()) {
    LOG(ERROR) << "no input entries";
    return 1;
  }

  path file_path = argc > 1 ? path(argv[1]) : path{"predict.db"};
  PredictDb db(file_path);
  LOG(INFO) << "creating " << db.file_path()
            << (has_v2 ? " (format v2, with codes)" : " (format v1)");
  bool ok = has_v2 ? db.BuildV2(data, codes, code_boundaries) : db.Build(data);
  if (!ok || !db.Save()) {
    LOG(ERROR) << "failed to build " << db.file_path();
    return 1;
  }
  LOG(INFO) << "created: " << db.file_path();
  return 0;
}
