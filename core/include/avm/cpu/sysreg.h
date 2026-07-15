// AArch64 시스템 레지스터 접근 (MRS/MSR).
//
// 시스템 레지스터는 (op0, op1, CRn, CRm, op2) 5-튜플로 식별되며,
// 이를 16비트 ID로 패킹해 디코더/인터프리터/JIT가 공유한다.
//
// 원칙:
//  - 구현하지 않은 레지스터 접근은 조용히 무시하지 않고 실패(false)를
//    반환한다. 실행 계층은 이를 미정의 명령어 예외로 처리한다.
//  - EL0에서 EL1 전용 레지스터 접근도 실패로 처리한다 (트랩 의미).
#pragma once

#include "avm/cpu/cpu_state.h"
#include "avm/types.h"

namespace avm::sysreg {

// (op0,op1,CRn,CRm,op2) -> 16비트 패킹.
// [15:14]=op0, [13:11]=op1, [10:7]=CRn, [6:3]=CRm, [2:0]=op2
constexpr u16 Id(unsigned op0, unsigned op1, unsigned crn, unsigned crm,
                 unsigned op2) {
    return static_cast<u16>((op0 << 14) | (op1 << 11) | (crn << 7) |
                            (crm << 3) | op2);
}

constexpr unsigned IdOp0(u16 id) { return (id >> 14) & 3; }
constexpr unsigned IdOp1(u16 id) { return (id >> 11) & 7; }
constexpr unsigned IdCrn(u16 id) { return (id >> 7) & 15; }
constexpr unsigned IdCrm(u16 id) { return (id >> 3) & 15; }
constexpr unsigned IdOp2(u16 id) { return id & 7; }

// --- 식별 ---------------------------------------------------------------
inline constexpr u16 kMidrEl1    = Id(3, 0, 0, 0, 0);
inline constexpr u16 kMpidrEl1   = Id(3, 0, 0, 0, 5);
inline constexpr u16 kRevidrEl1  = Id(3, 0, 0, 0, 6);
inline constexpr u16 kCurrentEl  = Id(3, 0, 4, 2, 2);

// --- 메모리 시스템 (Stage 3 MMU에서 의미 부여) -----------------------------
inline constexpr u16 kSctlrEl1   = Id(3, 0, 1, 0, 0);
inline constexpr u16 kCpacrEl1   = Id(3, 0, 1, 0, 2);
inline constexpr u16 kTtbr0El1   = Id(3, 0, 2, 0, 0);
inline constexpr u16 kTtbr1El1   = Id(3, 0, 2, 0, 1);
inline constexpr u16 kTcrEl1     = Id(3, 0, 2, 0, 2);
inline constexpr u16 kMairEl1    = Id(3, 0, 10, 2, 0);

// --- 예외 처리 ------------------------------------------------------------
inline constexpr u16 kVbarEl1    = Id(3, 0, 12, 0, 0);
inline constexpr u16 kEsrEl1     = Id(3, 0, 5, 2, 0);
inline constexpr u16 kFarEl1     = Id(3, 0, 6, 0, 0);
inline constexpr u16 kElrEl1     = Id(3, 0, 4, 0, 1);
inline constexpr u16 kSpsrEl1    = Id(3, 0, 4, 0, 0);
inline constexpr u16 kSpEl0      = Id(3, 0, 4, 1, 0);

// --- PSTATE 별칭 -----------------------------------------------------------
inline constexpr u16 kNzcv       = Id(3, 3, 4, 2, 0);
inline constexpr u16 kDaif       = Id(3, 3, 4, 2, 1);
inline constexpr u16 kSpSel      = Id(3, 0, 4, 2, 0);

// --- 스레드 ID -------------------------------------------------------------
inline constexpr u16 kTpidrEl0   = Id(3, 3, 13, 0, 2);
inline constexpr u16 kTpidrroEl0 = Id(3, 3, 13, 0, 3);
inline constexpr u16 kTpidrEl1   = Id(3, 0, 13, 0, 4);
inline constexpr u16 kContextidrEl1 = Id(3, 0, 13, 0, 1);

// --- Generic Timer ----------------------------------------------------------
inline constexpr u16 kCntfrqEl0  = Id(3, 3, 14, 0, 0);
inline constexpr u16 kCntpctEl0  = Id(3, 3, 14, 0, 1);
inline constexpr u16 kCntvctEl0  = Id(3, 3, 14, 0, 2);
inline constexpr u16 kCntpTvalEl0 = Id(3, 3, 14, 2, 0);
inline constexpr u16 kCntpCtlEl0  = Id(3, 3, 14, 2, 1);
inline constexpr u16 kCntpCvalEl0 = Id(3, 3, 14, 2, 2);
inline constexpr u16 kCntvTvalEl0 = Id(3, 3, 14, 3, 0);
inline constexpr u16 kCntvCtlEl0  = Id(3, 3, 14, 3, 1);
inline constexpr u16 kCntvCvalEl0 = Id(3, 3, 14, 3, 2);

// 게스트가 보는 카운터 값.
// Stage 2: 실행 명령어 수 기반의 결정적 카운터 (테스트 재현성).
// Stage 5: 호스트 단조 시계 기반으로 교체된다 (VM 일시 정지 보정 포함).
u64 CounterValue(const CpuState& cpu);

// MRS: 성공 시 true. 실패(미구현/권한 부족/쓰기 전용)는 미정의 명령어 예외로
// 처리해야 한다.
bool Read(CpuState& cpu, u16 id, u64& value);

// MSR (register): 성공 시 true. 읽기 전용/미구현/권한 부족이면 false.
bool Write(CpuState& cpu, u16 id, u64 value);

} // namespace avm::sysreg
