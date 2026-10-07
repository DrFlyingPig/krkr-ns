/* SPDX-License-Identifier: MIT */
/* Optional VM-entry timing. Only owned labels and slot numbers survive entry;
 * neither a context, its weak Parent, nor a script block is retained. */
#ifndef KRKRNS_VM_PROF_H
#define KRKRNS_VM_PROF_H

#include <cstdint>

#if defined(__SWITCH__) || defined(KRKRNS_VM_PROF_TEST)
struct KrkrNSVMProfLabel
{
    std::uint64_t hash = 14695981039346656037ull;
    int type = 0, line = 0, ip = 0;
    bool truncated = false;
    char name[192] = {}, script[512] = {};
};

class KrkrNSVMProfScope;
bool krkrsdl2_vmprof_active() noexcept;
void krkrsdl2_vmprof_enter(KrkrNSVMProfScope *, const KrkrNSVMProfLabel &) noexcept;
void krkrsdl2_vmprof_leave(KrkrNSVMProfScope *) noexcept;
void krkrsdl2_vmprof_begin_dispatch() noexcept;
void krkrsdl2_vmprof_end_dispatch() noexcept;
void krkrsdl2_vmprof_report() noexcept;
bool krkrsdl2_vmprof_native_begin(std::uint64_t &) noexcept;
void krkrsdl2_vmprof_native_end(const char *, std::uint64_t) noexcept;

class KrkrNSVMProfScope
{
    friend void krkrsdl2_vmprof_enter(KrkrNSVMProfScope *, const KrkrNSVMProfLabel &) noexcept;
    friend void krkrsdl2_vmprof_leave(KrkrNSVMProfScope *) noexcept;
    KrkrNSVMProfScope *parent_ = nullptr;
    std::uint64_t start_ = 0, children_ = 0;
    unsigned slot_ = 0;
    bool linked_ = false;

    template<class Char, unsigned Size>
    static void CopyLabel(const Char *text, const char *fallback, char (&out)[Size],
                          KrkrNSVMProfLabel &label) noexcept
    {
        unsigned used = 0;
        auto put = [&](unsigned char byte) {
            // Hash the complete label even when the display text is truncated.
            label.hash = (label.hash ^ byte) * 1099511628211ull;
            if (used + 1 < Size) out[used++] = static_cast<char>(byte);
            else label.truncated = true;
        };
        if (!text || !*text)
        {
            while (*fallback) put(static_cast<unsigned char>(*fallback++));
        }
        else
        {
            while (*text)
            {
                std::uint32_t c = static_cast<std::uint32_t>(*text++);
                if (c >= 0xd800 && c <= 0xdbff && *text >= 0xdc00 && *text <= 0xdfff)
                    c = 0x10000 + ((c - 0xd800) << 10) + (static_cast<unsigned>(*text++) - 0xdc00);
                if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
                if (c > 0x10ffff) c = 0xfffd;
                if (c < 0x80) put(static_cast<unsigned char>(c));
                else if (c < 0x800)
                {
                    put(static_cast<unsigned char>(0xc0 | (c >> 6)));
                    put(static_cast<unsigned char>(0x80 | (c & 0x3f)));
                }
                else if (c < 0x10000)
                {
                    put(static_cast<unsigned char>(0xe0 | (c >> 12)));
                    put(static_cast<unsigned char>(0x80 | ((c >> 6) & 0x3f)));
                    put(static_cast<unsigned char>(0x80 | (c & 0x3f)));
                }
                else
                {
                    put(static_cast<unsigned char>(0xf0 | (c >> 18)));
                    put(static_cast<unsigned char>(0x80 | ((c >> 12) & 0x3f)));
                    put(static_cast<unsigned char>(0x80 | ((c >> 6) & 0x3f)));
                    put(static_cast<unsigned char>(0x80 | (c & 0x3f)));
                }
            }
        }
        out[used] = 0;
        label.hash = (label.hash ^ 0xffu) * 1099511628211ull;
    }

public:
    KrkrNSVMProfScope() noexcept = default;
    KrkrNSVMProfScope(const KrkrNSVMProfScope &) = delete;
    KrkrNSVMProfScope &operator=(const KrkrNSVMProfScope &) = delete;
    ~KrkrNSVMProfScope() noexcept
    {
        if (linked_) krkrsdl2_vmprof_leave(this);
    }

    template<class Char>
    void Begin(const Char *name, int type, const Char *script, int line, int ip) noexcept
    {
        if (!krkrsdl2_vmprof_active()) return;
        // Entry copying cannot call TJS. A TLS scratch label avoids adding a
        // large buffer to every recursive VM frame; enter copies it into a slot.
        static thread_local KrkrNSVMProfLabel label;
        label = KrkrNSVMProfLabel{};
        label.type = type; label.line = line; label.ip = ip;
        CopyLabel(name, "<anonymous>", label.name, label);
        CopyLabel(script, "<no-script>", label.script, label);
        krkrsdl2_vmprof_enter(this, label);
    }
};

// Operation names at these top-level boundaries are static string literals.
// The timer never retains the object whose cleanup it measures.
class KrkrNSVMProfNativeScope
{
    const char *name_;
    std::uint64_t start_ = 0;
    bool sampled_;
public:
    explicit KrkrNSVMProfNativeScope(const char *name) noexcept
        : name_(name), sampled_(krkrsdl2_vmprof_native_begin(start_)) {}
    KrkrNSVMProfNativeScope(const KrkrNSVMProfNativeScope &) = delete;
    ~KrkrNSVMProfNativeScope() noexcept
    {
        if (sampled_) krkrsdl2_vmprof_native_end(name_, start_);
    }
};

class KrkrNSVMProfDispatchScope
{
    bool finished_ = false;
public:
    KrkrNSVMProfDispatchScope() noexcept { krkrsdl2_vmprof_begin_dispatch(); }
    KrkrNSVMProfDispatchScope(const KrkrNSVMProfDispatchScope &) = delete;
    ~KrkrNSVMProfDispatchScope() noexcept
    {
        // Exception unwinding must also disarm capture. Normal reporting is
        // deferred until the outer loop has recorded its draw timestamps.
        Finish();
    }
    void Finish() noexcept
    {
        if (!finished_) { finished_ = true; krkrsdl2_vmprof_end_dispatch(); }
    }
};

#ifdef KRKRNS_VM_PROF_TEST
struct KrkrNSVMProfTestRecord
{
    KrkrNSVMProfLabel label;
    std::uint64_t inclusive = 0, self = 0, entries = 0;
};
void krkrsdl2_vmprof_test_begin(bool enabled) noexcept;
void krkrsdl2_vmprof_test_counter(std::uint64_t value) noexcept;
bool krkrsdl2_vmprof_test_find(const char *, KrkrNSVMProfTestRecord &) noexcept;
std::uint64_t krkrsdl2_vmprof_test_roots() noexcept;
bool krkrsdl2_vmprof_test_pending() noexcept;
void krkrsdl2_vmprof_test_fail_allocation(bool enabled) noexcept;
std::uint64_t krkrsdl2_vmprof_test_counter_reads() noexcept;
#endif

#else
inline bool krkrsdl2_vmprof_active() noexcept { return false; }
inline void krkrsdl2_vmprof_report() noexcept {}
class KrkrNSVMProfScope
{
public:
    template<class Char> void Begin(const Char *, int, const Char *, int, int) noexcept {}
};
class KrkrNSVMProfDispatchScope
{
public:
    void Finish() noexcept {}
};
class KrkrNSVMProfNativeScope
{
public:
    explicit KrkrNSVMProfNativeScope(const char *) noexcept {}
};
#endif
#endif
