//===--- SourceLoader.cpp - Import .swift files as modules ------*- c++ -*-===//
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
///
/// \file A simple module loader that loads .swift source files as
/// TranslationUnit modules.
///
//===----------------------------------------------------------------------===//

#include "swift/Sema/SourceLoader.h"
#include "swift/Subsystems.h"
#include "swift/AST/AST.h"
#include "swift/AST/Component.h"
#include "swift/AST/Diagnostics.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"

using namespace swift;

static llvm::error_code findModule(ASTContext &ctx, StringRef moduleID,
                                   SourceLoc importLoc,
                                   llvm::OwningPtr<llvm::MemoryBuffer> &buffer){
  llvm::SmallString<64> moduleFilename(moduleID);
  moduleFilename += ".swift";

  llvm::SmallString<128> inputFilename;

  // First, search in the directory corresponding to the import location.
  // FIXME: This screams for a proper FileManager abstraction.
  int currentBufferID = ctx.SourceMgr.FindBufferContainingLoc(importLoc.Value);
  if (currentBufferID >= 0) {
    const llvm::MemoryBuffer *importingBuffer
      = ctx.SourceMgr.getBufferInfo(currentBufferID).Buffer;
    StringRef currentDirectory
      = llvm::sys::path::parent_path(importingBuffer->getBufferIdentifier());
    if (!currentDirectory.empty()) {
      inputFilename = currentDirectory;
      llvm::sys::path::append(inputFilename, moduleFilename.str());
      llvm::error_code err = llvm::MemoryBuffer::getFile(inputFilename, buffer);
      if (!err)
        return err;
    }
  }

  // Second, search in the current directory.
  llvm::error_code err = llvm::MemoryBuffer::getFile(moduleFilename, buffer);
  if (!err)
    return err;

  // If we fail, search each import search path.
  for (auto Path : ctx.ImportSearchPaths) {
    inputFilename = Path;
    llvm::sys::path::append(inputFilename, moduleFilename.str());
    err = llvm::MemoryBuffer::getFile(inputFilename, buffer);
    if (!err)
      return err;
  }

  return err;
}


Module *SourceLoader::loadModule(
    SourceLoc importLoc,
    ArrayRef<std::pair<Identifier, SourceLoc>> path) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];

  llvm::OwningPtr<llvm::MemoryBuffer> inputFile;
  if (llvm::error_code err = findModule(Ctx, moduleID.first.str(),
                                        moduleID.second, inputFile)) {
    if (err.value() != llvm::errc::no_such_file_or_directory) {
      Ctx.Diags.diagnose(moduleID.second, diag::sema_opening_import,
                         moduleID.first.str(), err.message());
    }

    return nullptr;
  }

  unsigned bufferID = Ctx.SourceMgr.AddNewSourceBuffer(inputFile.take(),
                                                       moduleID.second.Value);

  // FIXME: Turn off the constraint-based type checker for the imported 'swift'
  // module.
  llvm::SaveAndRestore<bool> saveUseCS(Ctx.LangOpts.UseConstraintSolver,
                                       (Ctx.LangOpts.UseConstraintSolver &&
                                        moduleID.first.str() != "swift"));

  // For now, treat all separate modules as unique components.
  Component *comp = new (Ctx.Allocate<Component>(1)) Component();
  TranslationUnit *importTU = new (Ctx) TranslationUnit(moduleID.first, comp,
                                                        Ctx,
                                                        /*IsMainModule*/false,
                                                        /*IsReplModule*/false);

  Ctx.LoadedModules[moduleID.first.str()] = importTU;
  parseIntoTranslationUnit(importTU, bufferID);

  // We have to do name binding on it to ensure that types are fully resolved.
  // This should eventually be eliminated by having actual fully resolved binary
  // dumps of the code instead of reparsing though.
  // FIXME: We also need to deal with circular imports!
  performNameBinding(importTU);
  performTypeChecking(importTU);

  return importTU;
}
