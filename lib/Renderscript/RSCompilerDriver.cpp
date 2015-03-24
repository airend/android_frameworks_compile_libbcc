/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bcc/Renderscript/RSCompilerDriver.h"

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include <llvm/IR/Module.h>
#include "llvm/Linker/Linker.h"
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include "bcinfo/BitcodeWrapper.h"
#include "bcc/Assert.h"
#include "bcinfo/MetadataExtractor.h"
#include "bcc/BCCContext.h"
#include "bcc/Compiler.h"
#include "bcc/Config/Config.h"
#include "bcc/Renderscript/RSScript.h"
#include "bcc/Renderscript/RSScriptGroupFusion.h"
#include "bcc/Support/CompilerConfig.h"
#include "bcc/Source.h"
#include "bcc/Support/FileMutex.h"
#include "bcc/Support/Log.h"
#include "bcc/Support/InputFile.h"
#include "bcc/Support/Initialization.h"
#include "bcc/Support/OutputFile.h"

#include <sstream>
#include <string>

#ifdef HAVE_ANDROID_OS
#include <cutils/properties.h>
#endif
#include <utils/StopWatch.h>

using namespace bcc;

RSCompilerDriver::RSCompilerDriver(bool pUseCompilerRT) :
    mConfig(nullptr), mCompiler(), mDebugContext(false),
    mLinkRuntimeCallback(nullptr), mEnableGlobalMerge(true) {
  init::Initialize();
}

RSCompilerDriver::~RSCompilerDriver() {
  delete mConfig;
}


#if defined(PROVIDE_ARM_CODEGEN)
extern llvm::cl::opt<bool> EnableGlobalMerge;
#endif

bool RSCompilerDriver::setupConfig(const RSScript &pScript) {
  bool changed = false;

  const llvm::CodeGenOpt::Level script_opt_level =
      static_cast<llvm::CodeGenOpt::Level>(pScript.getOptimizationLevel());

#if defined(PROVIDE_ARM_CODEGEN)
  EnableGlobalMerge = mEnableGlobalMerge;
#endif

  if (mConfig != nullptr) {
    // Renderscript bitcode may have their optimization flag configuration
    // different than the previous run of RS compilation.
    if (mConfig->getOptimizationLevel() != script_opt_level) {
      mConfig->setOptimizationLevel(script_opt_level);
      changed = true;
    }
  } else {
    // Haven't run the compiler ever.
    mConfig = new (std::nothrow) CompilerConfig(DEFAULT_TARGET_TRIPLE_STRING);
    if (mConfig == nullptr) {
      // Return false since mConfig remains NULL and out-of-memory.
      return false;
    }
    mConfig->setOptimizationLevel(script_opt_level);
    changed = true;
  }

#if defined(PROVIDE_ARM_CODEGEN)
  bcinfo::MetadataExtractor me(&pScript.getSource().getModule());
  if (!me.extract()) {
    assert("Could not extract RS pragma metadata for module!");
  }

  bool script_full_prec = (me.getRSFloatPrecision() == bcinfo::RS_FP_Full);
  if (mConfig->getFullPrecision() != script_full_prec) {
    mConfig->setFullPrecision(script_full_prec);
    changed = true;
  }
#endif

  return changed;
}

Compiler::ErrorCode RSCompilerDriver::compileScript(RSScript& pScript, const char* pScriptName,
                                                    const char* pOutputPath,
                                                    const char* pRuntimePath,
                                                    const char* pBuildChecksum,
                                                    bool pDumpIR) {
  // embed build checksum metadata into the source
  if (pBuildChecksum != nullptr && strlen(pBuildChecksum) > 0) {
    pScript.getSource().addBuildChecksumMetadata(pBuildChecksum);
  }

  //===--------------------------------------------------------------------===//
  // Link RS script with Renderscript runtime.
  //===--------------------------------------------------------------------===//
  if (!RSScript::LinkRuntime(pScript, pRuntimePath)) {
    ALOGE("Failed to link script '%s' with Renderscript runtime %s!",
          pScriptName, pRuntimePath);
    return Compiler::kErrInvalidSource;
  }

  {
    // FIXME(srhines): Windows compilation can't use locking like this, but
    // we also don't need to worry about concurrent writers of the same file.
#ifndef USE_MINGW
    //===------------------------------------------------------------------===//
    // Acquire the write lock for writing output object file.
    //===------------------------------------------------------------------===//
    FileMutex<FileBase::kWriteLock> write_output_mutex(pOutputPath);

    if (write_output_mutex.hasError() || !write_output_mutex.lock()) {
      ALOGE("Unable to acquire the lock for writing %s! (%s)",
            pOutputPath, write_output_mutex.getErrorMessage().c_str());
      return Compiler::kErrInvalidSource;
    }
#endif

    // Open the output file for write.
    OutputFile output_file(pOutputPath,
                           FileBase::kTruncate | FileBase::kBinary);

    if (output_file.hasError()) {
        ALOGE("Unable to open %s for write! (%s)", pOutputPath,
              output_file.getErrorMessage().c_str());
      return Compiler::kErrInvalidSource;
    }

    // Setup the config to the compiler.
    bool compiler_need_reconfigure = setupConfig(pScript);

    if (mConfig == nullptr) {
      ALOGE("Failed to setup config for RS compiler to compile %s!",
            pOutputPath);
      return Compiler::kErrInvalidSource;
    }

    if (compiler_need_reconfigure) {
      Compiler::ErrorCode err = mCompiler.config(*mConfig);
      if (err != Compiler::kSuccess) {
        ALOGE("Failed to config the RS compiler for %s! (%s)",pOutputPath,
              Compiler::GetErrorString(err));
        return Compiler::kErrInvalidSource;
      }
    }

    OutputFile *ir_file = nullptr;
    llvm::raw_fd_ostream *IRStream = nullptr;
    if (pDumpIR) {
      std::string path(pOutputPath);
      path.append(".ll");
      ir_file = new OutputFile(path.c_str(), FileBase::kTruncate);
      IRStream = ir_file->dup();
    }

    // Run the compiler.
    Compiler::ErrorCode compile_result =
        mCompiler.compile(pScript, output_file, IRStream);

    if (ir_file) {
      ir_file->close();
      delete ir_file;
    }

    if (compile_result != Compiler::kSuccess) {
      ALOGE("Unable to compile the source to file %s! (%s)", pOutputPath,
            Compiler::GetErrorString(compile_result));
      return Compiler::kErrInvalidSource;
    }
  }

  return Compiler::kSuccess;
}

bool RSCompilerDriver::build(BCCContext &pContext,
                             const char *pCacheDir,
                             const char *pResName,
                             const char *pBitcode,
                             size_t pBitcodeSize,
                             const char *pBuildChecksum,
                             const char *pRuntimePath,
                             RSLinkRuntimeCallback pLinkRuntimeCallback,
                             bool pDumpIR) {
    //  android::StopWatch build_time("bcc: RSCompilerDriver::build time");
  //===--------------------------------------------------------------------===//
  // Check parameters.
  //===--------------------------------------------------------------------===//
  if ((pCacheDir == nullptr) || (pResName == nullptr)) {
    ALOGE("Invalid parameter passed to RSCompilerDriver::build()! (cache dir: "
          "%s, resource name: %s)", ((pCacheDir) ? pCacheDir : "(null)"),
                                    ((pResName) ? pResName : "(null)"));
    return false;
  }

  if ((pBitcode == nullptr) || (pBitcodeSize <= 0)) {
    ALOGE("No bitcode supplied! (bitcode: %p, size of bitcode: %u)",
          pBitcode, static_cast<unsigned>(pBitcodeSize));
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Construct output path.
  // {pCacheDir}/{pResName}.o
  //===--------------------------------------------------------------------===//
  llvm::SmallString<80> output_path(pCacheDir);
  llvm::sys::path::append(output_path, pResName);
  llvm::sys::path::replace_extension(output_path, ".o");

  //===--------------------------------------------------------------------===//
  // Load the bitcode and create script.
  //===--------------------------------------------------------------------===//
  Source *source = Source::CreateFromBuffer(pContext, pResName,
                                            pBitcode, pBitcodeSize);
  if (source == nullptr) {
    return false;
  }

  RSScript script(*source);
  if (pLinkRuntimeCallback) {
    setLinkRuntimeCallback(pLinkRuntimeCallback);
  }

  script.setLinkRuntimeCallback(getLinkRuntimeCallback());

  // Read information from bitcode wrapper.
  bcinfo::BitcodeWrapper wrapper(pBitcode, pBitcodeSize);
  script.setCompilerVersion(wrapper.getCompilerVersion());
  script.setOptimizationLevel(static_cast<RSScript::OptimizationLevel>(
                              wrapper.getOptimizationLevel()));

  //===--------------------------------------------------------------------===//
  // Compile the script
  //===--------------------------------------------------------------------===//
  Compiler::ErrorCode status = compileScript(script, pResName,
                                             output_path.c_str(),
                                             pRuntimePath,
                                             pBuildChecksum,
                                             pDumpIR);

  return status == Compiler::kSuccess;
}

bool RSCompilerDriver::buildScriptGroup(
    BCCContext& Context, const char* pOutputFilepath, const char* pRuntimePath,
    const char* pRuntimeRelaxedPath, bool dumpIR,
    const std::vector<Source*>& sources,
    const std::list<std::list<std::pair<int, int>>>& toFuse,
    const std::list<std::string>& fused,
    const std::list<std::list<std::pair<int, int>>>& invokes,
    const std::list<std::string>& invokeBatchNames) {
  // ---------------------------------------------------------------------------
  // Link all input modules into a single module
  // ---------------------------------------------------------------------------

  llvm::LLVMContext& context = Context.getLLVMContext();
  llvm::Module module("Merged Script Group", context);

  llvm::Linker linker(&module);
  for (Source* source : sources) {
    if (linker.linkInModule(&source->getModule())) {
      ALOGE("Linking for module in source failed.");
      return false;
    }
  }

  // ---------------------------------------------------------------------------
  // Create fused kernels
  // ---------------------------------------------------------------------------

  auto inputIter = toFuse.begin();
  for (const std::string& nameOfFused : fused) {
    auto inputKernels = *inputIter++;
    std::vector<Source*> sourcesToFuse;
    std::vector<int> slots;

    for (auto p : inputKernels) {
      sourcesToFuse.push_back(sources[p.first]);
      slots.push_back(p.second);
    }

    if (!fuseKernels(Context, sourcesToFuse, slots, nameOfFused, &module)) {
      return false;
    }
  }

  // ---------------------------------------------------------------------------
  // Rename invokes
  // ---------------------------------------------------------------------------

  auto invokeIter = invokes.begin();
  for (const std::string& newName : invokeBatchNames) {
    auto inputInvoke = *invokeIter++;
    auto p = inputInvoke.front();
    Source* source = sources[p.first];
    int slot = p.second;

    if (!renameInvoke(Context, source, slot, newName, &module)) {
      return false;
    }
  }

  // ---------------------------------------------------------------------------
  // Compile the new module with fused kernels
  // ---------------------------------------------------------------------------

  const std::unique_ptr<Source> source(
      Source::CreateFromModule(Context, pOutputFilepath, module, true));
  RSScript script(*source);

  const char* buildChecksum = "DummyChecksumForScriptGroup";

  // Embed the info string directly in the ELF
  script.setEmbedInfo(true);
  script.setOptimizationLevel(RSScript::kOptLvl3);

  llvm::SmallString<80> output_path(pOutputFilepath);
  llvm::sys::path::replace_extension(output_path, ".o");

  // Pick the right runtime lib
  const char* coreLibPath = pRuntimePath;
  if (strcmp(pRuntimeRelaxedPath, "")) {
      bcinfo::MetadataExtractor me(&module);
      me.extract();
      if (me.getRSFloatPrecision() == bcinfo::RS_FP_Relaxed) {
          coreLibPath = pRuntimeRelaxedPath;
      }
  }

  compileScript(script, pOutputFilepath, output_path.c_str(), coreLibPath,
                buildChecksum, dumpIR);

  return true;
}

bool RSCompilerDriver::buildForCompatLib(RSScript &pScript, const char *pOut,
                                         const char *pBuildChecksum,
                                         const char *pRuntimePath,
                                         bool pDumpIR) {
  // Embed the info string directly in the ELF, since this path is for an
  // offline (host) compilation.
  pScript.setEmbedInfo(true);

  Compiler::ErrorCode status = compileScript(pScript, pOut, pOut, pRuntimePath,
                                             pBuildChecksum, pDumpIR);
  if (status != Compiler::kSuccess) {
    return false;
  }

  return true;
}
