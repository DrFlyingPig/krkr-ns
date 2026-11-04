// UNSAFE, isolated registry experiment. Not a production compatibility layer.
#include "proof_registry.h"
#include "tjsDebug.h"
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <new>
#include <typeinfo>
#include <unordered_map>

namespace krkr_exact_proof {
using namespace TJS;
namespace {
struct CapturedMember {
    ttstr name;
    tjs_uint32 flags;
    tTJSVariant value;
};
struct FactorySnapshot {
    iTJSDispatch2 *object = nullptr; // the original factory holder owns it
    std::vector<CapturedMember> members;
    bool valid = false;
};
struct Entry {
    Callback callback;
    bool expanding = false;
    std::exception_ptr failure;
    explicit Entry(Callback c) : callback(std::move(c)) {}
};
struct Registry {
    std::recursive_mutex mutex;
    std::unordered_map<iTJSDispatch2 *, std::shared_ptr<Entry>> entries;
    FactorySnapshot array;
    FactorySnapshot dictionary;
    std::atomic<size_t> active{0};
    tjs_uint64 registered = 0, expanded = 0, capped = 0;
    // Diagnostic only: how much deferred construction these registrations turn
    // into, and what the worst single expansion costs.
    tjs_uint64 expandCalls = 0;
    double expandMs = 0.0, expandMaxMs = 0.0;
};

// Process-lifetime proof registry. Avoid recursively entering a C++ local
// static initializer if allocation/new_handler constructs a native class.
std::atomic<Registry *> published{nullptr};
thread_local bool creatingRegistry = false;
thread_local FactoryScanScope *factoryScans = nullptr;
thread_local TransactionScope *transactionScope = nullptr;
struct HookState {
    iTJSDispatch2 *target = nullptr;
    krkr_lazy_transaction_test::Hook hook = nullptr;
    void *context = nullptr;
    size_t calls = 0, rejected = 0;
};
thread_local HookState hookState;
// Test observer: generations distinguish address reuse, object data not retained.
struct LifetimeState {
    std::mutex mutex;
    std::uint64_t next=1;
    std::unordered_map<iTJSDispatch2 *,std::uint64_t> live;
    std::unordered_map<std::uint64_t,std::size_t> deaths;
};
std::atomic<LifetimeState *> lifePublished{nullptr};
LifetimeState &Life() {
    auto *s=lifePublished.load(std::memory_order_acquire);
    if (s) return *s;
    auto *fresh=new LifetimeState;
    LifetimeState *expected=nullptr;
    if (lifePublished.compare_exchange_strong(expected,fresh,std::memory_order_acq_rel)) return *fresh;
    delete fresh; return *expected;
}

Registry *State() noexcept {
    Registry *existing = published.load(std::memory_order_acquire);
    if (existing || creatingRegistry) return existing;
    creatingRegistry = true;
    Registry *fresh = nullptr;
    try { fresh = new Registry(); } catch (...) {}
    creatingRegistry = false;
    if (!fresh) return nullptr;
    Registry *expected = nullptr;
    if (published.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) return fresh;
    delete fresh; // no published state or callbacks were installed in it
    return expected;
}

bool SameValue(const tTJSVariant &a, const tTJSVariant &b) {
    if (a.Type() != b.Type()) return false;
    if (a.Type() == tvtObject) {
        const auto x = a.AsObjectClosureNoAddRef();
        const auto y = b.AsObjectClosureNoAddRef();
        return x.Object == y.Object && x.ObjThis == y.ObjThis;
    }
    return a.DiscernCompare(b);
}

bool SnapshotMatches(const FactorySnapshot &expected, bool array) {
    if (!expected.valid || !expected.object) return false;
    auto *base = dynamic_cast<tTJSNativeClass *>(expected.object);
    if (!base || base->GetSuper()) return false;
    if (array ? typeid(*base) != typeid(tTJSArrayClass)
              : typeid(*base) != typeid(tTJSDictionaryClass)) return false;
    if (expected.object->IsValid(0, nullptr, nullptr, expected.object) != TJS_S_TRUE) return false;
    size_t count = 0;
    bool same = true;
    FactoryScanScope scanning(base);
    base->EnumStoredMembers([&](tTJSVariantString *name, tjs_uint32 flags, const tTJSVariant &value) {
        ++count;
        bool found = false;
        for (const auto &entry : expected.members) {
            if (entry.name.AsVariantStringNoAddRef() == name && entry.flags == flags &&
                SameValue(entry.value, value)) { found = true; break; }
        }
        if (!found) same = false;
    });
    return same && count == expected.members.size();
}
}

TransactionScope::TransactionScope(iTJSDispatch2 *a, iTJSDispatch2 *b) noexcept
    : target(a), stage(b), previous(transactionScope) { transactionScope=this; }
TransactionScope::~TransactionScope() { transactionScope=previous; }
void TransactionPoint(krkr_lazy_transaction_test::Point point, iTJSDispatch2 *stage, std::size_t n) {
    auto *scope=transactionScope;
    if (!scope || scope->stage!=stage || hookState.target!=scope->target || !hookState.hook) return;
    ++hookState.calls;
    hookState.hook(point,scope->target,stage,n,hookState.context);
}

FactoryScanScope::FactoryScanScope(iTJSDispatch2 *value) noexcept
    : target(value), previous(factoryScans) { factoryScans = this; }
FactoryScanScope::~FactoryScanScope() { factoryScans = previous; }

namespace {
// Pending entries are always exact engine Array/Dictionary objects. Every hook
// below therefore rejects any other receiver with a vtable type check before it
// touches the registry mutex. While a load holds pending entries, KAG calls
// Object.DeleteAllMembers and property accessors tens of thousands of times on
// ordinary objects; those calls must stay a branch, not a lock plus a lookup.
inline bool CanBePending(iTJSDispatch2 *object) noexcept {
    return typeid(*object) == typeid(tTJSArrayObject) ||
           typeid(*object) == typeid(tTJSDictionaryObject);
}
bool HoldPending(iTJSDispatch2 *object) {
    if (!object) return false;
    Registry *s=published.load(std::memory_order_acquire);
    if (!s || !s->active.load(std::memory_order_acquire)) return false;
    if (!CanBePending(object)) return false;
    std::lock_guard<std::recursive_mutex> lock(s->mutex);
    auto it=s->entries.find(object);
    if (it==s->entries.end()) return false;
    // Entries contain exact engine Array/Dictionary objects only; their native
    // AddRef is a plain counter. Never invoke an unknown plugin's AddRef here.
    object->AddRef();
    return true;
}
}
DispatchHold::DispatchHold(iTJSDispatch2 *a, iTJSDispatch2 *b) {
    auto holdExact = [](iTJSDispatch2 *object) {
        if (!object) return false;
        if (typeid(*object)!=typeid(tTJSArrayObject) &&
            typeid(*object)!=typeid(tTJSDictionaryObject))
            throw std::runtime_error("foreign dispatch context cannot be safely retained");
        object->AddRef(); // exact engine-native AddRef, not a plugin override
        return true;
    };
    if (HoldPending(a)) receiver=a;
    try {
        if (b!=a && HoldPending(b)) context=b;
        // Once either side might materialize, retain the other side too. An
        // unknown foreign object cannot be safely pinned by this prototype.
        if (receiver && b && b!=a && !context && holdExact(b)) context=b;
        if (context && a && !receiver && holdExact(a)) receiver=a;
    } catch (...) {
        if (context) { try { context->Release(); } catch (...) {} context=nullptr; }
        if (receiver) { try { receiver->Release(); } catch (...) {} receiver=nullptr; }
        throw;
    }
}
bool DispatchHold::ReleaseReceiver() {
    auto *p=receiver; receiver=nullptr;
    return !p || p->Release()!=0;
}
DispatchHold::~DispatchHold() noexcept(false) {
    // Release in reverse order, never while holding the registry mutex. During
    // exception unwinding a second destructor exception cannot be propagated.
    auto *b=context; context=nullptr;
    auto *a=receiver; receiver=nullptr;
    if (std::uncaught_exceptions()) {
        if (b) { try { b->Release(); } catch (...) {} }
        if (a) { try { a->Release(); } catch (...) {} }
    } else {
        try { if (b) b->Release(); }
        catch (...) { if (a) { try { a->Release(); } catch (...) {} } throw; }
        if (a) a->Release();
    }
}

void BeforeStoredEnum(iTJSDispatch2 *object) {
    if (!object) return;
    for (auto *scope = factoryScans; scope; scope = scope->previous)
        if (scope->target == object) return;
    // An ordinary object scan must not initialize the side table and reenter
    // native factory construction while the first registry is being allocated.
    Registry *s = published.load(std::memory_order_acquire);
    if (!s || !s->active.load(std::memory_order_acquire)) return;
    if (!CanBePending(object)) return;
    {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        const auto pos = s->entries.find(object);
        if (pos != s->entries.end() && pos->second->expanding)
            throw std::runtime_error("Stored-member enumeration reentered incomplete lazy materialization");
    }
    Access(object);
}

void CaptureFactory(iTJSDispatch2 *factory, bool array) noexcept {
    try {
        Registry *s = State();
        if (!s || !factory) return;
        auto *base = dynamic_cast<tTJSNativeClass *>(factory);
        if (!base || base->GetSuper()) return;
        FactorySnapshot candidate;
        candidate.object = factory;
        candidate.valid = true;
        FactoryScanScope scanning(base);
        base->EnumStoredMembers([&](tTJSVariantString *name, tjs_uint32 flags, const tTJSVariant &v) {
            if (!name) { candidate.valid = false; return; }
            if (v.Type() == tvtObject) {
                const auto closure = v.AsObjectClosureNoAddRef();
                if (closure.ObjThis) { candidate.valid = false; return; }
                if (closure.Object) {
                    const auto &kind = typeid(*closure.Object);
                    if (kind != typeid(tTJSNativeClassMethod) &&
                        kind != typeid(tTJSNativeClassConstructor) &&
                        kind != typeid(tTJSNativeClassProperty)) {
                        candidate.valid = false; return;
                    }
                }
            }
            candidate.members.push_back({ttstr(name), flags, v});
        });
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        (array ? s->array : s->dictionary) = std::move(candidate);
    } catch (...) {
        // Failure must disable the proof rather than change factory creation.
    }
}

void ForgetFactory(iTJSDispatch2 *factory) noexcept {
    Registry *s = State();
    if (!s || !factory) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        if (s->array.object == factory) s->array = FactorySnapshot{};
        if (s->dictionary.object == factory) s->dictionary = FactorySnapshot{};
    } catch (...) {}
}

bool FactoriesPristine() noexcept {
    try {
        Registry *s = State();
        if (!s || TJSObjectHashMapEnabled() || std::get_new_handler()) return false;
        (void)TJS::UnsafeArrayFactoryNoCreateImpl();
        (void)TJS::UnsafeDictionaryFactoryNoCreateImpl();
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        return SnapshotMatches(s->array, true) && SnapshotMatches(s->dictionary, false);
    } catch (...) { return false; }
}

bool Register(iTJSDispatch2 *object, Callback callback) {
    Registry *s = State();
    if (!s || !object) return false;
    try {
        auto entry = std::make_shared<Entry>(std::move(callback));
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        if (s->entries.size() >= 8192) { ++s->capped; return false; }
        if (!s->entries.emplace(object, std::move(entry)).second)
            throw std::runtime_error("Duplicate exact-lazy object");
        ++s->registered;
        s->active.store(s->entries.size(), std::memory_order_release);
        return true;
    } catch (const std::bad_alloc &) { return false; }
}

void Access(iTJSDispatch2 *object) {
    Registry *s = State();
    if (!s || !object || !s->active.load(std::memory_order_acquire)) return;
    if (!CanBePending(object)) return;
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        auto pos = s->entries.find(object);
        if (pos == s->entries.end()) return;
        if (pos->second->expanding) {
            ++hookState.rejected;
            throw std::runtime_error("same target lazy expansion reentered");
        }
        entry = pos->second;
        if (entry->failure) std::rethrow_exception(entry->failure);
        entry->expanding = true;
    }
    const auto expandStart = std::chrono::steady_clock::now();
    try {
        entry->callback();
        const auto expandEnd = std::chrono::steady_clock::now();
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        auto pos = s->entries.find(object);
        if (pos != s->entries.end() && pos->second == entry) s->entries.erase(pos);
        ++s->expanded;
        ++s->expandCalls;
        {
            const double cost = std::chrono::duration<double, std::milli>(expandEnd - expandStart).count();
            s->expandMs += cost;
            if (cost > s->expandMaxMs) s->expandMaxMs = cost;
        }
        s->active.store(s->entries.size(), std::memory_order_release);
    } catch (...) {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        entry->expanding = false;
        // Transaction callback must explicitly poison an entry when cleanup
        // cannot be proven. A successfully cleaned stage may be retried.
        throw;
    }
}

void ObserveDestruction(iTJSDispatch2 *object) noexcept {
    try {
        auto *s=lifePublished.load(std::memory_order_acquire);
        if (!s) return;
        std::lock_guard<std::mutex> lock(s->mutex);
        auto pos=s->live.find(object);
        if (pos!=s->live.end()) { ++s->deaths[pos->second]; s->live.erase(pos); }
    } catch (...) {}
}

void Poison(iTJSDispatch2 *object, std::exception_ptr failure) noexcept {
    Registry *s = published.load(std::memory_order_acquire);
    if (!s || !object) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        auto pos=s->entries.find(object);
        if (pos!=s->entries.end()) pos->second->failure=failure;
    } catch (...) {}
}

void Forget(iTJSDispatch2 *object) noexcept {
    Registry *s = State();
    if (!s || !object || !s->active.load(std::memory_order_acquire)) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        s->entries.erase(object);
        s->active.store(s->entries.size(), std::memory_order_release);
        if (s->entries.empty())
            KRKRNS_LOG("[exact-lazy-proof] registered=%llu expanded=%llu capped=%llu live=0 expand-ms=%.1f expand-max-ms=%.1f expand-calls=%llu",
                static_cast<unsigned long long>(s->registered),
                static_cast<unsigned long long>(s->expanded),
                static_cast<unsigned long long>(s->capped),
                s->expandMs, s->expandMaxMs,
                static_cast<unsigned long long>(s->expandCalls));
    } catch (...) {}
}

bool BeforeClassMutation(iTJSDispatch2 *object) {
    Registry *s = State();
    if (!s || !object || !s->active.load(std::memory_order_acquire)) return false;
    static thread_local bool flushing = false;
    if (flushing) return false; // reentry here remains a known unsupported case
    {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        if (object != s->array.object && object != s->dictionary.object) return false;
    }
    flushing = true;
    try {
        for (;;) {
            iTJSDispatch2 *pending = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(s->mutex);
                if (s->entries.empty()) break;
                auto pos = s->entries.begin();
                if (pos->second->expanding)
                    throw std::runtime_error("Class mutation during lazy expansion");
                pending = pos->first;
                pending->AddRef(); // exact native receiver, protected by registry lock
            }
            try { Access(pending); }
            catch (...) { try { pending->Release(); } catch (...) {} throw; }
            pending->Release(); // outside registry lock
        }
        flushing = false;
        return true;
    } catch (...) { flushing = false; throw; }
}
} // namespace krkr_exact_proof

namespace krkr_lazy_transaction_test {
bool Arm(iTJSDispatch2 *target, Hook hook, void *context) {
    if (!target || !hook || krkr_exact_proof::hookState.target) return false;
    auto &h=krkr_exact_proof::hookState;
    h.target=target; h.hook=hook; h.context=context; h.calls=0; h.rejected=0;
    return true;
}
void Disarm() noexcept { auto &h=krkr_exact_proof::hookState; h.target=nullptr; h.hook=nullptr; h.context=nullptr; }
std::size_t HookCalls() noexcept { return krkr_exact_proof::hookState.calls; }
std::size_t ReentryRejections() noexcept { return krkr_exact_proof::hookState.rejected; }
bool RetryCleanup(iTJSDispatch2 *) { return false; } // deliberately permanent poison
RawState ReadRawState(iTJSDispatch2 *object) {
    RawState out{};
    auto *s=krkr_exact_proof::published.load(std::memory_order_acquire);
    if (s) {
        std::lock_guard<std::recursive_mutex> lock(s->mutex);
        out.liveEntries=s->entries.size();
        const auto it=s->entries.find(object);
        if (it!=s->entries.end()) {
            out.pending=true; out.expanding=it->second->expanding;
            out.cleanupBlocked=bool(it->second->failure);
        }
    }
    auto *base=dynamic_cast<TJS::tTJSCustomObject *>(object);
    if (!base) return out;
    out.count=base->UnsafeTxnCount(); out.hashSize=base->UnsafeTxnHashSize();
    out.hashMask=base->UnsafeTxnHashMask(); out.rebuildMagic=base->UnsafeTxnHashMagic();
    out.symbols=reinterpret_cast<std::uintptr_t>(base->UnsafeTxnSymbols());
    if (typeid(*object)==typeid(TJS::tTJSArrayObject)) {
        auto *ni=static_cast<TJS::tTJSArrayNI *>(base->UnsafeTxnNative(TJS::TJSGetArrayClassID()));
        if (ni) { out.arrayItems=ni->Items.size(); out.arrayData=reinterpret_cast<std::uintptr_t>(ni->Items.data()); }
    }
    return out;
}
bool ReadRawMember(iTJSDispatch2 *object, const tjs_char *name, tTJSVariant &out) {
    auto *base=dynamic_cast<TJS::tTJSCustomObject *>(object);
    return base && name && base->UnsafeTxnReadMember(name,out);
}
bool InjectPendingArrayNative(iTJSDispatch2 *object, tjs_int32 id, TJS::iTJSNativeInstance *native) {
    if (!object || typeid(*object)!=typeid(TJS::tTJSArrayObject)) return false;
    auto *s=krkr_exact_proof::published.load(std::memory_order_acquire);
    if (!s) return false;
    std::lock_guard<std::recursive_mutex> lock(s->mutex);
    const auto pos=s->entries.find(object);
    if (pos==s->entries.end() || pos->second->expanding || pos->second->failure) return false;
    return static_cast<TJS::tTJSCustomObject *>(object)->UnsafeTxnTestInstallNative(id,native);
}
TJS::iTJSNativeInstance *ReadRawNative(iTJSDispatch2 *object, tjs_int32 id) {
    auto *base=dynamic_cast<TJS::tTJSCustomObject *>(object);
    return base ? base->UnsafeTxnNative(id) : nullptr;
}
std::uint64_t LifetimeToken(iTJSDispatch2 *object) {
    if (!object) return 0;
    auto &s=krkr_exact_proof::Life(); std::lock_guard<std::mutex> lock(s.mutex);
    auto it=s.live.find(object); if (it!=s.live.end()) return it->second;
    const auto id=s.next++; s.live.emplace(object,id); s.deaths.emplace(id,0); return id;
}
std::size_t Destructions(std::uint64_t token) noexcept {
    try { auto &s=krkr_exact_proof::Life(); std::lock_guard<std::mutex> lock(s.mutex);
        auto it=s.deaths.find(token); return it==s.deaths.end()?0:it->second;
    } catch (...) { return 0; }
}
}
