// 테스트 전용 ARM64 인코딩 빌더 (미니 어셈블러).
//
// 용도: 단위 테스트와 내장 셀프테스트가 게스트 테스트 바이너리를
// C++ 코드에서 직접 조립하기 위한 것. 프로덕션 JIT 백엔드(Stage 4)는
// 별도의 emitter를 가지며 이 헤더에 의존하지 않는다.
//
// 각 함수는 ARM ARM의 인코딩 다이어그램을 그대로 옮긴 것이며,
// tests/decoder_test.cpp에서 GNU as 출력과 대조한 상수로 교차 검증한다.
#pragma once

#include "avm/cpu/sysreg.h"
#include "avm/types.h"

namespace avm::asm64 {

// 레지스터 번호는 0..30, 31은 문맥에 따라 XZR 또는 SP.
inline constexpr unsigned kZr = 31;
inline constexpr unsigned kSp = 31;
inline constexpr unsigned kLr = 30;

constexpr u32 Nop() { return 0xD503201F; }

// MOVZ/MOVN/MOVK Rd, #imm16, LSL #(hw*16)
constexpr u32 MoveWide(unsigned opc, unsigned rd, u16 imm16, unsigned hw, bool is64) {
    return (is64 ? 1u << 31 : 0) | (opc << 29) | (0b100101u << 23) |
           (hw << 21) | (u32(imm16) << 5) | rd;
}
constexpr u32 Movn(unsigned rd, u16 imm16, unsigned hw = 0, bool is64 = true) {
    return MoveWide(0b00, rd, imm16, hw, is64);
}
constexpr u32 Movz(unsigned rd, u16 imm16, unsigned hw = 0, bool is64 = true) {
    return MoveWide(0b10, rd, imm16, hw, is64);
}
constexpr u32 Movk(unsigned rd, u16 imm16, unsigned hw = 0, bool is64 = true) {
    return MoveWide(0b11, rd, imm16, hw, is64);
}

// ADD/SUB (immediate). imm12는 0..4095, shift12는 LSL #12 적용 여부.
constexpr u32 AddSubImm(bool sub, bool set_flags, unsigned rd, unsigned rn,
                        unsigned imm12, bool shift12, bool is64) {
    return (is64 ? 1u << 31 : 0) | (sub ? 1u << 30 : 0) |
           (set_flags ? 1u << 29 : 0) | (0b100010u << 23) |
           (shift12 ? 1u << 22 : 0) | (imm12 << 10) | (rn << 5) | rd;
}
constexpr u32 AddImm(unsigned rd, unsigned rn, unsigned imm12, bool is64 = true) {
    return AddSubImm(false, false, rd, rn, imm12, false, is64);
}
constexpr u32 SubImm(unsigned rd, unsigned rn, unsigned imm12, bool is64 = true) {
    return AddSubImm(true, false, rd, rn, imm12, false, is64);
}
constexpr u32 AddsImm(unsigned rd, unsigned rn, unsigned imm12, bool is64 = true) {
    return AddSubImm(false, true, rd, rn, imm12, false, is64);
}
constexpr u32 SubsImm(unsigned rd, unsigned rn, unsigned imm12, bool is64 = true) {
    return AddSubImm(true, true, rd, rn, imm12, false, is64);
}
// CMP Xn, #imm == SUBS XZR, Xn, #imm
constexpr u32 CmpImm(unsigned rn, unsigned imm12, bool is64 = true) {
    return SubsImm(kZr, rn, imm12, is64);
}

// ADD/SUB (shifted register).
constexpr u32 AddSubReg(bool sub, bool set_flags, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift_type, unsigned shift_amount,
                        bool is64) {
    return (is64 ? 1u << 31 : 0) | (sub ? 1u << 30 : 0) |
           (set_flags ? 1u << 29 : 0) | (0b01011u << 24) |
           (shift_type << 22) | (rm << 16) | (shift_amount << 10) |
           (rn << 5) | rd;
}
constexpr u32 AddReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return AddSubReg(false, false, rd, rn, rm, 0, 0, is64);
}
constexpr u32 SubReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return AddSubReg(true, false, rd, rn, rm, 0, 0, is64);
}
constexpr u32 AddsReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return AddSubReg(false, true, rd, rn, rm, 0, 0, is64);
}
constexpr u32 SubsReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return AddSubReg(true, true, rd, rn, rm, 0, 0, is64);
}
constexpr u32 CmpReg(unsigned rn, unsigned rm, bool is64 = true) {
    return SubsReg(kZr, rn, rm, is64);
}

// 논리 (shifted register). opc: 00=AND 01=ORR 10=EOR 11=ANDS.
constexpr u32 LogicalReg(unsigned opc, unsigned rd, unsigned rn, unsigned rm,
                         bool invert, unsigned shift_type, unsigned shift_amount,
                         bool is64) {
    return (is64 ? 1u << 31 : 0) | (opc << 29) | (0b01010u << 24) |
           (shift_type << 22) | (invert ? 1u << 21 : 0) | (rm << 16) |
           (shift_amount << 10) | (rn << 5) | rd;
}
constexpr u32 AndReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return LogicalReg(0b00, rd, rn, rm, false, 0, 0, is64);
}
constexpr u32 OrrReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return LogicalReg(0b01, rd, rn, rm, false, 0, 0, is64);
}
constexpr u32 EorReg(unsigned rd, unsigned rn, unsigned rm, bool is64 = true) {
    return LogicalReg(0b10, rd, rn, rm, false, 0, 0, is64);
}
// MOV Xd, Xm == ORR Xd, XZR, Xm
constexpr u32 MovReg(unsigned rd, unsigned rm, bool is64 = true) {
    return OrrReg(rd, kZr, rm, is64);
}

// 논리 (immediate). n/immr/imms는 bitmask immediate 필드 원본.
constexpr u32 LogicalImm(unsigned opc, unsigned rd, unsigned rn, unsigned n,
                         unsigned immr, unsigned imms, bool is64) {
    return (is64 ? 1u << 31 : 0) | (opc << 29) | (0b100100u << 23) |
           (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd;
}

// LDR/STR (unsigned immediate). size_log2: 0=B 1=H 2=W 3=X.
// imm은 바이트 오프셋이며 size로 나누어 떨어져야 한다.
constexpr u32 LdrStrUnsigned(bool load, unsigned size_log2, unsigned rt,
                             unsigned rn, unsigned byte_offset) {
    const u32 imm12 = byte_offset >> size_log2;
    return (size_log2 << 30) | (0b111001u << 24) | ((load ? 1u : 0u) << 22) |
           (imm12 << 10) | (rn << 5) | rt;
}
constexpr u32 LdrX(unsigned rt, unsigned rn, unsigned byte_offset = 0) {
    return LdrStrUnsigned(true, 3, rt, rn, byte_offset);
}
constexpr u32 StrX(unsigned rt, unsigned rn, unsigned byte_offset = 0) {
    return LdrStrUnsigned(false, 3, rt, rn, byte_offset);
}
constexpr u32 LdrW(unsigned rt, unsigned rn, unsigned byte_offset = 0) {
    return LdrStrUnsigned(true, 2, rt, rn, byte_offset);
}
constexpr u32 StrW(unsigned rt, unsigned rn, unsigned byte_offset = 0) {
    return LdrStrUnsigned(false, 2, rt, rn, byte_offset);
}
constexpr u32 LdrB(unsigned rt, unsigned rn, unsigned byte_offset = 0) {
    return LdrStrUnsigned(true, 0, rt, rn, byte_offset);
}
constexpr u32 StrB(unsigned rt, unsigned rn, unsigned byte_offset = 0) {
    return LdrStrUnsigned(false, 0, rt, rn, byte_offset);
}

// LDR/STR (pre/post index, imm9: -256..255).
constexpr u32 LdrStrIndexed(bool load, bool pre, unsigned size_log2,
                            unsigned rt, unsigned rn, int imm9) {
    const u32 imm = static_cast<u32>(imm9) & 0x1FF;
    return (size_log2 << 30) | (0b111000u << 24) | ((load ? 1u : 0u) << 22) |
           (imm << 12) | ((pre ? 0b11u : 0b01u) << 10) | (rn << 5) | rt;
}
constexpr u32 StrXPre(unsigned rt, unsigned rn, int imm9) {
    return LdrStrIndexed(false, true, 3, rt, rn, imm9);
}
constexpr u32 LdrXPost(unsigned rt, unsigned rn, int imm9) {
    return LdrStrIndexed(true, false, 3, rt, rn, imm9);
}

// 분기. offset은 현재 명령어 기준 부호 있는 바이트 오프셋 (4의 배수).
constexpr u32 B(i64 byte_offset) {
    return 0x14000000u | (static_cast<u32>(byte_offset >> 2) & 0x03FFFFFF);
}
constexpr u32 Bl(i64 byte_offset) {
    return 0x94000000u | (static_cast<u32>(byte_offset >> 2) & 0x03FFFFFF);
}
// 조건 코드: 0=EQ 1=NE 2=CS 3=CC 4=MI 5=PL 6=VS 7=VC
//            8=HI 9=LS 10=GE 11=LT 12=GT 13=LE 14=AL
constexpr u32 BCond(unsigned cond, i64 byte_offset) {
    return 0x54000000u | ((static_cast<u32>(byte_offset >> 2) & 0x7FFFF) << 5) | cond;
}
constexpr u32 Cbz(unsigned rt, i64 byte_offset, bool is64 = true) {
    return (is64 ? 1u << 31 : 0) | 0x34000000u |
           ((static_cast<u32>(byte_offset >> 2) & 0x7FFFF) << 5) | rt;
}
constexpr u32 Cbnz(unsigned rt, i64 byte_offset, bool is64 = true) {
    return (is64 ? 1u << 31 : 0) | 0x35000000u |
           ((static_cast<u32>(byte_offset >> 2) & 0x7FFFF) << 5) | rt;
}
constexpr u32 Br(unsigned rn) { return 0xD61F0000u | (rn << 5); }
constexpr u32 Blr(unsigned rn) { return 0xD63F0000u | (rn << 5); }
constexpr u32 Ret(unsigned rn = kLr) { return 0xD65F0000u | (rn << 5); }

constexpr u32 Svc(u16 imm16 = 0) { return 0xD4000001u | (u32(imm16) << 5); }

// MRS/MSR (register). id는 sysreg::Id 패킹 (op0은 2 또는 3만 인코딩 가능).
constexpr u32 SysRegAccess(bool load, unsigned rt, u16 id) {
    return 0xD5000000u | ((load ? 1u : 0u) << 21) | (1u << 20) |
           ((sysreg::IdOp0(id) & 1u) << 19) | (sysreg::IdOp1(id) << 16) |
           (sysreg::IdCrn(id) << 12) | (sysreg::IdCrm(id) << 8) |
           (sysreg::IdOp2(id) << 5) | rt;
}
constexpr u32 Mrs(unsigned rt, u16 id) { return SysRegAccess(true, rt, id); }
constexpr u32 Msr(u16 id, unsigned rt) { return SysRegAccess(false, rt, id); }

// MSR (immediate): PSTATE 필드.
constexpr u32 MsrPstate(unsigned op1, unsigned op2, unsigned imm4) {
    return 0xD500401Fu | (op1 << 16) | (imm4 << 8) | (op2 << 5);
}
constexpr u32 MsrSpsel(unsigned imm)   { return MsrPstate(0b000, 0b101, imm); }
constexpr u32 MsrDaifSet(unsigned imm) { return MsrPstate(0b011, 0b110, imm); }
constexpr u32 MsrDaifClr(unsigned imm) { return MsrPstate(0b011, 0b111, imm); }

constexpr u32 Eret()  { return 0xD69F03E0u; }
constexpr u32 Wfi()   { return 0xD503207Fu; }
constexpr u32 Wfe()   { return 0xD503205Fu; }
constexpr u32 Yield() { return 0xD503203Fu; }
constexpr u32 DsbSy() { return 0xD5033F9Fu; }
constexpr u32 DmbSy() { return 0xD5033FBFu; }
constexpr u32 Isb()   { return 0xD5033FDFu; }

} // namespace avm::asm64
