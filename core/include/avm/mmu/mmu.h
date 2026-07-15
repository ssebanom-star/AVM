// ARMv8-A Stage 1 MMU (EL1&0 변환 레짐) + 소프트웨어 TLB.
//
// 범위 (Stage 3):
//  - 4KB 그래뉼, 다단계(레벨 0~3) 페이지 테이블 워크
//  - TTBR0(하위 반), TTBR1(상위 반), TCR.T0SZ/T1SZ/EPD0/EPD1/TG0/TG1
//  - 블록(1GB/2MB)과 페이지(4KB) 매핑
//  - AP[2:1] 권한, UXN/PXN, Access Flag (하드웨어 AF 갱신 없음 — ARMv8.0)
//  - EL0-쓰기 가능 페이지는 EL1 실행 금지 (아키텍처 규칙)
//  - 정밀 폴트: translation/permission/access-flag/주소 범위/워크 중 외부 중단
//    (FSC 인코딩은 ESR의 DFSC/IFSC 6비트 값 그대로)
//  - 소프트웨어 TLB: 명령어/데이터 분리, 직접 사상, 세대 번호 기반 무효화
//
// 후속: 16K/64K 그래뉼, ASID별 TLB, hierarchical AP/XN 테이블 속성,
// 호스트 포인터 직접 매핑 TLB(Stage 4 JIT 빠른 경로), HW AF/DBM.
#pragma once

#include "avm/cpu/cpu_state.h"
#include "avm/mem/phys_mem.h"
#include "avm/types.h"

namespace avm {

enum class AccessType : u8 { kFetch, kLoad, kStore };

// ESR.DFSC/IFSC 값 (6비트).
namespace fsc {
inline constexpr u32 kAddressSizeL0     = 0b000000;
inline constexpr u32 kTranslationL0     = 0b000100; // +level
inline constexpr u32 kAccessFlagL0      = 0b001000; // +level
inline constexpr u32 kPermissionL0      = 0b001100; // +level
inline constexpr u32 kSyncExternal      = 0b010000;
inline constexpr u32 kSyncExternalWalkL0 = 0b010100; // +level
inline constexpr u32 kAlignment         = 0b100001;

constexpr u32 Translation(unsigned level) { return kTranslationL0 + level; }
constexpr u32 AccessFlag(unsigned level)  { return kAccessFlagL0 + level; }
constexpr u32 Permission(unsigned level)  { return kPermissionL0 + level; }
constexpr u32 ExternalWalk(unsigned level) { return kSyncExternalWalkL0 + level; }
} // namespace fsc

struct MmuFault {
    bool valid = false;
    u32 fsc = 0; // DFSC/IFSC
    explicit operator bool() const { return valid; }

    static MmuFault Make(u32 fsc_value) { return MmuFault{true, fsc_value}; }
};

struct MmuStats {
    u64 tlb_hits = 0;
    u64 tlb_misses = 0;
    u64 walks = 0;
    u64 faults = 0;
};

class Mmu {
public:
    Mmu(CpuState& cpu, PhysMem& mem) : cpu_(cpu), mem_(mem) {}

    // SCTLR_EL1.M — EL1&0 레짐 Stage 1 변환 활성 여부.
    bool Enabled() const { return cpu_.sys.sctlr_el1 & 1; }

    // VA -> PA 변환. 현재 pstate.el(EL0/EL1) 기준으로 권한을 검사한다.
    // MMU가 꺼져 있으면 항등 변환. 실패 시 false + fault 채움.
    bool Translate(u64 va, AccessType type, MmuFault& fault, u64& pa);

    // TLBI 계열: 전체 무효화 (보수적 — ASID/VA 단위는 후속 최적화).
    void InvalidateTlb();

    const MmuStats& Stats() const { return stats_; }

private:
    // TLB 엔트리 권한 비트.
    enum PermBits : u8 {
        kPermR0 = 1 << 0,
        kPermW0 = 1 << 1,
        kPermX0 = 1 << 2,
        kPermR1 = 1 << 3,
        kPermW1 = 1 << 4,
        kPermX1 = 1 << 5,
    };

    struct TlbEntry {
        u64 vpage = ~0ull; // VA >> 12 (미사용 = ~0)
        u64 ppage = 0;     // PA의 페이지 베이스
        u8 perms = 0;
        u8 leaf_level = 3; // 리프 디스크립터 레벨 (권한 폴트 보고용)
    };
    static constexpr unsigned kTlbSize = 256; // 엔트리 수 (직접 사상)

    static u8 RequiredPerm(AccessType type, ExceptionLevel el);
    bool Walk(u64 va, MmuFault& fault, TlbEntry& entry);
    void CheckGeneration();

    CpuState& cpu_;
    PhysMem& mem_;
    u32 seen_generation_ = 0;
    TlbEntry itlb_[kTlbSize];
    TlbEntry dtlb_[kTlbSize];
    MmuStats stats_;
};

} // namespace avm
