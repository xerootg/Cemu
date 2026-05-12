#include "BackendAArch64.h"

#pragma push_macro("CSIZE")
#undef CSIZE
#include <xbyak_aarch64.h>
#pragma pop_macro("CSIZE")
#include <xbyak_aarch64_util.h>

#include <cstddef>

#include "../PPCRecompiler.h"
#include "Common/precompiled.h"
#include "Common/cpu_features.h"
#include "HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "HW/Espresso/Interpreter/PPCInterpreterHelper.h"
#include "HW/Espresso/PPCState.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"

using namespace Xbyak_aarch64;

// x24 holds PPCInterpreter_t::remainingCycles for the lifetime of a JIT
// execution session. See IMLArchAArch64::PHYSREG_GPR_COUNT -- x24 is held
// back from the IML allocator pool so the basic-block cycle decrement is a
// single sub_imm against a live host register instead of an ldr/sub/str
// round-trip to PPCInterpreter_t.
constexpr uint32 REMAINING_CYCLES_REG_ID = 24;
constexpr uint32 TEMP_GPR_1_ID = 25;
constexpr uint32 TEMP_GPR_2_ID = 26;
constexpr uint32 PPC_RECOMPILER_INSTANCE_DATA_REG_ID = 27;
constexpr uint32 MEMORY_BASE_REG_ID = 28;
constexpr uint32 HCPU_REG_ID = 29;

constexpr uint32 TEMP_FPR_ID = 31;

struct FPReg
{
	explicit FPReg(size_t index)
		: index(index), VReg(index), QReg(index), DReg(index), SReg(index), HReg(index), BReg(index)
	{
	}
	const size_t index;
	const ::VReg VReg;
	const ::QReg QReg;
	const ::DReg DReg;
	const ::SReg SReg;
	const ::HReg HReg;
	const ::BReg BReg;
};

struct GPReg
{
	explicit GPReg(size_t index)
		: index(index), XReg(index), WReg(index)
	{
	}
	const size_t index;
	const ::XReg XReg;
	const ::WReg WReg;
};

static const XReg HCPU_REG{HCPU_REG_ID}, PPC_REC_INSTANCE_REG{PPC_RECOMPILER_INSTANCE_DATA_REG_ID}, MEM_BASE_REG{MEMORY_BASE_REG_ID};
static const GPReg TEMP_GPR1{TEMP_GPR_1_ID};
static const GPReg TEMP_GPR2{TEMP_GPR_2_ID};
static const GPReg LR{TEMP_GPR_2_ID};
static const GPReg REMAINING_CYCLES_REG{REMAINING_CYCLES_REG_ID};

static const FPReg TEMP_FPR{TEMP_FPR_ID};

static const util::Cpu s_cpu;

class AArch64Allocator : public Allocator
{
  private:
#ifdef XBYAK_USE_MMAP_ALLOCATOR
	inline static MmapAllocator s_allocator;
#else
	inline static Allocator s_allocator;
#endif
	inline static std::mutex s_allocatorMutex;
	Allocator* m_allocatorImpl;
	bool m_freeDisabled = false;

  public:
	AArch64Allocator()
		: m_allocatorImpl(reinterpret_cast<Allocator*>(&s_allocator)) {}

	uint32* alloc(size_t size) override
	{
		std::lock_guard lock{s_allocatorMutex};
		return m_allocatorImpl->alloc(size);
	}

	void setFreeDisabled(bool disabled)
	{
		m_freeDisabled = disabled;
	}

	void free(uint32* p) override
	{
		if (m_freeDisabled)
			return;

		std::lock_guard lock{s_allocatorMutex};
		m_allocatorImpl->free(p);
	}

	[[nodiscard]] bool useProtect() const override
	{
		return !m_freeDisabled && m_allocatorImpl->useProtect();
	}
};

struct UnconditionalJumpInfo
{
	IMLSegment* target;
};

struct ConditionalRegJumpInfo
{
	IMLSegment* target;
	WReg regBool;
	bool mustBeTrue;
};

struct NegativeRegValueJumpInfo
{
	IMLSegment* target;
	WReg regValue;
};

struct NZCVJumpInfo
{
	IMLSegment* target;
	Cond cond;
};

struct TbzJumpInfo
{
	IMLSegment* target;
	WReg regSrc;
	uint8_t bitIndex;
	bool mustBeZero;
};

using JumpInfo = std::variant<
	UnconditionalJumpInfo,
	ConditionalRegJumpInfo,
	NegativeRegValueJumpInfo,
	NZCVJumpInfo,
	TbzJumpInfo>;

struct AArch64GenContext_t : CodeGenerator
{
	explicit AArch64GenContext_t(Allocator* allocator = nullptr);
	void enterRecompilerCode();
	void leaveRecompilerCode();

	void r_name(IMLInstruction* imlInstruction);
	void name_r(IMLInstruction* imlInstruction);
	bool r_s32(IMLInstruction* imlInstruction);
	bool r_r(IMLInstruction* imlInstruction);
	bool r_r_s32(IMLInstruction* imlInstruction);
	bool r_r_s32_carry(IMLInstruction* imlInstruction);
	bool r_r_r(IMLInstruction* imlInstruction);
	bool r_r_r_carry(IMLInstruction* imlInstruction);
	void compare(IMLInstruction* imlInstruction);
	void compare_s32(IMLInstruction* imlInstruction);
	bool load(IMLInstruction* imlInstruction, bool indexed);
	bool store(IMLInstruction* imlInstruction, bool indexed);
	// Peepholes that fuse two adjacent IML LOAD/STOREs (same base reg, +4
	// stride, 32-bit width, same endian-swap flag, data regs in consecutive
	// AArch64 host registers, offset in LDP/STP's range) into a single LDP/
	// STP. They catch the IML expansion of `lmw`/`stmw` plus any hand-written
	// multi-lwz/multi-stw burst the register allocator laid out into
	// consecutive host regs. Return true if both instructions were emitted.
	bool tryFuseLoadPair(IMLInstruction* a, IMLInstruction* b);
	bool tryFuseStorePair(IMLInstruction* a, IMLInstruction* b);
	void atomic_cmp_store(IMLInstruction* imlInstruction);
	bool macro(IMLInstruction* imlInstruction);
	void call_imm(IMLInstruction* imlInstruction);
	bool fpr_load(IMLInstruction* imlInstruction, bool indexed);
	bool fpr_store(IMLInstruction* imlInstruction, bool indexed);
	// Peepholes for adjacent FPR loads/stores with the same base reg and +4
	// stride (lfs/lfs and stfs/stfs after IML expansion). PPC big-endian
	// memory means each unfused FP single load is ldr+rev+fmov(+fcvt) — 3 or 4
	// host instructions per access. By loading both 32-bit BE words as one
	// 64-bit value into a NEON D, byte-swapping both lanes with a single
	// rev32, and (for the expanding case) doing the single→double widen with
	// one fcvtl .2d,.2s, we collapse a 6-8 instruction sequence into 5-6.
	// Matches the heaviest static fusion candidate in WW HD (lfs→lfs is the
	// #1 adjacent-pair pattern in the binary).
	bool tryFuseFprLoadPair(IMLInstruction* a, IMLInstruction* b);
	bool tryFuseFprStorePair(IMLInstruction* a, IMLInstruction* b);
	void fpr_r_r(IMLInstruction* imlInstruction);
	void fpr_r_r_r(IMLInstruction* imlInstruction);
	void fpr_r_r_r_r(IMLInstruction* imlInstruction);
	void fpr_r(IMLInstruction* imlInstruction);
	void fpr_compare(IMLInstruction* imlInstruction);
	void cjump(IMLInstruction* imlInstruction, IMLSegment* imlSegment);
	void cjump_nzcv(IMLInstruction* imlInstruction, IMLSegment* imlSegment);
	void cjump_tbz(IMLInstruction* imlInstruction, IMLSegment* imlSegment);
	void jump(IMLSegment* imlSegment);
	void conditionalJumpCycleCheck(IMLSegment* imlSegment);

	static constexpr size_t MAX_JUMP_INSTR_COUNT = 2;
	std::list<std::pair<size_t, JumpInfo>> jumps;
	void prepareJump(JumpInfo&& jumpInfo)
	{
		jumps.emplace_back(getSize(), jumpInfo);
		for (int i = 0; i < MAX_JUMP_INSTR_COUNT; ++i)
			nop();
	}

	std::map<IMLSegment*, size_t> segmentStarts;
	void storeSegmentStart(IMLSegment* imlSegment)
	{
		segmentStarts[imlSegment] = getSize();
	}

	bool processAllJumps()
	{
		for (auto jump : jumps)
		{
			auto jumpStart = jump.first;
			auto jumpInfo = jump.second;
			bool success = std::visit(
				[&, this](const auto& jump) {
					setSize(jumpStart);
					sint64 targetAddress = segmentStarts.at(jump.target);
					sint64 addressOffset = targetAddress - jumpStart;
					return handleJump(addressOffset, jump);
				},
				jumpInfo);
			if (!success)
			{
				return false;
			}
		}
		return true;
	}

	bool handleJump(sint64 addressOffset, const UnconditionalJumpInfo& jump)
	{
		// in +/-128MB
		if (-0x8000000 <= addressOffset && addressOffset <= 0x7ffffff)
		{
			b(addressOffset);
			return true;
		}

		cemu_assert_suspicious();

		return false;
	}

	bool handleJump(sint64 addressOffset, const ConditionalRegJumpInfo& jump)
	{
		bool mustBeTrue = jump.mustBeTrue;

		// in +/-32KB
		if (-0x8000 <= addressOffset && addressOffset <= 0x7fff)
		{
			if (mustBeTrue)
				tbnz(jump.regBool, 0, addressOffset);
			else
				tbz(jump.regBool, 0, addressOffset);
			return true;
		}

		// in +/-1MB
		if (-0x100000 <= addressOffset && addressOffset <= 0xfffff)
		{
			if (mustBeTrue)
				cbnz(jump.regBool, addressOffset);
			else
				cbz(jump.regBool, addressOffset);
			return true;
		}

		Label skipJump;
		if (mustBeTrue)
			tbz(jump.regBool, 0, skipJump);
		else
			tbnz(jump.regBool, 0, skipJump);
		addressOffset -= 4;

		// in +/-128MB
		if (-0x8000000 <= addressOffset && addressOffset <= 0x7ffffff)
		{
			b(addressOffset);
			L(skipJump);
			return true;
		}

		cemu_assert_suspicious();

		return false;
	}

	bool handleJump(sint64 addressOffset, const NZCVJumpInfo& jump)
	{
		// b.cond reach: +/-1MB
		if (-0x100000 <= addressOffset && addressOffset <= 0xfffff)
		{
			b(jump.cond, addressOffset);
			return true;
		}
		// fall back to inverted-cond skip + b for +/-128MB
		Label skipJump;
		b(invert(jump.cond), skipJump);
		addressOffset -= 4;
		if (-0x8000000 <= addressOffset && addressOffset <= 0x7ffffff)
		{
			b(addressOffset);
			L(skipJump);
			return true;
		}
		cemu_assert_suspicious();
		return false;
	}

	static Cond invert(Cond c)
	{
		// flip the bottom bit per the ARM A64 condition encoding
		return static_cast<Cond>(static_cast<uint32>(c) ^ 1u);
	}

	bool handleJump(sint64 addressOffset, const TbzJumpInfo& jump)
	{
		// tbz/tbnz reach: +/-32KB. Falls back to inverted tbnz/tbz + b for longer.
		if (-0x8000 <= addressOffset && addressOffset <= 0x7fff)
		{
			if (jump.mustBeZero)
				tbz(jump.regSrc, jump.bitIndex, addressOffset);
			else
				tbnz(jump.regSrc, jump.bitIndex, addressOffset);
			return true;
		}
		Label skipJump;
		if (jump.mustBeZero)
			tbnz(jump.regSrc, jump.bitIndex, skipJump);
		else
			tbz(jump.regSrc, jump.bitIndex, skipJump);
		addressOffset -= 4;
		if (-0x8000000 <= addressOffset && addressOffset <= 0x7ffffff)
		{
			b(addressOffset);
			L(skipJump);
			return true;
		}
		cemu_assert_suspicious();
		return false;
	}

	bool handleJump(sint64 addressOffset, const NegativeRegValueJumpInfo& jump)
	{
		// in +/-32KB
		if (-0x8000 <= addressOffset && addressOffset <= 0x7fff)
		{
			tbnz(jump.regValue, 31, addressOffset);
			return true;
		}

		// in +/-1MB
		if (-0x100000 <= addressOffset && addressOffset <= 0xfffff)
		{
			tst(jump.regValue, 0x80000000);
			addressOffset -= 4;
			bne(addressOffset);
			return true;
		}

		Label skipJump;
		tbz(jump.regValue, 31, skipJump);
		addressOffset -= 4;

		// in +/-128MB
		if (-0x8000000 <= addressOffset && addressOffset <= 0x7ffffff)
		{
			b(addressOffset);
			L(skipJump);
			return true;
		}

		cemu_assert_suspicious();

		return false;
	}
};

template<std::derived_from<VRegSc> T>
T fpReg(const IMLReg& imlReg)
{
	cemu_assert_debug(imlReg.GetRegFormat() == IMLRegFormat::F64);
	auto regId = imlReg.GetRegID();
	cemu_assert_debug(regId >= IMLArchAArch64::PHYSREG_FPR_BASE && regId < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT);
	return T(regId - IMLArchAArch64::PHYSREG_FPR_BASE);
}

template<std::derived_from<RReg> T>
T gpReg(const IMLReg& imlReg)
{
	auto regFormat = imlReg.GetRegFormat();
	if (std::is_same_v<T, WReg>)
		cemu_assert_debug(regFormat == IMLRegFormat::I32);
	else if (std::is_same_v<T, XReg>)
		cemu_assert_debug(regFormat == IMLRegFormat::I64);
	else
		cemu_assert_unimplemented();

	auto regId = imlReg.GetRegID();
	cemu_assert_debug(regId >= IMLArchAArch64::PHYSREG_GPR_BASE && regId < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT);
	return T(regId - IMLArchAArch64::PHYSREG_GPR_BASE);
}

template<std::derived_from<VRegSc> To, std::derived_from<VRegSc> From>
To aliasAs(const From& reg)
{
	return To(reg.getIdx());
}

template<std::derived_from<RReg> To, std::derived_from<RReg> From>
To aliasAs(const From& reg)
{
	return To(reg.getIdx());
}

AArch64GenContext_t::AArch64GenContext_t(Allocator* allocator)
	: CodeGenerator(DEFAULT_MAX_CODE_SIZE, AutoGrow, allocator)
{
}

constexpr uint64 ones(uint32 size)
{
	return (size == 64) ? 0xffffffffffffffff : ((uint64)1 << size) - 1;
}

constexpr bool isAdrImmValidFPR(sint32 imm, uint32 bits)
{
	uint32 times = bits / 8;
	uint32 sh = std::countr_zero(times);
	return (0 <= imm && imm <= 4095 * times) && ((uint64)imm & ones(sh)) == 0;
}

constexpr bool isAdrImmValidGPR(sint32 imm, uint32 bits = 32)
{
	uint32 size = std::countr_zero(bits / 8u);
	sint32 times = 1 << size;
	return (0 <= imm && imm <= 4095 * times) && ((uint64)imm & ones(size)) == 0;
}

constexpr bool isAdrImmRangeValid(sint32 rangeStart, sint32 rangeOffset, sint32 bits, std::invocable<sint32, uint32> auto check)
{
	for (sint32 i = rangeStart; i <= rangeStart + rangeOffset; i += bits / 8)
		if (!check(i, bits))
			return false;
	return true;
}

constexpr bool isAdrImmRangeValidGPR(sint32 rangeStart, sint32 rangeOffset, sint32 bits = 32)
{
	return isAdrImmRangeValid(rangeStart, rangeOffset, bits, isAdrImmValidGPR);
}

constexpr bool isAdrImmRangeValidFpr(sint32 rangeStart, sint32 rangeOffset, sint32 bits)
{
	return isAdrImmRangeValid(rangeStart, rangeOffset, bits, isAdrImmValidFPR);
}

// Verify that all of the offsets for the PPCInterpreter_t members that we use in r_name/name_r have a valid imm value for AdrUimm
static_assert(isAdrImmRangeValidGPR(offsetof(PPCInterpreter_t, gpr), sizeof(uint32) * 31));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, spr.LR)));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, spr.CTR)));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, spr.XER)));
static_assert(isAdrImmRangeValidGPR(offsetof(PPCInterpreter_t, spr.UGQR), sizeof(PPCInterpreter_t::spr.UGQR[0]) * (SPR_UGQR7 - SPR_UGQR0)));
static_assert(isAdrImmRangeValidGPR(offsetof(PPCInterpreter_t, temporaryGPR_reg), sizeof(uint32) * 3));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, xer_ca), 8));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, xer_so), 8));
static_assert(isAdrImmRangeValidGPR(offsetof(PPCInterpreter_t, cr), PPCREC_NAME_CR_LAST - PPCREC_NAME_CR, 8));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, reservedMemAddr)));
static_assert(isAdrImmValidGPR(offsetof(PPCInterpreter_t, reservedMemValue)));
static_assert(isAdrImmRangeValidFpr(offsetof(PPCInterpreter_t, fpr), sizeof(FPR_t) * 63, 64));
static_assert(isAdrImmRangeValidFpr(offsetof(PPCInterpreter_t, temporaryFPR), sizeof(FPR_t) * 7, 128));

void AArch64GenContext_t::r_name(IMLInstruction* imlInstruction)
{
	uint32 name = imlInstruction->op_r_name.name;

	if (imlInstruction->op_r_name.regR.GetBaseFormat() == IMLRegFormat::I64)
	{
		XReg regRXReg = gpReg<XReg>(imlInstruction->op_r_name.regR);
		WReg regR = aliasAs<WReg>(regRXReg);
		if (name >= PPCREC_NAME_R0 && name < PPCREC_NAME_R0 + 32)
		{
			ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * (name - PPCREC_NAME_R0)));
		}
		else if (name >= PPCREC_NAME_SPR0 && name < PPCREC_NAME_SPR0 + 999)
		{
			uint32 sprIndex = (name - PPCREC_NAME_SPR0);
			if (sprIndex == SPR_LR)
				ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));
			else if (sprIndex == SPR_CTR)
				ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.CTR)));
			else if (sprIndex == SPR_XER)
				ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.XER)));
			else if (sprIndex >= SPR_UGQR0 && sprIndex <= SPR_UGQR7)
				ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.UGQR) + sizeof(PPCInterpreter_t::spr.UGQR[0]) * (sprIndex - SPR_UGQR0)));
			else
				cemu_assert_suspicious();
		}
		else if (name >= PPCREC_NAME_TEMPORARY && name < PPCREC_NAME_TEMPORARY + 4)
		{
			ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, temporaryGPR_reg) + sizeof(uint32) * (name - PPCREC_NAME_TEMPORARY)));
		}
		else if (name == PPCREC_NAME_XER_CA)
		{
			ldrb(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, xer_ca)));
		}
		else if (name == PPCREC_NAME_XER_SO)
		{
			ldrb(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, xer_so)));
		}
		else if (name >= PPCREC_NAME_CR && name <= PPCREC_NAME_CR_LAST)
		{
			ldrb(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, cr) + (name - PPCREC_NAME_CR)));
		}
		else if (name == PPCREC_NAME_CPU_MEMRES_EA)
		{
			ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, reservedMemAddr)));
		}
		else if (name == PPCREC_NAME_CPU_MEMRES_VAL)
		{
			ldr(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, reservedMemValue)));
		}
		else
		{
			cemu_assert_suspicious();
		}
	}
	else if (imlInstruction->op_r_name.regR.GetBaseFormat() == IMLRegFormat::F64)
	{
		auto imlRegR = imlInstruction->op_r_name.regR;

		if (name >= PPCREC_NAME_FPR_HALF && name < (PPCREC_NAME_FPR_HALF + 64))
		{
			uint32 regIndex = (name - PPCREC_NAME_FPR_HALF) / 2;
			uint32 pairIndex = (name - PPCREC_NAME_FPR_HALF) % 2;
			uint32 offset = offsetof(PPCInterpreter_t, fpr) + sizeof(FPR_t) * regIndex + (pairIndex ? sizeof(double) : 0);
			ldr(fpReg<DReg>(imlRegR), AdrUimm(HCPU_REG, offset));
		}
		else if (name >= PPCREC_NAME_TEMPORARY_FPR0 && name < (PPCREC_NAME_TEMPORARY_FPR0 + 8))
		{
			ldr(fpReg<QReg>(imlRegR), AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, temporaryFPR) + sizeof(FPR_t) * (name - PPCREC_NAME_TEMPORARY_FPR0)));
		}
		else
		{
			cemu_assert_suspicious();
		}
	}
	else
	{
		cemu_assert_suspicious();
	}
}

void AArch64GenContext_t::name_r(IMLInstruction* imlInstruction)
{
	uint32 name = imlInstruction->op_r_name.name;

	if (imlInstruction->op_r_name.regR.GetBaseFormat() == IMLRegFormat::I64)
	{
		XReg regRXReg = gpReg<XReg>(imlInstruction->op_r_name.regR);
		WReg regR = aliasAs<WReg>(regRXReg);
		if (name >= PPCREC_NAME_R0 && name < PPCREC_NAME_R0 + 32)
		{
			str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * (name - PPCREC_NAME_R0)));
		}
		else if (name >= PPCREC_NAME_SPR0 && name < PPCREC_NAME_SPR0 + 999)
		{
			uint32 sprIndex = (name - PPCREC_NAME_SPR0);
			if (sprIndex == SPR_LR)
				str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));
			else if (sprIndex == SPR_CTR)
				str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.CTR)));
			else if (sprIndex == SPR_XER)
				str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.XER)));
			else if (sprIndex >= SPR_UGQR0 && sprIndex <= SPR_UGQR7)
				str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.UGQR) + sizeof(PPCInterpreter_t::spr.UGQR[0]) * (sprIndex - SPR_UGQR0)));
			else
				cemu_assert_suspicious();
		}
		else if (name >= PPCREC_NAME_TEMPORARY && name < PPCREC_NAME_TEMPORARY + 4)
		{
			str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, temporaryGPR_reg) + sizeof(uint32) * (name - PPCREC_NAME_TEMPORARY)));
		}
		else if (name == PPCREC_NAME_XER_CA)
		{
			strb(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, xer_ca)));
		}
		else if (name == PPCREC_NAME_XER_SO)
		{
			strb(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, xer_so)));
		}
		else if (name >= PPCREC_NAME_CR && name <= PPCREC_NAME_CR_LAST)
		{
			strb(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, cr) + (name - PPCREC_NAME_CR)));
		}
		else if (name == PPCREC_NAME_CPU_MEMRES_EA)
		{
			str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, reservedMemAddr)));
		}
		else if (name == PPCREC_NAME_CPU_MEMRES_VAL)
		{
			str(regR, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, reservedMemValue)));
		}
		else
		{
			cemu_assert_suspicious();
		}
	}
	else if (imlInstruction->op_r_name.regR.GetBaseFormat() == IMLRegFormat::F64)
	{
		auto imlRegR = imlInstruction->op_r_name.regR;
		if (name >= PPCREC_NAME_FPR_HALF && name < (PPCREC_NAME_FPR_HALF + 64))
		{
			uint32 regIndex = (name - PPCREC_NAME_FPR_HALF) / 2;
			uint32 pairIndex = (name - PPCREC_NAME_FPR_HALF) % 2;
			sint32 offset = offsetof(PPCInterpreter_t, fpr) + sizeof(FPR_t) * regIndex + pairIndex * sizeof(double);
			str(fpReg<DReg>(imlRegR), AdrUimm(HCPU_REG, offset));
		}
		else if (name >= PPCREC_NAME_TEMPORARY_FPR0 && name < (PPCREC_NAME_TEMPORARY_FPR0 + 8))
		{
			str(fpReg<QReg>(imlRegR), AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, temporaryFPR) + sizeof(FPR_t) * (name - PPCREC_NAME_TEMPORARY_FPR0)));
		}
		else
		{
			cemu_assert_suspicious();
		}
	}
	else
	{
		cemu_assert_suspicious();
	}
}

bool AArch64GenContext_t::r_r(IMLInstruction* imlInstruction)
{
	WReg regR = gpReg<WReg>(imlInstruction->op_r_r.regR);
	WReg regA = gpReg<WReg>(imlInstruction->op_r_r.regA);

	if (imlInstruction->operation == PPCREC_IML_OP_ASSIGN)
	{
		mov(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ENDIAN_SWAP)
	{
		rev(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ASSIGN_S8_TO_S32)
	{
		sxtb(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ASSIGN_S16_TO_S32)
	{
		sxth(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_NOT)
	{
		mvn(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_NEG)
	{
		neg(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_CNTLZW)
	{
		clz(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ARM64_CMP)
	{
		// fused cmp emitted by IMLOptimizerArm64_SubstituteCJumpForNZCVJump.
		// regR is the first operand (PPC's rA), regA is the second (PPC's rB).
		cmp(regR, regA);
	}
	else
	{
		cemuLog_log(LogType::Recompiler, "PPCRecompilerAArch64Gen_imlInstruction_r_r(): Unsupported operation {:x}", imlInstruction->operation);
		return false;
	}
	return true;
}

bool AArch64GenContext_t::r_s32(IMLInstruction* imlInstruction)
{
	sint32 imm32 = imlInstruction->op_r_immS32.immS32;
	WReg reg = gpReg<WReg>(imlInstruction->op_r_immS32.regR);

	if (imlInstruction->operation == PPCREC_IML_OP_ASSIGN)
	{
		mov(reg, imm32);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_LEFT_ROTATE)
	{
		ror(reg, reg, 32 - (imm32 & 0x1f));
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ARM64_CMP)
	{
		// fused cmp emitted by IMLOptimizerArm64_SubstituteCJumpForNZCVJump.
		cmp_imm(reg, imm32, TEMP_GPR1.WReg);
	}
	else
	{
		cemuLog_log(LogType::Recompiler, "PPCRecompilerAArch64Gen_imlInstruction_r_s32(): Unsupported operation {:x}", imlInstruction->operation);
		return false;
	}
	return true;
}

bool AArch64GenContext_t::r_r_s32(IMLInstruction* imlInstruction)
{
	WReg regR = gpReg<WReg>(imlInstruction->op_r_r_s32.regR);
	WReg regA = gpReg<WReg>(imlInstruction->op_r_r_s32.regA);
	sint32 immS32 = imlInstruction->op_r_r_s32.immS32;

	if (imlInstruction->operation == PPCREC_IML_OP_ADD)
	{
		add_imm(regR, regA, immS32, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_SUB)
	{
		sub_imm(regR, regA, immS32, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_AND)
	{
		// and_imm uses the AArch64 logical-immediate encoding when the mask
		// fits (single host op); falls back to mov+reg-form otherwise. The
		// old explicit mov+and_ always paid the 2-op cost, even for trivial
		// masks like 0xFF/0xFFFF/single-bit/contiguous-run that the ISA
		// encodes directly. Same for orr_imm/eor_imm below. rlwinm's
		// MB..ME masks are contiguous bit ranges and almost always fit.
		and_imm(regR, regA, (uint32_t)immS32, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_OR)
	{
		orr_imm(regR, regA, (uint32_t)immS32, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_XOR)
	{
		eor_imm(regR, regA, (uint32_t)immS32, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_MULTIPLY_SIGNED)
	{
		mov(TEMP_GPR1.WReg, immS32);
		mul(regR, regA, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_LEFT_SHIFT)
	{
		lsl(regR, regA, (uint32)immS32 & 0x1f);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_RIGHT_SHIFT_U)
	{
		lsr(regR, regA, (uint32)immS32 & 0x1f);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_RIGHT_SHIFT_S)
	{
		asr(regR, regA, (uint32)immS32 & 0x1f);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_BFI ||
	         imlInstruction->operation == PPCREC_IML_OP_BFXIL)
	{
		// immS32 layout: bits[4:0] = lsb (0..31), bits[9:5] = width-1
		// (encoding widths 1..32). See IMLInstruction.h.
		uint32 lsb = (uint32)immS32 & 0x1f;
		uint32 width = (((uint32)immS32 >> 5) & 0x1f) + 1;
		if (imlInstruction->operation == PPCREC_IML_OP_BFI)
			bfi(regR, regA, lsb, width);
		else
			bfxil(regR, regA, lsb, width);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ARM64_UBFX)
	{
		// Same lsb/width-1 packing as BFI/BFXIL. ubfx extracts width bits
		// starting at bit lsb of regA, places them at bit 0 of regR, zero-
		// extends the rest. Single host op vs the assign+rotate+and-mask
		// chain the general rlwinm path would emit otherwise.
		uint32 lsb = (uint32)immS32 & 0x1f;
		uint32 width = (((uint32)immS32 >> 5) & 0x1f) + 1;
		ubfx(regR, regA, lsb, width);
	}
	else
	{
		cemuLog_log(LogType::Recompiler, "PPCRecompilerAArch64Gen_imlInstruction_r_r_s32(): Unsupported operation {:x}", imlInstruction->operation);
		cemu_assert_suspicious();
		return false;
	}
	return true;
}

bool AArch64GenContext_t::r_r_s32_carry(IMLInstruction* imlInstruction)
{
	WReg regR = gpReg<WReg>(imlInstruction->op_r_r_s32_carry.regR);
	WReg regA = gpReg<WReg>(imlInstruction->op_r_r_s32_carry.regA);
	WReg regCarry = gpReg<WReg>(imlInstruction->op_r_r_s32_carry.regCarry);

	sint32 immS32 = imlInstruction->op_r_r_s32_carry.immS32;
	if (imlInstruction->operation == PPCREC_IML_OP_ADD)
	{
		adds_imm(regR, regA, immS32, TEMP_GPR1.WReg);
		cset(regCarry, Cond::CS);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ADD_WITH_CARRY)
	{
		mov(TEMP_GPR1.WReg, immS32);
		cmp(regCarry, 1);
		adcs(regR, regA, TEMP_GPR1.WReg);
		cset(regCarry, Cond::CS);
	}
	else
	{
		cemu_assert_suspicious();
		return false;
	}

	return true;
}

bool AArch64GenContext_t::r_r_r(IMLInstruction* imlInstruction)
{
	WReg regResult = gpReg<WReg>(imlInstruction->op_r_r_r.regR);
	XReg reg64Result = aliasAs<XReg>(regResult);
	WReg regOperand1 = gpReg<WReg>(imlInstruction->op_r_r_r.regA);
	WReg regOperand2 = gpReg<WReg>(imlInstruction->op_r_r_r.regB);

	if (imlInstruction->operation == PPCREC_IML_OP_ADD)
	{
		add(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_SUB)
	{
		sub(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_OR)
	{
		orr(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_AND)
	{
		and_(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_XOR)
	{
		eor(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_MULTIPLY_SIGNED)
	{
		mul(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_SLW)
	{
		tst(regOperand2, 32);
		lsl(regResult, regOperand1, regOperand2);
		csel(regResult, regResult, wzr, Cond::EQ);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_SRW)
	{
		tst(regOperand2, 32);
		lsr(regResult, regOperand1, regOperand2);
		csel(regResult, regResult, wzr, Cond::EQ);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_LEFT_ROTATE)
	{
		neg(TEMP_GPR1.WReg, regOperand2);
		ror(regResult, regOperand1, TEMP_GPR1.WReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_RIGHT_SHIFT_S)
	{
		asr(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_RIGHT_SHIFT_U)
	{
		lsr(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_LEFT_SHIFT)
	{
		lsl(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_DIVIDE_SIGNED)
	{
		sdiv(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_DIVIDE_UNSIGNED)
	{
		udiv(regResult, regOperand1, regOperand2);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_MULTIPLY_HIGH_SIGNED)
	{
		smull(reg64Result, regOperand1, regOperand2);
		lsr(reg64Result, reg64Result, 32);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_MULTIPLY_HIGH_UNSIGNED)
	{
		umull(reg64Result, regOperand1, regOperand2);
		lsr(reg64Result, reg64Result, 32);
	}
	else
	{
		cemuLog_log(LogType::Recompiler, "PPCRecompilerAArch64Gen_imlInstruction_r_r_r(): Unsupported operation {:x}", imlInstruction->operation);
		return false;
	}
	return true;
}

bool AArch64GenContext_t::r_r_r_carry(IMLInstruction* imlInstruction)
{
	WReg regR = gpReg<WReg>(imlInstruction->op_r_r_r_carry.regR);
	WReg regA = gpReg<WReg>(imlInstruction->op_r_r_r_carry.regA);
	WReg regB = gpReg<WReg>(imlInstruction->op_r_r_r_carry.regB);
	WReg regCarry = gpReg<WReg>(imlInstruction->op_r_r_r_carry.regCarry);

	if (imlInstruction->operation == PPCREC_IML_OP_ADD)
	{
		adds(regR, regA, regB);
		cset(regCarry, Cond::CS);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_ADD_WITH_CARRY)
	{
		cmp(regCarry, 1);
		adcs(regR, regA, regB);
		cset(regCarry, Cond::CS);
	}
	else
	{
		cemu_assert_suspicious();
		return false;
	}

	return true;
}

Cond ImlFPCondToArm64Cond(IMLCondition cond); // forward decl — defined alongside fpr_compare

Cond ImlCondToArm64Cond(IMLCondition condition)
{
	switch (condition)
	{
	case IMLCondition::EQ:
		return Cond::EQ;
	case IMLCondition::NEQ:
		return Cond::NE;
	case IMLCondition::UNSIGNED_GT:
		return Cond::HI;
	case IMLCondition::UNSIGNED_LT:
		return Cond::LO;
	case IMLCondition::SIGNED_GT:
		return Cond::GT;
	case IMLCondition::SIGNED_LT:
		return Cond::LT;
	default:
	{
		cemu_assert_suspicious();
		return Cond::EQ;
	}
	}
}

void AArch64GenContext_t::compare(IMLInstruction* imlInstruction)
{
	WReg regR = gpReg<WReg>(imlInstruction->op_compare.regR);
	WReg regA = gpReg<WReg>(imlInstruction->op_compare.regA);
	WReg regB = gpReg<WReg>(imlInstruction->op_compare.regB);
	Cond cond = ImlCondToArm64Cond(imlInstruction->op_compare.cond);
	cmp(regA, regB);
	cset(regR, cond);
}

void AArch64GenContext_t::compare_s32(IMLInstruction* imlInstruction)
{
	WReg regR = gpReg<WReg>(imlInstruction->op_compare.regR);
	WReg regA = gpReg<WReg>(imlInstruction->op_compare.regA);
	sint32 imm = imlInstruction->op_compare_s32.immS32;
	auto cond = ImlCondToArm64Cond(imlInstruction->op_compare.cond);
	cmp_imm(regA, imm, TEMP_GPR1.WReg);
	cset(regR, cond);
}

void AArch64GenContext_t::cjump(IMLInstruction* imlInstruction, IMLSegment* imlSegment)
{
	auto regBool = gpReg<WReg>(imlInstruction->op_conditional_jump.registerBool);
	prepareJump(ConditionalRegJumpInfo{
		.target = imlSegment->nextSegmentBranchTaken,
		.regBool = regBool,
		.mustBeTrue = imlInstruction->op_conditional_jump.mustBeTrue,
	});
}

void AArch64GenContext_t::cjump_tbz(IMLInstruction* imlInstruction, IMLSegment* imlSegment)
{
	prepareJump(TbzJumpInfo{
		.target = imlSegment->nextSegmentBranchTaken,
		.regSrc = gpReg<WReg>(imlInstruction->op_arm64_tbz.regSrc),
		.bitIndex = imlInstruction->op_arm64_tbz.bitIndex,
		.mustBeZero = imlInstruction->op_arm64_tbz.mustBeZero,
	});
}

void AArch64GenContext_t::cjump_nzcv(IMLInstruction* imlInstruction, IMLSegment* imlSegment)
{
	IMLCondition imlCond = imlInstruction->op_arm64_nzcv_jcc.cond;
	// FP conditions (UNORDERED_GT..) need the FP cond mapping because ARM's flag
	// interpretation after fcmp differs from int cmp.
	Cond cond = (imlCond >= IMLCondition::UNORDERED_GT)
		? ImlFPCondToArm64Cond(imlCond)
		: ImlCondToArm64Cond(imlCond);
	if (imlInstruction->op_arm64_nzcv_jcc.invertedCondition)
		cond = static_cast<Cond>(static_cast<uint32_t>(cond) ^ 1u);
	prepareJump(NZCVJumpInfo{
		.target = imlSegment->nextSegmentBranchTaken,
		.cond = cond,
	});
}

void AArch64GenContext_t::jump(IMLSegment* imlSegment)
{
	prepareJump(UnconditionalJumpInfo{.target = imlSegment->nextSegmentBranchTaken});
}

void AArch64GenContext_t::conditionalJumpCycleCheck(IMLSegment* imlSegment)
{
	// w24 is live with PPCInterpreter_t::remainingCycles -- the test is just
	// "did the cumulative MACRO_COUNT_CYCLES push us negative yet?". Skips
	// the per-check ldr the legacy path needed.
	prepareJump(NegativeRegValueJumpInfo{
		.target = imlSegment->nextSegmentBranchTaken,
		.regValue = REMAINING_CYCLES_REG.WReg,
	});
}

void* PPCRecompiler_virtualHLE(PPCInterpreter_t* ppcInterpreter, uint32 hleFuncId)
{
	// The trailing PPCInterpreter_getCurrentInstance() used to return the post-call
	// TLS value in case a fiber switch had migrated us to a different per-core PPC
	// context. In HLE mode that can't happen: PPCInterpreter_setCurrentInstance is
	// only ever called with the per-core pointer or nullptr, and we're guaranteed to
	// be running on the same host thread (= same core) after hleCall returns. So the
	// TLS load is redundant and we can hand the input pointer back directly, saving
	// a tlsdesc_resolver_dynamic call per HLE invocation.
	void* prevRSPTemp = ppcInterpreter->rspTemp;
	if (hleFuncId == 0xFFD0)
	{
		ppcInterpreter->remainingCycles -= 500; // let subtract about 500 cycles for each HLE call
		ppcInterpreter->gpr[3] = 0;
		PPCInterpreter_nextInstruction(ppcInterpreter);
		return ppcInterpreter;
	}
	else
	{
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall != nullptr);
		hleCall(ppcInterpreter);
	}
	ppcInterpreter->rspTemp = prevRSPTemp;
	cemu_assert_debug(PPCInterpreter_getCurrentInstance() == ppcInterpreter);
	return ppcInterpreter;
}

bool AArch64GenContext_t::macro(IMLInstruction* imlInstruction)
{
	if (imlInstruction->operation == PPCREC_IML_MACRO_B_TO_REG)
	{
		WReg branchDstReg = gpReg<WReg>(imlInstruction->op_macro.paramReg);

		mov(TEMP_GPR1.WReg, offsetof(PPCRecompilerInstanceData_t, ppcRecompilerDirectJumpTable));
		add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, branchDstReg, ShMod::LSL, 1);
		ldr(TEMP_GPR1.XReg, AdrExt(PPC_REC_INSTANCE_REG, TEMP_GPR1.WReg, ExtMod::UXTW));
		mov(LR.WReg, branchDstReg);
		br(TEMP_GPR1.XReg);
		return true;
	}
	else if (imlInstruction->operation == PPCREC_IML_MACRO_BL)
	{
		uint32 newLR = imlInstruction->op_macro.param + 4;

		mov(TEMP_GPR1.WReg, newLR);
		str(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));

		uint32 newIP = imlInstruction->op_macro.param2;
		uint64 lookupOffset = (uint64)offsetof(PPCRecompilerInstanceData_t, ppcRecompilerDirectJumpTable) + (uint64)newIP * 2ULL;
		mov(TEMP_GPR1.XReg, lookupOffset);
		ldr(TEMP_GPR1.XReg, AdrReg(PPC_REC_INSTANCE_REG, TEMP_GPR1.XReg));
		mov(LR.WReg, newIP);
		br(TEMP_GPR1.XReg);
		return true;
	}
	else if (imlInstruction->operation == PPCREC_IML_MACRO_B_FAR)
	{
		uint32 newIP = imlInstruction->op_macro.param2;
		uint64 lookupOffset = (uint64)offsetof(PPCRecompilerInstanceData_t, ppcRecompilerDirectJumpTable) + (uint64)newIP * 2ULL;
		mov(TEMP_GPR1.XReg, lookupOffset);
		ldr(TEMP_GPR1.XReg, AdrReg(PPC_REC_INSTANCE_REG, TEMP_GPR1.XReg));
		mov(LR.WReg, newIP);
		br(TEMP_GPR1.XReg);
		return true;
	}
	else if (imlInstruction->operation == PPCREC_IML_MACRO_LEAVE)
	{
		uint32 currentInstructionAddress = imlInstruction->op_macro.param;
		mov(TEMP_GPR1.XReg, (uint64)offsetof(PPCRecompilerInstanceData_t, ppcRecompilerDirectJumpTable)); // newIP = 0 special value for recompiler exit
		ldr(TEMP_GPR1.XReg, AdrReg(PPC_REC_INSTANCE_REG, TEMP_GPR1.XReg));
		mov(LR.WReg, currentInstructionAddress);
		br(TEMP_GPR1.XReg);
		return true;
	}
	else if (imlInstruction->operation == PPCREC_IML_MACRO_DEBUGBREAK)
	{
		brk(0xf000);
		return true;
	}
	else if (imlInstruction->operation == PPCREC_IML_MACRO_COUNT_CYCLES)
	{
		uint32 cycleCount = imlInstruction->op_macro.param;
		// w24 (REMAINING_CYCLES_REG) holds remainingCycles for the whole JIT
		// session -- decrement is a single sub_imm against the live register.
		// Was ldr + sub_imm + str against PPCInterpreter_t::remainingCycles
		// at every basic block entry (one of the most-emitted IML macros).
		sub_imm(REMAINING_CYCLES_REG.WReg, REMAINING_CYCLES_REG.WReg, cycleCount, TEMP_GPR1.WReg);
		return true;
	}
	else if (imlInstruction->operation == PPCREC_IML_MACRO_HLE)
	{
		uint32 ppcAddress = imlInstruction->op_macro.param;
		uint32 funcId = imlInstruction->op_macro.param2;
		Label cyclesLeftLabel;

		// update instruction pointer
		mov(TEMP_GPR1.WReg, ppcAddress);
		str(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
		// Flush the in-register cycle counter to PPCInterpreter_t before any
		// C++ call -- PPCRecompiler_virtualHLE reads/writes remainingCycles
		// directly (-= 500 per HLE), and we don't want to clobber that with
		// the cached register value on the way out. We reload below after
		// the call returns.
		str(REMAINING_CYCLES_REG.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, remainingCycles)));
		// set parameters
		str(x30, AdrPreImm(sp, -16));

		// Fast path: when the HLE id is OSSendMessage / OSReceiveMessage, skip the
		// PPCRecompiler_virtualHLE → cafeExportCallWrapper dispatch chain AND inline
		// the function body itself. Same signature for both:
		//   (OSMessageQueue*, OSMessage*, uint32 flags) -> int
		//
		// Inlined fast path covers the queue-not-empty/not-full case. Pending waiters
		// on the opposite side are handled inline by snapshotting the slot counter,
		// completing the dequeue/enqueue + unlock, then calling a small wake helper
		// (OSWakeOneSender/Receiver) only if the snapshot was non-zero. Bails to the
		// C++ slow path when:
		//   - msgQueue is NULL or is the system message queue
		//   - the spinlock CAS finds the lock already held
		//   - the queue is empty/full AND BLOCK flag is set (caller needs to sleep)
		//   - Send is asked for HIGH_PRIORITY insertion (complex)
		//
		// PPCInterpreter_t::gpr[] is plain uint32 (native endian). OSMessageQueue and
		// OSMessage fields are uint32be — we byteswap on every load/store of those.
		bool emittedDirectCall = false;
		if (funcId == (uint32)coreinit::g_hleIdx_OSSendMessage ||
		    funcId == (uint32)coreinit::g_hleIdx_OSReceiveMessage)
		{
			const bool isReceive = (funcId == (uint32)coreinit::g_hleIdx_OSReceiveMessage);
			Label slowCall, slowUnlock, fastDone;

			// ===== translate guest args to host pointers (x0 = msgQueue, x1 = msg, w2 = flags) =====
			ldr(w0, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
			cmp(w0, 0);
			add(x0, MEM_BASE_REG, x0, ExtMod::UXTW);
			csel(x0, xzr, x0, Cond::EQ);
			ldr(w1, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 4));
			cmp(w1, 0);
			add(x1, MEM_BASE_REG, x1, ExtMod::UXTW);
			csel(x1, xzr, x1, Cond::EQ);
			ldr(w2, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 5));

			// ===== bail-out conditions before locking =====
			cbz(x0, slowCall);                                              // NULL queue
			mov(x9, (uint64)&coreinit::g_systemMessageQueuePtr);
			ldr(x9, AdrNoOfs(x9));
			cmp(x0, x9);
			beq(slowCall);                                                  // system queue

			// ===== hash queue addr to slot index, compute &slot =====
			// h = ((uintptr_t)q >> 4) * 0x9E3779B97F4A7C15ULL;
			// slot = pool[(h >> QUEUE_LOCK_POOL_INDEX_SHIFT) & (QUEUE_LOCK_POOL_SIZE - 1)]
			// shift = 64 - log2(POOL_SIZE) so the top log2(POOL_SIZE) bits of the
			// multiplicative hash become the slot index. Mask is implicit: the top
			// bits of a 64-bit shift-right are guaranteed in range when extracted.
			lsr(x10, x0, 4);
			mov(x9, (uint64)0x9E3779B97F4A7C15ULL);
			mul(x10, x10, x9);
			lsr(x10, x10, coreinit::QUEUE_LOCK_POOL_INDEX_SHIFT);            // x10 = slot index
			mov(x9, (uint64)&coreinit::g_queueLockPool[0]);
			add(x9, x9, x10, ShMod::LSL, 4);                                // x9 = &slot (size 16)

			// ===== atomic-OR lockState bit 0; prior value also carries waiter bits =====
			// LDSETA = atomic load + bitwise-or with acquire ordering. One round-trip
			// yields both the lock-acquisition outcome (was bit 0 zero?) and the
			// opposite-side waiter snapshot (bits 1..15 for recv waiters / 16..30 for
			// send waiters), eliminating the post-CAS dependent LDR that previously
			// dominated cycle attribution on this path.
			//
			// Waiter bits are bumped by the slow path UNDER qlock before releasing —
			// the acquire ordering here pairs with that release. We can't substitute
			// a post-unlock atomic load on the wait-queue head pointer: the slow
			// path's storeHeadRelease happens later under the writer lock, after
			// the waiter bump but before the actual sleep. A peer reading head in
			// that window would see null, skip the wake, and the slow-path waiter
			// — having missed our enqueue at its writer-lock bail-check (racy with
			// our non-atomic usedCount write) — would sleep with no waker. The
			// packed waiter bits are the only signal synchronized through qlock
			// with the enqueue, so they're the only safe wake-decision predicate.
			mov(w11, coreinit::QueueLockSlot::LOCKED_BIT);
			ldseta(w11, w10, AdrNoOfs(x9));                                 // w10 = prior packed
			tbnz(w10, 0, slowCall);                                         // prior lock bit set → contended
			// Mask opposite-side waiter bits into w16; non-zero ⇒ wake needed later.
			and_(w16, w10,
			     isReceive ? coreinit::QueueLockSlot::SEND_WAITER_MASK
			               : coreinit::QueueLockSlot::RECV_WAITER_MASK);

			// ===== load queue fields with ldp consolidation =====
			// firstIndex (offset 0x34) + usedCount (offset 0x38) are adjacent — one
			// ldp loads both. usedCount is BE-encoded but uint32be==0 has the same
			// byte pattern in any endianness, so cbz works directly on the BE value;
			// only revswap when we need to do arithmetic on it.
			ldp(w11, w10, AdrImm(x0, offsetof(coreinit::OSMessageQueue, firstIndex)));

			if (isReceive)
			{
				Label dequeue;
				cbnz(w10, dequeue);                                         // non-empty → dequeue (BE 0 == LE 0)

				// empty: if BLOCK set, slow path waits; else return false
				tbnz(w2, 0, slowUnlock);                                    // bit 0 = OS_MESSAGE_BLOCK
				str(wzr, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
				// Unlock: atomic clear of LOCKED_BIT with release, preserving waiter bits.
				mov(w11, coreinit::QueueLockSlot::LOCKED_BIT);
				ldclrl(w11, wzr, AdrNoOfs(x9));
				b(fastDone);

				L(dequeue);
				// msgArray (0x2C) + msgCount (0x30) via ldp
				ldp(w13, w12, AdrImm(x0, offsetof(coreinit::OSMessageQueue, msgArray)));
				rev(w10, w10);                                              // usedCount native
				rev(w11, w11);                                              // firstIndex native
				rev(w12, w12);                                              // msgCount native
				rev(w13, w13);                                              // msgArray native
				add(x13, MEM_BASE_REG, x13, ExtMod::UXTW);                   // host ptr to msgArray
				add(x13, x13, x11, ShMod::LSL, 4);                           // &msgArray[firstIndex] (16B stride)
				ldp(x14, x15, AdrNoOfs(x13));                                // copy 16B (preserves BE layout)
				stp(x14, x15, AdrNoOfs(x1));
				// firstIndex = (firstIndex + 1) % msgCount (firstIndex < msgCount, so +1 ≤ msgCount)
				add(w11, w11, 1);
				cmp(w11, w12);
				csel(w11, wzr, w11, Cond::EQ);
				rev(w11, w11);
				// usedCount -= 1
				sub(w10, w10, 1);
				rev(w10, w10);
				// Store updated firstIndex + usedCount in one stp (adjacent, same offsets as load)
				stp(w11, w10, AdrImm(x0, offsetof(coreinit::OSMessageQueue, firstIndex)));
				// gpr[3] = 1 (stored before wake call so the helper's clobbers don't matter)
				mov(w11, 1);
				str(w11, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
				// Unlock: atomic clear of LOCKED_BIT (w11 == 1) with release. Waiters
				// in upper bits survive untouched, so the next acquirer sees them.
				ldclrl(w11, wzr, AdrNoOfs(x9));
				// If a send-side waiter existed at snapshot time, wake one.
				// x0 still holds msgQueue.
				cbz(w16, fastDone);
				mov(TEMP_GPR1.XReg, (uint64)&coreinit::OSWakeOneSender);
				blr(TEMP_GPR1.XReg);
				b(fastDone);
			}
			else
			{
				Label enqueue;
				// Load msgArray (0x2C) + msgCount (0x30) via ldp
				ldp(w13, w12, AdrImm(x0, offsetof(coreinit::OSMessageQueue, msgArray)));
				// Compare BE values directly: usedCount < msgCount. Both are small
				// non-negative uint32; raw BE bytes preserve unsigned ordering when
				// both high 24 bits are zero (which is always the case for these
				// fields in practice — Wii U queues never exceed ~256 entries).
				cmp(w10, w12);
				blo(enqueue);                                               // usedCount < msgCount → enqueue (unsigned)

				// full: if BLOCK set, slow path waits; else return false
				tbnz(w2, 0, slowUnlock);
				str(wzr, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
				mov(w11, coreinit::QueueLockSlot::LOCKED_BIT);
				ldclrl(w11, wzr, AdrNoOfs(x9));
				b(fastDone);

				L(enqueue);
				// HIGH_PRIORITY uses backward firstIndex rotation — leave that to the C++ path
				tbnz(w2, 1, slowUnlock);                                    // bit 1 = OS_MESSAGE_HIGH_PRIORITY
				// Need native values for arithmetic
				rev(w10, w10);                                              // usedCount native
				rev(w11, w11);                                              // firstIndex native (was w12 in old code; now in w11 from ldp)
				rev(w12, w12);                                              // msgCount native
				rev(w13, w13);                                              // msgArray native
				// messageIndex = (firstIndex + usedCount) mod msgCount
				// firstIndex < msgCount && usedCount < msgCount → sum < 2*msgCount, so one subtract suffices
				add(w14, w11, w10);
				sub(w15, w14, w12);
				cmp(w14, w12);
				csel(w14, w15, w14, Cond::GE);                              // w14 = messageIndex
				add(x13, MEM_BASE_REG, x13, ExtMod::UXTW);
				add(x13, x13, x14, ShMod::LSL, 4);                          // &msgArray[messageIndex]
				ldp(x14, x15, AdrNoOfs(x1));                                // load msg
				stp(x14, x15, AdrNoOfs(x13));                               // store into slot
				// usedCount += 1
				add(w10, w10, 1);
				rev(w10, w10);
				str(w10, AdrUimm(x0, offsetof(coreinit::OSMessageQueue, usedCount)));
				// gpr[3] = 1 (stored before wake call so the helper's clobbers don't matter)
				mov(w11, 1);
				str(w11, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
				// Unlock: atomic clear of LOCKED_BIT (w11 == 1) with release.
				ldclrl(w11, wzr, AdrNoOfs(x9));
				// If a receive-side waiter existed at snapshot time, wake one.
				cbz(w16, fastDone);
				mov(TEMP_GPR1.XReg, (uint64)&coreinit::OSWakeOneReceiver);
				blr(TEMP_GPR1.XReg);
				b(fastDone);
			}

			L(slowUnlock);
			// Release lock before C++ slow path. Atomic clear of LOCKED_BIT only —
			// any waiter counters in upper bits stay live for the slow path to read.
			mov(w11, coreinit::QueueLockSlot::LOCKED_BIT);
			ldclrl(w11, wzr, AdrNoOfs(x9));
			// fall through

			L(slowCall);
			{
				uint64 target = isReceive ? (uint64)&coreinit::OSReceiveMessage
				                          : (uint64)&coreinit::OSSendMessage;
				mov(TEMP_GPR1.XReg, target);
				blr(TEMP_GPR1.XReg);
				str(w0, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
			}

			L(fastDone);
			// instructionPointer = LR (cafeExportCallWrapper does this; mirror it here)
			ldr(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));
			str(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
			emittedDirectCall = true;
		}
		else if (funcId == (uint32)coreinit::g_hleIdx_OSGetCoreId)
		{
			// Inline OSGetCoreId: returns hCPU->spr.UPIR. Saves the
			// PPCRecompiler_virtualHLE + cafeExportCallWrapper round-trip; game
			// callers use this in mutex/affinity hot paths.
			ldr(w0, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.UPIR)));
			str(w0, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
			ldr(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));
			str(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
			emittedDirectCall = true;
		}
		else if (funcId == (uint32)coreinit::g_hleIdx_OSGetCurrentThread)
		{
			// Inline OSGetCurrentThread: returns __currentCoreThread[hCPU->spr.UPIR]
			// converted to a guest MPTR. Game uses this in every mutex/event
			// owner-check; bypassing the C++ wrapper avoids the host-ptr<->MPTR
			// conversion overhead in cafeExportCallWrapper.
			ldr(w10, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.UPIR)));
			mov(TEMP_GPR1.XReg, (uint64)&coreinit::__currentCoreThread[0]);
			ldr(x11, AdrExt(TEMP_GPR1.XReg, w10, ExtMod::UXTW, 3));        // host ptr (sizeof OSThread_t* == 8)
			cmp(x11, 0);
			sub(x12, x11, MEM_BASE_REG);                                    // guest MPTR
			csel(w12, wzr, w12, Cond::EQ);                                  // null host → 0 MPTR
			str(w12, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, gpr) + sizeof(uint32) * 3));
			ldr(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));
			str(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
			emittedDirectCall = true;
		}
		else if (funcId == (uint32)coreinit::g_hleIdx_DCInvalidateRange)
		{
			// Inline DCInvalidateRange: no-op in Cemu (the function body only does
			// dead arithmetic; there's no real CPU cache to invalidate and the
			// LatteBufferCache hook is commented out). Drop the call entirely.
			ldr(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, spr.LR)));
			str(TEMP_GPR1.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
			emittedDirectCall = true;
		}

		if (!emittedDirectCall)
		{
			mov(x0, HCPU_REG);
			mov(w1, funcId);
			// call HLE function

			mov(TEMP_GPR1.XReg, (uint64)PPCRecompiler_virtualHLE);
			blr(TEMP_GPR1.XReg);

			mov(HCPU_REG, x0);
		}

		ldr(x30, AdrPostImm(sp, 16));

		// Refill the cycle counter cache after the HLE call: virtualHLE
		// (or the wake-helper / native OSSend|Receive in the inline path)
		// may have modified PPCInterpreter_t::remainingCycles. After the
		// fiber-aware HCPU reload above this points at the correct context.
		ldr(REMAINING_CYCLES_REG.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, remainingCycles)));
		tbz(REMAINING_CYCLES_REG.WReg, 31, cyclesLeftLabel); // check if negative

		mov(TEMP_GPR1.XReg, offsetof(PPCRecompilerInstanceData_t, ppcRecompilerDirectJumpTable));
		ldr(TEMP_GPR1.XReg, AdrReg(PPC_REC_INSTANCE_REG, TEMP_GPR1.XReg));
		ldr(LR.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
		// branch to recompiler exit
		br(TEMP_GPR1.XReg);

		L(cyclesLeftLabel);
		// check if instruction pointer was changed
		// assign new instruction pointer to LR.WReg
		ldr(LR.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
		mov(TEMP_GPR1.XReg, offsetof(PPCRecompilerInstanceData_t, ppcRecompilerDirectJumpTable));
		add(TEMP_GPR1.XReg, TEMP_GPR1.XReg, LR.XReg, ShMod::LSL, 1);
		ldr(TEMP_GPR1.XReg, AdrReg(PPC_REC_INSTANCE_REG, TEMP_GPR1.XReg));
		// branch to [ppcRecompilerDirectJumpTable + PPCInterpreter_t::instructionPointer * 2]
		br(TEMP_GPR1.XReg);
		return true;
	}
	else
	{
		cemuLog_log(LogType::Recompiler, "Unknown recompiler macro operation %d\n", imlInstruction->operation);
		cemu_assert_suspicious();
	}
	return false;
}

bool AArch64GenContext_t::load(IMLInstruction* imlInstruction, bool indexed)
{
	cemu_assert_debug(imlInstruction->op_storeLoad.registerData.GetRegFormat() == IMLRegFormat::I32);
	cemu_assert_debug(imlInstruction->op_storeLoad.registerMem.GetRegFormat() == IMLRegFormat::I32);
	if (indexed)
		cemu_assert_debug(imlInstruction->op_storeLoad.registerMem2.GetRegFormat() == IMLRegFormat::I32);

	sint32 memOffset = imlInstruction->op_storeLoad.immS32;
	bool signExtend = imlInstruction->op_storeLoad.flags2.signExtend;
	bool switchEndian = imlInstruction->op_storeLoad.flags2.swapEndian;
	WReg memReg = gpReg<WReg>(imlInstruction->op_storeLoad.registerMem);
	WReg dataReg = gpReg<WReg>(imlInstruction->op_storeLoad.registerData);

	// Skip add_imm when offset is zero — for non-indexed loads we can use memReg
	// directly as the LDR index register, and for indexed loads we can fold the
	// (mem + mem2 + 0) chain into a single add. PPC has many `lwz r, 0(rN)`-style
	// accesses, so this saves an instruction on a hot path.
	WReg adrIdx = TEMP_GPR1.WReg;
	if (memOffset == 0)
	{
		if (indexed)
			add(TEMP_GPR1.WReg, memReg, gpReg<WReg>(imlInstruction->op_storeLoad.registerMem2));
		else
			adrIdx = memReg;
	}
	else
	{
		add_imm(TEMP_GPR1.WReg, memReg, memOffset, TEMP_GPR1.WReg);
		if (indexed)
			add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, gpReg<WReg>(imlInstruction->op_storeLoad.registerMem2));
	}

	auto adr = AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW);
	if (imlInstruction->op_storeLoad.copyWidth == 32)
	{
		ldr(dataReg, adr);
		if (switchEndian)
			rev(dataReg, dataReg);
	}
	else if (imlInstruction->op_storeLoad.copyWidth == 16)
	{
		if (switchEndian)
		{
			// ldrh zero-extends to 32b, then rev16 byte-swaps bytes within each
			// halfword. The low halfword is now the byte-swapped value and the
			// upper 16 bits stay 0, so for lhz this is already the final result —
			// drops the lsr from the old ldrh+rev+lsr-16 form. lha still needs a
			// sxth, but that's a 1-uop, low-latency fixup with no awkward shift
			// dependency on the rev output.
			ldrh(dataReg, adr);
			rev16(dataReg, dataReg);
			if (signExtend)
				sxth(dataReg, dataReg);
		}
		else
		{
			if (signExtend)
				ldrsh(dataReg, adr);
			else
				ldrh(dataReg, adr);
		}
	}
	else if (imlInstruction->op_storeLoad.copyWidth == 8)
	{
		if (signExtend)
			ldrsb(dataReg, adr);
		else
			ldrb(dataReg, adr);
	}
	else
	{
		return false;
	}
	return true;
}

bool AArch64GenContext_t::tryFuseLoadPair(IMLInstruction* a, IMLInstruction* b)
{
	// Both must be plain non-indexed 32-bit loads.
	if (a->type != PPCREC_IML_TYPE_LOAD || b->type != PPCREC_IML_TYPE_LOAD)
		return false;
	if (a->op_storeLoad.copyWidth != 32 || b->op_storeLoad.copyWidth != 32)
		return false;
	if (a->op_storeLoad.flags2.swapEndian != b->op_storeLoad.flags2.swapEndian)
		return false;
	// 32-bit loads ignore signExtend, but be conservative.
	if (a->op_storeLoad.flags2.signExtend || b->op_storeLoad.flags2.signExtend)
		return false;
	// Same base register.
	if (a->op_storeLoad.registerMem.GetRegID() != b->op_storeLoad.registerMem.GetRegID())
		return false;
	// +4 stride in increasing address order. (LDP semantics: Wt1 at offset+0,
	// Wt2 at offset+4.)
	sint32 offA = a->op_storeLoad.immS32;
	sint32 offB = b->op_storeLoad.immS32;
	if (offB != offA + 4)
		return false;
	// LDP immediate field is signed imm7 scaled by 4 → range [-256, +252],
	// must be a multiple of 4. Stride-4 pairs are word-aligned to each other,
	// but the first offset itself isn't guaranteed to be 4-aligned (rare, but
	// we have to check before encoding).
	if (offA < -256 || offA > 252 || (offA & 3) != 0)
		return false;
	// Destination IMLRegs must be different and map to consecutive AArch64 host
	// registers in the order they will be loaded (Wt1 lower index, Wt2 higher).
	IMLReg dataA = a->op_storeLoad.registerData;
	IMLReg dataB = b->op_storeLoad.registerData;
	if (dataA.GetRegID() == dataB.GetRegID())
		return false; // AArch64 LDP requires distinct dest regs
	WReg wDataA = gpReg<WReg>(dataA);
	WReg wDataB = gpReg<WReg>(dataB);
	if (wDataA.getIdx() + 1 != wDataB.getIdx())
		return false;
	// The base PPC reg must not be one of the destinations (else the LDP would
	// overwrite the base before reading both halves — actually LDP reads the
	// base first so technically safe, but we leave the dest=base case to the
	// per-instruction lowering for clarity / safety).
	WReg wBase = gpReg<WReg>(a->op_storeLoad.registerMem);
	if (wBase.getIdx() == wDataA.getIdx() || wBase.getIdx() == wDataB.getIdx())
		return false;
	// Emit:  add Xtmp, MEM_BASE, Wbase, UXTW
	//        ldp Wa, Wb, [Xtmp, #offA]
	//        rev Wa, Wa   (if swap)
	//        rev Wb, Wb   (if swap)
	add(TEMP_GPR1.XReg, MEM_BASE_REG, wBase, ExtMod::UXTW);
	ldp(wDataA, wDataB, AdrImm(TEMP_GPR1.XReg, offA));
	if (a->op_storeLoad.flags2.swapEndian)
	{
		rev(wDataA, wDataA);
		rev(wDataB, wDataB);
	}
	return true;
}

bool AArch64GenContext_t::store(IMLInstruction* imlInstruction, bool indexed)
{
	cemu_assert_debug(imlInstruction->op_storeLoad.registerData.GetRegFormat() == IMLRegFormat::I32);
	cemu_assert_debug(imlInstruction->op_storeLoad.registerMem.GetRegFormat() == IMLRegFormat::I32);
	if (indexed)
		cemu_assert_debug(imlInstruction->op_storeLoad.registerMem2.GetRegFormat() == IMLRegFormat::I32);

	WReg dataReg = gpReg<WReg>(imlInstruction->op_storeLoad.registerData);
	WReg memReg = gpReg<WReg>(imlInstruction->op_storeLoad.registerMem);
	sint32 memOffset = imlInstruction->op_storeLoad.immS32;
	bool swapEndian = imlInstruction->op_storeLoad.flags2.swapEndian;

	WReg adrIdx = TEMP_GPR1.WReg;
	if (memOffset == 0)
	{
		if (indexed)
			add(TEMP_GPR1.WReg, memReg, gpReg<WReg>(imlInstruction->op_storeLoad.registerMem2));
		else
			adrIdx = memReg;
	}
	else
	{
		add_imm(TEMP_GPR1.WReg, memReg, memOffset, TEMP_GPR1.WReg);
		if (indexed)
			add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, gpReg<WReg>(imlInstruction->op_storeLoad.registerMem2));
	}
	AdrExt adr = AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW);
	if (imlInstruction->op_storeLoad.copyWidth == 32)
	{
		if (swapEndian)
		{
			rev(TEMP_GPR2.WReg, dataReg);
			str(TEMP_GPR2.WReg, adr);
		}
		else
		{
			str(dataReg, adr);
		}
	}
	else if (imlInstruction->op_storeLoad.copyWidth == 16)
	{
		if (swapEndian)
		{
			// rev32 + lsr-16 was needed because rev swaps all 4 bytes of the
			// register; rev16 byte-swaps within each halfword, so the low 16
			// bits hold the byte-swapped low halfword directly. strh only reads
			// the low 16 bits, so the upper-halfword content doesn't matter.
			// Drops one instruction off every PPC sth.
			rev16(TEMP_GPR2.WReg, dataReg);
			strh(TEMP_GPR2.WReg, adr);
		}
		else
		{
			strh(dataReg, adr);
		}
	}
	else if (imlInstruction->op_storeLoad.copyWidth == 8)
	{
		strb(dataReg, adr);
	}
	else
	{
		return false;
	}
	return true;
}

bool AArch64GenContext_t::tryFuseStorePair(IMLInstruction* a, IMLInstruction* b)
{
	// Symmetric to tryFuseLoadPair, with one twist: when swapEndian is set
	// (the common case for PPC stores into Wii U BE memory), we'd normally
	// need two scratch GPRs to hold the byte-swapped source values, but we
	// only have TEMP_GPR1 (used for the address) and TEMP_GPR2 left. Use
	// NEON instead: pack both 32-bit values into a vector register, rev32
	// each lane, then str D — uses only TEMP_FPR plus TEMP_GPR1.
	if (a->type != PPCREC_IML_TYPE_STORE || b->type != PPCREC_IML_TYPE_STORE)
		return false;
	if (a->op_storeLoad.copyWidth != 32 || b->op_storeLoad.copyWidth != 32)
		return false;
	if (a->op_storeLoad.flags2.swapEndian != b->op_storeLoad.flags2.swapEndian)
		return false;
	if (a->op_storeLoad.registerMem.GetRegID() != b->op_storeLoad.registerMem.GetRegID())
		return false;
	sint32 offA = a->op_storeLoad.immS32;
	sint32 offB = b->op_storeLoad.immS32;
	if (offB != offA + 4)
		return false;
	// STP-W and the NEON-shuffle STUR-D path have different encodable ranges,
	// so the per-path range check happens below after we know which one we'll
	// emit. Common precondition: offA must be a multiple of 4 for STP-W; for
	// STUR-D it can be any byte offset in [-256, +255]. PPC word stores are
	// always 4-aligned, but be defensive.
	if ((offA & 3) != 0)
		return false;
	IMLReg dataA = a->op_storeLoad.registerData;
	IMLReg dataB = b->op_storeLoad.registerData;
	// STP requires distinct source registers (the architecture allows it but
	// xbyak rejects identical Wt1/Wt2 for non-WBACK form).
	if (dataA.GetRegID() == dataB.GetRegID())
		return false;
	WReg wDataA = gpReg<WReg>(dataA);
	WReg wDataB = gpReg<WReg>(dataB);
	bool swap = a->op_storeLoad.flags2.swapEndian;
	if (!swap)
	{
		// No-swap path needs consecutive source host regs for STP — the
		// instruction takes two register operands by name. The byte-swap
		// path below does NOT require this because we shuffle via NEON.
		if (wDataA.getIdx() + 1 != wDataB.getIdx())
			return false;
		// STP-W: signed imm7 scaled by 4 → [-256, +252].
		if (offA < -256 || offA > 252)
			return false;
	}
	else
	{
		// STUR-D imm9 range, any byte alignment.
		if (offA < -256 || offA > 255)
			return false;
	}
	WReg wBase = gpReg<WReg>(a->op_storeLoad.registerMem);
	// Compute the host address once.
	add(TEMP_GPR1.XReg, MEM_BASE_REG, wBase, ExtMod::UXTW);
	if (swap)
	{
		// Pack [Wa, Wb] into TEMP_FPR.D, byte-reverse each 32-bit lane, stur D.
		// fmov to SReg writes lane 0 of V and clears the upper bits; mov into
		// the s2[1] element view writes lane 1 without touching lane 0.
		// Use stur (unscaled, signed imm9) rather than str — str(DReg, AdrImm)
		// silently converts to the unsigned-offset form and aborts when offA
		// is negative or not 8-aligned.
		fmov(TEMP_FPR.SReg, wDataA);
		mov(TEMP_FPR.VReg.s2[1], wDataB);
		rev32(TEMP_FPR.VReg.b8, TEMP_FPR.VReg.b8);
		stur(TEMP_FPR.DReg, AdrImm(TEMP_GPR1.XReg, offA));
	}
	else
	{
		stp(wDataA, wDataB, AdrImm(TEMP_GPR1.XReg, offA));
	}
	return true;
}

void AArch64GenContext_t::atomic_cmp_store(IMLInstruction* imlInstruction)
{
	WReg outReg = gpReg<WReg>(imlInstruction->op_atomic_compare_store.regBoolOut);
	WReg eaReg = gpReg<WReg>(imlInstruction->op_atomic_compare_store.regEA);
	WReg valReg = gpReg<WReg>(imlInstruction->op_atomic_compare_store.regWriteValue);
	WReg cmpValReg = gpReg<WReg>(imlInstruction->op_atomic_compare_store.regCompareValue);

	if (s_cpu.isAtomicSupported())
	{
		mov(TEMP_GPR2.WReg, cmpValReg);
		add(TEMP_GPR1.XReg, MEM_BASE_REG, eaReg, ExtMod::UXTW);
		casal(TEMP_GPR2.WReg, valReg, AdrNoOfs(TEMP_GPR1.XReg));
		cmp(TEMP_GPR2.WReg, cmpValReg);
		cset(outReg, Cond::EQ);
	}
	else
	{
		Label notEqual;
		Label storeFailed;

		add(TEMP_GPR1.XReg, MEM_BASE_REG, eaReg, ExtMod::UXTW);
		L(storeFailed);
		ldaxr(TEMP_GPR2.WReg, AdrNoOfs(TEMP_GPR1.XReg));
		cmp(TEMP_GPR2.WReg, cmpValReg);
		bne(notEqual);
		stlxr(TEMP_GPR2.WReg, valReg, AdrNoOfs(TEMP_GPR1.XReg));
		cbnz(TEMP_GPR2.WReg, storeFailed);

		L(notEqual);
		cset(outReg, Cond::EQ);
	}
}

bool AArch64GenContext_t::fpr_load(IMLInstruction* imlInstruction, bool indexed)
{
	const IMLReg& dataReg = imlInstruction->op_storeLoad.registerData;
	SReg dataSReg = fpReg<SReg>(dataReg);
	DReg dataDReg = fpReg<DReg>(dataReg);
	WReg realRegisterMem = gpReg<WReg>(imlInstruction->op_storeLoad.registerMem);
	WReg indexReg = indexed ? gpReg<WReg>(imlInstruction->op_storeLoad.registerMem2) : wzr;
	sint32 adrOffset = imlInstruction->op_storeLoad.immS32;
	uint8 mode = imlInstruction->op_storeLoad.mode;

	// Compute the LDR index register. Skip add_imm when offset is 0 — same hot-path
	// optimization as integer load/store. Saves one ARM instruction per FP load.
	WReg adrIdx = TEMP_GPR1.WReg;
	if (adrOffset == 0)
	{
		if (indexed)
			add(TEMP_GPR1.WReg, realRegisterMem, indexReg);
		else
			adrIdx = realRegisterMem;
	}
	else
	{
		add_imm(TEMP_GPR1.WReg, realRegisterMem, adrOffset, TEMP_GPR1.WReg);
		if (indexed)
			add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, indexReg);
	}

	if (mode == PPCREC_FPR_LD_MODE_SINGLE)
	{
		ldr(TEMP_GPR2.WReg, AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW));
		rev(TEMP_GPR2.WReg, TEMP_GPR2.WReg);
		fmov(dataSReg, TEMP_GPR2.WReg);

		if (imlInstruction->op_storeLoad.flags2.notExpanded)
		{
			// leave value as single
		}
		else
		{
			fcvt(dataDReg, dataSReg);
		}
	}
	else if (mode == PPCREC_FPR_LD_MODE_DOUBLE)
	{
		ldr(TEMP_GPR2.XReg, AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW));
		rev(TEMP_GPR2.XReg, TEMP_GPR2.XReg);
		fmov(dataDReg, TEMP_GPR2.XReg);
	}
	else
	{
		return false;
	}
	return true;
}

// store to memory
bool AArch64GenContext_t::fpr_store(IMLInstruction* imlInstruction, bool indexed)
{
	const IMLReg& dataImlReg = imlInstruction->op_storeLoad.registerData;
	DReg dataDReg = fpReg<DReg>(dataImlReg);
	SReg dataSReg = fpReg<SReg>(dataImlReg);
	WReg memReg = gpReg<WReg>(imlInstruction->op_storeLoad.registerMem);
	WReg indexReg = indexed ? gpReg<WReg>(imlInstruction->op_storeLoad.registerMem2) : wzr;
	sint32 memOffset = imlInstruction->op_storeLoad.immS32;
	uint8 mode = imlInstruction->op_storeLoad.mode;

	// Compute STR index register once. Skip add_imm when offset is 0 — same hot-path
	// optimization as integer load/store. Saves one ARM instruction per FP store.
	WReg adrIdx = TEMP_GPR1.WReg;
	if (memOffset == 0)
	{
		if (indexed)
			add(TEMP_GPR1.WReg, memReg, indexReg);
		else
			adrIdx = memReg;
	}
	else
	{
		add_imm(TEMP_GPR1.WReg, memReg, memOffset, TEMP_GPR1.WReg);
		if (indexed)
			add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, indexReg);
	}

	if (mode == PPCREC_FPR_ST_MODE_SINGLE)
	{
		if (imlInstruction->op_storeLoad.flags2.notExpanded)
		{
			// value is already in single format
			fmov(TEMP_GPR2.WReg, dataSReg);
		}
		else
		{
			fcvt(TEMP_FPR.SReg, dataDReg);
			fmov(TEMP_GPR2.WReg, TEMP_FPR.SReg);
		}
		rev(TEMP_GPR2.WReg, TEMP_GPR2.WReg);
		str(TEMP_GPR2.WReg, AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW));
	}
	else if (mode == PPCREC_FPR_ST_MODE_DOUBLE)
	{
		fmov(TEMP_GPR2.XReg, dataDReg);
		rev(TEMP_GPR2.XReg, TEMP_GPR2.XReg);
		str(TEMP_GPR2.XReg, AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW));
	}
	else if (mode == PPCREC_FPR_ST_MODE_UI32_FROM_PS0)
	{
		fmov(TEMP_GPR2.WReg, dataSReg);
		rev(TEMP_GPR2.WReg, TEMP_GPR2.WReg);
		str(TEMP_GPR2.WReg, AdrExt(MEM_BASE_REG, adrIdx, ExtMod::UXTW));
	}
	else
	{
		cemu_assert_suspicious();
		cemuLog_log(LogType::Recompiler, "PPCRecompilerAArch64Gen_imlInstruction_fpr_store(): Unsupported mode %d\n", mode);
		return false;
	}
	return true;
}

bool AArch64GenContext_t::tryFuseFprLoadPair(IMLInstruction* a, IMLInstruction* b)
{
	// Two adjacent non-indexed FPR loads, same base, +4 stride, SINGLE mode,
	// matching swapEndian and notExpanded flags, distinct destination FPRs.
	if (a->type != PPCREC_IML_TYPE_FPR_LOAD || b->type != PPCREC_IML_TYPE_FPR_LOAD)
		return false;
	if (a->op_storeLoad.mode != PPCREC_FPR_LD_MODE_SINGLE ||
		b->op_storeLoad.mode != PPCREC_FPR_LD_MODE_SINGLE)
		return false;
	// Endian-swap mismatch is rare but possible (raw byte read paths); only fuse
	// when both lanes need the same treatment so a single rev32 covers them.
	if (a->op_storeLoad.flags2.swapEndian != b->op_storeLoad.flags2.swapEndian)
		return false;
	if (a->op_storeLoad.flags2.notExpanded != b->op_storeLoad.flags2.notExpanded)
		return false;
	if (a->op_storeLoad.registerMem.GetRegID() != b->op_storeLoad.registerMem.GetRegID())
		return false;
	sint32 offA = a->op_storeLoad.immS32;
	sint32 offB = b->op_storeLoad.immS32;
	if (offB != offA + 4)
		return false;
	// We use ldur D for the 8-byte combined load (unscaled imm9, any alignment,
	// range [-256, +255]). PPC lfs offsets are 4-aligned, which trivially fits.
	if (offA < -256 || offA > 255)
		return false;
	IMLReg dataA = a->op_storeLoad.registerData;
	IMLReg dataB = b->op_storeLoad.registerData;
	if (dataA.GetRegID() == dataB.GetRegID())
		return false;

	WReg wBase = gpReg<WReg>(a->op_storeLoad.registerMem);
	size_t idxA = fpReg<SReg>(dataA).getIdx();
	size_t idxB = fpReg<SReg>(dataB).getIdx();
	// The destinations must not be TEMP_FPR (we use it as the load buffer).
	if (idxA == TEMP_FPR_ID || idxB == TEMP_FPR_ID)
		return false;

	add(TEMP_GPR1.XReg, MEM_BASE_REG, wBase, ExtMod::UXTW);

	if (a->op_storeLoad.flags2.swapEndian)
	{
		// ldur D loads both 32-bit BE singles into the low 64 bits of TEMP_FPR.
		// rev32 on the .8b view byte-swaps each 32-bit lane in place; upper 64
		// bits of TEMP_FPR are zeroed by the load and irrelevant after.
		ldur(TEMP_FPR.DReg, AdrImm(TEMP_GPR1.XReg, offA));
		rev32(TEMP_FPR.VReg.b8, TEMP_FPR.VReg.b8);
	}
	else
	{
		ldur(TEMP_FPR.DReg, AdrImm(TEMP_GPR1.XReg, offA));
	}

	::VReg vA(idxA), vB(idxB);
	if (a->op_storeLoad.flags2.notExpanded)
	{
		// Keep as single. `mov S, V.s[i]` is the DUP-scalar alias and writes
		// only the low 32 bits of the destination V, zeroing the rest — matches
		// the semantics of the per-instruction fmov(SReg, WReg) path.
		mov(SReg(idxA), TEMP_FPR.VReg.s2[0]);
		mov(SReg(idxB), TEMP_FPR.VReg.s2[1]);
	}
	else
	{
		// fcvtl V.2d, V.2s widens the lower two single-precision lanes of
		// TEMP_FPR to two double-precision lanes (the high half of the V is
		// written too). Then DUP-scalar each .d[i] into the destination D regs,
		// which zero-extends the upper bits of each destination V — same as
		// the per-instruction fcvt(DReg, SReg) path.
		fcvtl(TEMP_FPR.VReg.d2, TEMP_FPR.VReg.s2);
		mov(DReg(idxA), TEMP_FPR.VReg.d2[0]);
		mov(DReg(idxB), TEMP_FPR.VReg.d2[1]);
	}
	return true;
}

bool AArch64GenContext_t::tryFuseFprStorePair(IMLInstruction* a, IMLInstruction* b)
{
	// Mirror of tryFuseFprLoadPair. We pack both source FPRs (after the usual
	// double→single down-convert when needed) into a NEON D, byte-swap each
	// lane, and stur D — replacing two (fcvt + fmov-to-GPR + rev + str) chains
	// with a single shuffle and store.
	if (a->type != PPCREC_IML_TYPE_FPR_STORE || b->type != PPCREC_IML_TYPE_FPR_STORE)
		return false;
	if (a->op_storeLoad.mode != PPCREC_FPR_ST_MODE_SINGLE ||
		b->op_storeLoad.mode != PPCREC_FPR_ST_MODE_SINGLE)
		return false;
	if (a->op_storeLoad.flags2.swapEndian != b->op_storeLoad.flags2.swapEndian)
		return false;
	if (a->op_storeLoad.flags2.notExpanded != b->op_storeLoad.flags2.notExpanded)
		return false;
	if (a->op_storeLoad.registerMem.GetRegID() != b->op_storeLoad.registerMem.GetRegID())
		return false;
	sint32 offA = a->op_storeLoad.immS32;
	sint32 offB = b->op_storeLoad.immS32;
	if (offB != offA + 4)
		return false;
	// stur D — unscaled imm9, [-256, +255], any alignment. PPC stfs is 4-aligned.
	if (offA < -256 || offA > 255)
		return false;
	IMLReg dataA = a->op_storeLoad.registerData;
	IMLReg dataB = b->op_storeLoad.registerData;
	// Distinct source FPRs simplify the packing (we read each independently).
	// Same-reg stores happen but are rare and would need an extra mov.
	if (dataA.GetRegID() == dataB.GetRegID())
		return false;

	WReg wBase = gpReg<WReg>(a->op_storeLoad.registerMem);
	size_t idxA = fpReg<DReg>(dataA).getIdx();
	size_t idxB = fpReg<DReg>(dataB).getIdx();
	if (idxA == TEMP_FPR_ID || idxB == TEMP_FPR_ID)
		return false;

	if (a->op_storeLoad.flags2.notExpanded)
	{
		// Sources already in single format in the low 32 bits of each FPR.
		// Pack them into lanes 0/1 of TEMP_FPR.s2 via element-to-element ins.
		// `mov(VRegSElem, VRegSElem)` is the INS alias and preserves the
		// untouched lanes; that's fine here because we overwrite both.
		mov(TEMP_FPR.VReg.s2[0], ::VReg(idxA).s4[0]);
		mov(TEMP_FPR.VReg.s2[1], ::VReg(idxB).s4[0]);
	}
	else
	{
		// Down-convert both doubles to singles via a vector fcvtn. Pack the
		// two source doubles into TEMP_FPR.d2 first, then fcvtn writes the two
		// singles to TEMP_FPR.s2 (lower 64 bits) and zeroes the upper half.
		mov(TEMP_FPR.VReg.d2[0], ::VReg(idxA).d2[0]);
		mov(TEMP_FPR.VReg.d2[1], ::VReg(idxB).d2[0]);
		fcvtn(TEMP_FPR.VReg.s2, TEMP_FPR.VReg.d2);
	}
	if (a->op_storeLoad.flags2.swapEndian)
		rev32(TEMP_FPR.VReg.b8, TEMP_FPR.VReg.b8);
	add(TEMP_GPR1.XReg, MEM_BASE_REG, wBase, ExtMod::UXTW);
	stur(TEMP_FPR.DReg, AdrImm(TEMP_GPR1.XReg, offA));
	return true;
}

// FPR op FPR
void AArch64GenContext_t::fpr_r_r(IMLInstruction* imlInstruction)
{
	auto imlRegR = imlInstruction->op_fpr_r_r.regR;
	auto imlRegA = imlInstruction->op_fpr_r_r.regA;

	if (imlInstruction->operation == PPCREC_IML_OP_FPR_FLOAT_TO_INT)
	{
		fcvtzs(gpReg<WReg>(imlRegR), fpReg<DReg>(imlRegA));
		return;
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_INT_TO_FLOAT)
	{
		scvtf(fpReg<DReg>(imlRegR), gpReg<WReg>(imlRegA));
		return;
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_BITCAST_INT_TO_FLOAT)
	{
		cemu_assert_debug(imlRegR.GetRegFormat() == IMLRegFormat::F64); // assuming target is always F64 for now
		// exact operation depends on size of types. Floats are automatically promoted to double if the target is F64
		DReg regFprDReg = fpReg<DReg>(imlRegR);
		SReg regFprSReg = fpReg<SReg>(imlRegR);
		if (imlRegA.GetRegFormat() == IMLRegFormat::I32)
		{
			fmov(regFprSReg, gpReg<WReg>(imlRegA));
			// float to double
			fcvt(regFprDReg, regFprSReg);
		}
		else if (imlRegA.GetRegFormat() == IMLRegFormat::I64)
		{
			fmov(regFprDReg, gpReg<XReg>(imlRegA));
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return;
	}

	DReg regR = fpReg<DReg>(imlRegR);
	DReg regA = fpReg<DReg>(imlRegA);

	if (imlInstruction->operation == PPCREC_IML_OP_FPR_ASSIGN)
	{
		fmov(regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_MULTIPLY)
	{
		fmul(regR, regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_DIVIDE)
	{
		fdiv(regR, regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_ADD)
	{
		fadd(regR, regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_SUB)
	{
		fsub(regR, regR, regA);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_FCTIWZ)
	{
		fcvtzs(regR, regA);
	}
	else
	{
		cemu_assert_suspicious();
	}
}

void AArch64GenContext_t::fpr_r_r_r(IMLInstruction* imlInstruction)
{
	DReg regR = fpReg<DReg>(imlInstruction->op_fpr_r_r_r.regR);
	DReg regA = fpReg<DReg>(imlInstruction->op_fpr_r_r_r.regA);
	DReg regB = fpReg<DReg>(imlInstruction->op_fpr_r_r_r.regB);

	if (imlInstruction->operation == PPCREC_IML_OP_FPR_MULTIPLY)
	{
		fmul(regR, regA, regB);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_ADD)
	{
		fadd(regR, regA, regB);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_SUB)
	{
		fsub(regR, regA, regB);
	}
	else
	{
		cemu_assert_suspicious();
	}
}

/*
 * FPR = op (fprA, fprB, fprC)
 */
void AArch64GenContext_t::fpr_r_r_r_r(IMLInstruction* imlInstruction)
{
	DReg regR = fpReg<DReg>(imlInstruction->op_fpr_r_r_r_r.regR);
	DReg regA = fpReg<DReg>(imlInstruction->op_fpr_r_r_r_r.regA);
	DReg regB = fpReg<DReg>(imlInstruction->op_fpr_r_r_r_r.regB);
	DReg regC = fpReg<DReg>(imlInstruction->op_fpr_r_r_r_r.regC);

	if (imlInstruction->operation == PPCREC_IML_OP_FPR_SELECT)
	{
		fcmp(regA, 0.0);
		fcsel(regR, regC, regB, Cond::GE);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_MULTIPLY_ADD)
	{
		// Vd = Va*Vb + Vc -- the AArch64 fmadd Da, Db, Dc form. The host
		// instruction reads all three operands before writing Vd, so any
		// regR/source overlap (common in PPC code) is safe.
		fmadd(regR, regA, regB, regC);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_MULTIPLY_SUB)
	{
		// PPC fmsub semantics: Va*Vb - Vc. AArch64 maps this to fnmsub
		// (despite the name) -- ARM ARM defines fnmsub Dd = Da*Db - Dc.
		fnmsub(regR, regA, regB, regC);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_NEG_MULTIPLY_ADD)
	{
		// -(Va*Vb + Vc). AArch64 fnmadd Dd = -Va*Vb - Vc.
		fnmadd(regR, regA, regB, regC);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_NEG_MULTIPLY_SUB)
	{
		// Vc - Va*Vb (= -(Va*Vb - Vc), PPC fnmsub). AArch64 fmsub Dd = -Va*Vb + Vc.
		fmsub(regR, regA, regB, regC);
	}
	else
	{
		cemu_assert_suspicious();
	}
}

void AArch64GenContext_t::fpr_r(IMLInstruction* imlInstruction)
{
	DReg regRDReg = fpReg<DReg>(imlInstruction->op_fpr_r.regR);
	SReg regRSReg = fpReg<SReg>(imlInstruction->op_fpr_r.regR);

	if (imlInstruction->operation == PPCREC_IML_OP_FPR_NEGATE)
	{
		fneg(regRDReg, regRDReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_LOAD_ONE)
	{
		fmov(regRDReg, 1.0);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_ABS)
	{
		fabs(regRDReg, regRDReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_NEGATIVE_ABS)
	{
		fabs(regRDReg, regRDReg);
		fneg(regRDReg, regRDReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_ROUND_TO_SINGLE_PRECISION_BOTTOM)
	{
		// convert to 32bit single
		fcvt(regRSReg, regRDReg);
		// convert back to 64bit double
		fcvt(regRDReg, regRSReg);
	}
	else if (imlInstruction->operation == PPCREC_IML_OP_FPR_EXPAND_F32_TO_F64)
	{
		// convert bottom to 64bit double
		fcvt(regRDReg, regRSReg);
	}
	else
	{
		cemu_assert_unimplemented();
	}
}

Cond ImlFPCondToArm64Cond(IMLCondition cond)
{
	switch (cond)
	{
	case IMLCondition::UNORDERED_GT:
		return Cond::GT;
	case IMLCondition::UNORDERED_LT:
		return Cond::MI;
	case IMLCondition::UNORDERED_EQ:
		return Cond::EQ;
	case IMLCondition::UNORDERED_U:
		return Cond::VS;
	default:
	{
		cemu_assert_suspicious();
		return Cond::EQ;
	}
	}
}

void AArch64GenContext_t::fpr_compare(IMLInstruction* imlInstruction)
{
	DReg regA = fpReg<DReg>(imlInstruction->op_fpr_compare.regA);
	DReg regB = fpReg<DReg>(imlInstruction->op_fpr_compare.regB);
	fcmp(regA, regB);
	// Fused form (ARM64_FCMP): emit only the fcmp. The following ARM64_NZCV_JCC
	// reads the live NZCV flags. No cset / no GPR write.
	if (imlInstruction->operation == PPCREC_IML_OP_ARM64_FCMP)
		return;
	WReg regR = gpReg<WReg>(imlInstruction->op_fpr_compare.regR);
	auto cond = ImlFPCondToArm64Cond(imlInstruction->op_fpr_compare.cond);
	cset(regR, cond);
}

void AArch64GenContext_t::call_imm(IMLInstruction* imlInstruction)
{
	str(x30, AdrPreImm(sp, -16));
	mov(TEMP_GPR1.XReg, imlInstruction->op_call_imm.callAddress);
	blr(TEMP_GPR1.XReg);
	ldr(x30, AdrPostImm(sp, 16));
}

bool PPCRecompiler_generateAArch64Code(struct PPCRecFunction_t* PPCRecFunction, struct ppcImlGenContext_t* ppcImlGenContext)
{
	AArch64Allocator allocator;
	AArch64GenContext_t aarch64GenContext{&allocator};

	// generate iml instruction code
	bool codeGenerationFailed = false;
	for (IMLSegment* segIt : ppcImlGenContext->segmentList2)
	{
		if (codeGenerationFailed)
			break;
		segIt->x64Offset = aarch64GenContext.getSize();

		aarch64GenContext.storeSegmentStart(segIt);

		for (size_t i = 0; i < segIt->imlList.size(); i++)
		{
			IMLInstruction* imlInstruction = segIt->imlList.data() + i;
			if (imlInstruction->type == PPCREC_IML_TYPE_R_NAME)
			{
				aarch64GenContext.r_name(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_NAME_R)
			{
				aarch64GenContext.name_r(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_R_R)
			{
				if (!aarch64GenContext.r_r(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_R_S32)
			{
				if (!aarch64GenContext.r_s32(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_R_R_S32)
			{
				if (!aarch64GenContext.r_r_s32(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_R_R_S32_CARRY)
			{
				if (!aarch64GenContext.r_r_s32_carry(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_R_R_R)
			{
				if (!aarch64GenContext.r_r_r(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_R_R_R_CARRY)
			{
				if (!aarch64GenContext.r_r_r_carry(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_COMPARE)
			{
				aarch64GenContext.compare(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_COMPARE_S32)
			{
				aarch64GenContext.compare_s32(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_CONDITIONAL_JUMP)
			{
				aarch64GenContext.cjump(imlInstruction, segIt);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_ARM64_NZCV_JCC)
			{
				aarch64GenContext.cjump_nzcv(imlInstruction, segIt);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_ARM64_TBZ)
			{
				aarch64GenContext.cjump_tbz(imlInstruction, segIt);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_JUMP)
			{
				aarch64GenContext.jump(segIt);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_CJUMP_CYCLE_CHECK)
			{
				aarch64GenContext.conditionalJumpCycleCheck(segIt);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_MACRO)
			{
				if (!aarch64GenContext.macro(imlInstruction))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_LOAD)
			{
				// Try fusing with the next instruction into a single LDP. This
				// collapses the IML expansion of `lmw` (one IML LOAD per saved
				// register) as well as any hand-written multi-lwz burst the
				// register allocator happened to lay out into consecutive host
				// regs.
				bool fused = false;
				if (i + 1 < segIt->imlList.size())
				{
					IMLInstruction* next = segIt->imlList.data() + (i + 1);
					if (aarch64GenContext.tryFuseLoadPair(imlInstruction, next))
					{
						i++; // skip the partner LOAD we just emitted
						fused = true;
					}
				}
				if (!fused && !aarch64GenContext.load(imlInstruction, false))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_LOAD_INDEXED)
			{
				if (!aarch64GenContext.load(imlInstruction, true))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_STORE)
			{
				// Try fusing with the next STORE into a single STP (or, for
				// the byte-swapped Wii U BE case, a NEON-shuffled str D).
				// Mirrors the LDP fusion above; together they collapse the
				// IML expansion of lmw/stmw into roughly half the AArch64
				// instructions.
				bool fused = false;
				if (i + 1 < segIt->imlList.size())
				{
					IMLInstruction* next = segIt->imlList.data() + (i + 1);
					if (aarch64GenContext.tryFuseStorePair(imlInstruction, next))
					{
						i++;
						fused = true;
					}
				}
				if (!fused && !aarch64GenContext.store(imlInstruction, false))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_STORE_INDEXED)
			{
				if (!aarch64GenContext.store(imlInstruction, true))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_ATOMIC_CMP_STORE)
			{
				aarch64GenContext.atomic_cmp_store(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_CALL_IMM)
			{
				aarch64GenContext.call_imm(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_NO_OP)
			{
				// no op
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_LOAD)
			{
				// lfs→lfs is the single biggest adjacent-pair pattern in the
				// game binary by static count; fuse the swap+widen into one
				// NEON shuffle. Falls through to the scalar path on any
				// constraint failure (mode mismatch, big offset, etc.).
				bool fused = false;
				if (i + 1 < segIt->imlList.size())
				{
					IMLInstruction* next = segIt->imlList.data() + (i + 1);
					if (aarch64GenContext.tryFuseFprLoadPair(imlInstruction, next))
					{
						i++;
						fused = true;
					}
				}
				if (!fused && !aarch64GenContext.fpr_load(imlInstruction, false))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_LOAD_INDEXED)
			{
				if (!aarch64GenContext.fpr_load(imlInstruction, true))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_STORE)
			{
				// stfs→stfs mirror of the load-pair fuse — packs both singles
				// (with optional double→single narrowing) into a NEON D,
				// byte-swaps both lanes with one rev32, and stores.
				bool fused = false;
				if (i + 1 < segIt->imlList.size())
				{
					IMLInstruction* next = segIt->imlList.data() + (i + 1);
					if (aarch64GenContext.tryFuseFprStorePair(imlInstruction, next))
					{
						i++;
						fused = true;
					}
				}
				if (!fused && !aarch64GenContext.fpr_store(imlInstruction, false))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_STORE_INDEXED)
			{
				if (!aarch64GenContext.fpr_store(imlInstruction, true))
					codeGenerationFailed = true;
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_R_R)
			{
				aarch64GenContext.fpr_r_r(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_R_R_R)
			{
				aarch64GenContext.fpr_r_r_r(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_R_R_R_R)
			{
				aarch64GenContext.fpr_r_r_r_r(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_R)
			{
				aarch64GenContext.fpr_r(imlInstruction);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_COMPARE)
			{
				aarch64GenContext.fpr_compare(imlInstruction);
			}
			else
			{
				codeGenerationFailed = true;
				cemu_assert_suspicious();
				cemuLog_log(LogType::Recompiler, "PPCRecompiler_generateAArch64Code(): Unsupported iml type {}", imlInstruction->type);
			}
		}
	}

	// handle failed code generation
	if (codeGenerationFailed)
	{
		return false;
	}

	if (!aarch64GenContext.processAllJumps())
	{
		cemuLog_log(LogType::Recompiler, "PPCRecompiler_generateAArch64Code(): some jumps exceeded the +/-128MB offset.");
		return false;
	}

	aarch64GenContext.readyRE();

	// set code
	PPCRecFunction->x86Code = aarch64GenContext.getCode<void*>();
	PPCRecFunction->x86Size = aarch64GenContext.getMaxSize();
	// set free disabled to skip freeing the code from the CodeGenerator destructor
	allocator.setFreeDisabled(true);
	return true;
}

void PPCRecompiler_cleanupAArch64Code(void* code, size_t size)
{
	AArch64Allocator allocator;
	if (allocator.useProtect())
		CodeArray::protect(code, size, CodeArray::PROTECT_RW);
	allocator.free(static_cast<uint32*>(code));
}

void AArch64GenContext_t::enterRecompilerCode()
{
	constexpr size_t STACK_SIZE = 160 /* x19 .. x30 + v8.d[0] .. v15.d[0] */;
	static_assert(STACK_SIZE % 16 == 0);
	sub(sp, sp, STACK_SIZE);
	mov(x9, sp);

	stp(x19, x20, AdrPostImm(x9, 16));
	stp(x21, x22, AdrPostImm(x9, 16));
	stp(x23, x24, AdrPostImm(x9, 16));
	stp(x25, x26, AdrPostImm(x9, 16));
	stp(x27, x28, AdrPostImm(x9, 16));
	stp(x29, x30, AdrPostImm(x9, 16));
	st4((v8.d - v11.d)[0], AdrPostImm(x9, 32));
	st4((v12.d - v15.d)[0], AdrPostImm(x9, 32));
	mov(HCPU_REG, x1); // call argument 2
	mov(PPC_REC_INSTANCE_REG, (uint64)ppcRecompilerInstanceData);
	mov(MEM_BASE_REG, (uint64)memory_base);
	// Cache the cycle counter in a callee-saved host register for the whole
	// JIT execution session. Decrements at every basic block boundary then
	// become a one-instruction sub_imm against this register instead of an
	// ldr/sub/str triple against PPCInterpreter_t. The matching writeback
	// happens in leaveRecompilerCode (the universal JIT->native exit stub)
	// and around the synchronous HLE call in MACRO_HLE.
	ldr(REMAINING_CYCLES_REG.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, remainingCycles)));

	// branch to recFunc
	blr(x0); // call argument 1

	mov(x9, sp);
	ldp(x19, x20, AdrPostImm(x9, 16));
	ldp(x21, x22, AdrPostImm(x9, 16));
	ldp(x23, x24, AdrPostImm(x9, 16));
	ldp(x25, x26, AdrPostImm(x9, 16));
	ldp(x27, x28, AdrPostImm(x9, 16));
	ldp(x29, x30, AdrPostImm(x9, 16));
	ld4((v8.d - v11.d)[0], AdrPostImm(x9, 32));
	ld4((v12.d - v15.d)[0], AdrPostImm(x9, 32));

	add(sp, sp, STACK_SIZE);

	ret();
}

void AArch64GenContext_t::leaveRecompilerCode()
{
	// Flush the in-register cycle counter back to PPCInterpreter_t. Every
	// MACRO_LEAVE and every cycle-exhausted exit path branches through this
	// stub, so doing the writeback once here covers all of them.
	str(REMAINING_CYCLES_REG.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, remainingCycles)));
	str(LR.WReg, AdrUimm(HCPU_REG, offsetof(PPCInterpreter_t, instructionPointer)));
	ret();
}

bool initializedInterfaceFunctions = false;
AArch64GenContext_t enterRecompilerCode_ctx{};

AArch64GenContext_t leaveRecompilerCode_unvisited_ctx{};
AArch64GenContext_t leaveRecompilerCode_visited_ctx{};
void PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions()
{
	if (initializedInterfaceFunctions)
		return;
	initializedInterfaceFunctions = true;

	enterRecompilerCode_ctx.enterRecompilerCode();
	enterRecompilerCode_ctx.readyRE();
	PPCRecompiler_enterRecompilerCode = enterRecompilerCode_ctx.getCode<decltype(PPCRecompiler_enterRecompilerCode)>();

	leaveRecompilerCode_unvisited_ctx.leaveRecompilerCode();
	leaveRecompilerCode_unvisited_ctx.readyRE();
	PPCRecompiler_leaveRecompilerCode_unvisited = leaveRecompilerCode_unvisited_ctx.getCode<decltype(PPCRecompiler_leaveRecompilerCode_unvisited)>();

	leaveRecompilerCode_visited_ctx.leaveRecompilerCode();
	leaveRecompilerCode_visited_ctx.readyRE();
	PPCRecompiler_leaveRecompilerCode_visited = leaveRecompilerCode_visited_ctx.getCode<decltype(PPCRecompiler_leaveRecompilerCode_visited)>();
}
