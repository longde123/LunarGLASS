//===- TopToBottom.cpp - Translate Top IR to Bottom IR --------------------===//
//
// LunarGLASS: An Open Modular Shader Compiler Architecture
// Copyright (C) 2010-2011 LunarG, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; version 2 of the
// License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
// 02110-1301, USA.
//
//===----------------------------------------------------------------------===//
//
// Author: John Kessenich, LunarG
//
// Translate Top IR to Bottom IR
//
//===----------------------------------------------------------------------===//

// LLVM includes
#include "llvm/DerivedTypes.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <string>
#include <map>
#include <vector>

// LunarGLASS includes
#include "Exceptions.h"
#include "Backend.h"
#include "PrivateManager.h"
#include "Options.h"

// LunarGLASS Passes
#include "Passes/Passes.h"

void gla::PrivateManager::translateTopToBottom()
{
#ifdef _WIN32
    unsigned int oldFormat = _set_output_format(_TWO_DIGIT_EXPONENT);
#endif

    if (gla::Options.debug) {
        llvm::errs() << "\nTop IR:\n";
        module->dump();
    }

    runLLVMOptimizations1();

    if (gla::Options.debug) {
        llvm::errs() << "\n\nBottom IR:\n";
        module->dump();
    }

#ifdef _WIN32
    _set_output_format(oldFormat);
#endif

    int innerAoS, outerSoA;
    backEnd->getRegisterForm(outerSoA, innerAoS);
    if (outerSoA != 1)
        UnsupportedFunctionality("SoA in middle end: ", outerSoA);
    if (innerAoS != 4)
        UnsupportedFunctionality("AoS other than size 4 in middle end: ", innerAoS);

    // make sure we can decompose all the intrisics
    for (int d = 0; d < gla::EDiCount; ++d) {
        if (backEnd->decomposeIntrinsic(d))
            UnsupportedFunctionality("intrinsic decomposition in middle end");
    }
}

static inline void RunOnModule(llvm::FunctionPassManager& pm, llvm::Module* m)
{
    pm.doInitialization();
    for (llvm::Module::iterator f = m->begin(), e = m->end(); f != e; ++f)
        pm.run(*f);
    pm.doFinalization();
}

// Verify each function
static inline void VerifyModule(llvm::Module* module)
{
    llvm::FunctionPassManager verifier(module);
    verifier.add(llvm::createVerifierPass());

    RunOnModule(verifier, module);
}

void gla::PrivateManager::runLLVMOptimizations1()
{
    // TODO: turn off for release builds
    VerifyModule(module);

    // TODO: If we can turn on InstCombine, then replace many of the occurances
    // of InstSimplify with InstCombine.

    // First, do some global (module-level) optimizations, which can free up
    // function passes to do more.
    llvm::PassManager globalPM;
    globalPM.add(llvm::createGlobalOptimizerPass());
    globalPM.add(llvm::createIPSCCPPass());
    globalPM.add(llvm::createConstantMergePass());
    globalPM.add(llvm::createInstructionSimplifierPass());
    globalPM.run(*module);

    // Next, do interprocedural passes
    // Future work: If we ever have non-inlined functions, we'll want to add some

    // TODO: turn off for release builds
    VerifyModule(module);

    // Set up the function-level optimizations we want
    // TODO: explore ordering of passes more
    llvm::FunctionPassManager passManager(module);

    // TODO: explore ScalarReplAggregates more
    passManager.add(llvm::createPromoteMemoryToRegisterPass());

    // TODO: explore SimplifyLibCalls
    // TODO: see if we can avoid running gvn/sccp multiple times

    // Early, simple optimizations to enable others/make others more efficient
    passManager.add(llvm::createInstructionSimplifierPass());
    passManager.add(llvm::createEarlyCSEPass());
    passManager.add(llvm::createCorrelatedValuePropagationPass());

    passManager.add(gla_llvm::createCanonicalizeCFGPass());
    passManager.add(llvm::createReassociatePass());
    passManager.add(llvm::createInstructionSimplifierPass());

    // Run GVN and SCCP using BasicAliasAnalysis
    passManager.add(llvm::createBasicAliasAnalysisPass());
    passManager.add(llvm::createGVNPass());
    passManager.add(llvm::createSCCPPass());

    // Make multiinsert intrinsics, and clean up afterwards
    passManager.add(gla_llvm::createCoalesceInsertsPass());
    passManager.add(llvm::createAggressiveDCEPass());

    // Loop optimizations, and clean up afterwards
    passManager.add(llvm::createIndVarSimplifyPass());
    passManager.add(llvm::createLICMPass());
    passManager.add(llvm::createLoopStrengthReducePass());
    passManager.add(llvm::createAggressiveDCEPass());

    // Run GVN and SCCP using BasicAliasAnalysis
    passManager.add(llvm::createBasicAliasAnalysisPass());
    passManager.add(llvm::createGVNPass());
    passManager.add(llvm::createSCCPPass());

    // TODO: Consider if we really want it. For some reason StandardPasses.h
    // doesn't have it listed.
    // passManager.add(llvm::createSinkingPass());

    // Run some post-redancy-elimination passes
    passManager.add(llvm::createInstructionSimplifierPass());
    passManager.add(llvm::createCorrelatedValuePropagationPass());
    passManager.add(llvm::createAggressiveDCEPass());

    // LunarGLASS CFG optimizations
    passManager.add(gla_llvm::createFlattenConditionalAssignmentsPass());

    // TODO: Figure out if we can use InstCombine, or if we have to make our own
    // // passManager.add(llvm::createInstructionCombiningPass());

    RunOnModule(passManager, module);

    // TODO: turn off for release builds
    VerifyModule(module);

    // Post Function passes cleanup
    llvm::PassManager pm;
    pm.add(llvm::createInstructionSimplifierPass());
    pm.add(llvm::createDeadStoreEliminationPass());
    pm.add(llvm::createAggressiveDCEPass());
    pm.add(llvm::createStripDeadPrototypesPass());
    pm.add(llvm::createDeadTypeEliminationPass());

    // TODO: Consider using the below in the presense of functions
    // pm.add(llvm::createGlobalDCEPass());

    pm.run(*module);

    // TODO: turn off for release builds
    VerifyModule(module);

    // TODO: Refactor the below use of GlobalOpt. Perhaps we want to repeat our
    // some function passes?

    llvm::PassManager modulePassManager;
    modulePassManager.add(llvm::createGlobalOptimizerPass());

    // Optimize the whole module
    bool changed = modulePassManager.run(*module);

    if (changed) {
        // removing globals created stack allocations we want to eliminate
        llvm::FunctionPassManager postGlobalManager(module);
        postGlobalManager.add(llvm::createPromoteMemoryToRegisterPass());

        // run across all functions
        postGlobalManager.doInitialization();
        for (llvm::Module::iterator function = module->begin(), lastFunction = module->end(); function != lastFunction; ++function) {
            postGlobalManager.run(*function);
        }
        postGlobalManager.doFinalization();
    }

    if (! backEnd->preferRegistersOverMemory()) {
        llvm::FunctionPassManager memoryPassManager(module);
        memoryPassManager.add(llvm::createDemoteRegisterToMemoryPass());

        memoryPassManager.doInitialization();
        for (llvm::Module::iterator function = module->begin(), lastFunction = module->end(); function != lastFunction; ++function) {
            memoryPassManager.run(*function);
        }
        memoryPassManager.doFinalization();
    }

    // TODO: turn off for release builds
    VerifyModule(module);

    llvm::PassManager canonicalize;

    canonicalize.add(llvm::createIndVarSimplifyPass());
    canonicalize.add(gla_llvm::createCanonicalizeCFGPass());
    canonicalize.run(*module);

    // TODO: turn off for release builds
    VerifyModule(module);

}
