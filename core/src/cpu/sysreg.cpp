#include "avm/cpu/sysreg.h"

namespace avm::sysreg {

namespace {

// EL0에서 접근이 허용되는 레지스터 집합.
// DAIF(SCTLR.UMA)와 EL0 타이머 접근(CNTKCTL_EL1)은 기본값(트랩)을 따른다.
bool El0Accessible(u16 id, bool is_write) {
    switch (id) {
        case kNzcv:
        case kTpidrEl0:
            return true;
        case kTpidrroEl0:
        case kCntfrqEl0:
        case kCntpctEl0:
        case kCntvctEl0:
        case kCtrEl0:
        case kDczidEl0:
            return !is_write; // EL0에서는 읽기만
        default:
            return false;
    }
}

u64 DaifBits(const Pstate& ps) {
    return (ps.d ? 1ull << 9 : 0) | (ps.a ? 1ull << 8 : 0) |
           (ps.i ? 1ull << 7 : 0) | (ps.f ? 1ull << 6 : 0);
}

void SetDaifBits(Pstate& ps, u64 value) {
    ps.d = value & (1ull << 9);
    ps.a = value & (1ull << 8);
    ps.i = value & (1ull << 7);
    ps.f = value & (1ull << 6);
}

// TVAL은 32비트 부호 있는 값으로 정의된다.
u64 TvalFromCval(u64 cval, u64 count) {
    return static_cast<u64>(static_cast<i64>(static_cast<i32>(
        static_cast<u32>(cval - count))));
}

u64 TimerCtlRead(u64 ctl, u64 cval, u64 count) {
    // ISTATUS(bit2)는 읽을 때 계산: ENABLE이고 카운터가 CVAL에 도달.
    const bool enable = ctl & 1;
    const bool istatus = enable && count >= cval;
    return (ctl & 0b011) | (istatus ? 0b100 : 0);
}

} // namespace

u64 CounterValue(const CpuState& cpu) {
    // 결정적 카운터: 실행된 명령어 수. Stage 5에서 호스트 단조 시계로 교체.
    return cpu.executed_instructions;
}

bool Read(CpuState& cpu, u16 id, u64& value) {
    if (cpu.pstate.el == ExceptionLevel::EL0 && !El0Accessible(id, false)) {
        return false;
    }
    SystemRegisters& sys = cpu.sys;
    switch (id) {
        case kMidrEl1:    value = sys.midr_el1; return true;
        case kMpidrEl1:   value = sys.mpidr_el1; return true;
        case kRevidrEl1:  value = sys.revidr_el1; return true;
        case kCurrentEl:  value = static_cast<u64>(cpu.pstate.el) << 2; return true;
        // CTR_EL0: 64B I/D 캐시 라인, PIPT (Cortex급 값).
        case kCtrEl0:     value = 0x8444C004ull; return true;
        // DCZID_EL0: DC ZVA 블록 = 2^4 워드 = 64바이트, DZP=0(허용).
        case kDczidEl0:   value = 0x4ull; return true;

        case kSctlrEl1:   value = sys.sctlr_el1; return true;
        case kCpacrEl1:   value = sys.cpacr_el1; return true;
        case kTtbr0El1:   value = sys.ttbr0_el1; return true;
        case kTtbr1El1:   value = sys.ttbr1_el1; return true;
        case kTcrEl1:     value = sys.tcr_el1; return true;
        case kMairEl1:    value = sys.mair_el1; return true;

        case kVbarEl1:    value = sys.vbar_el1; return true;
        case kEsrEl1:     value = sys.esr_el1; return true;
        case kFarEl1:     value = sys.far_el1; return true;
        case kElrEl1:     value = sys.elr_el1; return true;
        case kSpsrEl1:    value = sys.spsr_el1; return true;
        case kSpEl0:      value = cpu.sp[0]; return true;

        case kNzcv:       value = cpu.pstate.ToNzcvBits(); return true;
        case kDaif:       value = DaifBits(cpu.pstate); return true;
        case kSpSel:      value = cpu.pstate.sp_sel ? 1 : 0; return true;

        case kTpidrEl0:   value = sys.tpidr_el0; return true;
        case kTpidrroEl0: value = sys.tpidrro_el0; return true;
        case kTpidrEl1:   value = sys.tpidr_el1; return true;
        case kContextidrEl1: value = sys.contextidr_el1; return true;

        case kCntfrqEl0:  value = sys.cntfrq_el0; return true;
        case kCntpctEl0:  value = CounterValue(cpu); return true;
        case kCntvctEl0:  value = CounterValue(cpu) - sys.cntvoff_el2; return true;
        case kCntpCtlEl0:
            value = TimerCtlRead(sys.cntp_ctl_el0, sys.cntp_cval_el0,
                                 CounterValue(cpu));
            return true;
        case kCntpCvalEl0: value = sys.cntp_cval_el0; return true;
        case kCntpTvalEl0:
            value = TvalFromCval(sys.cntp_cval_el0, CounterValue(cpu));
            return true;
        case kCntvCtlEl0:
            value = TimerCtlRead(sys.cntv_ctl_el0, sys.cntv_cval_el0,
                                 CounterValue(cpu) - sys.cntvoff_el2);
            return true;
        case kCntvCvalEl0: value = sys.cntv_cval_el0; return true;
        case kCntvTvalEl0:
            value = TvalFromCval(sys.cntv_cval_el0,
                                 CounterValue(cpu) - sys.cntvoff_el2);
            return true;

        default:
            return false; // 미구현 레지스터: 미정의 명령어 예외
    }
}

bool Write(CpuState& cpu, u16 id, u64 value) {
    if (cpu.pstate.el == ExceptionLevel::EL0 && !El0Accessible(id, true)) {
        return false;
    }
    SystemRegisters& sys = cpu.sys;
    switch (id) {
        // 읽기 전용.
        case kMidrEl1:
        case kMpidrEl1:
        case kRevidrEl1:
        case kCurrentEl:
        case kCntpctEl0:
        case kCntvctEl0:
            return false;

        // MMU 관련 레지스터: 쓰기 시 TLB 세대를 올린다.
        case kSctlrEl1:
            sys.sctlr_el1 = value;
            cpu.mmu_generation++;
            return true;
        case kCpacrEl1:   sys.cpacr_el1 = value; return true;
        case kTtbr0El1:
            sys.ttbr0_el1 = value;
            cpu.mmu_generation++;
            return true;
        case kTtbr1El1:
            sys.ttbr1_el1 = value;
            cpu.mmu_generation++;
            return true;
        case kTcrEl1:
            sys.tcr_el1 = value;
            cpu.mmu_generation++;
            return true;
        case kMairEl1:
            sys.mair_el1 = value;
            cpu.mmu_generation++;
            return true;

        case kVbarEl1:    sys.vbar_el1 = value & ~0x7FFull; return true; // 2KB 정렬
        case kEsrEl1:     sys.esr_el1 = value; return true;
        case kFarEl1:     sys.far_el1 = value; return true;
        case kElrEl1:     sys.elr_el1 = value; return true;
        case kSpsrEl1:    sys.spsr_el1 = value; return true;
        case kSpEl0:      cpu.sp[0] = value; return true;

        case kNzcv:
            cpu.pstate.SetNzcvBits(static_cast<u32>(value));
            return true;
        case kDaif:       SetDaifBits(cpu.pstate, value); return true;
        case kSpSel:      cpu.pstate.sp_sel = value & 1; return true;

        case kTpidrEl0:   sys.tpidr_el0 = value; return true;
        case kTpidrroEl0: sys.tpidrro_el0 = value; return true;
        case kTpidrEl1:   sys.tpidr_el1 = value; return true;
        case kContextidrEl1: sys.contextidr_el1 = value; return true;

        case kCntfrqEl0:  sys.cntfrq_el0 = value; return true;
        case kCntpCtlEl0: sys.cntp_ctl_el0 = value & 0b11; return true;
        case kCntpCvalEl0: sys.cntp_cval_el0 = value; return true;
        case kCntpTvalEl0:
            sys.cntp_cval_el0 =
                CounterValue(cpu) + SignExtend(value & 0xFFFFFFFF, 32);
            return true;
        case kCntvCtlEl0: sys.cntv_ctl_el0 = value & 0b11; return true;
        case kCntvCvalEl0: sys.cntv_cval_el0 = value; return true;
        case kCntvTvalEl0:
            sys.cntv_cval_el0 = (CounterValue(cpu) - sys.cntvoff_el2) +
                                SignExtend(value & 0xFFFFFFFF, 32);
            return true;

        default:
            return false;
    }
}

} // namespace avm::sysreg
