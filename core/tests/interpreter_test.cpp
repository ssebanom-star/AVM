// 참조 인터프리터 단위 테스트.
// 각 테스트는 미니 어셈블러로 게스트 프로그램을 조립해 RAM에 적재하고
// 실행 후 아키텍처 상태를 검증한다.
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/interp/interpreter.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;

namespace {

struct Vm {
    PhysMem mem;
    CpuState cpu;

    explicit Vm(const std::vector<u32>& program, u64 ram_size = 1 << 20) {
        mem.AddRam(board::kRamBase, ram_size, "ram");
        mem.LoadImage(board::kRamBase, program.data(), program.size() * 4);
        cpu.Reset(board::kRamBase);
        cpu.SetCurrentSp(board::kRamBase + ram_size);
    }
    StopInfo Run(u64 max = 10000) {
        Interpreter interp(cpu, mem);
        return interp.Run(max);
    }
};

} // namespace

TEST(Interp_MovzMovkMovn) {
    Vm vm({
        Movz(0, 0xBEEF, 0),
        Movk(0, 0xDEAD, 3),
        Movn(1, 0),               // x1 = ~0 = 0xFFFF...FFFF
        Movn(2, 0, 0, false),     // w2 = 0xFFFFFFFF (상위 32비트 0)
        Movz(3, 0xFFFF, 0),
        Movk(3, 0x1, 0, false),   // w형 movk: 상위 32비트 클리어
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], 0xDEAD00000000BEEFull);
    CHECK_EQ(vm.cpu.x[1], ~0ull);
    CHECK_EQ(vm.cpu.x[2], 0xFFFFFFFFull);
    CHECK_EQ(vm.cpu.x[3], 0x1ull);
}

TEST(Interp_AddSubFlags64) {
    // x0 = INT64_MAX, x1 = 1, adds x2, x0, x1 -> V=1, N=1
    Vm vm({
        Movn(0, 0x8000, 3),       // x0 = ~(0x8000<<48) = 0x7FFF FFFF FFFF FFFF
        Movz(1, 1),
        AddsReg(2, 0, 1),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], 0x7FFFFFFFFFFFFFFFull);
    CHECK_EQ(vm.cpu.x[2], 0x8000000000000000ull);
    CHECK(vm.cpu.pstate.v);
    CHECK(vm.cpu.pstate.n);
    CHECK(!vm.cpu.pstate.z);
    CHECK(!vm.cpu.pstate.c);
}

TEST(Interp_SubsFlagsEqual) {
    // 같은 값 비교: Z=1, C=1 (borrow 없음)
    Vm vm({
        Movz(0, 42),
        Movz(1, 42),
        SubsReg(2, 0, 1),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2], 0ull);
    CHECK(vm.cpu.pstate.z);
    CHECK(vm.cpu.pstate.c);
    CHECK(!vm.cpu.pstate.n);
    CHECK(!vm.cpu.pstate.v);
}

TEST(Interp_AddsCarry32) {
    // w형: 0xFFFFFFFF + 1 -> 결과 0, C=1, Z=1, 상위 32비트는 0.
    Vm vm({
        Movn(0, 0, 0, false),        // w0 = 0xFFFFFFFF
        Movz(1, 1),
        AddSubReg(false, true, 2, 0, 1, 0, 0, /*is64=*/false), // adds w2, w0, w1
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2], 0ull);
    CHECK(vm.cpu.pstate.c);
    CHECK(vm.cpu.pstate.z);
    CHECK(!vm.cpu.pstate.v);
}

TEST(Interp_ShiftedOperands) {
    // x2 = x0 + (x1 << 4); x3 = x0 - (x1 >> 1); x4 = x1(ASR, 음수)
    Vm vm({
        Movz(0, 100),
        Movz(1, 8),
        AddSubReg(false, false, 2, 0, 1, 0 /*LSL*/, 4, true),
        AddSubReg(true, false, 3, 0, 1, 1 /*LSR*/, 1, true),
        Movn(5, 0),                                            // x5 = -1
        AddSubReg(false, false, 4, 31, 5, 2 /*ASR*/, 8, true), // x4 = 0 + (-1>>8 ASR)
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2], 100 + (8ull << 4));
    CHECK_EQ(vm.cpu.x[3], 100 - 4ull);
    CHECK_EQ(vm.cpu.x[4], ~0ull); // ASR로 부호 유지
}

TEST(Interp_LogicalOps) {
    Vm vm({
        Movz(0, 0xF0F0, 0),
        Movz(1, 0x0FF0, 0),
        AndReg(2, 0, 1),
        OrrReg(3, 0, 1),
        EorReg(4, 0, 1),
        LogicalReg(0b01, 5, 31, 0, /*invert=*/true, 0, 0, true), // orn x5, xzr, x0
        LogicalImm(0b00, 6, 0, 1, 0, 7, true),                   // and x6, x0, #0xff
        LogicalReg(0b11, 7, 0, 1, false, 0, 0, true),            // ands x7, x0, x1
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2], 0x00F0ull);
    CHECK_EQ(vm.cpu.x[3], 0xFFF0ull);
    CHECK_EQ(vm.cpu.x[4], 0xFF00ull);
    CHECK_EQ(vm.cpu.x[5], ~0xF0F0ull);
    CHECK_EQ(vm.cpu.x[6], 0xF0ull);
    CHECK_EQ(vm.cpu.x[7], 0x00F0ull);
    CHECK(!vm.cpu.pstate.n);
    CHECK(!vm.cpu.pstate.z); // 0xF0 != 0
}

TEST(Interp_ZeroRegisterSemantics) {
    Vm vm({
        Movz(0, 7),
        MovReg(31, 0),            // orr xzr, xzr, x0 -> 무시되어야 함
        AddReg(1, 31, 31),        // x1 = xzr + xzr = 0
        Movz(31, 0x1234, 0),      // movz xzr -> 무시
        AddImm(2, 31, 4),         // ★ add x2, sp, #4 (rn=31은 SP!)
        Svc(0),
    });
    const u64 sp = board::kRamBase + (1 << 20);
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[1], 0ull);
    CHECK_EQ(vm.cpu.x[2], sp + 4);
    CHECK_EQ(vm.cpu.CurrentSp(), sp);
}

TEST(Interp_LoadStoreSizes) {
    const u64 buf = board::kRamBase + 0x2000;
    Vm vm({
        Movz(0, static_cast<u16>(buf & 0xFFFF), 0),
        Movk(0, static_cast<u16>((buf >> 16) & 0xFFFF), 1),
        Movk(0, static_cast<u16>((buf >> 32) & 0xFFFF), 2),
        Movn(1, 0),               // x1 = 전부 1
        StrX(1, 0, 0),
        StrW(1, 0, 8),
        StrB(1, 0, 12),
        LdrX(2, 0, 0),
        LdrW(3, 0, 8),
        LdrB(4, 0, 12),
        LdrB(5, 0, 13),           // 인접 바이트는 0이어야 함
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2], ~0ull);
    CHECK_EQ(vm.cpu.x[3], 0xFFFFFFFFull); // zero-extend
    CHECK_EQ(vm.cpu.x[4], 0xFFull);
    CHECK_EQ(vm.cpu.x[5], 0ull);
}

TEST(Interp_StackPushPop) {
    // str x0, [sp, #-16]! ; ldr x1, [sp], #16 — 스택 왕복 후 SP 복원.
    Vm vm({
        Movz(0, 0x5A5A, 0),
        StrXPre(0, kSp, -16),
        LdrXPost(1, kSp, 16),
        Svc(0),
    });
    const u64 sp0 = board::kRamBase + (1 << 20);
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[1], 0x5A5Aull);
    CHECK_EQ(vm.cpu.CurrentSp(), sp0);
}

TEST(Interp_BranchLinkAndReturn) {
    //  0: bl +12  (-> 12)
    //  4: movz x1, #2
    //  8: svc
    // 12: movz x0, #1
    // 16: ret
    Vm vm({
        Bl(12),
        Movz(1, 2),
        Svc(0),
        Movz(0, 1),
        Ret(),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], 1ull);
    CHECK_EQ(vm.cpu.x[1], 2ull);
    CHECK_EQ(vm.cpu.x[30], board::kRamBase + 4);
}

TEST(Interp_ConditionalBranches) {
    // 조건별로 마커 레지스터를 채우는 프로그램.
    Vm vm({
        /* 0*/ Movz(0, 1),
        /* 4*/ Movz(1, 2),
        /* 8*/ CmpReg(0, 1),          // 1-2: N=1, C=0
        /*12*/ BCond(11 /*LT*/, 8),   // taken -> 20
        /*16*/ Movz(2, 0xBAD, 0),
        /*20*/ BCond(3 /*CC*/, 8),    // C=0 -> taken -> 28
        /*24*/ Movz(3, 0xBAD, 0),
        /*28*/ BCond(0 /*EQ*/, 8),    // Z=0 -> not taken
        /*32*/ Movz(4, 0x600D, 0),
        /*36*/ Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2], 0ull);
    CHECK_EQ(vm.cpu.x[3], 0ull);
    CHECK_EQ(vm.cpu.x[4], 0x600Dull);
}

TEST(Interp_CbzCbnzLoop) {
    // do { x0 += x1; x1 -= 1 } while (x1 != 0)  — cbnz 역방향 분기.
    Vm vm({
        /* 0*/ Movz(0, 0),
        /* 4*/ Movz(1, 100),
        /* 8*/ AddReg(0, 0, 1),
        /*12*/ SubImm(1, 1, 1),
        /*16*/ Cbnz(1, -8),
        /*20*/ Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], 5050ull);
}

TEST(Interp_W32ResultsZeroExtend) {
    // 32비트 연산 결과는 상위 32비트가 0이어야 한다.
    Vm vm({
        Movn(0, 0),                                              // x0 = ~0
        AddSubImm(false, false, 1, 0, 1, false, /*is64=*/false), // add w1, w0, #1
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[1], 0ull); // 0xFFFFFFFF+1 = 0 (32비트), 상위도 0
}

TEST(Interp_UndefinedInstructionStops) {
    Vm vm({Nop(), 0x9B037C41u /* madd: 미구현 */, Svc(0)});
    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kUndefinedInst);
    CHECK_EQ(stop.pc, board::kRamBase + 4);
    CHECK_EQ(stop.raw_inst, 0x9B037C41u);
    // PC는 미정의 명령어를 가리킨 채 정지 (진행 금지).
    CHECK_EQ(vm.cpu.pc, board::kRamBase + 4);
}

TEST(Interp_DataAbortReportsAddress) {
    Vm vm({
        Movz(0, 0x2000, 0),   // 미매핑 주소
        StrX(1, 0, 8),
        Svc(0),
    });
    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kDataAbort);
    CHECK(stop.fault.kind == MemFaultKind::kUnmapped);
    CHECK_EQ(stop.fault.addr, 0x2008ull);
    CHECK(stop.fault.is_write);
}

TEST(Interp_FetchFromUnmappedStops) {
    Vm vm({
        Movz(0, 0x100, 0),
        Br(0),                // PC = 0x100 (미매핑)
    });
    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kInstAbort);
    CHECK_EQ(stop.pc, 0x100ull);
    CHECK(stop.fault.is_fetch);
}

TEST(Interp_PcAlignment) {
    Vm vm({
        Movz(0, 0x2, 0),      // 정렬되지 않은 대상
        Br(0),
    });
    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kPcMisaligned);
    CHECK_EQ(stop.pc, 2ull);
}

TEST(Interp_SvcReportsImmediate) {
    Vm vm({Svc(0xABCD)});
    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kSvc);
    CHECK_EQ(stop.svc_imm, 0xABCD);
    // ELR 의미: PC는 SVC 다음 명령을 가리킨다.
    CHECK_EQ(vm.cpu.pc, board::kRamBase + 4);
}

TEST(Interp_MaxInstructionsStops) {
    // 무한 루프: b .
    Vm vm({B(0)});
    const StopInfo stop = vm.Run(1000);
    CHECK(stop.reason == StopReason::kMaxInstructions);
    CHECK_EQ(vm.cpu.executed_instructions, 1000ull);
}
