#include "avm/jit/jit_engine.h"

#include <cinttypes>
#include <cstddef>
#include <cstring>

#include "avm/cpu/exception.h"
#include "avm/log.h"

namespace avm::jit {

namespace {
constexpr const char* kTag = "avm.jit";

// CpuState 필드 오프셋 (호스트 코드가 x19=CpuState* 기준으로 접근).
constexpr u32 kPcOff = offsetof(CpuState, pc);
constexpr u32 XOff(unsigned n) { return static_cast<u32>(n * 8); }
static_assert(offsetof(CpuState, x) == 0, "x[]는 오프셋 0");

// JitRuntime 폴트 플래그 오프셋.
constexpr u32 kFaultFlagOff = offsetof(JitRuntime, fault_flag);

// 프롤로그 워드 수 (본체 진입점 계산에 사용).
constexpr size_t kPrologueWords = 5;

// 블록당 최대 게스트 명령 수 (spec 10절: 최대 명령어 수 도달 시 종료).
constexpr u64 kMaxBlockInsts = 128;

bool Jittable(Op op) {
    switch (op) {
        case Op::kNop:
        case Op::kMovz: case Op::kMovn: case Op::kMovk:
        case Op::kAddImm: case Op::kSubImm:
        case Op::kAddReg: case Op::kSubReg:
        case Op::kAndReg: case Op::kOrrReg: case Op::kEorReg:
        case Op::kAndImm: case Op::kOrrImm: case Op::kEorImm:
        case Op::kLdr: case Op::kStr:
        case Op::kB: case Op::kBl: case Op::kBCond:
        case Op::kBr: case Op::kBlr: case Op::kRet:
        case Op::kCbz: case Op::kCbnz:
            return true;
        default:
            return false; // SVC/MRS/MSR/ERET/WFI/BARRIER/SYS/UDF -> 인터프리터
    }
}

bool IsTerminator(Op op) {
    switch (op) {
        case Op::kB: case Op::kBl: case Op::kBCond:
        case Op::kBr: case Op::kBlr: case Op::kRet:
        case Op::kCbz: case Op::kCbnz:
            return true;
        default:
            return false;
    }
}

HShift ToHShift(ShiftType t) { return static_cast<HShift>(static_cast<u8>(t)); }

} // namespace

// ---------------------------------------------------------------------------
// 호스트 코드가 호출하는 런타임 헬퍼 (AAPCS64).
// ---------------------------------------------------------------------------
extern "C" {

u64 avm_jit_read_sp(JitRuntime* rt) { return rt->cpu->CurrentSp(); }
void avm_jit_write_sp(JitRuntime* rt, u64 v) { rt->cpu->SetCurrentSp(v); }
u64 avm_jit_load_nzcv(JitRuntime* rt) { return rt->cpu->pstate.ToNzcvBits(); }
void avm_jit_store_nzcv(JitRuntime* rt, u64 packed) {
    rt->cpu->pstate.SetNzcvBits(static_cast<u32>(packed));
}

u64 avm_jit_load(JitRuntime* rt, u64 va, u64 size) {
    u64 value = 0;
    u32 fsc = 0;
    u64 fva = 0;
    if (!MemAccess(*rt->mmu, *rt->mem, *rt->cpu, va, &value,
                   static_cast<unsigned>(size), false, fsc, fva)) {
        rt->fault_flag = 1;
        rt->fault_fsc = fsc;
        rt->fault_va = fva;
        rt->fault_is_write = 0;
        return 0;
    }
    return value;
}

void avm_jit_store(JitRuntime* rt, u64 va, u64 val, u64 size) {
    u32 fsc = 0;
    u64 fva = 0;
    if (!MemAccess(*rt->mmu, *rt->mem, *rt->cpu, va, &val,
                   static_cast<unsigned>(size), true, fsc, fva)) {
        rt->fault_flag = 1;
        rt->fault_fsc = fsc;
        rt->fault_va = fva;
        rt->fault_is_write = 1;
    }
}

} // extern "C"

// ---------------------------------------------------------------------------
// 블록 로우어링 (한 basic block을 호스트 ARM64로 방출)
// ---------------------------------------------------------------------------
namespace {

using namespace hostreg;

// 방출 중 수집하는 분기 링크 (패치 대상 워드 + 게스트 대상 PC).
struct PendingLink {
    size_t patch_word;
    u64 target_pc;
};

// 한 블록의 호스트 코드를 방출하는 로우어러.
// 콜리 세이브 스크래치: x21/x22(로드/스토어 주소·베이스가 헬퍼 호출을
// 건너 생존해야 하므로 사용).
class Lowerer {
public:
    Lowerer(Arm64Emitter& e, std::vector<size_t>& to_epilogue,
            std::vector<PendingLink>& links)
        : e_(e), to_epilogue_(to_epilogue), links_(links) {}

    // 헬퍼 절대 주소.
    u64 read_sp = 0, write_sp = 0, load_nzcv = 0, store_nzcv = 0;
    u64 load_fn = 0, store_fn = 0;

    // 프롤로그: 콜리 세이브 저장, x0->x19(state), x1->x20(rt).
    void Prologue() {
        e_.StpPre(kFp, kLr, kSp, -48);
        e_.StpOff(19, 20, kSp, 16);
        e_.StpOff(21, 22, kSp, 32);
        e_.MovReg(kState, 0);
        e_.MovReg(kRt, 1);
        // (정확히 kPrologueWords 워드)
    }
    // 에필로그: 콜리 세이브 복원, ret. x0(종료 사유)은 보존된다.
    size_t Epilogue() {
        const size_t label = e_.Here();
        e_.LdpOff(21, 22, kSp, 32);
        e_.LdpOff(19, 20, kSp, 16);
        e_.LdpPost(kFp, kLr, kSp, 48);
        e_.Ret();
        return label;
    }

    // 스트레이트 라인 명령 방출.
    void LowerStraight(const DecodedInst& d, u64 pc);
    // 종료 명령 방출.
    void LowerTerminator(const DecodedInst& d, u64 pc);
    // 종료: pc=const, 종료사유, 에필로그로.
    void ExitConst(u64 pc, ExitReason reason) {
        SetPcConst(pc);
        e_.MovConst(0, static_cast<u64>(reason));
        to_epilogue_.push_back(e_.B(0));
    }

private:
    void SetPcConst(u64 pc) {
        e_.MovConst(kTmp0, pc);
        e_.StrX(kTmp0, kState, kPcOff);
    }
    void CallHelper(u64 addr) {
        e_.MovConst(kCall, addr);
        e_.Blr(kCall);
    }
    void ReadZr(unsigned n, unsigned dst) {
        if (n == 31) e_.Movz(dst, 0);
        else e_.LdrX(dst, kState, XOff(n));
    }
    void WriteZr(unsigned n, unsigned src) {
        if (n != 31) e_.StrX(src, kState, XOff(n));
    }
    void ReadSp(unsigned n, unsigned dst) {
        if (n != 31) { e_.LdrX(dst, kState, XOff(n)); return; }
        e_.MovReg(0, kRt);
        CallHelper(read_sp);
        if (dst != 0) e_.MovReg(dst, 0);
    }
    void WriteSp(unsigned n, unsigned src) {
        if (n != 31) { e_.StrX(src, kState, XOff(n)); return; }
        e_.MovReg(1, src);
        e_.MovReg(0, kRt);
        CallHelper(write_sp);
    }
    // 플래그 로드 -> 호스트 NZCV (b.cond용).
    void LoadFlagsToHost() {
        e_.MovReg(0, kRt);
        CallHelper(load_nzcv);
        e_.MsrNzcv(0);
    }
    // 결과를 이미 저장한 뒤, 직전 host 플래그를 게스트로 저장.
    void StoreHostFlags(unsigned captured) {
        e_.MovReg(1, captured);
        e_.MovReg(0, kRt);
        CallHelper(store_nzcv);
    }

    // 메모리 접근(로드/스토어) 방출.
    void LowerMem(const DecodedInst& d, u64 pc);

    Arm64Emitter& e_;
    std::vector<size_t>& to_epilogue_;
    std::vector<PendingLink>& links_;
};

void Lowerer::LowerStraight(const DecodedInst& d, u64 pc) {
    const bool is64 = d.is64;
    switch (d.op) {
        case Op::kNop:
            break;

        case Op::kMovz: {
            const u64 v = (d.imm << d.hw_shift) & MaskForWidth(is64);
            e_.MovConst(kTmp0, v);
            WriteZr(d.rd, kTmp0);
            break;
        }
        case Op::kMovn: {
            const u64 v = ~(d.imm << d.hw_shift) & MaskForWidth(is64);
            e_.MovConst(kTmp0, v);
            WriteZr(d.rd, kTmp0);
            break;
        }
        case Op::kMovk: {
            ReadZr(d.rd, kTmp0);
            e_.Movk(kTmp0, static_cast<u16>(d.imm), d.hw_shift, is64);
            WriteZr(d.rd, kTmp0);
            break;
        }

        case Op::kAddImm:
        case Op::kSubImm: {
            const bool sub = d.op == Op::kSubImm;
            ReadSp(d.rn, kTmp0);          // 산술 imm의 Rn은 SP 형식
            e_.MovConst(kTmp1, d.imm);
            if (d.set_flags) {
                if (sub) e_.Subs(kTmp0, kTmp0, kTmp1, is64);
                else e_.Adds(kTmp0, kTmp0, kTmp1, is64);
                e_.MrsNzcv(kTmp2);
                WriteZr(d.rd, kTmp0);     // ADDS/SUBS의 Rd는 ZR 형식
                StoreHostFlags(kTmp2);
            } else {
                if (sub) e_.Sub(kTmp0, kTmp0, kTmp1, is64);
                else e_.Add(kTmp0, kTmp0, kTmp1, is64);
                WriteSp(d.rd, kTmp0);     // ADD/SUB의 Rd는 SP 형식
            }
            break;
        }

        case Op::kAddReg:
        case Op::kSubReg: {
            const bool sub = d.op == Op::kSubReg;
            ReadZr(d.rn, kTmp0);
            ReadZr(d.rm, kTmp1);
            const HShift sh = ToHShift(d.shift_type);
            if (d.set_flags) {
                if (sub) e_.Subs(kTmp0, kTmp0, kTmp1, is64, sh, d.shift_amount);
                else e_.Adds(kTmp0, kTmp0, kTmp1, is64, sh, d.shift_amount);
                e_.MrsNzcv(kTmp2);
                WriteZr(d.rd, kTmp0);
                StoreHostFlags(kTmp2);
            } else {
                if (sub) e_.Sub(kTmp0, kTmp0, kTmp1, is64, sh, d.shift_amount);
                else e_.Add(kTmp0, kTmp0, kTmp1, is64, sh, d.shift_amount);
                WriteZr(d.rd, kTmp0);
            }
            break;
        }

        case Op::kAndReg:
        case Op::kOrrReg:
        case Op::kEorReg: {
            ReadZr(d.rm, kTmp1);
            // operand2 = shift(rm), 필요 시 invert.
            const HShift sh = ToHShift(d.shift_type);
            if (d.shift_amount != 0 || d.shift_type != ShiftType::kLsl) {
                e_.Orr(kTmp1, kZr, kTmp1, is64, sh, d.shift_amount);
            }
            if (d.invert) {
                e_.MovConst(kTmp2, ~0ull);
                e_.Eor(kTmp1, kTmp1, kTmp2, is64);
            }
            ReadZr(d.rn, kTmp0);
            const bool flags = d.set_flags; // ANDS
            if (d.op == Op::kAndReg) {
                if (flags) e_.Ands(kTmp0, kTmp0, kTmp1, is64);
                else e_.And(kTmp0, kTmp0, kTmp1, is64);
            } else if (d.op == Op::kOrrReg) {
                e_.Orr(kTmp0, kTmp0, kTmp1, is64);
            } else {
                e_.Eor(kTmp0, kTmp0, kTmp1, is64);
            }
            if (flags) {
                e_.MrsNzcv(kTmp2);
                WriteZr(d.rd, kTmp0);
                StoreHostFlags(kTmp2);
            } else {
                WriteZr(d.rd, kTmp0);
            }
            break;
        }

        case Op::kAndImm:
        case Op::kOrrImm:
        case Op::kEorImm: {
            ReadZr(d.rn, kTmp0);
            e_.MovConst(kTmp1, d.imm);
            const bool flags = d.set_flags; // ANDS immediate
            if (d.op == Op::kAndImm) {
                if (flags) e_.Ands(kTmp0, kTmp0, kTmp1, is64);
                else e_.And(kTmp0, kTmp0, kTmp1, is64);
            } else if (d.op == Op::kOrrImm) {
                e_.Orr(kTmp0, kTmp0, kTmp1, is64);
            } else {
                e_.Eor(kTmp0, kTmp0, kTmp1, is64);
            }
            if (flags) {
                e_.MrsNzcv(kTmp2);
                WriteZr(d.rd, kTmp0);     // ANDS: Rd는 ZR 형식
                StoreHostFlags(kTmp2);
            } else {
                WriteSp(d.rd, kTmp0);     // 논리 imm 비플래그: Rd는 SP 형식
            }
            break;
        }

        case Op::kLdr:
        case Op::kStr:
            LowerMem(d, pc);
            break;

        default:
            // 도달하면 번역기 버그 (Jittable 목록과 불일치).
            e_.Brk(1);
            break;
    }
}

void Lowerer::LowerMem(const DecodedInst& d, u64 pc) {
    const unsigned size = 1u << d.access_size_log2;
    const bool is_store = d.op == Op::kStr;
    const bool post = d.addr_mode == AddrMode::kPostIndex;
    const bool pre = d.addr_mode == AddrMode::kPreIndex;

    // 폴트 시 사용할 PC를 미리 저장 (성공 경로에서는 종료 시 덮어씀).
    SetPcConst(pc);

    // 베이스 (SP 형식). x21(콜리 세이브)에 보존.
    ReadSp(d.rn, 21);
    e_.MovConst(kTmp1, static_cast<u64>(d.mem_offset));
    // 유효 주소 -> x22(콜리 세이브).
    if (post) e_.MovReg(22, 21);            // post: addr = base
    else e_.Add(22, 21, kTmp1, true);        // unsigned/pre: addr = base + off

    if (is_store) {
        ReadZr(d.rt, kTmp0);                 // 저장 값
        e_.MovReg(2, kTmp0);                 // x2 = val
        e_.MovConst(3, size);                // x3 = size
        e_.MovReg(1, 22);                    // x1 = va
        e_.MovReg(0, kRt);                   // x0 = rt
        CallHelper(store_fn);
    } else {
        e_.MovConst(2, size);                // x2 = size
        e_.MovReg(1, 22);                    // x1 = va
        e_.MovReg(0, kRt);                   // x0 = rt
        CallHelper(load_fn);
        // 결과 x0. 폴트 검사 전에 임시 보관.
        e_.MovReg(kTmp3, 0);
    }
    // 폴트 검사.
    e_.Ldrb(kTmp0, kRt, kFaultFlagOff);
    e_.MovConst(0, static_cast<u64>(ExitReason::kMemFault));
    to_epilogue_.push_back(e_.Cbnz(kTmp0, 0, /*is64=*/false));
    if (!is_store) {
        WriteZr(d.rt, kTmp3);                // 로드 결과 기록
    }
    // 라이트백.
    if (pre) {
        WriteSp(d.rn, 22);
    } else if (post) {
        e_.MovConst(kTmp1, static_cast<u64>(d.mem_offset));
        e_.Add(kTmp1, 21, kTmp1, true);
        WriteSp(d.rn, kTmp1);
    }
}

void Lowerer::LowerTerminator(const DecodedInst& d, u64 pc) {
    switch (d.op) {
        case Op::kB: {
            const u64 target = pc + static_cast<u64>(d.branch_offset);
            SetPcConst(target);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            links_.push_back({e_.B(0), target});
            break;
        }
        case Op::kBl: {
            const u64 target = pc + static_cast<u64>(d.branch_offset);
            e_.MovConst(kTmp0, pc + 4);
            e_.StrX(kTmp0, kState, XOff(30));
            SetPcConst(target);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            links_.push_back({e_.B(0), target});
            break;
        }
        case Op::kBCond: {
            const u64 taken = pc + static_cast<u64>(d.branch_offset);
            LoadFlagsToHost();
            const size_t bc = e_.BCond(static_cast<HCond>(d.cond), 0);
            // fallthrough tail.
            SetPcConst(pc + 4);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            links_.push_back({e_.B(0), pc + 4});
            // taken tail.
            e_.PatchBranch(bc, e_.Here());
            SetPcConst(taken);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            links_.push_back({e_.B(0), taken});
            break;
        }
        case Op::kCbz:
        case Op::kCbnz: {
            const u64 taken = pc + static_cast<u64>(d.branch_offset);
            ReadZr(d.rt, kTmp0);
            const size_t bc = (d.op == Op::kCbz)
                                  ? e_.Cbz(kTmp0, 0, d.is64)
                                  : e_.Cbnz(kTmp0, 0, d.is64);
            SetPcConst(pc + 4);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            links_.push_back({e_.B(0), pc + 4});
            e_.PatchBranch(bc, e_.Here());
            SetPcConst(taken);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            links_.push_back({e_.B(0), taken});
            break;
        }
        case Op::kBr:
        case Op::kRet: {
            ReadZr(d.rn, kTmp0);
            e_.StrX(kTmp0, kState, kPcOff);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            to_epilogue_.push_back(e_.B(0));
            break;
        }
        case Op::kBlr: {
            ReadZr(d.rn, kTmp0);              // 대상 (LR 쓰기 전에 읽음)
            e_.MovConst(kTmp1, pc + 4);
            e_.StrX(kTmp1, kState, XOff(30));
            e_.StrX(kTmp0, kState, kPcOff);
            e_.MovConst(0, static_cast<u64>(ExitReason::kContinue));
            to_epilogue_.push_back(e_.B(0));
            break;
        }
        default:
            e_.Brk(2);
            break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// JitEngine 구현
// ---------------------------------------------------------------------------

JitEngine::JitEngine(CpuState& cpu, PhysMem& mem, ExecConfig config)
    : cpu_(cpu), mem_(mem), config_(config), mmu_(cpu, mem),
      interp_(cpu, mem, config) {
    rt_.cpu = &cpu_;
    rt_.mem = &mem_;
    rt_.mmu = &mmu_;
}

JitEngine::Block* JitEngine::LookupBlock(u64 guest_pc) {
    auto it = blocks_.find(guest_pc);
    if (it == blocks_.end()) return nullptr;
    Block* b = it->second.get();
    // 코드 자체 수정 감지: 시작 페이지 세대가 바뀌었으면 폐기.
    u64 pa = guest_pc;
    MmuFault f;
    if (mmu_.Translate(guest_pc, AccessType::kFetch, f, pa)) {
        if (PageInfo* page = mem_.Page(pa)) {
            if (page->code_generation != b->code_generation) {
                blocks_.erase(it);
                return nullptr;
            }
        }
    }
    return b;
}

JitEngine::Block* JitEngine::TranslateBlock(u64 start_pc) {
    // 인출: MMU 변환 후 물리 읽기 (인터프리터 인출과 동일 의미).
    auto fetch = [&](u64 pc, u32& raw, u32& page_gen) -> bool {
        u64 pa = pc;
        MmuFault f;
        if (!mmu_.Translate(pc, AccessType::kFetch, f, pa)) return false;
        if (mem_.FetchU32(pa, raw)) return false;
        if (PageInfo* page = mem_.Page(pa)) page_gen = page->code_generation;
        return true;
    };

    // 첫 명령이 인출 실패거나 비-JIT면 번역하지 않는다(인터프리터가 처리).
    u32 first_raw = 0, start_gen = 0;
    if (!fetch(start_pc, first_raw, start_gen)) return nullptr;
    if (!Jittable(Decode(first_raw).op)) return nullptr;

    Arm64Emitter e;
    std::vector<size_t> to_epilogue;
    std::vector<PendingLink> pending;
    Lowerer lo(e, to_epilogue, pending);
    lo.read_sp = reinterpret_cast<u64>(&avm_jit_read_sp);
    lo.write_sp = reinterpret_cast<u64>(&avm_jit_write_sp);
    lo.load_nzcv = reinterpret_cast<u64>(&avm_jit_load_nzcv);
    lo.store_nzcv = reinterpret_cast<u64>(&avm_jit_store_nzcv);
    lo.load_fn = reinterpret_cast<u64>(&avm_jit_load);
    lo.store_fn = reinterpret_cast<u64>(&avm_jit_store);

    lo.Prologue();

    u64 pc = start_pc;
    u64 count = 0;
    while (true) {
        if (count >= kMaxBlockInsts) {
            lo.ExitConst(pc, ExitReason::kContinue);
            break;
        }
        u32 raw = 0, gen = 0;
        if (!fetch(pc, raw, gen)) {
            // 인출 폴트: 인터프리터가 pc에서 정확히 재현하도록 위임.
            lo.ExitConst(pc, ExitReason::kInterpret);
            break;
        }
        const DecodedInst d = Decode(raw);
        if (!Jittable(d.op)) {
            lo.ExitConst(pc, ExitReason::kInterpret);
            break;
        }
        if (IsTerminator(d.op)) {
            lo.LowerTerminator(d, pc);
            ++count;
            break;
        }
        lo.LowerStraight(d, pc);
        ++count;
        pc += 4;
        // 페이지 경계에서 블록 종료 (spec 10절).
        if ((pc & kGuestPageMask) == 0) {
            lo.ExitConst(pc, ExitReason::kContinue);
            break;
        }
    }

    // 에필로그 방출 후, 에필로그로 향하는 모든 분기와 링크 기본값을 패치.
    const size_t epilogue = lo.Epilogue();
    for (size_t w : to_epilogue) e.PatchBranch(w, epilogue);
    for (const auto& link : pending) e.PatchBranch(link.patch_word, epilogue);

    void* host = cache_.Emit(e.Code());
    if (host == nullptr) {
        // 코드 캐시 가득: 전체 비우고 재시도 (모든 블록/링크 무효화).
        blocks_.clear();
        cache_.Reset();
        stats_.cache_resets++;
        host = cache_.Emit(e.Code());
        if (host == nullptr) return nullptr;
    }

    auto block = std::make_unique<Block>();
    block->guest_pc = start_pc;
    block->host_code = host;
    block->guest_inst_count = count;
    block->code_generation = start_gen;
    for (const auto& link : pending) {
        Block::Link bl;
        bl.patch_word = link.patch_word;
        bl.patch_addr = static_cast<u8*>(host) + link.patch_word * 4;
        bl.target_pc = link.target_pc;
        bl.patched = false;
        block->links.push_back(bl);
    }
    Block* result = block.get();
    blocks_[start_pc] = std::move(block);
    stats_.blocks_translated++;
    return result;
}

JitEngine::BlockSummary JitEngine::TranslateForTest(u64 guest_pc) {
    BlockSummary s;
    Block* b = TranslateBlock(guest_pc);
    if (b == nullptr) return s;
    s.translated = true;
    s.inst_count = b->guest_inst_count;
    s.link_count = b->links.size();
    s.has_host_code = b->host_code != nullptr;
    return s;
}

StopInfo JitEngine::Run(u64 max_instructions) {
#if defined(__aarch64__)
    if (validate_) return RunValidated(max_instructions);

    u64 executed = 0;
    while (executed < max_instructions) {
        Block* b = LookupBlock(cpu_.pc);
        if (b == nullptr) b = TranslateBlock(cpu_.pc);
        if (b == nullptr) {
            // 비-JIT 명령 또는 인출 폴트: 인터프리터가 한 스텝 처리.
            StopInfo s = interp_.Step();
            stats_.interp_steps++;
            executed++;
            if (s.reason != StopReason::kNone) return s;
            continue;
        }
        TryChain(*b);
        rt_.fault_flag = 0;
        const ExitReason er = ExecuteBlock(*b);
        stats_.blocks_executed++;
        executed += b->guest_inst_count ? b->guest_inst_count : 1;

        if (er == ExitReason::kMemFault) {
            const u64 fault_pc = cpu_.pc; // 블록이 폴트 명령 PC로 설정함
            const bool from_el0 = cpu_.pstate.el == ExceptionLevel::EL0;
            const auto ec = from_el0 ? ExceptionClass::kDataAbortLower
                                     : ExceptionClass::kDataAbort;
            const u32 iss = (rt_.fault_is_write ? 1u << 6 : 0) | rt_.fault_fsc;
            if (config_.guest_vectors) {
                TakeSyncException(cpu_, ec, iss, rt_.fault_va, fault_pc);
                continue;
            }
            StopInfo s;
            s.reason = StopReason::kDataAbort;
            s.pc = fault_pc;
            s.fault = MemFault{MemFaultKind::kPermission, rt_.fault_va,
                               rt_.fault_is_write != 0, false};
            return s;
        }
        if (er == ExitReason::kInterpret) {
            StopInfo s = interp_.Step();
            stats_.interp_steps++;
            executed++;
            if (s.reason != StopReason::kNone) return s;
            continue;
        }
        // kContinue: 다음 블록으로.
    }
    StopInfo done;
    done.reason = StopReason::kMaxInstructions;
    done.pc = cpu_.pc;
    return done;
#else
    // JIT 실행은 ARM64 호스트 전용. 다른 호스트(개발 빌드)에서는 참조
    // 인터프리터로 대체한다 (번역/인코딩/코드캐시는 별도 테스트로 검증).
    return interp_.Run(max_instructions);
#endif
}

#if defined(__aarch64__)
ExitReason JitEngine::ExecuteBlock(Block& block) {
    using Fn = u64 (*)(CpuState*, JitRuntime*);
    Fn fn = reinterpret_cast<Fn>(block.host_code);
    return static_cast<ExitReason>(fn(&cpu_, &rt_));
}
#endif

void JitEngine::TryChain(Block& from) {
    for (auto& link : from.links) {
        if (link.patched) continue;
        auto it = blocks_.find(link.target_pc);
        if (it == blocks_.end()) continue;
        Block* target = it->second.get();
        u8* body = static_cast<u8*>(target->host_code) + kPrologueWords * 4;
        u8* patch = static_cast<u8*>(link.patch_addr);
        const i64 rel = body - patch;
        const u32 word = 0x14000000u | (static_cast<u32>(rel >> 2) & 0x03FFFFFF);
        cache_.PatchWord(patch, word);
        link.patched = true;
        stats_.chain_patches++;
    }
}

StopInfo JitEngine::RunValidated(u64 max_instructions) {
    u64 executed = 0;
    while (executed < max_instructions) {
        Block* b = LookupBlock(cpu_.pc);
        if (b == nullptr) b = TranslateBlock(cpu_.pc);
        if (b == nullptr) {
            StopInfo s = interp_.Step();
            stats_.interp_steps++;
            executed++;
            if (s.reason != StopReason::kNone) return s;
            continue;
        }
        const CpuState before = cpu_;
        // 검증 모드에서는 블록을 연결(체이닝)하지 않는다: 한 번의 실행이
        // 정확히 한 블록만 수행해야 참조 인터프리터의 동일 명령 수 실행과
        // 대조가 성립한다. (체이닝 정확성은 plain 모드에서 별도 검증)
        if (!ValidateBlock(*b, before)) {
            StopInfo s;
            s.reason = StopReason::kInternalError;
            s.pc = before.pc;
            return s;
        }
        executed += b->guest_inst_count ? b->guest_inst_count : 1;
        // ValidateBlock가 남긴 종료 사유 처리.
        if (last_exit_ == ExitReason::kMemFault) {
            const u64 fault_pc = cpu_.pc;
            const bool from_el0 = cpu_.pstate.el == ExceptionLevel::EL0;
            const auto ec = from_el0 ? ExceptionClass::kDataAbortLower
                                     : ExceptionClass::kDataAbort;
            const u32 iss = (rt_.fault_is_write ? 1u << 6 : 0) | rt_.fault_fsc;
            if (config_.guest_vectors) {
                TakeSyncException(cpu_, ec, iss, rt_.fault_va, fault_pc);
                continue;
            }
            StopInfo s;
            s.reason = StopReason::kDataAbort;
            s.pc = fault_pc;
            s.fault = MemFault{MemFaultKind::kPermission, rt_.fault_va,
                               rt_.fault_is_write != 0, false};
            return s;
        }
        if (last_exit_ == ExitReason::kInterpret) {
            StopInfo s = interp_.Step();
            stats_.interp_steps++;
            executed++;
            if (s.reason != StopReason::kNone) return s;
        }
    }
    StopInfo done;
    done.reason = StopReason::kMaxInstructions;
    done.pc = cpu_.pc;
    return done;
}

bool JitEngine::ValidateBlock(Block& block, const CpuState& before) {
#if defined(__aarch64__)
    std::vector<u8> ram_before = mem_.SnapshotRam();

    // 1) JIT 실행.
    rt_.fault_flag = 0;
    last_exit_ = ExecuteBlock(block);
    const CpuState after_jit = cpu_;
    std::vector<u8> ram_after_jit = mem_.SnapshotRam();

    // 2) 동일 시작 상태에서 참조 인터프리터로 같은 명령 수 실행.
    cpu_ = before;
    mem_.RestoreRam(ram_before);
    Interpreter ref(cpu_, mem_, config_);
    for (u64 i = 0; i < block.guest_inst_count; ++i) {
        StopInfo s = ref.Step();
        if (s.reason != StopReason::kNone) break; // 폴트 등: 같은 지점에서 정지
    }
    const CpuState after_ref = cpu_;
    std::vector<u8> ram_after_ref = mem_.SnapshotRam();

    // 3) 비교.
    auto mismatch = [&](const char* what) {
        validation_failed_ = true;
        char buf[128];
        snprintf(buf, sizeof(buf), "검증 불일치 @0x%" PRIx64 ": %s\n",
                 before.pc, what);
        validation_log_ += buf;
    };
    bool ok = true;
    for (unsigned i = 0; i < 31; ++i) {
        if (after_jit.x[i] != after_ref.x[i]) { mismatch("x[]"); ok = false; break; }
    }
    if (ok && after_jit.pc != after_ref.pc) { mismatch("pc"); ok = false; }
    if (ok && after_jit.CurrentSp() != after_ref.CurrentSp()) { mismatch("sp"); ok = false; }
    if (ok && after_jit.pstate.ToNzcvBits() != after_ref.pstate.ToNzcvBits()) {
        mismatch("nzcv"); ok = false;
    }
    if (ok && ram_after_jit != ram_after_ref) { mismatch("memory"); ok = false; }

    // 4) JIT 결과로 상태 복원 (진행은 JIT 기준).
    cpu_ = after_jit;
    mem_.RestoreRam(ram_after_jit);
    return ok;
#else
    (void)block;
    (void)before;
    return true;
#endif
}

} // namespace avm::jit
