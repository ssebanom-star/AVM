#include "avm/dev/pl011.h"

#include "avm/log.h"

namespace avm {

namespace {
constexpr const char* kTag = "avm.uart";

// PL011 식별 레지스터 (0xFE0..0xFFC). Linux 드라이버가 프로브한다.
constexpr u8 kPeriphId[4] = {0x11, 0x10, 0x14, 0x00};
constexpr u8 kCellId[4]   = {0x0D, 0xF0, 0x05, 0xB1};
} // namespace

u32 Pl011::ReadFr() const {
    u32 fr = kFrTxfe; // TX FIFO는 즉시 비워지므로 항상 empty, 절대 full 아님
    if (rx_fifo_.empty()) fr |= kFrRxfe;
    if (rx_fifo_.size() >= kFifoDepth) fr |= kFrRxff;
    return fr;
}

u32 Pl011::ComputeRis() const {
    u32 ris = kIntTx; // TX는 항상 준비 상태 (FIFO 즉시 소진)
    if (!rx_fifo_.empty()) ris |= kIntRx;
    return ris;
}

void Pl011::UpdateIrq() {
    const bool level = (ComputeRis() & imsc_) != 0;
    if (level != irq_level_) {
        irq_level_ = level;
        if (irq_cb_) irq_cb_(level);
    }
}

bool Pl011::MmioRead(u64 offset, unsigned size, u64& value) {
    if (size != 1 && size != 2 && size != 4) return false;

    // 식별 레지스터.
    if (offset >= 0xFE0 && offset < 0x1000) {
        const unsigned index = static_cast<unsigned>((offset - 0xFE0) >> 2);
        if ((offset & 3) != 0) return false;
        value = index < 4 ? kPeriphId[index] : kCellId[index - 4];
        return true;
    }

    switch (offset) {
        case kDr: {
            if (rx_fifo_.empty()) {
                value = 0; // 빈 FIFO 읽기: 0 (오류 플래그 없음)
            } else {
                value = rx_fifo_.front();
                rx_fifo_.pop_front();
                UpdateIrq();
            }
            return true;
        }
        case kRsr:   value = 0; return true;
        case kFr:    value = ReadFr(); return true;
        case kIlpr:  value = 0; return true;
        case kIbrd:  value = ibrd_; return true;
        case kFbrd:  value = fbrd_; return true;
        case kLcrH:  value = lcr_h_; return true;
        case kCr:    value = cr_; return true;
        case kIfls:  value = ifls_; return true;
        case kImsc:  value = imsc_; return true;
        case kRis:   value = ComputeRis(); return true;
        case kMis:   value = ComputeRis() & imsc_; return true;
        case kIcr:   value = 0; return true; // 쓰기 전용
        case kDmacr: value = dmacr_; return true;
        default:
            AVM_LOGD(kTag, "미구현 레지스터 읽기 offset=0x%llx",
                     (unsigned long long)offset);
            return false;
    }
}

bool Pl011::MmioWrite(u64 offset, unsigned size, u64 value) {
    if (size != 1 && size != 2 && size != 4) return false;
    switch (offset) {
        case kDr: {
            const u8 ch = static_cast<u8>(value);
            tx_log_.push_back(static_cast<char>(ch));
            // 부팅 로그를 호스트 로그에도 라인 단위로 흘린다.
            if (ch == '\n') {
                AVM_LOGI(kTag, "TX| %s", line_buf_.c_str());
                line_buf_.clear();
            } else {
                line_buf_.push_back(static_cast<char>(ch));
            }
            UpdateIrq();
            return true;
        }
        case kRsr:   return true; // ECR: 오류 클리어 (오류 미모델링)
        case kIlpr:  return true;
        case kIbrd:  ibrd_ = static_cast<u32>(value) & 0xFFFF; return true;
        case kFbrd:  fbrd_ = static_cast<u32>(value) & 0x3F; return true;
        case kLcrH:  lcr_h_ = static_cast<u32>(value) & 0xFF; return true;
        case kCr:    cr_ = static_cast<u32>(value) & 0xFFFF; return true;
        case kIfls:  ifls_ = static_cast<u32>(value) & 0x3F; return true;
        case kImsc:
            imsc_ = static_cast<u32>(value) & 0x7FF;
            UpdateIrq();
            return true;
        case kIcr:
            // 레벨 계산형 인터럽트(RX/TX)는 원인이 남아 있으면 클리어돼도
            // 다시 계산된다. 상태 재평가만 수행.
            UpdateIrq();
            return true;
        case kDmacr: dmacr_ = static_cast<u32>(value) & 0x7; return true;
        case kFr:
        case kRis:
        case kMis:
            return true; // 읽기 전용: 쓰기 무시 (PL011 관례)
        default:
            AVM_LOGD(kTag, "미구현 레지스터 쓰기 offset=0x%llx",
                     (unsigned long long)offset);
            return false;
    }
}

void Pl011::InjectInput(const void* data, u64 len) {
    const u8* bytes = static_cast<const u8*>(data);
    for (u64 i = 0; i < len; ++i) {
        if (rx_fifo_.size() >= kFifoDepth) break; // FIFO 가득: 초과분 폐기
        rx_fifo_.push_back(bytes[i]);
    }
    UpdateIrq();
}

} // namespace avm
