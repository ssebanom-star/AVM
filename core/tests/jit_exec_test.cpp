// JIT 실행 + 검증 테스트.
//
// 실제 호스트 ARM64 코드 실행이 필요하므로 ARM64 호스트에서만 검증한다
// (개발 머신에서는 aarch64 크로스 빌드를 qemu-user로 실행). 그 외 호스트에서
// 는 컴파일만 되고 실행 어서션은 건너뛴다.
//
// 각 프로그램은 (1) JIT로 실행하고 (2) 동일 프로그램을 참조 인터프리터로
// 실행해 최종 상태를 대조한다. 추가로 SetValidate(true)로 블록 단위 자동
// 대조도 활성화한다.
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/interp/interpreter.h"
#include "avm/jit/jit_engine.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;
using namespace avm::jit;

#if defined(__aarch64__)

namespace {

constexpr u64 kRamSize = 2 * 1024 * 1024;

// JIT로 프로그램 실행 (검증 모드 on).
CpuState RunJit(const std::vector<u32>& prog, u64 sp_top, u64 max = 100000,
                JitEngine::Stats* out_stats = nullptr) {
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, kRamSize, "ram");
    mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);
    cpu.Reset(board::kRamBase);
    cpu.SetCurrentSp(sp_top);
    JitEngine jit(cpu, mem);
    jit.SetValidate(true);
    jit.Run(max);
    CHECK(!jit.ValidationFailed());
    if (out_stats) *out_stats = jit.GetStats();
    return cpu;
}

// JIT로 프로그램 실행 (체이닝 활성, 검증 off — 체인 실행 정확성 확인용).
CpuState RunJitPlain(const std::vector<u32>& prog, u64 sp_top, u64 max = 100000,
                     JitEngine::Stats* out_stats = nullptr) {
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, kRamSize, "ram");
    mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);
    cpu.Reset(board::kRamBase);
    cpu.SetCurrentSp(sp_top);
    JitEngine jit(cpu, mem);
    jit.Run(max);
    if (out_stats) *out_stats = jit.GetStats();
    return cpu;
}

// 참조 인터프리터로 프로그램 실행.
CpuState RunInterp(const std::vector<u32>& prog, u64 sp_top, u64 max = 100000) {
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, kRamSize, "ram");
    mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);
    cpu.Reset(board::kRamBase);
    cpu.SetCurrentSp(sp_top);
    Interpreter interp(cpu, mem);
    interp.Run(max);
    return cpu;
}

// JIT와 인터프리터 결과 레지스터/플래그 일치 확인.
void CompareStates(const CpuState& j, const CpuState& i) {
    for (unsigned r = 0; r < 31; ++r) CHECK_EQ(j.x[r], i.x[r]);
    CHECK_EQ(j.pc, i.pc);
    CHECK_EQ(j.CurrentSp(), i.CurrentSp());
    CHECK_EQ((u64)j.pstate.ToNzcvBits(), (u64)i.pstate.ToNzcvBits());
}

// 검증 모드 JIT(블록 단위 대조)와 plain JIT(체인 실행) 둘 다 인터프리터와 일치.
void CheckMatches(const std::vector<u32>& prog, u64 sp_top = board::kRamBase + kRamSize) {
    CpuState ref = RunInterp(prog, sp_top);
    CompareStates(RunJit(prog, sp_top), ref);       // 검증 모드(체이닝 off)
    CompareStates(RunJitPlain(prog, sp_top), ref);  // 체이닝 on
}

} // namespace

TEST(JitExec_Arithmetic) {
    CheckMatches({
        Movz(0, 0xDEF0, 0), Movk(0, 0x9ABC, 1),
        Movk(0, 0x5678, 2), Movk(0, 0x1234, 3),
        AddImm(1, 0, 0x123),
        SubReg(2, 1, 0),
        AddReg(3, 0, 1),
        Svc(0),
    });
}

TEST(JitExec_MovVariants) {
    CheckMatches({
        Movz(0, 0xBEEF, 0), Movk(0, 0xDEAD, 3),
        Movn(1, 0),
        Movn(2, 0, 0, false),
        Movz(3, 0xFFFF), Movk(3, 1, 0, false),
        Svc(0),
    });
}

TEST(JitExec_Logical) {
    CheckMatches({
        Movz(0, 0xF0F0, 0), Movz(1, 0x0FF0, 0),
        AndReg(2, 0, 1), OrrReg(3, 0, 1), EorReg(4, 0, 1),
        LogicalReg(0b01, 5, 31, 0, true, 0, 0, true), // orn
        LogicalImm(0b00, 6, 0, 1, 0, 7, true),        // and x6,x0,#0xff
        Svc(0),
    });
}

TEST(JitExec_ShiftedOperands) {
    CheckMatches({
        Movz(0, 100), Movz(1, 8),
        AddSubReg(false, false, 2, 0, 1, 0, 4, true),  // add x2,x0,x1,lsl#4
        AddSubReg(true, false, 3, 0, 1, 1, 1, true),   // sub x3,x0,x1,lsr#1
        Svc(0),
    });
}

TEST(JitExec_FlagsAddSub) {
    CheckMatches({
        Movn(0, 0x8000, 3),  // INT64_MAX
        Movz(1, 1),
        AddsReg(2, 0, 1),    // 오버플로 -> V=1,N=1
        Movz(4, 42), Movz(5, 42),
        SubsReg(6, 4, 5),    // Z=1,C=1
        Svc(0),
    });
}

TEST(JitExec_Flags32Bit) {
    CheckMatches({
        Movn(0, 0, 0, false),  // w0 = 0xFFFFFFFF
        Movz(1, 1),
        AddSubReg(false, true, 2, 0, 1, 0, 0, false), // adds w2,w0,w1 -> C=1,Z=1
        Svc(0),
    });
}

TEST(JitExec_ZeroAndSpForms) {
    // XZR 의미 + SP 형식 (add x2, sp, #4).
    const u64 sp = board::kRamBase + kRamSize;
    CheckMatches({
        Movz(0, 7),
        AddReg(1, 31, 31),   // x1 = xzr + xzr = 0
        AddImm(2, 31, 4),    // x2 = sp + 4
        SubImm(31, 31, 16),  // sp -= 16
        AddImm(3, 31, 0),    // x3 = sp
        Svc(0),
    }, sp);
}

TEST(JitExec_LoadStore) {
    CheckMatches({
        Movz(0, 0x8000, 0),          // 데이터 주소 (RAM 내)
        Movn(1, 0),
        StrX(1, 0, 0),
        StrW(1, 0, 8),
        StrB(1, 0, 12),
        LdrX(2, 0, 0),
        LdrW(3, 0, 8),
        LdrB(4, 0, 12),
        LdrB(5, 0, 13),
        Svc(0),
    });
}

TEST(JitExec_StackPrePost) {
    const u64 sp = board::kRamBase + kRamSize;
    CheckMatches({
        Movz(0, 0x5A5A, 0),
        StrXPre(0, kSp, -16),
        LdrXPost(1, kSp, 16),
        Svc(0),
    }, sp);
}

TEST(JitExec_BranchLinkLoopSum) {
    // 1..10 합 = 55 (직접 분기 루프 + BL/RET 서브루틴).
    CheckMatches({
        /* 0*/ Movz(0, 0),
        /* 4*/ Movz(1, 10),
        /* 8*/ Cbz(1, 16),
        /*12*/ Bl(16),
        /*16*/ SubImm(1, 1, 1),
        /*20*/ B(-12),
        /*24*/ Svc(0),
        /*28*/ AddReg(0, 0, 1),
        /*32*/ Ret(),
    });
}

TEST(JitExec_ConditionalBranches) {
    CheckMatches({
        /* 0*/ Movz(0, 1),
        /* 4*/ Movz(1, 2),
        /* 8*/ CmpReg(0, 1),
        /*12*/ BCond(11 /*LT*/, 8),
        /*16*/ Movz(2, 0xBAD, 0),
        /*20*/ BCond(3 /*CC*/, 8),
        /*24*/ Movz(3, 0xBAD, 0),
        /*28*/ BCond(0 /*EQ*/, 8),
        /*32*/ Movz(4, 0x600D, 0),
        /*36*/ Svc(0),
    });
}

TEST(JitExec_CbnzCountdown) {
    CheckMatches({
        /* 0*/ Movz(0, 0),
        /* 4*/ Movz(1, 100),
        /* 8*/ AddReg(0, 0, 1),
        /*12*/ SubImm(1, 1, 1),
        /*16*/ Cbnz(1, -8),
        /*20*/ Svc(0),
    });
}

TEST(JitExec_IndirectBranch) {
    const u64 target = board::kRamBase + 0x80;
    std::vector<u32> prog = {
        Movz(9, static_cast<u16>(target & 0xFFFF), 0),
        Movk(9, static_cast<u16>((target >> 16) & 0xFFFF), 1),
        Movk(9, static_cast<u16>((target >> 32) & 0xFFFF), 2),
        Br(9),
    };
    prog.resize(0x80 / 4);
    prog.push_back(Movz(7, 0x77, 0));
    prog.push_back(Svc(0));
    CheckMatches(prog);
}

// SVC/MRS 등 비-JIT 명령이 인터프리터 폴백으로 정확히 처리되는지.
TEST(JitExec_InterpreterFallbackForSysreg) {
    CheckMatches({
        Movz(0, 0x1234, 0),
        Msr(sysreg::kTpidrEl0, 0),
        Mrs(1, sysreg::kTpidrEl0),
        AddImm(1, 1, 1),
        Svc(0),
    });
}

// 블록 연결(체이닝)이 실제로 일어나는지: 루프가 반복되면 체인 패치 발생.
TEST(JitExec_ChainingHappens) {
    JitEngine::Stats stats;
    CpuState j = RunJitPlain({
        Movz(0, 0), Movz(1, 1000),
        AddImm(0, 0, 1), SubImm(1, 1, 1), Cbnz(1, -8),
        Svc(0),
    }, board::kRamBase + kRamSize, 100000, &stats);
    CHECK_EQ(j.x[0], 1000ull);              // 루프가 정확히 1000회
    CHECK(stats.chain_patches > 0);         // back-edge가 체인됨
    CHECK(stats.blocks_translated < 5ull);  // 블록 몇 개만 번역되고 재사용
}

// 데이터 중단(RO 매핑 없이 미매핑 주소)이 인터프리터와 동일하게 보고되는지.
TEST(JitExec_MemFaultMatchesInterp) {
    std::vector<u32> prog = {
        Movz(0, 0x10, 0),     // 미매핑 (RAM은 0x40000000부터)
        LdrX(1, 0, 0),
        Svc(0),
    };
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, kRamSize, "ram");
    mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);
    cpu.Reset(board::kRamBase);
    JitEngine jit(cpu, mem);
    StopInfo s = jit.Run(1000);
    CHECK(s.reason == StopReason::kDataAbort);
    CHECK_EQ(s.fault.addr, 0x10ull);
    CHECK_EQ(s.pc, board::kRamBase + 4);
}

#else  // !__aarch64__

// 비-ARM64 호스트: JIT 실행은 검증하지 않는다 (번역/인코딩은 별도 테스트).
TEST(JitExec_SkippedOnNonArm64Host) {
    CHECK(true);
}

#endif
