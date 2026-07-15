// 디코더 단위 테스트.
// 미니 어셈블러의 인코딩을 GNU as / LLVM 출력에서 얻은 알려진 상수와
// 교차 검증한 뒤, 디코더가 각 필드를 정확히 복원하는지 확인한다.
#include "avm/asm/assembler.h"
#include "avm/decode/decoder.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;

// --- 어셈블러 교차 검증: 알려진 기계어 상수와 비교 -------------------------
TEST(Assembler_KnownEncodings) {
    CHECK_EQ(Nop(), 0xD503201Fu);                 // nop
    CHECK_EQ(Movz(0, 1), 0xD2800020u);            // mov x0, #1
    CHECK_EQ(Movk(0, 0x1234, 1), 0xF2A24680u);    // movk x0, #0x1234, lsl #16
    CHECK_EQ(AddImm(0, 1, 4), 0x91001020u);       // add x0, x1, #4
    CHECK_EQ(SubImm(kSp, kSp, 16), 0xD10043FFu);  // sub sp, sp, #16
    CHECK_EQ(AddReg(0, 1, 2), 0x8B020020u);       // add x0, x1, x2
    CHECK_EQ(CmpReg(0, 1), 0xEB01001Fu);          // cmp x0, x1
    CHECK_EQ(AndReg(0, 1, 2), 0x8A020020u);       // and x0, x1, x2
    CHECK_EQ(OrrReg(0, 1, 2), 0xAA020020u);       // orr x0, x1, x2
    CHECK_EQ(EorReg(0, 1, 2), 0xCA020020u);       // eor x0, x1, x2
    CHECK_EQ(MovReg(0, 1), 0xAA0103E0u);          // mov x0, x1
    CHECK_EQ(LdrX(1, 0), 0xF9400001u);            // ldr x1, [x0]
    CHECK_EQ(StrX(0, kSp, 8), 0xF90007E0u);       // str x0, [sp, #8]
    CHECK_EQ(B(16), 0x14000004u);                 // b .+16
    CHECK_EQ(Bl(4), 0x94000001u);                 // bl .+4
    CHECK_EQ(BCond(0, 8), 0x54000040u);           // b.eq .+8
    CHECK_EQ(Cbz(0, 8), 0xB4000040u);             // cbz x0, .+8
    CHECK_EQ(Br(1), 0xD61F0020u);                 // br x1
    CHECK_EQ(Ret(), 0xD65F03C0u);                 // ret
    CHECK_EQ(Svc(0), 0xD4000001u);                // svc #0
    // and x0, x1, #0xff (bitmask immediate: N=1 immr=0 imms=7)
    CHECK_EQ(LogicalImm(0b00, 0, 1, 1, 0, 7, true), 0x92401C20u);
}

TEST(Decoder_MoveWide) {
    DecodedInst inst = Decode(Movz(5, 0xABCD, 2));
    CHECK(inst.op == Op::kMovz);
    CHECK_EQ(inst.rd, 5);
    CHECK_EQ(inst.imm, 0xABCDull);
    CHECK_EQ(inst.hw_shift, 32);
    CHECK(inst.is64);

    inst = Decode(Movk(3, 0x1111, 1, /*is64=*/false));
    CHECK(inst.op == Op::kMovk);
    CHECK(!inst.is64);
    CHECK_EQ(inst.hw_shift, 16);

    // W형에서 hw>=2는 UNALLOCATED.
    inst = Decode(MoveWide(0b10, 0, 1, 2, /*is64=*/false));
    CHECK(inst.op == Op::kUndefined);
    // opc=01은 UNALLOCATED.
    inst = Decode(MoveWide(0b01, 0, 1, 0, true));
    CHECK(inst.op == Op::kUndefined);
}

TEST(Decoder_AddSubImm) {
    DecodedInst inst = Decode(AddImm(2, 3, 100));
    CHECK(inst.op == Op::kAddImm);
    CHECK(!inst.set_flags);
    CHECK_EQ(inst.rd, 2);
    CHECK_EQ(inst.rn, 3);
    CHECK_EQ(inst.imm, 100ull);

    inst = Decode(SubsImm(0, 1, 5, /*is64=*/false));
    CHECK(inst.op == Op::kSubImm);
    CHECK(inst.set_flags);
    CHECK(!inst.is64);

    // LSL #12 시프트.
    inst = Decode(AddSubImm(false, false, 0, 0, 1, /*shift12=*/true, true));
    CHECK_EQ(inst.imm, 0x1000ull);
}

TEST(Decoder_ShiftedRegister) {
    DecodedInst inst = Decode(AddSubReg(true, true, 1, 2, 3, 2 /*ASR*/, 7, true));
    CHECK(inst.op == Op::kSubReg);
    CHECK(inst.set_flags);
    CHECK(inst.shift_type == ShiftType::kAsr);
    CHECK_EQ(inst.shift_amount, 7);
    CHECK_EQ(inst.rm, 3);

    // add/sub에 ROR 시프트는 UNALLOCATED.
    inst = Decode(AddSubReg(false, false, 0, 0, 0, 3, 0, true));
    CHECK(inst.op == Op::kUndefined);
    // W형 시프트량 >= 32는 UNALLOCATED.
    inst = Decode(AddSubReg(false, false, 0, 0, 0, 0, 33, false));
    CHECK(inst.op == Op::kUndefined);
}

TEST(Decoder_Logical) {
    DecodedInst inst = Decode(LogicalReg(0b11, 4, 5, 6, false, 1, 3, true));
    CHECK(inst.op == Op::kAndReg); // ANDS
    CHECK(inst.set_flags);
    CHECK(inst.shift_type == ShiftType::kLsr);

    inst = Decode(LogicalReg(0b01, 0, 0, 1, /*invert=*/true, 0, 0, true));
    CHECK(inst.op == Op::kOrrReg); // ORN
    CHECK(inst.invert);

    // 논리 immediate: and x0, x1, #0xff
    inst = Decode(LogicalImm(0b00, 0, 1, 1, 0, 7, true));
    CHECK(inst.op == Op::kAndImm);
    CHECK_EQ(inst.imm, 0xFFull);

    // orr w0, w1, #0x0f0f0f0f — esize 8, S=3 -> N=0 immr=0 imms=0b110011.
    inst = Decode(LogicalImm(0b01, 0, 1, 0, 0, 0b110011, false));
    CHECK(inst.op == Op::kOrrImm);
    CHECK_EQ(inst.imm, 0x0F0F0F0Full);

    // 전부 1 마스크는 UNALLOCATED (imms 모든 비트 1).
    inst = Decode(LogicalImm(0b00, 0, 1, 1, 0, 0x3F, true));
    CHECK(inst.op == Op::kUndefined);
}

TEST(Decoder_LoadStore) {
    DecodedInst inst = Decode(LdrX(7, 8, 0x40));
    CHECK(inst.op == Op::kLdr);
    CHECK_EQ(inst.rt, 7);
    CHECK_EQ(inst.rn, 8);
    CHECK_EQ(inst.access_size_log2, 3);
    CHECK_EQ(inst.mem_offset, 0x40);
    CHECK(inst.addr_mode == AddrMode::kUnsignedOffset);

    inst = Decode(StrB(1, 2, 5));
    CHECK(inst.op == Op::kStr);
    CHECK_EQ(inst.access_size_log2, 0);
    CHECK_EQ(inst.mem_offset, 5);

    inst = Decode(StrXPre(0, kSp, -16));
    CHECK(inst.op == Op::kStr);
    CHECK(inst.addr_mode == AddrMode::kPreIndex);
    CHECK_EQ(inst.mem_offset, -16);

    inst = Decode(LdrXPost(0, 1, 8));
    CHECK(inst.addr_mode == AddrMode::kPostIndex);
    CHECK_EQ(inst.mem_offset, 8);
}

TEST(Decoder_Branches) {
    DecodedInst inst = Decode(B(-0x1000));
    CHECK(inst.op == Op::kB);
    CHECK_EQ(inst.branch_offset, -0x1000);

    inst = Decode(Bl(0x2000));
    CHECK(inst.op == Op::kBl);
    CHECK_EQ(inst.branch_offset, 0x2000);

    inst = Decode(BCond(10 /*GE*/, -8));
    CHECK(inst.op == Op::kBCond);
    CHECK_EQ(inst.cond, 10);
    CHECK_EQ(inst.branch_offset, -8);

    inst = Decode(Cbnz(9, 12, /*is64=*/false));
    CHECK(inst.op == Op::kCbnz);
    CHECK(!inst.is64);
    CHECK_EQ(inst.rt, 9);

    inst = Decode(Br(16));
    CHECK(inst.op == Op::kBr);
    CHECK_EQ(inst.rn, 16);

    inst = Decode(Blr(2));
    CHECK(inst.op == Op::kBlr);

    inst = Decode(Ret());
    CHECK(inst.op == Op::kRet);
    CHECK_EQ(inst.rn, 30);
}

TEST(Decoder_System) {
    DecodedInst inst = Decode(Svc(0x1234));
    CHECK(inst.op == Op::kSvc);
    CHECK_EQ(inst.sys_imm16, 0x1234);

    CHECK(Decode(Nop()).op == Op::kNop);
    // WFI는 실제 대기 의미를 가지므로 전용 op.
    CHECK(Decode(Wfi()).op == Op::kWfi);
    // WFE/YIELD/미할당 힌트는 아키텍처 NOP 동작.
    CHECK(Decode(Wfe()).op == Op::kNop);
    CHECK(Decode(Yield()).op == Op::kNop);
    // 배리어.
    CHECK(Decode(DsbSy()).op == Op::kBarrier);
    CHECK(Decode(DmbSy()).op == Op::kBarrier);
    CHECK(Decode(Isb()).op == Op::kBarrier);
    // ERET.
    CHECK(Decode(Eret()).op == Op::kEret);
    CHECK_EQ(Eret(), 0xD69F03E0u); // 알려진 인코딩
}

TEST(Decoder_SysRegAccess) {
    // 알려진 기계어와 교차 검증: mrs x0, midr_el1 / msr vbar_el1, x0
    CHECK_EQ(Mrs(0, sysreg::kMidrEl1), 0xD5380000u);
    CHECK_EQ(Msr(sysreg::kVbarEl1, 0), 0xD518C000u);
    CHECK_EQ(Mrs(0, sysreg::kCurrentEl), 0xD5384240u);
    CHECK_EQ(MsrDaifSet(2), 0xD50342DFu); // msr daifset, #2

    DecodedInst inst = Decode(Mrs(7, sysreg::kEsrEl1));
    CHECK(inst.op == Op::kMrs);
    CHECK_EQ(inst.rt, 7);
    CHECK_EQ(inst.sysreg, sysreg::kEsrEl1);

    inst = Decode(Msr(sysreg::kVbarEl1, 3));
    CHECK(inst.op == Op::kMsrReg);
    CHECK_EQ(inst.rt, 3);
    CHECK_EQ(inst.sysreg, sysreg::kVbarEl1);

    inst = Decode(MsrSpsel(1));
    CHECK(inst.op == Op::kMsrImm);
    CHECK(inst.pstate_field == PStateField::kSpSel);
    CHECK_EQ(inst.imm, 1ull);

    inst = Decode(MsrDaifClr(0xF));
    CHECK(inst.pstate_field == PStateField::kDaifClr);
    CHECK_EQ(inst.imm, 0xFull);
}

TEST(Decoder_UndefinedIsNotIgnored) {
    // 대표적 미구현/불법 인코딩이 반드시 Undefined로 나오는지.
    CHECK(Decode(0x00000000u).op == Op::kUndefined);
    CHECK(Decode(0xFFFFFFFFu).op == Op::kUndefined);
    CHECK(Decode(0x9B037C41u).op == Op::kUndefined); // madd (후속 단계)
    CHECK(Decode(0xA9BF7BFDu).op == Op::kUndefined); // stp (후속 단계)
    CHECK(Decode(0xD65F0FFFu).op == Op::kUndefined); // ret 인코딩 변형(불법)
    CHECK(Decode(0xD69F03E1u).op == Op::kUndefined); // eret 변형(불법)
    // SYS(캐시/TLB 관리, op0=01)는 Stage 3: tlbi vmalle1 = 0xD508871F
    CHECK(Decode(0xD508871Fu).op == Op::kUndefined);
}

TEST(Decoder_BitmaskImmediates) {
    u64 mask = 0;
    CHECK(DecodeBitMasks(1, 7, 0, true, mask));
    CHECK_EQ(mask, 0xFFull);
    CHECK(DecodeBitMasks(1, 0, 0, true, mask));
    CHECK_EQ(mask, 1ull);
    // 0xFFFF0000FFFF0000: esize 32, S=15, R=16 -> N=0 imms=0b001111 immr=16
    CHECK(DecodeBitMasks(0, 0b001111, 16, true, mask));
    CHECK_EQ(mask, 0xFFFF0000FFFF0000ull);
    // 불법: 전부 1.
    CHECK(!DecodeBitMasks(1, 0x3F, 0, true, mask));
    // W형에 N=1은 디코더에서 걸러지지만 알고리즘 수준에서도 esize>32 거부.
    CHECK(!DecodeBitMasks(1, 7, 0, false, mask));
}
