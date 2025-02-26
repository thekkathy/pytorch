#include <torch/csrc/jit/mobile/module.h>

#include <torch/csrc/jit/backends/backend_exception.h>
#include <torch/csrc/jit/mobile/interpreter.h>
#include <torch/csrc/jit/mobile/observer.h>
#include <torch/csrc/jit/runtime/jit_exception.h>
#include <exception>

#include <ATen/record_function.h>
#include <c10/util/ScopeExit.h>
#include <c10/util/irange.h>

namespace torch {
namespace jit {
std::ostream& operator<<(std::ostream& out, Instruction inst);
namespace mobile {

void CompilationUnit::register_function(std::unique_ptr<Function> fn) {
  methods_.emplace_back(std::move(fn));
}

Function* CompilationUnit::find_function(const c10::QualifiedName& qn) {
  for (auto& fn : methods_) {
    if (fn->qualname() == qn) {
      return fn.get();
    }
  }
  return nullptr;
}

Method Module::get_method(const std::string& name) const {
  if (auto method = find_method(name)) {
    return *method;
  }
  AT_ERROR("Method '", name, "' is not defined.");
}

c10::optional<Method> Module::find_method(const std::string& basename) const {
  for (auto& fn : cu_->methods()) {
    if (fn->name() == basename) {
      return c10::make_optional<Method>(Method(this, fn.get()));
    }
  }
  return c10::nullopt;
}

namespace {
void set_train_recurse(
    const c10::intrusive_ptr<c10::ivalue::Object>& obj,
    bool on) {
  if (auto slot = obj->type()->findAttributeSlot("training")) {
    obj->setSlot(*slot, on);
  } else {
    TORCH_INTERNAL_ASSERT(false, "'training' attribute not found");
  }
  for (const auto& slot : obj->slots()) {
    if (slot.isObject()) {
      set_train_recurse(slot.toObject(), on);
    }
  }
}

void slot_params_recurse(
    const c10::intrusive_ptr<c10::ivalue::Object>& obj,
    std::vector<at::Tensor>* params) {
  for (const auto& slot : obj->slots()) {
    if (slot.isTensor()) {
      params->emplace_back(slot.toTensor());
    } else if (slot.isObject()) {
      slot_params_recurse(slot.toObject(), params);
    }
  }
}

void slot_named_params_recurse(
    const c10::intrusive_ptr<c10::ivalue::Object>& obj,
    std::map<std::string, at::Tensor>* params,
    const std::string& parent_name) {
  auto slots = obj->slots();
  size_t nslots = slots.size();
  for (const auto i : c10::irange(nslots)) {
    auto slot = slots[i];
    std::string name =
        parent_name.size() == 0 ? parent_name : parent_name + ".";
    name += obj->type()->getAttributeName(i);
    // TODO: Fix this filter. Requires_grad is not the appropriate
    // filter of a parameter, but is a temporary hack to help probable
    // users of this api. The correct behavior is to filter by the
    // obj->type->is_parameter() but this currently always returns
    // false on mobile.
    if (slot.isTensor() && slot.toTensor().requires_grad()) {
      (*params)[name] = slot.toTensor();
    } else if (slot.isObject()) {
      slot_named_params_recurse(slot.toObject(), params, name);
    }
  }
}

std::string getTopModuleTypeName(const Module& m) {
  std::string name;
  if (m._ivalue()->type() && m._ivalue()->type()->name()) {
    name = m._ivalue()->type()->name().value().name();
  }
  return name;
}
} // namespace

const std::vector<at::Tensor> Module::parameters() const {
  std::vector<at::Tensor> params;
  slot_params_recurse(object_, &params);
  return params;
}

// Returns a mapping for all attributes that requires_grad=True in a module.
// This behavior differs from full torch script modules. This is a bug,
// but currently there is no way to correctly label parameters in the
// loading of a mobile module. TODO
const std::map<std::string, at::Tensor> Module::named_parameters() const {
  std::map<std::string, at::Tensor> params;
  const std::string name = "";
  slot_named_params_recurse(object_, &params, name);
  return params;
}

std::string Module::getModuleHierarchy(const int64_t debug_handle) const {
#if defined(SYMBOLICATE_MOBILE_DEBUG_HANDLE)
  return getDebugTable().getModuleHierarchyInfo(
      debug_handle, getTopModuleTypeName(*this));
#else
  return "";
#endif
}

std::string Module::getCallStack(const int64_t debug_handle) const {
#if defined(SYMBOLICATE_MOBILE_DEBUG_HANDLE)
  return getDebugTable().getSourceDebugString(
      debug_handle, getTopModuleTypeName(*this));
#else
  return "";
#endif
}

// We will continue to support this API for now as this is being relied upon
// for profiling.
// We really need to change this part, so in the next step for profiling support
// for delegates, the first thing will be to rewrite how profiling is done
// for lite interpreter.
std::string Module::get_forward_method_debug_info(size_t pc) const {
  auto debug_handle = find_method("forward")->get_debug_handle(pc);
#if defined(SYMBOLICATE_MOBILE_DEBUG_HANDLE)
  return getDebugTable().getModuleHierarchyInfo(
      debug_handle, getTopModuleTypeName(*this));
#else
  return "";
#endif
}

void Module::train(bool on) {
  set_train_recurse(object_, on);
}

bool Module::is_training() const {
  if (auto slot = object_->type()->findAttributeSlot("training")) {
    return object_->getSlot(*slot).toBool();
  }
  return true;
}

const std::vector<Method> Module::get_methods() const {
  std::vector<Method> methods;
  for (std::unique_ptr<Function>& fn : cu_->methods()) {
    methods.emplace_back(this, fn.get());
  }
  return methods;
}

Method::Method(const Module* owner, Function* function)
    : owner_(owner), function_(function) {}

void Method::run(Stack& stack) const {
  auto observer = torch::observerConfig().getModuleObserver();
  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.rand)
  auto instance_key = std::rand();
  /* if the metadata dict doesn't contain "model_name", copy the metadata and
  set the value of "model_name" as name() */
  std::unordered_map<std::string, std::string> copied_metadata =
      owner_->getMetadata();

  if (observer) {
    observer->onEnterRunMethod(
        copied_metadata, instance_key, function_->name());
  }

  auto debug_info = std::make_shared<MobileDebugInfo>();
  std::string name = copied_metadata["model_name"];
  debug_info->setModelName(name);
  debug_info->setMethodName(function_->name());
  at::DebugInfoGuard guard(at::DebugInfoKind::MOBILE_RUNTIME_INFO, debug_info);

  std::string error_message;
  auto failure_guard = c10::make_scope_exit([&]() {
    if (!observer) {
      return;
    }

#if defined(SYMBOLICATE_MOBILE_DEBUG_HANDLE)
    if (error_message.empty()) {
      error_message = owner_->getDebugTable().getSourceDebugString(
          function_->getExceptionDebugHandle(), getTopModuleTypeName(*owner_));
    }
#endif

    observer->onFailRunMethod(
        instance_key,
        error_message.empty() ? "Unknown exception" : error_message.c_str());
  });

  try {
    stack.insert(stack.begin(), owner_->_ivalue()); // self
    function_->run(stack);
    if (observer) {
      observer->onExitRunMethod(instance_key);
    }
    failure_guard.release();
    // This exception must be caught first as it derived from c10::Error
  } catch (c10::BackendRuntimeException& e) {
#if defined(SYMBOLICATE_MOBILE_DEBUG_HANDLE)
    e.pushDebugHandle(function_->getExceptionDebugHandle());
    // symbolicate all handles
    auto debug_string = owner_->getDebugTable().getSourceDebugString(
        e.getDebugHandles(), getTopModuleTypeName(*owner_));
    e.add_context(debug_string);
#endif
    error_message = e.what();
    TORCH_RETHROW(e);
  } catch (c10::Error& error) {
#if defined(SYMBOLICATE_MOBILE_DEBUG_HANDLE)
    auto debug_string = owner_->getDebugTable().getSourceDebugString(
        function_->getExceptionDebugHandle(), getTopModuleTypeName(*owner_));
    error.add_context(debug_string);
#endif
    error_message = error.what();
    TORCH_RETHROW(error);
  }
}

c10::IValue Method::operator()(std::vector<c10::IValue> stack) const {
  run(stack);
  TORCH_INTERNAL_ASSERT(!stack.empty());
  return stack.front();
}

} // namespace mobile
} // namespace jit
} // namespace torch
