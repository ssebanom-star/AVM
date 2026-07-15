# AVM — Android용 독자 ARM64 시스템 가상화 엔진

AVM은 Android ARM64 기기에서 **현대적인 ARM64 운영체제(Linux, 이후 Windows on ARM)를
부팅·실행**하기 위한 독자 개발 시스템 에뮬레이터다. QEMU/TCG/Limbo/Unicorn/Dynarmic 등
기존 엔진을 포팅하지 않고, CPU 실행·동적 바이너리 변환(JIT)·MMU·인터럽트·가상 장치를
전부 직접 구현한다. 목표 성능 기준은 동일한 ARM64 게스트의 **실제 부팅 시간과 실행
성능에서 Limbo(QEMU/TCG)보다 빠른 것**이다.

- 호스트: Android (arm64-v8a, minSdk 29 — S23U/S25U급 ARMv9 기기 우선)
- 게스트: AArch64 전용 (x86/ARM32/RISC-V 없음)
- 실행 방식: EL2/KVM 없이 동작하는 **ARM64→ARM64 동적 바이너리 변환** + 소프트웨어 MMU
- 전체 설계: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

## 현재 상태: Stage 1 / 11 (CPU 인터프리터)

구현 완료:

- vCPU 아키텍처 상태(X0–X30, SP_ELx, PC, PSTATE/NZCV, EL, 시스템 레지스터, V0–V31 저장 공간)
- 게스트 물리 주소 공간: RAM/ROM/MMIO 영역, 4KB 페이지 메타데이터(R/W/X, dirty,
  코드 번역 추적, 코드 무효화 세대 번호), 호스트 포인터 빠른 경로
- ARM64 디코더 + 참조 인터프리터:
  NOP, MOVZ/MOVN/MOVK, ADD/SUB(imm/reg, S 변형), AND/ORR/EOR(reg/imm, ANDS/ORN 등),
  LDR/STR(B/H/W/X, unsigned/pre/post-index), B, BL, B.cond, BR, BLR, RET, CBZ, CBNZ, SVC
- 미지원 명령어는 NOP로 무시하지 않고 **정확한 미정의 명령어 정지**로 처리
- 정밀 폴트 보고 (미매핑/권한/폴트 주소/폴트 PC)
- 호스트 단위 테스트 38개 + 게스트 바이너리 셀프테스트(검증 32항목)
- Android 앱 (Jetpack Compose): 네이티브 엔진에서 게스트 ARM64 테스트 바이너리를
  실행하고 결과를 표시하는 개발 콘솔

## 빌드

### 코어 엔진 (호스트 개발/테스트)

```bash
cmake -S core -B build-host -G Ninja
cmake --build build-host
./build-host/avm_tests      # 단위 테스트
./build-host/avm_selftest   # 게스트 바이너리 셀프테스트
```

### Android APK

Android Studio(또는 SDK/NDK가 설치된 환경)에서:

```bash
./gradlew :app:assembleDebug
```

산출물: `app/build/outputs/apk/debug/app-debug.apk` (arm64-v8a 전용)

## 저장소 구조

```
core/       네이티브 VM 엔진 (C++20, Android/호스트 공용)
app/        Android 애플리케이션 (Kotlin + Compose + JNI)
docs/       아키텍처 및 설계 문서
```
