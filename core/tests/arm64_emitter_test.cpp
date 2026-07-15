// 호스트 ARM64 이미터 인코딩 테스트.
// 방출 워드를 알려진 기계어 상수(GNU as / LLVM 출력)와 교차 검증한다.
// (실행 검증은 jit_exec_test.cpp에서 qemu로 수행)
#include "avm/jit/arm64_emitter.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::jit;

namespace {
u32 One(void (*build)(Arm64Emitter&)) {
    Arm64Emitter e;
    build(e);
    return e.Code()[0];
}
} // namespace

TEST(Emitter_MovesAndArith) {
    Arm64Emitter e;
    e.Movz(0, 1);                CHECK_EQ(e.Code()[0], 0xD2800020u); // mov x0,#1
    e.Clear(); e.Movk(0, 0x1234, 16); CHECK_EQ(e.Code()[0], 0xF2A24680u);
    e.Clear(); e.Movn(1, 0);     CHECK_EQ(e.Code()[0], 0x92800001u); // mov x1,#-1
    e.Clear(); e.MovReg(0, 1);   CHECK_EQ(e.Code()[0], 0xAA0103E0u); // mov x0,x1
    e.Clear(); e.Add(0, 1, 2);   CHECK_EQ(e.Code()[0], 0x8B020020u); // add x0,x1,x2
    e.Clear(); e.Sub(0, 1, 2);   CHECK_EQ(e.Code()[0], 0xCB020020u); // sub x0,x1,x2
    e.Clear(); e.Adds(0, 1, 2);  CHECK_EQ(e.Code()[0], 0xAB020020u); // adds x0,x1,x2
    e.Clear(); e.Subs(0, 1, 2);  CHECK_EQ(e.Code()[0], 0xEB020020u); // subs x0,x1,x2
    // add x0, x1, x2, lsl #4
    e.Clear(); e.Add(0, 1, 2, true, HShift::kLsl, 4);
    CHECK_EQ(e.Code()[0], 0x8B021020u);
    // 32-bit: add w0, w1, w2
    e.Clear(); e.Add(0, 1, 2, false);
    CHECK_EQ(e.Code()[0], 0x0B020020u);
}

TEST(Emitter_Logical) {
    Arm64Emitter e;
    e.And(0, 1, 2);  CHECK_EQ(e.Code()[0], 0x8A020020u); // and x0,x1,x2
    e.Clear(); e.Orr(0, 1, 2); CHECK_EQ(e.Code()[0], 0xAA020020u);
    e.Clear(); e.Eor(0, 1, 2); CHECK_EQ(e.Code()[0], 0xCA020020u);
    e.Clear(); e.Ands(0, 1, 2); CHECK_EQ(e.Code()[0], 0xEA020020u);
    // orr x9, xzr, x9, lsr #1  (operand shift path)
    e.Clear(); e.Orr(9, 31, 9, true, HShift::kLsr, 1);
    CHECK_EQ(e.Code()[0], 0xAA4907E9u);
}

TEST(Emitter_LoadStore) {
    Arm64Emitter e;
    e.LdrX(1, 0, 0);   CHECK_EQ(e.Code()[0], 0xF9400001u); // ldr x1,[x0]
    e.Clear(); e.StrX(0, 19, 8); CHECK_EQ(e.Code()[0], 0xF9000660u); // str x0,[x19,#8]
    e.Clear(); e.LdrX(2, 19, 240); CHECK_EQ(e.Code()[0], 0xF9407A62u); // ldr x2,[x19,#240]
    e.Clear(); e.Ldrb(9, 20, 4);  CHECK_EQ(e.Code()[0], 0x39401289u); // ldrb w9,[x20,#4]
    e.Clear(); e.StrW(0, 1, 0);   CHECK_EQ(e.Code()[0], 0xB9000020u); // str w0,[x1]
}

TEST(Emitter_StackPairs) {
    Arm64Emitter e;
    // stp x29,x30,[sp,#-48]!
    e.StpPre(29, 30, 31, -48); CHECK_EQ(e.Code()[0], 0xA9BD7BFDu);
    // stp x19,x20,[sp,#16]
    e.Clear(); e.StpOff(19, 20, 31, 16); CHECK_EQ(e.Code()[0], 0xA90153F3u);
    // ldp x21,x22,[sp,#32]
    e.Clear(); e.LdpOff(21, 22, 31, 32); CHECK_EQ(e.Code()[0], 0xA9425BF5u);
    // ldp x29,x30,[sp],#48
    e.Clear(); e.LdpPost(29, 30, 31, 48); CHECK_EQ(e.Code()[0], 0xA8C37BFDu);
}

TEST(Emitter_BranchesAndSystem) {
    Arm64Emitter e;
    e.Ret();         CHECK_EQ(e.Code()[0], 0xD65F03C0u);
    e.Clear(); e.Br(9);  CHECK_EQ(e.Code()[0], 0xD61F0120u);
    e.Clear(); e.Blr(16); CHECK_EQ(e.Code()[0], 0xD63F0200u);
    e.Clear(); e.MrsNzcv(11); CHECK_EQ(e.Code()[0], 0xD53B420Bu); // mrs x11,nzcv
    e.Clear(); e.MsrNzcv(0);  CHECK_EQ(e.Code()[0], 0xD51B4200u); // msr nzcv,x0
    e.Clear(); e.Nop();  CHECK_EQ(e.Code()[0], 0xD503201Fu);
    e.Clear(); e.Brk(1); CHECK_EQ(e.Code()[0], 0xD4200020u);
}

TEST(Emitter_MovConstSequences) {
    Arm64Emitter e;
    e.MovConst(0, 0);        CHECK_EQ(e.SizeWords(), 1u); // movz #0
    e.Clear(); e.MovConst(0, 0x1234);
    CHECK_EQ(e.SizeWords(), 1u);
    e.Clear(); e.MovConst(0, 0x1234'5678'9ABC'DEF0ull);
    CHECK_EQ(e.SizeWords(), 4u); // 4개 16비트 조각 모두 비영
    e.Clear(); e.MovConst(5, 0x4000'0000ull);
    CHECK_EQ(e.SizeWords(), 1u); // 상위 조각 하나
    CHECK_EQ(e.Code()[0], 0xD2A80005u); // movz x5,#0x4000,lsl#16
}

TEST(Emitter_BranchPatch) {
    Arm64Emitter e;
    const size_t br = e.B(0);   // placeholder
    e.Nop();
    e.Nop();
    const size_t target = e.Here();
    e.Ret();
    e.PatchBranch(br, target);
    // b .+12  => imm26 = 3
    CHECK_EQ(e.Code()[br], 0x14000003u);

    // 후방 분기.
    Arm64Emitter e2;
    const size_t t2 = e2.Here();
    e2.Nop();
    const size_t bc = e2.BCond(HCond::kEq, 0);
    e2.PatchBranch(bc, t2);
    // b.eq .-4 => imm19 = -1
    CHECK_EQ(e2.Code()[bc], 0x54FFFFE0u);
}
