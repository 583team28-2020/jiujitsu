#include "hash.h"

intmap::bucket::bucket(): status(EMPTY) {
	//
}

inline void intmap::bucket::fill(uint64_t key_in, uint64_t value_in) {
	if (status != FILLED) {
		status = FILLED;
	}
	key = key_in, value = value_in;
}

inline void intmap::bucket::evict() {
	status = GHOST;
}

inline void intmap::bucket::clear() {
	status = EMPTY;
}

inline void intmap::init(uint32_t size) {
	_size = 0, _capacity = size;
	data = new bucket[size];
	_mask = size - 1;
}

inline void intmap::swap(uint64_t& a, uint64_t& b) {
	uint64_t t = a;
	a = b;
	b = t;
}

inline void intmap::free() {
	delete[] data;
}

void intmap::copy(const bucket* bs) {
	for (uint32_t i = 0; i < _capacity; ++ i) {
		data[i] = bs[i];
	}
}

void intmap::grow() {
	bucket* old = data;
	uint32_t oldsize = _capacity;
	init(_capacity * 4);
	for (uint32_t i = 0; i < oldsize; ++ i) {
		if (old[i].status == FILLED) emplace(old[i].key, old[i].value);
	}
	delete[] old;
}

inline uint64_t intmap::hash(uint64_t k) const {
	k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ul;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebul;
    k = k ^ (k >> 31);
    return k;
}

intmap::intmap() {
	init(8);
}

intmap::~intmap() {
	free();
}

intmap::intmap(const intmap& other) {
	init(other._capacity);
	_size = other._size, _mask = other._mask;
	copy(other.data);
}

intmap& intmap::operator=(const intmap& other) {
	if (this != &other) {
		free();
		init(other._capacity);
		_size = other._size;
		copy(other.data);
	}
	return *this;
}

intmap::const_iterator::const_iterator(const bucket* ptr_in, const bucket* end_in): 
	ptr(ptr_in), end(end_in) {
	//
}

std::pair<uint64_t, uint64_t> intmap::const_iterator::operator*() const {
	return { ptr->key, ptr->value };
}

intmap::const_iterator& intmap::const_iterator::operator++() {
	if (ptr != end) ++ ptr;
	while (ptr != end && ptr->status != FILLED) ++ ptr;
	return *this;
}

intmap::const_iterator intmap::const_iterator::operator++(int) {
	const_iterator it = *this;
	operator++();
	return it;
}

bool intmap::const_iterator::operator==(const const_iterator& other) const {
	return ptr == other.ptr;
}

bool intmap::const_iterator::operator!=(const const_iterator& other) const {
	return ptr != other.ptr;
}

intmap::const_iterator intmap::begin() const {
	const bucket *start = data, *end = data + _capacity;
	while (start != end && start->status != FILLED) ++ start;
	return const_iterator(start, end);
}

intmap::const_iterator intmap::end() const {
	return const_iterator(data + _capacity, data + _capacity);
}

void intmap::emplace(uint64_t k, uint64_t v) {
	if (_size + 1 > _capacity * 5 / 8) grow();
	uint64_t h = hash(k);
	uint64_t dist = 0;
	uint64_t i = h & _mask;
	uint64_t key = k, value = v;
	while (true) {
		if (data[i].status == EMPTY || data[i].status == GHOST) {
			data[i].fill(key, value);
			++ _size;
			return;
		}

		if (data[i].status == FILLED && data[i].key == key) {
			data[i].value = value;
			return;
		}

		uint64_t other_dist = (i - (hash(data[i].key) & _mask)) & _mask;
		if (other_dist < dist) {
			if (data[i].status == GHOST) {
				data[i].fill(key, value);
				++ _size;
				return;
			}

			uint64_t tk = data[i].key, tv = data[i].value;
			data[i].key = key, data[i].value = value;
			key = tk, value = tv;
			dist = other_dist;
		}
		i = (i + 1) & _mask;
		++ dist;
	}
}

void intmap::erase(uint64_t k) {
	uint64_t h = hash(k);
	uint64_t i = h & _mask;
	while (true) {
		if (data[i].status == EMPTY) return;
		if (data[i].status == FILLED && data[i].key == k) {
			data[i].evict();
			-- _size;
			return;
		}
		i = (i + 1) & _mask;
	}
}

intmap::const_iterator intmap::find(uint64_t k) const {
	uint64_t h = hash(k);
	uint64_t i = h & _mask;
	while (true) {
		if (data[i].status == EMPTY) return end();
		uint64_t dist = (i - h) & _mask;
		uint64_t oh = hash(data[i].key);
		uint64_t other_dist = (i - (oh & _mask)) & _mask;
		if (other_dist < dist) return end();
		if (data[i].status == FILLED && data[i].key == k) {
			return const_iterator(data + i, data + _capacity);
		}
		i = (i + 1) & _mask;
	}
}

uint32_t intmap::size() const {
	return _size;
}

uint32_t intmap::capacity() const {
	return _capacity;
}