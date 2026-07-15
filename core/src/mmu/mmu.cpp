#include "avm/mmu/mmu.h"

#include "avm/log.h"

namespace avm {

namespace {
constexpr const char* kTag = "avm.mmu";

// 48비트 출력 주소 마스크 (디스크립터의 OA 필드).
constexpr u64 kOaMask = 0x0000'FFFF'FFFF'F000ull;
} // namespace

u8 Mmu::RequiredPerm(AccessType type, ExceptionLevel el) {
    const bool el0 = el == ExceptionLevel::EL0;
    switch (type) {
        case AccessType::kFetch: return el0 ? kPermX0 : kPermX1;
        case AccessType::kLoad:  return el0 ? kPermR0 : kPermR1;
        case AccessType::kStore: return el0 ? kPermW0 : kPermW1;
    }
    return 0;
}

void Mmu::CheckGeneration() {
    // MSR TTBRx/TCR/SCTLR/MAIR 및 TLBI가 CpuState.mmu_generation을 올린다.
    if (seen_generation_ != cpu_.mmu_generation) {
        InvalidateTlb();
        seen_generation_ = cpu_.mmu_generation;
    }
}

void Mmu::InvalidateTlb() {
    for (auto& entry : itlb_) entry = TlbEntry{};
    for (auto& entry : dtlb_) entry = TlbEntry{};
}

bool Mmu::Translate(u64 va, AccessType type, MmuFault& fault, u64& pa) {
    if (!Enabled()) {
        pa = va;
        return true;
    }
    CheckGeneration();

    const u64 vpage = va >> kGuestPageShift;
    TlbEntry* tlb = (type == AccessType::kFetch) ? itlb_ : dtlb_;
    TlbEntry& slot = tlb[vpage & (kTlbSize - 1)];
    const u8 need = RequiredPerm(type, cpu_.pstate.el);

    if (slot.vpage == vpage && (slot.perms & need)) {
        stats_.tlb_hits++;
        pa = slot.ppage | (va & kGuestPageMask);
        return true;
    }
    // 미스 또는 권한 불일치: 워크가 정본 (정확한 폴트 레벨 보고 포함).
    stats_.tlb_misses++;

    TlbEntry entry;
    if (!Walk(va, fault, entry)) {
        stats_.faults++;
        return false;
    }
    if (!(entry.perms & need)) {
        // 권한 폴트는 마지막 레벨(리프)에서 보고된다. Walk가 리프 레벨을
        // fault.fsc 초기값으로 남겨 두므로 여기서는 레벨 3/2/1 리프 정보를
        // entry에 함께 반환받아 사용한다.
        stats_.faults++;
        fault = MmuFault::Make(fsc::Permission(entry.leaf_level));
        return false;
    }
    slot = entry;
    pa = entry.ppage | (va & kGuestPageMask);
    return true;
}

bool Mmu::Walk(u64 va, MmuFault& fault, TlbEntry& entry) {
    stats_.walks++;
    const SystemRegisters& sys = cpu_.sys;
    const u64 tcr = sys.tcr_el1;
    const bool high = (va >> 55) & 1;

    const unsigned txsz =
        high ? static_cast<unsigned>((tcr >> 16) & 0x3F)
             : static_cast<unsigned>(tcr & 0x3F);
    // 4KB 그래뉼에서 지원하는 T*SZ 범위 (레벨 0~3 워크).
    if (txsz < 16 || txsz > 39) {
        fault = MmuFault::Make(fsc::Translation(0));
        return false;
    }
    const unsigned region_bits = 64 - txsz;

    // VA 범위 검사: 하위 반은 상위 비트가 전부 0, 상위 반은 전부 1.
    if (!high) {
        if (region_bits < 64 && (va >> region_bits) != 0) {
            fault = MmuFault::Make(fsc::Translation(0));
            return false;
        }
    } else {
        if ((~va) >> region_bits != 0) {
            fault = MmuFault::Make(fsc::Translation(0));
            return false;
        }
    }

    // EPD: 해당 반의 워크 비활성.
    const bool epd = high ? ((tcr >> 23) & 1) : ((tcr >> 7) & 1);
    if (epd) {
        fault = MmuFault::Make(fsc::Translation(0));
        return false;
    }
    // 그래뉼 검사: TG0=00(4KB), TG1=10(4KB)만 지원. 다른 값은 지원 제한을
    // 명확히 알리기 위해 translation fault로 처리한다 (16K/64K는 후속).
    if (!high && ((tcr >> 14) & 3) != 0) {
        AVM_LOGW(kTag, "TG0!=4KB는 미지원 (tcr=0x%llx)", (unsigned long long)tcr);
        fault = MmuFault::Make(fsc::Translation(0));
        return false;
    }
    if (high && ((tcr >> 30) & 3) != 2) {
        AVM_LOGW(kTag, "TG1!=4KB는 미지원 (tcr=0x%llx)", (unsigned long long)tcr);
        fault = MmuFault::Make(fsc::Translation(0));
        return false;
    }

    u64 table = (high ? sys.ttbr1_el1 : sys.ttbr0_el1) & kOaMask;
    // 필요한 레벨 수: 페이지 오프셋(12) 위의 비트를 레벨당 9비트씩 해석.
    const unsigned levels = (region_bits - 12 + 8) / 9;
    unsigned level = 4 - levels;

    for (;; ++level) {
        const unsigned shift = 12 + 9 * (3 - level);
        const u64 index = (va >> shift) & 0x1FF;
        u64 desc = 0;
        if (mem_.Read(table + index * 8, &desc, 8)) {
            fault = MmuFault::Make(fsc::ExternalWalk(level));
            return false;
        }
        if (!(desc & 1)) {
            fault = MmuFault::Make(fsc::Translation(level));
            return false;
        }
        if (level < 3 && (desc & 2)) {
            table = desc & kOaMask; // 다음 레벨 테이블
            continue;
        }
        // 리프 후보: 레벨 3은 페이지(bits[1:0]=11 필수),
        // 레벨 1/2는 블록(bits[1:0]=01), 레벨 0 블록은 불법.
        if (level == 3 && !(desc & 2)) {
            fault = MmuFault::Make(fsc::Translation(3));
            return false;
        }
        if (level == 0) {
            fault = MmuFault::Make(fsc::Translation(0));
            return false;
        }

        // Access Flag (하드웨어 갱신 없음 — 소프트웨어가 처리).
        if (!(desc & (1ull << 10))) {
            fault = MmuFault::Make(fsc::AccessFlag(level));
            return false;
        }

        const u64 block_mask = (1ull << shift) - 1;
        const u64 oa = (desc & kOaMask) & ~block_mask;

        // 권한 계산.
        const bool ap_el0 = desc & (1ull << 6); // AP[1]
        const bool ap_ro = desc & (1ull << 7);  // AP[2]
        const bool uxn = desc & (1ull << 54);
        const bool pxn = desc & (1ull << 53);

        u8 perms = kPermR1;
        if (!ap_ro) perms |= kPermW1;
        if (ap_el0) {
            perms |= kPermR0;
            if (!ap_ro) perms |= kPermW0;
            if (!uxn) perms |= kPermX0;
        }
        // EL1 실행: PXN이 없고, EL0-쓰기 가능이 아닐 것 (아키텍처 규칙:
        // 비특권 쓰기 가능 영역은 특권 실행 불가).
        const bool el0_writable = ap_el0 && !ap_ro;
        if (!pxn && !el0_writable) perms |= kPermX1;

        entry.vpage = va >> kGuestPageShift;
        entry.ppage = oa | ((va & ~kGuestPageMask) & block_mask);
        entry.perms = perms;
        entry.leaf_level = static_cast<u8>(level);
        return true;
    }
}

bool MemAccess(Mmu& mmu, PhysMem& mem, const CpuState& cpu, u64 va, void* buf,
               unsigned size, bool is_store, u32& fsc, u64& fault_va,
               MemFaultKind* kind_out) {
    // SCTLR_EL1.A: 정렬 검사 활성 시 비정렬 접근은 정렬 폴트.
    if ((cpu.sys.sctlr_el1 & 2) && (va & (size - 1)) != 0) {
        fsc = fsc::kAlignment;
        fault_va = va;
        if (kind_out) *kind_out = MemFaultKind::kPermission;
        return false;
    }
    const AccessType type = is_store ? AccessType::kStore : AccessType::kLoad;
    u8* cursor = static_cast<u8*>(buf);
    u64 cur_va = va;
    unsigned remaining = size;
    while (remaining > 0) {
        const unsigned in_page = static_cast<unsigned>(
            kGuestPageSize - (cur_va & kGuestPageMask));
        const unsigned chunk = remaining < in_page ? remaining : in_page;

        u64 pa = cur_va;
        MmuFault mmu_fault;
        if (!mmu.Translate(cur_va, type, mmu_fault, pa)) {
            fsc = mmu_fault.fsc;
            fault_va = cur_va;
            if (kind_out) *kind_out = MemFaultKind::kPermission;
            return false;
        }
        const MemFault fault = is_store ? mem.Write(pa, cursor, chunk)
                                        : mem.Read(pa, cursor, chunk);
        if (fault) {
            fsc = fsc::kSyncExternal;
            fault_va = cur_va;
            if (kind_out) *kind_out = fault.kind;
            return false;
        }
        cur_va += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

} // namespace avm
