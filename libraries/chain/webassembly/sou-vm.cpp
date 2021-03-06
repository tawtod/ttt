#include <sou/chain/webassembly/sou-vm.hpp>
#include <sou/chain/apply_context.hpp>
#include <sou/chain/transaction_context.hpp>
#include <sou/chain/wasm_sou_constraints.hpp>
//sou-vm includes
#include <sou/vm/backend.hpp>

namespace sou { namespace chain { namespace webassembly { namespace sou_vm_runtime {

using namespace sou::vm;

namespace wasm_constraints = sou::chain::wasm_constraints;

namespace {

  struct checktime_watchdog {
     checktime_watchdog(transaction_checktime_timer& timer) : _timer(timer) {}
     template<typename F>
     struct guard {
        guard(transaction_checktime_timer& timer, F&& func)
           : _timer(timer), _func(static_cast<F&&>(func)) {
           _timer.set_expiration_callback(&callback, this);
           if(_timer.expired) {
              _func(); // it's harmless if _func is invoked twice
           }
        }
        ~guard() {
           _timer.set_expiration_callback(nullptr, nullptr);
        }
        static void callback(void* data) {
           guard* self = static_cast<guard*>(data);
           self->_func();
        }
        transaction_checktime_timer& _timer;
        F _func;
     };
     template<typename F>
     guard<F> scoped_run(F&& func) {
        return guard{_timer, static_cast<F&&>(func)};
     }
     transaction_checktime_timer& _timer;
  };

}

template<typename Impl>
class sou_vm_instantiated_module : public wasm_instantiated_module_interface {
      using backend_t = backend<apply_context, Impl>;
   public:
      
      sou_vm_instantiated_module(sou_vm_runtime<Impl>* runtime, std::unique_ptr<backend_t> mod) :
         _runtime(runtime),
         _instantiated_module(std::move(mod)) {}

      void apply(apply_context& context) override {
         _instantiated_module->set_wasm_allocator(&context.control.get_wasm_allocator());
         _runtime->_bkend = _instantiated_module.get();
         auto fn = [&]() {
            _runtime->_bkend->initialize(&context);
            const auto& res = _runtime->_bkend->call(
                &context, "env", "apply", context.get_receiver().to_uint64_t(),
                context.get_action().account.to_uint64_t(),
                context.get_action().name.to_uint64_t());
         };
         try {
            checktime_watchdog wd(context.trx_context.transaction_timer);
            _runtime->_bkend->timed_run(wd, fn);
         } catch(sou::vm::timeout_exception&) {
            context.trx_context.checktime();
         } catch(sou::vm::wasm_memory_exception& e) {
            FC_THROW_EXCEPTION(wasm_execution_error, "access violation");
         } catch(sou::vm::exception& e) {
            // FIXME: Do better translation
            FC_THROW_EXCEPTION(wasm_execution_error, "something went wrong...");
         }
         _runtime->_bkend = nullptr;
      }

   private:
      sou_vm_runtime<Impl>*            _runtime;
      std::unique_ptr<backend_t> _instantiated_module;
};

template<typename Impl>
sou_vm_runtime<Impl>::sou_vm_runtime() {}

template<typename Impl>
void sou_vm_runtime<Impl>::immediately_exit_currently_running_module() {
   throw wasm_exit{};
}

template<typename Impl>
bool sou_vm_runtime<Impl>::inject_module(IR::Module& module) {
   return false;
}

template<typename Impl>
std::unique_ptr<wasm_instantiated_module_interface> sou_vm_runtime<Impl>::instantiate_module(const char* code_bytes, size_t code_size, std::vector<uint8_t>,
                                                                                             const digest_type&, const uint8_t&, const uint8_t&) {
   using backend_t = backend<apply_context, Impl>;
   try {
      wasm_code_ptr code((uint8_t*)code_bytes, code_size);
      std::unique_ptr<backend_t> bkend = std::make_unique<backend_t>(code, code_size);
      registered_host_functions<apply_context>::resolve(bkend->get_module());
      return std::make_unique<sou_vm_instantiated_module<Impl>>(this, std::move(bkend));
   } catch(sou::vm::exception& e) {
      FC_THROW_EXCEPTION(wasm_execution_error, "Error building sou-vm interp: ${e}", ("e", e.what()));
   }
}

template class sou_vm_runtime<sou::vm::interpreter>;
template class sou_vm_runtime<sou::vm::jit>;

}}}}
