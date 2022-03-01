#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include <catch2/catch.hpp>

#include <fmt/printf.h>
#include "Frontend/Lexer.h"
#include "Common/Utils.h"
#include "Frontend/Parser.h"
#include "Backend/Codegen.h"

using namespace lesma;

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

Codegen *initializeCodegen(Parser *parser) {
    std::unique_ptr<Parser> parser_pointer;
    parser_pointer.reset(parser);
    auto codegen = new Codegen(std::move(parser_pointer), "");
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
            Token{TokenType::VAR, "var", SourceLocation{0,3}},
            Token{TokenType::IDENTIFIER, "y", SourceLocation{0,5}},
            Token{TokenType::COLON, ":", SourceLocation{0,6}},
            Token{TokenType::INT_TYPE, "int", SourceLocation{0,10}},
            Token{TokenType::EQUAL, "=", SourceLocation{0,12}},
            Token{TokenType::INTEGER, "100", SourceLocation{0,16}},
            Token{TokenType::NEWLINE, "NEWLINE", SourceLocation{1,0}},
            Token{TokenType::IDENTIFIER, "y", SourceLocation{1,1}},
            Token{TokenType::EQUAL, "=", SourceLocation{1,3}},
            Token{TokenType::INTEGER, "101", SourceLocation{1,7}},
            Token{TokenType::NEWLINE, "NEWLINE", SourceLocation{2,0}},
            Token{TokenType::IDENTIFIER, "exit", SourceLocation{2,4}},
            Token{TokenType::LEFT_PAREN, "(", SourceLocation{2,5}},
            Token{TokenType::IDENTIFIER, "y", SourceLocation{2,6}},
            Token{TokenType::RIGHT_PAREN, ")", SourceLocation{2,7}},
            Token{TokenType::NEWLINE, "NEWLINE", SourceLocation{3,0}},
            Token{TokenType::EOF_TOKEN, "EOF", SourceLocation{3,0}},
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

    REQUIRE(parser->getAST()->getBlock()->getChildren().size() == 3);
    REQUIRE(parser->getAST()->getBlock()->getChildren().at(0)->toString(0) == "VarDecl[1:1]: y: int = 100\n");
    REQUIRE(parser->getAST()->getBlock()->getChildren().at(1)->toString(0) == "Assignment[1:1]: y EQUAL 101\n");
    REQUIRE(parser->getAST()->getBlock()->getChildren().at(2)->toString(0) == "Expression[2:4]: exit(y)\n");

    BENCHMARK("Parser") {
        initializeParser(lexer);
    };
}

// We cannot return from top-level, and the exit function just exits the whole process including the test
TEST_CASE("Codegen", "Run & Optimize") {
    std::string source =
            "var y: int = 100\n"
            "y = 101\n";

    auto lexer = initializeLexer(source);
    auto parser = initializeParser(lexer);
    auto codegen = initializeCodegen(parser);
    codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
    int exit_code = codegen->JIT(std::move(codegen->Modules));

    REQUIRE(exit_code == 0);

    BENCHMARK("Codegen") {
        initializeCodegen(parser);
    };

    BENCHMARK("Codegen & Optimize") {
        auto cg = initializeCodegen(parser);
        cg->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
    };

    BENCHMARK("Codegen & JIT") {
        auto cg = initializeCodegen(parser);
        cg->JIT(std::move(codegen->Modules));
    };

    BENCHMARK("Codegen, Optimize & JIT") {
        auto cg = initializeCodegen(parser);
        cg->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
        cg->JIT(std::move(codegen->Modules));
    };
}