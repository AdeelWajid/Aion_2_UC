// Memory.hpp — process memory abstraction shared by external (RPM) and internal contexts.
//
// Every typed accessor goes through IMemory so the identical UE layer (NamePool,
// GObjects, UObject, reflection, world) runs both out-of-process (validation tool)
// and in-process (injected DLL) without duplication.
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Logger.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#  include "KernMem.hpp"   // the broker transport: a shared section, not a process handle
#endif

namespace aion {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using addr_t = std::uintptr_t;

// ---------------------------------------------------------------- IMemory ----
class IMemory {
public:
    virtual ~IMemory() = default;

    virtual bool read(addr_t addr, void* out, std::size_t n) const = 0;
    virtual bool write(addr_t addr, const void* in, std::size_t n) = 0;
    virtual addr_t module_base() const = 0;
    virtual std::size_t module_size() const { return 0; }
    virtual bool writable() const { return false; }

    // Attach diagnostics (see Sdk::Diagnose). The defaults describe the in-process backend;
    // ExternalMemory reports its own handle state, because a refused process handle and a
    // different client build both leave every global reading zero.
    virtual std::uint32_t pid() const { return 0; }
    virtual std::string image_path() const { return {}; }
    virtual std::uint64_t image_file_size() const { return 0; }
    virtual unsigned reopen_count() const { return 0; }
    virtual unsigned open_fail_count() const { return 0; }
    virtual unsigned last_open_error() const { return 0; }
    virtual unsigned stripped_count() const { return 0; }

    template <class T>
    std::optional<T> try_read(addr_t addr) const {
        T v{};
        if (!read(addr, &v, sizeof(T))) return std::nullopt;
        return v;
    }
    template <class T>
    T read_or(addr_t addr, T fallback = T{}) const {
        T v{};
        return read(addr, &v, sizeof(T)) ? v : fallback;
    }

    u8   u8_at(addr_t a)  const { return read_or<u8>(a); }
    u16  u16_at(addr_t a) const { return read_or<u16>(a); }
    u32  u32_at(addr_t a) const { return read_or<u32>(a); }
    i32  i32_at(addr_t a) const { return read_or<i32>(a); }
    u64  u64_at(addr_t a) const { return read_or<u64>(a); }
    float f32_at(addr_t a) const { return read_or<float>(a); }
    double f64_at(addr_t a) const { return read_or<double>(a); }
    addr_t ptr_at(addr_t a) const { return static_cast<addr_t>(u64_at(a)); }

    // Null-terminated ASCII/UTF-8 read with a hard cap.
    std::string cstr(addr_t a, std::size_t max = 256) const {
        std::string s;
        s.reserve(32);
        for (std::size_t i = 0; i < max; ++i) {
            u8 c = u8_at(a + i);
            if (!c) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }

    bool valid_ptr(addr_t a) const { return a > 0x10000 && a < 0x00007FFFFFFFFFFFULL; }
    // Inside the client's main module. The span defaults to the module's real mapped size: the fixed
    // 0x20000000 this used to carry accepts any pointer up to 256 MB past the end of the image, and
    // rejects valid ones in a client whose image grows past that (this one is 0x19779000 = 408 MB,
    // so it is already within a factor of two of the guess). The guess survives only as a fallback
    // for an IMemory that cannot report a size.
    bool in_module(addr_t a, u64 span = 0) const {
        const addr_t b = module_base();
        if (b == 0) return false;
        const u64 n = span ? span : (module_size() ? (u64)module_size() : 0x20000000ULL);
        return a >= b && a < b + n;
    }

    // UE TArray<T> header: [data ptr][num i32][max i32]
    template <class T>
    std::vector<T> read_tarray(addr_t header, std::size_t cap = 100000) const {
        std::vector<T> out;
        addr_t data = ptr_at(header);
        i32 num = i32_at(header + 8);
        if (!valid_ptr(data) || num <= 0 || static_cast<std::size_t>(num) > cap) return out;
        out.resize(static_cast<std::size_t>(num));
        if (!read(data, out.data(), sizeof(T) * out.size())) out.clear();
        return out;
    }

    // UE FString: [TCHAR* data][i32 num][i32 max] ; UTF-16LE. Non-ASCII is folded to '?'
    // unless `utf8` is set, which the dump files use so names stay readable.
    std::string read_fstring(addr_t header, std::size_t cap = 512, bool utf8 = false) const {
        addr_t data = ptr_at(header);
        i32 num = i32_at(header + 8);
        if (!valid_ptr(data) || num <= 0 || static_cast<std::size_t>(num) > cap) return {};
        std::vector<char16_t> buf(static_cast<std::size_t>(num));
        if (!read(data, buf.data(), sizeof(char16_t) * buf.size())) return {};
        std::string s;
        s.reserve(buf.size());
        for (char16_t c : buf) {
            if (!c) break;
            // Control characters mean this is not a text string (tabs and newlines are
            // legitimate in tooltips and descriptions).
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return {};
            if (c < 0x80) {
                s.push_back(static_cast<char>(c));
            } else if (!utf8) {
                s.push_back('?');
            } else if (c < 0x800) {
                s.push_back(static_cast<char>(0xC0 | (c >> 6)));
                s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                s.push_back(static_cast<char>(0xE0 | (c >> 12)));
                s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return s;
    }

    // UE FText: [FTextData* shared object][shared ref controller][u32 flags]. The string
    // lives behind FTextData, whose layout moved between engine versions (the display text
    // sits after the inline history union), so scan the start of FTextData for a plausible
    // FString rather than trusting one offset. A None FText is a null pointer, so empty
    // text yields nothing.
    std::string read_ftext(addr_t header, std::size_t cap = 256, bool utf8 = false) const {
        const addr_t d = ptr_at(header);
        if (!valid_ptr(d)) return {};
        for (addr_t off = 0x8; off <= 0x60; off += 8) {
            const addr_t p = d + off;
            const addr_t sd = ptr_at(p);
            const i32 n = i32_at(p + 8);
            const i32 mx = i32_at(p + 12);
            if (!valid_ptr(sd) || n < 1 || static_cast<std::size_t>(n) > cap) continue;
            if (mx < n || mx > 8192) continue;
            std::vector<char16_t> w(static_cast<std::size_t>(n));
            if (!read(sd, w.data(), sizeof(char16_t) * w.size())) continue;
            bool printable = true;
            for (char16_t c : w) {
                if (!c) break;
                if (c < 0x20 || c == 0xFFFD || c == 0xFFFF) { printable = false; break; }
            }
            if (!printable) continue;
            std::string s = read_fstring(p, cap, utf8);  // same formatting as FString
            if (!s.empty()) return s;
        }
        return {};
    }
};

// --------------------------------------------------------- InternalMemory ----
// Used when this code runs inside the target process. Direct, no syscalls.
class InternalMemory final : public IMemory {
public:
    explicit InternalMemory(addr_t base = 0) : base_(base) {}

    bool read(addr_t addr, void* out, std::size_t n) const override {
        if (!addr || !out || !n) return false;
#if defined(_WIN32)
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(addr), n);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
#else
        std::memcpy(out, reinterpret_cast<const void*>(addr), n);
#endif
        return true;
    }
    bool write(addr_t addr, const void* in, std::size_t n) override {
        if (!addr || !in || !n) return false;
#if defined(_WIN32)
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(reinterpret_cast<void*>(addr), in, n);
        VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
#else
        std::memcpy(reinterpret_cast<void*>(addr), in, n);
#endif
        return true;
    }
    addr_t module_base() const override { return base_; }
    bool writable() const override { return true; }
    void set_module_base(addr_t b) { base_ = b; }

private:
    addr_t base_;
};

#if defined(_WIN32)
// --------------------------------------------------------- ExternalMemory ----
class ExternalMemory final : public IMemory {
public:
    ExternalMemory() = default;
    ~ExternalMemory() override { Close(); }

    // log=false keeps a refused open quiet, so a caller polling several candidates can report one
    // state line per candidate itself instead of an error per attempt; the error code is recorded
    // either way so the caller can distinguish a dead pid (87) from a protected one (5).
    bool Attach(std::uint32_t pid, bool log = true) {
        Close();
        pid_ = pid;

        // A running broker serves reads out of the kernel: no process handle is opened for reading
        // and the anti-cheat has nothing to strip. Without one, everything below is what it was.
        kernmem_.attach(0);
        if (kernmem_.attached() && log)
            AION_INFO("pid %u: reading through kernmemd (kernel path)", pid);

        handle_query_ = OpenProcess(kQueryAccess, FALSE, pid);
        primed_[2] = handle_query_ != nullptr;  // opened here, not through Acquire: not a re-open
        if (!handle_query_) {
            open_last_error_ = GetLastError();
            if (log) AION_ERROR("OpenProcess(%u) failed err=%lu", pid, open_last_error_);
            pid_ = 0;
            return false;
        }
        base_ = FindMainModule(pid, size_);
        return base_ != 0;
    }
    void Close() {
        std::lock_guard<std::mutex> lk(handle_mx_);
        kernmem_.detach();
        HANDLE* slots[] = {&handle_read_, &handle_write_, &handle_query_};
        for (HANDLE* h : slots)
            if (*h) { CloseHandle(*h); *h = nullptr; }
        for (bool& p : primed_) p = false;  // a fresh attach re-baselines the re-open counter
    }

    bool read(addr_t addr, void* out, std::size_t n) const override {
        if (!addr || !out || !n) return false;
        if (kernmem_read(addr, out, n)) return true;
        // The anti-cheat periodically closes our process handle (~every few seconds).
        // Transparently re-open and retry so reads never die. NOTE: only a dead handle
        // warrants a re-open - ERROR_PARTIAL_COPY just means the address is unreadable.
        for (int attempt = 0; attempt < 3; ++attempt) {
            HANDLE h = AcquireRead();
            if (!h) { Sleep(1); continue; }
            SIZE_T got = 0;
            if (::ReadProcessMemory(h, reinterpret_cast<const void*>(addr), out, n, &got) &&
                got == n)
                return true;
            const DWORD err = GetLastError();
            if (err == ERROR_INVALID_HANDLE || err == ERROR_ACCESS_DENIED) {
                ++stripped_count_;  // handle stripped by the anti-cheat -> fresh handle
                Drop(handle_read_, h);
                continue;
            }
            if (err == ERROR_PARTIAL_COPY) return false;  // unmapped / partially mapped
            Sleep(0);            // target may be mid-write; yield and retry once
        }
        return false;
    }
    bool write(addr_t addr, const void* in, std::size_t n) override {
        if (!addr || !in || !n) return false;
        if (kernmem_write(addr, in, n)) return true;
        for (int attempt = 0; attempt < 4; ++attempt) {
            HANDLE h = AcquireWrite();
            if (!h) { Sleep(1); continue; }
            DWORD old = 0;
            if (!VirtualProtectEx(h, reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE,
                                  &old)) {
                const DWORD err = GetLastError();
                if (err == ERROR_INVALID_HANDLE || err == ERROR_ACCESS_DENIED) {
                    Drop(handle_write_, h);
                    continue;
                }
                // Page may already be writable; try the write anyway.
            }
            SIZE_T wrote = 0;
            const bool ok = ::WriteProcessMemory(h, reinterpret_cast<void*>(addr), in, n, &wrote) &&
                            wrote == n;
            if (ok) {
                VirtualProtectEx(h, reinterpret_cast<void*>(addr), n, old, &old);
                return true;
            }
            Drop(handle_write_, h);
        }
        return false;
    }
    addr_t module_base() const override { return base_; }
    std::size_t module_size() const override { return size_; }
    bool writable() const override { return true; }
    void* handle() const { return handle_read_ ? handle_read_ : handle_write_; }
    std::uint32_t pid() const override { return pid_; }
    // True when reads and writes are going through the broker rather than a process handle.
    bool kernmem_active() const { return kernmem_.attached(); }
    unsigned kernmem_fail_count() const { return kernmem_fail_count_; }
    // Number of times the handle had to be re-opened (anti-cheat stripping).
    unsigned reopen_count() const override { return reopen_count_; }
    unsigned open_fail_count() const override { return open_fail_count_; }
    unsigned last_open_error() const override { return open_last_error_; }
    unsigned stripped_count() const override { return stripped_count_; }

    // Full image path and on-disk size of the client, for the "is this the build the offsets
    // were derived from" check. Falls back to a query-only handle when ours has been stripped.
    std::string image_path() const override {
        char buf[MAX_PATH * 2]{};
        DWORD n = static_cast<DWORD>(sizeof(buf));
        HANDLE h = AcquireQuery();
        if (h && ::QueryFullProcessImageNameA(h, 0, buf, &n)) return std::string(buf, n);
        h = OpenProcess(kQueryAccess, FALSE, pid_);
        if (!h) return {};
        n = static_cast<DWORD>(sizeof(buf));
        const bool ok = ::QueryFullProcessImageNameA(h, 0, buf, &n) != 0;
        CloseHandle(h);
        return ok ? std::string(buf, n) : std::string{};
    }
    std::uint64_t image_file_size() const override {
        const std::string p = image_path();
        if (p.empty()) return 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
        return (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    static std::uint32_t FindPidByName(const wchar_t* exe_name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        std::uint32_t pid = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exe_name) == 0) { pid = pe.th32ProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    // Every match, not just the first: a stale AION2.exe from an earlier launch can sit in the
    // process list beside the live client, and the snapshot order decides which one a single
    // lookup picks. Attaching to the wrong one reads a client that never initialises anything.
    static std::vector<std::uint32_t> FindPidsByName(const wchar_t* exe_name) {
        std::vector<std::uint32_t> out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exe_name) == 0) out.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return out;
    }

    // Is this pid still running? The launch chain makes this necessary: AION2.exe starts as an
    // NCGuard bootstrap that installs, relaunches the real client with a -bbId token and exits,
    // so a pid collected moments ago can be gone by the next poll. OpenProcess on a vanished pid
    // fails with ERROR_INVALID_PARAMETER (87) while a live-but-protected client fails with
    // ERROR_ACCESS_DENIED (5); only the former proves the process is gone, so anything else is
    // treated as alive rather than dropping a client we merely lack rights to.
    static bool IsAlive(std::uint32_t pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return GetLastError() != ERROR_INVALID_PARAMETER;
        const DWORD w = WaitForSingleObject(h, 0);
        CloseHandle(h);
        return w == WAIT_TIMEOUT;
    }

private:
    // NCGuard closes any AION2 handle that joins PROCESS_VM_READ with a QUERY bit *or* with
    // PROCESS_VM_WRITE (measured 2026-10-10: 0x1010/0x410 die in ~1.4-3.4 s, 0x38 in ~1.6 s,
    // while 0x10, 0x28 and 0x1000 each survive indefinitely). Hold single-purpose handles so a
    // stripped combination is never present.
    static constexpr DWORD kReadAccess  = PROCESS_VM_READ;                          // 0x10
    static constexpr DWORD kWriteAccess = PROCESS_VM_WRITE | PROCESS_VM_OPERATION;  // 0x28
    static constexpr DWORD kQueryAccess = PROCESS_QUERY_LIMITED_INFORMATION;        // 0x1000

    // One flag per slot: has this slot been opened at least once since Attach? The FIRST open of a
    // slot is acquisition, not a re-open - and because Attach opens only the query handle (the read
    // handle is created lazily by the first read), counting it made the overlay report "reopens 1"
    // in the steady state and colour the HUD row amber forever, which hid the one event the counter
    // exists to show: the anti-cheat stripping a handle. Cleared by Close(), so a fresh attach
    // re-baselines and a client restart is not reported as a strip.
    mutable bool primed_[3] = {false, false, false};

    // The kernel path, when a broker is publishing. A request that cannot go through it falls back
    // to the process handle, so nothing that worked before stops working; the counter says how
    // often that happened, which is what tells the two apart while diagnosing.
    bool kernmem_read(addr_t addr, void* out, std::size_t n) const {
        if (!kernmem_active_()) return false;
        unsigned long got = 0;
        if (kernmem_.read(pid_, addr, out, static_cast<unsigned long>(n), &got) && got == n)
            return true;
        ++kernmem_fail_count_;
        return false;
    }
    bool kernmem_write(addr_t addr, const void* in, std::size_t n) const {
        if (!kernmem_active_()) return false;
        unsigned long moved = 0;
        if (kernmem_.write(pid_, addr, in, static_cast<unsigned long>(n), &moved) && moved == n)
            return true;
        ++kernmem_fail_count_;
        return false;
    }
    // A broker may start after this process did, so the section is looked for again now and then.
    bool kernmem_active_() const {
        if (kernmem_.attached()) return true;
        if ((kernmem_poll_++ & 0xFF) != 0) return false;
        return kernmem_.attach(0);
    }

    HANDLE AcquireRead() const  { return Acquire(kReadAccess,  handle_read_,  primed_[0]); }
    HANDLE AcquireWrite() const { return Acquire(kWriteAccess, handle_write_, primed_[1]); }
    HANDLE AcquireQuery() const { return Acquire(kQueryAccess, handle_query_, primed_[2]); }

    // Serialize handle (re)creation across threads; reads are otherwise lock-free.
    HANDLE Acquire(DWORD access, HANDLE& slot, bool& primed) const {
        if (slot) return slot;
        std::lock_guard<std::mutex> lk(handle_mx_);
        if (slot) return slot;
        slot = OpenProcess(access, FALSE, pid_);
        if (slot) {
            if (primed) ++reopen_count_;  // this slot held a handle before: this IS a re-open
            primed = true;
        } else {
            // Keep the reason: on a foreign machine this separates "the overlay is not
            // permitted to read the game" from "the game build is different".
            ++open_fail_count_;
            open_last_error_ = GetLastError();
        }
        return slot;
    }
    void Drop(HANDLE& slot, HANDLE h) const {
        std::lock_guard<std::mutex> lk(handle_mx_);
        if (slot == h) {
            CloseHandle(slot);
            slot = nullptr;
        }
    }

    static addr_t FindMainModule(std::uint32_t pid, std::size_t& size_out) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        addr_t base = 0;
        if (Module32FirstW(snap, &me)) {
            base = reinterpret_cast<addr_t>(me.modBaseAddr);
            size_out = me.modBaseSize;
        }
        CloseHandle(snap);
        return base;
    }

    std::uint32_t pid_ = 0;
    addr_t base_ = 0;
    std::size_t size_ = 0;
    mutable HANDLE handle_read_ = nullptr;   // PROCESS_VM_READ
    mutable HANDLE handle_write_ = nullptr;  // PROCESS_VM_WRITE | PROCESS_VM_OPERATION
    mutable HANDLE handle_query_ = nullptr;  // PROCESS_QUERY_LIMITED_INFORMATION
    mutable aion::KernMem kernmem_;          // the broker transport, when one is running
    mutable unsigned kernmem_poll_ = 0;      // how often to look again for a broker that came later
    mutable unsigned kernmem_fail_count_ = 0;
    mutable std::mutex handle_mx_;
    mutable unsigned reopen_count_ = 0;
    mutable unsigned open_fail_count_ = 0;   // OpenProcess refused (elevation / AV)
    mutable unsigned open_last_error_ = 0;   // Win32 error of the last refusal
    mutable unsigned stripped_count_ = 0;    // reads that hit a stripped handle
};
#endif  // _WIN32

}  // namespace aion
