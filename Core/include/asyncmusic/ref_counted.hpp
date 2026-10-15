//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <utility>


namespace asyncmusic {

/// Intrusive reference count for the core's shared objects. Swift imports these classes as
/// reference types (through `SWIFT_SHARED_REFERENCE`); C++ code holds them with `Ref<T>`.
/// Objects start with one reference, owned by whoever called `create`.
class RefCounted {
public:
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    void retain() const { references_.fetch_add(1, std::memory_order_relaxed); }

    void release() const {
        if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

protected:
    RefCounted() = default;
    virtual ~RefCounted() = default;

private:
    mutable std::atomic<int> references_{1};
};

/// Owning handle for a `RefCounted` object.
template <typename T>
class Ref {
public:
    Ref() = default;
    /// Takes over the reference returned by `create`.
    static Ref adopt(T* object) {
        Ref ref;
        ref.object_ = object;
        return ref;
    }
    Ref(const Ref& other) : object_(other.object_) {
        if (object_) object_->retain();
    }
    Ref(Ref&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {}
    Ref& operator=(Ref other) noexcept {
        std::swap(object_, other.object_);
        return *this;
    }
    ~Ref() {
        if (object_) object_->release();
    }

    T* get() const { return object_; }
    T* operator->() const { return object_; }
    T& operator*() const { return *object_; }
    explicit operator bool() const { return object_ != nullptr; }

private:
    T* object_ = nullptr;
};

}  // namespace asyncmusic
