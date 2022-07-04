//===- ModuleDepCollector.cpp - Callbacks to collect deps -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/DependencyScanning/ModuleDepCollector.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningWorker.h"
#include "llvm/Support/StringSaver.h"

using namespace clang;
using namespace tooling;
using namespace dependencies;

static void optimizeHeaderSearchOpts(HeaderSearchOptions &Opts,
                                     ASTReader &Reader,
                                     const serialization::ModuleFile &MF) {
  // Only preserve search paths that were used during the dependency scan.
  std::vector<HeaderSearchOptions::Entry> Entries = Opts.UserEntries;
  Opts.UserEntries.clear();

  llvm::BitVector SearchPathUsage(Entries.size());
  llvm::DenseSet<const serialization::ModuleFile *> Visited;
  std::function<void(const serialization::ModuleFile *)> VisitMF =
      [&](const serialization::ModuleFile *MF) {
        SearchPathUsage |= MF->SearchPathUsage;
        Visited.insert(MF);
        for (const serialization::ModuleFile *Import : MF->Imports)
          if (!Visited.contains(Import))
            VisitMF(Import);
      };
  VisitMF(&MF);

  for (auto Idx : SearchPathUsage.set_bits())
    Opts.UserEntries.push_back(Entries[Idx]);
}

CompilerInvocation ModuleDepCollector::makeInvocationForModuleBuildWithoutPaths(
    const ModuleDeps &Deps,
    llvm::function_ref<void(CompilerInvocation &)> Optimize) const {
  // Make a deep copy of the original Clang invocation.
  CompilerInvocation CI(OriginalInvocation);

  CI.getLangOpts()->resetNonModularOptions();
  CI.getPreprocessorOpts().resetNonModularOptions();

  // Remove options incompatible with explicit module build or are likely to
  // differ between identical modules discovered from different translation
  // units.
  CI.getFrontendOpts().Inputs.clear();
  CI.getFrontendOpts().OutputFile.clear();
  CI.getCodeGenOpts().MainFileName.clear();
  CI.getCodeGenOpts().DwarfDebugFlags.clear();

  CI.getFrontendOpts().ProgramAction = frontend::GenerateModule;
  CI.getLangOpts()->ModuleName = Deps.ID.ModuleName;
  CI.getFrontendOpts().IsSystemModule = Deps.IsSystem;

  // Disable implicit modules and canonicalize options that are only used by
  // implicit modules.
  CI.getLangOpts()->ImplicitModules = false;
  CI.getHeaderSearchOpts().ImplicitModuleMaps = false;
  CI.getHeaderSearchOpts().ModuleCachePath.clear();
  CI.getHeaderSearchOpts().ModulesValidateOncePerBuildSession = false;
  CI.getHeaderSearchOpts().BuildSessionTimestamp = 0;
  // The specific values we canonicalize to for pruning don't affect behaviour,
  /// so use the default values so they will be dropped from the command-line.
  CI.getHeaderSearchOpts().ModuleCachePruneInterval = 7 * 24 * 60 * 60;
  CI.getHeaderSearchOpts().ModuleCachePruneAfter = 31 * 24 * 60 * 60;

  // Report the prebuilt modules this module uses.
  for (const auto &PrebuiltModule : Deps.PrebuiltModuleDeps)
    CI.getFrontendOpts().ModuleFiles.push_back(PrebuiltModule.PCMFile);

  CI.getFrontendOpts().ModuleMapFiles = Deps.ModuleMapFileDeps;

  Optimize(CI);

  // The original invocation probably didn't have strict context hash enabled.
  // We will use the context hash of this invocation to distinguish between
  // multiple incompatible versions of the same module and will use it when
  // reporting dependencies to the clients. Let's make sure we're using
  // **strict** context hash in order to prevent accidental sharing of
  // incompatible modules (e.g. with differences in search paths).
  CI.getHeaderSearchOpts().ModulesStrictContextHash = true;

  return CI;
}

static std::vector<std::string>
serializeCompilerInvocation(const CompilerInvocation &CI) {
  // Set up string allocator.
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Strings(Alloc);
  auto SA = [&Strings](const Twine &Arg) { return Strings.save(Arg).data(); };

  // Synthesize full command line from the CompilerInvocation, including "-cc1".
  SmallVector<const char *, 32> Args{"-cc1"};
  CI.generateCC1CommandLine(Args, SA);

  // Convert arguments to the return type.
  return std::vector<std::string>{Args.begin(), Args.end()};
}

std::vector<std::string> ModuleDeps::getCanonicalCommandLine(
    std::function<StringRef(ModuleID)> LookupPCMPath) const {
  CompilerInvocation CI(BuildInvocation);
  FrontendOptions &FrontendOpts = CI.getFrontendOpts();

  InputKind ModuleMapInputKind(FrontendOpts.DashX.getLanguage(),
                               InputKind::Format::ModuleMap);
  FrontendOpts.Inputs.emplace_back(ClangModuleMapFile, ModuleMapInputKind);
  FrontendOpts.OutputFile = std::string(LookupPCMPath(ID));

  for (ModuleID MID : ClangModuleDeps)
    FrontendOpts.ModuleFiles.emplace_back(LookupPCMPath(MID));

  return serializeCompilerInvocation(CI);
}

std::vector<std::string>
ModuleDeps::getCanonicalCommandLineWithoutModulePaths() const {
  return serializeCompilerInvocation(BuildInvocation);
}

void ModuleDepCollectorPP::FileChanged(SourceLocation Loc,
                                       FileChangeReason Reason,
                                       SrcMgr::CharacteristicKind FileType,
                                       FileID PrevFID) {
  if (Reason != PPCallbacks::EnterFile)
    return;

  // This has to be delayed as the context hash can change at the start of
  // `CompilerInstance::ExecuteAction`.
  if (MDC.ContextHash.empty()) {
    MDC.ContextHash = MDC.ScanInstance.getInvocation().getModuleHash();
    MDC.Consumer.handleContextHash(MDC.ContextHash);
  }

  SourceManager &SM = MDC.ScanInstance.getSourceManager();

  // Dependency generation really does want to go all the way to the
  // file entry for a source location to find out what is depended on.
  // We do not want #line markers to affect dependency generation!
  if (Optional<StringRef> Filename =
          SM.getNonBuiltinFilenameForID(SM.getFileID(SM.getExpansionLoc(Loc))))
    MDC.FileDeps.push_back(
        std::string(llvm::sys::path::remove_leading_dotslash(*Filename)));
}

void ModuleDepCollectorPP::InclusionDirective(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FilenameRange, Optional<FileEntryRef> File,
    StringRef SearchPath, StringRef RelativePath, const Module *Imported,
    SrcMgr::CharacteristicKind FileType) {
  if (!File && !Imported) {
    // This is a non-modular include that HeaderSearch failed to find. Add it
    // here as `FileChanged` will never see it.
    MDC.FileDeps.push_back(std::string(FileName));
  }
  handleImport(Imported);
}

void ModuleDepCollectorPP::moduleImport(SourceLocation ImportLoc,
                                        ModuleIdPath Path,
                                        const Module *Imported) {
  handleImport(Imported);
}

void ModuleDepCollectorPP::handleImport(const Module *Imported) {
  if (!Imported)
    return;

  const Module *TopLevelModule = Imported->getTopLevelModule();

  if (MDC.isPrebuiltModule(TopLevelModule))
    DirectPrebuiltModularDeps.insert(TopLevelModule);
  else
    DirectModularDeps.insert(TopLevelModule);
}

void ModuleDepCollectorPP::EndOfMainFile() {
  FileID MainFileID = MDC.ScanInstance.getSourceManager().getMainFileID();
  MDC.MainFile = std::string(MDC.ScanInstance.getSourceManager()
                                 .getFileEntryForID(MainFileID)
                                 ->getName());

  if (!MDC.ScanInstance.getPreprocessorOpts().ImplicitPCHInclude.empty())
    MDC.FileDeps.push_back(
        MDC.ScanInstance.getPreprocessorOpts().ImplicitPCHInclude);

  for (const Module *M : DirectModularDeps) {
    // A top-level module might not be actually imported as a module when
    // -fmodule-name is used to compile a translation unit that imports this
    // module. In that case it can be skipped. The appropriate header
    // dependencies will still be reported as expected.
    if (!M->getASTFile())
      continue;
    handleTopLevelModule(M);
  }

  MDC.Consumer.handleDependencyOutputOpts(*MDC.Opts);

  for (auto &&I : MDC.ModularDeps)
    MDC.Consumer.handleModuleDependency(*I.second);

  for (auto &&I : MDC.FileDeps)
    MDC.Consumer.handleFileDependency(I);

  for (auto &&I : DirectPrebuiltModularDeps)
    MDC.Consumer.handlePrebuiltModuleDependency(PrebuiltModuleDep{I});
}

ModuleID ModuleDepCollectorPP::handleTopLevelModule(const Module *M) {
  assert(M == M->getTopLevelModule() && "Expected top level module!");

  // If this module has been handled already, just return its ID.
  auto ModI = MDC.ModularDeps.insert({M, nullptr});
  if (!ModI.second)
    return ModI.first->second->ID;

  ModI.first->second = std::make_unique<ModuleDeps>();
  ModuleDeps &MD = *ModI.first->second;

  MD.ID.ModuleName = M->getFullModuleName();
  MD.ImportedByMainFile = DirectModularDeps.contains(M);
  MD.ImplicitModulePCMPath = std::string(M->getASTFile()->getName());
  MD.IsSystem = M->IsSystem;

  const FileEntry *ModuleMap = MDC.ScanInstance.getPreprocessor()
                                   .getHeaderSearchInfo()
                                   .getModuleMap()
                                   .getModuleMapFileForUniquing(M);

  if (ModuleMap) {
    StringRef Path = ModuleMap->tryGetRealPathName();
    if (Path.empty())
      Path = ModuleMap->getName();
    MD.ClangModuleMapFile = std::string(Path);
  }

  serialization::ModuleFile *MF =
      MDC.ScanInstance.getASTReader()->getModuleManager().lookup(
          M->getASTFile());
  MDC.ScanInstance.getASTReader()->visitInputFiles(
      *MF, true, true, [&](const serialization::InputFile &IF, bool isSystem) {
        // __inferred_module.map is the result of the way in which an implicit
        // module build handles inferred modules. It adds an overlay VFS with
        // this file in the proper directory and relies on the rest of Clang to
        // handle it like normal. With explicitly built modules we don't need
        // to play VFS tricks, so replace it with the correct module map.
        if (IF.getFile()->getName().endswith("__inferred_module.map")) {
          MD.FileDeps.insert(ModuleMap->getName());
          return;
        }
        MD.FileDeps.insert(IF.getFile()->getName());
      });

  // We usually don't need to list the module map files of our dependencies when
  // building a module explicitly: their semantics will be deserialized from PCM
  // files.
  //
  // However, some module maps loaded implicitly during the dependency scan can
  // describe anti-dependencies. That happens when this module, let's call it
  // M1, is marked as '[no_undeclared_includes]' and tries to access a header
  // "M2/M2.h" from another module, M2, but doesn't have a 'use M2;'
  // declaration. The explicit build needs the module map for M2 so that it
  // knows that textually including "M2/M2.h" is not allowed.
  // E.g., '__has_include("M2/M2.h")' should return false, but without M2's
  // module map the explicit build would return true.
  //
  // An alternative approach would be to tell the explicit build what its
  // textual dependencies are, instead of having it re-discover its
  // anti-dependencies. For example, we could create and use an `-ivfs-overlay`
  // with `fall-through: false` that explicitly listed the dependencies.
  // However, that's more complicated to implement and harder to reason about.
  if (M->NoUndeclaredIncludes) {
    // We don't have a good way to determine which module map described the
    // anti-dependency (let alone what's the corresponding top-level module
    // map). We simply specify all the module maps in the order they were loaded
    // during the implicit build during scan.
    // TODO: Resolve this by serializing and only using Module::UndeclaredUses.
    MDC.ScanInstance.getASTReader()->visitTopLevelModuleMaps(
        *MF, [&](const FileEntry *FE) {
          if (FE->getName().endswith("__inferred_module.map"))
            return;
          // The top-level modulemap of this module will be the input file. We
          // don't need to specify it as a module map.
          if (FE == ModuleMap)
            return;
          MD.ModuleMapFileDeps.push_back(FE->getName().str());
        });
  }

  // Add direct prebuilt module dependencies now, so that we can use them when
  // creating a CompilerInvocation and computing context hash for this
  // ModuleDeps instance.
  llvm::DenseSet<const Module *> SeenModules;
  addAllSubmodulePrebuiltDeps(M, MD, SeenModules);

  MD.BuildInvocation = MDC.makeInvocationForModuleBuildWithoutPaths(
      MD, [&](CompilerInvocation &BuildInvocation) {
        if (MDC.OptimizeArgs)
          optimizeHeaderSearchOpts(BuildInvocation.getHeaderSearchOpts(),
                                   *MDC.ScanInstance.getASTReader(), *MF);
      });
  MD.ID.ContextHash = MD.BuildInvocation.getModuleHash();

  llvm::DenseSet<const Module *> AddedModules;
  addAllSubmoduleDeps(M, MD, AddedModules);

  return MD.ID;
}

static void forEachSubmoduleSorted(const Module *M,
                                   llvm::function_ref<void(const Module *)> F) {
  // Submodule order depends on order of header includes for inferred submodules
  // we don't care about the exact order, so sort so that it's consistent across
  // TUs to improve sharing.
  SmallVector<const Module *> Submodules(M->submodule_begin(),
                                         M->submodule_end());
  llvm::stable_sort(Submodules, [](const Module *A, const Module *B) {
    return A->Name < B->Name;
  });
  for (const Module *SubM : Submodules)
    F(SubM);
}

void ModuleDepCollectorPP::addAllSubmodulePrebuiltDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &SeenSubmodules) {
  addModulePrebuiltDeps(M, MD, SeenSubmodules);

  forEachSubmoduleSorted(M, [&](const Module *SubM) {
    addAllSubmodulePrebuiltDeps(SubM, MD, SeenSubmodules);
  });
}

void ModuleDepCollectorPP::addModulePrebuiltDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &SeenSubmodules) {
  for (const Module *Import : M->Imports)
    if (Import->getTopLevelModule() != M->getTopLevelModule())
      if (MDC.isPrebuiltModule(Import->getTopLevelModule()))
        if (SeenSubmodules.insert(Import->getTopLevelModule()).second)
          MD.PrebuiltModuleDeps.emplace_back(Import->getTopLevelModule());
}

void ModuleDepCollectorPP::addAllSubmoduleDeps(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  addModuleDep(M, MD, AddedModules);

  forEachSubmoduleSorted(M, [&](const Module *SubM) {
    addAllSubmoduleDeps(SubM, MD, AddedModules);
  });
}

void ModuleDepCollectorPP::addModuleDep(
    const Module *M, ModuleDeps &MD,
    llvm::DenseSet<const Module *> &AddedModules) {
  for (const Module *Import : M->Imports) {
    if (Import->getTopLevelModule() != M->getTopLevelModule() &&
        !MDC.isPrebuiltModule(Import)) {
      ModuleID ImportID = handleTopLevelModule(Import->getTopLevelModule());
      if (AddedModules.insert(Import->getTopLevelModule()).second)
        MD.ClangModuleDeps.push_back(ImportID);
    }
  }
}

ModuleDepCollector::ModuleDepCollector(
    std::unique_ptr<DependencyOutputOptions> Opts,
    CompilerInstance &ScanInstance, DependencyConsumer &C,
    CompilerInvocation &&OriginalCI, bool OptimizeArgs)
    : ScanInstance(ScanInstance), Consumer(C), Opts(std::move(Opts)),
      OriginalInvocation(std::move(OriginalCI)), OptimizeArgs(OptimizeArgs) {}

void ModuleDepCollector::attachToPreprocessor(Preprocessor &PP) {
  PP.addPPCallbacks(std::make_unique<ModuleDepCollectorPP>(*this));
}

void ModuleDepCollector::attachToASTReader(ASTReader &R) {}

bool ModuleDepCollector::isPrebuiltModule(const Module *M) {
  std::string Name(M->getTopLevelModuleName());
  const auto &PrebuiltModuleFiles =
      ScanInstance.getHeaderSearchOpts().PrebuiltModuleFiles;
  auto PrebuiltModuleFileIt = PrebuiltModuleFiles.find(Name);
  if (PrebuiltModuleFileIt == PrebuiltModuleFiles.end())
    return false;
  assert("Prebuilt module came from the expected AST file" &&
         PrebuiltModuleFileIt->second == M->getASTFile()->getName());
  return true;
}
