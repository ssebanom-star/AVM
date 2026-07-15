#include "avm/decode/decoder.h"

namespace avm {

namespace {

// welem을 esize 비트 폭 안에서 오른쪽으로 r만큼 회전.
u64 RotateRightInEsize(u64 value, unsigned r, unsigned esize) {
    if (r == 0) return value;
    const u64 mask = (esize == 64) ? ~0ull : ((1ull << esize) - 1);
    value &= mask;
    return ((value >> r) | (value << (esize - r))) & mask;
}

int HighestSetBit(u32 value) {
    for (int i = 31; i >= 0; --i) {
        if (value & (1u << i)) return i;
    }
    return -1;
}

} // namespace

bool DecodeBitMasks(unsigned n, unsigned imms, unsigned immr, bool is64, u64& wmask) {
    // ARM ARM의 aarch64/instrs/integer/bitmasks/DecodeBitMasks 알고리즘.
    const int len = HighestSetBit((n << 6) | (~imms & 0x3F));
    if (len < 1) return false; // 전부 1인 imms 등 UNALLOCATED
    const unsigned esize = 1u << len;
    if (!is64 && esize > 32) return false;

    const unsigned levels = esize - 1;
    const unsigned s = imms & levels;
    const unsigned r = immr & levels;
    if (s == levels) return false; // 모든 비트 1은 허용되지 않음

    u64 welem = (s + 1 == 64) ? ~0ull : ((1ull << (s + 1)) - 1);
    u64 elem = RotateRightInEsize(welem, r, esize);

    // esize 단위 패턴을 64비트로 복제.
    u64 result = 0;
    for (unsigned i = 0; i < 64; i += esize) {
        result |= elem << i;
        if (esize == 64) break;
    }
    wmask = is64 ? result : (result & 0xFFFFFFFFull);
    return true;
}

namespace {

DecodedInst Undefined(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.op = Op::kUndefined;
    return inst;
}

// --- Data processing (immediate) -------------------------------------------

DecodedInst DecodeMoveWide(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    const u32 opc = Bits32(raw, 30, 29);
    const u32 hw = Bits32(raw, 22, 21);
    inst.is64 = Bits32(raw, 31, 31);
    if (!inst.is64 && hw > 1) return Undefined(raw); // W형은 hw 0/1만
    switch (opc) {
        case 0b00: inst.op = Op::kMovn; break;
        case 0b10: inst.op = Op::kMovz; break;
        case 0b11: inst.op = Op::kMovk; break;
        default:   return Undefined(raw); // opc=01 UNALLOCATED
    }
    inst.imm = Bits32(raw, 20, 5);
    inst.hw_shift = static_cast<u8>(hw * 16);
    inst.rd = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

DecodedInst DecodeAddSubImm(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.is64 = Bits32(raw, 31, 31);
    const bool is_sub = Bits32(raw, 30, 30);
    inst.set_flags = Bits32(raw, 29, 29);
    const u32 shift = Bits32(raw, 22, 22);
    inst.op = is_sub ? Op::kSubImm : Op::kAddImm;
    inst.imm = static_cast<u64>(Bits32(raw, 21, 10)) << (shift ? 12 : 0);
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    inst.rd = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

DecodedInst DecodeLogicalImm(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.is64 = Bits32(raw, 31, 31);
    const u32 opc = Bits32(raw, 30, 29);
    const unsigned n = Bits32(raw, 22, 22);
    if (!inst.is64 && n != 0) return Undefined(raw);
    u64 mask = 0;
    if (!DecodeBitMasks(n, Bits32(raw, 15, 10), Bits32(raw, 21, 16), inst.is64, mask)) {
        return Undefined(raw);
    }
    switch (opc) {
        case 0b00: inst.op = Op::kAndImm; break;
        case 0b01: inst.op = Op::kOrrImm; break;
        case 0b10: inst.op = Op::kEorImm; break;
        case 0b11: inst.op = Op::kAndImm; inst.set_flags = true; break; // ANDS
    }
    inst.imm = mask;
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    inst.rd = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

// --- Data processing (register) ---------------------------------------------

DecodedInst DecodeAddSubShiftedReg(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.is64 = Bits32(raw, 31, 31);
    const bool is_sub = Bits32(raw, 30, 30);
    inst.set_flags = Bits32(raw, 29, 29);
    const u32 shift = Bits32(raw, 23, 22);
    const u32 imm6 = Bits32(raw, 15, 10);
    if (shift == 0b11) return Undefined(raw);          // ROR은 add/sub에 없음
    if (!inst.is64 && (imm6 & 0x20)) return Undefined(raw); // W형 시프트 >= 32
    inst.op = is_sub ? Op::kSubReg : Op::kAddReg;
    inst.shift_type = static_cast<ShiftType>(shift);
    inst.shift_amount = static_cast<u8>(imm6);
    inst.rm = static_cast<u8>(Bits32(raw, 20, 16));
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    inst.rd = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

DecodedInst DecodeLogicalShiftedReg(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.is64 = Bits32(raw, 31, 31);
    const u32 opc = Bits32(raw, 30, 29);
    const u32 shift = Bits32(raw, 23, 22);
    const u32 imm6 = Bits32(raw, 15, 10);
    if (!inst.is64 && (imm6 & 0x20)) return Undefined(raw);
    inst.invert = Bits32(raw, 21, 21); // N: BIC/ORN/EON/BICS
    switch (opc) {
        case 0b00: inst.op = Op::kAndReg; break;
        case 0b01: inst.op = Op::kOrrReg; break;
        case 0b10: inst.op = Op::kEorReg; break;
        case 0b11: inst.op = Op::kAndReg; inst.set_flags = true; break; // ANDS/BICS
    }
    inst.shift_type = static_cast<ShiftType>(shift);
    inst.shift_amount = static_cast<u8>(imm6);
    inst.rm = static_cast<u8>(Bits32(raw, 20, 16));
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    inst.rd = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

// --- Loads and stores --------------------------------------------------------

DecodedInst DecodeLoadStoreUnsignedImm(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    const u32 size = Bits32(raw, 31, 30);
    const u32 opc = Bits32(raw, 23, 22);
    // opc: 00=STR, 01=LDR (zero-extend). 부호 확장 로드(10/11)는 후속 단계.
    if (opc > 1) return Undefined(raw);
    inst.op = (opc == 1) ? Op::kLdr : Op::kStr;
    inst.access_size_log2 = static_cast<u8>(size);
    inst.is64 = (size == 3);
    inst.addr_mode = AddrMode::kUnsignedOffset;
    inst.mem_offset = static_cast<i64>(Bits32(raw, 21, 10)) << size;
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    inst.rt = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

DecodedInst DecodeLoadStoreImm9(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    const u32 size = Bits32(raw, 31, 30);
    const u32 opc = Bits32(raw, 23, 22);
    const u32 mode = Bits32(raw, 11, 10); // 01=post, 11=pre
    if (opc > 1) return Undefined(raw);
    if (mode != 0b01 && mode != 0b11) return Undefined(raw); // LDUR/unpriv은 후속
    inst.op = (opc == 1) ? Op::kLdr : Op::kStr;
    inst.access_size_log2 = static_cast<u8>(size);
    inst.is64 = (size == 3);
    inst.addr_mode = (mode == 0b01) ? AddrMode::kPostIndex : AddrMode::kPreIndex;
    inst.mem_offset = static_cast<i64>(SignExtend(Bits32(raw, 20, 12), 9));
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    inst.rt = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

// --- Branches / system --------------------------------------------------------

DecodedInst DecodeUncondBranchImm(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.op = Bits32(raw, 31, 31) ? Op::kBl : Op::kB;
    inst.branch_offset = static_cast<i64>(SignExtend(Bits32(raw, 25, 0), 26)) << 2;
    return inst;
}

DecodedInst DecodeCondBranch(u32 raw) {
    if (Bits32(raw, 4, 4) != 0 || Bits32(raw, 24, 24) != 0) return Undefined(raw);
    DecodedInst inst;
    inst.raw = raw;
    inst.op = Op::kBCond;
    inst.branch_offset = static_cast<i64>(SignExtend(Bits32(raw, 23, 5), 19)) << 2;
    inst.cond = static_cast<u8>(Bits32(raw, 3, 0));
    return inst;
}

DecodedInst DecodeCompareBranch(u32 raw) {
    DecodedInst inst;
    inst.raw = raw;
    inst.is64 = Bits32(raw, 31, 31);
    inst.op = Bits32(raw, 24, 24) ? Op::kCbnz : Op::kCbz;
    inst.branch_offset = static_cast<i64>(SignExtend(Bits32(raw, 23, 5), 19)) << 2;
    inst.rt = static_cast<u8>(Bits32(raw, 4, 0));
    return inst;
}

DecodedInst DecodeUncondBranchReg(u32 raw) {
    // 1101011 opc(4) 11111 000000 Rn(5) 00000
    if (Bits32(raw, 20, 16) != 0b11111 || Bits32(raw, 15, 10) != 0 ||
        Bits32(raw, 4, 0) != 0) {
        return Undefined(raw);
    }
    DecodedInst inst;
    inst.raw = raw;
    inst.rn = static_cast<u8>(Bits32(raw, 9, 5));
    switch (Bits32(raw, 24, 21)) {
        case 0b0000: inst.op = Op::kBr; break;
        case 0b0001: inst.op = Op::kBlr; break;
        case 0b0010: inst.op = Op::kRet; break;
        default:     return Undefined(raw); // ERET/DRPS는 후속 단계
    }
    return inst;
}

DecodedInst DecodeExceptionGen(u32 raw) {
    // 11010100 opc(3) imm16 op2(3) LL(2)
    const u32 opc = Bits32(raw, 23, 21);
    const u32 op2 = Bits32(raw, 4, 2);
    const u32 ll = Bits32(raw, 1, 0);
    if (opc == 0b000 && op2 == 0 && ll == 0b01) {
        DecodedInst inst;
        inst.raw = raw;
        inst.op = Op::kSvc;
        inst.sys_imm16 = static_cast<u16>(Bits32(raw, 20, 5));
        return inst;
    }
    return Undefined(raw); // HVC/SMC/BRK 등은 후속 단계
}

DecodedInst DecodeSystem(u32 raw) {
    // HINT 공간: NOP/YIELD/WFE/WFI/SEV/SEVL 및 미할당 힌트.
    // 아키텍처상 미할당 HINT는 NOP처럼 동작해야 하므로 kNop으로 디코딩한다.
    // (이는 "미지원 명령어를 NOP로 무시"하는 것이 아니라 아키텍처 정의를
    //  따르는 것이다. WFI/WFE의 실제 대기 의미는 Stage 5에서 부여된다.)
    if ((raw & 0xFFFFF01F) == 0xD503201F) {
        DecodedInst inst;
        inst.raw = raw;
        inst.op = Op::kNop;
        return inst;
    }
    return Undefined(raw); // MSR/MRS/배리어/캐시 관리는 Stage 3
}

} // namespace

DecodedInst Decode(u32 raw) {
    // AArch64 최상위 디코드: op0 = bits[28:25].
    const u32 op0 = Bits32(raw, 28, 25);
    switch (op0) {
        case 0b1000:
        case 0b1001: { // Data processing (immediate)
            const u32 op1 = Bits32(raw, 25, 23);
            switch (op1) {
                case 0b010: return DecodeAddSubImm(raw);       // add/sub immediate
                case 0b100: return DecodeLogicalImm(raw);      // logical immediate
                case 0b101: return DecodeMoveWide(raw);        // move wide immediate
                default:    return Undefined(raw); // PC-rel/bitfield/tags/extract 후속
            }
        }
        case 0b0101: { // Data processing (register): logical / add-sub shifted
            if (Bits32(raw, 24, 24) == 1) {
                if (Bits32(raw, 21, 21) == 0) return DecodeAddSubShiftedReg(raw);
                return Undefined(raw); // extended register 형식은 후속
            }
            return DecodeLogicalShiftedReg(raw);
        }
        case 0b1101: // ADC/CSEL/1~3소스 데이터 처리 등은 후속 단계
            return Undefined(raw);
        case 0b0100:
        case 0b0110:
        case 0b1100:
        case 0b1110: { // Loads and stores
            // 스칼라 정수 로드/스토어만: bits[29:28]=11, V(bit26)=0.
            if (Bits32(raw, 29, 28) == 0b11 && Bits32(raw, 26, 26) == 0) {
                if (Bits32(raw, 24, 24) == 1) return DecodeLoadStoreUnsignedImm(raw);
                if (Bits32(raw, 21, 21) == 0) return DecodeLoadStoreImm9(raw);
            }
            return Undefined(raw); // 페어/원자적/SIMD 로드스토어는 후속
        }
        case 0b1010:
        case 0b1011: { // Branches, exception generating and system
            const u32 top3 = Bits32(raw, 31, 29);
            if (Bits32(raw, 30, 26) == 0b00101) return DecodeUncondBranchImm(raw); // B/BL
            if (top3 == 0b010 && Bits32(raw, 28, 25) == 0b1010) {
                return DecodeCondBranch(raw);
            }
            if (Bits32(raw, 30, 25) == 0b011010) return DecodeCompareBranch(raw);
            if (top3 == 0b110) {
                if (Bits32(raw, 25, 25) == 1) return DecodeUncondBranchReg(raw);
                if (Bits32(raw, 25, 24) == 0b00 && Bits32(raw, 31, 24) == 0b11010100) {
                    return DecodeExceptionGen(raw);
                }
                if (Bits32(raw, 31, 22) == 0b1101010100) return DecodeSystem(raw);
            }
            return Undefined(raw); // TBZ/TBNZ 등 후속
        }
        default:
            return Undefined(raw);
    }
}

const char* OpName(Op op) {
    switch (op) {
        case Op::kUndefined: return "UDF";
        case Op::kNop:       return "NOP";
        case Op::kMovz:      return "MOVZ";
        case Op::kMovn:      return "MOVN";
        case Op::kMovk:      return "MOVK";
        case Op::kAddImm:    return "ADDimm";
        case Op::kSubImm:    return "SUBimm";
        case Op::kAddReg:    return "ADDreg";
        case Op::kSubReg:    return "SUBreg";
        case Op::kAndReg:    return "ANDreg";
        case Op::kOrrReg:    return "ORRreg";
        case Op::kEorReg:    return "EORreg";
        case Op::kAndImm:    return "ANDimm";
        case Op::kOrrImm:    return "ORRimm";
        case Op::kEorImm:    return "EORimm";
        case Op::kLdr:       return "LDR";
        case Op::kStr:       return "STR";
        case Op::kB:         return "B";
        case Op::kBl:        return "BL";
        case Op::kBCond:     return "B.cond";
        case Op::kBr:        return "BR";
        case Op::kBlr:       return "BLR";
        case Op::kRet:       return "RET";
        case Op::kCbz:       return "CBZ";
        case Op::kCbnz:      return "CBNZ";
        case Op::kSvc:       return "SVC";
    }
    return "?";
}

} // namespace avm
