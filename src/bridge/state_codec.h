#pragma once

#include "engine/pipeline_model.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace effetune::vst {

class StateCodec {
public:
  [[nodiscard]] static std::string encode(const PluginStateDocument &state);
  [[nodiscard]] static bool decode(std::string_view json, PluginStateDocument &state,
                                   std::string *error = nullptr);
};

[[nodiscard]] PipelineState reconcilePipelineSnapshot(const PipelineState &preserved,
                                                       PipelineState incoming,
                                                       bool preserveAllMissing);

[[nodiscard]] std::string mergeExtraJsonObjects(std::string_view preserved,
                                                std::string_view incoming);

class UndoOpaqueStateStore {
public:
  [[nodiscard]] PipelineState reconcile(char pipeline, const PipelineState &preserved,
                                        PipelineState incoming, bool preserveAllMissing);
  void clear() noexcept;

private:
  struct Entry {
    char pipeline = 'A';
    PluginState plugin;
  };

  static constexpr std::size_t kMaxEntries = 100u * kMaxPipelineNodes;
  std::vector<Entry> entries_;
};

} // namespace effetune::vst
