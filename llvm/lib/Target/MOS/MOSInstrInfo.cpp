//===-- MOSInstrInfo.cpp - MOS Instruction Information --------------------===//
//
// Part of LLVM-MOS, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the MOS implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "MOSInstrInfo.h"

#include "MCTargetDesc/MOSMCTargetDesc.h"
#include "MOSRegisterInfo.h"

#include "MOSSubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "mos-instrinfo"

#define GET_INSTRINFO_CTOR_DTOR
#include "MOSGenInstrInfo.inc"

MOSInstrInfo::MOSInstrInfo()
    : MOSGenInstrInfo(/*CFSetupOpcode=*/MOS::ADJCALLSTACKDOWN,
                      /*CFDestroyOpcode=*/MOS::ADJCALLSTACKUP) {}

unsigned MOSInstrInfo::isLoadFromStackSlot(const MachineInstr &MI,
                                           int &FrameIndex) const {
  switch (MI.getOpcode()) {
  default:
    return 0;
  case MOS::LDAbsOffset:
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  case MOS::LDStk:
    FrameIndex = MI.getOperand(2).getIndex();
    return MI.getOperand(0).getReg();
  }
}

unsigned MOSInstrInfo::isStoreToStackSlot(const MachineInstr &MI,
                                          int &FrameIndex) const {
  switch (MI.getOpcode()) {
  default:
    return 0;
  case MOS::STAbsOffset:
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  case MOS::STStk:
    FrameIndex = MI.getOperand(2).getIndex();
    return MI.getOperand(1).getReg();
  }
}

// The main difficulty in commuting 6502 instructions is that their register
// classes aren't symmetric. This routine determines whether or not the operands
// of an instruction can be commuted anyway, potentially rewriting the register
// classes of virtual registers to do so.
MachineInstr *MOSInstrInfo::commuteInstructionImpl(MachineInstr &MI, bool NewMI,
                                                   unsigned Idx1,
                                                   unsigned Idx2) const {
  // NOTE: This doesn't seem to actually be used anywhere.
  if (NewMI)
    report_fatal_error("NewMI is not supported");

  MachineFunction &MF = *MI.getMF();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  LLVM_DEBUG(dbgs() << "Commute: " << MI);

  // Determines the register class for a given virtual register constrained by a
  // target register class and all uses outside this instruction. This
  // effectively removes the constraints due to just this instruction, then
  // tries to apply the constraint for the other operand.
  const auto NewRegClass =
      [&](Register Reg,
          const TargetRegisterClass *RC) -> const TargetRegisterClass * {
    for (MachineOperand &MO : MRI.reg_nodbg_operands(Reg)) {
      MachineInstr *UseMI = MO.getParent();
      if (UseMI == &MI)
        continue;
      unsigned OpNo = &MO - &UseMI->getOperand(0);
      RC = UseMI->getRegClassConstraintEffect(OpNo, RC, this, &TRI);
      if (!RC)
        return nullptr;
    }
    return RC;
  };

  const TargetRegisterClass *RegClass1 =
      getRegClass(MI.getDesc(), Idx1, &TRI, MF);
  const TargetRegisterClass *RegClass2 =
      getRegClass(MI.getDesc(), Idx2, &TRI, MF);
  Register Reg1 = MI.getOperand(Idx1).getReg();
  Register Reg2 = MI.getOperand(Idx2).getReg();

  // See if swapping the two operands are possible given their register classes.
  const TargetRegisterClass *Reg1Class = nullptr;
  const TargetRegisterClass *Reg2Class = nullptr;
  if (Reg1.isVirtual()) {
    Reg1Class = NewRegClass(Reg1, RegClass2);
    if (!Reg1Class)
      return nullptr;
  }
  if (Reg1.isPhysical() && !RegClass2->contains(Reg1))
    return nullptr;
  if (Reg2.isVirtual()) {
    Reg2Class = NewRegClass(Reg2, RegClass1);
    if (!Reg2Class)
      return nullptr;
  }
  if (Reg2.isPhysical() && !RegClass1->contains(Reg2))
    return nullptr;

  // If this fails, make sure to get it out of the way before rewriting reg
  // classes.
  MachineInstr *CommutedMI =
      TargetInstrInfo::commuteInstructionImpl(MI, NewMI, Idx1, Idx2);
  if (!CommutedMI)
    return nullptr;

  // Use the new register classes computed above, if any.
  if (Reg1Class)
    MRI.setRegClass(Reg1, Reg1Class);
  if (Reg2Class)
    MRI.setRegClass(Reg2, Reg2Class);
  return CommutedMI;
}

unsigned MOSInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  // Overestimate the size of each instruction to guarantee that any necessary
  // branches are relaxed.
  return 3;
}

// 6502 instructions aren't as regular as most commutable instructions, so this
// routine determines the commutable operands manually.
bool MOSInstrInfo::findCommutedOpIndices(const MachineInstr &MI,
                                         unsigned &SrcOpIdx1,
                                         unsigned &SrcOpIdx2) const {
  assert(!MI.isBundle() &&
         "MOSInstrInfo::findCommutedOpIndices() can't handle bundles");

  const MCInstrDesc &MCID = MI.getDesc();
  if (!MCID.isCommutable())
    return false;

  unsigned CommutableOpIdx1, CommutableOpIdx2;
  switch (MI.getOpcode()) {
  default:
    llvm_unreachable("Unexpected opcode; don't know how to commute.");
  case MOS::ADCImag8:
    CommutableOpIdx1 = 3;
    CommutableOpIdx2 = 4;
    break;
  case MOS::ANDImag8:
  case MOS::EORImag8:
  case MOS::ORAImag8:
    CommutableOpIdx1 = 1;
    CommutableOpIdx2 = 2;
    break;
  }

  if (!fixCommutedOpIndices(SrcOpIdx1, SrcOpIdx2, CommutableOpIdx1,
                            CommutableOpIdx2))
    return false;

  if (!MI.getOperand(SrcOpIdx1).isReg() || !MI.getOperand(SrcOpIdx2).isReg()) {
    // No idea.
    return false;
  }
  return true;
}

bool MOSInstrInfo::isBranchOffsetInRange(unsigned BranchOpc,
                                         int64_t BrOffset) const {
  switch (BranchOpc) {
  default:
    llvm_unreachable("Bad branch opcode");
  case MOS::BR:
  case MOS::BRA:
    // BR range is [-128,127] starting from the PC location after the
    // instruction, which is two bytes after the start of the instruction.
    return -126 <= BrOffset && BrOffset <= 129;
  case MOS::JMP:
    return true;
  }
}

MachineBasicBlock *
MOSInstrInfo::getBranchDestBlock(const MachineInstr &MI) const {
  switch (MI.getOpcode()) {
  default:
    llvm_unreachable("Bad branch opcode");
  case MOS::BR:
  case MOS::BRA:
  case MOS::JMP:
    return MI.getOperand(0).getMBB();
  }
}

bool MOSInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                 MachineBasicBlock *&TBB,
                                 MachineBasicBlock *&FBB,
                                 SmallVectorImpl<MachineOperand> &Cond,
                                 bool AllowModify) const {
  auto I = MBB.getFirstTerminator();

  // Advance past any comparison terminators.
  while (I != MBB.end() && I->isCompare())
    ++I;

  // If no terminators, falls through.
  if (I == MBB.end())
    return false;

  // Non-branch terminators cannot be analyzed.
  if (!I->isBranch())
    return true;

  // Analyze first branch.
  auto FirstBR = I++;
  if (FirstBR->isPreISelOpcode())
    return true;
  // First branch always forms true edge, whether conditional or unconditional.
  TBB = getBranchDestBlock(*FirstBR);
  if (FirstBR->isConditionalBranch()) {
    Cond.push_back(FirstBR->getOperand(1));
    Cond.push_back(FirstBR->getOperand(2));
  }

  // If there's no second branch, done.
  if (I == MBB.end())
    return false;

  // Cannot analyze branch followed by non-branch.
  if (!I->isBranch())
    return true;

  auto SecondBR = I++;

  // If any instructions follow the second branch, cannot analyze.
  if (I != MBB.end())
    return true;

  // Exactly two branches present.

  // Can only analyze conditional branch followed by unconditional branch.
  if (!SecondBR->isUnconditionalBranch() || SecondBR->isPreISelOpcode())
    return true;

  // Second unconditional branch forms false edge.
  FBB = getBranchDestBlock(*SecondBR);
  return false;
}

unsigned MOSInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                    int *BytesRemoved) const {
  // Since analyzeBranch succeeded, we know that the only terminators are
  // comparisons and branches.

  auto Begin = MBB.getFirstTerminator();
  auto End = MBB.end();

  // Advance to first branch.
  while (Begin != End && Begin->isCompare())
    ++Begin;

  // Erase all remaining terminators.
  unsigned NumRemoved = std::distance(Begin, End);
  if (BytesRemoved) {
    *BytesRemoved = 0;
    for (auto I = Begin; I != End; ++I)
      *BytesRemoved += getInstSizeInBytes(*I);
  }
  MBB.erase(Begin, End);
  return NumRemoved;
}

unsigned MOSInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *TBB,
                                    MachineBasicBlock *FBB,
                                    ArrayRef<MachineOperand> Cond,
                                    const DebugLoc &DL, int *BytesAdded) const {
  // Since analyzeBranch succeeded and any existing branches were removed, the
  // only remaining terminators are comparisons.

  const MOSSubtarget &STI = MBB.getParent()->getSubtarget<MOSSubtarget>();

  MachineIRBuilder Builder(MBB, MBB.end());
  unsigned NumAdded = 0;
  if (BytesAdded)
    *BytesAdded = 0;

  // Unconditional branch target.
  auto *UBB = TBB;

  // Conditional branch.
  if (!Cond.empty()) {
    assert(TBB);
    // The condition stores the arguments for the BR instruction.
    assert(Cond.size() == 2);

    // The unconditional branch will be to the false branch (if any).
    UBB = FBB;

    // Add conditional branch.
    auto BR = Builder.buildInstr(MOS::BR).addMBB(TBB);
    for (const MachineOperand &Op : Cond)
      BR.add(Op);
    ++NumAdded;
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(*BR);
  }

  // Add unconditional branch if necessary.
  if (UBB) {
    // For 65C02, assume BRA and relax into JMP in insertIndirectBranch if
    // necessary.
    auto JMP =
        Builder.buildInstr(STI.has65C02() ? MOS::BRA : MOS::JMP).addMBB(UBB);
    ++NumAdded;
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(*JMP);
  }

  return NumAdded;
}

unsigned MOSInstrInfo::insertIndirectBranch(MachineBasicBlock &MBB,
                                            MachineBasicBlock &NewDestBB,
                                            const DebugLoc &DL,
                                            int64_t BrOffset,
                                            RegScavenger *RS) const {
  // This method inserts a *direct* branch (JMP), despite its name.
  // LLVM calls this method to fixup unconditional branches; it never calls
  // insertBranch or some hypothetical "insertDirectBranch".
  // See lib/CodeGen/BranchRelaxation.cpp for details.
  // We end up here when a jump is too long for a BRA instruction.

  MachineIRBuilder Builder(MBB, MBB.end());
  Builder.setDebugLoc(DL);

  auto JMP = Builder.buildInstr(MOS::JMP).addMBB(&NewDestBB);
  return getInstSizeInBytes(*JMP);
}

void MOSInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator MI,
                               const DebugLoc &DL, MCRegister DestReg,
                               MCRegister SrcReg, bool KillSrc) const {
  MachineIRBuilder Builder(MBB, MI);
  copyPhysRegImpl(Builder, DestReg, SrcReg);
}

static Register createVReg(MachineIRBuilder &Builder,
                           const TargetRegisterClass &RC) {
  Builder.getMF().getProperties().reset(
      MachineFunctionProperties::Property::NoVRegs);
  return Builder.getMRI()->createVirtualRegister(&RC);
}

void MOSInstrInfo::copyPhysRegImpl(MachineIRBuilder &Builder, Register DestReg,
                                   Register SrcReg) const {
  if (DestReg == SrcReg)
    return;

  const MOSSubtarget &STI = Builder.getMF().getSubtarget<MOSSubtarget>();
  const TargetRegisterInfo &TRI = *STI.getRegisterInfo();

  const auto &IsClass = [&](Register Reg, const TargetRegisterClass &RC) {
    if (Reg.isPhysical() && !RC.contains(Reg))
      return false;
    if (Reg.isVirtual() &&
        !Builder.getMRI()->getRegClass(Reg)->hasSuperClassEq(&RC))
      return false;
    return true;
  };

  const auto &AreClasses = [&](const TargetRegisterClass &Dest,
                               const TargetRegisterClass &Src) {
    return IsClass(DestReg, Dest) && IsClass(SrcReg, Src);
  };

  if (AreClasses(MOS::GPRRegClass, MOS::GPRRegClass)) {
    if (IsClass(SrcReg, MOS::AcRegClass)) {
      assert(MOS::XYRegClass.contains(DestReg));
      Builder.buildInstr(MOS::TA).addDef(DestReg).addUse(SrcReg);
    } else if (IsClass(DestReg, MOS::AcRegClass)) {
      assert(MOS::XYRegClass.contains(SrcReg));
      Builder.buildInstr(MOS::T_A).addDef(DestReg).addUse(SrcReg);
    } else {
      Register Tmp = createVReg(Builder, MOS::AcRegClass);
      copyPhysRegImpl(Builder, Tmp, SrcReg);
      copyPhysRegImpl(Builder, DestReg, Tmp);
    }
  } else if (AreClasses(MOS::Imag8RegClass, MOS::GPRRegClass)) {
    Builder.buildInstr(MOS::STImag8).addDef(DestReg).addUse(SrcReg);
  } else if (AreClasses(MOS::GPRRegClass, MOS::Imag8RegClass)) {
    Builder.buildInstr(MOS::LDImag8).addDef(DestReg).addUse(SrcReg);
  } else if (AreClasses(MOS::Imag8RegClass, MOS::Imag8RegClass)) {
    Register Tmp = createVReg(Builder, MOS::GPRRegClass);
    copyPhysRegImpl(Builder, Tmp, SrcReg);
    copyPhysRegImpl(Builder, DestReg, Tmp);
  } else if (AreClasses(MOS::Imag16RegClass, MOS::Imag16RegClass)) {
    assert(SrcReg.isPhysical() && DestReg.isPhysical());
    copyPhysRegImpl(Builder, TRI.getSubReg(DestReg, MOS::sublo),
                    TRI.getSubReg(SrcReg, MOS::sublo));
    copyPhysRegImpl(Builder, TRI.getSubReg(DestReg, MOS::subhi),
                    TRI.getSubReg(SrcReg, MOS::subhi));
  } else if (AreClasses(MOS::Anyi1RegClass, MOS::Anyi1RegClass)) {
    assert(SrcReg.isPhysical() && DestReg.isPhysical());
    Register SrcReg8 =
        TRI.getMatchingSuperReg(SrcReg, MOS::sublsb, &MOS::Anyi8RegClass);
    Register DestReg8 =
        TRI.getMatchingSuperReg(DestReg, MOS::sublsb, &MOS::Anyi8RegClass);

    if (SrcReg8) {
      SrcReg = SrcReg8;
      if (DestReg8) {
        DestReg = DestReg8;
        const MachineInstr &MI = *Builder.getInsertPt();
        // MOS defines LSB writes to write the whole 8-bit register, not just
        // part of it.
        assert(!MI.readsRegister(DestReg));

        copyPhysRegImpl(Builder, DestReg, SrcReg);
      } else {
        if (DestReg == MOS::C) {
          if (!MOS::GPRRegClass.contains(SrcReg)) {
            Register Tmp = createVReg(Builder, MOS::GPRRegClass);
            copyPhysRegImpl(Builder, Tmp, SrcReg);
            SrcReg = Tmp;
          }
          // C = SrcReg >= 1
          Builder.buildInstr(MOS::CMPImm, {MOS::C}, {SrcReg, INT64_C(1)});
        } else {
          assert(DestReg == MOS::V);
          const TargetRegisterClass &StackRegClass =
              STI.has65C02() ? MOS::GPRRegClass : MOS::AcRegClass;

          if (StackRegClass.contains(SrcReg)) {
            Builder.buildInstr(MOS::PH, {}, {SrcReg});
            Builder.buildInstr(MOS::PL, {SrcReg}, {})
                .addDef(MOS::NZ, RegState::Implicit);
            Builder.buildInstr(MOS::SelectImm, {MOS::V},
                               {Register(MOS::Z), INT64_C(0), INT64_C(-1)});
          } else {
            Register Tmp = createVReg(Builder, StackRegClass);
            copyPhysRegImpl(Builder, Tmp, SrcReg);
            std::prev(Builder.getInsertPt())
                ->addOperand(MachineOperand::CreateReg(MOS::NZ,
                                                       /*isDef=*/true,
                                                       /*isImp=*/true));
            Builder.buildInstr(MOS::SelectImm, {MOS::V},
                               {Register(MOS::Z), INT64_C(0), INT64_C(-1)});
          }
        }
      }
    } else {
      if (DestReg8) {
        DestReg = DestReg8;

        Register Tmp = DestReg;
        if (!MOS::GPRRegClass.contains(Tmp))
          Tmp = createVReg(Builder, MOS::GPRRegClass);
        Builder.buildInstr(MOS::SelectImm, {Tmp},
                           {SrcReg, INT64_C(1), INT64_C(0)});
        if (Tmp != DestReg)
          copyPhysRegImpl(Builder, DestReg, Tmp);
      } else {
        Builder.buildInstr(MOS::SelectImm, {DestReg},
                           {SrcReg, INT64_C(-1), INT64_C(0)});
      }
    }
  } else
    llvm_unreachable("Unexpected physical register copy.");
}

void MOSInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MI,
                                       Register SrcReg, bool isKill,
                                       int FrameIndex,
                                       const TargetRegisterClass *RC,
                                       const TargetRegisterInfo *TRI) const {
  loadStoreRegStackSlot(MBB, MI, SrcReg, isKill, FrameIndex, RC, TRI,
                        /*IsLoad=*/false);
}

void MOSInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator MI,
                                        Register DestReg, int FrameIndex,
                                        const TargetRegisterClass *RC,
                                        const TargetRegisterInfo *TRI) const {
  loadStoreRegStackSlot(MBB, MI, DestReg, false, FrameIndex, RC, TRI,
                        /*IsLoad=*/true);
}

// Load or store one byte from/to a location on the static stack.
static void loadStoreByteStaticStackSlot(MachineIRBuilder &Builder,
                                         MachineOperand MO, int FrameIndex,
                                         int64_t Offset,
                                         MachineMemOperand *MMO) {
  const MachineRegisterInfo &MRI = *Builder.getMRI();
  const TargetRegisterInfo &TRI =
      *Builder.getMF().getSubtarget().getRegisterInfo();

  Register Reg = MO.getReg();

  // Convert bit to byte if directly possible.
  if (Reg.isPhysical() && MOS::GPR_LSBRegClass.contains(Reg)) {
    Reg = TRI.getMatchingSuperReg(Reg, MOS::sublsb, &MOS::GPRRegClass);
    MO.setReg(Reg);
  } else if (Reg.isVirtual() &&
             MRI.getRegClass(Reg)->hasSuperClassEq(&MOS::GPRRegClass) &&
             MO.getSubReg() == MOS::sublsb) {
    MO.setSubReg(0);
  }

  // Emit directly through GPR if possible.
  if ((Reg.isPhysical() && MOS::GPRRegClass.contains(Reg)) ||
      (Reg.isVirtual() &&
       MRI.getRegClass(Reg)->hasSuperClassEq(&MOS::GPRRegClass) &&
       !MO.getSubReg())) {
    Builder.buildInstr(MO.isDef() ? MOS::LDAbsOffset : MOS::STAbsOffset)
        .add(MO)
        .addFrameIndex(FrameIndex)
        .addImm(Offset)
        .addMemOperand(MMO);
    return;
  }

  // Emit via copy through GPR.
  bool IsBit = (Reg.isPhysical() && MOS::Anyi1RegClass.contains(Reg)) ||
               (Reg.isVirtual() &&
                (MRI.getRegClass(Reg)->hasSuperClassEq(&MOS::Anyi1RegClass) ||
                 MO.getSubReg() == MOS::sublsb));
  MachineOperand Tmp = MachineOperand::CreateReg(
      Builder.getMRI()->createVirtualRegister(&MOS::GPRRegClass), MO.isDef());
  if (Tmp.isUse()) {
    // Define the temporary register via copy from the MO.
    MachineOperand TmpDef = Tmp;
    TmpDef.setIsDef();
    if (IsBit) {
      TmpDef.setSubReg(MOS::sublsb);
      TmpDef.setIsUndef();
    }
    Builder.buildInstr(MOS::COPY).add(TmpDef).add(MO);

    loadStoreByteStaticStackSlot(Builder, Tmp, FrameIndex, Offset, MMO);
  } else {
    assert(Tmp.isDef());

    loadStoreByteStaticStackSlot(Builder, Tmp, FrameIndex, Offset, MMO);

    // Define the MO via copy from the temporary register.
    MachineOperand TmpUse = Tmp;
    TmpUse.setIsUse();
    if (IsBit)
      TmpUse.setSubReg(MOS::sublsb);
    Builder.buildInstr(MOS::COPY).add(MO).add(TmpUse);
  }
}

void MOSInstrInfo::loadStoreRegStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register Reg,
    bool IsKill, int FrameIndex, const TargetRegisterClass *RC,
    const TargetRegisterInfo *TRI, bool IsLoad) const {
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  MachinePointerInfo PtrInfo =
      MachinePointerInfo::getFixedStack(MF, FrameIndex);
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      PtrInfo, IsLoad ? MachineMemOperand::MOLoad : MachineMemOperand::MOStore,
      MFI.getObjectSize(FrameIndex), MFI.getObjectAlign(FrameIndex));

  MachineIRBuilder Builder(MBB, MI);
  MachineInstrSpan MIS(MI, &MBB);

  // If we're using the soft stack, since the offset is not yet known, it may be
  // either 8 or 16 bits. Emit a 16-bit pseudo to be lowered during frame index
  // elimination.
  if (!MF.getFunction().doesNotRecurse()) {
    Register Ptr = MRI.createVirtualRegister(&MOS::Imag16RegClass);
    auto Instr = Builder.buildInstr(IsLoad ? MOS::LDStk : MOS::STStk);
    if (!IsLoad)
      Instr.addDef(Ptr, RegState::EarlyClobber);
    Instr.addReg(Reg, getDefRegState(IsLoad) | getKillRegState(IsKill));
    if (IsLoad)
      Instr.addDef(Ptr, RegState::EarlyClobber);
    Instr.addFrameIndex(FrameIndex).addImm(0).addMemOperand(MMO);
  } else {
    if ((Reg.isPhysical() && MOS::Imag16RegClass.contains(Reg)) ||
        (Reg.isVirtual() &&
         MRI.getRegClass(Reg)->hasSuperClassEq(&MOS::Imag16RegClass))) {
      MachineOperand Lo = MachineOperand::CreateReg(Reg, IsLoad);
      MachineOperand Hi = Lo;
      Register Tmp = Reg;
      if (Reg.isPhysical()) {
        Lo.setReg(TRI->getSubReg(Reg, MOS::sublo));
        Hi.setReg(TRI->getSubReg(Reg, MOS::subhi));
      } else {
        assert(Reg.isVirtual());
        // Live intervals for the original virtual register will already have
        // been computed by this point. Since this code introduces subregisters,
        // these must be using a new virtual register; otherwise there would be
        // no subregister live ranges for the new instructions. This can cause
        // VirtRegMap to fail.
        Tmp = MRI.createVirtualRegister(&MOS::Imag16RegClass);
        Lo.setReg(Tmp);
        Lo.setSubReg(MOS::sublo);
        if (Lo.isDef())
          Lo.setIsUndef();
        Hi.setReg(Tmp);
        Hi.setSubReg(MOS::subhi);
      }
      if (!IsLoad) {
        if (Tmp != Reg)
          Builder.buildCopy(Tmp, Reg);

        // The register may not have been fully defined at this point. Adding a
        // KILL here makes the entire value alive, regardless of whether or not
        // it was prior to the store. We do this because this function does not
        // have access to the detailed liveness information about the virtual
        // register in use; if we did, we'd only need to store the portion of
        // the virtual register that is actually alive.
        Builder.buildInstr(MOS::KILL, {Tmp}, {Tmp});
      }
      loadStoreByteStaticStackSlot(Builder, Lo, FrameIndex, 0,
                                   MF.getMachineMemOperand(MMO, 0, 1));
      loadStoreByteStaticStackSlot(Builder, Hi, FrameIndex, 1,
                                   MF.getMachineMemOperand(MMO, 1, 1));
      if (IsLoad && Tmp != Reg)
        Builder.buildCopy(Reg, Tmp);
    } else {
      loadStoreByteStaticStackSlot(
          Builder, MachineOperand::CreateReg(Reg, IsLoad), FrameIndex, 0, MMO);
    }
  }

  LLVM_DEBUG({
    dbgs() << "Inserted stack slot load/store:\n";
    for (auto MI = MIS.begin(), End = MIS.getInitial(); MI != End; ++MI)
      dbgs() << *MI;
  });
}

bool MOSInstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  MachineIRBuilder Builder(MI);

  bool Changed = true;
  switch (MI.getOpcode()) {
  default:
    Changed = false;
    break;
  case MOS::CMPImmTerm:
  case MOS::CMPImag8Term:
    expandCMPTerm(Builder);
    break;
  case MOS::SBCNZImag8:
    expandSBCNZImag8(Builder);
    break;
  case MOS::LDIdx:
    expandLDIdx(Builder);
    break;
  case MOS::LDImm1:
    expandLDImm1(Builder);
    break;
  case MOS::SetSPLo:
  case MOS::SetSPHi:
    expandSetSP(Builder);
    break;
  }

  return Changed;
}

void MOSInstrInfo::expandCMPTerm(MachineIRBuilder &Builder) const {
  MachineInstr &MI = *Builder.getInsertPt();
  switch (MI.getOpcode()) {
  case MOS::CMPImmTerm:
    MI.setDesc(Builder.getTII().get(MOS::CMPImm));
    break;
  case MOS::CMPImag8Term:
    MI.setDesc(Builder.getTII().get(MOS::CMPImag8));
    break;
  }
  MI.addOperand(
      MachineOperand::CreateReg(MOS::NZ, /*isDef=*/true, /*isImp=*/true));
}

void MOSInstrInfo::expandSBCNZImag8(MachineIRBuilder &Builder) const {
  MachineInstr &MI = *Builder.getInsertPt();
  auto SBC = Builder.buildInstr(
      MOS::SBCImag8, {MI.getOperand(0), MI.getOperand(1), MI.getOperand(3)},
      {MI.getOperand(5), MI.getOperand(6), MI.getOperand(7)});
  Register NZOut = MI.getOperand(2).getReg();
  Register NZIn = MOS::N;
  if (NZOut == MOS::NoRegister) {
    NZOut = MI.getOperand(4).getReg();
    NZIn = MOS::Z;
  } else
    assert(MI.getOperand(4).getReg() == MOS::NoRegister &&
           "At most one of N and Z can be set in SBCNZImag8");
  if (NZOut != MOS::NoRegister) {
    SBC.addDef(MOS::NZ, RegState::Implicit);
    Builder.buildInstr(MOS::SelectImm, {NZOut},
                       {NZIn, INT64_C(-1), INT64_C(0)});
  }
  MI.eraseFromParent();
}

void MOSInstrInfo::expandLDIdx(MachineIRBuilder &Builder) const {
  auto &MI = *Builder.getInsertPt();

  // This occur when X or Y is both the destination and index register.
  // Since the 6502 has no instruction for this, use A as the destination
  // instead, then transfer to the real destination.
  if (MI.getOperand(0).getReg() == MI.getOperand(2).getReg()) {
    Register Tmp = createVReg(Builder, MOS::AcRegClass);
    Builder.buildInstr(MOS::LDAIdx)
        .addDef(Tmp)
        .add(MI.getOperand(1))
        .add(MI.getOperand(2));
    Builder.buildInstr(MOS::TA).add(MI.getOperand(0)).addUse(Tmp);
    MI.eraseFromParent();
    return;
  }

  unsigned Opcode;
  switch (MI.getOperand(0).getReg()) {
  default:
    llvm_unreachable("Bad destination for LDIdx.");
  case MOS::A:
    Opcode = MOS::LDAIdx;
    break;
  case MOS::X:
    Opcode = MOS::LDXIdx;
    break;
  case MOS::Y:
    Opcode = MOS::LDYIdx;
    break;
  }

  MI.setDesc(Builder.getTII().get(Opcode));
}

void MOSInstrInfo::expandLDImm1(MachineIRBuilder &Builder) const {
  auto &MI = *Builder.getInsertPt();
  Register DestReg = MI.getOperand(0).getReg();
  int64_t Val = MI.getOperand(1).getImm();

  unsigned Opcode;
  switch (DestReg) {
  default: {
    DestReg =
        Builder.getMF().getSubtarget().getRegisterInfo()->getMatchingSuperReg(
            DestReg, MOS::sublsb, &MOS::Anyi8RegClass);
    assert(DestReg && "Unexpected destination for LDImm1");
    assert(MOS::GPRRegClass.contains(DestReg));
    Opcode = MOS::LDImm;
    MI.getOperand(0).setReg(DestReg);
    MI.getOperand(1).setImm(!!Val);
    break;
  }
  case MOS::C:
    Opcode = MOS::LDCImm;
    break;
  case MOS::V:
    if (Val) {
      auto Instr = Builder.buildInstr(MOS::BITAbs, {MOS::V}, {})
                       .addUse(MOS::A, RegState::Undef)
                       .addExternalSymbol("__set_v");
      Instr->getOperand(1).setIsUndef();
      MI.eraseFromParent();
      return;
    }
    Opcode = MOS::CLV;
    // Remove imm.
    MI.RemoveOperand(1);
    break;
  }

  MI.setDesc(Builder.getTII().get(Opcode));
}

void MOSInstrInfo::expandSetSP(MachineIRBuilder &Builder) const {
  auto &MI = *Builder.getInsertPt();
  Register Src = MI.getOperand(0).getReg();

  if (MI.getOpcode() == MOS::SetSPLo) {
    copyPhysRegImpl(Builder, MOS::RC0, Src);
  } else {
    assert(MI.getOpcode() == MOS::SetSPHi);
    copyPhysRegImpl(Builder, MOS::RC1, Src);
  }
  MI.eraseFromParent();
}

bool MOSInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 2);
  auto &Val = Cond[1];
  Val.setImm(!Val.getImm());
  // Success.
  return false;
}

std::pair<unsigned, unsigned>
MOSInstrInfo::decomposeMachineOperandsTargetFlags(unsigned TF) const {
  return std::make_pair(TF, 0u);
}

ArrayRef<std::pair<int, const char *>>
MOSInstrInfo::getSerializableTargetIndices() const {
  static const std::pair<int, const char *> Flags[] = {
      {MOS::TI_STATIC_STACK, "mos-static-stack"}};
  return Flags;
}

ArrayRef<std::pair<unsigned, const char *>>
MOSInstrInfo::getSerializableDirectMachineOperandTargetFlags() const {
  static const std::pair<unsigned, const char *> Flags[] = {{MOS::MO_LO, "lo"},
                                                            {MOS::MO_HI, "hi"}};
  return Flags;
}
