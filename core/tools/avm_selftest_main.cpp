// 호스트/단말 공용 셀프테스트 CLI.
// 사용: avm_selftest
// 반환 코드: 0 = 전부 통과, 1 = 실패 존재.
#include <cstdio>

#include "avm/selftest.h"

int main() {
    const avm::SelfTestResult result = avm::RunCpuSelfTest();
    std::fputs(result.log.c_str(), stdout);
    return result.all_passed ? 0 : 1;
}
