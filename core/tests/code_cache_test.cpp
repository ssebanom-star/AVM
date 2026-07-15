// JIT 코드 캐시 테스트: 실행 메모리 할당, W^X, 패치.
// 실제 실행(호스트 코드 호출)은 ARM64 호스트에서만 (qemu 크로스 실행 포함).
#include "avm/jit/arm64_emitter.h"
#include "avm/jit/code_cache.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::jit;

TEST(CodeCache_AllocAndBounds) {
    CodeCache cache(64 * 1024);
    CHECK(cache.ok());
    Arm64Emitter e;
    e.Movz(0, 42);
    e.Ret();
    void* p = cache.Emit(e.Code());
    CHECK(p != nullptr);
    CHECK(cache.Contains(p));
    CHECK(cache.Used() >= 8u);
    CHECK(!cache.Contains(reinterpret_cast<void*>(0x1234)));
}

TEST(CodeCache_ResetReusesArena) {
    CodeCache cache(64 * 1024);
    Arm64Emitter e;
    e.Ret();
    void* p1 = cache.Emit(e.Code());
    cache.Reset();
    CHECK_EQ(cache.Used(), 0u);
    void* p2 = cache.Emit(e.Code());
    CHECK_EQ(p1, p2); // 아레나 재사용
}

TEST(CodeCache_CapacityExhaustion) {
    CodeCache cache(4096);
    Arm64Emitter e;
    for (int i = 0; i < 2000; ++i) e.Nop(); // 8000바이트 > 4096
    void* p = cache.Emit(e.Code());
    CHECK(p == nullptr);
}

#if defined(__aarch64__)
// 실제 실행: 방출한 함수를 호출한다.
TEST(CodeCache_ExecuteEmittedFunction) {
    CodeCache cache(64 * 1024);
    Arm64Emitter e;
    e.Movz(0, 0xABC); // mov x0, #0xABC
    e.Ret();
    void* p = cache.Emit(e.Code());
    CHECK(p != nullptr);
    using Fn = unsigned long (*)();
    Fn fn = reinterpret_cast<Fn>(p);
    CHECK_EQ(fn(), 0xABCul);
}

TEST(CodeCache_PatchRetargetsBranch) {
    CodeCache cache(64 * 1024);
    Arm64Emitter e;
    //  0: b .+12  -> word3 (mov x0,#2)
    //  4: mov x0,#1 ; 8: ret
    // 12: mov x0,#2 ; 16: ret
    const size_t br = e.B(12);
    e.Movz(0, 1); e.Ret();
    e.Movz(0, 2); e.Ret();
    void* p = cache.Emit(e.Code());
    using Fn = unsigned long (*)();
    CHECK_EQ(reinterpret_cast<Fn>(p)(), 2ul); // 초기: +12로 점프 -> #2

    // 분기를 +4로 재조준 (mov x0,#1 경로).
    u32 new_branch = 0x14000001u; // b .+4
    cache.PatchWord(p, new_branch);
    CHECK_EQ(reinterpret_cast<Fn>(p)(), 1ul);
}
#endif
