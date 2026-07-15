#include "avm/jit/arm64_emitter.h"

namespace avm::jit {

void Arm64Emitter::Movz(unsigned rd, u16 imm16, unsigned shift, bool is64) {
    Emit((is64 ? 1u << 31 : 0) | (0b10u << 29) | (0b100101u << 23) |
         ((shift / 16) << 21) | (u32(imm16) << 5) | rd);
}
void Arm64Emitter::Movk(unsigned rd, u16 imm16, unsigned shift, bool is64) {
    Emit((is64 ? 1u << 31 : 0) | (0b11u << 29) | (0b100101u << 23) |
         ((shift / 16) << 21) | (u32(imm16) << 5) | rd);
}
void Arm64Emitter::Movn(unsigned rd, u16 imm16, unsigned shift, bool is64) {
    Emit((is64 ? 1u << 31 : 0) | (0b00u << 29) | (0b100101u << 23) |
         ((shift / 16) << 21) | (u32(imm16) << 5) | rd);
}

void Arm64Emitter::MovReg(unsigned rd, unsigned rm, bool is64) {
    Orr(rd, hostreg::kZr, rm, is64);
}

void Arm64Emitter::MovConst(unsigned rd, u64 value) {
    // 0이면 movz #0. 아니면 0이 아닌 16비트 조각마다 movz/movk.
    if (value == 0) {
        Movz(rd, 0);
        return;
    }
    bool first = true;
    for (unsigned shift = 0; shift < 64; shift += 16) {
        const u16 chunk = static_cast<u16>((value >> shift) & 0xFFFF);
        if (chunk == 0) continue;
        if (first) {
            Movz(rd, chunk, shift);
            first = false;
        } else {
            Movk(rd, chunk, shift);
        }
    }
}

void Arm64Emitter::EmitAddSub(unsigned rd, unsigned rn, unsigned rm, bool is64,
                              bool sub, bool set_flags, HShift shift,
                              unsigned amount) {
    Emit((is64 ? 1u << 31 : 0) | (sub ? 1u << 30 : 0) |
         (set_flags ? 1u << 29 : 0) | (0b01011u << 24) |
         (static_cast<u32>(shift) << 22) | (rm << 16) | (amount << 10) |
         (rn << 5) | rd);
}
void Arm64Emitter::Add(unsigned rd, unsigned rn, unsigned rm, bool is64,
                       HShift shift, unsigned amount) {
    EmitAddSub(rd, rn, rm, is64, false, false, shift, amount);
}
void Arm64Emitter::Sub(unsigned rd, unsigned rn, unsigned rm, bool is64,
                       HShift shift, unsigned amount) {
    EmitAddSub(rd, rn, rm, is64, true, false, shift, amount);
}
void Arm64Emitter::Adds(unsigned rd, unsigned rn, unsigned rm, bool is64,
                        HShift shift, unsigned amount) {
    EmitAddSub(rd, rn, rm, is64, false, true, shift, amount);
}
void Arm64Emitter::Subs(unsigned rd, unsigned rn, unsigned rm, bool is64,
                        HShift shift, unsigned amount) {
    EmitAddSub(rd, rn, rm, is64, true, true, shift, amount);
}

void Arm64Emitter::EmitLogical(unsigned op, unsigned rd, unsigned rn,
                               unsigned rm, bool is64, HShift shift,
                               unsigned amount) {
    // op: 00=AND 01=ORR 10=EOR 11=ANDS
    Emit((is64 ? 1u << 31 : 0) | (op << 29) | (0b01010u << 24) |
         (static_cast<u32>(shift) << 22) | (rm << 16) | (amount << 10) |
         (rn << 5) | rd);
}
void Arm64Emitter::And(unsigned rd, unsigned rn, unsigned rm, bool is64,
                       HShift shift, unsigned amount) {
    EmitLogical(0b00, rd, rn, rm, is64, shift, amount);
}
void Arm64Emitter::Orr(unsigned rd, unsigned rn, unsigned rm, bool is64,
                       HShift shift, unsigned amount) {
    EmitLogical(0b01, rd, rn, rm, is64, shift, amount);
}
void Arm64Emitter::Eor(unsigned rd, unsigned rn, unsigned rm, bool is64,
                       HShift shift, unsigned amount) {
    EmitLogical(0b10, rd, rn, rm, is64, shift, amount);
}
void Arm64Emitter::Ands(unsigned rd, unsigned rn, unsigned rm, bool is64,
                        HShift shift, unsigned amount) {
    EmitLogical(0b11, rd, rn, rm, is64, shift, amount);
}

void Arm64Emitter::EmitLoadStore(unsigned size, bool load, unsigned rt,
                                 unsigned rn, u32 byte_offset) {
    const u32 imm12 = byte_offset >> size;
    Emit((size << 30) | (0b111001u << 24) | ((load ? 1u : 0u) << 22) |
         (imm12 << 10) | (rn << 5) | rt);
}
void Arm64Emitter::LdrX(unsigned rt, unsigned rn, u32 off) {
    EmitLoadStore(3, true, rt, rn, off);
}
void Arm64Emitter::StrX(unsigned rt, unsigned rn, u32 off) {
    EmitLoadStore(3, false, rt, rn, off);
}
void Arm64Emitter::LdrW(unsigned rt, unsigned rn, u32 off) {
    EmitLoadStore(2, true, rt, rn, off);
}
void Arm64Emitter::StrW(unsigned rt, unsigned rn, u32 off) {
    EmitLoadStore(2, false, rt, rn, off);
}
void Arm64Emitter::Ldrb(unsigned rt, unsigned rn, u32 off) {
    EmitLoadStore(0, true, rt, rn, off);
}
void Arm64Emitter::Strb(unsigned rt, unsigned rn, u32 off) {
    EmitLoadStore(0, false, rt, rn, off);
}

void Arm64Emitter::StpPre(unsigned rt, unsigned rt2, unsigned rn, int imm) {
    const u32 imm7 = static_cast<u32>((imm >> 3) & 0x7F);
    // STP (64-bit, pre-index): 1010 1001 10 imm7 rt2 rn rt
    Emit(0xA9800000u | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}
void Arm64Emitter::LdpPost(unsigned rt, unsigned rt2, unsigned rn, int imm) {
    const u32 imm7 = static_cast<u32>((imm >> 3) & 0x7F);
    // LDP (64-bit, post-index): 1010 1000 11 imm7 rt2 rn rt
    Emit(0xA8C00000u | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}
void Arm64Emitter::StpOff(unsigned rt, unsigned rt2, unsigned rn, int imm) {
    const u32 imm7 = static_cast<u32>((imm >> 3) & 0x7F);
    // STP (64-bit, signed offset): 1010 1001 00 imm7 rt2 rn rt
    Emit(0xA9000000u | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}
void Arm64Emitter::LdpOff(unsigned rt, unsigned rt2, unsigned rn, int imm) {
    const u32 imm7 = static_cast<u32>((imm >> 3) & 0x7F);
    // LDP (64-bit, signed offset): 1010 1001 01 imm7 rt2 rn rt
    Emit(0xA9400000u | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}

size_t Arm64Emitter::B(i64 byte_offset) {
    const size_t at = code_.size();
    Emit(0x14000000u | (static_cast<u32>(byte_offset >> 2) & 0x03FFFFFF));
    return at;
}
size_t Arm64Emitter::BCond(HCond cond, i64 byte_offset) {
    const size_t at = code_.size();
    Emit(0x54000000u | ((static_cast<u32>(byte_offset >> 2) & 0x7FFFF) << 5) |
         static_cast<u32>(cond));
    return at;
}
size_t Arm64Emitter::Cbz(unsigned rt, i64 byte_offset, bool is64) {
    const size_t at = code_.size();
    Emit((is64 ? 1u << 31 : 0) | 0x34000000u |
         ((static_cast<u32>(byte_offset >> 2) & 0x7FFFF) << 5) | rt);
    return at;
}
size_t Arm64Emitter::Cbnz(unsigned rt, i64 byte_offset, bool is64) {
    const size_t at = code_.size();
    Emit((is64 ? 1u << 31 : 0) | 0x35000000u |
         ((static_cast<u32>(byte_offset >> 2) & 0x7FFFF) << 5) | rt);
    return at;
}
void Arm64Emitter::Br(unsigned rn) { Emit(0xD61F0000u | (rn << 5)); }
void Arm64Emitter::Blr(unsigned rn) { Emit(0xD63F0000u | (rn << 5)); }
void Arm64Emitter::Ret(unsigned rn) { Emit(0xD65F0000u | (rn << 5)); }

void Arm64Emitter::MrsNzcv(unsigned rt) {
    // MRS Xt, NZCV : 1101 0101 0011 0100 0010 0000 000 Rt
    Emit(0xD53B4200u | rt);
}
void Arm64Emitter::MsrNzcv(unsigned rt) {
    // MSR NZCV, Xt : 1101 0101 0001 0100 0010 0000 000 Rt
    Emit(0xD51B4200u | rt);
}
void Arm64Emitter::Nop() { Emit(0xD503201Fu); }
void Arm64Emitter::Brk(u16 imm) { Emit(0xD4200000u | (u32(imm) << 5)); }

void Arm64Emitter::PatchBranch(size_t branch_word_index, size_t target_word_index) {
    const i64 rel =
        (static_cast<i64>(target_word_index) - static_cast<i64>(branch_word_index));
    u32& word = code_[branch_word_index];
    const u32 op = word & 0xFF000000u;
    if ((word & 0x7C000000u) == 0x14000000u) {
        // B (imm26)
        word = (word & 0xFC000000u) | (static_cast<u32>(rel) & 0x03FFFFFF);
    } else {
        // B.cond / CBZ / CBNZ (imm19 at [23:5])
        word = (word & ~(0x7FFFFu << 5)) |
               ((static_cast<u32>(rel) & 0x7FFFF) << 5);
    }
    (void)op;
}

} // namespace avm::jit
