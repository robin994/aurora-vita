#include "gfx/vita_byte_compare.h"
#include "gfx/vita_byte_compare.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
unsigned checks = 0;
void check(bool result) {
    ++checks;
    if (!result) { std::fprintf(stderr, "byte equality failed at check %u\n", checks); std::exit(1); }
}
void exercise(unsigned char* a, unsigned char* b, size_t length) {
    for (size_t i = 0; i < length; ++i) a[i] = b[i] = static_cast<unsigned char>(i * 73u + 19u);
    check(aurora_vita_bytes_equal(a, b, length));
    check(aurora_vita_bytes_equal(a, a, length));
    for (size_t i = 0; i < length; ++i) {
        b[i] ^= 0x80;
        check(!aurora_vita_bytes_equal(a, b, length));
        check(!aurora_vita_bytes_equal(b, a, length));
        b[i] ^= 0x80;
    }
}
}
int main() {
    check(aurora_vita_bytes_equal(nullptr, nullptr, 0));
    std::array<unsigned char, 576> a{}, b{};
    for(size_t i=0;i<a.size();++i)a[i]=b[i]=static_cast<unsigned char>(i*29u+7u);
    const auto baseHash=aurora::vita::gfx::byte_span_hash(a.data(),a.size());
    check(baseHash==aurora::vita::gfx::byte_span_hash(b.data(),b.size()));
    b[317]^=0x40;
    check(baseHash!=aurora::vita::gfx::byte_span_hash(b.data(),b.size()));
    b[317]^=0x40;
    for (size_t length = 0; length <= 257; ++length)
        for (unsigned ao = 0; ao < 16; ++ao)
            for (unsigned bo = 0; bo < 16; ++bo) exercise(a.data() + ao, b.data() + bo, length);
#if defined(__unix__) || defined(__APPLE__)
    const long pageSize = sysconf(_SC_PAGESIZE);
    check(pageSize >= 512);
    void* ma = mmap(nullptr, size_t(pageSize) * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void* mb = mmap(nullptr, size_t(pageSize) * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(ma != MAP_FAILED && mb != MAP_FAILED);
    auto* ea = static_cast<unsigned char*>(ma) + pageSize;
    auto* eb = static_cast<unsigned char*>(mb) + pageSize;
    check(mprotect(ea, pageSize, PROT_NONE) == 0 && mprotect(eb, pageSize, PROT_NONE) == 0);
    // End every possible short tail immediately before inaccessible memory.
    for (size_t length = 0; length <= 511; ++length) exercise(ea - length, eb - length, length);
    check(munmap(ma, size_t(pageSize) * 2) == 0 && munmap(mb, size_t(pageSize) * 2) == 0);
#endif
    std::printf("PASS byte equality: %u checks, every mismatch position, unaligned buffers and guarded tails\n", checks);
}
