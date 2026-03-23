#include <cstddef>
#include <filesystem>
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
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;

namespace {
auto initializeSrcMgr(const std::string& src) -> std::shared_ptr<SourceMgr> {
  // Configure Source Manager
  auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

  auto buffer = MemoryBuffer::getMemBuffer(src);
  sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

  return sourceMgr;
}

auto initializeLexer(const std::shared_ptr<SourceMgr>& sourceMgr)
    -> std::unique_ptr<Lexer> {
  auto curLexer = std::make_unique<Lexer>(sourceMgr);
  curLexer->scanAll();

  return curLexer;
}

auto initializeParser(std::unique_ptr<Lexer> lexer) -> std::unique_ptr<Parser> {
  auto curParser = std::make_unique<Parser>(lexer->getTokens());
  curParser->parse();

  return curParser;
}

auto initializeCodegen(std::unique_ptr<Parser> parser,
                       const std::shared_ptr<SourceMgr>& srcMgr)
    -> std::unique_ptr<Codegen> {
  Typechecker typechecker;
  typechecker.run(parser->getAst());
  auto codegen = std::make_unique<Codegen>(
      std::move(parser), srcMgr, __FILE__, std::vector<std::string>{}, true, true, "", nullptr,
      nullptr, nullptr, typechecker.takeRootScope(), typechecker.takeTypeCache());
  codegen->run();

  return codegen;
}

auto analyzeSource(const std::string& src) -> AnalysisResult {
  auto options = std::make_unique<Options>();
  options->sourceType = SourceType::STRING;
  options->source = src;
  options->implicitFilePath = "analysis_index_test.les";
  return analyze(std::move(options));
}

auto analyzeFile(const std::filesystem::path& path) -> AnalysisResult {
  auto options = std::make_unique<Options>();
  options->sourceType = SourceType::FILE;
  options->source = path.string();
  return analyze(std::move(options));
}

auto getRange(const char* bufferStart, int x, int y) -> llvm::SMRange {
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

  auto SetUp() -> void override { srcMgr = initializeSrcMgr(source); }
  auto TearDown() -> void override {
    // Place any cleanup code here, if needed
  }
};

class LexerTest : public BaseTest {
public:
  std::unique_ptr<Lexer> lexer;

  auto SetUp() -> void override {
    BaseTest::SetUp();

    lexer = initializeLexer(BaseTest::srcMgr);
  }
  auto TearDown() -> void override { BaseTest::TearDown(); }
};

class ParserTest : public LexerTest {
public:
  std::unique_ptr<Parser> parser;

  auto SetUp() -> void override {
    LexerTest::SetUp();

    parser = initializeParser(std::move(LexerTest::lexer));
  }

  auto TearDown() -> void override { LexerTest::TearDown(); }
};

class CodegenTest : public ParserTest {
public:
  std::unique_ptr<Codegen> codegen;

  auto SetUp() -> void override {
    ParserTest::SetUp();

    codegen =
        initializeCodegen(std::move(ParserTest::parser), LexerTest::srcMgr);
  }

  auto TearDown() -> void override { ParserTest::TearDown(); }
};

TEST_F(LexerTest, Tokens) {
  EXPECT_TRUE(lexer->getTokens().size() > 1);

  // Get the buffer start pointer from the SourceMgr (not the local source
  // string)
  const char* bufStart =
      srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())->getBufferStart();

  std::vector<std::unique_ptr<Token>> expectedTokens;
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::VAR, "var", getRange(bufStart, 0, 3)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::IDENTIFIER, "y",
                                                   getRange(bufStart, 4, 5)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::COLON, ":", getRange(bufStart, 5, 6)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::INT_TYPE, "int",
                                                   getRange(bufStart, 7, 10)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::EQUAL, "=",
                                                   getRange(bufStart, 11, 12)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::INTEGER, "100",
                                                   getRange(bufStart, 13, 16)));
  expectedTokens.push_back(std::make_unique<Token>(
      TokenType::NEWLINE, "NEWLINE", getRange(bufStart, 16, 17)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::IDENTIFIER, "y",
                                                   getRange(bufStart, 17, 18)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::EQUAL, "=",
                                                   getRange(bufStart, 19, 20)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::INTEGER, "101",
                                                   getRange(bufStart, 21, 24)));
  expectedTokens.push_back(std::make_unique<Token>(
      TokenType::NEWLINE, "NEWLINE", getRange(bufStart, 24, 25)));
  expectedTokens.push_back(std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF",
                                                   getRange(bufStart, 24, 25)));

  auto actualTokens = lexer->getTokens();
  ASSERT_EQ(expectedTokens.size(), actualTokens.size());

  for (size_t i = 0; i < expectedTokens.size(); i++) {
    EXPECT_EQ(*expectedTokens[i], *actualTokens[i]);
  }
}

TEST_F(ParserTest, AST) {
  EXPECT_EQ(parser->getAst()->getChildren().size(), 2);
  EXPECT_EQ(
      parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true),
      "└──VarDecl[Line(1-1):Col(1-17)]: y: int = 100\n");
  EXPECT_EQ(
      parser->getAst()->getChildren().at(1)->toString(srcMgr.get(), "", true),
      "└──Assignment[Line(2-2):Col(1-8)]: y EQUAL 101\n");
}

// We cannot return from top-level, and the exit function just exits the whole
// process including the test
TEST_F(CodegenTest, Run) {
  codegen->optimize(OptimizationLevel::O3);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();

  EXPECT_TRUE(exitCode == 0);
}

// ============================================================================
// Additional Lexer Tests
// ============================================================================

TEST(LexerTests, LexFloat) {
  auto srcMgr = initializeSrcMgr("var x = 3.14\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  ASSERT_GE(tokens.size(), 5);
  EXPECT_EQ(tokens[0]->type, TokenType::VAR);
  EXPECT_EQ(tokens[1]->type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[2]->type, TokenType::EQUAL);
  EXPECT_EQ(tokens[3]->type, TokenType::DOUBLE);
  EXPECT_EQ(tokens[3]->lexeme, "3.14");
}

TEST(LexerTests, LexString) {
  auto srcMgr = initializeSrcMgr("var s = \"hello\"\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  ASSERT_GE(tokens.size(), 5);
  EXPECT_EQ(tokens[3]->type, TokenType::STRING);
  EXPECT_EQ(tokens[3]->lexeme, "hello");
}

TEST(LexerTests, LexInteger) {
  auto srcMgr = initializeSrcMgr("var x = 42\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  ASSERT_GE(tokens.size(), 5);
  EXPECT_EQ(tokens[3]->type, TokenType::INTEGER);
  EXPECT_EQ(tokens[3]->lexeme, "42");
}

TEST(LexerTests, LexBoolean) {
  auto srcMgr = initializeSrcMgr("var x = true\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  ASSERT_GE(tokens.size(), 5);
  // 'true' is lexed as TRUE_ keyword, not BOOL literal
  EXPECT_EQ(tokens[3]->type, TokenType::TRUE_);
  EXPECT_EQ(tokens[3]->lexeme, "true");
}

TEST(LexerTests, LexArithmeticOperators) {
  auto srcMgr = initializeSrcMgr("var x = 1 + 2\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  EXPECT_EQ(tokens[4]->type, TokenType::PLUS);
}

TEST(LexerTests, LexComparisonOperators) {
  auto srcMgr = initializeSrcMgr("var x = 1 == 2\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  EXPECT_EQ(tokens[4]->type, TokenType::EQUAL_EQUAL);
}

// ============================================================================
// Additional Parser Tests
// ============================================================================

TEST(ParserTests, ParseBinaryOp) {
  auto srcMgr = initializeSrcMgr("var x = 1 + 2\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr =
      parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  // BinaryOp toString returns "left OP right", so check for the operator
  EXPECT_TRUE(astStr.find("PLUS") != std::string::npos);
}

TEST(ParserTests, ParseUnaryMinus) {
  auto srcMgr = initializeSrcMgr("var x = -5\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr =
      parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  // UnaryOp toString returns "OP expr", so check for MINUS
  EXPECT_TRUE(astStr.find("MINUS") != std::string::npos);
}

TEST(ParserTests, ParseVarDecl) {
  auto srcMgr = initializeSrcMgr("var x: int = 10\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr =
      parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  EXPECT_TRUE(astStr.find("VarDecl") != std::string::npos);
}

TEST(ParserTests, ParseLetDecl) {
  auto srcMgr = initializeSrcMgr("let x = 10\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr =
      parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  EXPECT_TRUE(astStr.find("VarDecl") != std::string::npos);
}

// ============================================================================
// Additional Codegen Tests
// ============================================================================

TEST(CodegenTests, Arithmetic) {
  auto srcMgr = initializeSrcMgr("var x = 2 + 3 * 4\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, FloatLiteral) {
  auto srcMgr = initializeSrcMgr("var x: float = 3.14\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, StringLiteral) {
  auto srcMgr = initializeSrcMgr("var s = \"hello\"\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, BooleanLiteral) {
  auto srcMgr = initializeSrcMgr("var b = true\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, UnaryMinus) {
  auto srcMgr = initializeSrcMgr("var x = -42\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, UnaryNot) {
  auto srcMgr = initializeSrcMgr("var x = not true\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(CodegenTests, Comparison) {
  auto srcMgr = initializeSrcMgr("var x = 5 > 3\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));
  auto codegen = initializeCodegen(std::move(parser), srcMgr);

  codegen->optimize(OptimizationLevel::O0);
  codegen->prepareJit();
  int exitCode = codegen->executeJit();
  EXPECT_EQ(exitCode, 0);
}

TEST(AnalysisIndexTests, IndexesOnlyResolvedEnumMemberAccesses) {
  constexpr auto source = R"(enum Status
    READY

class Holder
    var ready: int

    def new(value: int)
        self.ready = value

var holder = Holder(1)
var propertyValue = holder.ready
var status: Status = Status.READY
)";

  AnalysisResult const result = analyzeSource(source);
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);

  bool sawHolderPropertyAccess = false;
  bool sawSelfPropertyAccess = false;
  for (const IndexedSymbolOccurrence& occurrence : result.index.symbolOccurrences) {
    if (occurrence.name != "ready" || !occurrence.isMemberAccess || occurrence.modifiers != 0U ||
        occurrence.fallbackTokenKind != IndexedTokenKind::Property) {
      continue;
    }
    if (occurrence.dotBase == std::optional<std::string>("holder")) {
      sawHolderPropertyAccess = true;
    }
    if (occurrence.dotBase == std::optional<std::string>("self")) {
      sawSelfPropertyAccess = true;
    }
  }
  EXPECT_TRUE(sawHolderPropertyAccess);
  EXPECT_TRUE(sawSelfPropertyAccess);

  bool sawStatusReady = false;
  for (const IndexedSymbolOccurrence& occurrence : result.index.symbolOccurrences) {
    if (occurrence.name == "ready" &&
        occurrence.fallbackTokenKind == IndexedTokenKind::EnumMember) {
      FAIL() << "property access was classified as enum member";
    }
    if (occurrence.name == "READY" &&
        occurrence.fallbackTokenKind == IndexedTokenKind::EnumMember) {
      sawStatusReady = true;
    }
  }
  EXPECT_TRUE(sawStatusReady);
}

TEST(AnalysisIndexTests, MemberAccessesCarryDeclarationIdentity) {
  constexpr auto source = R"(enum Status
    READY

class Holder
    var ready: int

    def new(value: int)
        self.ready = value

var holder = Holder(1)
var propertyValue = holder.ready
var status: Status = Status.READY
)";

  AnalysisResult const result = analyzeSource(source);
  ASSERT_FALSE(result.hasErrors());

  bool sawReadyDeclaration = false;
  bool sawReadyUsageDeclaration = false;
  bool sawEnumDeclaration = false;
  bool sawEnumUsageDeclaration = false;

  for (const IndexedSymbolOccurrence& occurrence : result.index.symbolOccurrences) {
    if (occurrence.name == "ready" && occurrence.fallbackTokenKind == IndexedTokenKind::Variable) {
      ASSERT_TRUE(occurrence.declaration.has_value());
      if ((occurrence.modifiers & analysis_index_modifier::DECLARATION) != 0U) {
        sawReadyDeclaration = true;
      }
    }
    if (occurrence.name == "ready" && occurrence.fallbackTokenKind == IndexedTokenKind::Property) {
      ASSERT_TRUE(occurrence.declaration.has_value());
      if ((occurrence.modifiers & analysis_index_modifier::DECLARATION) == 0U) {
        sawReadyUsageDeclaration = true;
      }
    }
    if (occurrence.name == "READY" && occurrence.fallbackTokenKind == IndexedTokenKind::EnumMember) {
      ASSERT_TRUE(occurrence.declaration.has_value());
      if ((occurrence.modifiers & analysis_index_modifier::DECLARATION) != 0U) {
        sawEnumDeclaration = true;
      } else {
        sawEnumUsageDeclaration = true;
      }
    }
  }

  EXPECT_TRUE(sawReadyDeclaration);
  EXPECT_TRUE(sawReadyUsageDeclaration);
  EXPECT_TRUE(sawEnumDeclaration);
  EXPECT_TRUE(sawEnumUsageDeclaration);
}

TEST(AnalysisIndexTests, NestedMemberAccessPreservesOuterReceiverName) {
  constexpr auto source = R"(class Payload
    var value: int

    def new(value: int)
        self.value = value

class Holder
    var payload: Payload

    def new(value: int)
        self.payload = Payload(value)

var holder = Holder(101)
var nestedValue = holder.payload.value
)";

  AnalysisResult const result = analyzeSource(source);
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);

  bool sawNestedValueAccess = false;
  for (const IndexedSymbolOccurrence& occurrence : result.index.symbolOccurrences) {
    if (occurrence.name != "value" || !occurrence.isMemberAccess ||
        occurrence.fallbackTokenKind != IndexedTokenKind::Property) {
      continue;
    }
    if (occurrence.dotBase == std::optional<std::string>("holder")) {
      sawNestedValueAccess = true;
    }
  }

  EXPECT_TRUE(sawNestedValueAccess);
}

TEST(AnalysisIndexTests, ImportedModulesAreIndexedDuringTypecheck) {
  std::filesystem::path const importClassMethodPath =
      std::filesystem::path(__FILE__).parent_path() / "lesma" / "success" / "import_class_method.les";

  AnalysisResult const result = analyzeFile(importClassMethodPath);
  ASSERT_FALSE(result.hasErrors());

  std::filesystem::path const importedModulePath =
      std::filesystem::path(__FILE__).parent_path() / "lesma" / "success" / "class.les";
  auto importedIt = result.importedModules.find(std::filesystem::weakly_canonical(importedModulePath).string());
  ASSERT_NE(importedIt, result.importedModules.end());
  ASSERT_NE(importedIt->second, nullptr);
  EXPECT_FALSE(importedIt->second->index.symbolOccurrences.empty());

  bool sawGetXDeclaration = false;
  for (const IndexedSymbolOccurrence& occurrence : importedIt->second->index.symbolOccurrences) {
    if (occurrence.name == "getX" &&
        occurrence.fallbackTokenKind == IndexedTokenKind::Method &&
        (occurrence.modifiers & analysis_index_modifier::DECLARATION) != 0U) {
      sawGetXDeclaration = true;
    }
  }
  EXPECT_TRUE(sawGetXDeclaration);
}
} // namespace

// Google Test main function
auto main(int argc, char** argv) -> int {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
