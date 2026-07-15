// 게스트 예외 모델.
//
// Stage 1: 예외를 "기록"하고 실행 루프를 정지시키는 것까지 구현한다.
// (테스트 하니스가 SVC를 종료 신호로 사용)
// Stage 2~3: VBAR_EL1 벡터로의 실제 예외 진입(ESR/FAR/ELR/SPSR 기록,
// EL 전환, ERET 복귀)이 이 모듈 위에 구현된다.
#pragma once

#include "avm/types.h"

namespace avm {

// ARMv8 ESR.EC (Exception Class) 중 현재 단계에서 사용하는 값.
enum class ExceptionClass : u32 {
    kUnknown          = 0x00, // 미정의 명령어 등
    kSvc64            = 0x15, // AArch64 SVC
    kInstructionAbort = 0x21, // 명령어 인출 중단 (동일 EL)
    kDataAbort        = 0x25, // 데이터 접근 중단 (동일 EL)
    kPcAlignment      = 0x22,
    kSpAlignment      = 0x26,
};

// 메모리 접근 실패 종류.
enum class MemFaultKind : u8 {
    kNone = 0,
    kUnmapped,     // 어떤 영역에도 속하지 않는 물리 주소
    kPermission,   // 페이지 권한 위반 (Stage 3에서 MMU와 함께 확장)
    kMmioError,    // 장치가 접근을 거부
};

// 메모리 접근 실패 상세 (FAR/ESR 구성에 사용).
struct MemFault {
    MemFaultKind kind = MemFaultKind::kNone;
    GuestAddr addr = 0;
    bool is_write = false;
    bool is_fetch = false;

    explicit operator bool() const { return kind != MemFaultKind::kNone; }
};

// 실행 루프가 정지한 이유.
enum class StopReason : u8 {
    kNone = 0,          // 정지하지 않음 (계속 실행)
    kSvc,               // SVC 실행 (Stage 1: 하니스 종료 신호)
    kUndefinedInst,     // 미지원/미정의 명령어 — NOP로 무시하지 않는다
    kInstAbort,         // 명령어 인출 실패
    kDataAbort,         // 데이터 접근 실패
    kPcMisaligned,      // PC 4바이트 정렬 위반
    kMaxInstructions,   // 요청한 명령어 수 실행 완료
    kWfi,               // WFI 진입 (Stage 5에서 타이머 이벤트와 연동)
    kInternalError,     // 엔진 내부 오류 (버그)
};

const char* StopReasonName(StopReason reason);

// 정지 시점의 진단 정보.
struct StopInfo {
    StopReason reason = StopReason::kNone;
    u64 pc = 0;          // 정지를 유발한 명령어의 PC
    u32 raw_inst = 0;    // 해당 명령어 원본 인코딩 (인출 실패 시 0)
    u16 svc_imm = 0;     // reason == kSvc일 때 SVC #imm16
    MemFault fault;      // 메모리 관련 정지의 상세
};

} // namespace avm
