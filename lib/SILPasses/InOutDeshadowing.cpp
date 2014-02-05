//===--- InOutDeshadowing.cpp - Remove non-escaping inout shadows ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// SILGen produces shadow variables for "inout" arguments to provide proper
// semantics for when the inout argument is closed over.  However, this shadow
// value is *only* needed when the argument is closed over (and when that
// closure isn't inlined).  This pass looks for shadow allocations and removes
// them.
//
// This is a guaranteed optimization pass, because adding additional references
// can cause algorithmic performance changes, e.g. turning amortized constant
// time string and array operations into linear time operations.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "inout-deshadow"
#include "swift/SILPasses/Passes.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILPasses/PassManager.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumShadowsRemoved, "Number of inout shadow variables removed");
STATISTIC(NumShadowsKept, "Number of inout shadow variables kept");

//===----------------------------------------------------------------------===//
//                          inout Deshadowing
//===----------------------------------------------------------------------===//

/// promoteShadow - Given an AllocStackInst that is copied to/from an @inout
/// argument, completely replace the alloc_stack with that inout argument.
static void promoteShadow(AllocStackInst *Alloc, SILArgument *InOutArg) {

  // Since the allocation has already been promoted to an alloc_stack, we know
  // it doesn't escape.  Simply eliminate the allocation and any obviously
  // trivial noop copies into and out of it.
  while (!Alloc->use_empty()) {
    auto Use = *Alloc->use_begin();
    auto *User = Use->getUser();

    // If this is a use of the 0th result, not the address result, just zap the
    // instruction.  It is a dealloc_stack or something similar.
    if (Use->get().getResultNumber() == 0) {
      User->eraseFromParent();
      continue;
    }

    // Otherwise, it is a use of the argument.  If this is a copy_addr that
    // defines or destroys the value, then remove it.
    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      if (CAI->getSrc() == InOutArg || CAI->getDest() == InOutArg) {
        User->eraseFromParent();
        continue;
      }
    }

    // Otherwise, this is something else that is using the memory.  Remap this
    // to use the InOutArg directly instead of using the allocation.
    Use->set(InOutArg);
  }

  Alloc->eraseFromParent();
}


//===----------------------------------------------------------------------===//
//                     Candidate Variable Identification
//===----------------------------------------------------------------------===//

/// isCopyToOrFromStack - Check to see if the specified use of an @inout
/// argument is a copy_addr to/from an alloc_stack.
///
/// This returns the alloc_stack if found, or null if not.
static AllocStackInst *isCopyToOrFromStack(Operand *UI) {
  auto CAI = dyn_cast<CopyAddrInst>(UI->getUser());
  if (CAI == nullptr) return nullptr;

  // We only look at autogenerated copy_addr's.  We don't want to muck with
  // user variables, as in:
  //   func f(a : @inout Int) { var b = a }
  if (!CAI->getLoc().isAutoGenerated() && !CAI->getLoc().is<SILFileLocation>())
    return nullptr;
  
  // If this is an explicit copy_addr, return the other operand if it is a stack
  // allocation.
  SILValue OtherOp =
    UI->getOperandNumber() == 0 ? CAI->getDest() : CAI->getSrc();
  
  // Look through mark_uninitialized.
  if (auto *MUI = dyn_cast<MarkUninitializedInst>(OtherOp))
    OtherOp = MUI->getOperand();
  
  return dyn_cast<AllocStackInst>(OtherOp);
}


/// processInOutValue - Walk the use-def list of the inout argument to find uses
/// of it.  If we find any autogenerated copies to/from an alloc_stack, then
/// remove the alloc stack in favor of loading/storing to the inout pointer
/// directly.
///
/// This returns true if it promotes away the shadow variable.
///
static bool processInOutValue(SILArgument *InOutArg) {
  assert(InOutArg->getType().isAddress() &&
         "inout arguments should always be addresses");

  for (auto UI : InOutArg->getUses())
    if (AllocStackInst *ASI = isCopyToOrFromStack(UI)) {
      DEBUG(llvm::dbgs() << "    Promoting shadow variable " << *ASI);
      promoteShadow(ASI, InOutArg);
      return true;
    }

  // If we fail, dump out some internal state.
  DEBUG({
    llvm::dbgs() << "*** Failed to deshadow.  Uses:\n";
    for (auto UI : InOutArg->getUses())
      llvm::dbgs() << "    " << *UI->getUser();
  });
  
  return false;
}

//===----------------------------------------------------------------------===//
//                          Top Level Driver
//===----------------------------------------------------------------------===//

void swift::performInOutDeshadowing(SILModule *M) {
  DEBUG(llvm::dbgs() << "*** inout Deshadowing\n");

  for (auto &Fn : *M) {
    if (Fn.empty()) continue;
    SILBasicBlock &EntryBlock = Fn.front();

    // For each function, find any inout arguments and try to optimize each of
    // them.
    SILFunctionType *FTI = Fn.getLoweredFunctionType();
    
    for (unsigned arg = 0, e = FTI->getInterfaceParameters().size(); arg != e; ++arg) {
      if (!FTI->getInterfaceParameters()[arg].isIndirectInOut()) continue;

      DEBUG(llvm::dbgs() << "  " << Fn.getName() << ": argument #"
                         << arg << "\n");

      if (processInOutValue(EntryBlock.getBBArgs()[arg]))
        ++NumShadowsRemoved;
      else {
        ++NumShadowsKept;
      }
    }
  }
}

class InOutDeshadowing : public SILFunctionTrans {
  virtual ~InOutDeshadowing() {}

  /// The entry point to the transformation.
  virtual void runOnFunction(SILFunction &F, SILPassManager *PM) {
    SILBasicBlock &EntryBlock = F.front();

    // For each function, find any inout arguments and try to optimize each of
    // them.
    SILFunctionType *FTI = F.getLoweredFunctionType();

    for (unsigned arg = 0, e = FTI->getInterfaceParameters().size();
         arg != e; ++arg) {
      if (!FTI->getInterfaceParameters()[arg].isIndirectInOut()) continue;

      DEBUG(llvm::dbgs()<< "  " << F.getName() << ": argument #"<< arg << "\n");

      if (processInOutValue(EntryBlock.getBBArgs()[arg]))
        ++NumShadowsRemoved;
      else {
        ++NumShadowsKept;
      }
    }
  }
};

SILTransform *swift::createInOutDeshadowing() {
  return new InOutDeshadowing();
}

