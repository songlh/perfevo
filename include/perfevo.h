/*
 * File: perfevo.h
 *
 *      This is the header file that is global to the entire project.
 *      It is located here so that everyone will find it.
 */
extern int compute_perfevo (int a);

#ifndef _PERFEVO_H
#define	_PERFEVO_H

namespace llvm {
class Function;
class Instruction;
class Module;
class StructType;
class GlobalVariable;
class AllocaInst;
class CallInst;
class Constant;
class BasicBlock;
class ConstantAggregateZero;
class ConstantInt;
class Pass;
class PointerType;
class Loop;
class LoopInfo;
class CallSite;
}

#include "llvm/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <string>
#include <vector>
#include <map>
#include <list>
#include <utility>

class PerfEvo : public llvm::FunctionPass {
  llvm::raw_ostream &Err;
  llvm::Module *_M;
  std::map<std::string, std::vector<std::string> > source_files;
  std::string intToString(int i);
  bool getPathAndLineNo(llvm::Instruction *i,
                        std::string &Path, unsigned &LineNo);
  void getAllocatedType(llvm::AllocaInst *i, std::string &Type);
  std::vector<std::string> loadSourceFile(std::string s);
  void addSourceLine(llvm::Instruction *i);
  std::string getSourceLine(std::string s, unsigned l);
  void loadSourceFiles(llvm::Module *m);
  std::list<llvm::Instruction *> searchCallSites(llvm::Function &F,
                                                 std::string s);
  llvm::BasicBlock* getLoopHeader(llvm::LoopInfo &li, llvm::Loop *l);
  bool containsCallSite(llvm::Function &F, const llvm::Function *T);
  bool JumpBackToLoop( llvm::LoopInfo & li , llvm::Loop *l , llvm::BasicBlock * pJumpInst );
  std::list<const llvm::CallSite*> getCallSitesForFunction(llvm::Function &F,
                                                     const llvm::Function *T);
  std::list<const llvm::Function*> getFunctionsWithString(llvm::Module &M,
                                                          std::string name);
  std::string getFunctionName( llvm::CallInst * i);
  void (PerfEvo::*pBugHandler)(llvm::Function &F);
  bool bBugHandlerInited;
  void MozillaBug35294(llvm::Function &F);
  void MozillaBug66461(llvm::Function &F);
  void MozillaBug267506(llvm::Function &F);
  void MozillaBug311566(llvm::Function &F);
  void MozillaBug103330(llvm::Function &F);
  void MozillaBug258793(llvm::Function &F);
  void MozillaBug409961(llvm::Function &F);
  void MySQLBug26527(llvm::Function &F);
  void MySQLBug38941(llvm::Function &F);

  void MySQLBug38769(llvm::Function &F);

  void MySQLBug38968(llvm::Function &F);
  void MySQLBug48229(llvm::Function &F);
  void MySQLBug38968();
  void MySQLBug49491(llvm::Function &F);
  void MySQLBug38824(llvm::Function &F);
  void MySQLBug14637(llvm::Function &F);
  void MySQLBug39268(llvm::Function &F);
  void MySQLBug15811(llvm::Function &F);
  void ApacheBug33605(llvm::Function &F);
  void ApacheBug45464(llvm::Function &F);
  void LoopNestedCallSites(llvm::Function &F);
public:
  static char ID;
  PerfEvo();
  bool doInitialization(llvm::Module &M);
  bool runOnFunction(llvm::Function&);
  void getAnalysisUsage(llvm::AnalysisUsage &Info) const;  
};

#endif	/* _PERFEVO_H */
