//#include <gtest/gtest.h>
//
//#include "liblesma/Backend/Codegen.h"
//#include "liblesma/Common/Utils.h"
//#include "liblesma/Frontend/Lexer.h"
//#include "liblesma/Frontend/Parser.h"
//
//#include <vector>
//
//using namespace lesma;
//
//static std::shared_ptr<SourceMgr> initializeSrcMgr(const std::string &src) {
//    // Configure Source Manager
//    auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());
//
//    auto buffer = MemoryBuffer::getMemBuffer(src);
//    sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
//
//    return sourceMgr;
//}
//
//static std::unique_ptr<Lexer> initializeLexer(const std::shared_ptr<SourceMgr> &sourceMgr) {
//    auto curLexer = std::make_unique<Lexer>(sourceMgr);
//    curLexer->ScanAll();
//
//    return curLexer;
//}
//
//static std::unique_ptr<Parser> initializeParser(std::unique_ptr<Lexer> lexer) {
//    auto curParser = std::make_unique<Parser>(lexer->getTokens());
//    curParser->Parse();
//
//    return curParser;
//}
//
//static Codegen *initializeCodegen(std::unique_ptr<Parser> parser, const std::shared_ptr<SourceMgr> &srcMgr) {
//    auto _codegen = new Codegen(std::move(parser), srcMgr, __FILE__, {}, true, true);
//    _codegen->Run();
//
//    return _codegen;
//}
//
//
//llvm::SMRange getRange(const std::string &source, int x, int y) {
//    return {llvm::SMLoc::getFromPointer(source.c_str() + x), llvm::SMLoc::getFromPointer(source.c_str() + y)};
//}
//
//class BaseTest : public ::testing::Test {
//public:
//    std::shared_ptr<SourceMgr> srcMgr;
//    std::string source =
//            "var y: int = 100\n"
//            "y = 101\n";
//
//    void SetUp() override {
//        srcMgr = initializeSrcMgr(source);
//    }
//    void TearDown() override {
//        // Place any cleanup code here, if needed
//    }
//};
//
//class LexerTest : public BaseTest {
//public:
//    std::unique_ptr<Lexer> lexer;
//
//    void SetUp() override {
//        BaseTest::SetUp();
//
//        lexer = initializeLexer(BaseTest::srcMgr);
//    }
//    void TearDown() override {
//        BaseTest::TearDown();
//    }
//};
//
//class ParserTest : public LexerTest {
//public:
//    std::unique_ptr<Parser> parser;
//
//    void SetUp() override {
//        LexerTest::SetUp();
//
//        parser = initializeParser(std::move(LexerTest::lexer));
//    }
//
//    void TearDown() override {
//        LexerTest::TearDown();
//    }
//};
//
//class CodegenTest : public ParserTest {
//public:
//    Codegen *codegen = nullptr;
//
//    void SetUp() override {
//        ParserTest::SetUp();
//
//        codegen = initializeCodegen(std::move(ParserTest::parser), LexerTest::srcMgr);
//    }
//
//    void TearDown() override {
//        ParserTest::TearDown();
//    }
//};
//
//
//TEST_F(LexerTest, Tokens) {
//    EXPECT_TRUE(lexer->getTokens().size() > 1);
//
//    std::vector<Token *> tokens = {
//            new Token{TokenType::VAR, "var", getRange(source, 0, 3)},
//            new Token{TokenType::IDENTIFIER, "y", getRange(source, 4, 5)},
//            new Token{TokenType::COLON, ":", getRange(source, 5, 6)},
//            new Token{TokenType::INT_TYPE, "int", getRange(source, 7, 10)},
//            new Token{TokenType::EQUAL, "=", getRange(source, 11, 12)},
//            new Token{TokenType::INTEGER, "100", getRange(source, 13, 16)},
//            new Token{TokenType::NEWLINE, "NEWLINE", getRange(source, 16, 17)},
//            new Token{TokenType::IDENTIFIER, "y", getRange(source, 17, 18)},
//            new Token{TokenType::EQUAL, "=", getRange(source, 19, 20)},
//            new Token{TokenType::INTEGER, "101", getRange(source, 21, 24)},
//            new Token{TokenType::NEWLINE, "NEWLINE", getRange(source, 24, 25)},
//            new Token{TokenType::EOF_TOKEN, "EOF", getRange(source, 25, 25)},
//    };
//
//    for (auto [a, b]: zip(tokens, lexer->getTokens())) {
//        EXPECT_EQ(*a, *b);
//    }
//}
//
//TEST_F(ParserTest, AST) {
//    EXPECT_EQ(parser->getAST()->getChildren().size(), 2);
//    EXPECT_EQ(parser->getAST()->getChildren().at(0)->toString(srcMgr.get(), "", true), "└──VarDecl[Line(1-1):Col(1-17)]: y: int = 100\n");
//    EXPECT_EQ(parser->getAST()->getChildren().at(1)->toString(srcMgr.get(), "", true), "└──Assignment[Line(2-2):Col(1-8)]: y EQUAL 101\n");
//}
//
//// We cannot return from top-level, and the exit function just exits the whole process including the test
//TEST_F(CodegenTest, Run) {
//    codegen->Optimize(OptimizationLevel::O3);
//    codegen->PrepareJIT();
//    int exit_code = codegen->ExecuteJIT();
//
//    EXPECT_TRUE(exit_code == 0);
//}
//
//// Google Test main function
//int main(int argc, char **argv) {
//    ::testing::InitGoogleTest(&argc, argv);
//    return RUN_ALL_TESTS();
//}