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

#ifndef BCC_RS_COMPILER_DRIVER_H
#define BCC_RS_COMPILER_DRIVER_H

#include <string>

#include "bcc/ExecutionEngine/BCCRuntimeSymbolResolver.h"
#include "bcc/ExecutionEngine/SymbolResolvers.h"
#include "bcc/ExecutionEngine/SymbolResolverProxy.h"
#include "bcc/RenderScript/RSCompiler.h"

namespace bcc {

class CompilerConfig;
class RSExecutable;
class RSScript;

class RSCompilerDriver {
private:
  CompilerConfig *mConfig;
  RSCompiler mCompiler;

  BCCRuntimeSymbolResolver mBCCRuntime;
  LookupFunctionSymbolResolver<void*> mRSRuntime;
  SymbolResolverProxy mResolver;

  RSExecutable *loadScriptCache(const RSScript &pScript,
                                const std::string &pOutputPath);

  // Setup the compiler config for the given script. Return true if mConfig has
  // been changed and false if it remains unchanged.
  bool setupConfig(const RSScript &pScript);

  RSExecutable *compileScript(RSScript &pScript,
                              const std::string &pOutputPath);

public:
  RSCompilerDriver();
  ~RSCompilerDriver();

  inline void setRSRuntimeLookupFunction(
      LookupFunctionSymbolResolver<>::LookupFunctionTy pLookupFunc)
  { mRSRuntime.setLookupFunction(pLookupFunc); }
  inline void setRSRuntimeLookupContext(void *pContext)
  { mRSRuntime.setContext(pContext); }

  // FIXME: This method accompany with loadScriptCache and compileScript should
  //        all be const-methods. They're not now because the getAddress() in
  //        SymbolResolverInterface is not a const-method.
  RSExecutable *build(RSScript &pScript,
                      const std::string &pOutputPath);
};

} // end namespace bcc

#endif // BCC_RS_COMPILER_DRIVER_H
