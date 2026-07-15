// avm-virt 최소 보드 통합 테스트 (Stage 2).
// 펌웨어 이미지가 플래시(ROM)에서 리셋 벡터부터 실제로 실행되어
// UART 출력, 예외 처리, WFI까지 도달하는지 검증한다.
#include <vector>

#include "avm/asm/assembler.h"
#include "avm/vm/test_firmware.h"
#include "avm/vm/virt_board.h"
#include "test_framework.h"

using namespace avm;
using namespace avm::asm64;

TEST(Board_ConstructsWithDefaultConfig) {
    VirtBoard board;
    CHECK(board.ok());
    // 리셋 상태: PC=플래시 시작, SP=RAM 끝, CNTFRQ 설정됨.
    CHECK_EQ(board.Cpu().pc, board::kFlashCodeBase);
    CHECK_EQ(board.Cpu().CurrentSp(),
             board::kRamBase + board.Config().ram_size);
    CHECK_EQ(board.Cpu().sys.cntfrq_el0, board::kTimerFreqHz);
}

TEST(Board_RejectsBadConfig) {
    VirtBoardConfig config;
    config.flash_size = board::kFlashCodeSize + kGuestPageSize; // 초과
    VirtBoard board(config);
    CHECK(!board.ok());

    VirtBoardConfig config2;
    config2.ram_size = 100; // 정렬 위반
    VirtBoard board2(config2);
    CHECK(!board2.ok());
}

TEST(Board_FirmwareBootsAndPrints) {
    VirtBoard board;
    CHECK(board.ok());
    const std::vector<u8> firmware = testfw::BuildTestFirmware();
    CHECK(board.LoadFirmware(firmware.data(), firmware.size()));

    const StopInfo stop = board.Run(100000);
    CHECK(stop.reason == StopReason::kWfi);
    // UART 출력: 배너 -> 핸들러 마커 -> 복귀 후 완료 메시지.
    CHECK(board.Uart().TxLog() == testfw::kExpectedUartOutput);
    // 핸들러가 읽은 ESR: EC=SVC, imm=kSvcNumber.
    CHECK_EQ(board.Cpu().x[10] >> 26, 0x15ull);
    CHECK_EQ(board.Cpu().x[10] & 0xFFFF, (u64)testfw::kSvcNumber);
    // FR 읽기: TXFE(bit7)=1.
    CHECK(board.Cpu().x[3] & (1u << 7));
    // 펌웨어는 ROM에서 실행되었다 (PC가 플래시 범위에서 정지).
    CHECK(stop.pc < board.Config().flash_size);
}

TEST(Board_ResetRunsFirmwareAgain) {
    VirtBoard board;
    const std::vector<u8> firmware = testfw::BuildTestFirmware();
    board.LoadFirmware(firmware.data(), firmware.size());

    CHECK(board.Run(100000).reason == StopReason::kWfi);
    board.Reset();
    CHECK_EQ(board.Cpu().pc, board::kFlashCodeBase);
    CHECK(board.Run(100000).reason == StopReason::kWfi);
    // 두 번 부팅 -> 출력 두 번 누적.
    const std::string expected = std::string(testfw::kExpectedUartOutput) +
                                 testfw::kExpectedUartOutput;
    CHECK(board.Uart().TxLog() == expected);
}

TEST(Board_RomWriteFaultsToVector) {
    // 플래시(ROM)에 쓰면 데이터 중단이 벡터로 전달된다.
    VirtBoard board;
    // 펌웨어: VBAR=0x800 설정, ROM(0x100)에 str -> 핸들러에서 ESR/FAR 읽고 WFI.
    const std::vector<u32> entry = {
        Movz(9, 0x800, 0),
        Msr(sysreg::kVbarEl1, 9),
        Movz(0, 0x100, 0),
        StrX(1, 0, 0),   // ROM 쓰기 -> Data Abort
        Nop(),           // 도달하면 안 됨
    };
    const std::vector<u32> handler = {
        Mrs(10, sysreg::kEsrEl1),
        Mrs(11, sysreg::kFarEl1),
        Wfi(),
    };
    std::vector<u8> image(0x800 + 0x200 + handler.size() * 4, 0);
    auto place = [&image](u64 off, const std::vector<u32>& ws) {
        for (u64 i = 0; i < ws.size(); ++i) {
            image[off + i * 4] = static_cast<u8>(ws[i]);
            image[off + i * 4 + 1] = static_cast<u8>(ws[i] >> 8);
            image[off + i * 4 + 2] = static_cast<u8>(ws[i] >> 16);
            image[off + i * 4 + 3] = static_cast<u8>(ws[i] >> 24);
        }
    };
    place(0, entry);
    place(0x800 + 0x200, handler);
    board.LoadFirmware(image.data(), image.size());

    const StopInfo stop = board.Run(1000);
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(board.Cpu().x[10] >> 26, 0x25ull);   // Data Abort (동일 EL)
    CHECK(board.Cpu().x[10] & (1 << 6));          // WnR
    CHECK_EQ(board.Cpu().x[11], 0x100ull);        // FAR
    // ELR = 폴트 명령어 (str) 위치.
    CHECK_EQ(board.Cpu().sys.elr_el1, 3ull * 4);
}

TEST(Board_UartRxVisibleToGuest) {
    VirtBoard board;
    // 펌웨어: RXFE가 0이 될 때까지 FR 폴링 -> DR 읽어 x5에 저장 -> WFI.
    //  0: movz x1, #0, lsl 0 ; movk x1, #0x0900, lsl 16
    //  8: poll: ldr w2, [x1, #0x18]
    // 12: ands wzr, w2, #0x10   (RXFE)
    // 16: b.ne poll(-8)
    // 20: ldr w5, [x1]          (DR)
    // 24: wfi
    const std::vector<u32> entry = {
        Movz(1, 0, 0),
        Movk(1, static_cast<u16>(board::kUart0Base >> 16), 1),
        LdrW(2, 1, 0x18),
        // tst w2, #0x10: ANDS wzr — logical imm N=0 immr=0 imms=... 0x10은
        // esize 32, 1비트 마스크 ror 28? 0x10 = 1<<4: imms=0(S=0), immr=28.
        LogicalImm(0b11, 31, 2, 0, 28, 0, false),
        BCond(1 /*NE*/, -8),
        LdrW(5, 1, 0),
        Wfi(),
    };
    std::vector<u8> image(entry.size() * 4, 0);
    for (u64 i = 0; i < entry.size(); ++i) {
        image[i * 4] = static_cast<u8>(entry[i]);
        image[i * 4 + 1] = static_cast<u8>(entry[i] >> 8);
        image[i * 4 + 2] = static_cast<u8>(entry[i] >> 16);
        image[i * 4 + 3] = static_cast<u8>(entry[i] >> 24);
    }
    board.LoadFirmware(image.data(), image.size());
    board.Uart().InjectInput("Z", 1);

    const StopInfo stop = board.Run(1000);
    CHECK(stop.reason == StopReason::kWfi);
    CHECK_EQ(board.Cpu().x[5], (u64)'Z');
}
