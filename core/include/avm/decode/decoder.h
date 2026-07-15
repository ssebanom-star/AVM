// ARM64 (AArch64) 명령어 디코더.
//
// 구조: 32비트 인코딩 -> DecodedInst (정규화된 필드).
// 인터프리터(Stage 1)와 IR 생성기(Stage 4)가 같은 디코더를 공유한다.
//
// 원칙: 디코더가 인식하지 못하는 인코딩은 반드시 Op::kUndefined로
// 반환한다. 인터프리터/JIT는 이를 NOP로 무시하지 않고 미정의 명령어
// 예외를 발생시킨다.
#pragma once

#include "avm/types.h"

namespace avm {

enum class Op : u16 {
    kUndefined = 0,

    // 힌트 (HINT 공간 — 아키텍처상 NOP처럼 동작해야 하는 명령 포함).
    kNop,

    // 이동 (wide immediate).
    kMovz,
    kMovn,
    kMovk,

    // 산술 (immediate / shifted register). set_flags 필드로 ADDS/SUBS 구분.
    kAddImm,
    kSubImm,
    kAddReg,
    kSubReg,

    // 논리 (shifted register / immediate). invert=1이면 BIC/ORN/EON.
    kAndReg,
    kOrrReg,
    kEorReg,
    kAndImm,
    kOrrImm,
    kEorImm,

    // 로드/스토어 (스칼라 정수).
    kLdr,   // LDR/LDRH/LDRB (zero-extend)
    kStr,   // STR/STRH/STRB

    // 분기.
    kB,       // 무조건 직접 분기
    kBl,      // 직접 호출
    kBCond,   // 조건 분기
    kBr,      // 간접 분기
    kBlr,     // 간접 호출
    kRet,     // 반환
    kCbz,
    kCbnz,

    // 시스템.
    kSvc,
};

// 시프트 종류 (shifted register 형식).
enum class ShiftType : u8 { kLsl = 0, kLsr = 1, kAsr = 2, kRor = 3 };

// 로드/스토어 주소 지정 방식.
enum class AddrMode : u8 {
    kUnsignedOffset, // [Xn, #imm] (imm12 * scale)
    kPostIndex,      // [Xn], #imm9
    kPreIndex,       // [Xn, #imm9]!
};

struct DecodedInst {
    u32 raw = 0;
    Op op = Op::kUndefined;

    u8 rd = 0;   // 목적 레지스터 (분기: 사용 안 함)
    u8 rn = 0;   // 1번 소스 / 베이스 / 간접 분기 대상
    u8 rm = 0;   // 2번 소스
    u8 rt = 0;   // 로드/스토어/CBZ 대상 레지스터

    bool is64 = true;      // sf: 0=W(32비트), 1=X(64비트)
    bool set_flags = false; // ADDS/SUBS/ANDS
    bool invert = false;    // 논리 reg 형식의 N 비트 (BIC/ORN/EON)

    u64 imm = 0;            // 종합 immediate (이동/산술/논리 마스크)
    u8 hw_shift = 0;        // MOVZ/MOVN/MOVK: 16비트 단위 시프트 (0/16/32/48)

    ShiftType shift_type = ShiftType::kLsl;
    u8 shift_amount = 0;

    // 로드/스토어.
    AddrMode addr_mode = AddrMode::kUnsignedOffset;
    u8 access_size_log2 = 3; // 0=B,1=H,2=W,3=X
    i64 mem_offset = 0;      // 부호 있는 최종 바이트 오프셋

    // 분기.
    i64 branch_offset = 0;   // PC 상대 (바이트)
    u8 cond = 0;             // B.cond 조건 코드

    // 시스템.
    u16 sys_imm16 = 0;       // SVC #imm16
};

// 단일 명령어 디코딩. 실패 시 op == Op::kUndefined.
DecodedInst Decode(u32 raw);

// 논리 immediate(bitmask immediate) 디코딩.
// 성공 시 true를 반환하고 wmask에 결과 마스크를 채운다.
bool DecodeBitMasks(unsigned n, unsigned imms, unsigned immr, bool is64, u64& wmask);

const char* OpName(Op op);

} // namespace avm
