#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/IML/IML.h"
#include "Cafe/HW/Espresso/Recompiler/IML/IMLInstruction.h"

#include "../PPCRecompiler.h"
#include "../PPCRecompilerIml.h"
#include "../BackendX64/BackendX64.h"

#include "Common/FileStream.h"

#include <boost/container/static_vector.hpp>
#include <boost/container/small_vector.hpp>

IMLReg _FPRRegFromID(IMLRegID regId)
{
	return IMLReg(IMLRegFormat::F64, IMLRegFormat::F64, 0, regId);
}

void PPCRecompiler_optimizeDirectFloatCopiesScanForward(ppcImlGenContext_t* ppcImlGenContext, IMLSegment* imlSegment, sint32 imlIndexLoad, IMLReg fprReg)
{
	IMLRegID fprIndex = fprReg.GetRegID();

	IMLInstruction* imlInstructionLoad = imlSegment->imlList.data() + imlIndexLoad;
	if (imlInstructionLoad->op_storeLoad.flags2.notExpanded)
		return;
	boost::container::static_vector<sint32, 4> trackedMoves; // only track up to 4 copies
	IMLUsedRegisters registersUsed;
	sint32 scanRangeEnd = std::min<sint32>(imlIndexLoad + 25, imlSegment->imlList.size()); // don't scan too far (saves performance and also the chances we can merge the load+store become low at high distances)
	bool foundMatch = false;
	sint32 lastStore = -1;
	for (sint32 i = imlIndexLoad + 1; i < scanRangeEnd; i++)
	{
		IMLInstruction* imlInstruction = imlSegment->imlList.data() + i;
		if (imlInstruction->IsSuffixInstruction())
			break;
		// check if FPR is stored
		if ((imlInstruction->type == PPCREC_IML_TYPE_FPR_STORE && imlInstruction->op_storeLoad.mode == PPCREC_FPR_ST_MODE_SINGLE) ||
			(imlInstruction->type == PPCREC_IML_TYPE_FPR_STORE_INDEXED && imlInstruction->op_storeLoad.mode == PPCREC_FPR_ST_MODE_SINGLE))
		{
			if (imlInstruction->op_storeLoad.registerData.GetRegID() == fprIndex)
			{
				if (foundMatch == false)
				{
					// flag the load-single instruction as "don't expand" (leave single value as-is)
					imlInstructionLoad->op_storeLoad.flags2.notExpanded = true;
				}
				// also set the flag for the store instruction
				IMLInstruction* imlInstructionStore = imlInstruction;
				imlInstructionStore->op_storeLoad.flags2.notExpanded = true;

				foundMatch = true;
				lastStore = i + 1;

				continue;
			}
		}
		// if the FPR is copied then keep track of it. We can expand the copies instead of the original
		if (imlInstruction->type == PPCREC_IML_TYPE_FPR_R_R && imlInstruction->operation == PPCREC_IML_OP_FPR_ASSIGN && imlInstruction->op_fpr_r_r.regA.GetRegID() == fprIndex)
		{
			if (imlInstruction->op_fpr_r_r.regR.GetRegID() == fprIndex)
			{
				// unexpected no-op
				break;
			}
			if (trackedMoves.size() >= trackedMoves.capacity())
			{
				// we cant track any more moves, expand here
				lastStore = i;
				break;
			}
			trackedMoves.push_back(i);
			continue;
		}
		// check if FPR is overwritten
		imlInstruction->CheckRegisterUsage(&registersUsed);
		if (registersUsed.writtenGPR1.IsValidAndSameRegID(fprIndex) || registersUsed.writtenGPR2.IsValidAndSameRegID(fprIndex))
			break;
		if (registersUsed.readGPR1.IsValidAndSameRegID(fprIndex))
			break;
		if (registersUsed.readGPR2.IsValidAndSameRegID(fprIndex))
			break;
		if (registersUsed.readGPR3.IsValidAndSameRegID(fprIndex))
			break;
		if (registersUsed.readGPR4.IsValidAndSameRegID(fprIndex))
			break;
	}

	if (foundMatch)
	{
		// insert expand instructions for each target register of a move
		sint32 positionBias = 0;
		for (auto& trackedMove : trackedMoves)
		{
			sint32 realPosition = trackedMove + positionBias;
			IMLInstruction* imlMoveInstruction = imlSegment->imlList.data() + realPosition;
			if (realPosition >= lastStore)
				break; // expand is inserted before this move
			else
				lastStore++;

			cemu_assert_debug(imlMoveInstruction->type == PPCREC_IML_TYPE_FPR_R_R && imlMoveInstruction->op_fpr_r_r.regA.GetRegID() == fprIndex);
			cemu_assert_debug(imlMoveInstruction->op_fpr_r_r.regA.GetRegFormat() == IMLRegFormat::F64);
			auto dstReg = imlMoveInstruction->op_fpr_r_r.regR;
			IMLInstruction* newExpand = PPCRecompiler_insertInstruction(imlSegment, realPosition+1); // one after the move
			newExpand->make_fpr_r(PPCREC_IML_OP_FPR_EXPAND_F32_TO_F64, dstReg);
			positionBias++;
		}
		// insert expand instruction after store
		IMLInstruction* newExpand = PPCRecompiler_insertInstruction(imlSegment, lastStore);
		newExpand->make_fpr_r(PPCREC_IML_OP_FPR_EXPAND_F32_TO_F64, _FPRRegFromID(fprIndex));
	}
}

/*
* Scans for patterns:
* <Load sp float into register f>
* <Random unrelated instructions>
* <Store sp float from register f>
* For these patterns the store and load is modified to work with un-extended values (float remains as float, no double conversion)
* The float->double extension is then executed later
* Advantages:
* Keeps denormals and other special float values intact
* Slightly improves performance
*/
void IMLOptimizer_OptimizeDirectFloatCopies(ppcImlGenContext_t* ppcImlGenContext)
{
	for (IMLSegment* segIt : ppcImlGenContext->segmentList2)
	{
		for (sint32 i = 0; i < segIt->imlList.size(); i++)
		{
			IMLInstruction* imlInstruction = segIt->imlList.data() + i;
			if (imlInstruction->type == PPCREC_IML_TYPE_FPR_LOAD && imlInstruction->op_storeLoad.mode == PPCREC_FPR_LD_MODE_SINGLE)
			{
				PPCRecompiler_optimizeDirectFloatCopiesScanForward(ppcImlGenContext, segIt, i, imlInstruction->op_storeLoad.registerData);
			}
			else if (imlInstruction->type == PPCREC_IML_TYPE_FPR_LOAD_INDEXED && imlInstruction->op_storeLoad.mode == PPCREC_FPR_LD_MODE_SINGLE)
			{
				PPCRecompiler_optimizeDirectFloatCopiesScanForward(ppcImlGenContext, segIt, i, imlInstruction->op_storeLoad.registerData);
			}
		}
	}
}

void PPCRecompiler_optimizeDirectIntegerCopiesScanForward(ppcImlGenContext_t* ppcImlGenContext, IMLSegment* imlSegment, sint32 imlIndexLoad, IMLReg gprReg)
{
	cemu_assert_debug(gprReg.GetBaseFormat() == IMLRegFormat::I64); // todo - proper handling required for non-standard sizes
	cemu_assert_debug(gprReg.GetRegFormat() == IMLRegFormat::I32);

	IMLRegID gprIndex = gprReg.GetRegID();
	IMLInstruction* imlInstructionLoad = imlSegment->imlList.data() + imlIndexLoad;
	if ( imlInstructionLoad->op_storeLoad.flags2.swapEndian == false )
		return;
	bool foundMatch = false;
	IMLUsedRegisters registersUsed;
	sint32 scanRangeEnd = std::min<sint32>(imlIndexLoad + 25, imlSegment->imlList.size()); // don't scan too far (saves performance and also the chances we can merge the load+store become low at high distances)
	sint32 i = imlIndexLoad + 1;
	for (; i < scanRangeEnd; i++)
	{
		IMLInstruction* imlInstruction = imlSegment->imlList.data() + i;
		if (imlInstruction->IsSuffixInstruction())
			break;
		// check if GPR is stored
		if ((imlInstruction->type == PPCREC_IML_TYPE_STORE && imlInstruction->op_storeLoad.copyWidth == 32 ) )
		{
			if (imlInstruction->op_storeLoad.registerMem.GetRegID() == gprIndex)
				break;
			if (imlInstruction->op_storeLoad.registerData.GetRegID() == gprIndex)
			{
				IMLInstruction* imlInstructionStore = imlInstruction;
				if (foundMatch == false)
				{
					// switch the endian swap flag for the load instruction
					imlInstructionLoad->op_storeLoad.flags2.swapEndian = !imlInstructionLoad->op_storeLoad.flags2.swapEndian;
					foundMatch = true;
				}
				// switch the endian swap flag for the store instruction
				imlInstructionStore->op_storeLoad.flags2.swapEndian = !imlInstructionStore->op_storeLoad.flags2.swapEndian;
				// keep scanning
				continue;
			}
		}
		// check if GPR is accessed
		imlInstruction->CheckRegisterUsage(&registersUsed);
		if (registersUsed.readGPR1.IsValidAndSameRegID(gprIndex) ||
			registersUsed.readGPR2.IsValidAndSameRegID(gprIndex) ||
			registersUsed.readGPR3.IsValidAndSameRegID(gprIndex))
		{
			break;
		}
		if (registersUsed.IsBaseGPRWritten(gprReg))
			return; // GPR overwritten, we don't need to byte swap anymore
	}
	if (foundMatch)
	{
		PPCRecompiler_insertInstruction(imlSegment, i)->make_r_r(PPCREC_IML_OP_ENDIAN_SWAP, gprReg, gprReg);
	}
}

/*
* Scans for patterns:
* <Load sp integer into register r>
* <Random unrelated instructions>
* <Store sp integer from register r>
* For these patterns the store and load is modified to work with non-swapped values
* The big_endian->little_endian conversion is then executed later
* Advantages:
* Slightly improves performance
*/
void IMLOptimizer_OptimizeDirectIntegerCopies(ppcImlGenContext_t* ppcImlGenContext)
{
	for (IMLSegment* segIt : ppcImlGenContext->segmentList2)
	{
		for (sint32 i = 0; i < segIt->imlList.size(); i++)
		{
			IMLInstruction* imlInstruction = segIt->imlList.data() + i;
			if (imlInstruction->type == PPCREC_IML_TYPE_LOAD && imlInstruction->op_storeLoad.copyWidth == 32 && imlInstruction->op_storeLoad.flags2.swapEndian )
			{
				PPCRecompiler_optimizeDirectIntegerCopiesScanForward(ppcImlGenContext, segIt, i, imlInstruction->op_storeLoad.registerData);
			}
		}
	}
}

IMLName PPCRecompilerImlGen_GetRegName(ppcImlGenContext_t* ppcImlGenContext, IMLReg reg);

sint32 _getGQRIndexFromRegister(ppcImlGenContext_t* ppcImlGenContext, IMLReg gqrReg)
{
	if (gqrReg.IsInvalid())
		return -1;
	sint32 namedReg = PPCRecompilerImlGen_GetRegName(ppcImlGenContext, gqrReg);
	if (namedReg >= (PPCREC_NAME_SPR0 + SPR_UGQR0) && namedReg <= (PPCREC_NAME_SPR0 + SPR_UGQR7))
	{
		return namedReg - (PPCREC_NAME_SPR0 + SPR_UGQR0);
	}
	else
	{
		cemu_assert_suspicious();
	}
	return -1;
}

bool PPCRecompiler_isUGQRValueKnown(ppcImlGenContext_t* ppcImlGenContext, sint32 gqrIndex, uint32& gqrValue)
{
	// the default configuration is:
	// UGQR0 = 0x00000000
	// UGQR2 = 0x00040004
	// UGQR3 = 0x00050005
	// UGQR4 = 0x00060006
	// UGQR5 = 0x00070007
	// but games are free to modify UGQR2 to UGQR7 it seems.
	// no game modifies UGQR0 so it's safe enough to optimize for the default value
	// Ideally we would do some kind of runtime tracking and second recompilation to create fast paths for PSQ_L/PSQ_ST but thats todo
	if (gqrIndex == 0)
		gqrValue = 0x00000000;
	else
		return false;
	return true;
}

// analyses register dependencies across the entire function
// per segment this will generate information about which registers need to be preserved and which ones don't (e.g. are overwritten)
class IMLOptimizerRegIOAnalysis
{
  public:
	// constructor with segment pointer list as span
	IMLOptimizerRegIOAnalysis(std::span<IMLSegment*> segmentList, uint32 maxRegId) : m_segmentList(segmentList), m_maxRegId(maxRegId)
	{
		m_segRegisterInOutList.resize(segmentList.size());
	}

	struct IMLSegmentRegisterInOut
	{
		// todo - since our register ID range is usually pretty small (<64) we could use integer bitmasks to accelerate this? There is a helper class used in RA code already
		std::unordered_set<IMLRegID> regWritten; // registers which are modified in this segment
		std::unordered_set<IMLRegID> regImported; // registers which are read in this segment before they are written (importing value from previous segments)
		std::unordered_set<IMLRegID> regForward; // registers which are not read or written in this segment, but are imported into a later segment (propagated info)
	};

	// calculate which registers are imported (read-before-written) and forwarded (read-before-written by a later segment) per segment
	// then in a second step propagate the dependencies across linked segments
	void ComputeDepedencies()
	{
		std::vector<IMLSegmentRegisterInOut>& segRegisterInOutList = m_segRegisterInOutList;
		IMLSegmentRegisterInOut* segIO = segRegisterInOutList.data();
		uint32 index = 0;
		for(auto& seg : m_segmentList)
		{
			seg->momentaryIndex = index;
			index++;
			for(auto& instr : seg->imlList)
			{
				IMLUsedRegisters registerUsage;
				instr.CheckRegisterUsage(&registerUsage);
				// registers are considered imported if they are read before being written in this seg
				registerUsage.ForEachReadGPR([&](IMLReg gprReg) {
					IMLRegID gprId = gprReg.GetRegID();
					if (!segIO->regWritten.contains(gprId))
					{
						segIO->regImported.insert(gprId);
					}
				});
				registerUsage.ForEachWrittenGPR([&](IMLReg gprReg) {
					IMLRegID gprId = gprReg.GetRegID();
					segIO->regWritten.insert(gprId);
				});
			}
			segIO++;
		}
		// for every exit segment, import all registers
		for(auto& seg : m_segmentList)
		{
			if (!seg->nextSegmentIsUncertain)
				continue;
			if(seg->deadCodeEliminationHintSeg)
				continue;
			IMLSegmentRegisterInOut& segIO = segRegisterInOutList[seg->momentaryIndex];
			for(uint32 i=0; i<=m_maxRegId; i++)
			{
				segIO.regImported.insert((IMLRegID)i);
			}
		}
		// broadcast dependencies across segment chains
		std::unordered_set<uint32> segIdsWhichNeedUpdate;
		for (uint32 i = 0; i < m_segmentList.size(); i++)
		{
			segIdsWhichNeedUpdate.insert(i);
		}
		while(!segIdsWhichNeedUpdate.empty())
		{
			auto firstIt = segIdsWhichNeedUpdate.begin();
			uint32 segId = *firstIt;
			segIdsWhichNeedUpdate.erase(firstIt);
			// forward regImported and regForward to earlier segments into their regForward, unless the register is written
			auto& curSeg = m_segmentList[segId];
			IMLSegmentRegisterInOut& curSegIO = segRegisterInOutList[segId];
			for(auto& prevSeg : curSeg->list_prevSegments)
			{
				IMLSegmentRegisterInOut& prevSegIO = segRegisterInOutList[prevSeg->momentaryIndex];
				bool prevSegChanged = false;
				for(auto& regId : curSegIO.regImported)
				{
					if (!prevSegIO.regWritten.contains(regId))
						prevSegChanged |= prevSegIO.regForward.insert(regId).second;
				}
				for(auto& regId : curSegIO.regForward)
				{
					if (!prevSegIO.regWritten.contains(regId))
						prevSegChanged |= prevSegIO.regForward.insert(regId).second;
				}
				if(prevSegChanged)
					segIdsWhichNeedUpdate.insert(prevSeg->momentaryIndex);
			}
			// same for hint links
			for(auto& prevSeg : curSeg->list_deadCodeHintBy)
			{
				IMLSegmentRegisterInOut& prevSegIO = segRegisterInOutList[prevSeg->momentaryIndex];
				bool prevSegChanged = false;
				for(auto& regId : curSegIO.regImported)
				{
					if (!prevSegIO.regWritten.contains(regId))
						prevSegChanged |= prevSegIO.regForward.insert(regId).second;
				}
				for(auto& regId : curSegIO.regForward)
				{
					if (!prevSegIO.regWritten.contains(regId))
						prevSegChanged |= prevSegIO.regForward.insert(regId).second;
				}
				if(prevSegChanged)
					segIdsWhichNeedUpdate.insert(prevSeg->momentaryIndex);
			}
		}
	}

	std::unordered_set<IMLRegID> GetRegistersNeededAtEndOfSegment(IMLSegment& seg)
	{
		std::unordered_set<IMLRegID> regsNeeded;
		if(seg.nextSegmentIsUncertain)
		{
			if(seg.deadCodeEliminationHintSeg)
			{
				auto& nextSegIO = m_segRegisterInOutList[seg.deadCodeEliminationHintSeg->momentaryIndex];
				regsNeeded.insert(nextSegIO.regImported.begin(), nextSegIO.regImported.end());
				regsNeeded.insert(nextSegIO.regForward.begin(), nextSegIO.regForward.end());
			}
			else
			{
				// add all regs
				for(uint32 i = 0; i <= m_maxRegId; i++)
					regsNeeded.insert(i);
			}
			return regsNeeded;
		}
		if(seg.nextSegmentBranchTaken)
		{
			auto& nextSegIO = m_segRegisterInOutList[seg.nextSegmentBranchTaken->momentaryIndex];
			regsNeeded.insert(nextSegIO.regImported.begin(), nextSegIO.regImported.end());
			regsNeeded.insert(nextSegIO.regForward.begin(), nextSegIO.regForward.end());
		}
		if(seg.nextSegmentBranchNotTaken)
		{
			auto& nextSegIO = m_segRegisterInOutList[seg.nextSegmentBranchNotTaken->momentaryIndex];
			regsNeeded.insert(nextSegIO.regImported.begin(), nextSegIO.regImported.end());
			regsNeeded.insert(nextSegIO.regForward.begin(), nextSegIO.regForward.end());
		}
		return regsNeeded;
	}

	bool IsRegisterNeededAtEndOfSegment(IMLSegment& seg, IMLRegID regId)
	{
		if(seg.nextSegmentIsUncertain)
		{
			if(!seg.deadCodeEliminationHintSeg)
				return true;
			auto& nextSegIO = m_segRegisterInOutList[seg.deadCodeEliminationHintSeg->momentaryIndex];
			if(nextSegIO.regImported.contains(regId))
				return true;
			if(nextSegIO.regForward.contains(regId))
				return true;
			return false;
		}
		if(seg.nextSegmentBranchTaken)
		{
			auto& nextSegIO = m_segRegisterInOutList[seg.nextSegmentBranchTaken->momentaryIndex];
			if(nextSegIO.regImported.contains(regId))
				return true;
			if(nextSegIO.regForward.contains(regId))
				return true;
		}
		if(seg.nextSegmentBranchNotTaken)
		{
			auto& nextSegIO = m_segRegisterInOutList[seg.nextSegmentBranchNotTaken->momentaryIndex];
			if(nextSegIO.regImported.contains(regId))
				return true;
			if(nextSegIO.regForward.contains(regId))
				return true;
		}
		return false;
	}

  private:
	std::span<IMLSegment*> m_segmentList;
	uint32 m_maxRegId;

	std::vector<IMLSegmentRegisterInOut> m_segRegisterInOutList;

};

// scan backwards starting from index and return the index of the first found instruction which writes to the given register (by id)
sint32 IMLUtil_FindInstructionWhichWritesRegister(IMLSegment& seg, sint32 startIndex, IMLReg reg, sint32 maxScanDistance = -1)
{
	sint32 endIndex = std::max<sint32>(startIndex - maxScanDistance, 0);
	for (sint32 i = startIndex; i >= endIndex; i--)
	{
		IMLInstruction& imlInstruction = seg.imlList[i];
		IMLUsedRegisters registersUsed;
		imlInstruction.CheckRegisterUsage(&registersUsed);
		if (registersUsed.IsBaseGPRWritten(reg))
			return i;
	}
	return -1;
}

// returns true if the instruction can safely be moved while keeping ordering constraints and data dependencies intact
// initialIndex is inclusive, targetIndex is exclusive
bool IMLUtil_CanMoveInstructionTo(IMLSegment& seg, sint32 initialIndex, sint32 targetIndex)
{
	boost::container::static_vector<IMLRegID, 8> regsWritten;
	boost::container::static_vector<IMLRegID, 8> regsRead;
	// get list of read and written registers
	IMLUsedRegisters registersUsed;
	seg.imlList[initialIndex].CheckRegisterUsage(&registersUsed);
	registersUsed.ForEachAccessedGPR([&](IMLReg reg, bool isWritten) {
		if (isWritten)
			regsWritten.push_back(reg.GetRegID());
		else
			regsRead.push_back(reg.GetRegID());
	});
	// check all the instructions inbetween
	if(initialIndex < targetIndex)
	{
		sint32 scanStartIndex = initialIndex+1; // +1 to skip the moving instruction itself
		sint32 scanEndIndex = targetIndex;
		for (sint32 i = scanStartIndex; i < scanEndIndex; i++)
		{
			IMLUsedRegisters registersUsed;
			seg.imlList[i].CheckRegisterUsage(&registersUsed);
			// in order to be able to move an instruction past another instruction, any of the read registers must not be modified (written)
			// and any of it's written registers must not be read
			bool canMove = true;
			registersUsed.ForEachAccessedGPR([&](IMLReg reg, bool isWritten) {
				IMLRegID regId = reg.GetRegID();
				if (!isWritten)
					canMove = canMove && std::find(regsWritten.begin(), regsWritten.end(), regId) == regsWritten.end();
				else
					canMove = canMove && std::find(regsRead.begin(), regsRead.end(), regId) == regsRead.end();
			});
			if(!canMove)
				return false;
		}
	}
	else
	{
		cemu_assert_unimplemented(); // backwards scan is todo
		return false;
	}
	return true;
}

sint32 IMLUtil_CountRegisterReadsInRange(IMLSegment& seg, sint32 scanStartIndex, sint32 scanEndIndex, IMLRegID regId)
{
	cemu_assert_debug(scanStartIndex <= scanEndIndex);
	cemu_assert_debug(scanEndIndex < seg.imlList.size());
	sint32 count = 0;
	for (sint32 i = scanStartIndex; i <= scanEndIndex; i++)
	{
		IMLUsedRegisters registersUsed;
		seg.imlList[i].CheckRegisterUsage(&registersUsed);
		registersUsed.ForEachReadGPR([&](IMLReg reg) {
			if (reg.GetRegID() == regId)
				count++;
		});
	}
	return count;
}

// move instruction from one index to another
// instruction will be inserted before the instruction at targetIndex
// returns the new instruction index of the moved instruction
sint32 IMLUtil_MoveInstructionTo(IMLSegment& seg, sint32 initialIndex, sint32 targetIndex)
{
	cemu_assert_debug(initialIndex != targetIndex);
	IMLInstruction temp = seg.imlList[initialIndex];
	if (initialIndex < targetIndex)
	{
		cemu_assert_debug(targetIndex > 0);
		targetIndex--;
		for(size_t i=initialIndex; i<targetIndex; i++)
			seg.imlList[i] = seg.imlList[i+1];
		seg.imlList[targetIndex] = temp;
		return targetIndex;
	}
	else
	{
		cemu_assert_unimplemented(); // testing needed
		std::copy(seg.imlList.begin() + targetIndex, seg.imlList.begin() + initialIndex, seg.imlList.begin() + targetIndex + 1);
		seg.imlList[targetIndex] = temp;
		return targetIndex;
	}
}

// x86 specific
bool IMLOptimizerX86_ModifiesEFlags(IMLInstruction& inst)
{
	// this is a very conservative implementation. There are more cases but this is good enough for now
	if(inst.type == PPCREC_IML_TYPE_NAME_R || inst.type == PPCREC_IML_TYPE_R_NAME)
		return false;
	if((inst.type == PPCREC_IML_TYPE_R_R || inst.type == PPCREC_IML_TYPE_R_S32) && inst.operation == PPCREC_IML_OP_ASSIGN)
		return false;
	return true; // if we dont know for sure, assume it does
}

// AArch64 specific. NZCV is only touched by a small whitelist of operations
// in the AArch64 backend (compare/compare_s32, atomic_cmp_store, the carry
// arithmetic, the shift-overflow tst). Loads, stores, plain moves, and the
// big arithmetic/logic family don't touch the flags. Be conservative: assume
// NZCV is preserved only across the patterns we've actually audited.
bool IMLOptimizerArm64_ModifiesNZCV(IMLInstruction& inst)
{
	if(inst.type == PPCREC_IML_TYPE_NO_OP)
		return false;
	if(inst.type == PPCREC_IML_TYPE_NAME_R || inst.type == PPCREC_IML_TYPE_R_NAME)
		return false;
	if(inst.type == PPCREC_IML_TYPE_LOAD || inst.type == PPCREC_IML_TYPE_LOAD_INDEXED)
		return false;
	if(inst.type == PPCREC_IML_TYPE_STORE || inst.type == PPCREC_IML_TYPE_STORE_INDEXED)
		return false;
	if((inst.type == PPCREC_IML_TYPE_R_R || inst.type == PPCREC_IML_TYPE_R_S32) && inst.operation == PPCREC_IML_OP_ASSIGN)
		return false;
	// bfi/bfxil/ubfx don't touch NZCV — bitfield encodings, no flag writeback.
	if(inst.type == PPCREC_IML_TYPE_R_R_S32 &&
	   (inst.operation == PPCREC_IML_OP_BFI || inst.operation == PPCREC_IML_OP_BFXIL ||
	    inst.operation == PPCREC_IML_OP_ARM64_UBFX))
		return false;
	return true; // if we don't know for sure, assume it does
}

void IMLOptimizer_DebugPrintSeg(ppcImlGenContext_t& ppcImlGenContext, IMLSegment& seg)
{
	printf("----------------\n");
	IMLDebug_DumpSegment(&ppcImlGenContext, &seg);
	fflush(stdout);
}

void IMLOptimizer_RemoveDeadCodeFromSegment(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	// algorithm works like this:
	// Calculate which registers need to be preserved at the end of each segment
	// Then for each segment:
	// - Iterate instructions backwards
	// - Maintain a list of registers which are read at a later point (initially this is the list from the first step)
	// - If an instruction only modifies registers which are not in the read list and has no side effects, then it is dead code and can be replaced with a no-op

	std::unordered_set<IMLRegID> regsNeeded = regIoAnalysis.GetRegistersNeededAtEndOfSegment(seg);

	// start with suffix instruction
	if(seg.HasSuffixInstruction())
	{
		IMLInstruction& imlInstruction = seg.imlList[seg.GetSuffixInstructionIndex()];
		IMLUsedRegisters registersUsed;
		imlInstruction.CheckRegisterUsage(&registersUsed);
		registersUsed.ForEachWrittenGPR([&](IMLReg reg) {
			regsNeeded.erase(reg.GetRegID());
		});
		registersUsed.ForEachReadGPR([&](IMLReg reg) {
			regsNeeded.insert(reg.GetRegID());
		});
	}
	// iterate instructions backwards
	for (sint32 i = seg.imlList.size() - (seg.HasSuffixInstruction() ? 2:1); i >= 0; i--)
	{
		IMLInstruction& imlInstruction = seg.imlList[i];
		IMLUsedRegisters registersUsed;
		imlInstruction.CheckRegisterUsage(&registersUsed);
		// register read -> remove from overwritten list
		// register written -> add to overwritten list

		// check if this instruction only writes registers which will never be read
		bool onlyWritesRedundantRegisters = true;
		registersUsed.ForEachWrittenGPR([&](IMLReg reg) {
			if (regsNeeded.contains(reg.GetRegID()))
				onlyWritesRedundantRegisters = false;
		});
		// check if any of the written registers are read after this point
		registersUsed.ForEachWrittenGPR([&](IMLReg reg) {
			regsNeeded.erase(reg.GetRegID());
		});
		registersUsed.ForEachReadGPR([&](IMLReg reg) {
			regsNeeded.insert(reg.GetRegID());
		});
		if(!imlInstruction.HasSideEffects() && onlyWritesRedundantRegisters)
		{
			imlInstruction.make_no_op();
		}
	}
}

void IMLOptimizerX86_SubstituteCJumpForEflagsJump(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	// convert and optimize bool condition jumps to eflags condition jumps
	// - Moves eflag setter (e.g. cmp) closer to eflags consumer (conditional jump) if necessary. If not possible but required then exit early
	// - Since we only rely on eflags, the boolean register can be optimized out if DCE considers it unused
	// - Further detect and optimize patterns like DEC + CMP + JCC into fused ops (todo)

	// check if this segment ends with a conditional jump
	if(!seg.HasSuffixInstruction())
		return;
	sint32 cjmpInstIndex = seg.GetSuffixInstructionIndex();
	if(cjmpInstIndex < 0)
		return;
	IMLInstruction& cjumpInstr = seg.imlList[cjmpInstIndex];
	if( cjumpInstr.type != PPCREC_IML_TYPE_CONDITIONAL_JUMP )
		return;
	IMLReg regCondBool = cjumpInstr.op_conditional_jump.registerBool;
	bool invertedCondition = !cjumpInstr.op_conditional_jump.mustBeTrue;
	// find the instruction which sets the bool
	sint32 cmpInstrIndex = IMLUtil_FindInstructionWhichWritesRegister(seg, cjmpInstIndex-1, regCondBool, 20);
	if(cmpInstrIndex < 0)
		return;
	// check if its an instruction combo which can be optimized (currently only cmp + cjump) and get the condition
	IMLInstruction& condSetterInstr = seg.imlList[cmpInstrIndex];
	IMLCondition cond;
	if(condSetterInstr.type == PPCREC_IML_TYPE_COMPARE)
		cond = condSetterInstr.op_compare.cond;
	else if(condSetterInstr.type == PPCREC_IML_TYPE_COMPARE_S32)
		cond = condSetterInstr.op_compare_s32.cond;
	else
		return;
	// check if instructions inbetween modify eflags
	sint32 indexEflagsSafeStart = -1; // index of the first instruction which does not modify eflags up to cjump
	for(sint32 i = cjmpInstIndex-1; i > cmpInstrIndex; i--)
	{
		if(IMLOptimizerX86_ModifiesEFlags(seg.imlList[i]))
		{
			indexEflagsSafeStart = i+1;
			break;
		}
	}
	if(indexEflagsSafeStart >= 0)
	{
		cemu_assert(indexEflagsSafeStart > 0);
		// there are eflags-modifying instructions inbetween the bool setter and cjump
		// try to move the eflags setter close enough to the cjump (to indexEflagsSafeStart)
		bool canMove = IMLUtil_CanMoveInstructionTo(seg, cmpInstrIndex, indexEflagsSafeStart);
		if(!canMove)
		{
			return;
		}
		else
		{
			cmpInstrIndex = IMLUtil_MoveInstructionTo(seg, cmpInstrIndex, indexEflagsSafeStart);
		}
	}
	// we can turn the jump into an eflags jump
	cjumpInstr.make_x86_eflags_jcc(cond, invertedCondition);

	// note: x86_eflags_jcc doesn't count towards cond reg reads, so we have to check > 0 here instead of > 1
	if (IMLUtil_CountRegisterReadsInRange(seg, cmpInstrIndex, cjmpInstIndex, regCondBool.GetRegID()) > 0 || regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, regCondBool.GetRegID()))
		return; // bool register is used beyond the CMP, we can't drop it

	auto& cmpInstr = seg.imlList[cmpInstrIndex];
	cemu_assert_debug(cmpInstr.type == PPCREC_IML_TYPE_COMPARE || cmpInstr.type == PPCREC_IML_TYPE_COMPARE_S32);
	if(cmpInstr.type == PPCREC_IML_TYPE_COMPARE)
	{
		IMLReg regA = cmpInstr.op_compare.regA;
		IMLReg regB = cmpInstr.op_compare.regB;
		seg.imlList[cmpInstrIndex].make_r_r(PPCREC_IML_OP_X86_CMP, regA, regB);
	}
	else
	{
		IMLReg regA = cmpInstr.op_compare_s32.regA;
		sint32 val = cmpInstr.op_compare_s32.immS32;
		seg.imlList[cmpInstrIndex].make_r_s32(PPCREC_IML_OP_X86_CMP, regA, val);
	}

}

void IMLOptimizerArm64_SubstituteCJumpForNZCVJump(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	// convert bool-condition jump (cmp -> cset bool -> cbz/cbnz bool) into an NZCV-fused branch (cmp -> b.cond).
	// - Moves the compare closer to the consumer if needed (NZCV-safe moves only).
	// - The bool register becomes dead and the materialization is dropped by DCE.

	if(!seg.HasSuffixInstruction())
		return;
	sint32 cjmpInstIndex = seg.GetSuffixInstructionIndex();
	if(cjmpInstIndex < 0)
		return;
	IMLInstruction& cjumpInstr = seg.imlList[cjmpInstIndex];
	if(cjumpInstr.type != PPCREC_IML_TYPE_CONDITIONAL_JUMP)
		return;
	IMLReg regCondBool = cjumpInstr.op_conditional_jump.registerBool;
	bool invertedCondition = !cjumpInstr.op_conditional_jump.mustBeTrue;
	// find the instruction which sets the bool
	sint32 cmpInstrIndex = IMLUtil_FindInstructionWhichWritesRegister(seg, cjmpInstIndex-1, regCondBool, 20);
	if(cmpInstrIndex < 0)
		return;
	// the setter must be a plain compare we can split into a cmp+b.cond pair
	IMLInstruction& condSetterInstr = seg.imlList[cmpInstrIndex];
	IMLCondition cond;
	if(condSetterInstr.type == PPCREC_IML_TYPE_COMPARE)
		cond = condSetterInstr.op_compare.cond;
	else if(condSetterInstr.type == PPCREC_IML_TYPE_COMPARE_S32)
		cond = condSetterInstr.op_compare_s32.cond;
	else if(condSetterInstr.type == PPCREC_IML_TYPE_FPR_COMPARE)
		cond = condSetterInstr.op_fpr_compare.cond;
	else
		return;
	// no NZCV-clobbering instructions may sit between the cmp and the cjump.
	// if some do, try to slide the cmp down past them (only if it's safe to move).
	sint32 indexNZCVSafeStart = -1;
	for(sint32 i = cjmpInstIndex-1; i > cmpInstrIndex; i--)
	{
		if(IMLOptimizerArm64_ModifiesNZCV(seg.imlList[i]))
		{
			indexNZCVSafeStart = i+1;
			break;
		}
	}
	if(indexNZCVSafeStart >= 0)
	{
		cemu_assert(indexNZCVSafeStart > 0);
		bool canMove = IMLUtil_CanMoveInstructionTo(seg, cmpInstrIndex, indexNZCVSafeStart);
		if(!canMove)
			return;
		cmpInstrIndex = IMLUtil_MoveInstructionTo(seg, cmpInstrIndex, indexNZCVSafeStart);
	}
	// turn the cjump into an NZCV branch
	cjumpInstr.make_arm64_nzcv_jcc(cond, invertedCondition);

	// note: arm64_nzcv_jcc doesn't count towards cond reg reads, so we have to check > 0 here instead of > 1
	if (IMLUtil_CountRegisterReadsInRange(seg, cmpInstrIndex, cjmpInstIndex, regCondBool.GetRegID()) > 0 || regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, regCondBool.GetRegID()))
		return; // bool register is used beyond the CMP, leave the setter as a compare so the bool is still materialized

	auto& cmpInstr = seg.imlList[cmpInstrIndex];
	cemu_assert_debug(cmpInstr.type == PPCREC_IML_TYPE_COMPARE || cmpInstr.type == PPCREC_IML_TYPE_COMPARE_S32 || cmpInstr.type == PPCREC_IML_TYPE_FPR_COMPARE);
	if(cmpInstr.type == PPCREC_IML_TYPE_COMPARE)
	{
		IMLReg regA = cmpInstr.op_compare.regA;
		IMLReg regB = cmpInstr.op_compare.regB;
		seg.imlList[cmpInstrIndex].make_r_r(PPCREC_IML_OP_ARM64_CMP, regA, regB);
	}
	else if(cmpInstr.type == PPCREC_IML_TYPE_COMPARE_S32)
	{
		IMLReg regA = cmpInstr.op_compare_s32.regA;
		sint32 val = cmpInstr.op_compare_s32.immS32;
		seg.imlList[cmpInstrIndex].make_r_s32(PPCREC_IML_OP_ARM64_CMP, regA, val);
	}
	else
	{
		// FPR_COMPARE: keep struct, switch operation. CheckRegisterUsage and
		// RewriteGPR both special-case operation==ARM64_FCMP to ignore regR.
		cmpInstr.operation = PPCREC_IML_OP_ARM64_FCMP;
	}
}

// Fold `lis Rd, Hi; addi/addis/ori/oris/xori/xoris Rd, Rd, Lo` into a single
// 32-bit immediate ASSIGN. PPC has no 32-bit immediate load so any constant
// gets built up as a high+low pair; the JIT lowers each half independently,
// which on AArch64 means a movz followed by an add/orr/eor of another 16-bit
// chunk. Folding gives the backend a single ASSIGN imm32 op which lowers to
// the optimal movz+movk pair and shows the regalloc only one definition.
//
// Pattern: adjacent IML ops where the second's regR/regA both equal the
// first's regR and nothing else uses the dest in between (adjacency is
// enough — if there's any instruction between them, no fold). Static
// analysis of WW HD's .text counts ~10 374 sites for the addi pair plus
// thousands more for the oris/xoris variants.
static void IMLOptimizer_FoldLisFollowedByImmediateOp(IMLSegment& seg)
{
	for (sint32 i = 0; i + 1 < (sint32)seg.imlList.size(); i++)
	{
		IMLInstruction& a = seg.imlList[i];
		IMLInstruction& b = seg.imlList[i + 1];
		// a must be `ASSIGN regR, imm`
		if (a.type != PPCREC_IML_TYPE_R_S32 || a.operation != PPCREC_IML_OP_ASSIGN)
			continue;
		// b must be `ADD/OR/XOR regR, regR, imm` (i.e. updates the same reg using itself + an immediate)
		if (b.type != PPCREC_IML_TYPE_R_R_S32)
			continue;
		if (b.operation != PPCREC_IML_OP_ADD &&
		    b.operation != PPCREC_IML_OP_OR &&
		    b.operation != PPCREC_IML_OP_XOR)
			continue;
		if (a.op_r_immS32.regR.GetRegID() != b.op_r_r_s32.regR.GetRegID())
			continue;
		if (b.op_r_r_s32.regR.GetRegID() != b.op_r_r_s32.regA.GetRegID())
			continue;
		// Compute the fused constant.
		sint32 fused = a.op_r_immS32.immS32;
		switch (b.operation)
		{
		case PPCREC_IML_OP_ADD: fused = (sint32)((uint32)fused + (uint32)b.op_r_r_s32.immS32); break;
		case PPCREC_IML_OP_OR:  fused = (sint32)((uint32)fused | (uint32)b.op_r_r_s32.immS32); break;
		case PPCREC_IML_OP_XOR: fused = (sint32)((uint32)fused ^ (uint32)b.op_r_r_s32.immS32); break;
		}
		// Rewrite: keep ASSIGN, drop the follow-up.
		a.op_r_immS32.immS32 = fused;
		b.make_no_op();
	}
}

// Detect a PPC carry chain (`addc + adde [+ adde ...]` and the symmetric
// subfc/subfe form, all of which lower to R_R_R_CARRY ADD / ADD_WITH_CARRY
// in IML) and rewrite the ops to the ARM64_CARRY_CHAIN_* family. The
// chained form lets NZCV.C flow directly between adjacent host adcs ops
// instead of being saved to a GPR and reloaded -- saves ~3 host insns per
// link in 64-bit add/sub chains, CRC routines, hash mixers, and the
// __lldiv inner loop.
//
// Chain breaks at any non-ADD_WITH_CARRY op or any consumer that reads the
// carry GPR before the next link does. The carry-out at the chain end is
// materialized via cset only if downstream consumers actually read it.
static void IMLOptimizerArm64_FuseCarryChain(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	sint32 segSize = (sint32)seg.imlList.size();
	for (sint32 i = 0; i < segSize; i++)
	{
		IMLInstruction& head = seg.imlList[i];
		if (head.type != PPCREC_IML_TYPE_R_R_R_CARRY || head.operation != PPCREC_IML_OP_ADD)
			continue;
		IMLReg carryReg = head.op_r_r_r_carry.regCarry;
		if (!carryReg.IsValid())
			continue;
		IMLRegID carryId = carryReg.GetRegID();
		// Walk forward collecting consecutive ADD_WITH_CARRY consumers that
		// chain through the same carry GPR. NO_OPs are skipped. Anything
		// else (including reads of carryId) breaks the chain.
		boost::container::small_vector<sint32, 4> chainLinkIdx;
		sint32 j = i + 1;
		while (j < segSize)
		{
			IMLInstruction& cand = seg.imlList[j];
			if (cand.type == PPCREC_IML_TYPE_NO_OP) { j++; continue; }
			if (cand.type != PPCREC_IML_TYPE_R_R_R_CARRY ||
			    cand.operation != PPCREC_IML_OP_ADD_WITH_CARRY)
				break;
			if (cand.op_r_r_r_carry.regCarry.GetRegID() != carryId)
				break;
			// regR of this link must not alias regA/regB of the head, or
			// rewriting head to drop its carry-out materialization is fine
			// but we still need to avoid clobbering inputs.
			chainLinkIdx.push_back(j);
			j++;
		}
		if (chainLinkIdx.empty())
			continue;
		// Decide whether the carry-out at the chain end needs to live in a
		// GPR (downstream non-chain consumer reads it).
		bool carryLiveOut = false;
		if (j <= segSize - 1 &&
		    IMLUtil_CountRegisterReadsInRange(seg, j, segSize - 1, carryId) > 0)
			carryLiveOut = true;
		if (regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, carryId))
			carryLiveOut = true;
		// Rewrite. The carry GPR is no longer touched by HEAD or by mid LINKs;
		// only TAIL_KEEP_CARRY writes it (when carryLiveOut). For ops that
		// don't touch the carry GPR we must clear the regCarry field too --
		// CheckRegisterUsage now reports no use, so the register allocator
		// drops the carry reg ID from its translation table, and RewriteGPR
		// would assert when it walked the stale reg field.
		head.operation = PPCREC_IML_OP_ARM64_CARRY_CHAIN_HEAD;
		head.op_r_r_r_carry.regCarry = IMLREG_INVALID;
		for (size_t k = 0; k < chainLinkIdx.size(); k++)
		{
			sint32 idx = chainLinkIdx[k];
			bool isLast = (k == chainLinkIdx.size() - 1);
			if (isLast)
			{
				if (carryLiveOut)
				{
					seg.imlList[idx].operation = PPCREC_IML_OP_ARM64_CARRY_CHAIN_TAIL_KEEP_CARRY;
					// regCarry stays valid -- this op writes it via cset.
				}
				else
				{
					seg.imlList[idx].operation = PPCREC_IML_OP_ARM64_CARRY_CHAIN_TAIL;
					seg.imlList[idx].op_r_r_r_carry.regCarry = IMLREG_INVALID;
				}
			}
			else
			{
				seg.imlList[idx].operation = PPCREC_IML_OP_ARM64_CARRY_CHAIN_LINK;
				seg.imlList[idx].op_r_r_r_carry.regCarry = IMLREG_INVALID;
			}
		}
		i = chainLinkIdx.back(); // skip past the chain
	}
}

// Fold a `LEFT_SHIFT rT, rN, K` (K=1..3) immediately followed by a
// LOAD_INDEXED / STORE_INDEXED that uses rT as the index and doesn't reread
// rT afterwards. The fused form stores K in op_storeLoad.mode and uses the
// unshifted source as the index reg; the AArch64 backend then folds the
// shift into the address computation as `add base, base, idx, lsl #K`.
// Saves one host insn per array index, vtable dispatch, etc.
//
// Only K in 1..3 (the AArch64 ldr/str shifted-index range). Larger shifts
// fall through to the original SLWI+ADD+LDR path.
static void IMLOptimizerArm64_FuseIndexedLoadShift(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	for (sint32 i = 0; i + 1 < (sint32)seg.imlList.size(); i++)
	{
		IMLInstruction& a = seg.imlList[i];
		if (a.type != PPCREC_IML_TYPE_R_R_S32 || a.operation != PPCREC_IML_OP_LEFT_SHIFT)
			continue;
		sint32 shiftAmt = a.op_r_r_s32.immS32 & 0x1f;
		if (shiftAmt < 1 || shiftAmt > 3)
			continue;
		IMLReg shiftDst = a.op_r_r_s32.regR;
		IMLReg shiftSrc = a.op_r_r_s32.regA;
		// Find the next non-noop instruction.
		sint32 j = i + 1;
		while (j < (sint32)seg.imlList.size() && seg.imlList[j].type == PPCREC_IML_TYPE_NO_OP)
			j++;
		if (j >= (sint32)seg.imlList.size())
			continue;
		IMLInstruction& b = seg.imlList[j];
		if (b.type != PPCREC_IML_TYPE_LOAD_INDEXED && b.type != PPCREC_IML_TYPE_STORE_INDEXED)
			continue;
		if (b.op_storeLoad.mode != 0) // already shifted (defensive)
			continue;
		// Index reg must be the shift's destination. Bail if shiftDst aliases
		// data or base — folding would change the load's input meaning.
		if (b.op_storeLoad.registerMem2.GetRegID() != shiftDst.GetRegID())
			continue;
		if (b.op_storeLoad.registerData.GetRegID() == shiftDst.GetRegID() ||
		    b.op_storeLoad.registerMem.GetRegID() == shiftDst.GetRegID())
			continue;
		// shiftDst must be dead after the load (only read = the load itself).
		sint32 segSize = (sint32)seg.imlList.size();
		if (j + 1 <= segSize - 1 &&
		    IMLUtil_CountRegisterReadsInRange(seg, j + 1, segSize - 1, shiftDst.GetRegID()) > 0)
			continue;
		if (regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, shiftDst.GetRegID()))
			continue;
		b.op_storeLoad.registerMem2 = shiftSrc;
		b.op_storeLoad.mode = (uint8)shiftAmt;
		a.make_no_op();
	}
}

// Demote ADDS-with-carry-out to plain ADD when the carry-out is never read
// before being overwritten (and not live across the segment boundary). Saves
// one cset per SUBFIC / ADDC / SUBFC site whose XER.CA result is dead.
//
// PPCRecompilerImlGen_SUBFIC emits NOT + R_R_S32_CARRY(ADD) and the backend
// lowers the latter as `adds_imm + cset CS` (BackendAArch64.cpp:885). The cset
// is wasted whenever XER.CA isn't consumed before the next addic/subfc/etc.
// rewrites it -- the common case.
static void IMLOptimizerArm64_DropDeadCarryWrite(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	for (sint32 i = 0; i < (sint32)seg.imlList.size(); i++)
	{
		IMLInstruction& inst = seg.imlList[i];
		IMLReg carryReg;
		if (inst.type == PPCREC_IML_TYPE_R_R_S32_CARRY && inst.operation == PPCREC_IML_OP_ADD)
			carryReg = inst.op_r_r_s32_carry.regCarry;
		else if (inst.type == PPCREC_IML_TYPE_R_R_R_CARRY && inst.operation == PPCREC_IML_OP_ADD)
			carryReg = inst.op_r_r_r_carry.regCarry;
		else
			continue;
		if (!carryReg.IsValid())
			continue;
		IMLRegID carryId = carryReg.GetRegID();
		bool deadInSegment = true;
		bool overwrittenInSegment = false;
		for (sint32 j = i + 1; j < (sint32)seg.imlList.size(); j++)
		{
			IMLUsedRegisters used;
			seg.imlList[j].CheckRegisterUsage(&used);
			bool reads = false;
			used.ForEachReadGPR([&](IMLReg r) { if (r.GetRegID() == carryId) reads = true; });
			if (reads) { deadInSegment = false; break; }
			if (used.IsWrittenByRegId(carryId)) { overwrittenInSegment = true; break; }
		}
		if (!deadInSegment)
			continue;
		if (!overwrittenInSegment && regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, carryId))
			continue;
		if (inst.type == PPCREC_IML_TYPE_R_R_S32_CARRY)
		{
			IMLReg regR = inst.op_r_r_s32_carry.regR;
			IMLReg regA = inst.op_r_r_s32_carry.regA;
			sint32 imm = inst.op_r_r_s32_carry.immS32;
			inst.make_r_r_s32(PPCREC_IML_OP_ADD, regR, regA, imm);
		}
		else
		{
			IMLReg regR = inst.op_r_r_r_carry.regR;
			IMLReg regA = inst.op_r_r_r_carry.regA;
			IMLReg regB = inst.op_r_r_r_carry.regB;
			inst.make_r_r_r(PPCREC_IML_OP_ADD, regR, regA, regB);
		}
	}
}

// Collapse the CodeWarrior `(x == 0) ? 1 : 0` boolean idiom:
//   cntlzw rT, rS  ;  rlwinm rT, rT, 27, 5, 31
// The rlwinm half is already lowered to ARM64_UBFX(lsb=5, width=27) by the
// imlgen path. Naive emit is `clz w, w ; ubfx w, w, #5, #27` (2 host insns,
// with clz's 3-4c latency on the critical path). Replace with a single
// `COMPARE_S32 rS, 0, rT, EQ` -- backend lowers to `cmp w, #0 ; cset w, eq`
// (also 2 insns, but no clz dep chain). When the boolean is consumed by a
// branch, the downstream NZCV-jump fuser collapses further to `cmp + b.cond`.
// Static count in WW HD: 250+ adjacent pairs.
static void IMLOptimizerArm64_FoldCntlzwIsZero(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	for (sint32 i = 0; i + 1 < (sint32)seg.imlList.size(); i++)
	{
		IMLInstruction& a = seg.imlList[i];
		if (a.type != PPCREC_IML_TYPE_R_R || a.operation != PPCREC_IML_OP_CNTLZW)
			continue;
		// Find the next non-noop instruction.
		sint32 j = i + 1;
		while (j < (sint32)seg.imlList.size() && seg.imlList[j].type == PPCREC_IML_TYPE_NO_OP)
			j++;
		if (j >= (sint32)seg.imlList.size())
			continue;
		IMLInstruction& b = seg.imlList[j];
		if (b.type != PPCREC_IML_TYPE_R_R_S32 || b.operation != PPCREC_IML_OP_ARM64_UBFX)
			continue;
		if (b.op_r_r_s32.regA.GetRegID() != a.op_r_r.regR.GetRegID())
			continue;
		sint32 imm = b.op_r_r_s32.immS32;
		sint32 lsb = imm & 0x1f;
		sint32 width = ((imm >> 5) & 0x1f) + 1;
		if (lsb != 5 || width != 27)
			continue;
		// If the cntlzw destination differs from the ubfx destination, the
		// intermediate count must be dead after the ubfx -- otherwise the count
		// is consumed elsewhere and we can't elide it.
		IMLReg cntDst = a.op_r_r.regR;
		IMLReg ubfxDst = b.op_r_r_s32.regR;
		if (cntDst.GetRegID() != ubfxDst.GetRegID())
		{
			if (j + 1 <= (sint32)seg.imlList.size() - 1 &&
			    IMLUtil_CountRegisterReadsInRange(seg, j + 1, (sint32)seg.imlList.size() - 1, cntDst.GetRegID()) > 0)
				continue;
			if (regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, cntDst.GetRegID()))
				continue;
		}
		IMLReg srcReg = a.op_r_r.regA;
		a.make_no_op();
		b.make_compare_s32(srcReg, 0, ubfxDst, IMLCondition::EQ);
	}
}

// Fuse `ADD/SUB r, ... ; (cmp r, #0)` into `ARM64_ADDS/SUBS r, ...` so the
// host adds/subs writes both regR and NZCV in one instruction. Saves the
// trailing cmp.
//
// Two consumer shapes:
//   Case A: ADD r ; ARM64_CMP r, #0 ; (consumer reads NZCV)
//           -> ADDS ; (NOOP) ; consumer
//           Common when the Rc=1 form's CR0 bit is consumed by a
//           non-EQ/NEQ branch (LT/GT/etc., where the CBZ fold doesn't
//           apply) or when CBZ folding bailed for some other reason.
//   Case B: ADD r ; COMPARE_S32 r, #0, crBit, cond
//           -> ADDS ; ARM64_CSET_FROM_NZCV crBit, cond
//           This is the typical Rc=1 ALU + materialize-CR-bit-to-GPR
//           shape (`add. r, a, b ; mfcr r4 ; rlwinm r4, r4, 1, 31, 31`,
//           or any other CR0-bit-to-bool extraction).
//
// Run AFTER NZCV-jump and CBZ folds: when CBZ already fired, the ARM64_CMP
// is gone and our case-A pattern can't match; case B still applies.
static void IMLOptimizerArm64_FuseAluCmpForFlags(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	sint32 segSize = (sint32)seg.imlList.size();
	for (sint32 i = 0; i + 1 < segSize; i++)
	{
		IMLInstruction& addInst = seg.imlList[i];
		if (addInst.type != PPCREC_IML_TYPE_R_R_R)
			continue;
		if (addInst.operation != PPCREC_IML_OP_ADD &&
		    addInst.operation != PPCREC_IML_OP_SUB)
			continue;
		IMLReg addResult = addInst.op_r_r_r.regR;
		IMLRegID addResultId = addResult.GetRegID();
		// Find the next non-noop instruction.
		sint32 j = i + 1;
		while (j < segSize && seg.imlList[j].type == PPCREC_IML_TYPE_NO_OP)
			j++;
		if (j >= segSize) continue;
		IMLInstruction& cand = seg.imlList[j];
		bool isCaseA = (cand.type == PPCREC_IML_TYPE_R_S32 &&
		                cand.operation == PPCREC_IML_OP_ARM64_CMP &&
		                cand.op_r_immS32.immS32 == 0 &&
		                cand.op_r_immS32.regR.GetRegID() == addResultId);
		bool isCaseB = (cand.type == PPCREC_IML_TYPE_COMPARE_S32 &&
		                cand.operation != PPCREC_IML_OP_ARM64_CSET_FROM_NZCV &&
		                cand.op_compare_s32.immS32 == 0 &&
		                cand.op_compare_s32.regA.GetRegID() == addResultId);
		if (!isCaseA && !isCaseB)
			continue;
		// addResult is unmodified between i and j (only NO_OPs sit there
		// per the j scan). After our rewrite, addResult is still written
		// by ARM64_ADDS/SUBS with the same value, so any downstream
		// readers stay correct.
		if (addInst.operation == PPCREC_IML_OP_ADD)
			addInst.operation = PPCREC_IML_OP_ARM64_ADDS;
		else
			addInst.operation = PPCREC_IML_OP_ARM64_SUBS;
		if (isCaseA)
			cand.make_no_op();
		else
			cand.operation = PPCREC_IML_OP_ARM64_CSET_FROM_NZCV;
	}
}

// Collapse a `AND regR, regSrc, #(1<<bit) ; ARM64_CMP regR, #0 ; ARM64_NZCV_JCC EQ/NEQ`
// chain into a single ARM64_TBZ/TBNZ on regSrc. AArch64's `tbz Rn, #bit, label`
// is a single instruction that branches on a single bit -- compared to the
// unfused `mov tmp, #imm; and regR, regSrc, tmp; cmp regR, #0; b.cond` we save
// ~3 host instructions per fire. Static analysis of WW HD .text counts
// 2 485 rlwinm.+branch sites with MB==ME (single-bit isolate) per FINDINGS
// item 2.
//
// Runs AFTER the cmp/branch fusion so the producer of NZCV is an ARM64_CMP.
// Skips the fold when the AND's result is still needed after the branch.
static void IMLOptimizerArm64_SubstituteSingleBitCmpForTBZ(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	if (!seg.HasSuffixInstruction())
		return;
	sint32 jccIdx = seg.GetSuffixInstructionIndex();
	if (jccIdx < 0)
		return;
	IMLInstruction& jcc = seg.imlList[jccIdx];
	if (jcc.type != PPCREC_IML_TYPE_ARM64_NZCV_JCC)
		return;
	if (jcc.op_arm64_nzcv_jcc.cond != IMLCondition::EQ && jcc.op_arm64_nzcv_jcc.cond != IMLCondition::NEQ)
		return;

	// Find the producing ARM64_CMP immediately before the jcc (skipping no-ops).
	sint32 cmpIdx = -1;
	for (sint32 i = jccIdx - 1; i >= 0; --i)
	{
		if (seg.imlList[i].type == PPCREC_IML_TYPE_NO_OP)
			continue;
		if (seg.imlList[i].type == PPCREC_IML_TYPE_R_S32 &&
		    seg.imlList[i].operation == PPCREC_IML_OP_ARM64_CMP &&
		    seg.imlList[i].op_r_immS32.immS32 == 0)
		{
			cmpIdx = i;
		}
		break;
	}
	if (cmpIdx < 0)
		return;
	IMLReg cmpReg = seg.imlList[cmpIdx].op_r_immS32.regR;

	// Find AND that writes cmpReg, immediately before cmpIdx (skipping no-ops).
	sint32 andIdx = -1;
	for (sint32 i = cmpIdx - 1; i >= 0; --i)
	{
		if (seg.imlList[i].type == PPCREC_IML_TYPE_NO_OP)
			continue;
		if (seg.imlList[i].type == PPCREC_IML_TYPE_R_R_S32 &&
		    seg.imlList[i].operation == PPCREC_IML_OP_AND &&
		    seg.imlList[i].op_r_r_s32.regR.GetRegID() == cmpReg.GetRegID())
		{
			uint32 imm = (uint32)seg.imlList[i].op_r_r_s32.immS32;
			if (imm != 0 && (imm & (imm - 1)) == 0) // power of two = single bit
				andIdx = i;
		}
		break;
	}
	if (andIdx < 0)
		return;

	// Bail if cmpReg is read more than once (cmp itself) before the jcc or
	// if it's needed after the branch -- we'd have to keep the AND otherwise.
	if (IMLUtil_CountRegisterReadsInRange(seg, andIdx + 1, jccIdx - 1, cmpReg.GetRegID()) > 1)
		return;
	if (regIoAnalysis.IsRegisterNeededAtEndOfSegment(seg, cmpReg.GetRegID()))
		return;

	uint32 imm = (uint32)seg.imlList[andIdx].op_r_r_s32.immS32;
	uint8 bitIndex = (uint8)std::countr_zero(imm);
	IMLReg regSrc = seg.imlList[andIdx].op_r_r_s32.regA;
	// tbz fires when the bit is zero. b.eq on a tst-style result (Z=1) fires
	// when the masked bit is zero. So for cond=EQ not-inverted: tbz.
	//   mustBeZero == (cond == EQ) XOR inverted == false
	bool jccEq = (jcc.op_arm64_nzcv_jcc.cond == IMLCondition::EQ);
	bool inverted = jcc.op_arm64_nzcv_jcc.invertedCondition;
	bool mustBeZero = (jccEq != inverted);

	jcc.make_arm64_tbz(regSrc, bitIndex, mustBeZero);
	seg.imlList[cmpIdx].make_no_op();
	seg.imlList[andIdx].make_no_op();
}

// Collapse `ARM64_CMP regA, #0; ARM64_NZCV_JCC EQ/NEQ` into a single `ARM64_CBZ regA`
// (or CBNZ). PPC's `cmpwi r, 0; beq/bne` is the most common branch shape; the
// NZCV fusion turns it into two host insns (cmp w, #0; b.eq) which cbz/cbnz can
// fold into one with the same +/-1MB reach.
//
// Runs after SubstituteSingleBitCmpForTBZ so the single-bit AND+CMP+JCC pattern
// gets the cheaper TBZ collapse first. If TBZ already fired, this finds nothing.
static void IMLOptimizerArm64_SubstituteCmpZeroForCBZ(IMLSegment& seg)
{
	if (!seg.HasSuffixInstruction())
		return;
	sint32 jccIdx = seg.GetSuffixInstructionIndex();
	if (jccIdx < 0)
		return;
	IMLInstruction& jcc = seg.imlList[jccIdx];
	if (jcc.type != PPCREC_IML_TYPE_ARM64_NZCV_JCC)
		return;
	if (jcc.op_arm64_nzcv_jcc.cond != IMLCondition::EQ && jcc.op_arm64_nzcv_jcc.cond != IMLCondition::NEQ)
		return;

	// Find the producing ARM64_CMP, #0 immediately before the jcc (skipping no-ops).
	sint32 cmpIdx = -1;
	for (sint32 i = jccIdx - 1; i >= 0; --i)
	{
		if (seg.imlList[i].type == PPCREC_IML_TYPE_NO_OP)
			continue;
		if (seg.imlList[i].type == PPCREC_IML_TYPE_R_S32 &&
		    seg.imlList[i].operation == PPCREC_IML_OP_ARM64_CMP &&
		    seg.imlList[i].op_r_immS32.immS32 == 0)
		{
			cmpIdx = i;
		}
		break;
	}
	if (cmpIdx < 0)
		return;
	IMLReg regSrc = seg.imlList[cmpIdx].op_r_immS32.regR;

	// cbz fires when reg is zero. b.eq after cmp #0 fires when reg is zero.
	// So for cond=EQ not-inverted: cbz.
	bool jccEq = (jcc.op_arm64_nzcv_jcc.cond == IMLCondition::EQ);
	bool inverted = jcc.op_arm64_nzcv_jcc.invertedCondition;
	bool mustBeZero = (jccEq != inverted);

	jcc.make_arm64_cbz(regSrc, mustBeZero);
	seg.imlList[cmpIdx].make_no_op();
}

void IMLOptimizer_StandardOptimizationPassForSegment(IMLOptimizerRegIOAnalysis& regIoAnalysis, IMLSegment& seg)
{
	// Fold lis+addi/ori before DCE so the dead intermediate ADD can be removed.
	IMLOptimizer_FoldLisFollowedByImmediateOp(seg);

#if defined(__aarch64__)
	// Fold cntlzw+ubfx is_zero idiom before DCE so the eliminated cntlzw can be removed.
	IMLOptimizerArm64_FoldCntlzwIsZero(regIoAnalysis, seg);
	// Drop dead carry-out writes (SUBFIC, ADDC, SUBFC, ... whose XER.CA is unread).
	IMLOptimizerArm64_DropDeadCarryWrite(regIoAnalysis, seg);
	// Fuse `addc + adde [+ adde ...]` carry chains so NZCV.C flows directly
	// between adjacent host adcs ops. Run AFTER DropDeadCarryWrite so any
	// chain whose carry-out becomes dead doesn't have to materialize it.
	IMLOptimizerArm64_FuseCarryChain(regIoAnalysis, seg);
	// Fold scaled-index left shifts into the following indexed load/store.
	IMLOptimizerArm64_FuseIndexedLoadShift(regIoAnalysis, seg);
#endif

	IMLOptimizer_RemoveDeadCodeFromSegment(regIoAnalysis, seg);

#ifdef ARCH_X86_64
	// x86 specific optimizations
	IMLOptimizerX86_SubstituteCJumpForEflagsJump(regIoAnalysis, seg); // this pass should be applied late since it creates invisible eflags dependencies (which would break further register dependency analysis)
#endif
#if defined(__aarch64__)
	// AArch64 specific optimizations
	IMLOptimizerArm64_SubstituteCJumpForNZCVJump(regIoAnalysis, seg); // late pass: creates invisible NZCV dependency between cmp and branch
	IMLOptimizerArm64_SubstituteSingleBitCmpForTBZ(regIoAnalysis, seg); // even later: collapse rlwinm.+branch into a single tbz
	IMLOptimizerArm64_SubstituteCmpZeroForCBZ(seg); // last: collapse cmpwi reg,0+beq/bne into a single cbz/cbnz
	IMLOptimizerArm64_FuseAluCmpForFlags(regIoAnalysis, seg); // very-last: fold ADD/SUB+cmp into ADDS/SUBS (skips cases CBZ/TBZ already won)
#endif
}

void IMLOptimizer_StandardOptimizationPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLOptimizerRegIOAnalysis regIoAnalysis(ppcImlGenContext.segmentList2, ppcImlGenContext.GetMaxRegId());
	regIoAnalysis.ComputeDepedencies();
	for (IMLSegment* segIt : ppcImlGenContext.segmentList2)
	{
		IMLOptimizer_StandardOptimizationPassForSegment(regIoAnalysis, *segIt);
	}
}
