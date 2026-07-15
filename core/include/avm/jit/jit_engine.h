// JIT 엔진: 게스트 번역 블록 -> 호스트 ARM64 코드 생성/실행/디스패치.
//
// 실행 구조:
//  - 각 번역 블록은 호스트 서브루틴이다: uint64_t block(CpuState* x0,
//    JitRuntime* x1). 프롤로그에서 x0->x19(state), x1->x20(rt)로 옮기고,
//    게스트 레지스터는 CpuState 메모리에서 로드/스토어한다(정확성 우선;
//    호스트 레지스터 캐싱은 Stage 11 최적화).
//  - 블록은 종료 시 CpuState.pc를 갱신하고 종료 코드를 x0에 담아 반환한다.
//  - 직접 분기는 블록 연결(패치)로 디스패처 복귀 없이 이어진다.
//
// 하이브리드: 이 단계의 JIT는 정수/논리/이동/분기/로드·스토어(핵심 경로)를
// 인라인 방출하고, 그 외(SVC/MRS/MSR/ERET/WFI/SYS/미정의)는 블록 경계에서
// 참조 인터프리터로 한 스텝 실행한다. 미구현 명령을 NOP로 넘기지 않으며,
// 모든 실행 경로가 아키텍처적으로 정확하다.
//
// 검증 모드: 각 JIT 블록 실행을 참조 인터프리터 실행과 대조한다.
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "avm/cpu/cpu_state.h"
#include "avm/interp/interpreter.h"
#include "avm/jit/arm64_emitter.h"
#include "avm/jit/code_cache.h"
#include "avm/mem/phys_mem.h"
#include "avm/mmu/mmu.h"

namespace avm::jit {

// 블록 종료 코드 (호스트 코드가 x0로 반환).
enum class ExitReason : u64 {
    kContinue = 0,   // 디스패처가 CpuState.pc로 다음 블록을 찾음
    kSvc = 1,        // SVC 실행 -> StopInfo
    kWfi = 2,
    kMemFault = 3,   // 데이터 접근 폴트 -> rt에 상세 기록됨
    kInterpret = 4,  // 다음 명령은 인터프리터가 실행해야 함(비-JIT 명령)
};

// 헬퍼 콜백에 전달되는 런타임 컨텍스트 (호스트 x20).
struct JitRuntime {
    CpuState* cpu = nullptr;
    PhysMem* mem = nullptr;
    Mmu* mmu = nullptr;

    // 메모리 헬퍼가 폴트 시 기록하는 정보 (디스패처가 예외로 승격).
    u32 fault_flag = 0; // 0=정상, 1=폴트
    u32 fault_fsc = 0;
    u64 fault_va = 0;
    u32 fault_is_write = 0;
};

class JitEngine {
public:
    JitEngine(CpuState& cpu, PhysMem& mem, ExecConfig config = {});

    bool ok() const { return cache_.ok(); }

    // 최대 max_instructions개의 게스트 명령을 실행.
    StopInfo Run(u64 max_instructions);

    // 검증 모드: 각 JIT 블록을 참조 인터프리터와 대조 (개발 전용).
    void SetValidate(bool on) { validate_ = on; }
    bool ValidationFailed() const { return validation_failed_; }
    const std::string& ValidationLog() const { return validation_log_; }

    struct Stats {
        u64 blocks_translated = 0;
        u64 blocks_executed = 0;
        u64 chain_patches = 0;
        u64 interp_steps = 0;
        u64 cache_resets = 0;
    };
    const Stats& GetStats() const { return stats_; }

    // 테스트 전용: 지정 PC의 블록을 번역만 하고 요약을 반환한다(실행 없음).
    // 호스트 아키텍처와 무관하게 번역기/이미터/코드캐시를 검증한다.
    struct BlockSummary {
        bool translated = false;
        u64 inst_count = 0;
        size_t link_count = 0;
        bool has_host_code = false;
    };
    BlockSummary TranslateForTest(u64 guest_pc);

private:
    // 번역 블록.
    struct Block {
        u64 guest_pc = 0;
        void* host_code = nullptr;
        u64 guest_inst_count = 0;
        // 코드 페이지 자체 수정 감지용 세대 (Stage 1 PhysMem 메타데이터).
        u32 code_generation = 0;
        // 종료 시 직접 분기 대상(있으면). 블록 연결 패치용.
        struct Link {
            size_t patch_word;   // host_code 기준 워드 인덱스
            void* patch_addr;    // 절대 주소
            u64 target_pc;       // 게스트 대상 PC
            bool patched = false;
        };
        std::vector<Link> links;
    };

    Block* LookupBlock(u64 guest_pc);
    Block* TranslateBlock(u64 guest_pc);
    void TryChain(Block& from);

#if defined(__aarch64__)
    ExitReason ExecuteBlock(Block& block);
#endif
    StopInfo RunValidated(u64 max_instructions);
    bool ValidateBlock(Block& block, const CpuState& before);

    CpuState& cpu_;
    PhysMem& mem_;
    ExecConfig config_;
    Mmu mmu_;
    Interpreter interp_;   // 폴백/검증용 참조 인터프리터
    CodeCache cache_;
    JitRuntime rt_;

    std::unordered_map<u64, std::unique_ptr<Block>> blocks_;

    bool validate_ = false;
    bool validation_failed_ = false;
    std::string validation_log_;
    ExitReason last_exit_ = ExitReason::kContinue;
    Stats stats_;
};

} // namespace avm::jit
