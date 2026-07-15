// JIT 번역기 구조 테스트 (호스트 아키텍처 무관).
// 블록 경계, 명령 수, 링크 수를 검증한다. 실제 실행은 jit_exec_test.cpp.
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/jit/jit_engine.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;
using namespace avm::jit;

namespace {
struct TransVm {
    PhysMem mem;
    CpuState cpu;
    std::unique_ptr<JitEngine> jit;

    explicit TransVm(const std::vector<u32>& program) {
        mem.AddRam(board::kRamBase, 1 << 20, "ram");
        mem.LoadImage(board::kRamBase, program.data(), program.size() * 4);
        cpu.Reset(board::kRamBase);
        jit = std::make_unique<JitEngine>(cpu, mem);
    }
};
} // namespace

TEST(JitTranslate_StraightLineEndsAtBranch) {
    // 3개 산술 + b -> 4개 명령, 링크 1개(직접 분기).
    TransVm vm({
        Movz(0, 1), AddImm(0, 0, 1), AddImm(0, 0, 1), B(-12),
    });
    auto s = vm.jit->TranslateForTest(board::kRamBase);
    CHECK(s.translated);
    CHECK_EQ(s.inst_count, 4ull);
    CHECK_EQ(s.link_count, 1u);
    CHECK(s.has_host_code);
}

TEST(JitTranslate_ConditionalHasTwoLinks) {
    TransVm vm({Movz(0, 5), CmpImm(0, 5), BCond(0, 8), Movz(1, 1), Svc(0)});
    auto s = vm.jit->TranslateForTest(board::kRamBase);
    CHECK(s.translated);
    // movz, cmp(subs), b.cond -> 3개 명령, b.cond가 양쪽 링크 2개.
    CHECK_EQ(s.inst_count, 3ull);
    CHECK_EQ(s.link_count, 2u);
}

TEST(JitTranslate_IndirectHasNoLinks) {
    TransVm vm({Movz(9, 0x100, 0), Br(9)});
    auto s = vm.jit->TranslateForTest(board::kRamBase);
    CHECK(s.translated);
    CHECK_EQ(s.inst_count, 2ull);
    CHECK_EQ(s.link_count, 0u); // 간접 분기는 링크 없음
}

TEST(JitTranslate_NonJittableFirstReturnsNothing) {
    // 첫 명령이 SVC(비-JIT)면 번역하지 않는다 (인터프리터가 처리).
    TransVm vm({Svc(0)});
    auto s = vm.jit->TranslateForTest(board::kRamBase);
    CHECK(!s.translated);
}

TEST(JitTranslate_TruncatesBeforeNonJittable) {
    // 산술 2개 후 MRS(비-JIT): 블록은 2개까지, kInterpret 종료.
    TransVm vm({Movz(0, 1), AddImm(0, 0, 1), Mrs(1, sysreg::kMidrEl1), Svc(0)});
    auto s = vm.jit->TranslateForTest(board::kRamBase);
    CHECK(s.translated);
    CHECK_EQ(s.inst_count, 2ull);
    CHECK_EQ(s.link_count, 0u);
}

TEST(JitTranslate_LoadStoreBlock) {
    TransVm vm({
        Movz(0, 0x100, 0),
        StrX(1, 0, 0),
        LdrX(2, 0, 0),
        Ret(),
    });
    auto s = vm.jit->TranslateForTest(board::kRamBase);
    CHECK(s.translated);
    CHECK_EQ(s.inst_count, 4ull);
}
