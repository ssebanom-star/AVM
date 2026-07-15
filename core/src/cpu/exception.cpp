#include "avm/cpu/exception.h"

#include "avm/cpu/cpu_state.h"

namespace avm {

namespace {

// SPSR.M[4:0] 인코딩 (AArch64).
constexpr u64 kModeEl0t = 0b00000;
constexpr u64 kModeEl1t = 0b00100;
constexpr u64 kModeEl1h = 0b00101;

} // namespace

u64 BuildSpsr(const CpuState& cpu) {
    const Pstate& ps = cpu.pstate;
    u64 mode = kModeEl0t;
    if (ps.el == ExceptionLevel::EL1) {
        mode = ps.sp_sel ? kModeEl1h : kModeEl1t;
    }
    return (static_cast<u64>(ps.ToNzcvBits())) |
           (ps.d ? 1ull << 9 : 0) | (ps.a ? 1ull << 8 : 0) |
           (ps.i ? 1ull << 7 : 0) | (ps.f ? 1ull << 6 : 0) | mode;
}

bool ApplySpsr(CpuState& cpu, u64 spsr) {
    if (spsr & (1ull << 4)) return false; // AArch32 모드는 미지원
    Pstate& ps = cpu.pstate;
    ExceptionLevel el;
    bool sp_sel;
    switch (spsr & 0xF) {
        case kModeEl0t: el = ExceptionLevel::EL0; sp_sel = false; break;
        case kModeEl1t: el = ExceptionLevel::EL1; sp_sel = false; break;
        case kModeEl1h: el = ExceptionLevel::EL1; sp_sel = true; break;
        default: return false; // EL2/EL3 및 불법 모드
    }
    // 불법 복귀 검사: 현재 EL보다 높은 EL로는 복귀할 수 없다.
    if (static_cast<u8>(el) > static_cast<u8>(ps.el)) return false;

    ps.SetNzcvBits(static_cast<u32>(spsr));
    ps.d = spsr & (1ull << 9);
    ps.a = spsr & (1ull << 8);
    ps.i = spsr & (1ull << 7);
    ps.f = spsr & (1ull << 6);
    ps.el = el;
    ps.sp_sel = sp_sel;
    return true;
}

void TakeSyncException(CpuState& cpu, ExceptionClass ec, u32 iss, u64 far,
                       u64 preferred_return) {
    Pstate& ps = cpu.pstate;

    // 벡터 오프셋: 발생 위치 기준.
    //  - 같은 EL, SP_EL0 사용(EL1t): 0x000
    //  - 같은 EL, SP_ELx 사용(EL1h): 0x200
    //  - 하위 EL(AArch64, EL0t):     0x400
    u64 offset;
    if (ps.el == ExceptionLevel::EL0) {
        offset = 0x400;
    } else {
        offset = ps.sp_sel ? 0x200 : 0x000;
    }

    cpu.sys.spsr_el1 = BuildSpsr(cpu);
    cpu.sys.elr_el1 = preferred_return;
    cpu.sys.esr_el1 = BuildEsr(ec, iss);
    // FAR은 주소가 의미 있는 예외에서만 갱신된다.
    switch (ec) {
        case ExceptionClass::kInstructionAbortLower:
        case ExceptionClass::kInstructionAbort:
        case ExceptionClass::kDataAbortLower:
        case ExceptionClass::kDataAbort:
        case ExceptionClass::kPcAlignment:
        case ExceptionClass::kSpAlignment:
            cpu.sys.far_el1 = far;
            break;
        default:
            break;
    }

    ps.el = ExceptionLevel::EL1;
    ps.sp_sel = true;
    ps.d = ps.a = ps.i = ps.f = true; // DAIF 마스크

    cpu.pc = cpu.sys.vbar_el1 + offset;
}

} // namespace avm
