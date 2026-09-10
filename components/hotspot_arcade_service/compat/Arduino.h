// Minimal Arduino compatibility used only by the vendored Hotspot Arcade engine.
// Derived from upstream sim/engine/Arduino.h; the clock, RNG and PSRAM allocator
// are backed directly by ESP-IDF rather than Arduino Core.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctype.h>
#include <math.h>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>

class String {
public:
    String() : data_(nullptr), length_(0), capacity_(0) {}

    String(const char* value) : String() {
        assign(value ? value : "");
    }

    String(char value) : String() {
        char text[2] = {value, '\0'};
        assign(text);
    }

    String(int value) : String() {
        assign_number("%d", value);
    }

    String(unsigned value) : String() {
        assign_number("%u", value);
    }

    String(long value) : String() {
        assign_number("%ld", value);
    }

    String(unsigned long value) : String() {
        assign_number("%lu", value);
    }

    String(const String& other) : String() {
        assign(other.c_str());
    }

    String(String&& other) noexcept
        : data_(other.data_), length_(other.length_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.length_ = 0;
        other.capacity_ = 0;
    }

    ~String() {
        std::free(data_);
    }

    String& operator=(const String& other) {
        if(this != &other) assign(other.c_str());
        return *this;
    }

    String& operator=(String&& other) noexcept {
        if(this == &other) return *this;
        std::free(data_);
        data_ = other.data_;
        length_ = other.length_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.length_ = 0;
        other.capacity_ = 0;
        return *this;
    }

    const char* c_str() const {
        return data_ ? data_ : "";
    }

    size_t length() const {
        return length_;
    }

    void reserve(size_t size) {
        ensure_capacity(size + 1);
    }

    String& operator+=(const String& other) {
        if(other.length_ == 0) return *this;
        if(this == &other) {
            const size_t original_length = length_;
            if(original_length > (SIZE_MAX - 1) / 2 ||
               !ensure_capacity(original_length * 2 + 1))
                return *this;
            memmove(data_ + original_length, data_, original_length);
            length_ = original_length * 2;
            data_[length_] = '\0';
            return *this;
        }
        if(other.length_ > SIZE_MAX - length_ - 1 ||
           !ensure_capacity(length_ + other.length_ + 1))
            return *this;
        memcpy(data_ + length_, other.c_str(), other.length_ + 1);
        length_ += other.length_;
        return *this;
    }

    friend String operator+(String lhs, const String& rhs) {
        lhs += rhs;
        return lhs;
    }

private:
    bool ensure_capacity(size_t required) {
        if(required <= capacity_) return true;
        size_t next = capacity_ ? capacity_ : 16;
        while(next < required) {
            if(next > SIZE_MAX / 2) {
                next = required;
                break;
            }
            next *= 2;
        }
        void* resized = std::realloc(data_, next);
        if(!resized) return false;
        data_ = static_cast<char*>(resized);
        capacity_ = next;
        if(length_ == 0) data_[0] = '\0';
        return true;
    }

    void assign(const char* value) {
        const size_t size = std::strlen(value);
        if(!ensure_capacity(size + 1)) return;
        memmove(data_, value, size + 1);
        length_ = size;
    }

    template <typename T>
    void assign_number(const char* format, T value) {
        char text[32];
        const int written = std::snprintf(text, sizeof(text), format, value);
        if(written > 0) assign(text);
    }

    char* data_;
    size_t length_;
    size_t capacity_;
};

inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

inline long random(long upper_bound) {
    return upper_bound > 0 ? static_cast<long>(esp_random() % static_cast<uint32_t>(upper_bound)) : 0;
}

inline void* ps_malloc(size_t size) {
    void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!memory) memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return memory;
}
