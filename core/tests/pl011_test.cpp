// PL011 UART 장치 단위 테스트.
#include "avm/board.h"
#include "avm/dev/pl011.h"
#include "test_framework.h"

using namespace avm;

namespace {

struct UartFixture {
    PhysMem mem;
    Pl011 uart;
    UartFixture() { mem.AddDevice(board::kUart0Base, board::kUart0Size, &uart); }

    u32 Read32(u64 offset) {
        u32 value = 0;
        mem.ReadT(board::kUart0Base + offset, value);
        return value;
    }
    void Write32(u64 offset, u32 value) {
        mem.WriteT<u32>(board::kUart0Base + offset, value);
    }
};

constexpr u64 kDr = 0x00;
constexpr u64 kFr = 0x18;
constexpr u64 kImsc = 0x38;
constexpr u64 kRis = 0x3C;
constexpr u64 kMis = 0x40;

} // namespace

TEST(Pl011_TxCapturesBytes) {
    UartFixture f;
    f.Write32(kDr, 'H');
    f.Write32(kDr, 'i');
    f.Write32(kDr, '\n');
    CHECK(f.uart.TxLog() == "Hi\n");
    // TX FIFO는 항상 비어 있음 (TXFE=1, TXFF=0).
    const u32 fr = f.Read32(kFr);
    CHECK(fr & (1u << 7));
    CHECK(!(fr & (1u << 5)));
}

TEST(Pl011_RxFifoAndFlags) {
    UartFixture f;
    // 비어 있을 때 RXFE=1.
    CHECK(f.Read32(kFr) & (1u << 4));
    f.uart.InjectInput("ab", 2);
    CHECK(!(f.Read32(kFr) & (1u << 4)));
    CHECK_EQ(f.Read32(kDr), (u32)'a');
    CHECK_EQ(f.Read32(kDr), (u32)'b');
    CHECK(f.Read32(kFr) & (1u << 4)); // 다시 비어 있음
}

TEST(Pl011_RxFifoDepthLimit) {
    UartFixture f;
    // 16바이트 초과분은 폐기.
    f.uart.InjectInput("0123456789abcdefXYZ", 19);
    CHECK(f.Read32(kFr) & (1u << 6)); // RXFF
    for (int i = 0; i < 16; ++i) f.Read32(kDr);
    CHECK(f.Read32(kFr) & (1u << 4)); // 정확히 16개만 있었음
}

TEST(Pl011_InterruptMaskAndLevel) {
    UartFixture f;
    int transitions = 0;
    bool level = false;
    f.uart.SetIrqCallback([&](bool l) { ++transitions; level = l; });

    // RIS: TX는 항상 준비(bit5). 마스크가 0이므로 MIS=0, IRQ 없음.
    CHECK(f.Read32(kRis) & (1u << 5));
    CHECK_EQ(f.Read32(kMis), 0u);
    CHECK(!f.uart.IrqLevel());

    // RX 인터럽트만 언마스크.
    f.Write32(kImsc, 1u << 4);
    CHECK_EQ(transitions, 0); // RX 비어 있음 -> 레벨 그대로

    f.uart.InjectInput("x", 1);
    CHECK_EQ(transitions, 1);
    CHECK(level);
    CHECK(f.Read32(kMis) & (1u << 4));

    // DR 읽어 FIFO 비우면 레벨 하강.
    f.Read32(kDr);
    CHECK_EQ(transitions, 2);
    CHECK(!level);
}

TEST(Pl011_IdRegisters) {
    UartFixture f;
    // PL011 PeriphID/CellID (Linux amba 버스 프로브가 확인).
    CHECK_EQ(f.Read32(0xFE0), 0x11u);
    CHECK_EQ(f.Read32(0xFE4), 0x10u);
    CHECK_EQ(f.Read32(0xFE8), 0x14u);
    CHECK_EQ(f.Read32(0xFEC), 0x00u);
    CHECK_EQ(f.Read32(0xFF0), 0x0Du);
    CHECK_EQ(f.Read32(0xFF4), 0xF0u);
    CHECK_EQ(f.Read32(0xFF8), 0x05u);
    CHECK_EQ(f.Read32(0xFFC), 0xB1u);
}

TEST(Pl011_ConfigRegistersRoundTrip) {
    UartFixture f;
    f.Write32(0x24, 0x10);  // IBRD
    f.Write32(0x28, 0x3B);  // FBRD
    f.Write32(0x2C, 0x70);  // LCR_H: 8N1 FIFO
    f.Write32(0x30, 0x301); // CR: UARTEN|TXE|RXE
    CHECK_EQ(f.Read32(0x24), 0x10u);
    CHECK_EQ(f.Read32(0x28), 0x3Bu);
    CHECK_EQ(f.Read32(0x2C), 0x70u);
    CHECK_EQ(f.Read32(0x30), 0x301u);
}

TEST(Pl011_UnknownOffsetIsError) {
    UartFixture f;
    u32 value = 0;
    // 구현되지 않은 오프셋 접근은 MMIO 오류 (조용한 0 반환 금지).
    CHECK(f.mem.ReadT(board::kUart0Base + 0x100, value).kind ==
          MemFaultKind::kMmioError);
}
