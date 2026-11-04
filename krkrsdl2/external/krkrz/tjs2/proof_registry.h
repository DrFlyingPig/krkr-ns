#pragma once
// Isolated experiment only. No production ABI or thread-safety guarantee.
#include <functional>
#include <exception>
#include "tjsInterface.h"
#include "transaction_test_hooks.h"
namespace TJS {
iTJSDispatch2 *UnsafeArrayFactoryNoCreateImpl();
iTJSDispatch2 *UnsafeDictionaryFactoryNoCreateImpl();
}
namespace krkr_exact_proof {
using TJS::iTJSDispatch2;
using Callback = std::function<void()>;
// A no-allocation linked stack. Only the exact class factory being scanned is
// exempt; pending objects visited by nested callbacks still materialize.
struct FactoryScanScope {
    iTJSDispatch2 *target;
    FactoryScanScope *previous;
    explicit FactoryScanScope(iTJSDispatch2 *) noexcept;
    ~FactoryScanScope();
    FactoryScanScope(const FactoryScanScope &) = delete;
    FactoryScanScope &operator=(const FactoryScanScope &) = delete;
};
// Hold exact pending receivers across the complete outer dispatch, including
// a distinct pending objthis. No foreign plugin object is retained here.
struct DispatchHold {
    iTJSDispatch2 *receiver=nullptr, *context=nullptr;
    DispatchHold(iTJSDispatch2 *, iTJSDispatch2 *);
    ~DispatchHold() noexcept(false);
    DispatchHold(const DispatchHold &)=delete;
    DispatchHold &operator=(const DispatchHold &)=delete;
    // Release at the NativeInstanceSupport return boundary. False means the
    // receiver was destroyed and its borrowed native pointer cannot be used.
    bool ReleaseReceiver();
};
void BeforeStoredEnum(iTJSDispatch2 *object);

void CaptureFactory(iTJSDispatch2 *factory, bool array) noexcept;
void ForgetFactory(iTJSDispatch2 *factory) noexcept;
bool FactoriesPristine() noexcept;
bool Register(iTJSDispatch2 *object, Callback callback);
void Access(iTJSDispatch2 *object);
void Forget(iTJSDispatch2 *object) noexcept;
void Poison(iTJSDispatch2 *object, std::exception_ptr failure) noexcept;
void ObserveDestruction(iTJSDispatch2 *object) noexcept;
// These callbacks exist only in this isolated contract build.
struct TransactionScope {
    iTJSDispatch2 *target, *stage;
    TransactionScope *previous;
    TransactionScope(iTJSDispatch2 *, iTJSDispatch2 *) noexcept;
    ~TransactionScope();
};
void TransactionPoint(krkr_lazy_transaction_test::Point, iTJSDispatch2 *, std::size_t);

bool BeforeClassMutation(iTJSDispatch2 *object);
}
