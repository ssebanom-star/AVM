// 내장 셀프테스트.
//
// 직접 조립한 ARM64 테스트 바이너리를 게스트 RAM에 적재하고 참조
// 인터프리터로 실행한 뒤 레지스터/메모리/정지 사유를 검증한다.
// Android 앱(JNI)과 호스트 CLI가 동일한 코드를 호출하므로,
// 단말에서 실행되는 것과 개발 머신에서 검증하는 것이 항상 같은
// 엔진 경로임이 보장된다.
#pragma once

#include <string>

namespace avm {

struct SelfTestResult {
    bool all_passed = false;
    int passed = 0;
    int failed = 0;
    std::string log; // 사람이 읽을 수 있는 실행 리포트
};

SelfTestResult RunCpuSelfTest();

} // namespace avm
