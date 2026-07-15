// JIT 실행 코드 캐시.
//
// 실행 가능한 호스트 메모리 아레나를 mmap으로 확보하고, 번역 블록을
// 범프 할당한다. Android의 W^X 정책에 대응해 "쓰기 상태 -> 실행 상태"
// 전환을 명시적으로 수행하고, 코드 방출/패치 후 명령어 캐시를 동기화한다.
//
// 현재 전략: 페이지 단위 mprotect 토글(RW <-> RX) + __builtin___clear_cache.
// 프로덕션(Android)에서는 동일 물리 페이지를 RW/RX 두 가상 주소에 매핑하는
// 이중 매핑으로 토글 비용을 없앨 수 있다(후속 최적화). 아레나가 모두
// 아직 실행되지 않았다면 토글은 아레나 전체 1회로 끝난다.
#pragma once

#include <cstddef>
#include <vector>

#include "avm/types.h"

namespace avm::jit {

class CodeCache {
public:
    explicit CodeCache(size_t capacity_bytes = 16 * 1024 * 1024);
    ~CodeCache();

    CodeCache(const CodeCache&) = delete;
    CodeCache& operator=(const CodeCache&) = delete;

    bool ok() const { return base_ != nullptr; }
    size_t Capacity() const { return capacity_; }
    size_t Used() const { return used_; }

    // 코드 워드 배열을 아레나에 방출하고 실행 가능한 호스트 주소를 반환한다.
    // 용량 부족이면 nullptr (호출자는 캐시를 비우고 재시도).
    // 반환 포인터는 32비트 정렬된 함수 진입점으로 사용 가능하다.
    void* Emit(const std::vector<u32>& code);

    // 이미 실행 가능한 코드 영역의 워드 하나를 교체(블록 연결/분기 패치).
    // 해당 페이지를 잠깐 쓰기 가능으로 바꾸고 기록한 뒤 다시 실행 가능으로
    // 되돌리며 I-cache를 동기화한다.
    void PatchWord(void* host_addr, u32 new_word);

    // 코드 캐시 제거: 모든 블록 폐기(아레나를 처음부터 재사용).
    // 호출자는 이 전에 모든 블록 포인터/연결을 무효화해야 한다.
    void Reset();

    // host_addr가 이 아레나 범위 안인지.
    bool Contains(const void* host_addr) const;

private:
    void MakeWritable(void* addr, size_t len);
    void MakeExecutable(void* addr, size_t len);
    void FlushICache(void* addr, size_t len);

    u8* base_ = nullptr;
    size_t capacity_ = 0;
    size_t used_ = 0;
    // 아레나가 현재 실행 가능(RX) 상태로 보호되어 있는지.
    bool executable_ = false;
};

} // namespace avm::jit
