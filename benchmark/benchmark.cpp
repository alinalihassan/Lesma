#include <iterator>
#include <memory>
#include <utility>

#include <benchmark/benchmark.h>

#include <llvm/Passes/OptimizationLevel.h>

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;

namespace {
auto InitializeSrcMgr(const std::string& src) -> std::shared_ptr<SourceMgr> {
  // Configure Source Manager
  auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

  auto buffer = MemoryBuffer::getMemBuffer(src);
  sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

  return sourceMgr;
}

[[maybe_unused]] auto InitializeSrcMgrFromFile(const std::string& file)
    -> std::shared_ptr<SourceMgr> {
  // Configure Source Manager
  auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

  auto buffer = llvm::MemoryBuffer::getFile(file);
  if (buffer.getError() != std::error_code()) {
    throw LesmaError(llvm::SMRange(), "Could not read file: {}", file);
  }

  sourceMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());

  return sourceMgr;
}

auto InitializeLexer(const std::shared_ptr<SourceMgr>& sourceMgr)
    -> std::shared_ptr<Lexer> {
  auto curLexer = std::make_shared<Lexer>(sourceMgr);
  curLexer->scanAll();

  return curLexer;
}

auto InitializeParser(const std::shared_ptr<Lexer>& lexer)
    -> std::shared_ptr<Parser> {
  auto curParser = std::make_shared<Parser>(lexer->getTokens());
  curParser->parse();

  return curParser;
}

auto InitializeCodegen(std::shared_ptr<Parser> parser,
                       const std::shared_ptr<SourceMgr>& srcMgr)
    -> std::unique_ptr<Codegen> {
  Typechecker typechecker;
  typechecker.run(parser->getAst());
  auto takenTypeCache = typechecker.takeTypeCache();
  auto takenRootScope = typechecker.takeRootScope();
  auto codegen = std::make_unique<Codegen>(std::move(parser), srcMgr, __FILE__,
                                           std::vector<std::string>{}, true, true, "", nullptr,
                                           nullptr, nullptr,
                                           std::move(takenRootScope),
                                           std::move(takenTypeCache),
                                           typechecker.takeSpecializedTypeEnv(),
                                           typechecker.takeSpecializedTypeToTemplate(),
                                           typechecker.takeSpecializedClassTypes());
  codegen->run();

  return codegen;
}

[[maybe_unused]] auto GetRange(const std::string& source, int x, int y)
    -> llvm::SMRange {
  return {llvm::SMLoc::getFromPointer(std::next(source.c_str(), x)),
          llvm::SMLoc::getFromPointer(std::next(source.c_str(), y))};
}

} // namespace

class LexerBenchmark : public benchmark::Fixture {
protected:
  std::shared_ptr<SourceMgr> srcMgr;
  std::string source = "var y: int = 100\n"
                       "y = 101\n";

  void SetUp(__attribute__((unused)) const ::benchmark::State& _) override {
    srcMgr = InitializeSrcMgr(source);
  }

  void TearDown(__attribute__((unused)) const ::benchmark::State& _) override {
    // Place any cleanup code here, if needed
  }
};

class ParserBenchmark : public LexerBenchmark {
protected:
  std::shared_ptr<Lexer> lexer;

  void SetUp(const ::benchmark::State& state) override {
    LexerBenchmark::SetUp(state);
    lexer = InitializeLexer(srcMgr);
  }
  void TearDown(const ::benchmark::State& state) override {
    // Place any cleanup code here, if needed
    LexerBenchmark::TearDown(state);
  }
};

class CodegenBenchmark : public ParserBenchmark {
protected:
  std::shared_ptr<Parser> parser;

  void SetUp(const ::benchmark::State& state) override {
    ParserBenchmark::SetUp(state);

    parser = InitializeParser(ParserBenchmark::lexer);
  }

  void TearDown(const ::benchmark::State& state) override {
    ParserBenchmark::TearDown(state);
  }
};

BENCHMARK_F(LexerBenchmark, Lexer)
(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    InitializeLexer(srcMgr);
  }
}

BENCHMARK_F(ParserBenchmark, Parser)
(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    InitializeParser(lexer);
  }
}

BENCHMARK_F(CodegenBenchmark, Initialize)
(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    InitializeCodegen(parser, srcMgr);
  }
}

BENCHMARK_F(CodegenBenchmark, Optimize)
(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    auto cg = InitializeCodegen(parser, srcMgr);
    cg->optimize(llvm::OptimizationLevel::O3);
  }
}

BENCHMARK_F(CodegenBenchmark, JIT)
(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    auto cg = InitializeCodegen(parser, srcMgr);
    cg->optimize(llvm::OptimizationLevel::O3);
    cg->prepareJit();
    cg->executeJit();
  }
}

BENCHMARK_F(CodegenBenchmark, All)
(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    auto cg = InitializeCodegen(parser, srcMgr);
    cg->optimize(llvm::OptimizationLevel::O3);
    cg->prepareJit();
    cg->executeJit();
  }
}