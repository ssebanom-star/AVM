#include "avm/jit/code_cache.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "avm/log.h"

namespace avm::jit {

namespace {
constexpr const char* kTag = "avm.jit.cache";

size_t PageSize() {
    static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}
size_t PageAlignDown(size_t v) { return v & ~(PageSize() - 1); }
size_t PageAlignUp(size_t v) { return (v + PageSize() - 1) & ~(PageSize() - 1); }
} // namespace

CodeCache::CodeCache(size_t capacity_bytes) {
    capacity_ = PageAlignUp(capacity_bytes);
    void* p = mmap(nullptr, capacity_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        AVM_LOGE(kTag, "mmap 실패: capacity=%zu", capacity_);
        base_ = nullptr;
        return;
    }
    base_ = static_cast<u8*>(p);
    executable_ = false;
}

CodeCache::~CodeCache() {
    if (base_ != nullptr) munmap(base_, capacity_);
}

bool CodeCache::Contains(const void* host_addr) const {
    const u8* p = static_cast<const u8*>(host_addr);
    return base_ != nullptr && p >= base_ && p < base_ + capacity_;
}

void CodeCache::MakeWritable(void* addr, size_t len) {
    const size_t start = PageAlignDown(static_cast<u8*>(addr) - base_);
    const size_t end = PageAlignUp(static_cast<u8*>(addr) - base_ + len);
    if (mprotect(base_ + start, end - start, PROT_READ | PROT_WRITE) != 0) {
        AVM_LOGE(kTag, "mprotect(RW) 실패");
    }
}

void CodeCache::MakeExecutable(void* addr, size_t len) {
    const size_t start = PageAlignDown(static_cast<u8*>(addr) - base_);
    const size_t end = PageAlignUp(static_cast<u8*>(addr) - base_ + len);
    if (mprotect(base_ + start, end - start, PROT_READ | PROT_EXEC) != 0) {
        AVM_LOGE(kTag, "mprotect(RX) 실패");
    }
}

void CodeCache::FlushICache(void* addr, size_t len) {
    u8* start = static_cast<u8*>(addr);
    __builtin___clear_cache(reinterpret_cast<char*>(start),
                            reinterpret_cast<char*>(start + len));
}

void* CodeCache::Emit(const std::vector<u32>& code) {
    if (base_ == nullptr) return nullptr;
    const size_t bytes = code.size() * 4;
    if (used_ + bytes > capacity_) {
        AVM_LOGW(kTag, "코드 캐시 가득 참: used=%zu need=%zu cap=%zu", used_,
                 bytes, capacity_);
        return nullptr;
    }
    u8* dst = base_ + used_;
    // 방출 대상 페이지를 쓰기 가능으로. 아레나가 실행 상태였다면 되돌린다.
    MakeWritable(dst, bytes);
    std::memcpy(dst, code.data(), bytes);
    MakeExecutable(dst, bytes);
    FlushICache(dst, bytes);
    used_ += bytes;
    // 다음 블록이 같은 페이지 끝에 이어질 수 있으므로 정렬은 워드 단위 유지.
    return dst;
}

void CodeCache::PatchWord(void* host_addr, u32 new_word) {
    if (!Contains(host_addr)) {
        AVM_LOGE(kTag, "패치 대상이 아레나 밖: %p", host_addr);
        return;
    }
    MakeWritable(host_addr, 4);
    std::memcpy(host_addr, &new_word, 4);
    MakeExecutable(host_addr, 4);
    FlushICache(host_addr, 4);
}

void CodeCache::Reset() {
    used_ = 0;
    // 아레나를 다시 쓰기 가능으로 되돌려 다음 Emit가 곧바로 기록하게 한다.
    if (base_ != nullptr) {
        mprotect(base_, capacity_, PROT_READ | PROT_WRITE);
        executable_ = false;
    }
}

} // namespace avm::jit
