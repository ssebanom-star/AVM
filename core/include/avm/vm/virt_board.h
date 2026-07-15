// avm-virt 최소 가상 보드 (Stage 2).
//
// 구성: AArch64 vCPU 1개 + 펌웨어 플래시(ROM) + RAM + PL011 UART.
// 리셋 벡터는 플래시 시작(0x0)이며, 펌웨어가 플래시에서 직접 실행된다.
//
// 이후 단계에서 추가되는 것: GICv3/Generic Timer(5), VirtIO Block(6),
// VirtIO GPU/Input(8), VirtIO Net(10), 멀티 vCPU(11).
#pragma once

#include <memory>

#include "avm/board.h"
#include "avm/cpu/cpu_state.h"
#include "avm/dev/pl011.h"
#include "avm/interp/interpreter.h"
#include "avm/mem/phys_mem.h"

namespace avm {

struct VirtBoardConfig {
    u64 flash_size = 2 * 1024 * 1024; // 펌웨어 플래시 (<= board::kFlashCodeSize)
    u64 ram_size = 16 * 1024 * 1024;
    // 아키텍처 예외 모드 (펌웨어/OS 실행 기본). 디버깅 시 false.
    bool guest_vectors = true;
};

class VirtBoard {
public:
    explicit VirtBoard(const VirtBoardConfig& config = {});

    VirtBoard(const VirtBoard&) = delete;
    VirtBoard& operator=(const VirtBoard&) = delete;

    // 초기화가 성공했는지 (메모리 맵 구성 실패 검출).
    bool ok() const { return ok_; }

    // 펌웨어 이미지를 플래시(0x0)에 적재. 실패 시 false.
    bool LoadFirmware(const void* data, u64 len);

    // vCPU 리셋: PC = 플래시 시작 (리셋 벡터), SP = RAM 끝.
    void Reset();

    // 최대 max_instructions개 실행.
    StopInfo Run(u64 max_instructions);

    CpuState& Cpu() { return cpu_; }
    PhysMem& Mem() { return mem_; }
    Pl011& Uart() { return uart_; }
    const VirtBoardConfig& Config() const { return config_; }

private:
    VirtBoardConfig config_;
    PhysMem mem_;
    CpuState cpu_;
    Pl011 uart_;
    std::unique_ptr<Interpreter> interp_;
    bool ok_ = false;
};

} // namespace avm
