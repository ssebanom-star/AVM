#include "avm/vm/virt_board.h"

#include "avm/log.h"

namespace avm {

namespace {
constexpr const char* kTag = "avm.board";
} // namespace

VirtBoard::VirtBoard(const VirtBoardConfig& config) : config_(config) {
    if (config_.flash_size == 0 || config_.flash_size > board::kFlashCodeSize ||
        (config_.flash_size & kGuestPageMask) != 0 ||
        config_.ram_size == 0 || (config_.ram_size & kGuestPageMask) != 0) {
        AVM_LOGE(kTag, "잘못된 보드 설정: flash=0x%llx ram=0x%llx",
                 (unsigned long long)config_.flash_size,
                 (unsigned long long)config_.ram_size);
        return;
    }
    if (!mem_.AddRom(board::kFlashCodeBase, config_.flash_size, "flash0")) return;
    if (!mem_.AddRam(board::kRamBase, config_.ram_size, "ram")) return;
    if (!mem_.AddDevice(board::kUart0Base, board::kUart0Size, &uart_)) return;

    ExecConfig exec;
    exec.guest_vectors = config_.guest_vectors;
    interp_ = std::make_unique<Interpreter>(cpu_, mem_, exec);

    Reset();
    ok_ = true;
}

bool VirtBoard::LoadFirmware(const void* data, u64 len) {
    if (len > config_.flash_size) {
        AVM_LOGE(kTag, "펌웨어가 플래시보다 큼: 0x%llx > 0x%llx",
                 (unsigned long long)len,
                 (unsigned long long)config_.flash_size);
        return false;
    }
    return mem_.LoadImage(board::kFlashCodeBase, data, len);
}

void VirtBoard::Reset() {
    cpu_.Reset(board::kFlashCodeBase); // 리셋 벡터 = 플래시 시작
    cpu_.SetCurrentSp(board::kRamBase + config_.ram_size);
    cpu_.sys.cntfrq_el0 = board::kTimerFreqHz;
}

StopInfo VirtBoard::Run(u64 max_instructions) {
    return interp_->Run(max_instructions);
}

} // namespace avm
