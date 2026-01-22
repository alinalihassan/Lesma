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

namespace {
auto InitializeSrcMgr(const std::string& src) -> std::shared_ptr<SourceMgr> {
  // Configure Source Manager
  auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

  auto buffer = MemoryBuffer::getMemBuffer(src);
  sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

  return sourceMgr;
}

auto InitializeLexer(const std::shared_ptr<SourceMgr>& sourceMgr)
    -> std::unique_ptr<Lexer> {
  auto curLexer = std::make_unique<Lexer>(sourceMgr);
  curLexer->ScanAll();

  return curLexer;
}

auto InitializeParser(std::unique_ptr<Lexer> lexer) -> std::unique_ptr<Parser> {
  auto curParser = std::make_unique<Parser>(lexer->GetTokens());
  curParser->Parse();

  return curParser;
}

auto InitializeCodegen(std::unique_ptr<Parser> parser,
                       const std::shared_ptr<SourceMgr>& srcMgr)
    -> std::unique_ptr<Codegen> {
  auto codegen =
      std::make_unique<Codegen>(std::move(parser), srcMgr, __FILE__,
                                std::vector<std::string>{}, true, true);
  codegen->Run();

  return codegen;
}

auto GetRange(const char* bufferStart, int x, int y) -> llvm::SMRange {
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return {llvm::SMLoc::getFromPointer(bufferStart + x),
          llvm::SMLoc::getFromPointer(bufferStart + y)};
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

class BaseTest : public ::testing::Test {
public:
  std::shared_ptr<SourceMgr> srcMgr;
  std::string source = "var y: int = 100\n"
                       "y = 101\n";

  auto SetUp() -> void override { srcMgr = InitializeSrcMgr(source); }
  auto TearDown() -> void override {
    // Place any cleanup code here, if needed
  }
};

class LexerTest : public BaseTest {
public:
  std::unique_ptr<Lexer> lexer;

  auto SetUp() -> void override {
    BaseTest::SetUp();

    lexer = InitializeLexer(BaseTest::srcMgr);
  }
  auto TearDown() -> void override { BaseTest::TearDown(); }
};

class ParserTest : public LexerTest {
public:
  std::unique_ptr<Parser> parser;

  auto SetUp() -> void override {
    LexerTest::SetUp();

    parser = InitializeParser(std::move(LexerTest::lexer));
  }

  auto TearDown() -> void override { LexerTest::TearDown(); }
};

class CodegenTest : public ParserTest {
public:
  std::unique_ptr<Codegen> codegen;

  auto SetUp() -> void override {
    ParserTest::SetUp();

    codegen =
        InitializeCodegen(std::move(ParserTest::parser), LexerTest::srcMgr);
  }

  auto TearDown() -> void override { ParserTest::TearDown(); }
};

TEST_F(LexerTest, Tokens) {
  EXPECT_TRUE(lexer->GetTokens().size() > 1);

  // Get the buffer start pointer from the SourceMgr (not the local source
  // string)
  const char* bufStart =
      srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())->getBufferStart();

  std::vector<std::unique_ptr<Token>> expectedTokens;
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::VAR, "var", GetRange(bufStart, 0, 3)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::IDENTIFIER, "y",
                                                   GetRange(bufStart, 4, 5)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::COLON, ":", GetRange(bufStart, 5, 6)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::INT_TYPE, "int",
                                                   GetRange(bufStart, 7, 10)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::EQUAL, "=",
                                                   GetRange(bufStart, 11, 12)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::INTEGER, "100",
                                                   GetRange(bufStart, 13, 16)));
  expectedTokens.push_back(std::make_unique<Token>(
      TokenType::NEWLINE, "NEWLINE", GetRange(bufStart, 16, 17)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::IDENTIFIER, "y",
                                                   GetRange(bufStart, 17, 18)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::EQUAL, "=",
                                                   GetRange(bufStart, 19, 20)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::INTEGER, "101",
                                                   GetRange(bufStart, 21, 24)));
  expectedTokens.push_back(std::make_unique<Token>(
      TokenType::NEWLINE, "NEWLINE", GetRange(bufStart, 24, 25)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF",
                                                   GetRange(bufStart, 24, 25)));

  auto actualTokens = lexer->GetTokens();
  ASSERT_EQ(expectedTokens.size(), actualTokens.size());

  for (size_t i = 0; i < expectedTokens.size(); i++) {
    EXPECT_EQ(*expectedTokens[i], *actualTokens[i]);
  }
}

TEST_F(ParserTest, AST) {
  EXPECT_EQ(parser->GetAst()->GetChildren().size(), 2);
  EXPECT_EQ(
      parser->GetAst()->GetChildren().at(0)->ToString(srcMgr.get(), "", true),
      "└──VarDecl[Line(1-1):Col(1-17)]: y: int = 100\n");
  EXPECT_EQ(
      parser->GetAst()->GetChildren().at(1)->ToString(srcMgr.get(), "", true),
      "└──Assignment[Line(2-2):Col(1-8)]: y EQUAL 101\n");
}

// We cannot return from top-level, and the exit function just exits the whole
// process including the test
TEST_F(CodegenTest, Run) {
  codegen->Optimize(OptimizationLevel::O3);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();

  EXPECT_TRUE(exitCode == 0);
}

// ============================================================================
// Additional Lexer Tests
// ============================================================================

TEST(LexerTests, LexFloat) {
  auto srcMgr = InitializeSrcMgr("var x = 3.14\n");
  auto lexer = InitializeLexer(srcMgr);
  auto tokens = lexer->GetTokens();

  ASSERT_GE(tokens.size(), 5);
  EXPECT_EQ(tokens[0]->type, TokenType::VAR);
  EXPECT_EQ(tokens[1]->type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[2]->type, TokenType::EQUAL);
  EXPECT_EQ(tokens[3]->type, TokenType::DOUBLE);
  EXPECT_EQ(tokens[3]->lexeme, "3.14");
}

TEST(LexerTests, LexString) {
  auto srcMgr = InitializeSrcMgr("var s = \"hello\"\n");
  auto lexer = InitializeLexer(srcMgr);
  auto tokens = lexer->GetTokens();

  ASSERT_GE(tokens.size(), 5);
  EXPECT_EQ(tokens[3]->type, TokenType::STRING);
  EXPECT_EQ(tokens[3]->lexeme, "hello");
}

TEST(LexerTests, LexInteger) {
  auto srcMgr = InitializeSrcMgr("var x = 42\n");
  auto lexer = InitializeLexer(srcMgr);
  auto tokens = lexer->GetTokens();

  ASSERT_GE(tokens.size(), 5);
  EXPECT_EQ(tokens[3]->type, TokenType::INTEGER);
  EXPECT_EQ(tokens[3]->lexeme, "42");
}

TEST(LexerTests, LexBoolean) {
  auto srcMgr = InitializeSrcMgr("var x = true\n");
  auto lexer = InitializeLexer(srcMgr);
  auto tokens = lexer->GetTokens();

  ASSERT_GE(tokens.size(), 5);
  // 'true' is lexed as TRUE_ keyword, not BOOL literal
  EXPECT_EQ(tokens[3]->type, TokenType::TRUE_);
  EXPECT_EQ(tokens[3]->lexeme, "true");
}

TEST(LexerTests, LexArithmeticOperators) {
  auto srcMgr = InitializeSrcMgr("var x = 1 + 2\n");
  auto lexer = InitializeLexer(srcMgr);
  auto tokens = lexer->GetTokens();

  EXPECT_EQ(tokens[4]->type, TokenType::PLUS);
}

TEST(LexerTests, LexComparisonOperators) {
  auto srcMgr = InitializeSrcMgr("var x = 1 == 2\n");
  auto lexer = InitializeLexer(srcMgr);
  auto tokens = lexer->GetTokens();

  EXPECT_EQ(tokens[4]->type, TokenType::EQUAL_EQUAL);
}

// ============================================================================
// Additional Parser Tests
// ============================================================================

TEST(ParserTests, ParseBinaryOp) {
  auto srcMgr = InitializeSrcMgr("var x = 1 + 2\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));

  ASSERT_EQ(parser->GetAst()->GetChildren().size(), 1);
  auto astStr =
      parser->GetAst()->GetChildren().at(0)->ToString(srcMgr.get(), "", true);
  // BinaryOp toString returns "left OP right", so check for the operator
  EXPECT_TRUE(astStr.find("PLUS") != std::string::npos);
}

TEST(ParserTests, ParseUnaryMinus) {
  auto srcMgr = InitializeSrcMgr("var x = -5\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));

  ASSERT_EQ(parser->GetAst()->GetChildren().size(), 1);
  auto astStr =
      parser->GetAst()->GetChildren().at(0)->ToString(srcMgr.get(), "", true);
  // UnaryOp toString returns "OP expr", so check for MINUS
  EXPECT_TRUE(astStr.find("MINUS") != std::string::npos);
}

TEST(ParserTests, ParseVarDecl) {
  auto srcMgr = InitializeSrcMgr("var x: int = 10\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));

  ASSERT_EQ(parser->GetAst()->GetChildren().size(), 1);
  auto astStr =
      parser->GetAst()->GetChildren().at(0)->ToString(srcMgr.get(), "", true);
  EXPECT_TRUE(astStr.find("VarDecl") != std::string::npos);
}

TEST(ParserTests, ParseLetDecl) {
  auto srcMgr = InitializeSrcMgr("let x = 10\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));

  ASSERT_EQ(parser->GetAst()->GetChildren().size(), 1);
  auto astStr =
      parser->GetAst()->GetChildren().at(0)->ToString(srcMgr.get(), "", true);
  EXPECT_TRUE(astStr.find("VarDecl") != std::string::npos);
}

// ============================================================================
// Additional Codegen Tests
// ============================================================================

TEST(CodegenTests, Arithmetic) {
  auto srcMgr = InitializeSrcMgr("var x = 2 + 3 * 4\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, FloatLiteral) {
  auto srcMgr = InitializeSrcMgr("var x: float = 3.14\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, StringLiteral) {
  auto srcMgr = InitializeSrcMgr("var s = \"hello\"\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, BooleanLiteral) {
  auto srcMgr = InitializeSrcMgr("var b = true\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, UnaryMinus) {
  auto srcMgr = InitializeSrcMgr("var x = -42\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, UnaryNot) {
  auto srcMgr = InitializeSrcMgr("var x = not true\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, Comparison) {
  auto srcMgr = InitializeSrcMgr("var x = 5 > 3\n");
  auto lexer = InitializeLexer(srcMgr);
  auto parser = InitializeParser(std::move(lexer));
  auto codegen = InitializeCodegen(std::move(parser), srcMgr);

  codegen->Optimize(OptimizationLevel::O0);
  codegen->PrepareJit();
  int exitCode = codegen->ExecuteJit();
  EXPECT_EQ(exitCode, 0);
}
} // namespace

// Google Test main function
auto main(int argc, char** argv) -> int {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
