#include "avm/log.h"

namespace avm {

// 기본 로그 레벨: 정보. 릴리스 VM 실행 시 kWarn으로 낮춘다.
LogLevel g_log_level = LogLevel::kInfo;

} // namespace avm
