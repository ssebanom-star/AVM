// 참조 인터프리터.
//
// 역할:
//  - Stage 1: 유일한 실행 엔진 (부팅 경로 초기 검증).
//  - Stage 4 이후: JIT 정확성 검증(검증 모드), 미구현 명령어 폴백,
//    디버깅(단일 스텝/중단점) 전용.
//
// 정상 실행 경로는 Stage 4부터 JIT 기반 동적 바이너리 변환이 담당한다.
// 따라서 이 인터프리터는 "빠름"보다 "아키텍처 정의에 정확히 일치"를
// 우선한다.
#pragma once

#include "avm/cpu/cpu_state.h"
#include "avm/cpu/exception.h"
#include "avm/decode/decoder.h"
#include "avm/mem/phys_mem.h"

namespace avm {

// 실행 모드 설정.
struct ExecConfig {
    // false: 하니스 모드 — SVC/미정의/중단에서 실행을 정지하고 StopInfo로
    //        보고한다 (단위 테스트, 디버깅, 검증 모드).
    // true : 아키텍처 모드 — 동기 예외를 VBAR_EL1 벡터로 전달한다
    //        (펌웨어/운영체제 실행의 기본값).
    bool guest_vectors = false;
};

class Interpreter {
public:
    Interpreter(CpuState& cpu, PhysMem& mem, ExecConfig config = {})
        : cpu_(cpu), mem_(mem), config_(config) {}

    // 명령어 1개 실행. 계속 실행 가능하면 reason == kNone.
    StopInfo Step();

    // 최대 max_instructions개 실행. 정지 사유가 생기면 즉시 반환.
    // 실행 루프의 인터럽트 검사 지점은 이 함수 안에 있다 (Stage 5에서
    // 가상 GIC의 IRQ 라인을 이 지점에서 샘플링한다).
    StopInfo Run(u64 max_instructions);

private:
    StopInfo Execute(const DecodedInst& inst, GuestAddr pc);

    // 동기 예외 발생 지점 공통 처리:
    // guest_vectors면 VBAR 벡터로 진입하고 kNone(계속 실행)을 반환,
    // 아니면 stop_reason으로 정지한다.
    StopInfo RaiseSync(ExceptionClass ec, u32 iss, u64 far, u64 preferred_return,
                       StopReason stop_reason, u64 pc, u32 raw);

    // NZCV를 갱신하는 덧셈 (SUB는 ~y, carry=1로 호출).
    u64 AddWithCarry(u64 x, u64 y, bool carry_in, bool is64, bool set_flags);
    u64 ApplyShift(u64 value, ShiftType type, unsigned amount, bool is64) const;
    bool ConditionHolds(u8 cond) const;

    CpuState& cpu_;
    PhysMem& mem_;
    ExecConfig config_;
};

} // namespace avm
