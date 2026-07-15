# AVM 시스템 아키텍처

이 문서는 AVM(독자 ARM64 시스템 에뮬레이터)의 전체 설계를 정의한다.
모든 단계의 구현은 이 문서의 구조를 따르며, 변경이 필요하면 문서를 먼저 갱신한다.

## 1. 실행 방식: KVM 없는 고성능 DBT

일반 Android 앱은 EL2(하이퍼바이저 예외 수준)와 `/dev/kvm`을 사용할 수 없고,
게스트 커널의 특권 명령(MMU 제어, 예외 복귀, 시스템 레지스터 접근)을 호스트
CPU에서 그대로 실행할 수도 없다. 따라서 AVM은 하드웨어 가상화가 아니라
**독자 동적 바이너리 변환(DBT) 기반 시스템 에뮬레이터**다.

실행 파이프라인:

```
게스트 메모리 ─▶ 인출 ─▶ ARM64 디코더 ─▶ IR 생성 ─▶ IR 최적화
                                             │
              ┌──────────────────────────────┘
              ▼
      ARM64 호스트 코드 생성 ─▶ 코드 캐시 ─▶ 번역 블록 실행 ─▶ 예외/인터럽트 검사
              ▲                                  │
              └────────── 디스패처(블록 연결/해시 조회) ◀──┘
```

성능의 핵심 근거:

- **ARM64 → ARM64 직접 변환**: 게스트 대부분의 데이터 처리 명령은 호스트 명령
  1~2개로 대응된다. QEMU TCG처럼 범용 중간 표현을 모든 아키텍처 조합에 맞춰
  일반화하는 비용이 없다.
- 단, 게스트 ARM64 명령을 "그대로 복사 실행"하는 것은 불가능하다는 것을 전제한다.
  특권 상태(EL, 시스템 레지스터), 게스트 주소 공간(소프트웨어 MMU/TLB), 예외
  의미(정밀 폴트)를 보존해야 하므로 메모리 접근·분기·시스템 명령은 반드시
  변환 계층을 거친다.
- 인터프리터는 초기 검증, JIT 미구현 명령 폴백, 디버깅, 검증 모드 전용이다.
  정상 실행 경로는 항상 JIT다.

## 2. 전체 시스템 구성

```
┌─────────────────────────────── Android UI 프로세스 ───────────────────────────────┐
│  Jetpack Compose UI (VM 목록/설정/실행 화면, SAF 파일 선택, 권한)                    │
│  Room 설정 저장소  ·  상태 표시  ·  입력 수집                                        │
└──────────────────────────────────────┬──────────────────────────────────────────┘
                                       │ Binder/AIDL (Stage 9)
┌──────────────────────────────────────▼──────────────────────────────────────────┐
│                     VM 서비스 프로세스 (Foreground Service)                        │
│  ┌──────────────────────────── libavmjni.so (JNI 경계) ─────────────────────────┐ │
│  │                          libavm_core (C++20 엔진)                            │ │
│  │  vCPU 스레드(들): 디스패처 ─ JIT 코드 캐시 ─ 인터프리터 폴백                     │ │
│  │  소프트웨어 MMU/TLB  ·  GICv3  ·  Generic Timer  ·  예외 모델                  │ │
│  │  PhysMem: RAM/ROM/MMIO  ·  페이지 메타데이터  ·  코드 무효화                    │ │
│  │  장치: PL011 UART · VirtIO Block/GPU/Input/Net · RTC · 플래시                 │ │
│  │  I/O 스레드: 비동기 디스크(QCOW2/RAW/ISO)  ·  네트워크(NAT)                     │ │
│  │  렌더링 스레드: VirtIO GPU scanout ─▶ ANativeWindow/AHardwareBuffer           │ │
│  └──────────────────────────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────────────────┘
```

- VM 엔진은 Activity 수명주기와 분리된 Foreground Service에서 실행하고,
  가능하면 별도 프로세스(`android:process`)로 격리해 네이티브 크래시가 UI를
  죽이지 않게 한다 (Stage 9).
- JNI 호출은 굵은 수명주기 단위로만 노출한다. 프레임/명령어 단위 JNI 호출 금지.

## 3. 가상 보드 "avm-virt" 메모리 맵

정의 위치: `core/include/avm/board.h` (모든 장치/DTB 생성의 단일 기준)

| 시작 주소      | 끝 주소        | 크기       | 용도 |
|---------------|---------------|-----------|------|
| `0x0000_0000` | `0x0400_0000` | 64 MiB    | UEFI 코드 플래시 (읽기 전용) |
| `0x0400_0000` | `0x0800_0000` | 64 MiB    | UEFI 변수 플래시 (VM별 NVRAM 파일 백킹) |
| `0x0800_0000` | `0x0801_0000` | 64 KiB    | GICv3 Distributor |
| `0x080A_0000` | `0x0900_0000` | —         | GICv3 Redistributor (vCPU당 128 KiB) |
| `0x0900_0000` | `0x0900_1000` | 4 KiB     | PL011 UART0 (SPI 1 / INTID 33) |
| `0x0901_0000` | `0x0901_1000` | 4 KiB     | PL031 RTC (SPI 2 / INTID 34) |
| `0x0A00_0000` | `0x0A00_4000` | 0x200×32  | VirtIO MMIO 슬롯 32개 (SPI 16+) |
| `0x1000_0000` | `0x3F00_0000` | ~752 MiB  | PCIe MMIO 윈도우 (후속) |
| `0x3F00_0000` | `0x4000_0000` | 16 MiB    | PCIe ECAM (후속) |
| `0x4000_0000` | RAM 끝        | VM 설정   | 게스트 RAM |

Generic Timer: PPI 14(EL1 phys), 11(EL1 virt), CNTFRQ = 62.5 MHz.
레거시 하드웨어(i8042, IDE, VGA 등)는 구현하지 않는다.

## 4. C++ 모듈 구성 (`core/`)

| 모듈 | 경로 | 역할 | 단계 |
|------|------|------|------|
| types/log/board | `include/avm/*.h` | 기본 타입, 로깅, 보드 메모리 맵 | 1 |
| cpu | `include/avm/cpu/` | `CpuState`, 예외 진입/복귀, 시스템 레지스터 파일 | 1~3 |
| mem | `include/avm/mem/`, `src/mem/` | `PhysMem`: RAM/ROM/MMIO, 페이지 메타데이터, 호스트 포인터 빠른 경로 | 1 |
| decode | `include/avm/decode/`, `src/decode/` | ARM64 디코더 (인터프리터/JIT 공용) | 1~ |
| interp | `include/avm/interp/`, `src/interp/` | 참조 인터프리터 (검증/폴백/디버깅) | 1 |
| asm | `include/avm/asm/` | 테스트용 인코딩 빌더 (프로덕션 JIT emitter와 별개) | 1 |
| selftest | `src/selftest.cpp` | 게스트 바이너리 셀프테스트 (JNI/CLI 공용) | 1 |
| dev | `include/avm/dev/`, `src/dev/` | PL011 UART (이후: RTC, VirtIO, 플래시) | 2~10 |
| vm | `include/avm/vm/`, `src/vm/` | VirtBoard 조립, 테스트 펌웨어 (이후: vCPU 스레드, 수명주기) | 2~9 |
| mmu | `include/avm/mmu/`, `src/mmu/` | Stage 1 주소 변환, 소프트웨어 TLB, 폴트 분류 | 3 |
| jit | `include/avm/jit/`, `src/jit/` | ARM64 이미터, 코드 캐시(W^X), 번역 블록, 블록 연결, 검증 모드 | 4 |
| irq / timer | (Stage 5) | GICv3(Distributor/Redistributor/CPU IF), Generic Timer IRQ | 5 |
| block | (Stage 6) | RAW/QCOW2 v3/ISO9660 블록 백엔드, 비동기 I/O 큐 | 6 |
| fw | (Stage 5~6) | DTB 생성, EDK II AArch64 펌웨어 이미지 적재/NVRAM | 5~6 |

의존 방향: `vm → (jit|interp) → decode → cpu → types`, `vm → dev → mem`.
역방향 의존 금지. JIT와 인터프리터는 동일한 `CpuState`/`PhysMem`/디코더를 공유한다
(검증 모드에서 블록 단위 상태 비교가 가능해야 하므로).

## 5. Android 프로젝트 구조

```
AVM/
├── settings.gradle.kts, build.gradle.kts, gradle/libs.versions.toml
├── core/                            # 네이티브 엔진 (호스트 빌드 가능)
│   ├── CMakeLists.txt
│   ├── include/avm/
│   │   ├── types.h  log.h  board.h  selftest.h
│   │   ├── cpu/cpu_state.h  cpu/exception.h
│   │   ├── mem/phys_mem.h
│   │   ├── decode/decoder.h
│   │   ├── interp/interpreter.h
│   │   └── asm/assembler.h
│   ├── src/ (log, mem/, decode/, interp/, selftest)
│   ├── tests/  (test_framework.h + decoder/interpreter/phys_mem 테스트)
│   └── tools/avm_selftest_main.cpp
└── app/
    ├── build.gradle.kts
    └── src/main/
        ├── AndroidManifest.xml
        ├── cpp/ (CMakeLists.txt, avm_jni.cpp)
        ├── java/com/avm/app/ (MainActivity.kt, NativeBridge.kt)
        └── res/values/
```

## 6. CPU 상태 모델 (`cpu/cpu_state.h`)

- `x[31]`(X0–X30), `sp[4]`(SP_EL0–EL3, 현재 EL0/EL1 사용), `pc`
- `Pstate`: NZCV, DAIF, 현재 EL, SPSel
- `SystemRegisters`: MIDR/MPIDR, SCTLR/TTBR0/TTBR1/TCR/MAIR(EL1),
  VBAR/ESR/FAR/ELR/SPSR(EL1), TPIDR 계열, CNT* 타이머 레지스터
- `V128 v[32]`, FPCR/FPSR (Stage 1은 저장 공간만, 실행은 후속)
- `wfi_pending`/`irq_pending`/`fiq_pending`: 실행 루프 인터럽트 검사 지점과 연동
- JIT가 고정 오프셋으로 접근하므로 핫 필드(x/sp/pc)는 구조체 선두에 고정하고
  `static_assert`로 오프셋을 검증한다.
- XZR/SP 구분은 명령어 형식에 따라 다르므로 접근 헬퍼(`XZr`/`XSp`)로 강제한다.

## 7. 게스트 메모리 모델 (`mem/phys_mem.h`)

- 영역 기반 물리 주소 공간: RAM(쓰기 가능 백킹), ROM(읽기 전용 백킹, LoadImage로만
  기록), MMIO(`DeviceRegion` 인터페이스, 1/2/4/8바이트 자연 정렬 접근만).
- RAM은 거대 배열 하나가 아니라 **4KB 페이지 단위 메타데이터**를 함께 유지:
  R/W/X, dirty, `kPageHasCode`(번역 블록 존재), 코드 무효화 세대 번호.
  게스트가 코드 페이지에 쓰면 세대가 증가하고 JIT는 해당 블록을 재번역한다
  (자체 수정 코드 대응의 기반, Stage 4에서 번역 블록 목록과 연결).
- `HostPtr()`: RAM의 호스트 포인터 직접 획득 — JIT 인라인 빠른 메모리 접근과
  소프트웨어 TLB의 "호스트 포인터 직접 매핑" 캐시가 사용할 기반 API.
- 접근 실패는 C++ 예외가 아닌 `MemFault` 값으로 보고한다 (게스트 페이지 폴트는
  정상 제어 흐름이며, 코어는 `-fno-exceptions`로 빌드된다).

## 8. 디코더 (`decode/decoder.h`)

- 최상위 4비트 클래스(op0) → 형식별 서브 디코더 구조. 이후 단계에서 빠른
  디코드 테이블로 확장하되, 디코더 출력(`DecodedInst`)의 정규화된 필드 계약은
  유지한다 (인터프리터와 JIT IR 생성기가 공유).
- bitmask immediate(논리 immediate)는 ARM ARM 알고리즘을 그대로 구현.
- 인식하지 못하는 인코딩은 전부 `Op::kUndefined` — 실행 계층이 정확한 미정의
  명령어 정지/예외를 발생시킨다. 단, HINT 공간의 미할당 힌트는 아키텍처 정의에
  따라 NOP 동작으로 디코딩한다 (이는 "미지원 명령 무시"가 아니라 아키텍처 준수).

## 9. 참조 인터프리터 (`interp/interpreter.h`)

- `Step()`: 인출(PC 정렬/실행 권한 검사) → 디코드 → 실행 → PC 갱신.
- `Run(n)`: 인터럽트 검사 지점을 포함한 실행 루프. 정지 사유(`StopInfo`)로
  SVC/미정의/데이터·명령어 중단/최대 명령어 수를 정밀 보고한다.
- NZCV는 `AddWithCarry`(ARM ARM 의미)로 계산하고, 32비트 결과는 상위 32비트를
  0으로 확장한다. SVC는 ELR 의미(PC=다음 명령)로 정지하며 Stage 2~3에서
  VBAR 벡터 진입으로 확장된다.
- 이 인터프리터는 Stage 4 이후 JIT 검증 모드의 기준 구현이 된다:
  동일 블록을 인터프리터/JIT로 각각 실행해 레지스터·PSTATE·메모리 변경·예외를
  비교하고, 불일치 시 최소 재현 로그를 남긴다.

## 9a. JIT 동적 바이너리 변환 (`jit/`, Stage 4)

성능 핵심. ARM64 게스트를 ARM64 호스트 코드로 직접 변환한다.

- **실행 ABI**: 각 번역 블록은 호스트 서브루틴 `u64 block(CpuState* x0,
  JitRuntime* x1)`. 프롤로그가 `x0→x19`(state), `x1→x20`(rt)로 옮기고
  콜리 세이브(x19–x22, fp, lr)를 저장한다. 종료 시 `CpuState.pc`를 갱신하고
  종료 코드를 x0로 반환한다.
- **레지스터 처리(정확성 우선)**: 게스트 레지스터는 `CpuState` 메모리에서
  로드/스토어(load-operate-store). 조건 플래그는 호스트 `ADDS/SUBS/ANDS` +
  `MRS NZCV`로 직접 계산 — 게스트/호스트가 동일 ISA이므로 플래그 의미가
  정확히 일치한다. 호스트 레지스터 캐싱(레지스터 할당 고도화)은 Stage 11.
- **블록 연결(체이닝)**: 직접 분기는 방출 시 자리표시 `b`를 두고, 대상
  블록이 캐시에 존재하면 그 `b`를 대상 블록 본체로 런타임 패치한다.
  조건 분기는 양쪽 에지를 각각 링크한다. 루프는 몇 개 블록만 번역 후 재사용.
- **하이브리드**: 정수/논리/시프트/이동/분기/로드·스토어는 JIT 인라인 방출.
  SVC/MRS/MSR/ERET/WFI/SYS/미정의 등은 블록을 그 지점에서 종료(`kInterpret`)
  하고 디스패처가 참조 인터프리터로 한 스텝 실행한다. 미구현을 NOP로 넘기지
  않으며 모든 경로가 아키텍처적으로 정확하다.
- **메모리/SP 슬로우 패스**: 게스트 로드/스토어와 SP 형식 접근, NZCV 저장은
  C++ 헬퍼(`avm_jit_load/store`, `avm_jit_read/write_sp` 등)를 호출한다.
  이들은 인터프리터와 동일한 `MemAccess` 경로를 공유해 의미가 일치한다.
  RAM 인라인 빠른 경로는 Stage 11.
- **코드 캐시 / W^X**: `mmap` 아레나에 범프 할당. Android W^X 대응으로
  방출/패치 시 페이지를 잠시 RW로 바꾸고 다시 RX로 되돌린 뒤
  `__builtin___clear_cache`로 I-cache 동기화. 이중 매핑은 후속 최적화.
- **자체 수정 코드**: 코드 페이지 세대 번호(PhysMem 메타데이터)로 블록을
  무효화한다. TLBI/코드 쓰기가 세대를 올리면 다음 조회에서 재번역.
- **검증 모드**: 각 블록을 JIT와 참조 인터프리터로 각각 실행(RAM 스냅샷으로
  격리)해 레지스터·PC·PSTATE·메모리를 대조. 불일치 시 최소 재현 로그.
- **호스트 검증 전략**: 개발/CI 호스트가 x86-64이므로 JIT 출력을 직접
  실행할 수 없다. 따라서 (1) 이미터 인코딩은 GNU as 출력과 교차 검증,
  (2) 번역기 블록 구조는 호스트 무관 단위 테스트, (3) **실제 실행/검증은
  aarch64로 크로스 컴파일해 qemu-user로 구동**한다(CI 잡 `jit-arm64-tests`).
  단말(APK)에서는 네이티브 ARM64로 그대로 실행된다.

현재 IR는 "디코드된 명령 블록" 수준의 중간 표현이며, 백엔드가 이를 호스트
코드로 로우어링한다. 더 세분화된 타입드 IR와 최적화 패스(상수 전파, 죽은
코드 제거, 플래그 지연 계산)는 Stage 11의 후속 작업이다.

## 10. 테스트 전략

- **호스트 단위 테스트** (`core/tests/`, 의존성 없는 자체 프레임워크):
  - 어셈블러 빌더를 GNU as 출력 상수와 교차 검증 → 빌더로 디코더/인터프리터 검증
  - 디코더: 필드 복원, UNALLOCATED 인코딩 거부
  - 인터프리터: 산술 플래그(오버플로/캐리/제로), XZR/SP 의미, 로드/스토어 크기와
    인덱싱, 분기/호출/루프, 정밀 폴트
  - 메모리: 매핑 규칙, 페이지 경계, dirty/코드 세대, MMIO 디스패치
- **게스트 바이너리 셀프테스트** (`src/selftest.cpp`): 직접 조립한 ARM64
  프로그램을 게스트 RAM에 적재해 실행 — 단말(APK)과 호스트 CLI에서 동일 코드 실행.
- 이후 단계: MMU 테스트(Stage 3), JIT-인터프리터 비교(Stage 4), 장치 테스트,
  최소 펌웨어→UEFI→Linux 부팅 통합 테스트 (프로젝트 명세 32절).

## 11. 구현 로드맵 (명세 33절 준수)

| 단계 | 내용 | 상태 |
|------|------|------|
| 1 | CPU 인터프리터 + 물리 메모리 + 단위 테스트 | **완료** |
| 2 | 최소 가상 보드: UART(PL011), 예외 벡터 진입, MSR/MRS, 테스트 펌웨어 | **완료** |
| 3 | MMU: Stage 1 변환, 소프트웨어 TLB, 권한/폴트, TLBI/DC ZVA | **완료** |
| 4 | JIT: ARM64 emitter, 코드 캐시(W^X), 번역 블록, 블록 연결, 검증 모드 | **완료** |
| 5 | GICv3 + Generic Timer + DTB 생성 + UEFI UART 부팅 | |
| 6 | 저장장치: RAW/QCOW2 v3/ISO9660, VirtIO Block, 비동기 I/O | |
| 7 | ARM64 Linux 커널 + initrd 부팅, 사용자 공간 진입 | |
| 8 | VirtIO GPU/Input + ANativeWindow 저지연 출력 | |
| 9 | VM 관리: Compose UI, Room 설정, SAF, Foreground Service | |
| 10 | VirtIO Network(NAT/포트 포워딩) + 오디오 | |
| 11 | 최적화: 블록 체이닝 고도화, 빠른 TLB, 멀티 vCPU, big.LITTLE | |

Windows on ARM은 Linux 부팅 안정화와 ACPI/그래픽/저장장치 완성 후 진행한다.

## 12. 금지 사항 (전 단계 공통)

- QEMU/TCG/Unicorn/Dynarmic 등 기존 엔진 코드 포팅·복사 금지
- KVM·`/dev/kvm`·원격 VM 사용 금지
- 미지원 명령어의 NOP 처리 금지 (정확한 예외/진단)
- 미구현 기능을 동작하는 것처럼 표시 금지 (QCOW2 압축/암호화 등은 명확한 오류)
- 가짜 부팅 화면·미리 제작한 UEFI 이미지 흉내 금지
- 오픈소스 라이브러리는 범용 기능(압축/JSON/DB/해시/UI/파일/네트워크 보조/이미지
  디코딩)에 한정. CPU/JIT/MMU/인터럽트/장치/VM 제어 핵심 코드는 직접 구현.
