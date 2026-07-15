// AVM JNI 브리지.
//
// 원칙 (docs/ARCHITECTURE.md 26절):
//  - Kotlin 계층은 UI/설정/권한만 담당하고, CPU 실행·메모리·장치는
//    전부 이 네이티브 계층 아래에 있다.
//  - JNI 호출은 굵은 단위로만 노출한다 (프레임 단위·명령어 단위 호출 금지).
#include <jni.h>

#include <string>

#include "avm/selftest.h"

namespace {

jstring ToJString(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

} // namespace

extern "C" {

// Stage 1: 내장 ARM64 테스트 바이너리를 게스트 RAM에 적재하고 참조
// 인터프리터로 실행한 결과 리포트를 반환한다.
JNIEXPORT jstring JNICALL
Java_com_avm_app_NativeBridge_runCpuSelfTest(JNIEnv* env, jobject /*thiz*/) {
    const avm::SelfTestResult result = avm::RunCpuSelfTest();
    return ToJString(env, result.log);
}

JNIEXPORT jboolean JNICALL
Java_com_avm_app_NativeBridge_lastSelfTestPassed(JNIEnv* /*env*/, jobject /*thiz*/) {
    // 셀프테스트는 상태를 남기지 않으므로 즉석 재실행으로 판정한다.
    // (Stage 1 전용 편의 API — VM 수명주기 관리는 Stage 9에서 도입)
    return avm::RunCpuSelfTest().all_passed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_avm_app_NativeBridge_engineVersion(JNIEnv* env, jobject /*thiz*/) {
    return ToJString(env,
                     "AVM engine 0.3.0-stage3 (AArch64 interpreter + "
                     "exceptions + MMU/TLB + PL011, avm-virt board)");
}

} // extern "C"
