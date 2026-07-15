// 최소 테스트 펌웨어 (Stage 2).
//
// 직접 조립한 ARM64 펌웨어 이미지: 플래시(0x0)에서 리셋 -> VBAR 설정 ->
// UART로 배너 출력 -> SVC -> 예외 핸들러에서 마커 출력 -> ERET 복귀 ->
// 완료 메시지 출력 -> WFI.
//
// 명세 21절의 부팅 검증 순서 중 1~3단계(CPU 리셋, 펌웨어 첫 명령 실행,
// UART 출력)를 실제로 수행한다. board_test와 내장 셀프테스트(단말 APK)가
// 동일한 이미지를 사용한다.
#pragma once

#include <vector>

#include "avm/asm/assembler.h"
#include "avm/board.h"
#include "avm/types.h"

namespace avm::testfw {

// 펌웨어가 UART로 출력해야 하는 정확한 문자열.
inline constexpr const char* kExpectedUartOutput = "AVM:!OK\n";
// 펌웨어가 사용하는 SVC 번호.
inline constexpr u16 kSvcNumber = 7;
// 벡터 테이블의 플래시 내 오프셋 (2KB 정렬).
inline constexpr u64 kVbarOffset = 0x800;

// 테스트 펌웨어 이미지 생성.
inline std::vector<u8> BuildTestFirmware() {
    using namespace asm64;

    const u16 uart_hi = static_cast<u16>(board::kUart0Base >> 16); // 0x0900

    // 엔트리 (플래시 0x0, 리셋 벡터).
    const std::vector<u32> entry = {
        Movz(1, 0, 0),               // x1 = UART 베이스
        Movk(1, uart_hi, 1),
        Movz(9, static_cast<u16>(kVbarOffset), 0),
        Msr(sysreg::kVbarEl1, 9),    // 벡터 테이블 = 플래시 + 0x800
        Movz(2, 'A', 0), StrB(2, 1, 0),
        Movz(2, 'V', 0), StrB(2, 1, 0),
        Movz(2, 'M', 0), StrB(2, 1, 0),
        Movz(2, ':', 0), StrB(2, 1, 0),
        LdrW(3, 1, 0x18),            // x3 = UART FR (MMIO 읽기 검증)
        Svc(kSvcNumber),             // -> EL1h 동기 벡터
        Movz(2, 'O', 0), StrB(2, 1, 0),
        Movz(2, 'K', 0), StrB(2, 1, 0),
        Movz(2, '\n', 0), StrB(2, 1, 0),
        Wfi(),
    };

    // EL1h 동기 예외 핸들러 (VBAR + 0x200).
    const std::vector<u32> handler = {
        Mrs(10, sysreg::kEsrEl1),    // x10 = ESR (테스트가 검증)
        Movz(2, '!', 0), StrB(2, 1, 0),
        Eret(),
    };

    const u64 handler_offset = kVbarOffset + 0x200;
    std::vector<u8> image(handler_offset + handler.size() * 4, 0);
    // 채움 바이트 0x00000000은 미정의 명령어로 디코딩된다 — 잘못된 제어
    // 흐름이 조용히 지나가지 않고 예외를 일으키게 하는 의도적 트랩.

    auto place = [&image](u64 offset, const std::vector<u32>& words) {
        for (u64 i = 0; i < words.size(); ++i) {
            const u32 w = words[i];
            image[offset + i * 4 + 0] = static_cast<u8>(w);
            image[offset + i * 4 + 1] = static_cast<u8>(w >> 8);
            image[offset + i * 4 + 2] = static_cast<u8>(w >> 16);
            image[offset + i * 4 + 3] = static_cast<u8>(w >> 24);
        }
    };
    place(0, entry);
    place(handler_offset, handler);
    return image;
}

} // namespace avm::testfw
