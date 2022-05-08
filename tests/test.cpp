#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include <catch2/catch.hpp>

#include <fmt/printf.h>
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Backend/Codegen.h"

using namespace lesma;

std::shared_ptr<SourceMgr> initializeSrcMgr(const std::string& source) {
    // Configure Source Manager
    std::shared_ptr<SourceMgr> srcMgr = std::make_shared<SourceMgr>(SourceMgr());

    auto buffer = MemoryBuffer::getMemBuffer(source);
    srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

    return srcMgr;
}

Lexer *initializeLexer(const std::string& source) {
    auto lexer = new Lexer(source, "");
    lexer->ScanAll();

    return lexer;
}

Parser *initializeParser(Lexer *lexer) {
    auto parser = new Parser(lexer->getTokens());
    parser->Parse();

    return parser;
}

Codegen *initializeCodegen(Parser *parser, std::shared_ptr<SourceMgr> srcMgr) {
    std::unique_ptr<Parser> parser_pointer;
    parser_pointer.reset(parser);
    auto codegen = new Codegen(std::move(parser_pointer), std::move(srcMgr), __FILE__, true, true);
    codegen->Run();

    return codegen;
}

TEST_CASE("Lexer", "Tokens" ) {
    std::string source =
            "var y: int = 100\n"
            "y = 101\n"
            "exit(y)\n";

    auto lexer = initializeLexer(source);

    REQUIRE(lexer->getTokens().size() > 1);

    std::vector<Token> tokens = {
            Token{TokenType::VAR, "var", Span{{1, 1}, {1, 4}}},
            Token{TokenType::IDENTIFIER, "y", Span{{1,5}, {1,6}}},
            Token{TokenType::COLON, ":", Span{{1,6}, {1,7}}},
            Token{TokenType::INT_TYPE, "int", Span{{1,8}, {1,11}}},
            Token{TokenType::EQUAL, "=", Span{{1,12}, {1,13}}},
            Token{TokenType::INTEGER, "100", Span{{1, 14}, {1,17}}},
            Token{TokenType::NEWLINE, "NEWLINE", Span{{1, 17}, {2, 1}}},
            Token{TokenType::IDENTIFIER, "y", Span{{2,1}, {2, 2}}},
            Token{TokenType::EQUAL, "=", Span{{2, 3}, {2,4}}},
            Token{TokenType::INTEGER, "101", Span{{2,5}, {2,8}}},
            Token{TokenType::NEWLINE, "NEWLINE", Span{{2,8}, {3,1}}},
            Token{TokenType::IDENTIFIER, "exit", Span{{3,1}, {3,5}}},
            Token{TokenType::LEFT_PAREN, "(", Span{{3,5}, {3,6}}},
            Token{TokenType::IDENTIFIER, "y", Span{{3,6}, {3,7}}},
            Token{TokenType::RIGHT_PAREN, ")", Span{{3,7}, {3,8}}},
            Token{TokenType::NEWLINE, "NEWLINE", Span{{3,8}, {4,1}}},
            Token{TokenType::EOF_TOKEN, "EOF", Span{{4,1}, {4,1}}},
    };

    BENCHMARK("Lexer") {
        initializeLexer(source);
    };

    REQUIRE(tokens == lexer->getTokens());
}

// Currently, all statement and expressions use the abstract base class Statement or Expression,
// which makes it difficult to test
TEST_CASE("Parser", "AST") {
    std::string source =
            "var y: int = 100\n"
            "y = 101\n"
            "exit(y)\n";

    auto lexer = initializeLexer(source);
    auto parser = initializeParser(lexer);

    REQUIRE(parser->getAST()->getChildren().size() == 3);
    REQUIRE(parser->getAST()->getChildren().at(0)->toString(0) == "VarDecl[Line(1-1):Col(1-17)]: y: int = 100\n");
    REQUIRE(parser->getAST()->getChildren().at(1)->toString(0) == "Assignment[Line(2-2):Col(1-8)]: y EQUAL 101\n");
    REQUIRE(parser->getAST()->getChildren().at(2)->toString(0) == "Expression[Line(3-3):Col(1-8)]: exit(y)\n");

    BENCHMARK("Parser") {
        initializeParser(lexer);
    };
}

// We cannot return from top-level, and the exit function just exits the whole process including the test
TEST_CASE("Codegen", "Run & Optimize") {
    std::string source =
            "var y: int = 100\n"
            "y = 101\n";

    auto srcMgr = initializeSrcMgr(source);
    auto lexer = initializeLexer(source);
    auto parser = initializeParser(lexer);
    auto codegen = initializeCodegen(parser, srcMgr);
    codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
    int exit_code = codegen->JIT();

    REQUIRE(exit_code == 0);

    BENCHMARK("Codegen") {
        initializeCodegen(parser, srcMgr);
    };

    BENCHMARK("Codegen & Optimize") {
        auto cg = initializeCodegen(parser, srcMgr);
        cg->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
    };

    BENCHMARK("Codegen & JIT") {
        auto cg = initializeCodegen(parser, srcMgr);
        cg->JIT();
    };

    BENCHMARK("Codegen, Optimize & JIT") {
        auto cg = initializeCodegen(parser, srcMgr);
        cg->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
        cg->JIT();
    };
}