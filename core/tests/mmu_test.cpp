// Stage 1 MMU 단위/통합 테스트 (Stage 3).
//
// 1부: Mmu::Translate 직접 호출 — 변환 정확성과 폴트 분류(FSC).
// 2부: 게스트 코드가 직접 MMU를 켜고 접근 — 인터프리터 통합 검증.
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/interp/interpreter.h"
#include "avm/mmu/mmu.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;

namespace {

// 페이지 테이블 디스크립터 속성.
constexpr u64 kAf    = 1ull << 10; // Access Flag
constexpr u64 kApEl0 = 1ull << 6;  // AP[1]: EL0 접근 허용
constexpr u64 kApRo  = 1ull << 7;  // AP[2]: 읽기 전용
constexpr u64 kUxn   = 1ull << 54;
constexpr u64 kPxn   = 1ull << 53;

// 테스트용 페이지 테이블 빌더: RAM의 풀에서 테이블을 할당하고
// 호스트 쓰기로 디스크립터를 구성한다 (4KB 그래뉼, 레벨 0 시작).
class PageTables {
public:
    PageTables(PhysMem& mem, u64 pool_base)
        : mem_(mem), next_(pool_base), root_(Alloc()) {}

    u64 root() const { return root_; }

    // 4KB 페이지 매핑.
    void Map4K(u64 va, u64 pa, u64 attrs) {
        u64 table = root_;
        for (unsigned level = 0; level < 3; ++level) {
            const unsigned shift = 12 + 9 * (3 - level);
            const u64 index = (va >> shift) & 0x1FF;
            u64 desc = ReadDesc(table, index);
            if (!(desc & 1)) {
                const u64 next_table = Alloc();
                WriteDesc(table, index, next_table | 0b11);
                table = next_table;
            } else {
                table = desc & 0x0000FFFFFFFFF000ull;
            }
        }
        WriteDesc(table, (va >> 12) & 0x1FF, (pa & ~0xFFFull) | attrs | 0b11);
    }

    // 2MB 블록 매핑 (레벨 2).
    void MapBlock2M(u64 va, u64 pa, u64 attrs) {
        u64 table = root_;
        for (unsigned level = 0; level < 2; ++level) {
            const unsigned shift = 12 + 9 * (3 - level);
            const u64 index = (va >> shift) & 0x1FF;
            u64 desc = ReadDesc(table, index);
            if (!(desc & 1)) {
                const u64 next_table = Alloc();
                WriteDesc(table, index, next_table | 0b11);
                table = next_table;
            } else {
                table = desc & 0x0000FFFFFFFFF000ull;
            }
        }
        WriteDesc(table, (va >> 21) & 0x1FF,
                  (pa & ~0x1FFFFFull) | attrs | 0b01);
    }

    // 리프 디스크립터 직접 수정 (TLB 무효화 테스트용).
    void Patch4K(u64 va, u64 new_desc) {
        u64 table = root_;
        for (unsigned level = 0; level < 3; ++level) {
            const unsigned shift = 12 + 9 * (3 - level);
            table = ReadDesc(table, (va >> shift) & 0x1FF) &
                    0x0000FFFFFFFFF000ull;
        }
        WriteDesc(table, (va >> 12) & 0x1FF, new_desc);
    }

private:
    u64 Alloc() {
        const u64 table = next_;
        next_ += kGuestPageSize;
        // RAM은 0으로 초기화되어 있으므로 별도 클리어 불필요.
        return table;
    }
    u64 ReadDesc(u64 table, u64 index) {
        u64 desc = 0;
        mem_.ReadT(table + index * 8, desc);
        return desc;
    }
    void WriteDesc(u64 table, u64 index, u64 desc) {
        mem_.WriteT(table + index * 8, desc);
    }

    PhysMem& mem_;
    u64 next_;
    u64 root_;
};

// 직접 변환 테스트 픽스처.
struct MmuFixture {
    PhysMem mem;
    CpuState cpu;
    PageTables pt;
    Mmu mmu;

    static constexpr u64 kRamSize = 8 * 1024 * 1024;
    static constexpr u64 kPtPool = board::kRamBase + 4 * 1024 * 1024;

    MmuFixture() : pt((mem.AddRam(board::kRamBase, kRamSize, "ram"), mem),
                      kPtPool),
                   mmu(cpu, mem) {
        cpu.Reset(board::kRamBase);
        cpu.sys.ttbr0_el1 = pt.root();
        cpu.sys.tcr_el1 = 16 | (16ull << 16) | (2ull << 30); // T0SZ=T1SZ=16, TG1=4K
        cpu.sys.sctlr_el1 |= 1; // MMU on
        cpu.mmu_generation++;   // 상태 변경 반영
    }

    // 편의: 변환 성공 검증.
    u64 MustTranslate(u64 va, AccessType type) {
        MmuFault fault;
        u64 pa = 0;
        const bool ok = mmu.Translate(va, type, fault, pa);
        CHECK(ok);
        return pa;
    }
    u32 MustFault(u64 va, AccessType type) {
        MmuFault fault;
        u64 pa = 0;
        const bool ok = mmu.Translate(va, type, fault, pa);
        CHECK(!ok);
        return fault.fsc;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 1부: 직접 변환
// ---------------------------------------------------------------------------

TEST(Mmu_DisabledIsIdentity) {
    PhysMem mem;
    CpuState cpu;
    mem.AddRam(board::kRamBase, 1 << 20, "ram");
    Mmu mmu(cpu, mem);
    cpu.sys.sctlr_el1 &= ~1ull;
    MmuFault fault;
    u64 pa = 0;
    CHECK(mmu.Translate(0x12345678, AccessType::kLoad, fault, pa));
    CHECK_EQ(pa, 0x12345678ull);
}

TEST(Mmu_Basic4KTranslation) {
    MmuFixture f;
    // VA 0x40_0000 -> PA kRamBase+0x1000 (비항등).
    f.pt.Map4K(0x400000, board::kRamBase + 0x1000, kAf);
    const u64 pa = f.MustTranslate(0x400123, AccessType::kLoad);
    CHECK_EQ(pa, board::kRamBase + 0x1123);
}

TEST(Mmu_Block2MTranslation) {
    MmuFixture f;
    f.pt.MapBlock2M(0x8000000, board::kRamBase, kAf);
    const u64 pa = f.MustTranslate(0x8123456, AccessType::kLoad);
    CHECK_EQ(pa, board::kRamBase + 0x123456);
}

TEST(Mmu_Ttbr1HighHalf) {
    MmuFixture f;
    // 상위 반: 별도 루트 테이블.
    PageTables pt1(f.mem, MmuFixture::kPtPool + 0x100000);
    f.cpu.sys.ttbr1_el1 = pt1.root();
    f.cpu.mmu_generation++;
    const u64 high_va = 0xFFFF'0000'0000'2000ull; // T1SZ=16 범위
    pt1.Map4K(high_va, board::kRamBase + 0x3000, kAf);
    const u64 pa = f.MustTranslate(high_va + 0x10, AccessType::kLoad);
    CHECK_EQ(pa, board::kRamBase + 0x3010);
}

TEST(Mmu_TranslationFaultLevels) {
    MmuFixture f;
    // 아무것도 매핑 안 됨: 레벨 0 디스크립터가 invalid -> L0 폴트.
    CHECK_EQ(f.MustFault(0x400000, AccessType::kLoad), fsc::Translation(0));
    // L0~L2 테이블은 있지만 L3 리프 없음 -> L3 폴트.
    f.pt.Map4K(0x400000, board::kRamBase, kAf);
    CHECK_EQ(f.MustFault(0x401000, AccessType::kLoad), fsc::Translation(3));
}

TEST(Mmu_VaRangeFault) {
    MmuFixture f;
    // T0SZ=16 -> 48비트 초과 하위 반 VA는 변환 폴트 L0.
    CHECK_EQ(f.MustFault(0x0001'0000'0000'0000ull, AccessType::kLoad),
             fsc::Translation(0));
    // 상위 반이지만 전부 1이 아닌 주소 (bit55=1이나 상위 비트 불일치).
    CHECK_EQ(f.MustFault(0x0080'0000'0000'0000ull, AccessType::kLoad),
             fsc::Translation(0));
}

TEST(Mmu_AccessFlagFault) {
    MmuFixture f;
    f.pt.Map4K(0x400000, board::kRamBase, 0); // AF=0
    CHECK_EQ(f.MustFault(0x400000, AccessType::kLoad), fsc::AccessFlag(3));
}

TEST(Mmu_PermissionFaults) {
    MmuFixture f;
    // 읽기 전용 페이지: EL1 쓰기 -> 권한 폴트 L3.
    f.pt.Map4K(0x400000, board::kRamBase, kAf | kApRo);
    CHECK_EQ(f.MustFault(0x400000, AccessType::kStore), fsc::Permission(3));
    // 읽기는 성공.
    f.MustTranslate(0x400000, AccessType::kLoad);

    // EL1 전용 페이지: EL0 접근 -> 권한 폴트.
    f.pt.Map4K(0x500000, board::kRamBase, kAf);
    f.cpu.pstate.el = ExceptionLevel::EL0;
    CHECK_EQ(f.MustFault(0x500000, AccessType::kLoad), fsc::Permission(3));
    f.cpu.pstate.el = ExceptionLevel::EL1;
}

TEST(Mmu_ExecutePermissions) {
    MmuFixture f;
    // UXN: EL0 실행 금지, EL1 실행 허용 (EL0 접근 불가 페이지).
    f.pt.Map4K(0x400000, board::kRamBase, kAf | kUxn);
    f.MustTranslate(0x400000, AccessType::kFetch); // EL1 fetch OK
    // PXN: EL1 실행 금지.
    f.pt.Map4K(0x500000, board::kRamBase, kAf | kPxn);
    CHECK_EQ(f.MustFault(0x500000, AccessType::kFetch), fsc::Permission(3));
    // EL0-쓰기 가능 페이지는 EL1 실행 금지 (아키텍처 규칙).
    f.pt.Map4K(0x600000, board::kRamBase, kAf | kApEl0);
    CHECK_EQ(f.MustFault(0x600000, AccessType::kFetch), fsc::Permission(3));
    // 같은 페이지를 EL0가 실행하는 것은 허용 (UXN=0).
    f.cpu.pstate.el = ExceptionLevel::EL0;
    f.MustTranslate(0x600000, AccessType::kFetch);
    f.cpu.pstate.el = ExceptionLevel::EL1;
}

TEST(Mmu_EpdDisablesWalk) {
    MmuFixture f;
    f.pt.Map4K(0x400000, board::kRamBase, kAf);
    f.cpu.sys.tcr_el1 |= 1ull << 7; // EPD0
    f.cpu.mmu_generation++;
    CHECK_EQ(f.MustFault(0x400000, AccessType::kLoad), fsc::Translation(0));
}

TEST(Mmu_WalkExternalAbort) {
    MmuFixture f;
    // TTBR0가 미매핑 물리 주소를 가리킴 -> 워크 중 외부 중단 (L0).
    f.cpu.sys.ttbr0_el1 = 0x1000; // RAM 밖
    f.cpu.mmu_generation++;
    CHECK_EQ(f.MustFault(0x400000, AccessType::kLoad), fsc::ExternalWalk(0));
}

TEST(Mmu_TlbHitAndInvalidate) {
    MmuFixture f;
    f.pt.Map4K(0x400000, board::kRamBase + 0x1000, kAf);
    f.MustTranslate(0x400000, AccessType::kLoad); // 미스 -> 워크
    const u64 misses = f.mmu.Stats().tlb_misses;
    f.MustTranslate(0x400008, AccessType::kLoad); // 같은 페이지 -> 적중
    CHECK_EQ(f.mmu.Stats().tlb_misses, misses);
    CHECK(f.mmu.Stats().tlb_hits >= 1);

    // 페이지 테이블 변경 후 TLBI 없이는 이전 매핑이 남을 수 있고(캐시),
    // TLBI(세대 증가) 후에는 반드시 새 매핑이 보인다.
    f.pt.Patch4K(0x400000, (board::kRamBase + 0x2000) | kAf | 0b11);
    f.cpu.mmu_generation++; // TLBI vmalle1과 동일 효과
    const u64 pa = f.MustTranslate(0x400000, AccessType::kLoad);
    CHECK_EQ(pa, board::kRamBase + 0x2000);
}

TEST(Mmu_UnsupportedGranuleFaults) {
    MmuFixture f;
    f.pt.Map4K(0x400000, board::kRamBase, kAf);
    f.cpu.sys.tcr_el1 |= 1ull << 14; // TG0=01 (64KB) — 미지원
    f.cpu.mmu_generation++;
    CHECK_EQ(f.MustFault(0x400000, AccessType::kLoad), fsc::Translation(0));
}

// ---------------------------------------------------------------------------
// 2부: 게스트 통합 (게스트 코드가 MMU를 켜고 사용)
// ---------------------------------------------------------------------------

namespace {

// 게스트 통합 픽스처: 코드는 RAM 시작(항등 매핑), 테이블 풀은 +4MB.
// 게스트 코드가 TTBR/TCR/SCTLR을 설정한다.
struct GuestMmuVm {
    PhysMem mem;
    CpuState cpu;
    PageTables pt;

    static constexpr u64 kRamSize = 8 * 1024 * 1024;

    GuestMmuVm() : pt((mem.AddRam(board::kRamBase, kRamSize, "ram"), mem),
                      board::kRamBase + 4 * 1024 * 1024) {
        cpu.Reset(board::kRamBase);
        cpu.SetCurrentSp(board::kRamBase + kRamSize);
        // 코드/스택 페이지 항등 매핑 (MMU 켠 직후에도 인출 가능해야 함).
        for (u64 offset = 0; offset < 0x10000; offset += kGuestPageSize) {
            pt.Map4K(board::kRamBase + offset, board::kRamBase + offset, kAf);
        }
        for (u64 offset = kRamSize - 0x10000; offset < kRamSize;
             offset += kGuestPageSize) {
            pt.Map4K(board::kRamBase + offset, board::kRamBase + offset, kAf);
        }
    }

    // MMU 활성화 시퀀스 (x15 스크래치).
    std::vector<u32> EnableMmuCode() const {
        std::vector<u32> code;
        const u64 root = pt.root();
        code.push_back(Movz(15, static_cast<u16>(root & 0xFFFF), 0));
        code.push_back(Movk(15, static_cast<u16>((root >> 16) & 0xFFFF), 1));
        code.push_back(Movk(15, static_cast<u16>((root >> 32) & 0xFFFF), 2));
        code.push_back(Msr(sysreg::kTtbr0El1, 15));
        // TCR: T0SZ=16, T1SZ=16, TG1=4KB, EPD1=1 (상위 반 미사용).
        const u64 tcr = 16 | (16ull << 16) | (2ull << 30) | (1ull << 23);
        code.push_back(Movz(15, static_cast<u16>(tcr & 0xFFFF), 0));
        code.push_back(Movk(15, static_cast<u16>((tcr >> 16) & 0xFFFF), 1));
        code.push_back(Msr(sysreg::kTcrEl1, 15));
        code.push_back(Movz(15, 0xFF, 0)); // MAIR attr0 = normal WB
        code.push_back(Msr(sysreg::kMairEl1, 15));
        code.push_back(Isb());
        code.push_back(Mrs(15, sysreg::kSctlrEl1));
        code.push_back(LogicalImm(0b01, 15, 15, 1, 0, 0, true)); // orr x15,x15,#1
        code.push_back(Msr(sysreg::kSctlrEl1, 15));
        code.push_back(Isb());
        return code;
    }

    StopInfo Run(const std::vector<u32>& program, bool vectors = false,
                 u64 max = 100000) {
        mem.LoadImage(board::kRamBase, program.data(), program.size() * 4);
        Interpreter interp(cpu, mem, ExecConfig{.guest_vectors = vectors});
        return interp.Run(max);
    }
};

} // namespace

TEST(MmuGuest_EnableAndRemappedLoad) {
    GuestMmuVm vm;
    // VA 0x20_0000 -> PA kRamBase+0x2000. 호스트가 마커를 물리에 기록.
    vm.pt.Map4K(0x200000, board::kRamBase + 0x2000, kAf);
    vm.mem.WriteT<u64>(board::kRamBase + 0x2000, 0xFEEDFACE12345678ull);

    std::vector<u32> prog = vm.EnableMmuCode();
    prog.push_back(Movz(0, 0x0020, 1)); // x0 = 0x200000
    prog.push_back(LdrX(1, 0, 0));      // 변환된 로드
    prog.push_back(Mrs(2, sysreg::kSctlrEl1));
    prog.push_back(Svc(0));

    const StopInfo stop = vm.Run(prog);
    CHECK(stop.reason == StopReason::kSvc);
    CHECK_EQ(vm.cpu.x[1], 0xFEEDFACE12345678ull);
    CHECK(vm.cpu.x[2] & 1); // MMU 켜진 상태로 실행 중이었음
}

TEST(MmuGuest_StoreThroughMappingHitsPhysical) {
    GuestMmuVm vm;
    vm.pt.Map4K(0x300000, board::kRamBase + 0x3000, kAf);

    std::vector<u32> prog = vm.EnableMmuCode();
    prog.push_back(Movz(0, 0x0030, 1));      // x0 = 0x300000
    prog.push_back(Movz(1, 0xABCD, 0));
    prog.push_back(StrX(1, 0, 8));
    prog.push_back(Svc(0));

    CHECK(vm.Run(prog).reason == StopReason::kSvc);
    u64 value = 0;
    vm.mem.ReadT(board::kRamBase + 0x3008, value);
    CHECK_EQ(value, 0xABCDull);
}

TEST(MmuGuest_TranslationFaultVectorsWithFsc) {
    GuestMmuVm vm;
    // 벡터 테이블: 코드 영역 안 +0x800 (항등 매핑됨).
    std::vector<u32> prog = vm.EnableMmuCode();
    const u64 vbar = board::kRamBase + 0x800;
    prog.push_back(Movz(9, static_cast<u16>(vbar & 0xFFFF), 0));
    prog.push_back(Movk(9, static_cast<u16>((vbar >> 16) & 0xFFFF), 1));
    prog.push_back(Msr(sysreg::kVbarEl1, 9));
    prog.push_back(Movz(0, 0x0070, 1));  // 미매핑 VA 0x700000
    prog.push_back(LdrX(1, 0, 0x20));
    prog.push_back(Nop());
    // 핸들러 공간까지 0으로 확장 후 배치.
    prog.resize(0x800 / 4 + 0x200 / 4, 0);
    const std::vector<u32> handler = {
        Mrs(10, sysreg::kEsrEl1),
        Mrs(11, sysreg::kFarEl1),
        Wfi(),
    };
    for (u32 w : handler) prog.push_back(w);

    const StopInfo stop = vm.Run(prog, /*vectors=*/true);
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(vm.cpu.x[10] >> 26, 0x25ull);            // Data Abort 동일 EL
    // L0[0]은 항등 매핑이 만든 L1 테이블을 가리키므로 L1에서 미매핑.
    CHECK_EQ(vm.cpu.x[10] & 0x3F, (u64)fsc::Translation(1));
    CHECK_EQ(vm.cpu.x[11], 0x700020ull);              // FAR = VA
}

TEST(MmuGuest_RoPageWriteFaults) {
    GuestMmuVm vm;
    vm.pt.Map4K(0x200000, board::kRamBase + 0x2000, kAf | kApRo);

    std::vector<u32> prog = vm.EnableMmuCode();
    prog.push_back(Movz(0, 0x0020, 1));
    prog.push_back(StrX(1, 0, 0)); // RO 페이지 쓰기
    prog.push_back(Svc(0));

    const StopInfo stop = vm.Run(prog); // 하니스 모드: 정지로 보고
    CHECK(stop.reason == StopReason::kDataAbort);
    CHECK_EQ(stop.fault.addr, 0x200000ull);
    CHECK(stop.fault.is_write);
}

TEST(MmuGuest_TlbiAfterPatchSeesNewMapping) {
    GuestMmuVm vm;
    vm.pt.Map4K(0x200000, board::kRamBase + 0x2000, kAf);
    vm.mem.WriteT<u64>(board::kRamBase + 0x2000, 111);
    vm.mem.WriteT<u64>(board::kRamBase + 0x5000, 222);

    // 새 리프 디스크립터를 미리 계산해 게스트가 str으로 패치하게 한다.
    // (더 간단히: 호스트가 패치하고 게스트는 TLBI만 수행)
    std::vector<u32> prog = vm.EnableMmuCode();
    prog.push_back(Movz(0, 0x0020, 1));
    prog.push_back(LdrX(1, 0, 0));      // TLB에 적재 (111)
    prog.push_back(Svc(1));             // 호스트: 테이블 패치 지점
    prog.push_back(TlbiVmalle1());
    prog.push_back(Isb());
    prog.push_back(LdrX(2, 0, 0));      // 새 매핑 (222)
    prog.push_back(Svc(0));

    vm.mem.LoadImage(board::kRamBase, prog.data(), prog.size() * 4);
    Interpreter interp(vm.cpu, vm.mem, ExecConfig{});
    StopInfo stop = interp.Run(100000);
    CHECK(stop.reason == StopReason::kSvc);
    CHECK_EQ(stop.svc_imm, 1);
    CHECK_EQ(vm.cpu.x[1], 111ull);
    // 호스트가 리프를 패치.
    vm.pt.Patch4K(0x200000, (board::kRamBase + 0x5000) | kAf | 0b11);
    stop = interp.Run(100000);
    CHECK(stop.reason == StopReason::kSvc);
    CHECK_EQ(stop.svc_imm, 0);
    CHECK_EQ(vm.cpu.x[2], 222ull);
}

TEST(MmuGuest_DcZvaZeroesBlock) {
    GuestMmuVm vm;
    // 항등 매핑 영역의 데이터 페이지에 패턴을 깔고 DC ZVA로 지운다.
    const u64 data = board::kRamBase + 0x4000;
    vm.pt.Map4K(data, data, kAf);
    for (unsigned i = 0; i < 128; i += 8) {
        vm.mem.WriteT<u64>(data + i, ~0ull);
    }
    std::vector<u32> prog = vm.EnableMmuCode();
    prog.push_back(Movz(0, static_cast<u16>(data & 0xFFFF), 0));
    prog.push_back(Movk(0, static_cast<u16>((data >> 16) & 0xFFFF), 1));
    prog.push_back(AddImm(0, 0, 0x20)); // 블록 중간 주소 (정렬은 명령이 수행)
    prog.push_back(DcZva(0));
    prog.push_back(Mrs(3, sysreg::kDczidEl0));
    prog.push_back(Svc(0));

    CHECK(vm.Run(prog).reason == StopReason::kSvc);
    u64 value = ~0ull;
    vm.mem.ReadT(data, value);
    CHECK_EQ(value, 0ull);           // 블록 시작(정렬됨)부터
    vm.mem.ReadT(data + 56, value);
    CHECK_EQ(value, 0ull);           // 블록 끝까지
    vm.mem.ReadT(data + 64, value);
    CHECK_EQ(value, ~0ull);          // 다음 블록은 그대로
    CHECK_EQ(vm.cpu.x[3], 4ull);     // DCZID: 64바이트
}

TEST(MmuGuest_AlignmentCheckWhenSctlrA) {
    GuestMmuVm vm;
    vm.pt.Map4K(0x200000, board::kRamBase + 0x2000, kAf);
    std::vector<u32> prog = vm.EnableMmuCode();
    // SCTLR.A 켜기.
    prog.push_back(Mrs(15, sysreg::kSctlrEl1));
    prog.push_back(LogicalImm(0b01, 15, 15, 1, 63, 0, true)); // orr #2
    prog.push_back(Msr(sysreg::kSctlrEl1, 15));
    prog.push_back(Movz(0, 0x0020, 1));
    prog.push_back(LdrX(1, 0, 0));      // 정렬 접근: OK
    prog.push_back(AddImm(0, 0, 1));
    prog.push_back(LdrX(2, 0, 0));      // 비정렬 8바이트 -> 정렬 폴트
    prog.push_back(Svc(0));

    const StopInfo stop = vm.Run(prog);
    CHECK(stop.reason == StopReason::kDataAbort);
    CHECK_EQ(stop.fault.addr, 0x200001ull);
}
