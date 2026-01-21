#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

static std::shared_ptr<SourceMgr> initializeSrcMgr(const std::string &src) {
    // Configure Source Manager
    auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

    auto buffer = MemoryBuffer::getMemBuffer(src);
    sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

    return sourceMgr;
}

static std::unique_ptr<Lexer> initializeLexer(const std::shared_ptr<SourceMgr> &sourceMgr) {
    auto curLexer = std::make_unique<Lexer>(sourceMgr);
    curLexer->scanAll();

    return curLexer;
}

static std::unique_ptr<Parser> initializeParser(std::unique_ptr<Lexer> lexer) {
    auto curParser = std::make_unique<Parser>(lexer->getTokens());
    curParser->parse();

    return curParser;
}

static Codegen *initializeCodegen(std::unique_ptr<Parser> parser, const std::shared_ptr<SourceMgr> &srcMgr) {
    auto *codegen = new Codegen(std::move(parser), srcMgr, __FILE__, {}, true, true);
    codegen->run();

    return codegen;
}


llvm::SMRange getRange(const char *bufferStart, int x, int y) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {llvm::SMLoc::getFromPointer(bufferStart + x), llvm::SMLoc::getFromPointer(bufferStart + y)};
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

class BaseTest : public ::testing::Test {
public:
    std::shared_ptr<SourceMgr> srcMgr;
    std::string source =
            "var y: int = 100\n"
            "y = 101\n";

    void SetUp() override {
        srcMgr = initializeSrcMgr(source);
    }
    void TearDown() override {
        // Place any cleanup code here, if needed
    }
};

class LexerTest : public BaseTest {
public:
    std::unique_ptr<Lexer> lexer;

    void SetUp() override {
        BaseTest::SetUp();

        lexer = initializeLexer(BaseTest::srcMgr);
    }
    void TearDown() override {
        BaseTest::TearDown();
    }
};

class ParserTest : public LexerTest {
public:
    std::unique_ptr<Parser> parser;

    void SetUp() override {
        LexerTest::SetUp();

        parser = initializeParser(std::move(LexerTest::lexer));
    }

    void TearDown() override {
        LexerTest::TearDown();
    }
};

class CodegenTest : public ParserTest {
public:
    Codegen *codegen = nullptr;

    void SetUp() override {
        ParserTest::SetUp();

        codegen = initializeCodegen(std::move(ParserTest::parser), LexerTest::srcMgr);
    }

    void TearDown() override {
        ParserTest::TearDown();
    }
};


TEST_F(LexerTest, Tokens) {
    EXPECT_TRUE(lexer->getTokens().size() > 1);

    // Get the buffer start pointer from the SourceMgr (not the local source string)
    const char *bufStart = srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())->getBufferStart();

    std::vector<std::unique_ptr<Token>> expectedTokens;
    expectedTokens.push_back(std::make_unique<Token>(TokenType::VAR, "var", getRange(bufStart, 0, 3)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::IDENTIFIER, "y", getRange(bufStart, 4, 5)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::COLON, ":", getRange(bufStart, 5, 6)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::INT_TYPE, "int", getRange(bufStart, 7, 10)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::EQUAL, "=", getRange(bufStart, 11, 12)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::INTEGER, "100", getRange(bufStart, 13, 16)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE", getRange(bufStart, 16, 17)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::IDENTIFIER, "y", getRange(bufStart, 17, 18)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::EQUAL, "=", getRange(bufStart, 19, 20)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::INTEGER, "101", getRange(bufStart, 21, 24)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE", getRange(bufStart, 24, 25)));
    expectedTokens.push_back(std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF", getRange(bufStart, 24, 25)));

    auto actualTokens = lexer->getTokens();
    ASSERT_EQ(expectedTokens.size(), actualTokens.size());

    for (size_t i = 0; i < expectedTokens.size(); i++) {
        EXPECT_EQ(*expectedTokens[i], *actualTokens[i]);
    }
}

TEST_F(ParserTest, AST) {
    EXPECT_EQ(parser->getAST()->getChildren().size(), 2);
    EXPECT_EQ(parser->getAST()->getChildren().at(0)->toString(srcMgr.get(), "", true), "└──VarDecl[Line(1-1):Col(1-17)]: y: int = 100\n");
    EXPECT_EQ(parser->getAST()->getChildren().at(1)->toString(srcMgr.get(), "", true), "└──Assignment[Line(2-2):Col(1-8)]: y EQUAL 101\n");
}

// We cannot return from top-level, and the exit function just exits the whole process including the test
TEST_F(CodegenTest, Run) {
    codegen->optimize(OptimizationLevel::O3);
    codegen->prepareJit();
    int exitCode = codegen->executeJit();

    EXPECT_TRUE(exitCode == 0);
}

// Google Test main function
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}