// AVM 가상 보드("avm-virt") 물리 메모리 맵.
//
// 실제 특정 스마트폰 보드를 복제하지 않는 단순한 ARM64 가상 보드.
// Linux/UEFI가 인식하기 쉬운 표준 장치(GICv3, Generic Timer, PL011,
// VirtIO)만 배치하고 레거시 하드웨어는 두지 않는다.
//
// Stage 1에서는 RAM만 실제로 매핑되며, 나머지 영역은 이후 단계에서
// 해당 장치 구현과 함께 활성화된다. 주소 상수는 지금 확정하여
// DTB 생성(Stage 5)과 장치 구현이 이 헤더 하나를 기준으로 삼게 한다.
#pragma once

#include "avm/types.h"

namespace avm::board {

// ---------------------------------------------------------------------------
// avm-virt 메모리 맵 (모든 값은 게스트 물리 주소)
// ---------------------------------------------------------------------------
// 0x0000_0000 .. 0x0400_0000  UEFI 코드 플래시 (최대 64 MiB, 읽기 전용)
// 0x0400_0000 .. 0x0800_0000  UEFI 변수 플래시 (NVRAM, VM별 파일 백킹)
// 0x0800_0000 .. 0x0801_0000  GICv3 Distributor
// 0x080A_0000 .. 0x0900_0000  GICv3 Redistributor (vCPU당 128KiB x 최대 8)
// 0x0900_0000 .. 0x0900_1000  PL011 UART0
// 0x0901_0000 .. 0x0901_1000  PL031 RTC
// 0x0A00_0000 .. 0x0A00_4000  VirtIO MMIO 슬롯 32개 (슬롯당 0x200)
// 0x1000_0000 .. 0x3EFF_FFFF  PCIe MMIO 윈도우 (후속 단계)
// 0x3F00_0000 .. 0x3FFF_FFFF  PCIe ECAM (후속 단계)
// 0x4000_0000 ..              게스트 RAM (크기는 VM 설정)
// ---------------------------------------------------------------------------

inline constexpr GuestAddr kFlashCodeBase   = 0x0000'0000;
inline constexpr u64       kFlashCodeSize   = 0x0400'0000;
inline constexpr GuestAddr kFlashVarsBase   = 0x0400'0000;
inline constexpr u64       kFlashVarsSize   = 0x0400'0000;

inline constexpr GuestAddr kGicDistBase     = 0x0800'0000;
inline constexpr u64       kGicDistSize     = 0x0001'0000;
inline constexpr GuestAddr kGicRedistBase   = 0x080A'0000;
inline constexpr u64       kGicRedistStride = 0x0002'0000;   // vCPU당 RD+SGI 프레임

inline constexpr GuestAddr kUart0Base       = 0x0900'0000;
inline constexpr u64       kUart0Size       = 0x0000'1000;
inline constexpr u32       kUart0Irq        = 33;            // SPI 1

inline constexpr GuestAddr kRtcBase         = 0x0901'0000;
inline constexpr u64       kRtcSize         = 0x0000'1000;
inline constexpr u32       kRtcIrq          = 34;            // SPI 2

inline constexpr GuestAddr kVirtioMmioBase  = 0x0A00'0000;
inline constexpr u64       kVirtioMmioSize  = 0x0000'0200;   // 슬롯당
inline constexpr u32       kVirtioMmioCount = 32;
inline constexpr u32       kVirtioIrqBase   = 48;            // SPI 16부터

inline constexpr GuestAddr kPcieMmioBase    = 0x1000'0000;
inline constexpr u64       kPcieMmioSize    = 0x2F00'0000;
inline constexpr GuestAddr kPcieEcamBase    = 0x3F00'0000;
inline constexpr u64       kPcieEcamSize    = 0x0100'0000;

inline constexpr GuestAddr kRamBase         = 0x4000'0000;

// Generic Timer PPI (GIC PPI 번호, Linux DT 관례와 동일 구성).
inline constexpr u32 kTimerIrqSecPhys  = 13; // PPI
inline constexpr u32 kTimerIrqPhys     = 14; // PPI (EL1 physical)
inline constexpr u32 kTimerIrqVirt     = 11; // PPI (EL1 virtual)
inline constexpr u32 kTimerIrqHypPhys  = 10; // PPI

// 가상 카운터 주파수 (CNTFRQ_EL0): 62.5MHz — 16ns 해상도.
inline constexpr u64 kTimerFreqHz = 62'500'000;

} // namespace avm::board
