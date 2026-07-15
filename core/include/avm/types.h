// AVM - 독자 ARM64 시스템 에뮬레이터
// 공통 기본 타입 정의.
//
// 이 프로젝트의 모든 코어 코드는 이 헤더의 타입만 사용한다.
// (호스트: Android AArch64 / 개발 검증: 임의의 64비트 리눅스)
#pragma once

#include <cstdint>
#include <cstddef>

namespace avm {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// 게스트 물리 주소. Stage 1에서는 MMU가 없으므로 게스트 VA == 게스트 PA.
// Stage 3(MMU)부터 GuestVirtAddr -> GuestPhysAddr 변환 계층이 추가된다.
using GuestAddr = u64;

// 게스트 페이지 크기: 초기 구현은 4KB 고정 (16K/64K는 후속 단계).
inline constexpr u64 kGuestPageSize  = 4096;
inline constexpr u64 kGuestPageShift = 12;
inline constexpr u64 kGuestPageMask  = kGuestPageSize - 1;

constexpr u64 PageAlignDown(u64 addr) { return addr & ~kGuestPageMask; }
constexpr u64 PageAlignUp(u64 addr)   { return (addr + kGuestPageMask) & ~kGuestPageMask; }

// 비트 조작 유틸리티 (디코더/인터프리터 공용).
constexpr u32 Bits32(u32 v, unsigned hi, unsigned lo) {
    return (v >> lo) & ((hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u));
}

constexpr u64 SignExtend(u64 value, unsigned bits) {
    const u64 sign = 1ull << (bits - 1);
    return (value ^ sign) - sign;
}

constexpr u64 MaskForWidth(bool is64) { return is64 ? ~0ull : 0xFFFFFFFFull; }

} // namespace avm
