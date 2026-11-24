#ifdef LIGHTUSD_WITH_WAMR

#include "core/native-io.hh"
#include "wasm-runtime.hh"

#include <fstream>
#include <iostream>

// WAMR includes
#include "wasm_export.h"

namespace lightusd { namespace legacy {
namespace tydra {

class WasmRuntime::Impl {
public:
  wasm_module_t module = nullptr;
  wasm_module_inst_t module_inst = nullptr;
  wasm_exec_env_t exec_env = nullptr;

  static const uint32_t STACK_SIZE = 8092;
  static const uint32_t HEAP_SIZE = 8092;
};

WasmRuntime::WasmRuntime() : initialized_(false), impl_(std::make_unique<Impl>()) {
}

WasmRuntime::~WasmRuntime() {
  cleanup();
}

bool WasmRuntime::initialize() {
  if (initialized_) {
    return true;
  }

  // Initialize WAMR runtime
  if (!wasm_runtime_init()) {
    return false;
  }

  initialized_ = true;
  return true;
}

void WasmRuntime::cleanup() {
  if (!initialized_) {
    return;
  }

  if (impl_->exec_env) {
    wasm_runtime_destroy_exec_env(impl_->exec_env);
    impl_->exec_env = nullptr;
  }

  if (impl_->module_inst) {
    wasm_runtime_deinstantiate(impl_->module_inst);
    impl_->module_inst = nullptr;
  }

  if (impl_->module) {
    wasm_runtime_unload(impl_->module);
    impl_->module = nullptr;
  }

  wasm_runtime_destroy();
  initialized_ = false;
}

WasmExecutionResult WasmRuntime::loadAndExecuteWasm(
    const std::vector<uint8_t>& wasm_binary,
    const std::string& function_name,
    const std::vector<uint8_t>& input_data) {

  WasmExecutionResult result;

  if (!initialized_) {
    result.error_message = "WASM runtime not initialized";
    return result;
  }

  if (wasm_binary.empty()) {
    result.error_message = "Empty WASM binary";
    return result;
  }

  char error_buf[128];

  // Load WASM module
  impl_->module = wasm_runtime_load(
    const_cast<uint8_t*>(wasm_binary.data()),
    wasm_binary.size(),
    error_buf,
    sizeof(error_buf)
  );

  if (!impl_->module) {
    result.error_message = std::string("Failed to load WASM module: ") + error_buf;
    return result;
  }

  // Instantiate WASM module
  impl_->module_inst = wasm_runtime_instantiate(
    impl_->module,
    impl_->STACK_SIZE,
    impl_->HEAP_SIZE,
    error_buf,
    sizeof(error_buf)
  );

  if (!impl_->module_inst) {
    result.error_message = std::string("Failed to instantiate WASM module: ") + error_buf;
    wasm_runtime_unload(impl_->module);
    impl_->module = nullptr;
    return result;
  }

  // Create execution environment
  impl_->exec_env = wasm_runtime_create_exec_env(impl_->module_inst, impl_->STACK_SIZE);
  if (!impl_->exec_env) {
    result.error_message = "Failed to create execution environment";
    wasm_runtime_deinstantiate(impl_->module_inst);
    impl_->module_inst = nullptr;
    wasm_runtime_unload(impl_->module);
    impl_->module = nullptr;
    return result;
  }

  // Find the function to call
  wasm_function_inst_t func = wasm_runtime_lookup_function(
    impl_->module_inst,
    function_name.c_str()
  );

  if (!func) {
    result.error_message = std::string("Function '") + function_name + "' not found in WASM module";
    return result;
  }

  // Prepare arguments (simplified - no arguments for now)
  uint32_t argv[1] = {0};

  // Execute the function
  bool success = wasm_runtime_call_wasm(
    impl_->exec_env,
    func,
    0,  // num_args
    argv
  );

  if (!success) {
    const char* exception = wasm_runtime_get_exception(impl_->module_inst);
    result.error_message = std::string("WASM execution failed: ") +
                          (exception ? exception : "Unknown error");
    return result;
  }

  result.success = true;
  return result;
}

WasmExecutionResult WasmRuntime::loadAndExecuteWasmFromFile(
    const std::string& wasm_file_path,
    const std::string& function_name,
    const std::vector<uint8_t>& input_data) {
  if (!::lightusd::NativeIOEnabled()) { WasmExecutionResult r; r.error_message = ::lightusd::NativeIOUnavailableMessage(); return r; }


  WasmExecutionResult result;

  // Read WASM file
  std::ifstream file(wasm_file_path, std::ios::binary | std::ios::ate);
  if (!file) {
    result.error_message = "Failed to open WASM file: " + wasm_file_path;
    return result;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> wasm_binary(size);
  if (!file.read(reinterpret_cast<char*>(wasm_binary.data()), size)) {
    result.error_message = "Failed to read WASM file: " + wasm_file_path;
    return result;
  }

  return loadAndExecuteWasm(wasm_binary, function_name, input_data);
}

} // namespace tydra
}  // namespace legacy
} // namespace lightusd

#endif // LIGHTUSD_WITH_WAMR