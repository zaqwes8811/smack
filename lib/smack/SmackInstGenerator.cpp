//
// This file is distributed under the MIT License. See LICENSE for details.
//
#define DEBUG_TYPE "smack-inst-gen"
#include "smack/SmackInstGenerator.h"
#include "smack/SmackOptions.h"
#include "smack/Naming.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Analysis/LoopInfo.h"
#include <sstream>

#include <iostream>
#include "llvm/Support/raw_ostream.h"
#include "dsa/DSNode.h"

namespace smack {

using llvm::errs;

const bool CODE_WARN = true;
const bool SHOW_ORIG = false;

#define WARN(str) \
    if (CODE_WARN) emit(Stmt::comment(std::string("WARNING: ") + str))
#define ORIG(ins) \
    if (SHOW_ORIG) emit(Stmt::comment(i2s(ins)))

Regex VAR_DECL("^[[:space:]]*var[[:space:]]+([[:alpha:]_.$#'`~^\\?][[:alnum:]_.$#'`~^\\?]*):.*;");

// Procedures whose return value should not be marked as external
Regex EXTERNAL_PROC_IGNORE("^(malloc|__VERIFIER_nondet)$");

std::string i2s(llvm::Instruction& i) {
  std::string s;
  llvm::raw_string_ostream ss(s);
  ss << i;
  s = s.substr(2);
  return s;
}

Block* SmackInstGenerator::createBlock() {
  Block* b = Block::block(naming.freshBlockName());
  proc.getBlocks().push_back(b);
  return b;
}

Block* SmackInstGenerator::getBlock(llvm::BasicBlock* bb) {
  if (blockMap.count(bb) == 0)
    blockMap[bb] = createBlock();
  return blockMap[bb];
}

void SmackInstGenerator::nameInstruction(llvm::Instruction& inst) {
  if (inst.getType()->isVoidTy())
    return;
  proc.getDeclarations().push_back(
    Decl::variable(naming.get(inst), rep.type(&inst))
  );
}

void SmackInstGenerator::annotate(llvm::Instruction& I, Block* B) {

  // do not generate sourceloc from calls to llvm.debug since
  // those point to variable declaration lines and such
  if (llvm::CallInst* ci = llvm::dyn_cast<llvm::CallInst>(&I)) {
    llvm::Function* f = ci->getCalledFunction();
    std::string name = f && f->hasName() ? f->getName().str() : "";
    if (name.find("llvm.dbg.") != std::string::npos) {
      return;
    }
  }

  if (llvm::MDNode* M = I.getMetadata("dbg")) {
    llvm::DILocation L(M);

    if (SmackOptions::SourceLocSymbols)
      B->addStmt(Stmt::annot(Attr::attr("sourceloc", L.getFilename().str(),
        L.getLineNumber(), L.getColumnNumber())));
  }
}

void SmackInstGenerator::processInstruction(llvm::Instruction& inst) {
  DEBUG(errs() << "Inst: " << inst << "\n");
  annotate(inst, currBlock);
  ORIG(inst);
  nameInstruction(inst);
}

void SmackInstGenerator::visitBasicBlock(llvm::BasicBlock& bb) {
  currBlock = getBlock(&bb);
}

void SmackInstGenerator::visitInstruction(llvm::Instruction& inst) {
  DEBUG(errs() << "Instruction not handled: " << inst << "\n");
  llvm_unreachable("Instruction not handled.");
}

void SmackInstGenerator::generatePhiAssigns(llvm::TerminatorInst& ti) {
  llvm::BasicBlock* block = ti.getParent();
  std::list<const Expr*> lhs;
  std::list<const Expr*> rhs;
  for (unsigned i = 0; i < ti.getNumSuccessors(); i++) {

    // write to the phi-node variable of the successor
    for (llvm::BasicBlock::iterator
         s = ti.getSuccessor(i)->begin(), e = ti.getSuccessor(i)->end();
         s != e && llvm::isa<llvm::PHINode>(s); ++s) {

      llvm::PHINode* phi = llvm::cast<llvm::PHINode>(s);
      if (llvm::Value* v =
            phi->getIncomingValueForBlock(block)) {
        lhs.push_back(rep.expr(phi));
        rhs.push_back(rep.expr(v));
      }
    }
  }
  if (!lhs.empty()) {
    emit(Stmt::assign(lhs, rhs));
  }
}

void SmackInstGenerator::generateGotoStmts(
  llvm::Instruction& inst,
  std::vector<std::pair<const Expr*, llvm::BasicBlock*> > targets) {

  assert(targets.size() > 0);

  if (targets.size() > 1) {
    std::list<std::string> dispatch;

    for (unsigned i = 0; i < targets.size(); i++) {
      const Expr* condition = targets[i].first;
      llvm::BasicBlock* target = targets[i].second;

      if (target->getUniquePredecessor() == inst.getParent()) {
        Block* b = getBlock(target);
        b->insert(Stmt::assume(condition));
        dispatch.push_back(b->getName());

      } else {
        Block* b = createBlock();
        annotate(inst, b);
        b->addStmt(Stmt::assume(condition));
        b->addStmt(Stmt::goto_({getBlock(target)->getName()}));
        dispatch.push_back(b->getName());
      }
    }

    emit(Stmt::goto_(dispatch));

  } else
    emit(Stmt::goto_({getBlock(targets[0].second)->getName()}));
}

/******************************************************************************/
/*                 TERMINATOR                  INSTRUCTIONS                   */
/******************************************************************************/

void SmackInstGenerator::visitReturnInst(llvm::ReturnInst& ri) {
  processInstruction(ri);

  llvm::Value* v = ri.getReturnValue();
  if (v)
    emit(Stmt::assign(Expr::id(Naming::RET_VAR), rep.expr(v)));
  emit(Stmt::assign(Expr::id(Naming::EXN_VAR), Expr::lit(false)));
  emit(Stmt::return_());
}

void SmackInstGenerator::visitBranchInst(llvm::BranchInst& bi) {
  processInstruction(bi);

  // Collect the list of tarets
  std::vector<std::pair<const Expr*, llvm::BasicBlock*> > targets;

  if (bi.getNumSuccessors() == 1) {

    // Unconditional branch
    targets.push_back({Expr::lit(true),bi.getSuccessor(0)});

  } else {

    // Conditional branch
    assert(bi.getNumSuccessors() == 2);
    const Expr* e = Expr::eq(rep.expr(bi.getCondition()), rep.integerLit(1UL,1));
    targets.push_back({e,bi.getSuccessor(0)});
    targets.push_back({Expr::not_(e),bi.getSuccessor(1)});
  }
  generatePhiAssigns(bi);
  if (bi.getNumSuccessors() > 1)
    emit(Stmt::annot(Attr::attr(Naming::BRANCH_CONDITION_ANNOTATION,
      {rep.expr(bi.getCondition())})));
  generateGotoStmts(bi, targets);
}

void SmackInstGenerator::visitSwitchInst(llvm::SwitchInst& si) {
  processInstruction(si);

  // Collect the list of tarets
  std::vector<std::pair<const Expr*, llvm::BasicBlock*> > targets;

  const Expr* e = rep.expr(si.getCondition());
  const Expr* n = Expr::lit(true);

  for (llvm::SwitchInst::CaseIt
       i = si.case_begin(); i != si.case_begin(); ++i) {

    const Expr* v = rep.expr(i.getCaseValue());
    targets.push_back({Expr::eq(e,v),i.getCaseSuccessor()});

    // Add the negation of this case to the default case
    n = Expr::and_(n, Expr::neq(e, v));
  }

  // The default case
  targets.push_back({n,si.getDefaultDest()});

  generatePhiAssigns(si);
  emit(Stmt::annot(Attr::attr(Naming::BRANCH_CONDITION_ANNOTATION,
    {rep.expr(si.getCondition())})));
  generateGotoStmts(si, targets);
}

void SmackInstGenerator::visitInvokeInst(llvm::InvokeInst& ii) {
  processInstruction(ii);
  llvm::Function* f = ii.getCalledFunction();
  if (f) {
    emit(rep.call(f, ii));
  } else {
    // llvm_unreachable("Unexpected invoke instruction.");
    WARN("unsoundly ignoring invoke instruction... ");
  }
  std::vector<std::pair<const Expr*, llvm::BasicBlock*> > targets;
  targets.push_back({
    Expr::not_(Expr::id(Naming::EXN_VAR)),
    ii.getNormalDest()});
  targets.push_back({
    Expr::id(Naming::EXN_VAR),
    ii.getUnwindDest()});
  emit(Stmt::annot(Attr::attr(Naming::BRANCH_CONDITION_ANNOTATION,
    {Expr::id(Naming::EXN_VAR)})));
  generateGotoStmts(ii, targets);
}

void SmackInstGenerator::visitResumeInst(llvm::ResumeInst& ri) {
  processInstruction(ri);
  emit(Stmt::assign(Expr::id(Naming::EXN_VAR), Expr::lit(true)));
  emit(Stmt::assign(Expr::id(Naming::EXN_VAL_VAR), rep.expr(ri.getValue())));
  emit(Stmt::return_());
}

void SmackInstGenerator::visitUnreachableInst(llvm::UnreachableInst& ii) {
  processInstruction(ii);

  emit(Stmt::assume(Expr::lit(false)));
}

/******************************************************************************/
/*                   BINARY                    OPERATIONS                     */
/******************************************************************************/

void SmackInstGenerator::visitBinaryOperator(llvm::BinaryOperator& I) {
  processInstruction(I);
  emit(Stmt::assign(rep.expr(&I),rep.bop(&I)));
}

/******************************************************************************/
/*                   VECTOR                    OPERATIONS                     */
/******************************************************************************/

// TODO implement std::vector operations

/******************************************************************************/
/*                  AGGREGATE                   OPERATIONS                    */
/******************************************************************************/

void SmackInstGenerator::visitExtractValueInst(llvm::ExtractValueInst& evi) {
  processInstruction(evi);
  const Expr* e = rep.expr(evi.getAggregateOperand());
  for (unsigned i = 0; i < evi.getNumIndices(); i++)
    e = Expr::fn(Naming::EXTRACT_VALUE, e, Expr::lit((unsigned long) evi.getIndices()[i]));
  emit(Stmt::assign(rep.expr(&evi),e));
}

void SmackInstGenerator::visitInsertValueInst(llvm::InsertValueInst& ivi) {
  processInstruction(ivi);
  const Expr* old = rep.expr(ivi.getAggregateOperand());
  const Expr* res = rep.expr(&ivi);
  const llvm::Type* t = ivi.getType();

  for (unsigned i = 0; i < ivi.getNumIndices(); i++) {
    unsigned idx = ivi.getIndices()[i];

    unsigned num_elements;
    if (const llvm::StructType* st = llvm::dyn_cast<const llvm::StructType>(t)) {
      num_elements = st->getNumElements();
      t = st->getElementType(idx);
    } else if (const llvm::ArrayType* at = llvm::dyn_cast<const llvm::ArrayType>(t)) {
      num_elements = at->getNumElements();
      t = at->getElementType();
    } else {
      assert (false && "Unexpected aggregate type");
    }

    for (unsigned j = 0; j < num_elements; j++) {
      if (j != idx) {
        emit(Stmt::assume(Expr::eq(
          Expr::fn(Naming::EXTRACT_VALUE, res, Expr::lit(j)),
          Expr::fn(Naming::EXTRACT_VALUE, old, Expr::lit(j))
        )));
      }
    }
    res = Expr::fn(Naming::EXTRACT_VALUE, res, Expr::lit(idx));
    old = Expr::fn(Naming::EXTRACT_VALUE, old, Expr::lit(idx));
  }
  emit(Stmt::assume(Expr::eq(res,rep.expr(ivi.getInsertedValueOperand()))));
}

/******************************************************************************/
/*     MEMORY       ACCESS        AND       ADDRESSING       OPERATIONS       */
/******************************************************************************/

void SmackInstGenerator::visitAllocaInst(llvm::AllocaInst& ai) {
  processInstruction(ai);
  emit(rep.alloca(ai));
}

void SmackInstGenerator::visitLoadInst(llvm::LoadInst& li) {
  processInstruction(li);

  // TODO what happens with aggregate types?
  // assert (!li.getType()->isAggregateType() && "Unexpected load value.");

  emit(Stmt::assign(rep.expr(&li), rep.load(li.getPointerOperand())));

  if (SmackOptions::MemoryModelDebug) {
    emit(Stmt::call(Naming::REC_MEM_OP, {Expr::id(Naming::MEM_OP_VAL)}));
    emit(Stmt::call("boogie_si_record_int", {Expr::lit(0L)}));
    emit(Stmt::call("boogie_si_record_int", {rep.expr(li.getPointerOperand())}));
    emit(Stmt::call("boogie_si_record_int", {rep.expr(&li)}));
  }
}

void SmackInstGenerator::visitStoreInst(llvm::StoreInst& si) {
  processInstruction(si);
  const llvm::Value* P = si.getPointerOperand();
  const llvm::Value* V = si.getOperand(0);
  assert (!V->getType()->isAggregateType() && "Unexpected store value.");

  emit(rep.store(P,V));

  if (SmackOptions::SourceLocSymbols) {
    if (const llvm::GlobalVariable* G = llvm::dyn_cast<const llvm::GlobalVariable>(P)) {
      assert(G->hasName() && "Expected named global variable.");
      emit(Stmt::call("boogie_si_record_" + rep.type(V), {rep.expr(V)}, {}, {Attr::attr("cexpr", G->getName().str())}));
    }
  }

  if (SmackOptions::MemoryModelDebug) {
    emit(Stmt::call(Naming::REC_MEM_OP, {Expr::id(Naming::MEM_OP_VAL)}));
    emit(Stmt::call("boogie_si_record_int", {Expr::lit(1L)}));
    emit(Stmt::call("boogie_si_record_int", {rep.expr(P)}));
    emit(Stmt::call("boogie_si_record_int", {rep.expr(V)}));
  }
}

void SmackInstGenerator::visitAtomicCmpXchgInst(llvm::AtomicCmpXchgInst& i) {
  processInstruction(i);
  const Expr* res = rep.expr(&i);
  const Expr* mem = rep.load(i.getOperand(0));
  const Expr* cmp = rep.expr(i.getOperand(1));
  const Expr* swp = rep.expr(i.getOperand(2));
  emit(Stmt::assign(res,mem));
  emit(rep.store(i.getOperand(0), Expr::cond(Expr::eq(mem, cmp), swp, mem)));
}

void SmackInstGenerator::visitAtomicRMWInst(llvm::AtomicRMWInst& i) {
  using llvm::AtomicRMWInst;
  processInstruction(i);
  const Expr* res = rep.expr(&i);
  const Expr* mem = rep.load(i.getPointerOperand());
  const Expr* val = rep.expr(i.getValOperand());
  emit(Stmt::assign(res,mem));
  emit(rep.store(i.getPointerOperand(),
    i.getOperation() == AtomicRMWInst::Xchg
      ? val
      : Expr::fn(Naming::ATOMICRMWINST_TABLE.at(i.getOperation()),mem,val) ));
  }

void SmackInstGenerator::visitGetElementPtrInst(llvm::GetElementPtrInst& I) {
  processInstruction(I);
  emit(Stmt::assign(rep.expr(&I), rep.ptrArith(&I)));
}

/******************************************************************************/
/*                 CONVERSION                    OPERATIONS                   */
/******************************************************************************/

void SmackInstGenerator::visitCastInst(llvm::CastInst& I) {
  processInstruction(I);
  emit(Stmt::assign(rep.expr(&I),rep.cast(&I)));
}

/******************************************************************************/
/*                   OTHER                     OPERATIONS                     */
/******************************************************************************/

void SmackInstGenerator::visitCmpInst(llvm::CmpInst& I) {
  processInstruction(I);
  emit(Stmt::assign(rep.expr(&I),rep.cmp(&I)));
}

void SmackInstGenerator::visitPHINode(llvm::PHINode& phi) {
  // NOTE: this is really a No-Op, since assignments to the phi nodes
  // are handled in the translation of branch/switch instructions.
  processInstruction(phi);
}

void SmackInstGenerator::visitSelectInst(llvm::SelectInst& i) {
  processInstruction(i);
  std::string x = naming.get(i);
  const Expr
  *c = rep.expr(i.getOperand(0)),
   *v1 = rep.expr(i.getOperand(1)),
    *v2 = rep.expr(i.getOperand(2));

  emit(Stmt::havoc(x));
  emit(Stmt::assume(Expr::and_(
    Expr::impl(Expr::eq(c,rep.integerLit(1L,1)), Expr::eq(Expr::id(x), v1)),
    Expr::impl(Expr::neq(c,rep.integerLit(1L,1)), Expr::eq(Expr::id(x), v2))
  )));
}

void SmackInstGenerator::visitCallInst(llvm::CallInst& ci) {
  processInstruction(ci);

  Function* f = ci.getCalledFunction();

  if (!f) {
    if (auto CE = dyn_cast<const ConstantExpr>(ci.getCalledValue())) {
      if (CE->isCast()) {
        if (auto CV = dyn_cast<Function>(CE->getOperand(0))) {
          f = CV;
        }
      }
    }
  }

  std::string name = f && f->hasName() ? f->getName() : "";

  if (ci.isInlineAsm()) {
    WARN("unsoundly ignoring inline asm call: " + i2s(ci));
    emit(Stmt::skip());

  } else if (name == "llvm.dbg.value" && SmackOptions::SourceLocSymbols) {
    const llvm::MDNode* m1 = dyn_cast<const llvm::MDNode>(ci.getArgOperand(0));
    const llvm::MDNode* m2 = dyn_cast<const llvm::MDNode>(ci.getArgOperand(2));
    assert(m1 && "Expected metadata node in first argument to llvm.dbg.value.");
    assert(m2 && "Expected metadata node in third argument to llvm.dbg.value.");
    const llvm::MDString* m3 = dyn_cast<const llvm::MDString>(m2->getOperand(2));
    assert(m3 && "Expected metadata string in the third argument to metadata node.");

    if (const llvm::Value* V = m1->getOperand(0)) {
      std::stringstream recordProc;
      recordProc << "boogie_si_record_" << rep.type(V);
      emit(Stmt::call(recordProc.str(), {rep.expr(V)}, {}, {Attr::attr("cexpr", m3->getString().str())}));
    }

  } else if (name.find("llvm.dbg.") != std::string::npos) {
    WARN("ignoring llvm.debug call.");
    emit(Stmt::skip());

  } else if (name.find(Naming::VALUE_PROC) != std::string::npos) {
    emit(rep.valueAnnotation(ci));

  } else if (name.find(Naming::RETURN_VALUE_PROC) != std::string::npos) {
    emit(rep.returnValueAnnotation(ci));

  } else if (name.find(Naming::MOD_PROC) != std::string::npos) {
    proc.getModifies().push_back(rep.code(ci));

  } else if (name.find(Naming::CODE_PROC) != std::string::npos) {
    emit(Stmt::code(rep.code(ci)));

  } else if (name.find(Naming::DECL_PROC) != std::string::npos) {
    std::string code = rep.code(ci);
    proc.getDeclarations().push_back(Decl::code(code, code));

  } else if (name.find(Naming::TOP_DECL_PROC) != std::string::npos) {
    std::string decl = rep.code(ci);
    rep.getProgram().getDeclarations().push_back(Decl::code(decl, decl));
    if (VAR_DECL.match(decl)) {
      std::string var = VAR_DECL.sub("\\1",decl);
      rep.addBplGlobal(var);
    }

  } else if (name.find(Naming::CONTRACT_EXPR) != std::string::npos) {
    // NOTE do not generate code for contract expressions

  } else if (name == Naming::CONTRACT_REQUIRES ||
             name == Naming::CONTRACT_ENSURES ||
             name == Naming::CONTRACT_INVARIANT) {

    CallInst* cj;
    Function* F;
    assert(ci.getNumArgOperands() == 1
        && (cj = dyn_cast<CallInst>(ci.getArgOperand(0)))
        && (F = cj->getCalledFunction())
        && F->getName().find(Naming::CONTRACT_EXPR) != std::string::npos
        && "Expected contract expression argument to contract function.");

    std::list<const Expr*> args;
    for (auto& V : cj->arg_operands())
      args.push_back(rep.expr(V));
    for (auto m : rep.memoryMaps())
      args.push_back(Expr::id(m.first));
    auto E = Expr::fn(F->getName(), args);
    if (name == Naming::CONTRACT_REQUIRES)
      proc.getRequires().push_back(E);
    else if (name == Naming::CONTRACT_ENSURES)
      proc.getEnsures().push_back(E);
    else {
      auto L = loops[ci.getParent()];
      assert(L);
      auto H = L->getHeader();
      assert(H && blockMap.count(H));
      blockMap[H]->getStatements().push_front(
        Stmt::assert_(E, {Attr::attr(Naming::LOOP_INVARIANT_ANNOTATION)}));
    }

  // } else if (name == "result") {
  //   assert(ci.getNumArgOperands() == 0 && "Unexpected operands to result.");
  //   emit(Stmt::assign(rep.expr(&ci),Expr::id(Naming::RET_VAR)));
  //
  // } else if (name == "qvar") {
  //   assert(ci.getNumArgOperands() == 1 && "Unexpected operands to qvar.");
  //   emit(Stmt::assign(rep.expr(&ci),Expr::id(rep.getString(ci.getArgOperand(0)))));
  //
  // } else if (name == "old") {
  //   assert(ci.getNumArgOperands() == 1 && "Unexpected operands to old.");
  //   llvm::LoadInst* LI = llvm::dyn_cast<llvm::LoadInst>(ci.getArgOperand(0));
  //   assert(LI && "Expected value from Load.");
  //   emit(Stmt::assign(rep.expr(&ci),
  //     Expr::fn("old",rep.load(LI->getPointerOperand())) ));

  // } else if (name == "forall") {
  //   assert(ci.getNumArgOperands() == 2 && "Unexpected operands to forall.");
  //   Value* var = ci.getArgOperand(0);
  //   Value* arg = ci.getArgOperand(1);
  //   Slice* S = getSlice(arg);
  //   emit(Stmt::assign(rep.expr(&ci),
  //     Expr::forall(rep.getString(var), "int", S->getBoogieExpression(naming,rep))));
  //
  // } else if (name == "exists") {
  //   assert(ci.getNumArgOperands() == 2 && "Unexpected operands to forall.");
  //   Value* var = ci.getArgOperand(0);
  //   Value* arg = ci.getArgOperand(1);
  //   Slice* S = getSlice(arg);
  //   emit(Stmt::assign(rep.expr(&ci),
  //     Expr::exists(rep.getString(var), "int", S->getBoogieExpression(naming,rep))));
  //
  // } else if (name == "invariant") {
  //   assert(ci.getNumArgOperands() == 1 && "Unexpected operands to invariant.");
  //   Slice* S = getSlice(ci.getArgOperand(0));
  //   emit(Stmt::assert_(S->getBoogieExpression(naming,rep)));

  } else if (f) {
    emit(rep.call(f, ci));

  } else {
    llvm_unreachable("Expected function devirtualization.");
  }

  if (f && f->isDeclaration() && rep.isExternal(&ci)) {
    std::string name = naming.get(*f);
    if (!EXTERNAL_PROC_IGNORE.match(name))
      emit(Stmt::assume(Expr::fn(Naming::EXTERNAL_ADDR,rep.expr(&ci))));
  }
}

void SmackInstGenerator::visitLandingPadInst(llvm::LandingPadInst& lpi) {
  processInstruction(lpi);
  // TODO what exactly!?
  emit(Stmt::assign(rep.expr(&lpi),Expr::id(Naming::EXN_VAL_VAR)));
  if (lpi.isCleanup())
    emit(Stmt::assign(Expr::id(Naming::EXN_VAR), Expr::lit(false)));
  WARN("unsoundly ignoring landingpad clauses...");
}

/******************************************************************************/
/*                  INTRINSIC                    FUNCTIONS                    */
/******************************************************************************/

void SmackInstGenerator::visitMemCpyInst(llvm::MemCpyInst& mci) {
  processInstruction(mci);
  assert (mci.getNumOperands() == 6);
  emit(rep.memcpy(mci));
}

void SmackInstGenerator::visitMemSetInst(llvm::MemSetInst& msi) {
  processInstruction(msi);
  assert (msi.getNumOperands() == 6);
  emit(rep.memset(msi));
}

} // namespace smack
