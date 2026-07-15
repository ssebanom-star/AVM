// 호스트 ARM64(AArch64) 머신 코드 이미터.
//
// JIT 백엔드가 게스트 번역 블록을 호스트 명령으로 방출할 때 사용한다.
// (테스트용 asm64 상수 빌더와 별개: 이쪽은 성장 가능한 버퍼 + 라벨/패치
//  지원을 갖춘 실제 코드 생성기다.)
//
// 모든 인코딩은 tests/arm64_emitter_test.cpp에서 aarch64 어셈블러(GNU as)
// 출력과 교차 검증하며, aarch64 크로스 빌드를 qemu로 실행해 동작까지 확인한다.
#pragma once

#include <vector>

#include "avm/types.h"

namespace avm::jit {

// 호스트 레지스터 사용 규약 (AAPCS64 기반, 번역 블록 내부).
namespace hostreg {
inline constexpr unsigned kState = 19; // CpuState* (콜리 세이브)
inline constexpr unsigned kRt    = 20; // JitRuntime* (콜리 세이브)
inline constexpr unsigned kTmp0  = 9;
inline constexpr unsigned kTmp1  = 10;
inline constexpr unsigned kTmp2  = 11;
inline constexpr unsigned kTmp3  = 12;
inline constexpr unsigned kCall  = 16; // 헬퍼 주소 로딩용 (IP0)
inline constexpr unsigned kZr    = 31;
inline constexpr unsigned kSp    = 31;
inline constexpr unsigned kFp    = 29;
inline constexpr unsigned kLr    = 30;
} // namespace hostreg

// 시프트 종류 (호스트 shifted-register 형식).
enum class HShift : u8 { kLsl = 0, kLsr = 1, kAsr = 2, kRor = 3 };

// 조건 코드 (게스트와 동일 인코딩 — 직접 대응).
enum class HCond : u8 {
    kEq = 0, kNe = 1, kCs = 2, kCc = 3, kMi = 4, kPl = 5, kVs = 6, kVc = 7,
    kHi = 8, kLs = 9, kGe = 10, kLt = 11, kGt = 12, kLe = 13, kAl = 14,
};

class Arm64Emitter {
public:
    Arm64Emitter() = default;

    const std::vector<u32>& Code() const { return code_; }
    size_t SizeWords() const { return code_.size(); }
    size_t SizeBytes() const { return code_.size() * 4; }
    void Clear() { code_.clear(); }

    // 현재 방출 위치(워드 인덱스). 라벨/패치 기준점.
    size_t Here() const { return code_.size(); }

    // --- 이동 / 상수 -------------------------------------------------------
    void Movz(unsigned rd, u16 imm16, unsigned shift = 0, bool is64 = true);
    void Movk(unsigned rd, u16 imm16, unsigned shift = 0, bool is64 = true);
    void Movn(unsigned rd, u16 imm16, unsigned shift = 0, bool is64 = true);
    void MovReg(unsigned rd, unsigned rm, bool is64 = true); // orr rd, zr, rm
    // 임의의 64비트 상수를 rd에 적재 (movz + movk 시퀀스).
    void MovConst(unsigned rd, u64 value);

    // --- 산술 (shifted register) ------------------------------------------
    void Add(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
             HShift shift = HShift::kLsl, unsigned amount = 0);
    void Sub(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
             HShift shift = HShift::kLsl, unsigned amount = 0);
    void Adds(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
              HShift shift = HShift::kLsl, unsigned amount = 0);
    void Subs(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
              HShift shift = HShift::kLsl, unsigned amount = 0);

    // --- 논리 (shifted register) ------------------------------------------
    void And(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
             HShift shift = HShift::kLsl, unsigned amount = 0);
    void Orr(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
             HShift shift = HShift::kLsl, unsigned amount = 0);
    void Eor(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
             HShift shift = HShift::kLsl, unsigned amount = 0);
    void Ands(unsigned rd, unsigned rn, unsigned rm, bool is64 = true,
              HShift shift = HShift::kLsl, unsigned amount = 0);

    // --- 로드/스토어 (base + 스케일 unsigned imm) --------------------------
    void LdrX(unsigned rt, unsigned rn, u32 byte_offset);
    void StrX(unsigned rt, unsigned rn, u32 byte_offset);
    void LdrW(unsigned rt, unsigned rn, u32 byte_offset); // zero-extend
    void StrW(unsigned rt, unsigned rn, u32 byte_offset);
    void Ldrb(unsigned rt, unsigned rn, u32 byte_offset);
    void Strb(unsigned rt, unsigned rn, u32 byte_offset);

    // --- 스택 (STP/LDP) ----------------------------------------------------
    void StpPre(unsigned rt, unsigned rt2, unsigned rn, int imm);  // stp [rn,#imm]!
    void LdpPost(unsigned rt, unsigned rt2, unsigned rn, int imm); // ldp [rn],#imm
    void StpOff(unsigned rt, unsigned rt2, unsigned rn, int imm);  // stp [rn,#imm]
    void LdpOff(unsigned rt, unsigned rt2, unsigned rn, int imm);  // ldp [rn,#imm]

    // --- 분기 -------------------------------------------------------------
    // 상대 분기는 patch를 위해 위치를 반환한다.
    size_t B(i64 byte_offset = 0);
    size_t BCond(HCond cond, i64 byte_offset = 0);
    size_t Cbz(unsigned rt, i64 byte_offset = 0, bool is64 = true);
    size_t Cbnz(unsigned rt, i64 byte_offset = 0, bool is64 = true);
    void Br(unsigned rn);
    void Blr(unsigned rn);
    void Ret(unsigned rn = hostreg::kLr);

    // --- 시스템 / 기타 -----------------------------------------------------
    void MrsNzcv(unsigned rt);
    void MsrNzcv(unsigned rt);
    void Nop();
    void Brk(u16 imm = 0);

    // --- 패치 --------------------------------------------------------------
    // branch_word_index 위치의 B/BCond/CBZ/CBNZ를 target_word_index로 재조준.
    void PatchBranch(size_t branch_word_index, size_t target_word_index);

private:
    void Emit(u32 word) { code_.push_back(word); }
    void EmitAddSub(unsigned rd, unsigned rn, unsigned rm, bool is64, bool sub,
                    bool set_flags, HShift shift, unsigned amount);
    void EmitLogical(unsigned op, unsigned rd, unsigned rn, unsigned rm,
                     bool is64, HShift shift, unsigned amount);
    void EmitLoadStore(unsigned size, bool load, unsigned rt, unsigned rn,
                       u32 byte_offset);

    std::vector<u32> code_;
};

} // namespace avm::jit
