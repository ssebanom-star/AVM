#include "avm/selftest.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/interp/interpreter.h"
#include "avm/vm/test_firmware.h"
#include "avm/vm/virt_board.h"

namespace avm {

namespace {

using namespace asm64;

class Reporter {
public:
    void Printf(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log_ += buf;
        log_ += '\n';
    }
    void Check(bool ok, const char* what) {
        if (ok) {
            ++passed_;
        } else {
            ++failed_;
            Printf("  FAIL: %s", what);
        }
    }
    template <typename T>
    void CheckEq(T actual, T expected, const char* what) {
        if (actual == expected) {
            ++passed_;
        } else {
            ++failed_;
            Printf("  FAIL: %s (actual=0x%" PRIx64 " expected=0x%" PRIx64 ")",
                   what, (u64)actual, (u64)expected);
        }
    }
    int passed() const { return passed_; }
    int failed() const { return failed_; }
    std::string& log() { return log_; }

private:
    int passed_ = 0;
    int failed_ = 0;
    std::string log_;
};

// 테스트 VM: RAM 4MiB @ board::kRamBase, 프로그램을 RAM 시작에 적재 후 실행.
struct TestVm {
    PhysMem mem;
    CpuState cpu;

    explicit TestVm(const std::vector<u32>& program) {
        mem.AddRam(board::kRamBase, 4 * 1024 * 1024, "test-ram");
        mem.LoadImage(board::kRamBase, program.data(), program.size() * 4);
        cpu.Reset(board::kRamBase);
        // 테스트 스택: RAM 끝.
        cpu.SetCurrentSp(board::kRamBase + 4 * 1024 * 1024);
    }

    StopInfo Run(u64 max_insts = 100000) {
        Interpreter interp(cpu, mem);
        return interp.Run(max_insts);
    }
};

void TestArithmetic(Reporter& r) {
    r.Printf("[1] 산술/논리/이동 명령");
    // x0 = 0x1234'5678'9ABC'DEF0 (movz+movk*3)
    // x1 = x0 + 0x123;  x2 = x1 - x0;  x3 = x0 & x1; x4 = x0 | 1; x5 = x0 ^ x0
    const std::vector<u32> prog = {
        Movz(0, 0xDEF0, 0),
        Movk(0, 0x9ABC, 1),
        Movk(0, 0x5678, 2),
        Movk(0, 0x1234, 3),
        AddImm(1, 0, 0x123),
        SubReg(2, 1, 0),
        AndReg(3, 0, 1),
        Movz(6, 1, 0),
        OrrReg(4, 0, 6),
        EorReg(5, 0, 0),
        Svc(0),
    };
    TestVm vm(prog);
    const StopInfo stop = vm.Run();
    r.Check(stop.reason == StopReason::kSvc, "SVC로 정상 종료");
    r.CheckEq<u64>(vm.cpu.x[0], 0x123456789ABCDEF0ull, "x0 = movz/movk 조립 상수");
    r.CheckEq<u64>(vm.cpu.x[1], 0x123456789ABCDEF0ull + 0x123, "x1 = x0 + 0x123");
    r.CheckEq<u64>(vm.cpu.x[2], 0x123ull, "x2 = x1 - x0");
    r.CheckEq<u64>(vm.cpu.x[3], vm.cpu.x[0] & vm.cpu.x[1], "x3 = x0 & x1");
    r.CheckEq<u64>(vm.cpu.x[4], 0x123456789ABCDEF1ull, "x4 = x0 | 1");
    r.CheckEq<u64>(vm.cpu.x[5], 0ull, "x5 = x0 ^ x0");
    r.CheckEq<u64>(vm.cpu.executed_instructions, prog.size(), "실행 명령어 수");
}

void TestMemory(Reporter& r) {
    r.Printf("[2] 로드/스토어 (unsigned/pre/post index)");
    const u64 data_addr = board::kRamBase + 0x1000;
    const std::vector<u32> prog = {
        // x0 = data_addr (movz+movk)
        Movz(0, static_cast<u16>(data_addr & 0xFFFF), 0),
        Movk(0, static_cast<u16>((data_addr >> 16) & 0xFFFF), 1),
        Movk(0, static_cast<u16>((data_addr >> 32) & 0xFFFF), 2),
        // x1 = 0xCAFE; [x0] = x1; x2 = [x0]
        Movz(1, 0xCAFE, 0),
        StrX(1, 0, 0),
        LdrX(2, 0, 0),
        // 32비트: [x0+8](W) = w1; w3 = [x0+8]
        StrW(1, 0, 8),
        LdrW(3, 0, 8),
        // pre-index: [x0, #16]! = x1  (x0 += 16)
        StrXPre(1, 0, 16),
        // post-index: x4 = [x0], #8 (x4 = mem[x0], x0 += 8)
        LdrXPost(4, 0, 8),
        Svc(0),
    };
    TestVm vm(prog);
    const StopInfo stop = vm.Run();
    r.Check(stop.reason == StopReason::kSvc, "SVC로 정상 종료");
    u64 stored = 0;
    vm.mem.ReadT(data_addr, stored);
    r.CheckEq<u64>(stored, 0xCAFEull, "STR/메모리 반영");
    r.CheckEq<u64>(vm.cpu.x[2], 0xCAFEull, "LDR 결과");
    r.CheckEq<u64>(vm.cpu.x[3], 0xCAFEull, "32비트 STR/LDR 왕복");
    r.CheckEq<u64>(vm.cpu.x[4], 0xCAFEull, "pre-index 저장 후 post-index 로드");
    r.CheckEq<u64>(vm.cpu.x[0], data_addr + 24, "인덱스 라이트백 (16 + 8)");
    // dirty 페이지 추적 검증.
    r.Check(vm.mem.Page(data_addr)->flags & kPageDirty, "쓰기 페이지 dirty 표시");
}

void TestBranchesAndCalls(Reporter& r) {
    r.Printf("[3] 분기/루프/함수 호출 (1..10 합)");
    // x0 = 합계, x1 = 카운터(10 -> 0), BL로 덧셈 서브루틴 호출.
    //  0: movz x0, #0
    //  4: movz x1, #10
    //  8: cbz  x1, +16      ; -> 24 (svc)
    // 12: bl   +12          ; -> 24+? 서브루틴 @ 28... 계산: bl은 12에서 +16 => 28
    // 16: sub  x1, x1, #1
    // 20: b    -12          ; -> 8
    // 24: svc  #0
    // 28: add  x0, x0, x1   ; 서브루틴: 합산
    // 32: ret
    const std::vector<u32> prog = {
        /* 0*/ Movz(0, 0),
        /* 4*/ Movz(1, 10),
        /* 8*/ Cbz(1, 16),
        /*12*/ Bl(16),
        /*16*/ SubImm(1, 1, 1),
        /*20*/ B(-12),
        /*24*/ Svc(0),
        /*28*/ AddReg(0, 0, 1),
        /*32*/ Ret(),
    };
    TestVm vm(prog);
    const StopInfo stop = vm.Run();
    r.Check(stop.reason == StopReason::kSvc, "SVC로 정상 종료");
    r.CheckEq<u64>(vm.cpu.x[0], 55ull, "x0 = 1..10 합");
    r.CheckEq<u64>(vm.cpu.x[1], 0ull, "카운터 소진");
}

void TestIndirectBranch(Reporter& r) {
    r.Printf("[4] 간접 분기 (BR)");
    const u64 target = board::kRamBase + 0x100;
    const std::vector<u32> prog = {
        Movz(9, static_cast<u16>(target & 0xFFFF), 0),
        Movk(9, static_cast<u16>((target >> 16) & 0xFFFF), 1),
        Movk(9, static_cast<u16>((target >> 32) & 0xFFFF), 2),
        Br(9),
    };
    TestVm vm(prog);
    // target 지점에 x7 = 0x77; svc 를 배치.
    const u32 target_code[] = {Movz(7, 0x77, 0), Svc(0)};
    vm.mem.LoadImage(target, target_code, sizeof(target_code));
    const StopInfo stop = vm.Run();
    r.Check(stop.reason == StopReason::kSvc, "SVC로 정상 종료");
    r.CheckEq<u64>(vm.cpu.x[7], 0x77ull, "간접 분기 후 코드 실행");
}

void TestConditionFlags(Reporter& r) {
    r.Printf("[5] 조건 플래그와 B.cond");
    // subs xzr, x0, x1 (cmp) 후 b.eq / b.lt 검증.
    const std::vector<u32> prog = {
        /* 0*/ Movz(0, 5),
        /* 4*/ Movz(1, 5),
        /* 8*/ CmpReg(0, 1),
        /*12*/ BCond(0 /*EQ*/, 8),   // -> 20
        /*16*/ Movz(2, 0xBAD, 0),    // 실패 경로
        /*20*/ Movz(3, 1, 0),        // 성공 표시
        /*24*/ Movz(4, 3),
        /*28*/ CmpImm(4, 7),
        /*32*/ BCond(11 /*LT*/, 8),  // 3 < 7 -> 40
        /*36*/ Movz(5, 0xBAD, 0),
        /*40*/ Svc(0),
    };
    TestVm vm(prog);
    const StopInfo stop = vm.Run();
    r.Check(stop.reason == StopReason::kSvc, "SVC로 정상 종료");
    r.CheckEq<u64>(vm.cpu.x[2], 0ull, "EQ 분기: 실패 경로 미실행");
    r.CheckEq<u64>(vm.cpu.x[3], 1ull, "EQ 분기: 성공 경로 실행");
    r.CheckEq<u64>(vm.cpu.x[5], 0ull, "LT 분기: 실패 경로 미실행");
    r.Check(vm.cpu.pstate.n, "3-7 < 0 => N=1");
}

void TestPreciseFaults(Reporter& r) {
    r.Printf("[6] 정밀 예외: 미매핑 접근 / 미정의 명령어");
    {
        // 미매핑 주소 로드 -> Data Abort로 정지, 주소 정확 보고.
        const std::vector<u32> prog = {
            Movz(0, 0x1000, 0), // x0 = 0x1000 (RAM 밖)
            LdrX(1, 0, 0),
            Svc(0),
        };
        TestVm vm(prog);
        const StopInfo stop = vm.Run();
        r.Check(stop.reason == StopReason::kDataAbort, "미매핑 로드 -> Data Abort");
        r.CheckEq<u64>(stop.fault.addr, 0x1000ull, "폴트 주소 보고");
        r.CheckEq<u64>(stop.pc, board::kRamBase + 4, "폴트 PC 보고");
    }
    {
        // 미정의 명령어 -> NOP 무시가 아니라 정지.
        const std::vector<u32> prog = {Nop(), 0xFFFFFFFFu, Svc(0)};
        TestVm vm(prog);
        const StopInfo stop = vm.Run();
        r.Check(stop.reason == StopReason::kUndefinedInst,
                "미정의 명령어 -> Undefined 정지");
        r.CheckEq<u64>(stop.pc, board::kRamBase + 4, "미정의 명령어 PC 보고");
    }
}

void TestFirmwareBoot(Reporter& r) {
    r.Printf("[8] 최소 펌웨어 부팅 (플래시 리셋 -> UART -> SVC 예외 -> ERET -> WFI)");
    VirtBoard board;
    r.Check(board.ok(), "보드 구성 (플래시+RAM+UART)");
    const std::vector<u8> firmware = testfw::BuildTestFirmware();
    r.Check(board.LoadFirmware(firmware.data(), firmware.size()), "펌웨어 적재");

    const StopInfo stop = board.Run(100000);
    r.Check(stop.reason == StopReason::kWfi, "WFI로 정상 종료");
    r.Check(board.Uart().TxLog() == testfw::kExpectedUartOutput,
            "UART 출력 = \"AVM:!OK\\n\"");
    r.CheckEq<u64>(board.Cpu().x[10] >> 26, 0x15ull, "핸들러가 읽은 ESR.EC = SVC");
    r.CheckEq<u64>(board.Cpu().x[10] & 0xFFFF, testfw::kSvcNumber,
                   "ESR.ISS = SVC #imm");
    r.Printf("  UART: %s", board.Uart().TxLog().c_str());
}

void TestMmuTranslation(Reporter& r) {
    r.Printf("[9] MMU: 게스트가 켠 Stage 1 변환 + 소프트웨어 TLB");
    // RAM 8MB. 페이지 테이블 풀 = RAM+4MB. 게스트 코드가 TTBR/TCR/SCTLR을
    // 설정하고, 비항등 매핑(VA 0x200000 -> PA RAM+0x2000)을 통해 읽는다.
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, 8 * 1024 * 1024, "ram");
    cpu.Reset(board::kRamBase);

    u64 next_table = board::kRamBase + 4 * 1024 * 1024;
    auto alloc = [&next_table] {
        const u64 t = next_table;
        next_table += kGuestPageSize;
        return t;
    };
    const u64 root = alloc();
    constexpr u64 kAf = 1ull << 10;
    auto map4k = [&](u64 va, u64 pa) {
        u64 table = root;
        for (unsigned level = 0; level < 3; ++level) {
            const unsigned shift = 12 + 9 * (3 - level);
            const u64 idx = (va >> shift) & 0x1FF;
            u64 desc = 0;
            mem.ReadT(table + idx * 8, desc);
            if (!(desc & 1)) {
                const u64 t = alloc();
                mem.WriteT(table + idx * 8, t | 0b11);
                table = t;
            } else {
                table = desc & 0x0000FFFFFFFFF000ull;
            }
        }
        mem.WriteT(table + ((va >> 12) & 0x1FF) * 8, (pa & ~0xFFFull) | kAf | 0b11);
    };
    // 코드 영역 항등 매핑 + 검증용 비항등 매핑.
    for (u64 off = 0; off < 0x10000; off += kGuestPageSize) {
        map4k(board::kRamBase + off, board::kRamBase + off);
    }
    map4k(0x200000, board::kRamBase + 0x2000);
    mem.WriteT<u64>(board::kRamBase + 0x2000, 0xFEEDFACE12345678ull);

    std::vector<u32> prog;
    prog.push_back(Movz(15, static_cast<u16>(root & 0xFFFF), 0));
    prog.push_back(Movk(15, static_cast<u16>((root >> 16) & 0xFFFF), 1));
    prog.push_back(Msr(sysreg::kTtbr0El1, 15));
    const u64 tcr = 16 | (16ull << 16) | (2ull << 30) | (1ull << 23);
    prog.push_back(Movz(15, static_cast<u16>(tcr & 0xFFFF), 0));
    prog.push_back(Movk(15, static_cast<u16>((tcr >> 16) & 0xFFFF), 1));
    prog.push_back(Msr(sysreg::kTcrEl1, 15));
    prog.push_back(Movz(15, 0xFF, 0));
    prog.push_back(Msr(sysreg::kMairEl1, 15));
    prog.push_back(Isb());
    prog.push_back(Mrs(15, sysreg::kSctlrEl1));
    prog.push_back(LogicalImm(0b01, 15, 15, 1, 0, 0, true)); // orr x15,x15,#1
    prog.push_back(Msr(sysreg::kSctlrEl1, 15));
    prog.push_back(Isb());
    prog.push_back(Movz(0, 0x0020, 1)); // x0 = VA 0x200000
    prog.push_back(LdrX(1, 0, 0));
    prog.push_back(TlbiVmalle1());
    prog.push_back(LdrX(2, 0, 0));      // TLBI 후에도 동일 값
    prog.push_back(Svc(0));
    mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);

    Interpreter interp(cpu, mem);
    const StopInfo stop = interp.Run(100000);
    r.Check(stop.reason == StopReason::kSvc, "MMU 활성 상태로 정상 종료");
    r.CheckEq<u64>(cpu.x[1], 0xFEEDFACE12345678ull, "비항등 매핑 로드");
    r.CheckEq<u64>(cpu.x[2], 0xFEEDFACE12345678ull, "TLBI 후 재변환 로드");
    r.Check(interp.GetMmu().Stats().tlb_hits > 0, "소프트웨어 TLB 적중 발생");
    r.Check(interp.GetMmu().Stats().walks > 0, "페이지 테이블 워크 수행");
}

void TestSelfModifyingCodeTracking(Reporter& r) {
    r.Printf("[7] 코드 페이지 수정 추적 (JIT 무효화 기반)");
    TestVm vm({Nop(), Svc(0)});
    vm.mem.MarkPageHasCode(board::kRamBase);
    const u32 gen_before = vm.mem.Page(board::kRamBase)->code_generation;
    const u32 patch = Movz(0, 1, 0);
    vm.mem.WriteT<u32>(board::kRamBase, patch);
    const PageInfo* page = vm.mem.Page(board::kRamBase);
    r.Check(page->code_generation == gen_before + 1, "코드 페이지 쓰기 -> 세대 증가");
    r.Check(!(page->flags & kPageHasCode), "코드 페이지 플래그 해제");
}

} // namespace

SelfTestResult RunCpuSelfTest() {
    Reporter r;
    r.Printf("AVM Stage 3 엔진 셀프테스트");
    r.Printf("게스트: AArch64 @ avm-virt 보드 (RAM 0x%llx)",
             (unsigned long long)board::kRamBase);
    r.Printf("----------------------------------------");

    TestArithmetic(r);
    TestMemory(r);
    TestBranchesAndCalls(r);
    TestIndirectBranch(r);
    TestConditionFlags(r);
    TestPreciseFaults(r);
    TestSelfModifyingCodeTracking(r);
    TestFirmwareBoot(r);
    TestMmuTranslation(r);

    r.Printf("----------------------------------------");
    r.Printf("결과: %d 통과, %d 실패", r.passed(), r.failed());

    SelfTestResult result;
    result.passed = r.passed();
    result.failed = r.failed();
    result.all_passed = r.failed() == 0;
    result.log = std::move(r.log());
    return result;
}

} // namespace avm
