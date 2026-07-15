package com.avm.app

/**
 * 네이티브 VM 엔진(libavmjni.so)과의 유일한 경계.
 *
 * Stage 1에서는 CPU 셀프테스트 진입점만 노출한다.
 * Stage 9(VM 관리)에서 VM 생성/시작/정지/상태 조회 API가 추가되며,
 * 그때도 JNI 호출은 굵은 단위(수명주기 이벤트)로만 유지한다.
 */
object NativeBridge {

    init {
        System.loadLibrary("avmjni")
    }

    /** 내장 ARM64 테스트 바이너리를 게스트 RAM에서 실행하고 리포트를 반환한다. */
    external fun runCpuSelfTest(): String

    /** 셀프테스트 전체 통과 여부. */
    external fun lastSelfTestPassed(): Boolean

    /** 엔진 버전 문자열. */
    external fun engineVersion(): String
}
