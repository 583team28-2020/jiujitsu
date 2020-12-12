#pragma once

#include <cstdint>
#include <functional>

class intmap {
    enum bucket_status {
        EMPTY, GHOST, FILLED
    };

    struct bucket {
        bucket_status status : 2;
        uint64_t key: 62;
        uint64_t value : 64;

        bucket();
        inline void fill(uint64_t key_in, uint64_t value_in);
        inline void evict();
        inline void clear();
    };

    bucket* data;
    uint32_t _size, _capacity, _mask;

    inline void init(uint32_t size);
    inline void swap(uint64_t& a, uint64_t& b);
    inline void free();
    void copy(const bucket* bs);
    void grow();
    inline uint64_t hash(uint64_t k) const;
public:
    intmap();
    ~intmap();
    intmap(const intmap& other);
    intmap& operator=(const intmap& other);

    class const_iterator {
        const bucket *ptr, *end;
        friend class set;
    public:
        const_iterator(const bucket* ptr_in, const bucket* end_in);
        std::pair<uint64_t, uint64_t> operator*() const;
        const_iterator& operator++();
        const_iterator operator++(int);
        bool operator==(const const_iterator& other) const;
        bool operator!=(const const_iterator& other) const;
    };

    const_iterator begin() const;
    const_iterator end() const;
    void emplace(uint64_t k, uint64_t v);
    void erase(uint64_t k);
    const_iterator find(uint64_t k) const;
    uint32_t size() const;
    uint32_t capacity() const;
};
