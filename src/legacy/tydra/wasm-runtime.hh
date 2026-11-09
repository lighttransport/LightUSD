#pragma once

#ifdef LIGHTUSD_WITH_WAMR

#include <string>
#include <vector>
#include <memory>

namespace lightusd {
namespace tydra {

struct WasmExecutionResult {
  bool success = false;
  std::string error_message;
  std::vector<uint8_t> result_data;
};

class WasmRuntime {
public:
  WasmRuntime();
  ~WasmRuntime();

  bool initialize();
  void cleanup();

  WasmExecutionResult loadAndExecuteWasm(
    const std::vector<uint8_t>& wasm_binary,
    const std::string& function_name,
    const std::vector<uint8_t>& input_data = {}
  );

  WasmExecutionResult loadAndExecuteWasmFromFile(
    const std::string& wasm_file_path,
    const std::string& function_name,
    const std::vector<uint8_t>& input_data = {}
  );

  bool isInitialized() const { return initialized_; }

private:
  bool initialized_;
  void* wasm_module_inst_;
  void* wasm_exec_env_;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tydra
} // namespace lightusd

#endif // LIGHTUSD_WITH_WAMR