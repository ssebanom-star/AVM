// 시스템 레지스터 접근(MRS/MSR) 단위 테스트.
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

    explicit Vm(const std::vector<u32>& program) {
        mem.AddRam(board::kRamBase, 1 << 20, "ram");
        mem.LoadImage(board::kRamBase, program.data(), program.size() * 4);
        cpu.Reset(board::kRamBase);
        cpu.SetCurrentSp(board::kRamBase + (1 << 20));
    }
    StopInfo Run(u64 max = 1000) {
        Interpreter interp(cpu, mem); // 하니스 모드
        return interp.Run(max);
    }
};

} // namespace

TEST(Sysreg_IdentityRegisters) {
    Vm vm({
        Mrs(0, sysreg::kMidrEl1),
        Mrs(1, sysreg::kMpidrEl1),
        Mrs(2, sysreg::kCurrentEl),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], vm.cpu.sys.midr_el1);
    CHECK_EQ(vm.cpu.x[1], vm.cpu.sys.mpidr_el1);
    CHECK_EQ(vm.cpu.x[2], 1ull << 2); // EL1
}

TEST(Sysreg_WriteReadRoundTrip) {
    Vm vm({
        Movz(0, 0xA000, 1),                 // x0 = 0xA000_0000
        Msr(sysreg::kVbarEl1, 0),
        Mrs(1, sysreg::kVbarEl1),
        Movz(2, 0x1234, 0),
        Msr(sysreg::kTpidrEl0, 2),
        Mrs(3, sysreg::kTpidrEl0),
        Msr(sysreg::kSctlrEl1, 2),
        Mrs(4, sysreg::kSctlrEl1),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[1], 0xA0000000ull);
    CHECK_EQ(vm.cpu.x[3], 0x1234ull);
    CHECK_EQ(vm.cpu.x[4], 0x1234ull);
}

TEST(Sysreg_VbarAlignment) {
    // VBAR 하위 11비트는 무시된다 (2KB 정렬).
    Vm vm({
        Movz(0, 0x87FF, 0),
        Movk(0, 0x4000, 1),   // x0 = 0x4000_87FF
        Msr(sysreg::kVbarEl1, 0),
        Mrs(1, sysreg::kVbarEl1),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[1], 0x40008000ull);
}

TEST(Sysreg_NzcvAlias) {
    Vm vm({
        Movz(0, 0x6000, 1),                // x0 = Z|C (bits 30,29)
        Msr(sysreg::kNzcv, 0),
        Mrs(1, sysreg::kNzcv),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK(vm.cpu.pstate.z);
    CHECK(vm.cpu.pstate.c);
    CHECK(!vm.cpu.pstate.n);
    CHECK_EQ(vm.cpu.x[1], 0x60000000ull);
}

TEST(Sysreg_DaifImmediates) {
    Vm vm({
        MsrDaifClr(0xF),                   // 전부 언마스크
        Mrs(0, sysreg::kDaif),
        MsrDaifSet(0b0010),                // I만 마스크
        Mrs(1, sysreg::kDaif),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], 0ull);
    CHECK_EQ(vm.cpu.x[1], 1ull << 7); // I bit
    CHECK(vm.cpu.pstate.i);
    CHECK(!vm.cpu.pstate.f);
}

TEST(Sysreg_SpSelSwitchesStacks) {
    Vm vm({
        Movz(0, 0x100, 0),
        MovReg(1, 0),
        AddImm(31, 0, 0),      // mov sp, x0  (SP_EL1 = 0x100)
        MsrSpsel(0),           // SP_EL0 선택
        Movz(2, 0x200, 0),
        AddImm(31, 2, 0),      // SP_EL0 = 0x200
        Mrs(3, sysreg::kSpSel),
        AddImm(4, 31, 0),      // x4 = 현재 SP (=SP_EL0)
        MsrSpsel(1),
        AddImm(5, 31, 0),      // x5 = 현재 SP (=SP_EL1)
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[3], 0ull);
    CHECK_EQ(vm.cpu.x[4], 0x200ull);
    CHECK_EQ(vm.cpu.x[5], 0x100ull);
    CHECK_EQ(vm.cpu.sp[0], 0x200ull);
    CHECK_EQ(vm.cpu.sp[1], 0x100ull);
}

TEST(Sysreg_TimerCounterAndTval) {
    Vm vm({
        Mrs(0, sysreg::kCntpctEl0),   // 카운터 (실행 명령어 기반)
        Mrs(1, sysreg::kCntpctEl0),
        Movz(2, 100),
        Msr(sysreg::kCntpTvalEl0, 2), // CVAL = 현재 + 100
        Mrs(3, sysreg::kCntpTvalEl0),
        Mrs(4, sysreg::kCntpCtlEl0),  // ENABLE=0 -> ISTATUS=0
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK(vm.cpu.x[1] > vm.cpu.x[0]); // 카운터 단조 증가
    // TVAL 읽기는 남은 틱: 쓰기(3번째 명령)와 읽기(4번째) 사이 1명령 경과.
    CHECK_EQ(vm.cpu.x[3], 99ull);
    CHECK_EQ(vm.cpu.x[4] & 0b100, 0ull); // ISTATUS=0
}

TEST(Sysreg_TimerIstatusFires) {
    Vm vm({
        Movz(0, 1),
        Msr(sysreg::kCntpCtlEl0, 0),  // ENABLE=1
        Movz(1, 2),
        Msr(sysreg::kCntpCvalEl0, 1), // CVAL=2 (이미 지난 시점)
        Mrs(2, sysreg::kCntpCtlEl0),
        Svc(0),
    });
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[2] & 0b101, 0b101ull); // ENABLE|ISTATUS
}

TEST(Sysreg_UnknownRegisterIsUndefined) {
    // 미구현 레지스터(예: ID_AA64PFR0_EL1 = 3,0,0,4,0) 접근은 정지.
    Vm vm({Mrs(0, sysreg::Id(3, 0, 0, 4, 0)), Svc(0)});
    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kUndefinedInst);
    CHECK_EQ(stop.pc, board::kRamBase);
}

TEST(Sysreg_ReadOnlyWriteIsUndefined) {
    Vm vm({Msr(sysreg::kMidrEl1, 0), Svc(0)});
    CHECK(vm.Run().reason == StopReason::kUndefinedInst);
}

TEST(Sysreg_CntfrqReset) {
    // VirtBoard가 아닌 원시 CpuState의 CNTFRQ는 0 (보드가 설정 책임).
    Vm vm({Mrs(0, sysreg::kCntfrqEl0), Svc(0)});
    CHECK(vm.Run().reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[0], 0ull);
}
