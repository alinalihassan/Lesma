#include "liblesma/Common/ExportDiscovery.h"

#include <filesystem>
#include <memory>
#include <utility>

#include "llvm/Support/MemoryBuffer.h"

#include "fmt/format.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

namespace lesma {

auto discoverExportedTopLevelNames(const std::string& filepath, bool isStd,
                                     const std::string& mainFilePath) -> ExportDiscoveryResult {
  std::string const absolutePath =
      isStd ? filepath
            : fmt::format("{}/{}", std::filesystem::absolute(mainFilePath).parent_path().string(),
                          filepath);
  auto buffer = llvm::MemoryBuffer::getFile(absolutePath);
  if (!buffer) {
    return ExportDiscoveryResult{
        .names = {},
        .failureMessage = fmt::format("Could not read file: {}", absolutePath),
    };
  }

  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  try {
    auto lexer = std::make_unique<Lexer>(srcMgr);
    lexer->scanAll();
    auto pars = std::make_unique<Parser>(lexer->getTokens(), nullptr, srcMgr,
                                         srcMgr->getNumBuffers(), absolutePath);
    pars->parse();
    Compound* ast = pars->getAst();
    if (ast == nullptr) {
      return ExportDiscoveryResult{
          .names = {},
          .failureMessage = fmt::format("Could not parse file: {}", absolutePath),
      };
    }

    std::vector<std::string> out;
    for (Statement* stmt : ast->getChildren()) {
      if (auto* f = dynamic_cast<FuncDecl*>(stmt)) {
        if (f->isExported()) {
          out.push_back(f->getName());
        }
      } else if (auto* c = dynamic_cast<Class*>(stmt)) {
        if (c->isExported()) {
          out.push_back(c->getIdentifier());
        }
      } else if (auto* e = dynamic_cast<Enum*>(stmt)) {
        if (e->isExported()) {
          out.push_back(e->getIdentifier());
        }
      } else if (auto* ef = dynamic_cast<ExternFuncDecl*>(stmt)) {
        if (ef->isExported()) {
          out.push_back(ef->getName());
        }
      } else if (auto* tr = dynamic_cast<TraitDecl*>(stmt)) {
        if (tr->isExported()) {
          out.push_back(tr->getIdentifier());
        }
      } else if (auto* vd = dynamic_cast<VarDecl*>(stmt)) {
        if (vd->isExported()) {
          for (Literal* lit : vd->getVarLiterals()) {
            out.push_back(lit->getValue());
          }
        }
      }
    }

    return ExportDiscoveryResult{.names = std::move(out), .failureMessage = {}};
  } catch (const LesmaError& err) {
    return ExportDiscoveryResult{
        .names = {},
        .failureMessage =
            fmt::format("Could not parse file {}: {}", absolutePath, std::string(err.what())),
    };
  }
}

auto getExportedTopLevelNamesFromFile(const std::string& filepath, bool isStd,
                                      const std::string& mainFilePath) -> std::vector<std::string> {
  ExportDiscoveryResult const r = discoverExportedTopLevelNames(filepath, isStd, mainFilePath);
  return r.names;
}

} // namespace lesma
