//
// Copyright RIME Developers
//
// 建联想词库（predict.db）。
//
// v1 输入格式（旧）：
//     <上文词>\t<候选词>\t<权重>
// v2 输入格式（带编码，支持「打一半」「只打首字母」）：
//     <上文词>\t<候选词>\t<权重>\t<拼音编码（音节空格分隔）>
// 两种行可以混着给；只要出现过一条带编码的行，整个词库就用 v2 格式落盘。
//
// 解析与构建逻辑在 PredictDb 的 BuildPredictDbFromText 里，Android 侧
// 走同一函数在设备上生成，保证两边产物一致。
//
#include <algorithm>
#include <iostream>
#include <rime/common.h>
#include "predict_db.h"

using namespace rime;

int main(int argc, char* argv[]) {
  string text;
  string line;
  while (std::getline(std::cin, line)) {
    text += line;
    text += '\n';
  }

  if (text.empty()) {
    LOG(ERROR) << "no input entries";
    return 1;
  }

  path file_path = argc > 1 ? path(argv[1]) : path{"predict.db"};
  LOG(INFO) << "creating " << file_path.string();
  if (!BuildPredictDbFromText(text, file_path)) {
    LOG(ERROR) << "failed to build " << file_path.string();
    return 1;
  }
  LOG(INFO) << "created: " << file_path.string();
  return 0;
}
