#ifndef RIME_PREDICT_DB_H_
#define RIME_PREDICT_DB_H_

#include <darts.h>
#include <rime/resource.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/string_table.h>
#include <rime/dict/table.h>

namespace rime {

namespace predict {

struct Metadata {
  static const int kFormatMaxLength = 32;
  char format[kFormatMaxLength];
  uint32_t db_checksum;
  OffsetPtr<char> key_trie;  // DoubleArray (query -> offset of Candidates)
  uint32_t key_trie_size;
  OffsetPtr<char> value_trie;  // StringTable
  uint32_t value_trie_size;
};

using Candidates = ::rime::Array<::rime::table::Entry>;

struct RawEntry {
  string text;
  double weight;
};

using RawData = map<string, vector<RawEntry>>;

// ---------------------------------------------------------------------------
// 联想词库格式 v2
//
// v1 只存「词 + 权重」，查询键是「上一个上屏词」，没有编码信息，
// 所以只能做整词精确匹配，写不出「打一半就猜」「只打首字母」。
//
// v2 给每条候选额外存一份**它自己的拼音编码**，并把编码音节边界也存下来，
// 于是运行时可以按当前输入做三档匹配并重新排序：
//   全拼（编码与输入完全一致）> 前缀（输入是编码的前缀，即「打一半」）
//   > 首字母（输入是编码各音节首字母的前缀，即「只打首字母」）
//   同档内排序用「更长的输入前缀优先级更高，再比词频」。
//
// v1 文件仍然可读（没有 v2 段时走旧路径），保证升级不炸老词库。
// ---------------------------------------------------------------------------
constexpr char kPredictFormatV2[] = "Rime::Predict/2.0";

/** 音节的边界位置，用来把编码切成首字母串（beng'tie -> bt）。 */
struct CodeBoundaries {
  ::rime::Array<uint32_t> offsets;
};

struct EntryV2 {
  table::Entry base;  // 词条文字 id + 词频权重
  OffsetPtr<String> code;            // 该词的拼音编码，音节用空格分隔
  OffsetPtr<CodeBoundaries> bounds;  // 每个音节在 code 里的起止下标
};

struct CandidatesV2 {
  ::rime::Array<EntryV2> entries;
};

}  // namespace predict

class PredictDb : public MappedFile {
 public:
  PredictDb(const path& file_path)
      : MappedFile(file_path),
        key_trie_(new Darts::DoubleArray),
        value_trie_(new StringTable) {}

  bool Load();
  bool Save();
  bool Build(const predict::RawData& data);
  /** v2 构建：带编码，支持全拼/前缀/首字母三档重排。 */
  bool BuildV2(const predict::RawData& data,
               const map<string, string>& codes,
               const map<string, vector<int>>& boundaries);
  predict::Candidates* Lookup(const string& query);
  string GetEntryText(const ::rime::table::Entry& entry);

  /** 词库是否带编码（v2）。 */
  bool has_codes() const { return has_codes_; }
  /** v2 查询：返回 (词, 词频, 匹配档位, 命中的输入前缀长度)，已按档位排序。 */
  struct RankedEntry {
    string text;
    double weight;
    int tier;  // 0=全拼 1=前缀 2=首字母 3=不匹配
    size_t matched;
  };
  vector<RankedEntry> LookupRanked(const string& key, const string& input);

 private:
  int WriteCandidates(const vector<predict::RawEntry>& candidates,
                      const table::Entry* entry);
  int WriteCandidatesV2(const vector<predict::RawEntry>& candidates,
                        const vector<string>& codes,
                        const vector<vector<int>>& bounds,
                        const table::Entry* entry);

  predict::Metadata* metadata_ = nullptr;
  the<Darts::DoubleArray> key_trie_;
  the<StringTable> value_trie_;
  bool has_codes_ = false;
};

/**
 * 由文本训练数据直接构建 predict.db。
 *
 * 输入每行：<上文词>\t<候选词>\t<权重>[\t<拼音编码>]，见 tools/build_predict.cc。
 * 出现编码即用 v2 格式，否则退回 v1。
 *
 * 单独抽成函数是为了让 Android 侧也能在**设备上**生成词库（JNI 调用），
 * 从而摆脱「必须用宿主 C++ 工具构建」的限制。
 */
bool BuildPredictDbFromText(const string& text, const path& db_path);

}  // namespace rime

#endif  // RIME_PREDICT_DB_H_
