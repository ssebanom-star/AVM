// AArch64 예외 진입/복귀(Stage 2) 단위 테스트.
// guest_vectors 모드에서 SVC/미정의/중단이 VBAR_EL1 벡터로 전달되고
// ERET으로 정확히 복귀하는지 검증한다.
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/interp/interpreter.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;

namespace {

// 벡터 모드 VM: 프로그램을 RAM에 적재, VBAR도 RAM 안에 배치할 수 있게
// 오프셋 단위 조립을 지원한다.
struct VectorVm {
    PhysMem mem;
    CpuState cpu;
    Interpreter interp;

    static constexpr u64 kRamSize = 1 << 20;

    VectorVm() : interp(cpu, mem, ExecConfig{.guest_vectors = true}) {
        mem.AddRam(board::kRamBase, kRamSize, "ram");
        cpu.Reset(board::kRamBase);
        cpu.SetCurrentSp(board::kRamBase + kRamSize);
    }

    void Put(u64 offset, const std::vector<u32>& words) {
        mem.LoadImage(board::kRamBase + offset, words.data(), words.size() * 4);
    }

    StopInfo Run(u64 max = 10000) { return interp.Run(max); }
};

constexpr u64 kVbarOff = 0x800; // RAM+0x800: 벡터 테이블 (2KB 정렬)

// x9 = vbar 절대 주소를 만드는 시퀀스.
std::vector<u32> SetVbar() {
    const u64 vbar = board::kRamBase + kVbarOff;
    return {
        Movz(9, static_cast<u16>(vbar & 0xFFFF), 0),
        Movk(9, static_cast<u16>((vbar >> 16) & 0xFFFF), 1),
        Movk(9, static_cast<u16>((vbar >> 32) & 0xFFFF), 2),
        Msr(sysreg::kVbarEl1, 9),
    };
}

} // namespace

TEST(Exc_SvcVectorsToEl1hAndEretReturns) {
    VectorVm vm;
    // 메인 (EL1h): VBAR 설정 -> svc #42 -> (복귀) x1 = 1 -> wfi
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(Svc(42));
    main_code.push_back(Movz(1, 1));
    main_code.push_back(Wfi());
    vm.Put(0, main_code);
    // EL1h 동기 벡터 (+0x200): x2 = 2 -> eret
    vm.Put(kVbarOff + 0x200, {Movz(2, 2), Eret()});

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[1], 1ull); // SVC 다음 명령으로 복귀했음
    CHECK_EQ(vm.cpu.x[2], 2ull); // 핸들러 실행됨
    // ESR: EC=0x15(SVC), IL=1, ISS=42.
    CHECK_EQ(vm.cpu.sys.esr_el1, (0x15ull << 26) | (1ull << 25) | 42);
    // ELR: SVC 다음 명령.
    const u64 svc_pc = board::kRamBase + 4 * 4;
    CHECK_EQ(vm.cpu.sys.elr_el1, svc_pc + 4);
}

TEST(Exc_SpsrRoundTrip) {
    VectorVm vm;
    // NZCV를 설정한 뒤 SVC: 핸들러에서 플래그를 바꿔도 ERET 후 복원되는지.
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(Movz(0, 5));
    main_code.push_back(SubsImm(0, 0, 5));   // Z=1, C=1
    main_code.push_back(Svc(0));
    main_code.push_back(Wfi());
    vm.Put(0, main_code);
    vm.Put(kVbarOff + 0x200, {
        AddsImm(3, 31, 1), // 핸들러에서 플래그 파괴 (Z=0)
        Eret(),
    });

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK(vm.cpu.pstate.z); // ERET이 SPSR에서 Z=1 복원
    CHECK(vm.cpu.pstate.c);
    // 진입 시 DAIF가 마스크되었다가 SPSR로 복원됨 (리셋 상태는 마스크).
    CHECK(vm.cpu.pstate.i);
}

TEST(Exc_UndefinedVectorsWithEc0) {
    VectorVm vm;
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(0x9B037C41u); // madd: 미구현 -> 미정의 명령어 예외
    main_code.push_back(Wfi());       // (복귀 안 함)
    vm.Put(0, main_code);
    vm.Put(kVbarOff + 0x200, {Movz(7, 0xDEAD, 0), Wfi()});

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[7], 0xDEADull);
    // EC=0 (Unknown), ELR = 폴트 명령어 자신.
    CHECK_EQ(vm.cpu.sys.esr_el1 >> 26, 0ull);
    CHECK_EQ(vm.cpu.sys.elr_el1, board::kRamBase + 4 * 4);
}

TEST(Exc_DataAbortVectorsWithFar) {
    VectorVm vm;
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(Movz(0, 0x3000, 0)); // 미매핑 주소
    main_code.push_back(StrX(1, 0, 0x10));
    main_code.push_back(Wfi());
    vm.Put(0, main_code);
    vm.Put(kVbarOff + 0x200, {Mrs(10, sysreg::kFarEl1),
                              Mrs(11, sysreg::kEsrEl1), Wfi()});

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[10], 0x3010ull);            // FAR = 폴트 주소
    CHECK_EQ(vm.cpu.x[11] >> 26, 0x25ull);        // EC = Data Abort (동일 EL)
    CHECK(vm.cpu.x[11] & (1 << 6));               // WnR = 쓰기
    // ELR = 폴트 명령어 (재시도 의미).
    CHECK_EQ(vm.cpu.sys.elr_el1, board::kRamBase + 5 * 4);
}

TEST(Exc_EretToEl0AndSvcFromEl0) {
    VectorVm vm;
    const u64 el0_entry = board::kRamBase + 0x400; // EL0 코드 위치
    // EL1: 벡터 설정, SPSR=EL0t(DAIF 클리어), ELR=el0_entry, ERET.
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(Movz(0, 0));              // SPSR: M=EL0t, DAIF=0
    main_code.push_back(Msr(sysreg::kSpsrEl1, 0));
    main_code.push_back(Movz(1, static_cast<u16>(el0_entry & 0xFFFF), 0));
    main_code.push_back(Movk(1, static_cast<u16>((el0_entry >> 16) & 0xFFFF), 1));
    main_code.push_back(Msr(sysreg::kElrEl1, 1));
    main_code.push_back(Eret());
    vm.Put(0, main_code);
    // EL0 코드: svc #9
    vm.Put(0x400, {Movz(5, 0x55, 0), Svc(9), Nop()});
    // 하위 EL(AArch64) 동기 벡터 (+0x400): EL 확인 후 wfi.
    vm.Put(kVbarOff + 0x400, {Mrs(6, sysreg::kCurrentEl), Wfi()});
    // EL1h 벡터에는 진입하면 안 됨: 마커.
    vm.Put(kVbarOff + 0x200, {Movz(7, 0xBAD, 0), Wfi()});

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[5], 0x55ull);          // EL0 코드 실행됨
    CHECK_EQ(vm.cpu.x[6], 1ull << 2);        // 핸들러는 EL1에서 실행
    CHECK_EQ(vm.cpu.x[7], 0ull);             // EL1h 벡터 미사용
    CHECK_EQ(vm.cpu.sys.esr_el1 & 0xFFFF, 9ull); // SVC imm
    // SPSR: EL0t 모드 기록.
    CHECK_EQ(vm.cpu.sys.spsr_el1 & 0x1F, 0ull);
    CHECK(vm.cpu.pstate.el == ExceptionLevel::EL1);
}

TEST(Exc_El0CannotUsePrivilegedOps) {
    VectorVm vm;
    // EL0로 내려간 뒤 msr vbar_el1 시도 -> 미정의 예외로 EL1 벡터 진입.
    const u64 el0_entry = board::kRamBase + 0x400;
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(Movz(0, 0));
    main_code.push_back(Msr(sysreg::kSpsrEl1, 0));
    main_code.push_back(Movz(1, static_cast<u16>(el0_entry & 0xFFFF), 0));
    main_code.push_back(Movk(1, static_cast<u16>((el0_entry >> 16) & 0xFFFF), 1));
    main_code.push_back(Msr(sysreg::kElrEl1, 1));
    main_code.push_back(Eret());
    vm.Put(0, main_code);
    vm.Put(0x400, {Msr(sysreg::kVbarEl1, 0), Nop()}); // EL0에서 특권 MSR
    vm.Put(kVbarOff + 0x400, {Mrs(6, sysreg::kEsrEl1), Wfi()});

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[6] >> 26, 0ull); // EC=0 (Unknown)
    // ELR = 트랩된 명령어.
    CHECK_EQ(vm.cpu.sys.elr_el1, el0_entry);
}

TEST(Exc_VectorOffsetForSpEl0) {
    VectorVm vm;
    // EL1에서 SPSel=0(SP_EL0 사용) 상태의 동기 예외는 +0x000 벡터.
    std::vector<u32> main_code = SetVbar();
    main_code.push_back(MsrSpsel(0));
    main_code.push_back(Svc(1));
    vm.Put(0, main_code);
    vm.Put(kVbarOff + 0x000, {Movz(3, 0xE10, 0), Wfi()}); // EL1t 벡터
    vm.Put(kVbarOff + 0x200, {Movz(4, 0xBAD, 0), Wfi()}); // EL1h 벡터(오답)

    const StopInfo stop = vm.Run();
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[3], 0xE10ull);
    CHECK_EQ(vm.cpu.x[4], 0ull);
    // 진입 후에는 SP_EL1로 전환 (SPSel=1).
    CHECK(vm.cpu.pstate.sp_sel);
}

TEST(Exc_HarnessModeStillStops) {
    // guest_vectors=false(기본)에서는 Stage 1과 동일하게 정지.
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, 1 << 20, "ram");
    const std::vector<u32> prog = {Svc(3)};
    mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);
    cpu.Reset(board::kRamBase);
    Interpreter interp(cpu, mem); // 기본 설정
    const StopInfo stop = interp.Run(10);
    CHECK(stop.reason == StopReason::kSvc);
    CHECK_EQ(stop.svc_imm, 3);
}
