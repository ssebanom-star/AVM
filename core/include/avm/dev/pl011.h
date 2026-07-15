// PL011 호환 UART.
//
// 초기 부팅 디버깅의 핵심 장치. TX 출력은 내부 콘솔 버퍼에 쌓이고
// (Android 로그/앱 콘솔이 이를 소비), RX는 InjectInput으로 주입한다.
//
// 인터럽트: RIS/IMSC/MIS 레지스터를 모델링하고 IRQ 레벨 변화를 콜백으로
// 알린다. Stage 5에서 이 콜백이 GICv3 SPI 라인(INTID 33)에 연결된다.
#pragma once

#include <deque>
#include <functional>
#include <string>

#include "avm/mem/phys_mem.h"
#include "avm/types.h"

namespace avm {

class Pl011 : public DeviceRegion {
public:
    using IrqCallback = std::function<void(bool level)>;

    Pl011() = default;

    // DeviceRegion 구현.
    const char* Name() const override { return "pl011"; }
    bool MmioRead(u64 offset, unsigned size, u64& value) override;
    bool MmioWrite(u64 offset, unsigned size, u64 value) override;

    // 호스트 측 콘솔 API.
    // TX된 모든 바이트의 누적 로그 (앱 콘솔 표시용).
    const std::string& TxLog() const { return tx_log_; }
    void ClearTxLog() { tx_log_.clear(); }
    // 게스트가 읽을 입력 주입 (호스트 키보드 -> 게스트 콘솔).
    void InjectInput(const void* data, u64 len);

    void SetIrqCallback(IrqCallback cb) { irq_cb_ = std::move(cb); }
    bool IrqLevel() const { return irq_level_; }

private:
    // 레지스터 오프셋.
    static constexpr u64 kDr    = 0x000;
    static constexpr u64 kRsr   = 0x004;
    static constexpr u64 kFr    = 0x018;
    static constexpr u64 kIlpr  = 0x020;
    static constexpr u64 kIbrd  = 0x024;
    static constexpr u64 kFbrd  = 0x028;
    static constexpr u64 kLcrH  = 0x02C;
    static constexpr u64 kCr    = 0x030;
    static constexpr u64 kIfls  = 0x034;
    static constexpr u64 kImsc  = 0x038;
    static constexpr u64 kRis   = 0x03C;
    static constexpr u64 kMis   = 0x040;
    static constexpr u64 kIcr   = 0x044;
    static constexpr u64 kDmacr = 0x048;

    // FR 비트.
    static constexpr u32 kFrBusy = 1u << 3;
    static constexpr u32 kFrRxfe = 1u << 4;
    static constexpr u32 kFrTxff = 1u << 5;
    static constexpr u32 kFrRxff = 1u << 6;
    static constexpr u32 kFrTxfe = 1u << 7;

    // 인터럽트 비트 (RIS/IMSC/MIS/ICR 공통).
    static constexpr u32 kIntRx = 1u << 4;
    static constexpr u32 kIntTx = 1u << 5;

    static constexpr unsigned kFifoDepth = 16;

    u32 ReadFr() const;
    u32 ComputeRis() const;
    void UpdateIrq();

    std::string tx_log_;
    std::string line_buf_; // 호스트 로그용 현재 라인 버퍼
    std::deque<u8> rx_fifo_;

    u32 ibrd_ = 0;
    u32 fbrd_ = 0;
    u32 lcr_h_ = 0;
    u32 cr_ = 0x0300; // TXE|RXE (리셋 값)
    u32 ifls_ = 0x12;
    u32 imsc_ = 0;
    u32 dmacr_ = 0;

    IrqCallback irq_cb_;
    bool irq_level_ = false;
};

} // namespace avm
