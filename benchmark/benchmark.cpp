#include <benchmark/benchmark.h>

#include <utility>

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

using namespace lesma;

static std::shared_ptr<SourceMgr> initializeSrcMgr(const std::string &src) {
    // Configure Source Manager
    auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

    auto buffer = MemoryBuffer::getMemBuffer(src);
    sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

    return sourceMgr;
}

static std::shared_ptr<Lexer> initializeLexer(const std::shared_ptr<SourceMgr> &sourceMgr) {
    auto curLexer = std::make_shared<Lexer>(sourceMgr);
    curLexer->ScanAll();

    return curLexer;
}

static std::shared_ptr<Parser> initializeParser(const std::shared_ptr<Lexer> &lexer) {
    auto curParser = std::make_shared<Parser>(lexer->getTokens());
    curParser->Parse();

    return curParser;
}

static Codegen *initializeCodegen(std::shared_ptr<Parser> parser, const std::shared_ptr<SourceMgr> &srcMgr) {
    auto _codegen = new Codegen(std::move(parser), srcMgr, __FILE__, {}, true, true);
    _codegen->Run();

    return _codegen;
}


llvm::SMRange getRange(const std::string &source, int x, int y) {
    return {llvm::SMLoc::getFromPointer(source.c_str() + x), llvm::SMLoc::getFromPointer(source.c_str() + y)};
}

class LexerBenchmark : public benchmark::Fixture {
protected:
    std::shared_ptr<SourceMgr> srcMgr;
    std::string source =
            "var y: int = 100\n"
            "y = 101\n"
            "exit(y)\n";

    void SetUp(const ::benchmark::State &state) override {
        srcMgr = initializeSrcMgr(source);
    }

    void TearDown(const ::benchmark::State &state) override {
        // Place any cleanup code here, if needed
    }
};

class ParserBenchmark : public LexerBenchmark {
protected:
    std::shared_ptr<Lexer> lexer;

    void SetUp(const ::benchmark::State &state) override {
        LexerBenchmark::SetUp(state);
        lexer = initializeLexer(srcMgr);
    }
    void TearDown(const ::benchmark::State &state) override {
        // Place any cleanup code here, if needed
        LexerBenchmark::TearDown(state);
    }
};

class CodegenBenchmark : public ParserBenchmark {
protected:
    std::shared_ptr<Parser> parser;

    void SetUp(const ::benchmark::State &state) override {
        ParserBenchmark::SetUp(state);

        parser = initializeParser(ParserBenchmark::lexer);
    }

    void TearDown(const ::benchmark::State &state) override {
        ParserBenchmark::TearDown(state);
    }
};

BENCHMARK_F(LexerBenchmark, Lexer)
(benchmark::State &state) {
    for (auto _: state) {
        initializeLexer(srcMgr);
    }
}

BENCHMARK_F(ParserBenchmark, Parser)
(benchmark::State &state) {
    for (auto _: state) {
        initializeParser(lexer);
    }
}

BENCHMARK_F(CodegenBenchmark, Initialize)
(benchmark::State &state) {
    for (auto _: state) {
        initializeCodegen(parser, srcMgr);
    }
}

BENCHMARK_F(CodegenBenchmark, Optimize)
(benchmark::State &state) {
    for (auto _: state) {
        auto cg = initializeCodegen(parser, srcMgr);
        cg->Optimize(OptimizationLevel::O3);
    }
}

BENCHMARK_F(CodegenBenchmark, JIT)
(benchmark::State &state) {
    for (auto _: state) {
        auto cg = initializeCodegen(parser, srcMgr);
        cg->PrepareJIT();
        cg->ExecuteJIT();
    }
}

BENCHMARK_F(CodegenBenchmark, All)
(benchmark::State &state) {
    for (auto _: state) {
        auto cg = initializeCodegen(parser, srcMgr);
        cg->PrepareJIT();
        cg->ExecuteJIT();
    }
}

BENCHMARK_MAIN();