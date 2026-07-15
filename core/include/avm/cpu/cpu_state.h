// 가상 CPU(vCPU) 아키텍처 상태.
//
// 이 구조체는 인터프리터(Stage 1)와 JIT(Stage 4)가 공유하는 유일한
// 게스트 CPU 상태 정의다. JIT는 이 구조체의 고정 오프셋을 직접
// 참조하여 호스트 코드를 생성하므로, 필드 배치 변경 시 반드시
// 아래 static_assert 오프셋 검증을 함께 갱신한다.
#pragma once

#include <cstring>

#include "avm/types.h"

namespace avm {

// 예외 수준. Stage 1~7은 EL0/EL1 동작을 대상으로 한다.
// EL2/EL3는 UEFI 부팅 구조에 필요한 최소 범위만 모델링하며,
// 게스트가 그 이상을 요구하면 명확한 진단 오류를 낸다.
enum class ExceptionLevel : u8 {
    EL0 = 0,
    EL1 = 1,
    EL2 = 2,
    EL3 = 3,
};

// PSTATE 중 에뮬레이터가 추적하는 비트들.
struct Pstate {
    // NZCV 조건 플래그. JIT에서 지연 계산(lazy flags)으로 대체될 수 있으나
    // 인터프리터와 상태 동기화 시점에는 항상 이 필드가 정본이다.
    bool n = false;
    bool z = false;
    bool c = false;
    bool v = false;

    // DAIF 마스크 비트.
    bool d = true; // Debug
    bool a = true; // SError
    bool i = true; // IRQ
    bool f = true; // FIQ

    ExceptionLevel el = ExceptionLevel::EL1;
    bool sp_sel = true; // true: SP_ELx 사용(SPSel=1), false: SP_EL0 사용

    u32 ToNzcvBits() const {
        return (n ? 1u << 31 : 0) | (z ? 1u << 30 : 0) |
               (c ? 1u << 29 : 0) | (v ? 1u << 28 : 0);
    }
    void SetNzcvBits(u32 bits) {
        n = bits & (1u << 31);
        z = bits & (1u << 30);
        c = bits & (1u << 29);
        v = bits & (1u << 28);
    }
};

// AArch64 시스템 레지스터 (EL1 이하 우선).
// Stage 1에서는 저장 공간만 확보하고, MSR/MRS 디코딩(Stage 3)에서
// 실제 접근 의미가 부여된다.
struct SystemRegisters {
    // 식별 레지스터 (읽기 전용 성격).
    u64 midr_el1   = 0x410F'D4B0;      // 구현자 ARM, 임의 파트 번호
    u64 mpidr_el1  = 0x8000'0000;      // 단일 코어, Aff0=0
    u64 revidr_el1 = 0;

    // 메모리 시스템 제어 (Stage 3 MMU에서 사용).
    u64 sctlr_el1 = 0x00C5'0838;       // MMU off 리셋 값 근사
    u64 ttbr0_el1 = 0;
    u64 ttbr1_el1 = 0;
    u64 tcr_el1   = 0;
    u64 mair_el1  = 0;

    // 예외 처리.
    u64 vbar_el1 = 0;
    u64 esr_el1  = 0;
    u64 far_el1  = 0;
    u64 elr_el1  = 0;
    u64 spsr_el1 = 0;

    // 스레드/프로세스 ID.
    u64 tpidr_el0   = 0;
    u64 tpidrro_el0 = 0;
    u64 tpidr_el1   = 0;
    u64 contextidr_el1 = 0;

    // Generic Timer (Stage 5).
    u64 cntfrq_el0    = 0;
    u64 cntvoff_el2   = 0;
    u64 cntkctl_el1   = 0;
    u64 cntv_ctl_el0  = 0;
    u64 cntv_cval_el0 = 0;
    u64 cntp_ctl_el0  = 0;
    u64 cntp_cval_el0 = 0;
};

// 128비트 SIMD/FP 레지스터.
struct V128 {
    u64 lo = 0;
    u64 hi = 0;
};

// vCPU 아키텍처 상태 전체.
struct alignas(64) CpuState {
    // --- JIT 핫 필드: 구조체 선두에 고정 배치 -----------------------------
    u64 x[31] = {};   // X0..X30 (X30 = LR)
    u64 sp[4] = {};   // SP_EL0..SP_EL3 (Stage 1은 EL0/EL1만 사용)
    u64 pc = 0;

    Pstate pstate;

    // --- FP/SIMD ----------------------------------------------------------
    V128 v[32] = {};
    u64 fpcr = 0;
    u64 fpsr = 0;

    // --- 시스템 레지스터 ---------------------------------------------------
    SystemRegisters sys;

    // --- 실행 엔진 상태 ----------------------------------------------------
    bool wfi_pending = false;   // WFI로 잠든 상태 (Stage 5에서 타이머와 연동)
    bool irq_pending = false;   // 가상 GIC가 세우는 IRQ 라인 (Stage 5)
    bool fiq_pending = false;

    u64 executed_instructions = 0; // 통계: 실행된 게스트 명령어 수

    // --- 접근 헬퍼 ---------------------------------------------------------
    // Xn 읽기: n==31은 XZR(항상 0).
    u64 XZr(unsigned n) const { return n == 31 ? 0 : x[n]; }
    // Xn 쓰기: n==31은 XZR(무시).
    void SetXZr(unsigned n, u64 value) {
        if (n != 31) x[n] = value;
    }
    // Xn 읽기: n==31은 현재 SP (ADD/SUB immediate, 로드/스토어 베이스 등).
    u64 XSp(unsigned n) const { return n == 31 ? CurrentSp() : x[n]; }
    void SetXSp(unsigned n, u64 value) {
        if (n == 31) SetCurrentSp(value); else x[n] = value;
    }

    u64 CurrentSp() const {
        const unsigned idx = pstate.sp_sel ? static_cast<unsigned>(pstate.el) : 0;
        return sp[idx];
    }
    void SetCurrentSp(u64 value) {
        const unsigned idx = pstate.sp_sel ? static_cast<unsigned>(pstate.el) : 0;
        sp[idx] = value;
    }

    // 리셋: PC와 EL1 초기 상태로. entry는 리셋 벡터(펌웨어 시작 주소).
    void Reset(u64 entry) {
        std::memset(x, 0, sizeof(x));
        std::memset(sp, 0, sizeof(sp));
        pc = entry;
        pstate = Pstate{};
        for (auto& reg : v) reg = V128{};
        fpcr = 0;
        fpsr = 0;
        sys = SystemRegisters{};
        wfi_pending = false;
        irq_pending = false;
        fiq_pending = false;
        executed_instructions = 0;
    }
};

// JIT 백엔드가 의존하는 오프셋 고정 검증.
static_assert(offsetof(CpuState, x) == 0, "JIT는 x[]가 오프셋 0이라고 가정한다");
static_assert(offsetof(CpuState, sp) == 31 * 8, "sp[] 오프셋 고정");
static_assert(offsetof(CpuState, pc) == 35 * 8, "pc 오프셋 고정");

} // namespace avm
