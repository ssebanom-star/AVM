#include "avm/interp/interpreter.h"

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

StopInfo Interpreter::Step() {
    const GuestAddr pc = cpu_.pc;
    if ((pc & 0x3) != 0) {
        AVM_LOGD(kTag, "PC 정렬 위반: 0x%llx", (unsigned long long)pc);
        return MakeStop(StopReason::kPcMisaligned, pc, 0);
    }
    u32 raw = 0;
    if (MemFault fault = mem_.FetchU32(pc, raw)) {
        StopInfo info = MakeStop(StopReason::kInstAbort, pc, 0);
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
            return MakeStop(StopReason::kUndefinedInst, pc, inst.raw);
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
                StopInfo info = MakeStop(StopReason::kDataAbort, pc, inst.raw);
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
            // Stage 1: SVC는 실행 루프 정지 이벤트다. PC는 SVC 다음 명령을
            // 가리키게 하여(ELR 의미) Stage 2에서 벡터 진입으로 자연스럽게
            // 확장되도록 한다.
            cpu_.pc = pc + 4;
            cpu_.executed_instructions++;
            StopInfo info = MakeStop(StopReason::kSvc, pc, inst.raw);
            info.svc_imm = inst.sys_imm16;
            return info;
        }
    }

    cpu_.pc = next_pc;
    cpu_.executed_instructions++;
    return stop;
}

} // namespace avm
