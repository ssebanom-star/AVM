#include "avm/interp/interpreter.h"

#include "avm/cpu/sysreg.h"
#include "avm/log.h"

namespace avm {

namespace {
constexpr const char* kTag = "avm.interp";

StopInfo MakeStop(StopReason reason, u64 pc, u32 raw) {
    StopInfo info;
    info.reason = reason;
    info.pc = pc;
    info.raw_inst = raw;
    return info;
}
} // namespace

const char* StopReasonName(StopReason reason) {
    switch (reason) {
        case StopReason::kNone:            return "none";
        case StopReason::kSvc:             return "svc";
        case StopReason::kUndefinedInst:   return "undefined-instruction";
        case StopReason::kInstAbort:       return "instruction-abort";
        case StopReason::kDataAbort:       return "data-abort";
        case StopReason::kPcMisaligned:    return "pc-misaligned";
        case StopReason::kMaxInstructions: return "max-instructions";
        case StopReason::kWfi:             return "wfi";
        case StopReason::kInternalError:   return "internal-error";
    }
    return "?";
}

u64 Interpreter::AddWithCarry(u64 x, u64 y, bool carry_in, bool is64, bool set_flags) {
    if (!is64) {
        x &= 0xFFFFFFFFull;
        y &= 0xFFFFFFFFull;
        const u64 unsigned_sum = x + y + (carry_in ? 1 : 0);
        const u64 result = unsigned_sum & 0xFFFFFFFFull;
        if (set_flags) {
            cpu_.pstate.n = (result >> 31) & 1;
            cpu_.pstate.z = result == 0;
            cpu_.pstate.c = (unsigned_sum >> 32) != 0;
            cpu_.pstate.v = ((~(x ^ y) & (x ^ result)) >> 31) & 1;
        }
        return result;
    }
    const u64 result = x + y + (carry_in ? 1 : 0);
    if (set_flags) {
        const unsigned __int128 wide =
            (unsigned __int128)x + y + (carry_in ? 1 : 0);
        cpu_.pstate.c = (wide >> 64) != 0;
        cpu_.pstate.n = (result >> 63) & 1;
        cpu_.pstate.z = result == 0;
        cpu_.pstate.v = ((~(x ^ y) & (x ^ result)) >> 63) & 1;
    }
    return result;
}

u64 Interpreter::ApplyShift(u64 value, ShiftType type, unsigned amount, bool is64) const {
    const unsigned width = is64 ? 64 : 32;
    value &= MaskForWidth(is64);
    if (amount == 0) return value;
    switch (type) {
        case ShiftType::kLsl:
            return (value << amount) & MaskForWidth(is64);
        case ShiftType::kLsr:
            return value >> amount;
        case ShiftType::kAsr: {
            const u64 sign = value >> (width - 1);
            u64 result = value >> amount;
            if (sign) result |= MaskForWidth(is64) << (width - amount);
            return result & MaskForWidth(is64);
        }
        case ShiftType::kRor:
            return ((value >> amount) | (value << (width - amount))) & MaskForWidth(is64);
    }
    return value;
}

bool Interpreter::ConditionHolds(u8 cond) const {
    const Pstate& ps = cpu_.pstate;
    bool result;
    switch (cond >> 1) {
        case 0b000: result = ps.z; break;                      // EQ/NE
        case 0b001: result = ps.c; break;                      // CS/CC
        case 0b010: result = ps.n; break;                      // MI/PL
        case 0b011: result = ps.v; break;                      // VS/VC
        case 0b100: result = ps.c && !ps.z; break;             // HI/LS
        case 0b101: result = ps.n == ps.v; break;              // GE/LT
        case 0b110: result = ps.n == ps.v && !ps.z; break;     // GT/LE
        default:    result = true; break;                      // AL
    }
    if ((cond & 1) && cond != 0b1111) result = !result;
    return result;
}

StopInfo Interpreter::RaiseSync(ExceptionClass ec, u32 iss, u64 far,
                                u64 preferred_return, StopReason stop_reason,
                                u64 pc, u32 raw) {
    if (config_.guest_vectors) {
        TakeSyncException(cpu_, ec, iss, far, preferred_return);
        return MakeStop(StopReason::kNone, pc, raw);
    }
    return MakeStop(stop_reason, pc, raw);
}

StopInfo Interpreter::Step() {
    const GuestAddr pc = cpu_.pc;
    if ((pc & 0x3) != 0) {
        AVM_LOGD(kTag, "PC 정렬 위반: 0x%llx", (unsigned long long)pc);
        return RaiseSync(ExceptionClass::kPcAlignment, 0, pc, pc,
                         StopReason::kPcMisaligned, pc, 0);
    }
    u32 raw = 0;
    if (MemFault fault = mem_.FetchU32(pc, raw)) {
        const bool from_el0 = cpu_.pstate.el == ExceptionLevel::EL0;
        const auto ec = from_el0 ? ExceptionClass::kInstructionAbortLower
                                 : ExceptionClass::kInstructionAbort;
        // IFSC = 0b010000: 동기 외부 중단 (MMU 도입 전의 물리 접근 실패).
        StopInfo info = RaiseSync(ec, 0b010000, pc, pc,
                                  StopReason::kInstAbort, pc, 0);
        info.fault = fault;
        return info;
    }
    const DecodedInst inst = Decode(raw);
    return Execute(inst, pc);
}

StopInfo Interpreter::Run(u64 max_instructions) {
    for (u64 i = 0; i < max_instructions; ++i) {
        // 인터럽트 검사 지점: Stage 5에서 가상 GIC의 IRQ/FIQ 라인을
        // 여기서 샘플링해 예외 진입을 수행한다.
        if (cpu_.irq_pending || cpu_.fiq_pending) {
            // 아직 GIC/예외 벡터가 없으므로 도달하면 내부 오류다.
            return MakeStop(StopReason::kInternalError, cpu_.pc, 0);
        }
        StopInfo info = Step();
        if (info.reason != StopReason::kNone) return info;
    }
    return MakeStop(StopReason::kMaxInstructions, cpu_.pc, 0);
}

StopInfo Interpreter::Execute(const DecodedInst& inst, GuestAddr pc) {
    GuestAddr next_pc = pc + 4;
    StopInfo stop; // 기본: kNone (계속 실행)

    switch (inst.op) {
        case Op::kUndefined: {
            AVM_LOGD(kTag, "미정의 명령어 0x%08x @ 0x%llx", inst.raw,
                     (unsigned long long)pc);
            return RaiseSync(ExceptionClass::kUnknown, 0, cpu_.sys.far_el1, pc,
                             StopReason::kUndefinedInst, pc, inst.raw);
        }

        case Op::kNop:
            break;

        // --- 이동 (wide immediate) -----------------------------------------
        case Op::kMovz:
            cpu_.SetXZr(inst.rd, (inst.imm << inst.hw_shift) & MaskForWidth(inst.is64));
            break;
        case Op::kMovn:
            cpu_.SetXZr(inst.rd, ~(inst.imm << inst.hw_shift) & MaskForWidth(inst.is64));
            break;
        case Op::kMovk: {
            // MOVK: rd의 해당 16비트 필드만 교체. rd==31은 XZR이므로 무시.
            const u64 old = cpu_.XZr(inst.rd);
            const u64 mask = 0xFFFFull << inst.hw_shift;
            const u64 result = (old & ~mask) | (inst.imm << inst.hw_shift);
            cpu_.SetXZr(inst.rd, result & MaskForWidth(inst.is64));
            break;
        }

        // --- 산술 immediate: Rn/Rd는 SP를 참조할 수 있다 --------------------
        case Op::kAddImm:
        case Op::kSubImm: {
            const bool is_sub = inst.op == Op::kSubImm;
            const u64 operand1 = cpu_.XSp(inst.rn);
            const u64 operand2 = is_sub ? ~inst.imm : inst.imm;
            const u64 result = AddWithCarry(operand1, operand2, is_sub,
                                            inst.is64, inst.set_flags);
            // ADDS/SUBS의 Rd==31은 ZR(CMP/CMN), ADD/SUB의 Rd==31은 SP.
            if (inst.set_flags) cpu_.SetXZr(inst.rd, result);
            else cpu_.SetXSp(inst.rd, result);
            break;
        }

        // --- 산술 shifted register: Rn/Rm/Rd는 ZR --------------------------
        case Op::kAddReg:
        case Op::kSubReg: {
            const bool is_sub = inst.op == Op::kSubReg;
            const u64 operand1 = cpu_.XZr(inst.rn);
            u64 operand2 = ApplyShift(cpu_.XZr(inst.rm), inst.shift_type,
                                      inst.shift_amount, inst.is64);
            // SUB = operand1 + ~operand2 + 1. AddWithCarry가 폭 마스킹을
            // 수행하므로 ~는 64비트로 취해도 32비트 결과가 정확하다.
            if (is_sub) operand2 = ~operand2;
            const u64 result = AddWithCarry(operand1, operand2, is_sub,
                                            inst.is64, inst.set_flags);
            cpu_.SetXZr(inst.rd, result);
            break;
        }

        // --- 논리 ------------------------------------------------------------
        case Op::kAndReg:
        case Op::kOrrReg:
        case Op::kEorReg: {
            const u64 operand1 = cpu_.XZr(inst.rn);
            u64 operand2 = ApplyShift(cpu_.XZr(inst.rm), inst.shift_type,
                                      inst.shift_amount, inst.is64);
            if (inst.invert) operand2 = ~operand2 & MaskForWidth(inst.is64);
            u64 result;
            switch (inst.op) {
                case Op::kAndReg: result = operand1 & operand2; break;
                case Op::kOrrReg: result = operand1 | operand2; break;
                default:          result = operand1 ^ operand2; break;
            }
            result &= MaskForWidth(inst.is64);
            if (inst.set_flags) { // ANDS/BICS
                cpu_.pstate.n = (result >> (inst.is64 ? 63 : 31)) & 1;
                cpu_.pstate.z = result == 0;
                cpu_.pstate.c = false;
                cpu_.pstate.v = false;
            }
            cpu_.SetXZr(inst.rd, result);
            break;
        }
        case Op::kAndImm:
        case Op::kOrrImm:
        case Op::kEorImm: {
            const u64 operand1 = cpu_.XZr(inst.rn);
            u64 result;
            switch (inst.op) {
                case Op::kAndImm: result = operand1 & inst.imm; break;
                case Op::kOrrImm: result = operand1 | inst.imm; break;
                default:          result = operand1 ^ inst.imm; break;
            }
            result &= MaskForWidth(inst.is64);
            if (inst.set_flags) { // ANDS immediate: Rd==31이면 TST(ZR)
                cpu_.pstate.n = (result >> (inst.is64 ? 63 : 31)) & 1;
                cpu_.pstate.z = result == 0;
                cpu_.pstate.c = false;
                cpu_.pstate.v = false;
                cpu_.SetXZr(inst.rd, result);
            } else {
                // 논리 immediate의 Rd==31은 SP (예: AND sp, x0, #mask).
                cpu_.SetXSp(inst.rd, result);
            }
            break;
        }

        // --- 로드/스토어 ------------------------------------------------------
        case Op::kLdr:
        case Op::kStr: {
            const u64 base = cpu_.XSp(inst.rn);
            GuestAddr addr = base;
            if (inst.addr_mode != AddrMode::kPostIndex) {
                addr = base + static_cast<u64>(inst.mem_offset);
            }
            const unsigned size = 1u << inst.access_size_log2;
            MemFault fault;
            if (inst.op == Op::kLdr) {
                u64 value = 0;
                fault = mem_.Read(addr, &value, size); // 리틀 엔디언 zero-extend
                if (!fault) cpu_.SetXZr(inst.rt, value);
            } else {
                const u64 value = cpu_.XZr(inst.rt);
                fault = mem_.Write(addr, &value, size);
            }
            if (fault) {
                const bool from_el0 = cpu_.pstate.el == ExceptionLevel::EL0;
                const auto ec = from_el0 ? ExceptionClass::kDataAbortLower
                                         : ExceptionClass::kDataAbort;
                // ISS: WnR(bit6) | DFSC=0b010000 (동기 외부 중단 — MMU 이전).
                const u32 iss = (fault.is_write ? 1u << 6 : 0) | 0b010000;
                StopInfo info = RaiseSync(ec, iss, fault.addr, pc,
                                          StopReason::kDataAbort, pc, inst.raw);
                info.fault = fault;
                return info;
            }
            if (inst.addr_mode == AddrMode::kPostIndex) {
                cpu_.SetXSp(inst.rn, base + static_cast<u64>(inst.mem_offset));
            } else if (inst.addr_mode == AddrMode::kPreIndex) {
                cpu_.SetXSp(inst.rn, addr);
            }
            break;
        }

        // --- 분기 -------------------------------------------------------------
        case Op::kB:
            next_pc = pc + static_cast<u64>(inst.branch_offset);
            break;
        case Op::kBl:
            cpu_.x[30] = pc + 4;
            next_pc = pc + static_cast<u64>(inst.branch_offset);
            break;
        case Op::kBCond:
            if (ConditionHolds(inst.cond)) {
                next_pc = pc + static_cast<u64>(inst.branch_offset);
            }
            break;
        case Op::kBr:
            next_pc = cpu_.XZr(inst.rn);
            break;
        case Op::kBlr:
            next_pc = cpu_.XZr(inst.rn);
            cpu_.x[30] = pc + 4;
            break;
        case Op::kRet:
            next_pc = cpu_.XZr(inst.rn);
            break;
        case Op::kCbz:
        case Op::kCbnz: {
            const u64 value = cpu_.XZr(inst.rt) & MaskForWidth(inst.is64);
            const bool taken = (inst.op == Op::kCbz) ? (value == 0) : (value != 0);
            if (taken) next_pc = pc + static_cast<u64>(inst.branch_offset);
            break;
        }

        // --- 시스템 ------------------------------------------------------------
        case Op::kSvc: {
            cpu_.executed_instructions++;
            if (config_.guest_vectors) {
                // 아키텍처 경로: EL1 벡터로 진입. ELR = 다음 명령.
                TakeSyncException(cpu_, ExceptionClass::kSvc64,
                                  inst.sys_imm16, 0, pc + 4);
                return MakeStop(StopReason::kNone, pc, inst.raw);
            }
            // 하니스 모드: 정지 이벤트 (PC는 ELR 의미로 다음 명령).
            cpu_.pc = pc + 4;
            StopInfo info = MakeStop(StopReason::kSvc, pc, inst.raw);
            info.svc_imm = inst.sys_imm16;
            return info;
        }

        case Op::kMrs: {
            u64 value = 0;
            if (!sysreg::Read(cpu_, inst.sysreg, value)) {
                AVM_LOGD(kTag, "MRS 미구현/불허 sysreg=0x%04x @ 0x%llx",
                         inst.sysreg, (unsigned long long)pc);
                return RaiseSync(ExceptionClass::kUnknown, 0, 0, pc,
                                 StopReason::kUndefinedInst, pc, inst.raw);
            }
            cpu_.SetXZr(inst.rt, value);
            break;
        }
        case Op::kMsrReg: {
            if (!sysreg::Write(cpu_, inst.sysreg, cpu_.XZr(inst.rt))) {
                AVM_LOGD(kTag, "MSR 미구현/불허 sysreg=0x%04x @ 0x%llx",
                         inst.sysreg, (unsigned long long)pc);
                return RaiseSync(ExceptionClass::kUnknown, 0, 0, pc,
                                 StopReason::kUndefinedInst, pc, inst.raw);
            }
            break;
        }
        case Op::kMsrImm: {
            const u64 imm = inst.imm; // CRm 4비트
            switch (inst.pstate_field) {
                case PStateField::kSpSel:
                    // EL0에서 SPSel 변경은 UNDEFINED.
                    if (cpu_.pstate.el == ExceptionLevel::EL0) {
                        return RaiseSync(ExceptionClass::kUnknown, 0, 0, pc,
                                         StopReason::kUndefinedInst, pc,
                                         inst.raw);
                    }
                    cpu_.pstate.sp_sel = imm & 1;
                    break;
                case PStateField::kDaifSet:
                    if (imm & 0b1000) cpu_.pstate.d = true;
                    if (imm & 0b0100) cpu_.pstate.a = true;
                    if (imm & 0b0010) cpu_.pstate.i = true;
                    if (imm & 0b0001) cpu_.pstate.f = true;
                    break;
                case PStateField::kDaifClr:
                    if (imm & 0b1000) cpu_.pstate.d = false;
                    if (imm & 0b0100) cpu_.pstate.a = false;
                    if (imm & 0b0010) cpu_.pstate.i = false;
                    if (imm & 0b0001) cpu_.pstate.f = false;
                    break;
            }
            break;
        }
        case Op::kEret: {
            // EL0에서 ERET은 UNDEFINED.
            if (cpu_.pstate.el == ExceptionLevel::EL0) {
                return RaiseSync(ExceptionClass::kUnknown, 0, 0, pc,
                                 StopReason::kUndefinedInst, pc, inst.raw);
            }
            const u64 spsr = cpu_.sys.spsr_el1;
            const u64 elr = cpu_.sys.elr_el1;
            if (!ApplySpsr(cpu_, spsr)) {
                // 불법 예외 복귀 (AArch32/불법 모드/상위 EL 복귀 시도).
                AVM_LOGE(kTag, "불법 ERET: SPSR=0x%llx @ 0x%llx",
                         (unsigned long long)spsr, (unsigned long long)pc);
                return MakeStop(StopReason::kInternalError, pc, inst.raw);
            }
            next_pc = elr;
            break;
        }
        case Op::kWfi: {
            // 대기할 인터럽트 소스가 아직 없으므로(GIC는 Stage 5) 정지
            // 이벤트로 보고한다. PC는 WFI 다음 명령 — 재개 시 그 지점부터.
            cpu_.pc = pc + 4;
            cpu_.executed_instructions++;
            cpu_.wfi_pending = true;
            return MakeStop(StopReason::kWfi, pc, inst.raw);
        }
        case Op::kBarrier:
            // 단일 vCPU 인터프리터: 프로그램 순서 실행이므로 no-op 의미.
            break;
    }

    cpu_.pc = next_pc;
    cpu_.executed_instructions++;
    return stop;
}

} // namespace avm
