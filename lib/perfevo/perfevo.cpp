//===-PerfEvo.cpp----------------------------------------------------------===//
//
// Guoliang Jin (aliang@cs.wisc.edu)
// Linhai Song (songlh@cs.wisc.edu)
// Joel Scherpelz (scherpel@cs.wisc.edu)
//
//===----------------------------------------------------------------------===//
//
// This file implements "PerfEvo"
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "perfevo"

#include "perfevo.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instruction.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InstVisitor.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/DataTypes.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Type.h"
#include "llvm/TypeSymbolTable.h"
#include "llvm/Use.h"
#include "llvm/Value.h"
#include "llvm/ValueSymbolTable.h"

#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <set>
using namespace llvm;


////////////////////////////////////////
class TypeFinder {
  // To avoid walking constant expressions multiple times and other IR
  // objects, we keep several helper maps.
  DenseSet<const Value*> VisitedConstants;
  DenseSet<const Type*> VisitedTypes;

  TypePrinting &TP;
  std::vector<const Type*> &NumberedTypes;
public:
  TypeFinder(TypePrinting &tp, std::vector<const Type*> &numberedTypes)
    : TP(tp), NumberedTypes(numberedTypes) {}

  void Run(const Module &M) {
    // Get types from the type symbol table.  This gets opaque types referened
    // only through derived named types.
    const TypeSymbolTable &ST = M.getTypeSymbolTable();
    for (TypeSymbolTable::const_iterator TI = ST.begin(), E = ST.end();
         TI != E; ++TI)
      IncorporateType(TI->second);

    // Get types from global variables.
    for (Module::const_global_iterator I = M.global_begin(),
         E = M.global_end(); I != E; ++I) {
      IncorporateType(I->getType());
      if (I->hasInitializer())
        IncorporateValue(I->getInitializer());
    }

    // Get types from aliases.
    for (Module::const_alias_iterator I = M.alias_begin(),
         E = M.alias_end(); I != E; ++I) {
      IncorporateType(I->getType());
      IncorporateValue(I->getAliasee());
    }

    // Get types from functions.
    for (Module::const_iterator FI = M.begin(), E = M.end(); FI != E; ++FI) {
      IncorporateType(FI->getType());

      for (Function::const_iterator BB = FI->begin(), E = FI->end();
           BB != E;++BB)
        for (BasicBlock::const_iterator II = BB->begin(),
             E = BB->end(); II != E; ++II) {
          const Instruction &I = *II;
          // Incorporate the type of the instruction and all its operands.
          IncorporateType(I.getType());
          for (User::const_op_iterator OI = I.op_begin(), OE = I.op_end();
               OI != OE; ++OI)
            IncorporateValue(*OI);
        }
     }
  }

private:
  void IncorporateType(const Type *Ty) {
    // Check to see if we're already visited this type.
    if (!VisitedTypes.insert(Ty).second)
      return;

    // If this is a structure or opaque type, add a name for the type.
    if (((Ty->isStructTy() && cast<StructType>(Ty)->getNumElements())
          || Ty->isOpaqueTy()) && !TP.hasTypeName(Ty)) {
      TP.addTypeName(Ty, "%"+utostr(unsigned(NumberedTypes.size())));
      NumberedTypes.push_back(Ty);
    }

    // Recursively walk all contained types.
    for (Type::subtype_iterator I = Ty->subtype_begin(),
         E = Ty->subtype_end(); I != E; ++I)
      IncorporateType(*I);
  }

    /// IncorporateValue - This method is used to walk operand lists finding
    /// types hiding in constant expressions and other operands that won't be
    /// walked in other ways.  GlobalValues, basic blocks, instructions, and
    /// inst operands are all explicitly enumerated.
  void IncorporateValue(const Value *V) {
    if (V == 0 || !isa<Constant>(V) || isa<GlobalValue>(V)) return;

    // Already visited?
    if (!VisitedConstants.insert(V).second)
      return;

    // Check this type.
    IncorporateType(V->getType());

    // Look in operands for types.
    const Constant *C = cast<Constant>(V);
    for (Constant::const_op_iterator I = C->op_begin(),
         E = C->op_end(); I != E;++I)
      IncorporateValue(*I);
  }
};


// PrintEscapedString - Print each character of the specified string, escaping
// it if it is not printable or if it is an escape char.
static void PrintEscapedString(StringRef Name, raw_ostream &Out) {
  for (unsigned i = 0, e = Name.size(); i != e; ++i) {
    unsigned char C = Name[i];
    if (isprint(C) && C != '\\' && C != '"')
      Out << C;
    else
      Out << '\\' << hexdigit(C >> 4) << hexdigit(C & 0x0F);
  }
}

enum PrefixType {
  GlobalPrefix,
  LabelPrefix,
  LocalPrefix,
  NoPrefix
};


/// PrintLLVMName - Turn the specified name into an 'LLVM name', which is either
/// prefixed with % (if the string only contains simple characters) or is
/// surrounded with ""'s (if it has special chars in it).  Print it out.
static void PrintLLVMName(raw_ostream &OS, StringRef Name, PrefixType Prefix) {
  assert(Name.data() && "Cannot get empty name!");
  switch (Prefix) {
  default: llvm_unreachable("Bad prefix!");
  case NoPrefix: break;
  case GlobalPrefix: OS << '@'; break;
  case LabelPrefix:  break;
  case LocalPrefix:  OS << '%'; break;
  }

  // Scan the name to see if it needs quotes first.
  bool NeedsQuotes = isdigit(Name[0]);
  if (!NeedsQuotes) {
    for (unsigned i = 0, e = Name.size(); i != e; ++i) {
      char C = Name[i];
      if (!isalnum(C) && C != '-' && C != '.' && C != '_') {
        NeedsQuotes = true;
        break;
      }
    }
  }

  // If we didn't need any quotes, just write out the name in one blast.
  if (!NeedsQuotes) {
    OS << Name;
    return;
  }

  // Okay, we need quotes.  Output the quotes and escape any scary characters as
  // needed.
  OS << '"';
  PrintEscapedString(Name, OS);
  OS << '"';
}


/// AddModuleTypesToPrinter - Add all of the symbolic type names for types in
/// the specified module to the TypePrinter and all numbered types to it and the
/// NumberedTypes table.
static void AddModuleTypesToPrinter(TypePrinting &TP,
                                    std::vector<const Type*> &NumberedTypes,
                                    const Module *M) {
  if (M == 0) return;

  // If the module has a symbol table, take all global types and stuff their
  // names into the TypeNames map.
  const TypeSymbolTable &ST = M->getTypeSymbolTable();
  for (TypeSymbolTable::const_iterator TI = ST.begin(), E = ST.end();
       TI != E; ++TI) {
    const Type *Ty = cast<Type>(TI->second);

    // As a heuristic, don't insert pointer to primitive types, because
    // they are used too often to have a single useful name.
    if (const PointerType *PTy = dyn_cast<PointerType>(Ty)) {
      const Type *PETy = PTy->getElementType();
      if ((PETy->isPrimitiveType() || PETy->isIntegerTy()) &&
          !PETy->isOpaqueTy())
        continue;
    }

    // Likewise don't insert primitives either.
    if (Ty->isIntegerTy() || Ty->isPrimitiveType())
      continue;

    // Get the name as a string and insert it into TypeNames.
    std::string NameStr;
    raw_string_ostream NameROS(NameStr);
    formatted_raw_ostream NameOS(NameROS , false );
    PrintLLVMName(NameOS, TI->first, LocalPrefix);
    NameOS.flush();
    TP.addTypeName(Ty, NameStr);
  }

  // Walk the entire module to find references to unnamed structure and opaque
  // types.  This is required for correctness by opaque types (because multiple
  // uses of an unnamed opaque type needs to be referred to by the same ID) and
  // it shrinks complex recursive structure types substantially in some cases.
  TypeFinder(TP, NumberedTypes).Run(*M);
}

///////////////////////////////




static cl::opt<std::string> strPerfBugID("perfBugID",
       cl::desc("Performance bug ID"), cl::Required,
       cl::value_desc("perfBugID"));

PerfEvo::PerfEvo() : FunctionPass(ID), Err(errs()), bBugHandlerInited(false) {}

std::string PerfEvo::intToString(int i) {
  std::stringstream b;
  b << i;
  return b.str();
}

bool PerfEvo::getPathAndLineNo(Instruction *i,
                               std::string &Path, unsigned &LineNo) {
  LLVMContext &Ctx = i->getContext();
  DebugLoc dl = i->getDebugLoc();
  std::string strPath;
  char *pPath;

  // TODO: should be a while loop
  while (MDNode *IA = dl.getInlinedAt(Ctx)) {
    dl = DebugLoc::getFromDILocation(IA);
  }

  DILocation dil(dl.getAsMDNode(Ctx));
  if (dil.Verify()) {
    strPath = dil.getDirectory().str() + "/" + dil.getFilename().str();
    pPath = canonicalize_file_name(strPath.c_str());
    if (pPath) {
      Path = std::string(pPath);
      LineNo = dil.getLineNumber();
      return true;
    }
  }
  return false;
}

void PerfEvo::getAllocatedType(AllocaInst *i,
                               std::string &Type) {
  std::string DisplayName, File, Directory;
  unsigned LineNo;
  if (!getLocationInfo(i, DisplayName, Type, LineNo, File, Directory))
    assert (false && "fail to get location info!");
}

std::vector<std::string> PerfEvo::loadSourceFile(std::string s) {
  std::vector<std::string> v;
  std::ifstream f(s.c_str());

  while(f.good()) {
    std::string l = "";
    getline(f, l);
    v.push_back(l);
  }

  f.close();

  return v;
}

void PerfEvo::addSourceLine(Instruction *i) {
  std::string s;
  unsigned l;
  if (!getPathAndLineNo(i, s, l))
    return;
  if (source_files[s].empty())
  {
    source_files[s] = loadSourceFile(s);
  }
}

std::string PerfEvo::getSourceLine(std::string s, unsigned l) {
  if (s.empty())
    return "";
  if ((unsigned int)l < source_files[s].size()) 
    return source_files[s][l - 1];
  else
    return "";
}

void PerfEvo::loadSourceFiles(Module *m) {
  for (Module::iterator f = m->begin(), fe = m->end(); f != fe; ++f) {
    for (Function::iterator b = f->begin(), be = f->end(); b != be; ++b) {
      for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
        addSourceLine(i);
      }
    }
  }
}

std::list<Instruction *> PerfEvo::searchCallSites(Function &F, std::string s) {
  std::list<Instruction *> l;
  for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
      if (isa<CallInst>(i) || isa<InvokeInst>(i)) {
        std::string strPath;
        unsigned uLineNo=0;
        assert(getPathAndLineNo(i, strPath, uLineNo) && "No debug info");
        if (getSourceLine(strPath, uLineNo).find(s) != std::string::npos) {
         l.push_back(i);
        }
      }
    }
  }
  return l;
}

BasicBlock* PerfEvo::getLoopHeader(LoopInfo &li, Loop *l) {
  for (Loop::block_iterator b = l->block_begin(), 
                 be = l->block_end(); b != be; ++b) {
    if (li.isLoopHeader(*b)) {
      return *b;
    }
  }
  return NULL;
}

bool PerfEvo::containsCallSite(Function &F, const Function *T) {
  for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
      if (isa<CallInst>(&(*i)) || isa<InvokeInst>(&(*i))) {
        CallSite c(&*i);
        if (c.getCalledFunction() == T) {
         return true;
        }
      }
    }
  }
  return false;
}

//void PerfEvo::ApacheBug33605( Function &F )
//{
//	
//}



void PerfEvo::ApacheBug45464(Function &F) 
{
   int target_flag = 0x0073b170;
   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; ++ b )
   {
       for( BasicBlock::iterator  i = b->begin() , ie = b->end() ; i != ie ; i ++ )
       {
           if( CallInst * pCall = dyn_cast<CallInst>(i) )
	   {
               Function * pFunction = pCall->getCalledFunction();
	       if( !pFunction )
	       {
	           continue;
	       }
	       
	       
	       std::string sFunctionName = pFunction->getName();
	       if( sFunctionName.find( "apr_stat" ) == std::string::npos && sFunctionName.find( "apr_lstat" ) == std::string::npos )
	       {
	           continue;
	       }

	       if( ConstantInt * v = dyn_cast<ConstantInt>( pCall->getArgOperand(2) ) )
	       {
	           if( v->getValue() != target_flag )
		   {
                       continue;
		   }
	       }
	       else
	       {
	           continue;
	       }

	       if( Instruction * pi =  dyn_cast<Instruction>( pCall->getArgOperand(0) ) )
	       {
	           std::string sResult;
		   raw_string_ostream rstring(sResult);
		   Printer.print( pi->getType() , rstring );
		   std::string sAllocatedType = rstring.str();
		   if( sAllocatedType != "%struct.apr_finfo_t*")
		   {
		       continue;
		   }
		   
		   
		   std::set<int> setIndex;
		   //bool bFlag = false;
		   for( Value::use_iterator pu = pi->use_begin() , pue = pi->use_end() ; pu != pue ; pu ++ )
		   {
		       if( GetElementPtrInst * pGet = dyn_cast<GetElementPtrInst>(*pu ))
		       {
		           if( pGet->getNumOperands() != 3 )
			   {
                               continue;
			   }

			   if( ConstantInt * v = dyn_cast<ConstantInt>(pGet->getOperand(2) ) )
			   {
                               //std::cout << v->getValue() << std::endl;
			       setIndex.insert( v->getValue().getLimitedValue() );
			   }
		       }

		  }
                  
		  if ( setIndex.size() <  17 && setIndex.size() > 0 ) 
		  {
		      std::string strPath;
		      unsigned uLineNo=0;
		      assert(getPathAndLineNo(i, strPath, uLineNo) && "No debug info");
		      Err << strPath << ":" << uLineNo << "\n"
		          << getSourceLine(strPath, uLineNo) << "\n";


	           }

	     }
       }
   }

   }


#if 0
  StringRef target_function1 = "apr_stat";
  StringRef target_function2 = "apr_lstat";
  int target_flag = 0x0073b170;
  Function *target_f1 = _M->getFunction(target_function1);
  Function *target_f2 = _M->getFunction(target_function2);
  if (target_f1 == NULL && target_f2 == NULL)
  return;
      
  for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) 
  {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) 
    {
      if (isa<CallInst>(&(*i)) || isa<InvokeInst>(&(*i))) {
        CallSite c(&*i);
        if ((c.getCalledFunction() == target_f1 && target_f1 != NULL)
           || (c.getCalledFunction() == target_f2 && target_f2 != NULL)) {
         if (ConstantInt *v = dyn_cast<ConstantInt>(c.getArgument(2))) 
	 {
           if (v->getValue() == target_flag) {
             std::string strPath;
             unsigned uLineNo;
             assert(getPathAndLineNo(i, strPath, uLineNo) && "No debug info");

             Err << strPath << ":" << uLineNo << "\n"
                 << getSourceLine(strPath, uLineNo) << "\n";
           }
         }
        }
      }
    }
  }

#endif
}

std::list<const CallSite*> PerfEvo::getCallSitesForFunction(Function &F,
                                      const Function *T) {
  std::list<const CallSite *> c_list;

  for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
      if (isa<CallInst>(&(*i)) || isa<InvokeInst>(&(*i))) {
        CallSite *c = new CallSite(&*i);
        if (c->getCalledFunction() == T) {
         c_list.push_back(c);
        }
      }
    }
  }
      
  return c_list;
}

std::list<const Function*> PerfEvo::getFunctionsWithString(Module &M,
                                     std::string name) {
  std::list<const Function *> f_list;

  for (Module::iterator f = M.begin(), fe = M.end(); f != fe; ++f) {
    if (f->getNameStr().find(name) != std::string::npos)
      f_list.push_back(&(*f));
  }

    return f_list;
}

void PerfEvo::MozillaBug267506(Function &F) {


   //if(F.getName().find("NewURIWithDocumentCharset") == std::string::npos)
   //  return;
#if 0
  TypePrinting Printer;
  std::vector<const Type *> NumberedTypes;
  AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

  for(Function::iterator b = F.begin(), be = F.end() ; b != be; ++b) 
  {
    for(BasicBlock::iterator i = b->begin(), ie = b->end() ; i != ie; ++i) 
    {
       if( CallInst * pCall = dyn_cast<CallInst>( i ) )
       {
          Function * pFunction = pCall->getCalledFunction();
	  if( !pFunction )
	  {
             continue;
	  }

	  std::string sFunctionName = pFunction->getName();
          
          if( sFunctionName.find("nsIDocument") == std::string::npos || sFunctionName.find("GetDocumentCharacterSet") == std::string::npos )
          {
             continue;
          }
          
          for( Value::use_iterator u = i->use_begin() , ue = i->use_end() ; u != ue ; u ++ )
          {
              if( CallInst * pCallUse = dyn_cast<CallInst>( *u )  )
              {
                   Function * pFunctionUse = pCallUse->getCalledFunction();
	           if( !pFunction )
	           {
                       continue;
	           } 

	           std::string sFunctionNameUse = pFunctionUse->getName();
                   if( sFunctionNameUse.find("nsCAutoString") == std::string::npos )
                   {
                       continue;
                   }
              
                   if( Instruction * pi =  dyn_cast<Instruction>( pCallUse->getArgOperand(0) ) )
                   { 
	       
                       std::string sResult;
	               raw_string_ostream rstring(sResult);
	               Printer.print( pi->getType() , rstring );
	               std::string sAllocatedType = rstring.str();

                        if( sAllocatedType != "%struct.nsCAutoString*")
                        {
                            continue;
                        }
                        int iNum = 0 ;
                        for( Value::use_iterator pu = pi->use_begin() , pue = pi->use_end() ; pu != pue ; pu ++ )
                        {
			     if( isa<GetElementPtrInst>( *pu ))
			     {
                                 iNum++;
			     }
			
                        }
		    //std::cout << iNum << std::endl;
                       if( iNum == 1 )
                       {
	       
	                   std::string strPath;
		           unsigned uLineNo;
		           assert(getPathAndLineNo(i, strPath, uLineNo) && "No DebugInfo");
		           std::cout << strPath << ":"<< uLineNo << "\n"
		               << getSourceLine(strPath, uLineNo) << "\n";
                       }
                   }
              }
          }
      }
    }
  }

  return ;
#endif

#if 1

   //if(F.getName().find("GetStyleSheetURL") == std::string::npos)
   //   return;
   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

  for( Function::iterator b = F.begin() , be = F.end() ; 
       b != be ; b ++ )
  {
    for( BasicBlock::iterator i = b->begin() , ie = b->end() ; 
        i != ie ; i ++ )
    {
      if( CallInst * pCall = dyn_cast<CallInst>( i )  )
      {
        Function * pFunction = pCall->getCalledFunction();
	if( !pFunction )
	{
         continue;
	}

	std::string sFunctionName = pFunction->getName();
        if( sFunctionName.find("nsIDocument") != std::string::npos &&  
           sFunctionName.find( "GetDocumentCharacterSet" ) != std::string::npos )
	{
         for( Value::use_iterator u = i->use_begin() , ue = i->use_end() ; u != ue ; u ++ )
         {
          if( GetElementPtrInst * pGet =  dyn_cast<GetElementPtrInst>( *u ) )
          {
              for( Value::use_iterator getU = pGet->use_begin() , getUE = pGet->use_end() ; getU != getUE ; getU ++ )
	      {
	         if( CallInst * pCall = dyn_cast<CallInst>(* getU ) )
		 {
	         if( Instruction * pi =  dyn_cast<Instruction>( pCall->getArgOperand(0) ) )
                 { 
	       
                    std::string sResult;
	            raw_string_ostream rstring(sResult);
	            Printer.print( pi->getType() , rstring );
	            std::string sAllocatedType = rstring.str();
               
                    //std::cout << sAllocatedType << std::endl;
                    if( sAllocatedType != "%struct.nsCAutoString*")
                    {
                       continue;
                    }
                    int iNum = 0 ;
                    for( Value::use_iterator pu = pi->use_begin() , pue = pi->use_end() ; pu != pue ; pu ++ )
                    {
			if( isa<GetElementPtrInst>( *pu ))
			{
                            iNum++;
			}
			
                    }
		    //std::cout << iNum << std::endl;
                   if( iNum == 1 )
                   {
	       
	               std::string strPath;
		       unsigned uLineNo=0;
		       //u->dump();
		       assert(getPathAndLineNo(i, strPath, uLineNo) && "No DebugInfo");
		       Err << strPath << ":"<< uLineNo << "\n"
		           << getSourceLine(strPath, uLineNo) << "\n";
                  }
	       }
	       }
	    }
          }
         }
	}
      }
    }
  }
 
  
  return ;
#endif
}

void PerfEvo::MozillaBug66461(Function &F) {
  /*
  //function to retrieve types defined by GTK

  GetGC
  GetDrawable

  //functions from GTK library

  gdk_draw_rectangle
  gdk_draw_rgb_image
  gdk_draw_rgb_image_dithalign
  gdk_gc_copy
  gdk_gc_new
  gdk_gc_new_with_values
  gdk_gc_ref
  gdk_gc_set_clip_mask
  gdk_gc_set_clip_origin
  gdk_gc_set_clip_rectangle
  gdk_gc_unref
  gdk_pixbuf_new
  gdk_pixbuf_new_from_data
  gdk_pixbuf_render_to_drawable
  gdk_pixbuf_scale
  gdk_pixbuf_unref
  gdk_pixmap_new
  gdk_pixmap_unref
  gdk_rgb_get_cmap
  gdk_rgb_get_visual
  gdk_window_copy_area

  */

  //target parameter type
  const Type *T = _M->getTypeByName("struct.nsIDeviceContext");

  //target mutator functions
  const Function *GetGC =
       _M->getFunction("_ZN21nsRenderingContextGTK5GetGCEv");
  const Function *GetDrawable =
      _M->getFunction("_ZN19nsDrawingSurfaceGTK11GetDrawableEv");

  if (T == NULL || GetGC == NULL || GetDrawable == NULL)
    return;
  
  const FunctionType *FT = F.getFunctionType();
  bool found_t = false;

  if (!(containsCallSite(F, GetGC) || containsCallSite(F, GetDrawable)))
    return;

  for (unsigned int j = 0; j < FT->getNumParams(); j++) {
    const Type *t = FT->getParamType(j);
    if (t != NULL) {
      const PointerType *pt = dyn_cast<PointerType>(t);
      if (pt != NULL) {
        if (pt->getElementType() != T) {
         found_t = true;
        }
      }
    }

    if (found_t) {
      //find the first instruction in the current instruction
      // that maps to a real source line
      int lineNumber = -1;
      for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) {
        for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
         const DebugLoc d = i->getDebugLoc();
         lineNumber = d.getLine();
         if (0 < lineNumber)
           break;
        }
        if (0 < lineNumber)
         break;
      }
      
      Err << "Possible skippable function (" << F.getNameStr()
         << ") found at line: " << lineNumber << "\n";
      
      break;
    }
  }
}

void PerfEvo::MozillaBug35294(Function &F) {
  LoopInfo &li = getAnalysis<LoopInfo>();

  std::list<Instruction *> csl = searchCallSites(F, "RemoveChildAt");

  for (std::list<Instruction *>::iterator cs = csl.begin(), cse = csl.end(); 
      cs != cse; ++cs) {
    BasicBlock *bb = (*cs)->getParent();
    unsigned int ld = li.getLoopDepth(bb);
        
    if (ld > 0) {
      if (true) {
        std::string strPath;
        unsigned uLineNo=0;
        assert(getPathAndLineNo(*cs, strPath, uLineNo) && "No DebugInfo");

        Err << strPath << ":" << uLineNo << "\n"
            << getSourceLine(strPath, uLineNo) << "\n"
            << "LoopDepth: " << li.getLoopDepth(bb) << "\n";

      }

      // errs() << *scev << "\n";

    }
  }
}

void PerfEvo::MozillaBug311566(Function &F) {
  LoopInfo &li = getAnalysis<LoopInfo>();

  std::list<Instruction *> csl = searchCallSites(F, "Append(");

  for (std::list<Instruction *>::iterator cs = csl.begin(), cse = csl.end(); 
      cs != cse; ++cs) {
    BasicBlock *bb = (*cs)->getParent();
    unsigned int ld = li.getLoopDepth(bb);

    if (ld > 0) {
      std::string strPath;
      unsigned uLineNo=0;
      assert(getPathAndLineNo(*cs, strPath, uLineNo) && "No DebugInfo");

      Err << strPath << ":" << uLineNo << "\n"
          << getSourceLine(strPath, uLineNo) << "\n"
          << "LoopDepth: " << li.getLoopDepth(bb) << "\n";
    }
  }      
}



std::string PerfEvo::getFunctionName( CallInst * i )
{
     TypePrinting Printer;
     std::vector<const Type *> NumberedTypes;
     AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

    Function * pFunction = i->getCalledFunction();
    std::string sFunctionName;
    if( pFunction )
    {
       sFunctionName = pFunction->getNameStr();
    }
    else
    {
       std::string strPath;
       unsigned uLineNo;
       getPathAndLineNo( i , strPath, uLineNo);
       sFunctionName = getSourceLine(strPath, uLineNo);
    }
    
    return sFunctionName;

}


void PerfEvo::MozillaBug103330(Function &F) 
{
    //if( F.getName().find( "GetStringValue" ) == std::string::npos) //|| F.getName().find( "AtomImpl" ) == std::string::npos )
    //{
    //   return;
    //}


#if 1
   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer, NumberedTypes , _M );
   
   LoopInfo *LI = &getAnalysis<LoopInfo>();

   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; b ++ )
   {
      for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; i ++ )
      {
          if( AllocaInst * pAlloc = dyn_cast<AllocaInst>(i) )
	  {
              std::string sResult;
	      raw_string_ostream rstring( sResult );
	      Printer.print( pAlloc->getType() , rstring );
	      std::string sAllocatedType = rstring.str() ;

	      if( sAllocatedType.find("struct.nsAString") == std::string::npos )
	      {
                  continue;
	      }
              //std::cout << sAllocatedType << std::endl;
	      for( Value::use_iterator su = pAlloc->use_begin() , sue = pAlloc->use_end() ; su != sue ; su ++ )
	      {
                  if( LoadInst * pLoad = dyn_cast<LoadInst>( *su ) )
		  {   
		      if( pLoad->use_begin() == pLoad->use_end() )
		      {
                           continue;
		      }
                      Value::use_iterator useLoad = pLoad->use_begin();
		      if( CallInst * pCall = dyn_cast<CallInst>( * useLoad) )
		      {
                          if( pCall->getNumArgOperands() != 2 )
			  {
                              continue;
			  }

			  if( ConstantInt * pConstant = dyn_cast<ConstantInt>(pCall->getOperand(1) ) )
			  {
                               APInt apInt = pConstant->getValue();
			       std::string sValue = apInt.toString( 10 , 0 );

			       if( sValue != "0")
			       {
			          continue;
			       }
                               
			       std::string sFunctionName = getFunctionName( pCall );
			       if( sFunctionName.find("SetLength") == std::string::npos )
			       {
                                  continue;
			       }

			       //std::string strPath;
			       //unsigned uLineNo;
			       //getPathAndLineNo( pCall , strPath , uLineNo );
			       //std::cout << strPath << " : " << uLineNo << std::endl;
                               //std::cout << "Find setLength" << std::endl;                       
			       //check current blocl
                               bool bFlag = false;
			       BasicBlock * bParent = pCall->getParent();
                               BasicBlock::iterator itInstruction = bParent->begin() ;
			       while( true )
			       {
                                   if(pCall->isIdenticalTo( itInstruction ))
				   {
				      break;
				   }

				   itInstruction++;
			       }

			       itInstruction++; 
			       //std::cout << "before inside block " << std::endl;
		               for( BasicBlock::iterator itInstructionEnd = bParent->end() ; itInstruction != itInstructionEnd ; itInstruction ++ )
			       {
                                   for( Value::use_iterator suInstruction = pAlloc->use_begin() , sueInstruction = pAlloc->use_end() ; suInstruction != sueInstruction ; suInstruction ++ )
				   {
				       if( Instruction * pInstruction = dyn_cast<Instruction>( *suInstruction) )
				       {
                                          if( itInstruction->isIdenticalTo(  pInstruction) )
				          {

				            if( LoadInst * pNextLoad = dyn_cast<LoadInst>( itInstruction ) )
					    {
					        if( pNextLoad->use_begin() != pNextLoad->use_end() )
						{

					
					        
                                                Value::use_iterator nextCall = pNextLoad->use_begin();
						if( CallInst * pNextCall = dyn_cast<CallInst>( *nextCall ) )
						{
                                                     std::string sAppend = getFunctionName( pNextCall );

						     if( sAppend.find("Append") != std::string::npos )
						     {
                                                         std::string strPath;
							 unsigned uLineNo;
							 getPathAndLineNo(pCall, strPath, uLineNo);
							 std::cout << strPath << " : " << uLineNo << std::endl;
							 std::cout << "\t" << getSourceLine( strPath, uLineNo)  << std::endl;
							 getPathAndLineNo( pNextCall , strPath , uLineNo );
							 std::cout << strPath << " : " << uLineNo << std::endl;
							 std::cout << "\t" << getSourceLine(strPath ,uLineNo ) << std::endl;
							 std::cout << "=============================" << std::endl;
						     }
						}
						}
					    }

                                            bFlag = true;
					    break;
				         }
				       }
				   }

				   if(bFlag)
				   {
                                      break;
				   }
			       }

                               //std::cout << bParent->getNameStr() << std::endl;                              
			       if( bFlag )
			       {
                                   continue;
                               }
			       //std::cout << "before next block" << std::endl;
                               
			       std::vector< std::vector<succ_iterator > > vectorIt;
                               std::vector<std::string> vecVisit;
                               
			       std::vector< succ_iterator > vecTmp;
			       vecTmp.push_back( succ_begin(bParent) );
			       vecTmp.push_back( succ_end( bParent ) );
                               vectorIt.push_back( vecTmp );			       
                               vecVisit.push_back( bParent->getNameStr() );
                               //std::cout << bParent->getNameStr() << std::endl;
			       while( vectorIt.size() > 0 )
			       {
			           bool bInnerFlag = false; 
                                   if( vectorIt[vectorIt.size()-1][0] == vectorIt[vectorIt.size()-1][1])
				   {
                                       vecVisit.pop_back();
                                       vectorIt.pop_back();
				       continue;
				   }

				  succ_iterator itBasicBlock = vectorIt[vectorIt.size() - 1][0];
                                  vectorIt[vectorIt.size() -1][0]++;
                                  
				  if( LI->getLoopDepth( *itBasicBlock ) > 0 )
				  {
                                      continue;
				  }
				  
				  std::vector< std::string >::iterator itBegin = vecVisit.begin();
				  std::vector< std::string >::iterator itEnd = vecVisit.end();
				  std::string sName = itBasicBlock->getNameStr();
                                  //std::cout << sName << std::endl;
				  while( itBegin != itEnd )
				  {
                                      if( (*itBegin) == sName )
				      {
                                          bInnerFlag = true;
					  break;
				      }
				      itBegin ++;
				  } 

                                  if( bInnerFlag )
				  { 
                                      continue;
				  }

			          for( BasicBlock::iterator itInstruction = itBasicBlock->begin() , itInstructionEnd = itBasicBlock->end() ; itInstruction != itInstructionEnd ; itInstruction++ )
				  {
                                       for( Value::use_iterator suInstruction = pAlloc->use_begin() , sueInstruction = pAlloc->use_end() ; suInstruction != sueInstruction ; suInstruction ++ )
				       {
				           if( Instruction * pInstruction = dyn_cast<Instruction>( *suInstruction ) )
					   {
                                               if( pInstruction->isIdenticalTo( itInstruction )  )
					       {
                                                   if( LoadInst * pNextLoad = dyn_cast<LoadInst>( itInstruction ) )
                                                   {
						       if( pNextLoad->use_begin() != pNextLoad->use_end() )
						       {
                                                           Value::use_iterator nextCall = pNextLoad->use_begin();
                                                           if( CallInst * pNextCall = dyn_cast<CallInst>( *nextCall ) )
						           {
						                std::string sAppend = getFunctionName( pNextCall );
							        if( sAppend.find("Append") != std::string::npos )
							        {
							           std::string strPath;
								   unsigned uLineNo;
								   getPathAndLineNo(pCall, strPath, uLineNo);
								   std::cout << strPath << " : " << uLineNo << std::endl;
								   std::cout << "\t" << getSourceLine( strPath, uLineNo)  
								           << std::endl;getPathAndLineNo( pNextCall , strPath , uLineNo );
							           std::cout << strPath << " : " << uLineNo << std::endl;
								   std::cout << "\t" << getSourceLine(strPath ,uLineNo ) << std::endl;
								   std::cout << "=============================" << std::endl;
						                }
						           }
							}
						   }
                                                   //pInstruction->dump();
                                                   bInnerFlag = true;
						   break;
					       }
					   }

				       }

				       if( bInnerFlag )
				       {
                                           break;
				       }
									    
				   }


				   
				   if( bInnerFlag )
				   {
				       
				   }
				   else
				   {
				       std::vector<succ_iterator> vecTmp;
				       vecTmp.push_back( succ_begin(*itBasicBlock) );
				       vecTmp.push_back( succ_end( *itBasicBlock) );
				       vectorIt.push_back( vecTmp );
                                       vecVisit.push_back( itBasicBlock->getNameStr());

				   }
                               }

			  }
		      }
		      else
		      {
		          continue;
		      }
		  }
	      }
	  }
      }

   }

#endif

#if 0
   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer , NumberedTypes , _M );


   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; b ++ )
   {
       for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; i++ )
       {
           if( CallInst * pCall = dyn_cast<CallInst> (i) )
           {
               Function * pFunction = pCall->getCalledFunction();
	       if( !pFunction )
	       {
                   continue;
	       }

               std::string sFunctionName = pFunction->getName();
               if( sFunctionName.find("nsACString") != std::string::npos &&  
                   sFunctionName.find( "Assign" ) != std::string::npos )
               {
                   
	           if( Value * pArgument = dyn_cast<Value>(pCall->getOperand(0)) )
	           {
	                std::string sResult;
		        raw_string_ostream rstring( sResult );
		        Printer.print( pArgument->getType() , rstring );
		        std::string sOperandOne = rstring.str();
		        if( sOperandOne != "%struct.nsACString*" )
		        {
		            continue;
		        }

		        if( pCall->getNumArgOperands() == 2 )
		        {
                            std::string strPath;
		            unsigned uLineNo;
		            getPathAndLineNo( pCall , strPath , uLineNo );
		            std::cout << strPath << " : " << uLineNo << std::endl;
                            std::cout << "\t" << getSourceLine( strPath , uLineNo ) << std::endl;
		        }
	           }
               }
           }
       }
   }

#endif
#if 0


   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

   std::vector<AllocaInst *> vecAllocnsAString;

   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; b ++ )
   {
       for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; i++ )
       {
           if( AllocaInst * pAlloc = dyn_cast<AllocaInst>( i ))
	   {
	       //pAlloc->dump();
               std::string sResult;
	       raw_string_ostream rstring(sResult);
	       Printer.print( pAlloc->getType() , rstring );
	       std::string sAllocatedType = rstring.str();
               //std::cout << sAllocatedType << std::endl;
	       if( sAllocatedType.find( "struct.nsAString" ) != std::string::npos )
	       {
                   vecAllocnsAString.push_back( pAlloc );
	       }
	   }
       }
   }
   
   std::vector<AllocaInst *>::iterator itBegin = vecAllocnsAString.begin();
   std::vector<AllocaInst *>::iterator itEnd = vecAllocnsAString.end();
   
 
   while( itBegin != itEnd )
   {
      //(*itBegin)->dump();
      std::vector<CallInst *> vecCallInstruction;
      
      for( Function::iterator b = F.begin() , be = F.end() ; b != be ; b ++ )
      {
          for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; i ++ )
	  {
             if( LoadInst * pLoad = dyn_cast<LoadInst>( i ))
	     {
		 if( AllocaInst * pAlloca = dyn_cast<AllocaInst>( pLoad->getOperand( 0) ) )
		 {
                    if(pAlloca->isIdenticalTo( (*itBegin ) ))
		    {
                        for( Value::use_iterator su = pLoad->use_begin() , sue = pLoad->use_end() ; su != sue ; ++su  )
			{
                            if( CallInst * pCall = dyn_cast<CallInst>( * su ) )
			    {
                               vecCallInstruction.push_back( pCall );
			    }
			}
		    }
		 }
	     }
	  }
      }

      std::vector<CallInst *>::iterator itCallBegin = vecCallInstruction.begin();
      std::vector<CallInst *>::iterator itCallEnd = vecCallInstruction.end();
      
      while( itCallBegin != itCallEnd )
      {
         if( (*itCallBegin)->getNumArgOperands() == 2 )
	 {
            if( LoadInst* pLoad = dyn_cast<LoadInst>((*itCallBegin)->getOperand(0)))
            {
	        if( ConstantInt* pConstant = dyn_cast<ConstantInt>((*itCallBegin)->getOperand(1)) )
	        {
	           std::string sResult;
		   raw_string_ostream rstring( sResult );
		   Printer.print( pLoad->getType() , rstring );
		   std::string sOperandOne = rstring.str();

		   APInt apInt = pConstant->getValue();
		   std::string sValue = apInt.toString( 10 , 0 );
                   
                   if( sOperandOne.find("struct.nsAString") != std::string::npos 
		       && sValue == "0")
		   {
		       std::string sFunctionName = getFunctionName( (*itCallBegin) );
                       //std::cout << sFunctionName << std::endl;

		       if( sFunctionName.find("SetLength") != std::string::npos )
		       {
			   std::vector<CallInst *>::iterator itTmp = itCallBegin;
			   itTmp ++;
                           if( itTmp != itCallEnd )
			   {
                              if( (*itTmp)->getNumArgOperands() > 1 )
			      {
                                  if( LoadInst * pLoad = dyn_cast<LoadInst>( (*itTmp)->getOperand(0)) )
				  {
                                     std::string sResult;
				     raw_string_ostream rstring(sResult);
				     Printer.print( pLoad->getType() , rstring );
				     std::string sTypeName = rstring.str();
                                     //std::cout << sTypeName << std::endl;
				     if( sTypeName.find("struct.nsAString") != std::string::npos  )
				     {
				         std::string sAppend = getFunctionName( *itTmp);
				         
				          if( sAppend.find("Append") != std::string::npos )
				          {
                                              std::string strPath;
					      unsigned uLineNo;
					      getPathAndLineNo(*itCallBegin, strPath, uLineNo);
					      std::cout << strPath << " : " << uLineNo << std::endl;
					      std::cout << "\t" << getSourceLine( strPath, uLineNo)  << std::endl;
                                              getPathAndLineNo( *itTmp , strPath , uLineNo );
					      std::cout << strPath << " : " << uLineNo << std::endl;
					      std::cout << "\t" << getSourceLine(strPath ,uLineNo ) << std::endl;
					      std::cout << "=============================" << std::endl; 
				           }
				      }
				   }
			       }
			}
                        
		       }
		   }

	        }
            }
         }

         itCallBegin++;
      }

      itBegin ++;
   }


   //std::cout << vecAllocnsAString.size() << std::endl;
   return;
#endif
}
void PerfEvo::MozillaBug258793(Function &F) {
}

void PerfEvo::MozillaBug409961(Function &F) {
  LoopInfo &LI = getAnalysis<LoopInfo>();
  unsigned min = 999999999;
  unsigned max = 0;
  bool bNeedSrcDump = false;
  std::string strPath;
  unsigned uLineNo;

  for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
      bool ret = getPathAndLineNo(i, strPath, uLineNo);
      if (ret) {
        if (uLineNo > max)
          max = uLineNo;
        if (uLineNo < min)
          min = uLineNo;
      }
      if (LI.getLoopDepth(&*b) == 0 && !LI.isLoopHeader(&*b))
        continue;

      if (CallInst* callInst = dyn_cast<CallInst>(&*i)) {
        if (! callInst->getCalledFunction()
            || callInst->getCalledFunction()->getNameStr() !=
               "_ZN13nsCOMPtr_base25assign_from_qi_with_error\
ERK25nsQueryInterfaceWithErrorRK4nsID"
         ) {
         continue;
        }
      }
#if 0
      else if (InvokeInst* invokeInst = dyn_cast<InvokeInst>(&*i)) {
        if (invokeInst->getCalledFunction()->getNameStr() !=
           "_ZN13nsCOMPtr_base25assign_from_qi_with_error\
ERK25nsQueryInterfaceWithErrorRK4nsID"
         ) {
         continue;
        }
      }
#endif
      else
        continue;
      i->dump();
      assert(ret && "No DebugInfo");
      bNeedSrcDump = true;
      Err << strPath << ":" << uLineNo << "\n"
          << getSourceLine(strPath, uLineNo) << "\n"
         << "LoopDepth: " << LI.getLoopDepth(&*b) << "\n"
         << "isLoopHeader: " << LI.isLoopHeader(&*b) << "\n\n";
    }
  }
  max += 5;
  if (bNeedSrcDump) {
    for (uLineNo = min -5; uLineNo < max; ++uLineNo)
      Err << getSourceLine(strPath, uLineNo) << "\n";
  }
}

void PerfEvo::MySQLBug26527(Function &F) {
}
void PerfEvo::MySQLBug38941(Function &F) {
}
void PerfEvo::MySQLBug38968() 
{
  //std::cout << "In 38968" << std::endl;
  TypePrinting Printer;
  std::vector<const Type *> NumberedTypes;
  AddModuleTypesToPrinter( Printer , NumberedTypes, _M);
  //std::set<std::string> setAllFunction;
  std::set<std::string> setInit_Destroy;
  setInit_Destroy.insert( "mutex_create_func" );
  setInit_Destroy.insert( "mutex_free" );
  setInit_Destroy.insert( "os_fast_mutex_init" );
  setInit_Destroy.insert( "os_fast_mutex_free" );
  setInit_Destroy.insert( "pthread_mutex_init");
  setInit_Destroy.insert( "pthread_mutex_destroy" );
  std::set<std::string> setAllFunction;
  for (Module::global_iterator v = _M->global_begin(), ve = _M->global_end();
          v !=  ve; ++v) {
        std::string sResult;
        raw_string_ostream rstring(sResult);
        Printer.print( v->getType() , rstring );       
        //std::set<std::string> setAllFunction;
        std::string sAllocatedType = rstring.str();
        if( sAllocatedType.find("pthread_mutex_t") != std::string::npos) //== "%union.os_fast_mutex_t*"  )
        {  
	   std::cout << sAllocatedType  << std::endl;
           //continue;
	   std::set< std::string > setFunctionUsed; 
	   //if( v->getNameStr() != "srv_innodb_monitor_mutex" )
	   //{
	   //   continue;
	   //}

           //for( Value::use_iterator u = v->use_begin(),  ue = v->use_end() ; u != ue ; u ++ )
	   //{
           //    u->dump();
	   //}

           for( Value::use_iterator u = v->use_begin() , ue = v->use_end() ; u != ue ; u ++ )
           {
               if( CallInst * pCall =  dyn_cast<CallInst>(*u) )
	       {
                   Function * pFunction = pCall->getCalledFunction();
	           if(!pFunction)
	           {
                      continue;
	           }

	           std::string fname = pFunction->getName();
		   if( setInit_Destroy.find( fname ) == setInit_Destroy.end() )
		   {
                       setFunctionUsed.insert( fname );
		   }
                   //setFunctionUsed.insert( fname );
		   setAllFunction.insert( fname );
               }
	       else
	       {
	          //std::cout << "else" << std::endl;
	          // u->dump();
                  
		  for( Value::use_iterator uP = u->use_begin() , ueP = u->use_end() ; uP != ueP ; uP ++ )
		  {
                      if( CallInst * pCall = dyn_cast<CallInst>( *uP ) )
		      {
                          Function * pFunction = pCall->getCalledFunction();
			  if( !pFunction )
			  {
                              continue;
			  }
			  std::string fname = pFunction->getName();
			  if( setInit_Destroy.find( fname ) == setInit_Destroy.end() )
			  {
                              setFunctionUsed.insert( fname );
			  }
			  //setFunctionUsed.insert( fname );
			  setAllFunction.insert( fname );
		      }
		  }
	       }
               //std::cout << "=============================" << std::endl;
           }
         
	   std::string sMutexName = v->getNameStr() ;

           if( setFunctionUsed.size() == 0 )
	   {
	       std::cout << "==============================" << std::endl;
               std::cout << "* bugs:  " << sMutexName << std::endl;
	       std::cout << "==============================" << std::endl;
	   }
           else
	   {
	       std::cout << "==============================" << std::endl;
	       std::cout << "* good practice: " << sMutexName << std::endl;
	       std::cout << "==============================" << std::endl;
           }


	   //std::set<std::string>::iterator itBegin = setFunctionUsed.begin();
	   //while( itBegin != setFunctionUsed.end() )
	   //{
           //    std::cout << ( * itBegin ) << std::endl;
	   //    itBegin++;
	   //}

	   //std::cout << "==================================" << std::endl;

	   /*std::string sMutexName = v->getNameStr() ;
	   if( setFunctionUsed.find( "pthread_mutex_init" ) == setFunctionUsed.end() 
	       && setFunctionUsed.find( "pthread_mutex_destroy" ) == setFunctionUsed.end()
	      )
	   {
              std::cout << sMutexName << " : situation 1"  << std::endl;
              std::cout << sMutexName << " : " << setFunctionUsed.size() << std::endl;
	      std::set<std::string>::iterator itBegin = setFunctionUsed.begin();
	      while( itBegin != setFunctionUsed.end() )
	      {
                   std::cout << (*itBegin) << std::endl;
		   itBegin++;
	      }
	      std::cout << "===============================" << std::endl; 
	   }
         
	   if( setFunctionUsed.find("pthread_mutex_init") != setFunctionUsed.end()
	       && setFunctionUsed.find("pthread_mutex_destroy") != setFunctionUsed.end() 
	       && setFunctionUsed.find("pthread_mutex_lock") == setFunctionUsed.end() 
	       && setFunctionUsed.find( "pthread_mutex_unlock" ) == setFunctionUsed.end() )
	   {
               std::cout << sMutexName << " : situation 2" << std::endl;
	       std::cout << sMutexName << " : " << setFunctionUsed.size() << std::endl;
	       std::set<std::string>::iterator itBegin = setFunctionUsed.begin() ;
	       while( itBegin != setFunctionUsed.end() ) 
	       {
                   std::cout << (*itBegin) << std::endl;
		   itBegin ++;
	       }
	       std::cout << "================================" << std::endl;
	   }*/


           //std::string sMutexName = v->getNameStr();
          /* if(    setFunctionUsed.find( std::string("pthread_mutex_init") ) != setFunctionUsed.end()   
               && setFunctionUsed.find( std::string("pthread_mutex_destroy") ) != setFunctionUsed.end() 
               && setFunctionUsed.size() == 2 )
           {
               std::cout << v->getNameStr() << std::endl;
           }*/
        }
   }

   //std::set<std::string>::iterator itBegin = setAllFunction.begin();
   //while( itBegin != setAllFunction.end()  )
   //{
   //   std::cout << (*itBegin) << std::endl;
   //   itBegin ++ ;
   // }
  
    
    return;
}

void PerfEvo::MySQLBug49491(Function &F)
{
  std::string sFunctionName = "sprintf";
  std::string sPatternOne = "%02X";
  std::string sPatternTwo = "%02x";
  for( Function::iterator b = F.begin(), be = F.end() ; b != be ; b ++ )
  {
    for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; i ++ )
    {
      if( CallInst * pCall = dyn_cast<CallInst>(i))
      {
        Function * pFunction = pCall->getCalledFunction();
	if(!pFunction)
	{
         continue;
	}

	std::string fname = pFunction->getName();
	if( fname == sFunctionName )
	{
         //Err << getPathFromInstruction(i) << ":" << getLineFromInstruction(i) << "\n"
         //    << getSourceLine(i) << "\n";
         if( ConstantExpr * pCE = dyn_cast<ConstantExpr>( pCall->getArgOperand(1)) )
         {
           if( GlobalVariable * pGV = dyn_cast<GlobalVariable>( pCE->getOperand(0) ))
           {
           if( pGV->hasInitializer() )
           {
             if( ConstantArray * pCA = dyn_cast<ConstantArray>( pGV->getInitializer() ))
		{
              std::string sSecondParameter = pCA->getAsString();
             if( sSecondParameter.length() == 0 || (sSecondParameter.length() - 1) % 4 != 0 )
             {
                continue;
             }
             while( sSecondParameter.length() > 1 )
             {
                 if( sSecondParameter.substr( 0 , 4) != sPatternOne && 
              sSecondParameter.substr( 0 , 4) != sPatternTwo )
              {
                 break;
              }
              sSecondParameter = sSecondParameter.substr( 4 , sSecondParameter.length() - 4 );
             }

             if( sSecondParameter.length() == 1 )
             {
               std::string strPath;
               unsigned uLineNo=0;
               assert(getPathAndLineNo(i, strPath, uLineNo) && "No DebugInfo");

               Err << strPath << ":" << uLineNo << "\n"
                   << getSourceLine(strPath, uLineNo) << "\n";
             }

		}
           }
           }
         }
	}
      }
    }
  }
}



void PerfEvo::MySQLBug38769(Function &F)
{
    //if( F.getName().find( "restore_table_data" ) == std::string::npos )
    //{
    //    return;
    //}
     
    //std::cout << F.getNameStr() << std::string::npos;

    TypePrinting Printer;
    std::vector<const Type *> NumberedTypes;
    AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

    LoopInfo *LI = &getAnalysis<LoopInfo>();

    for( Function::iterator b = F.begin(), be = F.end() ; b != be; ++ b )
    {
        for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; ++ i)
	{
            if( GetElementPtrInst * pGet = dyn_cast<GetElementPtrInst>(i))
	    {
	       
                 std::string sResult;
                 raw_string_ostream rstring(sResult);
                 Printer.print( pGet->getOperand(0)->getType() , rstring );
                 std::string sGetType = rstring.str();
                     
	         if( sGetType.find( "_info" ) != std::string::npos && sGetType.find("struct") != std::string::npos  )
	         {
                      //std::cout << sGetType << std::endl;   
		      if(pGet->getNumOperands() != 5 )
		      {
                          continue;
		      }

		      if( ConstantInt * pConstant = dyn_cast<ConstantInt>( pGet->getOperand(1) ) )
		      {
                          if( !pConstant->equalsInt(0) )
			  {
                              continue;
			  }
		      }
		      else
		      {
		          continue;
		      }

		      if( ConstantInt * pConstant = dyn_cast<ConstantInt>( pGet->getOperand(2) ) )
		      {
                          if( !pConstant->equalsInt(0) )
			  {
                             continue;
			  }
		      }
		      else
		      {
		          continue;
		      }


                      if( ConstantInt * pConstant = dyn_cast<ConstantInt>( pGet->getOperand(3) ) )
		      {
                          if( !pConstant->equalsInt(3) )
			  {
                             continue;
			  }
		      }
		      else
		      {
		          continue;
		      }

		      if( Instruction * pInst = dyn_cast<Instruction>( pGet->getOperand(4) ) )
		      {
                         if( !pInst->getType()->isIntegerTy() )
			 {
                             continue;
			 }
		      }
		      else
		      {
                          continue;
		      }

		      //pGet->dump();
		      
		      if( LI->getLoopDepth( i->getParent() ) > 0 )
		      {
                          Loop * pLoop = LI->getLoopFor( i->getParent() );
			  BasicBlock * pBlock = getLoopHeader( *LI , pLoop );
                          //pBlock->dump();
			  for( BasicBlock::iterator iloop = pBlock->begin(), ieloop = pBlock->end() ; iloop != ieloop ; iloop ++  )
			  {
                              if( BranchInst * pIndirect = dyn_cast<BranchInst>( iloop ) )
			      {
                                 //pIndirect->dump();
				 //std::cout << pIndirect->isConditional() << std::endl;
				 //pIndirect->getCondition()->dump();
				 if( pIndirect->isConditional() )
				 {
                                     if( ICmpInst * pICmp = dyn_cast<ICmpInst>( pIndirect->getCondition() ) )
				     {
				         //pICmp->dump();
                                         if( isa<ConstantInt>( pICmp->getOperand(0) ) && isa<Instruction>( pICmp->getOperand(1)) )
					 {
                                             std::string strPath;
					     unsigned uLineNo;
			                     getPathAndLineNo( i , strPath , uLineNo );
                                             if( strPath == "" )
					     {
					         getPathAndLineNo( iloop , strPath , uLineNo );
						 std::cout << strPath << " : " << uLineNo << std::endl;
						 std::cout << "\t" << getSourceLine( strPath , uLineNo ) << std::endl;
					         //pBlock->dump();
                                                 std::cout << F.getNameStr() << std::endl;
						 i->dump();
					     }
					     else
					     {
					         std::cout << strPath << ":" << uLineNo << std::endl;
						 std::cout << "\t"    << getSourceLine(strPath, uLineNo) << std::endl;
					     }

					     std::cout << "====================" << std::endl;
					 }
					 else if( isa<Instruction>( pICmp->getOperand(0)) && isa<ConstantInt>( pICmp->getOperand(1)) )
					 {
                                              std::string strPath;
					      unsigned uLineNo;
					      getPathAndLineNo( i , strPath , uLineNo );
					      if( strPath == "" )
					      {	
					           getPathAndLineNo( iloop , strPath , uLineNo );
						   std::cout << strPath << " : " << uLineNo << std::endl;
						   std::cout << "\t" << getSourceLine( strPath , uLineNo )  << std::endl;
					           //pBlock->dump();
					           std::cout << F.getNameStr() << std::endl;
						   i->dump();
					      }
					      else
					      {
					           std::cout << strPath << " : " << uLineNo << std::endl;
						   std::cout << "\t" << getSourceLine(strPath, uLineNo) << std::endl;
				              }

					      std::cout << "====================" << std::endl;

					 }


				     }
				 }
			      }
			  }
			  //pBlock->dump();
		      }
	         }
                     
                 //std::string strPath;
                 //unsigned uLineNo;
                 //getPathAndLineNo( i, strPath, uLineNo);
                 //std::cout << strPath << ":" << uLineNo << std::endl;
                 //std::cout << "\t"    << getSourceLine(strPath, uLineNo) << std::endl;

	    }
       }
   }

  
}


void PerfEvo::MySQLBug38824(Function &F) {
}

bool PerfEvo::JumpBackToLoop( LoopInfo &li , Loop  *l , BasicBlock * pJumpInst )
{
   for( Loop::iterator b = l->begin() , be = l->end() ;b != be ; ++ b  )
   {
       //if( b == pJumpInst )
       //{
       //   return true;
       //}
   }

   return false;
}



void PerfEvo::MySQLBug14637(Function &F) 
{
   //if( F.getName().find("my_strnncollsp_latin1_de") == std::string::npos )
   //{
   //    return;
   //}

   const LoopInfo *LI = &getAnalysis<LoopInfo>();

   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; ++ b )
   {
       for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; ++ i )
       {
          if( BranchInst * pBranchInst = dyn_cast<BranchInst>(i) )
	  {
               if( pBranchInst->isConditional() && pBranchInst->getNumSuccessors() == 2 && LI->getLoopDepth( pBranchInst->getParent() ) > 0  )
	       {
                   if( ICmpInst * pICmp = dyn_cast<ICmpInst>( pBranchInst->getCondition()))
		   {
                       if( pICmp->isEquality() )
		       {
		          //pBranchInst->dump();
		          Loop * pLoop = LI->getLoopFor( pBranchInst->getParent() );
			  BasicBlock * pBlock = pLoop->getHeader();
                          //pBlock->dump();
			  unsigned uHeadLineNum = 0;

			  for( BasicBlock::iterator iHeader = pBlock->begin() , ieHeader = pBlock->end() ; iHeader != ieHeader ; iHeader ++ )
			  {
                              if( BranchInst * pHeadBranchInst = dyn_cast<BranchInst>(iHeader) )
			      {
                                  std::string sHeadFile;
				  getPathAndLineNo( pHeadBranchInst , sHeadFile, uHeadLineNum);
				  break;
			      }
			  }

			  std::string sTmp;
			  unsigned uTmp = 0;
			  getPathAndLineNo( pBranchInst , sTmp , uTmp );
			

			  if( !( uHeadLineNum == uTmp  && uHeadLineNum != 0) )
			  {
                              continue;
			  }
			 

			  BasicBlock * pBasicBlockOne = pBranchInst->getSuccessor(0);
                          BasicBlock * pBasicBlockTwo = pBranchInst->getSuccessor(1);
                          if( !(pLoop->contains(pBasicBlockOne)&& !pLoop->contains(pBasicBlockTwo)) )
			  {
                             continue;
			  }
                          
			  Value * arrayPtr;
			  if( ConstantInt * pConstantInt = dyn_cast<ConstantInt>( pICmp->getOperand(0)) )
			  {
			      //Value * arrayPtr;

                              if( !isa<ConstantInt>( pICmp->getOperand(1)) )
			      {
                                  if( pConstantInt->getType()->isIntegerTy(8) )
				  {
                                      arrayPtr = pICmp->getOperand(1);
				  }
				  else
				  {
                                      continue;
				  }
			      }
			      else
			      {
			          continue;
			      }
			  }
			  else if( ConstantInt * pConstantInt = dyn_cast<ConstantInt>( pICmp->getOperand(1)) )
			  {
                               if( !isa<ConstantInt>(pICmp->getOperand(0) ) )
			       {
                                   if( pConstantInt->getType()->isIntegerTy(8) )
				   {
				       arrayPtr = pICmp->getOperand(0);
				   }
				   else
				   {
				      continue;
				   }
			       }
			       else
			       {
			           continue;
			       }
			  }
			  else
			  {
                               continue;
			  }


			  if( LoadInst * pLoad = dyn_cast<LoadInst>( arrayPtr ))
			  {
                              if( LI->getLoopDepth(pLoad->getParent()) > 0 )
			      {
                                  if( GetElementPtrInst * pGet = dyn_cast<GetElementPtrInst>(pLoad->getOperand(0)))
				  {
                                      if( LI->getLoopDepth( pGet->getParent() ) > 0  )
				      {
                                          if( pGet->getNumOperands() == 2 )
					  {
                                               if( pGet->getOperand(0)->getType()->isPointerTy() && pGet->getOperand(1)->getType()->isIntegerTy() )
					       {
                                             
                                                   //Loop * pLoop = LI->getLoopFor( pBranchInst->getParent() );
                                                   //int iNum = 0 ;
                                                   //for( Loop::iterator itBegin = pLoop->begin() , itEnd = pLoop->end(); itBegin != itEnd ; itBegin ++)
                                                   //{
                                                   //    iNum ++;
                                                   //}    
                                                   //if( iNum > 0 )
                                                   //{
                                                   //    continue;
                                                   //}
                                                   std::string strPath;
						   unsigned uLineNo;
                                                   //i->dump();
						   getPathAndLineNo( pICmp, strPath, uLineNo);
                                                   //std::cout << "Block Num:" << iNum  << std::endl; 
						   std::cout << strPath << ":" << uLineNo << std::endl;
						   std::cout << "\t"<< getSourceLine(strPath, uLineNo) << std::endl;
					       }
					  }
				      }
				  }
			      }
			  }
 
		       }
		   }
	       }
	  }
	 
         
       }
   }
}

void PerfEvo::MySQLBug39268(Function &F)
{
   //if( F.getName().find( "ndbcluster_log_schema_op" ) == std::string::npos )//|| F.getName().find("opTupleIdOnNdb") == std::string::npos )
   //{
   //    return;
   //}
 

 #if 0
   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; ++ b )
   {
       for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; ++ i )
       {
          if( CallInst * pCall = dyn_cast<CallInst>( i ) )
	  {
              Function * pFunction = pCall->getCalledFunction();
	      if( !pFunction )
	      {
                  continue;
	      }
	      std::string sFunction = pFunction->getName();
	      if( sFunction.find( "startTransaction" ) == std::string::npos )
	      {
                  continue;
	      }

	      if( Value * pArgument = dyn_cast<Value>(pCall->getOperand(0)) )
	      {
	          std::string sResult;
		  raw_string_ostream rstring( sResult );
		  Printer.print( pArgument->getType() , rstring );
		  std::string sOperandOne = rstring.str();
		  if( sOperandOne != "%struct.Ndb*" )
		  {
		      continue;
		  }

		  if( pCall->getNumArgOperands() > 1 )
		  {
                      std::string strPath;
		      unsigned uLineNo;
		      getPathAndLineNo( pCall , strPath , uLineNo );
		      pCall->dump();
		      std::cout << strPath << " : " << uLineNo << std::endl;std::cout << "\t"
		                << getSourceLine( strPath , uLineNo ) << std::endl;
		  }
	      }

	  }
       }
   }

#endif



#if 1  
   TypePrinting Printer;
   std::vector<const Type *> NumberedTypes;
   AddModuleTypesToPrinter( Printer , NumberedTypes , _M );

   for( Function::iterator b = F.begin() , be = F.end() ; b != be; ++ b )
   {
      for( BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; ++ i )
      {
          if( CallInst * pCall = dyn_cast<CallInst>( i ) )
	  {
              Function * pFunction = pCall->getCalledFunction();
	      if( !pFunction )
	      {
                 continue;
	      }

	      std::string sFunction = pFunction->getName();
	      if( sFunction.find("startTransaction") != std::string::npos )
	      {
                   //pCall->getOperand(0)->dump();

		   if( Value * pArgument = dyn_cast<Value>(pCall->getOperand(0)) )
		   {
		       //pCall->getOperand(0)->getType()->dump();
		       std::string sResult;
		       raw_string_ostream rstring( sResult );
		       Printer.print( pArgument->getType() , rstring );
	               std::string sOperandOne = rstring.str();
		       //std::cout << "Here" << std::endl;
		       //std::cout << sOperandOne << std::endl;
		       if( sOperandOne != "%struct.Ndb*" )
		       {
                           continue;
		       }
                       //std::cout << sOperandOne  << std::endl;
		   }
		   else
		   {
		       continue;
		   }

		   //std::cout << pCall->getOperand(1)->getNameStr() << std::endl;

		   if( Constant * pConstant = dyn_cast<Constant>( pCall->getOperand(1) ))
		   {
                       //std::cout << pConstant->isNullValue()  << std::endl;
		       if( !pConstant->isNullValue() )
		       {
                          continue;
		       }
		   }
		   else
		   {
		       continue;
		   }
                   
		   for( Value::use_iterator u = pCall->use_begin() , ue = pCall->use_end() ; u != ue ; u ++ )
		   {
                       if( CallInst * pUseCall = dyn_cast<CallInst>( *u ) )
		       {
                           Function * pFun = pUseCall->getCalledFunction();
			   if(!pFun)
			   {
                               continue;
			   }

			   std::string sFunName = pFun->getName();
                           //std::cout << sFunName << std::endl;
			   if( sFunName.find( "getNdbOperation" ) != std::string::npos )
			   {
                                std::string strPath;
				unsigned uLineNo;
				getPathAndLineNo( pCall , strPath , uLineNo );
				std::cout << strPath << " : " << uLineNo << std::endl;
				std::cout << "\t" << getSourceLine( strPath , uLineNo ) << std::endl;

				getPathAndLineNo( pUseCall , strPath , uLineNo );
				std::cout << strPath << " : " << uLineNo << std::endl;
				std::cout << "\t" << getSourceLine(strPath , uLineNo ) << std::endl;
				std::cout << "========================================" << std::endl;
			   }
		       }
		   }
		   
		
	      }
	  }
      }
   }

#endif
}


//void PerfEvo:MySQLBug15811( Function &F) 
//{
//
//}

void PerfEvo::MySQLBug48229(Function &F)
{
   //std::cout << F.getNameStr() << std::endl;
   for( Function::iterator b = F.begin() , be = F.end() ; b != be ; ++ b )
   {
      for(BasicBlock::iterator i = b->begin() , ie = b->end() ; i != ie ; ++ i )
      {
         if(CallInst * pCall = dyn_cast<CallInst>(i) )
	 {
            Function * pFunction = pCall->getCalledFunction();
	    if(!pFunction)
	    {
               continue;
	    }
	    if( pCall->getNumArgOperands() != 2 )
	    {
               continue;
	    }
	    std::string sFunction = pFunction->getName();
	    if( sFunction.find("val_str") != std::string::npos /*&& sFunction.find("info") != std::string::npos*/ )
	    {
                std::string strPath;
		unsigned uLineNo;
		getPathAndLineNo(pCall , strPath , uLineNo );
		std::cout << strPath << " : " << uLineNo << std::endl;
		std::cout << "\t" << getSourceLine(strPath , uLineNo ) << std::endl;
	    }
	 }
      }
   }
}

void PerfEvo::ApacheBug33605(Function &F) 
{
  std::string sFunctionName = "setsockopt";
  for (Function::iterator b = F.begin(), be = F.end(); b != be; ++b) 
  {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) 
    {
      if (isa<CallInst>(&(*i)) || isa<InvokeInst>(&(*i))) 
      {
        std::string strPath;
        unsigned uLineNo = 0;
        assert(getPathAndLineNo(i, strPath, uLineNo) && "No DebugInfo");

        if (getSourceLine(strPath, uLineNo).find(sFunctionName) !=
                std::string::npos) 
        {
          Err << strPath << ":" << uLineNo << "\n"
              << getSourceLine(strPath, uLineNo) << "\n";
        }
      }
    }
  }
}

void PerfEvo::LoopNestedCallSites(Function &F) {
  LoopInfo &li = getAnalysis<LoopInfo>();

  std::list<Instruction *> csl = searchCallSites(F, "");

  for (std::list<Instruction *>::iterator cs = csl.begin(), cse = csl.end();
      cs != cse; ++cs) {
    BasicBlock *bb = (*cs)->getParent();
    unsigned int ld = li.getLoopDepth(bb);

    if (ld > 0) {
      std::string strPath;
      unsigned uLineNo=0;
      assert(getPathAndLineNo(*cs, strPath, uLineNo) && "No DebugInfo");

      Err << strPath << ":" << uLineNo << "\n"
          << getSourceLine(strPath, uLineNo) << "\n"
          << "LoopDepth: " << li.getLoopDepth(bb) << "\n";
    }
  }  
}

bool PerfEvo::doInitialization(Module &M) {
  _M = &M;
  if (!bBugHandlerInited) {
    if (strPerfBugID == "MozillaBug35294")
      pBugHandler = &PerfEvo::MozillaBug35294;
    else if (strPerfBugID == "MozillaBug66461")
      pBugHandler = &PerfEvo::MozillaBug66461;
    else if (strPerfBugID == "MozillaBug267506")
      pBugHandler = &PerfEvo::MozillaBug267506;
    else if (strPerfBugID == "MozillaBug311566")
      pBugHandler = &PerfEvo::MozillaBug311566;
    else if (strPerfBugID == "MozillaBug103330")
      pBugHandler = &PerfEvo::MozillaBug103330;
    else if (strPerfBugID == "MozillaBug258793")
      pBugHandler = &PerfEvo::MozillaBug258793;
    else if (strPerfBugID == "MozillaBug409961")
      pBugHandler = &PerfEvo::MozillaBug409961;
    else if (strPerfBugID == "MySQLBug26527")
      pBugHandler = &PerfEvo::MySQLBug26527;
    else if (strPerfBugID == "MySQLBug38941")
      pBugHandler = &PerfEvo::MySQLBug38941;
    else if (strPerfBugID == "MySQLBug38968")
      pBugHandler = NULL; //&PerfEvo::MySQLBug38968;
    else if (strPerfBugID == "MySQLBug38769")
      pBugHandler = &PerfEvo::MySQLBug38769;
    else if (strPerfBugID == "MySQLBug49491")
      pBugHandler = &PerfEvo::MySQLBug49491;
    else if (strPerfBugID == "MySQLBug38824")
      pBugHandler = &PerfEvo::MySQLBug38824;
    else if (strPerfBugID == "MySQLBug14637")
      pBugHandler = &PerfEvo::MySQLBug14637;
    else if (strPerfBugID == "MySQLBug39268")
      pBugHandler = &PerfEvo::MySQLBug39268;
    //else if (strPerfBugID == "MySQLBug15811")
    //  pBugHandler = &PerfEvo::MySQLBug15811;
    else if (strPerfBugID == "ApacheBug33605")
      pBugHandler = &PerfEvo::ApacheBug33605;
    else if (strPerfBugID == "ApacheBug45464")
      pBugHandler = &PerfEvo::ApacheBug45464;
    else if(strPerfBugID == "MySQLBug48229")
      pBugHandler = &PerfEvo::MySQLBug48229;
    // else if (strPerfBugID == "ApacheBug45464")
    //   LoopNestedCallSites(F);
    else
      assert(false && "No checker implemented for this bug yet");
    if (strPerfBugID != "MySQLBug38968")
      loadSourceFiles(&M);
    else
      MySQLBug38968();
  }
  //Err << "Initialization Done!\n";
  return false;
}

bool PerfEvo::runOnFunction(Function &F) {
  if (strPerfBugID != "MySQLBug38968")
    (this->*pBugHandler)(F);
  return false;
}

// We don't modify the program, so we preserve all analyses
void PerfEvo::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LoopInfo>();
}

char PerfEvo::ID = 0;
INITIALIZE_PASS(PerfEvo, "PerfEvo",
             "PerfEvo Pass",
             false, 
             false );
