//===- ExpandTlsConstantExpr.cpp - Convert ConstantExprs to Instructions---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass is a helper used by the ExpandTls pass.
//
// LLVM treats the address of a TLS variable as a ConstantExpr.  This
// is arguably a bug because the address of a TLS variable is *not* a
// constant: it varies between threads.
//
// See http://llvm.org/bugs/show_bug.cgi?id=14353
//
// This is also a problem for the ExpandTls pass, which wants to use
// replaceUsesOfWith() to replace each TLS variable with an
// Instruction sequence that calls @llvm.nacl.read.tp().  This doesn't
// work if the TLS variable is used inside other ConstantExprs,
// because ConstantExprs are interned and are not associated with any
// function, whereas each Instruction must be part of a function.
//
// To fix that problem, this pass converts ConstantExprs that
// reference TLS variables into Instructions.
//
// For example, this use of a 'ptrtoint' ConstantExpr:
//
//   ret i32 ptrtoint (i32* @tls_var to i32)
//
// is converted into this 'ptrtoint' Instruction:
//
//   %expanded = ptrtoint i32* @tls_var to i32
//   ret i32 %expanded
//
//===----------------------------------------------------------------------===//

#include <vector>

#include "llvm/Pass.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/NaCl.h"

using namespace llvm;

namespace {
  class ExpandTlsConstantExpr : public ModulePass {
  public:
    static char ID; // Pass identification, replacement for typeid
    ExpandTlsConstantExpr() : ModulePass(ID) {
      initializeExpandTlsConstantExprPass(*PassRegistry::getPassRegistry());
    }

    virtual bool runOnModule(Module &M);
  };
}

char ExpandTlsConstantExpr::ID = 0;
INITIALIZE_PASS(ExpandTlsConstantExpr, "nacl-expand-tls-constant-expr",
                "Eliminate ConstantExpr references to TLS variables",
                false, false)

// This removes ConstantExpr references to the given Constant.
static void expandConstExpr(Constant *Expr) {
  // First, ensure that ConstantExpr references to Expr are converted
  // to Instructions so that we can modify them.
  for (Value::use_iterator UI = Expr->use_begin();
       UI != Expr->use_end();
       ++UI) {
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(*UI)) {
      expandConstExpr(CE);
    }
  }
  Expr->removeDeadConstantUsers();

  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Expr)) {
    while (!Expr->use_empty()) {
      Instruction *U = cast<Instruction>(*Expr->use_begin());
      Instruction *InsertPt = U;
      if (PHINode *PN = dyn_cast<PHINode>(InsertPt)) {
        // We cannot insert instructions before a PHI node, so insert
        // before the incoming block's terminator.  This could be
        // suboptimal if the terminator is a conditional.
        InsertPt = PN->getIncomingBlock(Expr->use_begin())->getTerminator();
      }
      Instruction *NewInst = CE->getAsInstruction();
      NewInst->insertBefore(InsertPt);
      NewInst->setName("expanded");
      U->replaceUsesOfWith(CE, NewInst);
    }
  }
}

bool ExpandTlsConstantExpr::runOnModule(Module &M) {
  for (Module::global_iterator Global = M.global_begin();
       Global != M.global_end();
       ++Global) {
    if (Global->isThreadLocal()) {
      expandConstExpr(Global);
    }
  }
  return true;
}

ModulePass *llvm::createExpandTlsConstantExprPass() {
  return new ExpandTlsConstantExpr();
}
