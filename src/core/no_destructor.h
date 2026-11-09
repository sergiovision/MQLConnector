// Storage for an object that is constructed on first use and intentionally never destroyed.
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace mqlc::core {

/// Used for the DLL's process-wide session: its destructor would join threads, and static
/// destructors of a DLL run under the loader lock (DLL_PROCESS_DETACH), where joining can
/// deadlock. The OS reclaims everything at process exit instead.
template <class T>
class NoDestructor {
public:
    template <class... Args>
    explicit NoDestructor(Args&&... args) {
        std::construct_at(reinterpret_cast<T*>(storage_), std::forward<Args>(args)...);
    }
    ~NoDestructor() = default;  // deliberately does not destroy the T

    NoDestructor(const NoDestructor&) = delete;
    NoDestructor& operator=(const NoDestructor&) = delete;

    [[nodiscard]] T& get() noexcept { return *pointer(); }

private:
    T* pointer() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }

    alignas(T) std::byte storage_[sizeof(T)];
};

}  // namespace mqlc::core
