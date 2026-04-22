#include <cstddef>
#include <filesystem>
#include <fstream>
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
#include "liblesma/Driver/Driver.h"
#include "liblesma/Formatter/SourceFormatter.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;

namespace {
auto initializeSrcMgr(const std::string& src) -> std::shared_ptr<SourceMgr> {
  // Configure Source Manager
  auto sourceMgr = std::make_shared<SourceMgr>(SourceMgr());

  auto buffer = MemoryBuffer::getMemBufferCopy(src);
  sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());

  return sourceMgr;
}

auto initializeLexer(const std::shared_ptr<SourceMgr>& sourceMgr) -> std::unique_ptr<Lexer> {
  auto curLexer = std::make_unique<Lexer>(sourceMgr);
  curLexer->scanAll();

  return curLexer;
}

auto initializeParser(std::unique_ptr<Lexer> lexer,
                      const std::shared_ptr<SourceMgr>& srcMgr = nullptr)
    -> std::unique_ptr<Parser> {
  auto curParser = srcMgr != nullptr ? std::make_unique<Parser>(lexer->getTokens(), nullptr, srcMgr,
                                                                srcMgr->getNumBuffers(), "test.les",
                                                                true)
                                     : std::make_unique<Parser>(lexer->getTokens());
  curParser->parse();

  return curParser;
}

auto initializeCodegen(std::unique_ptr<Parser> parser, const std::shared_ptr<SourceMgr>& srcMgr,
                       bool arcDebug = false, bool arcTrace = false) -> std::unique_ptr<Codegen> {
  Typechecker typechecker;
  typechecker.run(parser->getAst());
  auto takenTypeCache = typechecker.takeTypeCache();
  auto takenRootScope = typechecker.takeRootScope();
  auto takenSpecializedTypeEnv = typechecker.takeSpecializedTypeEnv();
  auto takenSpecializedTypeToTemplate = typechecker.takeSpecializedTypeToTemplate();
  auto takenSpecializedClassTypes = typechecker.takeSpecializedClassTypes();
  auto takenImportedModules = typechecker.takeImportedModules();
  auto codegen = std::make_unique<Codegen>(
      std::move(parser), srcMgr, __FILE__, std::vector<std::string>{}, true, true, "", nullptr,
      nullptr, nullptr, nullptr, nullptr, std::move(takenRootScope), std::move(takenTypeCache),
      std::move(takenSpecializedTypeEnv), std::move(takenSpecializedTypeToTemplate),
      std::move(takenSpecializedClassTypes), std::move(takenImportedModules),
      nullptr,
      false, arcDebug, arcTrace);
  codegen->run();

  return codegen;
}

auto buildModuleText(const std::string& src, bool arcDebug = false, bool arcTrace = false)
    -> std::string {
  auto srcMgr = initializeSrcMgr(src);
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer), srcMgr);
  auto codegen = initializeCodegen(std::move(parser), srcMgr, arcDebug, arcTrace);
  return codegen->moduleToString();
}

auto analyzeSource(const std::string& src) -> AnalysisResult {
  auto options = std::make_unique<Options>();
  options->sourceType = SourceType::STRING;
  options->source = src;
  options->implicitFilePath = "analysis_index_test.les";
  options->suppressWarnings = true;
  return analyze(std::move(options));
}

auto analyzeFile(const std::filesystem::path& path) -> AnalysisResult;

auto initializeCodegenFromAnalysis(AnalysisResult result, bool arcDebug = false, bool arcTrace = false)
    -> std::unique_ptr<Codegen> {
  auto codegen = std::make_unique<Codegen>(
      std::move(result.parser), result.sourceMgr,
      result.mainFilePath.empty() ? "" : result.mainFilePath, std::vector<std::string>{}, true, true,
      "", nullptr, nullptr, nullptr, nullptr, nullptr, std::move(result.rootScope),
      std::move(result.typeCache), std::move(result.specializedTypeEnv),
      std::move(result.specializedTypeToTemplate), std::move(result.specializedClassTypes),
      std::move(result.importedModules), nullptr, false,
      arcDebug, arcTrace);
  codegen->run();
  return codegen;
}

auto buildFileModuleText(const std::filesystem::path& path, bool arcDebug = false,
                         bool arcTrace = false) -> std::string {
  AnalysisResult result = analyzeFile(path);
  auto codegen = initializeCodegenFromAnalysis(std::move(result), arcDebug, arcTrace);
  return codegen->moduleToString();
}

auto runFileWithArcDebug(const std::filesystem::path& path, bool arcTrace = false)
    -> std::pair<int, std::string> {
  AnalysisResult result = analyzeFile(path);
  auto codegen = initializeCodegenFromAnalysis(std::move(result), true, arcTrace);
  testing::internal::CaptureStdout();
  codegen->prepareJit();
  int const exitCode = codegen->executeJit();
  return {exitCode, testing::internal::GetCapturedStdout()};
}

auto writeScratchFile(const std::filesystem::path& path, const std::string& contents) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << contents;
}

auto recreateScratchDir(const std::string& name) -> std::filesystem::path {
  std::filesystem::path const dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

auto analyzeFile(const std::filesystem::path& path) -> AnalysisResult {
  auto options = std::make_unique<Options>();
  options->sourceType = SourceType::FILE;
  options->source = path.string();
  options->suppressWarnings = true;
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

    codegen = initializeCodegen(std::move(ParserTest::parser), LexerTest::srcMgr);
  }

  auto TearDown() -> void override { ParserTest::TearDown(); }
};

TEST_F(LexerTest, Tokens) {
  EXPECT_TRUE(lexer->getTokens().size() > 1);

  // Get the buffer start pointer from the SourceMgr (not the local source
  // string)
  const char* bufStart = srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())->getBufferStart();

  std::vector<std::unique_ptr<Token>> expectedTokens;
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::VAR, "var", getRange(bufStart, 0, 3)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::IDENTIFIER, "y", getRange(bufStart, 4, 5)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::COLON, ":", getRange(bufStart, 5, 6)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::INT_TYPE, "int", getRange(bufStart, 7, 10)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::EQUAL, "=", getRange(bufStart, 11, 12)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::INTEGER, "100", getRange(bufStart, 13, 16)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE", getRange(bufStart, 16, 17)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::IDENTIFIER, "y", getRange(bufStart, 17, 18)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::EQUAL, "=", getRange(bufStart, 19, 20)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::INTEGER, "101", getRange(bufStart, 21, 24)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE", getRange(bufStart, 24, 25)));
  expectedTokens.push_back(
      std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF", getRange(bufStart, 24, 25)));

  auto actualTokens = lexer->getTokens();
  ASSERT_EQ(expectedTokens.size(), actualTokens.size());

  for (size_t i = 0; i < expectedTokens.size(); i++) {
    EXPECT_EQ(*expectedTokens[i], *actualTokens[i]);
  }
}

TEST_F(ParserTest, AST) {
  EXPECT_EQ(parser->getAst()->getChildren().size(), 2);
  EXPECT_EQ(parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true),
            "└──VarDecl[Line(1-1):Col(1-17)]: y: int = 100\n");
  EXPECT_EQ(parser->getAst()->getChildren().at(1)->toString(srcMgr.get(), "", true),
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

TEST(CodegenIRTests, BorrowedArcReturnEmitsRetain) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

func echo(box: Box) -> Box {
  return box
}

let out = echo(Box(1))
exit(out.value)
)");

  EXPECT_NE(moduleText.find("atomicrmw add"), std::string::npos);
  EXPECT_NE(moduleText.find("ret ptr"), std::string::npos);
}

TEST(CodegenIRTests, ForInIteratorCleanupReleasesIteratorSlot) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

class OnceIter impl Iterator<Box> {
  var current: Box?

  func new(value: Box?) {
    self.current = value
  }

  func next() -> Box? {
    let out = self.current
    self.current = null
    return out
  }
}

class Once impl Iterable<Box> {
  var current: Box?

  func new(value: Box?) {
    self.current = value
  }

  func iter() -> OnceIter {
    return OnceIter(self.current)
  }
}

for item in Once(Box(3)) {
  exit(item.value)
}
)");

  EXPECT_NE(moduleText.find("for.iter.slot"), std::string::npos);
  EXPECT_NE(moduleText.find("for.iter"), std::string::npos);
  EXPECT_NE(moduleText.find("arc.release.next"), std::string::npos);
}

TEST(CodegenIRTests, ListOverwriteAndPopEmitStableOwnershipBlocks) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

var items: list<Box> = []
items.push(Box(1))
items[0] = Box(2)
let tail = items.pop()
if tail is not null {
  exit(tail.value)
}
)");

  EXPECT_NE(moduleText.find("list.pop.result"), std::string::npos);
  EXPECT_NE(moduleText.find("list.pop.ptr"), std::string::npos);
  EXPECT_NE(moduleText.find("arc.release.next"), std::string::npos);
}

TEST(CodegenIRTests, ClosureCaptureMaterializationBuildsArcEnvironment) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

func make_reader(box: Box) -> func() -> int {
  return func() -> int {
    return box.value
  }
}

let reader = make_reader(Box(7))
exit(reader())
)");

  EXPECT_NE(moduleText.find(".env.arc.payload"), std::string::npos);
  EXPECT_NE(moduleText.find("__lesma_arc_destroy_env___lambda_0"), std::string::npos);
  EXPECT_NE(moduleText.find("cap.slot"), std::string::npos);
  EXPECT_NE(moduleText.find("arc.env.slot"), std::string::npos);
}

TEST(CodegenIRTests, AsyncManagedReturnReleasesPromisePayloadDuringCleanup) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

async func make_box(value: int) -> Box {
  return Box(value)
}

let box = await make_box(7)
exit(box.value)
)");

  EXPECT_NE(moduleText.find("async.cleanup.ready"), std::string::npos);
  EXPECT_NE(moduleText.find("async.cleanup.payload"), std::string::npos);
  EXPECT_NE(moduleText.find("async.cleanup.arc.release"), std::string::npos);
}

TEST(CodegenIRTests, OptionalReplacementEmitsReleaseOfPreviousValue) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

var current: Box? = Box(1)
current = Box(2)
if current is null {
  exit(1)
} else {
  exit(current.get())
}
)");

  EXPECT_NE(moduleText.find("arc.release.next"), std::string::npos);
  EXPECT_NE(moduleText.find("arc.release.destroyfn"), std::string::npos);
}

TEST(CodegenIRTests, AnyBoxedEnumPayloadBuildsEnumArcHelpers) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

enum MaybeBox {
  None
  Some(Box)
}

var payload: any = MaybeBox.Some(Box(1))
payload = 7
)");

  EXPECT_NE(moduleText.find("__lesma_arc_destroy_payload_"), std::string::npos);
  EXPECT_NE(moduleText.find("__lesma_arc_release_storage_"), std::string::npos);
  EXPECT_NE(moduleText.find("arc.enum.release"), std::string::npos);
}

TEST(CodegenIRTests, NullCoalesceManagedPayloadBuildsMergePhi) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

func choose(left: Box?, fallback: Box) -> Box {
  return left ?? fallback
}

let chosen = choose(Box(1), Box(2))
exit(chosen.get())
)");

  EXPECT_NE(moduleText.find("coalesce.result"), std::string::npos);
  EXPECT_NE(moduleText.find("coalesce.left.payload.tmp"), std::string::npos);
}

TEST(CodegenIRTests, UnionMethodDispatchManagedReturnBuildsPhi) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

class BoxA {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func make() -> Box {
    return Box(self.value)
  }
}

class BoxB {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func make() -> Box {
    return Box(self.value + 10)
  }
}

func read(value: BoxA | BoxB) -> int {
  let made = value.make()
  return made.get()
}

exit(read(BoxA(4)))
)");

  EXPECT_NE(moduleText.find("union.m.phi"), std::string::npos);
  EXPECT_NE(moduleText.find("union.m.case"), std::string::npos);
}

TEST(CodegenIRTests, MainModuleCleanupRunsBeforeArcReport) {
  std::string const moduleText = buildModuleText(R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

export var current: Box? = Box(4)

let first = current
if first is null {
  exit(1)
} else {
  if first.get() != 4 {
    exit(2)
  }
}
)",
                                           true);

  size_t const cleanupCallPos = moduleText.rfind("call void @__lesma_mod_fini_");
  size_t const reportCallPos = moduleText.rfind("call void @__lesma_arc_debug_report()");
  ASSERT_NE(cleanupCallPos, std::string::npos);
  ASSERT_NE(reportCallPos, std::string::npos);
  EXPECT_LT(cleanupCallPos, reportCallPos);
}

TEST(CodegenIRTests, ImportedModuleFinisRunBeforeMainCleanupAndReport) {
  std::filesystem::path const scratchDir =
      recreateScratchDir("lesma_arc_ir_imported_module_cleanup");
  std::filesystem::path const modulePath = scratchDir / "arc_mod.les";
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(modulePath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

export var current: Box? = Box(7)
)");
  writeScratchFile(mainPath, R"(import "arc_mod.les"

export var local: arc_mod.Box? = arc_mod.Box(2)

let first = arc_mod.current
if first is null {
  exit(1)
} else {
  if first.get() != 7 {
    exit(2)
  }
}
)");

  std::string const moduleText = buildFileModuleText(mainPath, true);
  size_t const importedFiniCallPos = moduleText.find("call void @__lesma_mod_fini_");
  size_t const mainFiniCallPos =
      moduleText.find("call void @__lesma_mod_fini_", importedFiniCallPos + 1U);
  size_t const reportCallPos = moduleText.rfind("call void @__lesma_arc_debug_report()");
  ASSERT_NE(importedFiniCallPos, std::string::npos);
  ASSERT_NE(mainFiniCallPos, std::string::npos);
  ASSERT_NE(reportCallPos, std::string::npos);
  EXPECT_LT(importedFiniCallPos, mainFiniCallPos);
  EXPECT_LT(mainFiniCallPos, reportCallPos);
}

TEST(ArcDebugRuntimeTests, LocalTopLevelArcChurnReportsZero) {
  std::filesystem::path const scratchDir = recreateScratchDir("lesma_arc_runtime_local_zero");
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(mainPath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

var current: Box? = Box(1)
current = Box(2)
if current is null {
  exit(1)
} else {
  if current.get() != 2 {
    exit(2)
  }
}
)");

  auto const [exitCode, output] = runFileWithArcDebug(mainPath);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos) << output;
}

TEST(ArcDebugRuntimeTests, MixedTupleBorrowedAndOwnedArcElementsReportZero) {
  std::filesystem::path const scratchDir = recreateScratchDir("lesma_arc_runtime_tuple_mixed_zero");
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(mainPath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

let a = Box(1)
let pair: (Box, Box) = (a, Box(2))
if pair[0].get() != 1 {
  exit(1)
}
if pair[1].get() != 2 {
  exit(2)
}
)");

  auto const [exitCode, output] = runFileWithArcDebug(mainPath);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos);
}

TEST(ArcDebugRuntimeTests, EnumPayloadOverwriteReleasesManagedPayloads) {
  std::filesystem::path const scratchDir = recreateScratchDir("lesma_arc_runtime_enum_payload_zero");
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(mainPath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }

  func get() -> int {
    return self.value
  }
}

enum MaybeBox {
  None
  Some(Box)
}

var current: MaybeBox = MaybeBox.Some(Box(1))
current = MaybeBox.None
if match current {
  MaybeBox.None => true
  MaybeBox.Some(_) => false
} == false {
  exit(1)
}
)");

  auto const [exitCode, output] = runFileWithArcDebug(mainPath);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos) << output;
}

TEST(ArcDebugRuntimeTests, EnumMultiPayloadOverwriteReleasesManagedPayloads) {
  std::filesystem::path const scratchDir =
      recreateScratchDir("lesma_arc_runtime_enum_multi_payload_zero");
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(mainPath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

enum MaybePair {
  Empty
  Pair(Box, Box)
}

var current: MaybePair = MaybePair.Pair(Box(1), Box(2))
current = MaybePair.Empty
if match current {
  MaybePair.Empty => true
  MaybePair.Pair(_, _) => false
} == false {
  exit(1)
}
)");

  auto const [exitCode, output] = runFileWithArcDebug(mainPath);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos) << output;
}

TEST(ArcDebugRuntimeTests, AnyBoxedEnumPayloadReportsZero) {
  std::filesystem::path const scratchDir =
      recreateScratchDir("lesma_arc_runtime_any_boxed_enum_payload_zero");
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(mainPath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

enum MaybeBox {
  None
  Some(Box)
}

var payload: any = MaybeBox.Some(Box(1))
payload = 7
if payload is not int or payload as int != 7 {
  exit(1)
}
)");

  auto const [exitCode, output] = runFileWithArcDebug(mainPath);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos) << output;
}

TEST(ArcDebugRuntimeTests, GenericEnumPayloadOverwriteReportsZero) {
  std::filesystem::path const scratchDir =
      recreateScratchDir("lesma_arc_runtime_generic_enum_payload_zero");
  std::filesystem::path const mainPath = scratchDir / "main.les";
  writeScratchFile(mainPath, R"(class Box {
  var value: int

  func new(value: int) {
    self.value = value
  }
}

var current: Result<Box, int> = Result.Ok(Box(1))
current = Result.Err(2)
if match current {
  Result.Err(value) => value == 2
  Result.Ok(_) => false
} == false {
  exit(1)
}
)");

  auto const [exitCode, output] = runFileWithArcDebug(mainPath);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos) << output;
}

TEST(ArcDebugRuntimeTests, ImportedModuleGlobalsReportZeroAfterCleanup) {
  std::filesystem::path const repoRoot =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  auto const [exitCode, output] =
      runFileWithArcDebug(repoRoot / "tests/lesma/success/arc_export_global_heap.les");
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos);
  EXPECT_NE(output.find("roots tracked:"), std::string::npos);
  EXPECT_NE(output.find("roots remaining: 0"), std::string::npos);
}

TEST(ArcDebugRuntimeTests, StaticArcRootsReportZeroAfterCleanup) {
  std::filesystem::path const repoRoot =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  auto const [exitCode, output] =
      runFileWithArcDebug(repoRoot / "tests/lesma/success/static_field_callable_closure.les");
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] live objects: 0"), std::string::npos);
  EXPECT_NE(output.find("roots tracked:"), std::string::npos);
  EXPECT_NE(output.find("roots remaining: 0"), std::string::npos);
}

TEST(ArcDebugRuntimeTests, ArcTracePrintsModuleRootNamesDuringCleanup) {
  std::filesystem::path const repoRoot =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  auto const [exitCode, output] =
      runFileWithArcDebug(repoRoot / "tests/lesma/success/static_field_callable_closure.les", true);
  EXPECT_EQ(exitCode, 0);
  EXPECT_NE(output.find("[arc] cleanup root lesma.sfs."), std::string::npos);
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

TEST(LexerTests, LexLineComments) {
  auto srcMgr = initializeSrcMgr("var x = 1 // trailing\n// leading\nvar y = 2\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  ASSERT_GE(tokens.size(), 11);
  EXPECT_EQ(tokens[4]->type, TokenType::LINE_COMMENT);
  EXPECT_EQ(tokens[4]->lexeme, "// trailing");
  EXPECT_EQ(tokens[5]->type, TokenType::NEWLINE);
  EXPECT_EQ(tokens[6]->type, TokenType::LINE_COMMENT);
  EXPECT_EQ(tokens[6]->lexeme, "// leading");
}

TEST(LexerTests, LexBlockComments) {
  auto srcMgr = initializeSrcMgr("var x = 1 /* block\n * comment\n */\nvar y = 2\n");
  auto lexer = initializeLexer(srcMgr);
  auto tokens = lexer->getTokens();

  ASSERT_GE(tokens.size(), 10);
  EXPECT_EQ(tokens[4]->type, TokenType::BLOCK_COMMENT);
  EXPECT_EQ(tokens[4]->lexeme, "/* block\n * comment\n */");
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
  auto astStr = parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  // BinaryOp toString returns "left OP right", so check for the operator
  EXPECT_TRUE(astStr.find("PLUS") != std::string::npos);
}

TEST(ParserTests, ParseUnaryMinus) {
  auto srcMgr = initializeSrcMgr("var x = -5\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr = parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  // UnaryOp toString returns "OP expr", so check for MINUS
  EXPECT_TRUE(astStr.find("MINUS") != std::string::npos);
}

TEST(ParserTests, ParseVarDecl) {
  auto srcMgr = initializeSrcMgr("var x: int = 10\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr = parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  EXPECT_TRUE(astStr.find("VarDecl") != std::string::npos);
}

TEST(ParserTests, ParseLetDecl) {
  auto srcMgr = initializeSrcMgr("let x = 10\n");
  auto lexer = initializeLexer(srcMgr);
  auto parser = initializeParser(std::move(lexer));

  ASSERT_EQ(parser->getAst()->getChildren().size(), 1);
  auto astStr = parser->getAst()->getChildren().at(0)->toString(srcMgr.get(), "", true);
  EXPECT_TRUE(astStr.find("VarDecl") != std::string::npos);
}

TEST(FormatterTests, FormatFilePreservesCommentsAndIsIdempotent) {
  std::filesystem::path const path =
      std::filesystem::temp_directory_path() / "lesma_fmt_preserves_comments.les";
  std::ofstream(path)
      << "var  x=1 // inline\n// leading\nfunc  add(a:int,b:int)->int{\nreturn a+b\n}\n";

  auto formatted = formatFile(path, 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  std::string const expected = "var x = 1 // inline\n"
                               "\n"
                               "// leading\n"
                               "func add(a: int, b: int) -> int {\n"
                               "  return a + b\n"
                               "}\n";
  EXPECT_EQ(*formatted, expected);

  std::ofstream(path) << expected;
  auto formattedAgain = formatFile(path, 100);
  ASSERT_TRUE(formattedAgain.has_value()) << formattedAgain.error().message;
  EXPECT_EQ(*formattedAgain, expected);

  std::filesystem::remove(path);
}

TEST(FormatterTests, DriverFormatsDirectoriesRecursively) {
  std::filesystem::path const root =
      std::filesystem::temp_directory_path() / "lesma_fmt_recursive_dir";
  std::filesystem::path const nested = root / "nested";
  std::filesystem::create_directories(nested);

  std::filesystem::path const lesFile = nested / "sample.les";
  std::filesystem::path const ignoredFile = nested / "ignore.txt";
  std::ofstream(lesFile) << "let  y=2\n";
  std::ofstream(ignoredFile) << "unchanged";

  EXPECT_EQ(Driver::formatPaths({root}, 100), 0);

  std::ifstream ifs(lesFile);
  std::string formatted((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_EQ(formatted, "let y = 2\n");
  EXPECT_TRUE(std::filesystem::exists(ignoredFile));

  std::filesystem::remove_all(root);
}

TEST(FormatterTests, DriverFormatsDirectoriesBestEffortWhenSomeFilesDoNotParse) {
  std::filesystem::path const root =
      std::filesystem::temp_directory_path() / "lesma_fmt_recursive_best_effort";
  std::filesystem::create_directories(root);

  std::filesystem::path const validFile = root / "valid.les";
  std::filesystem::path const invalidFile = root / "invalid.les";
  std::ofstream(validFile) << "let  y=2\n";
  std::ofstream(invalidFile) << "let broken = {\n";

  EXPECT_EQ(Driver::formatPaths({root}, 100), 0);

  std::ifstream ifs(validFile);
  std::string formatted((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_EQ(formatted, "let y = 2\n");

  std::filesystem::remove_all(root);
}

TEST(FormatterTests, ParserAttachesStatementTriviaAndNormalizedBlankLines) {
  auto parsed = parseSourceForFormatting(
      "// file comment\nlet x = 1 // trailing\n\n// step\nlet y = 2\n", "trivia_test.les");
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  auto& parser = parsed->parser;

  std::vector<Statement*> const children = parser->getAst()->getChildren();
  ASSERT_EQ(children.size(), 2U);

  EXPECT_EQ(children[0]->getLeadingComments().size(), 1U);
  EXPECT_EQ(children[0]->getLeadingComments()[0].text, "// file comment");
  ASSERT_TRUE(children[0]->getTrailingComment().has_value());
  EXPECT_EQ(children[0]->getTrailingComment()->text, "// trailing");

  EXPECT_EQ(children[1]->getExtraBlankLinesBefore(), 1U);
  ASSERT_EQ(children[1]->getLeadingComments().size(), 1U);
  EXPECT_EQ(children[1]->getLeadingComments()[0].text, "// step");
}

TEST(FormatterTests, FormatSourceNormalizesOptionalBlankLines) {
  auto formatted =
      formatSource("func work() -> void {\nlet a = 1\n\n\nlet b = 2\n\n// step\nlet c = 3\n}\n",
                   "blank_lines_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "func work() {\n"
                        "  let a = 1\n"
                        "\n"
                        "  let b = 2\n"
                        "\n"
                        "  // step\n"
                        "  let c = 3\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourcePreservesEnumValueComments) {
  auto formatted = formatSource("enum Status {\nREADY\n\n// transitional\nWAITING // trailing\n}\n",
                                "enum_comments_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "enum Status {\n"
                        "  READY\n"
                        "\n"
                        "  // transitional\n"
                        "  WAITING // trailing\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourcePreservesBlockComments) {
  auto formatted = formatSource("func work() -> void {\n/* setup\n * step\n */\nlet x = 1\n}\n",
                                "block_comment_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "func work() {\n"
                        "  /* setup\n"
                        "   * step\n"
                        "   */\n"
                        "  let x = 1\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourceNormalizesDocComments) {
  auto formatted = formatSource("/**\n* summary\n*details\n*/\nfunc work() -> int { return 1 }\n",
                                "doc_comment_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "/**\n"
                        " * summary\n"
                        " * details\n"
                        " */\n"
                        "func work() -> int {\n"
                        "  return 1\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourceIsParseStable) {
  auto formatted = formatSource(
      "// heading\nfunc add(a:int,b:int)->int{\nreturn a+b\n}\n\nlet result=add(1,2)\n",
      "roundtrip_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;

  auto reparsed = parseSourceForFormatting(*formatted, "roundtrip_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;

  std::string const formattedAgain = formatParsedFile(*reparsed, 100);
  EXPECT_EQ(formattedAgain, *formatted);
}

TEST(FormatterTests, FormatSourceCanonicalizesOptionalTypeSyntax) {
  auto formatted = formatSource("let value: int | null = null\n", "optional_type_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "let value: int? = null\n");
}

TEST(FormatterTests, FormatSourcePreservesNilCoalescingPrecedence) {
  auto formatted = formatSource("let ok = maybe ?? 0 == 1\n", "coalesce_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "let ok = maybe ?? 0 == 1\n");
}

TEST(FormatterTests, FormatSourcePreservesNullCoalescingAssignment) {
  auto formatted = formatSource("var maybe: int? = null\nmaybe ?"
                                "?= 1\n",
                                "coalesce_assign_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "var maybe: int? = null\nmaybe ?"
                        "?= 1\n");
}

TEST(FormatterTests, FormatSourcePreservesAddressOfUnaryOperator) {
  auto formatted = formatSource("var x = 1\nlet p: *int = &x\n", "address_of_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("&x"), std::string::npos);
  EXPECT_EQ(formatted->find("?x"), std::string::npos);
}

TEST(FormatterTests, FormatSourcePreservesAwaitUnaryOperator) {
  auto formatted = formatSource(
      "async func work(value: int) -> int {\nreturn await next(value)\n}\n", "await_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("await next(value)"), std::string::npos);
  EXPECT_EQ(formatted->find("?next(value)"), std::string::npos);
}

TEST(FormatterTests, FormatSourcePreservesAsyncLambdaSyntax) {
  auto formatted = formatSource("let read = async func(x: int) -> int => await next(x)\n"
                                "let run = async func(x: int) -> int {\n"
                                "return await next(x)\n"
                                "}\n",
                                "async_lambda_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("async func(x: int) -> int => await next(x)"), std::string::npos);
  EXPECT_NE(formatted->find("let run = async func(x: int) -> int {\n"), std::string::npos);
}

TEST(FormatterTests, FormatSourcePreservesAsyncMethodModifiers) {
  auto formatted = formatSource("class Worker {\n"
                                "private async func add(delta: int) -> int {\n"
                                "return delta\n"
                                "}\n"
                                "static async func twice(value: int) -> int {\n"
                                "return value * 2\n"
                                "}\n"
                                "}\n",
                                "async_method_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("private async func add(delta: int) -> int"), std::string::npos);
  EXPECT_NE(formatted->find("static async func twice(value: int) -> int"), std::string::npos);
}

TEST(FormatterTests, FormatSourcePreservesBitwisePipeOperator) {
  auto formatted = formatSource("func combine(lhs: int, rhs: int) -> int {\nreturn lhs | rhs\n}\n",
                                "bitwise_pipe_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("lhs | rhs"), std::string::npos);
  EXPECT_EQ(formatted->find("lhs ? rhs"), std::string::npos);
}

TEST(FormatterTests, FormatSourcePreservesParensForCastBeforeDotAccess) {
  auto formatted = formatSource("func readValue(boxed: any) -> int {\n"
                                "return (boxed as Box).value\n"
                                "}\n",
                                "cast_dot_precedence_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("(boxed as Box).value"), std::string::npos);
}

TEST(FormatterTests, FormatSourcePreservesInferredExpressionLambdaReturnType) {
  auto formatted = formatSource("let inc = func(x: int) => x + 1\n", "lambda_infer_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_NE(formatted->find("func(x: int) => x + 1"), std::string::npos);
  EXPECT_EQ(formatted->find("-> void"), std::string::npos);
}

TEST(FormatterTests, FormatSourceOmitsExplicitVoidReturnTypesEverywhere) {
  auto formatted = formatSource("func work() -> void {\n"
                                "  pass\n"
                                "}\n"
                                "func takes(callback: func(int) -> void) -> void {\n"
                                "  callback(1)\n"
                                "}\n"
                                "let handler: func(int) -> void = func(value: int) -> void {\n"
                                "  pass\n"
                                "}\n",
                                "void_return_style_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(formatted->find("-> void"), std::string::npos);
}

TEST(FormatterTests, NarrowWidthBreaksLongCalls) {
  auto formatted = formatSource("combine(alpha, beta, gamma, delta)\n", "width_test.les", 20);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "combine(\n"
                        "  alpha,\n"
                        "  beta,\n"
                        "  gamma,\n"
                        "  delta,\n"
                        ")\n");
}

TEST(FormatterTests, NarrowWidthBreaksChainedCallsOneSegmentPerLine) {
  auto formatted = formatSource("let total = xs.map(func(x: int) => x + 1).filter(func(x: int) => "
                                "x > 5).reduce(func(acc: int, x: int) => acc + x, 0)\n",
                                "chain_width_test.les", 32);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "let total = xs\n"
                        "  .map(func(x: int) => x + 1)\n"
                        "  .filter(func(x: int) => x > 5)\n"
                        "  .reduce(\n"
                        "    func(\n"
                        "      acc: int,\n"
                        "      x: int,\n"
                        "    ) => acc + x,\n"
                        "    0,\n"
                        "  )\n");

  auto reparsed = parseSourceForFormatting(*formatted, "chain_width_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, TopLevelMajorDeclarationsCapAtOneBlankLine) {
  auto formatted = formatSource("func a() -> void {}\n\n\nfunc b() -> void {}\n",
                                "top_level_spacing_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "func a() {}\n"
                        "\n"
                        "func b() {}\n");
}

TEST(FormatterTests, NarrowWidthBreaksLongReturnExpressionAndReparses) {
  auto formatted = formatSource(
      "func check(b: uint8) -> bool {\nreturn b == 9 as uint8 or b == 32 as uint8\n}\n",
      "return_expr_width_test.les", 20);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_TRUE(formatted->find("return\n") == std::string::npos);
  EXPECT_TRUE(formatted->find("return b == 9 as uint8 or b == 32 as uint8") == std::string::npos);

  auto reparsed = parseSourceForFormatting(*formatted, "return_expr_width_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, NarrowWidthWrapsRepeatedBinaryOperatorsConsistently) {
  auto formatted = formatSource("func calc() -> int {\nreturn alpha + beta + gamma + delta\n}\n",
                                "operator_chain_width_test.les", 22);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "func calc() -> int {\n"
                        "  return alpha\n"
                        "    + beta\n"
                        "    + gamma\n"
                        "    + delta\n"
                        "}\n");
}

TEST(FormatterTests, NarrowWidthBreaksLongIfConditionAndReparses) {
  auto formatted = formatSource("func check(flag: bool, other: bool, extra: bool) -> bool {\nif "
                                "flag and other or extra { return true }\nreturn false\n}\n",
                                "if_condition_width_test.les", 24);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_TRUE(formatted->find("flag and other or extra") == std::string::npos);

  auto reparsed = parseSourceForFormatting(*formatted, "if_condition_width_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, NarrowWidthBreaksLongAssignmentAndReparses) {
  auto formatted = formatSource("func work() -> void {\nvalue = alpha + beta + gamma + delta\n}\n",
                                "assignment_width_test.les", 20);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_TRUE(formatted->find("value = alpha + beta + gamma + delta") == std::string::npos);

  auto reparsed = parseSourceForFormatting(*formatted, "assignment_width_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, NarrowWidthBreaksLongWhileConditionAndReparses) {
  auto formatted =
      formatSource("func work() -> void {\nwhile alpha and beta or gamma { break }\n}\n",
                   "while_condition_width_test.les", 20);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_TRUE(formatted->find("while alpha and beta or gamma") == std::string::npos);

  auto reparsed = parseSourceForFormatting(*formatted, "while_condition_width_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, NarrowWidthBreaksLongLambdaExpressionBodyAndReparses) {
  auto formatted = formatSource(
      "func work() -> void {\nlet f = func(x: int) -> int => alpha + beta + gamma + delta\n}\n",
      "lambda_width_test.les", 24);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_TRUE(formatted->find("=> alpha + beta + gamma + delta") == std::string::npos);

  auto reparsed = parseSourceForFormatting(*formatted, "lambda_width_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, FormatSourceParsesIndentedContinuationInput) {
  auto formatted = formatSource("func check(b: uint8) -> bool {\n"
                                "  return\n"
                                "    b == 9 as uint8 or b == 10 as uint8 or b == 11 as uint8 or b "
                                "== 12 as uint8 or b == 13 as uint8\n"
                                "      or b == 32 as uint8\n"
                                "}\n",
                                "wrapped_return_input_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;

  auto reparsed = parseSourceForFormatting(*formatted, "wrapped_return_input_test.les");
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message;
}

TEST(FormatterTests, FormatSourcePreservesLogicalSectionsInsideBlocks) {
  auto formatted = formatSource("func work() -> void {\n"
                                "let a = 1\n"
                                "\n"
                                "// phase two\n"
                                "let b = 2\n"
                                "\n"
                                "let c = 3\n"
                                "}\n",
                                "block_grouping_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "func work() {\n"
                        "  let a = 1\n"
                        "\n"
                        "  // phase two\n"
                        "  let b = 2\n"
                        "\n"
                        "  let c = 3\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourceWrapsClassHeadsAndSeparatesFieldsFromMethods) {
  auto formatted = formatSource("class Widget : Base impl FirstTrait, SecondTrait, ThirdTrait {\n"
                                "var value: int\n"
                                "func read() -> int { return self.value }\n"
                                "}\n",
                                "class_layout_test.les", 36);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "class Widget : Base\n"
                        "  impl FirstTrait,\n"
                        "  SecondTrait,\n"
                        "  ThirdTrait {\n"
                        "  var value: int\n"
                        "\n"
                        "  func read() -> int {\n"
                        "    return self.value\n"
                        "  }\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourceKeepsNamedImportsOnOneLine) {
  auto formatted = formatSource("from json import Json, JsonErr, Serializable\n",
                                "named_imports_test.les", 20);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "from json import Json, JsonErr, Serializable\n");
}

TEST(FormatterTests, FormatSourceKeepsBlankLineAfterImports) {
  auto formatted = formatSource("from json import Json, JsonErr, Serializable\n"
                                "class User {\n"
                                "var name: str\n"
                                "}\n",
                                "import_spacing_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "from json import Json, JsonErr, Serializable\n"
                        "\n"
                        "class User {\n"
                        "  var name: str\n"
                        "}\n");
}

TEST(FormatterTests, FormatSourceKeepsBlankLineAfterImportsBeforeLet) {
  auto formatted = formatSource("from \"type_alias_export_mod.les\" import MaybeNum, Word\n"
                                "let maybe: MaybeNum = null\n",
                                "import_spacing_let_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "from \"type_alias_export_mod.les\" import MaybeNum, Word\n"
                        "\n"
                        "let maybe: MaybeNum = null\n");
}

TEST(FormatterTests, FormatSourceSupportsValuePositionTypeReceivers) {
  auto formatted =
      formatSource("func wrap() -> Result<int, int> {\nreturn Result<int, int>.Ok(1)\n}\n",
                   "value_type_receiver_test.les", 100);
  ASSERT_TRUE(formatted.has_value()) << formatted.error().message;
  EXPECT_EQ(*formatted, "func wrap() -> Result<int, int> {\n"
                        "  return Result<int, int>.Ok(1)\n"
                        "}\n");
}

TEST(ParserTests, ReturnExpressionAllowsIndentedContinuation) {
  AnalysisResult const result = analyzeSource("func check(b: uint8) -> bool {\n"
                                              "  return\n"
                                              "    b == 9 as uint8 or b == 10 as uint8 or b == 11 "
                                              "as uint8 or b == 12 as uint8 or b == 13 as uint8\n"
                                              "      or b == 32 as uint8\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, LogicalExpressionAllowsIndentedContinuationInInitializer) {
  AnalysisResult const result = analyzeSource("func check() -> bool {\n"
                                              "  let result = true and\n"
                                              "    false or\n"
                                              "    true\n"
                                              "  return result\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, ArithmeticExpressionAllowsIndentedContinuation) {
  AnalysisResult const result = analyzeSource("func calc() -> int {\n"
                                              "  return 1 +\n"
                                              "    2 * 3 -\n"
                                              "    4\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, ComparisonOperatorsAllowIndentedContinuationBeforeOperator) {
  AnalysisResult const result = analyzeSource("func check(b: uint8) -> bool {\n"
                                              "  return b\n"
                                              "    == 9 as uint8\n"
                                              "    or b\n"
                                              "      == 32 as uint8\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, ContinuationExpressionDoesNotConsumeFollowingStatement) {
  AnalysisResult const result = analyzeSource("func calc() -> int {\n"
                                              "  let total = 1 +\n"
                                              "    2\n"
                                              "  let extra = 3\n"
                                              "  return total + extra\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, InitializerAllowsIndentedContinuationAfterEquals) {
  AnalysisResult const result = analyzeSource("func calc() -> int {\n"
                                              "  let total =\n"
                                              "    1 + 2\n"
                                              "  return total\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, UnaryOperatorAllowsIndentedContinuationAfterOperator) {
  AnalysisResult const result = analyzeSource("func check(flag: bool) -> bool {\n"
                                              "  return not\n"
                                              "    flag\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, IfConditionAllowsIndentedContinuationAfterKeyword) {
  AnalysisResult const result = analyzeSource("func check(a: bool, b: bool) -> bool {\n"
                                              "  if\n"
                                              "    a or b {\n"
                                              "    return true\n"
                                              "  }\n"
                                              "  return false\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, WhileConditionAllowsIndentedContinuationAfterKeyword) {
  AnalysisResult const result = analyzeSource("func spin(a: bool, b: bool) -> void {\n"
                                              "  while\n"
                                              "    a and b {\n"
                                              "    break\n"
                                              "  }\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, ForIterableAllowsIndentedContinuationAfterIn) {
  AnalysisResult const result = analyzeSource("func walk() -> void {\n"
                                              "  for item in\n"
                                              "    range(3) {\n"
                                              "    print(item)\n"
                                              "  }\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, ParameterDefaultAllowsIndentedContinuationAfterEquals) {
  AnalysisResult const result = analyzeSource("func check(flag: bool =\n"
                                              "  true or false) -> bool {\n"
                                              "  return flag\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
}

TEST(ParserTests, LambdaExpressionBodyAllowsIndentedContinuationAfterFatArrow) {
  AnalysisResult const result = analyzeSource("func check() -> int {\n"
                                              "  let f = func(x: int) -> int =>\n"
                                              "    x + 1\n"
                                              "  return f(1)\n"
                                              "}\n");
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);
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

TEST(WarningDiagnostics, EmitsUnreachableCodeWarning) {
  constexpr auto source = R"(
func f() -> void {
    return
    var x: int = 1
}
)";
  auto options = std::make_unique<Options>();
  options->sourceType = SourceType::STRING;
  options->source = source;
  options->implicitFilePath = "unreachable_warn_test.les";
  options->suppressWarnings = false;
  AnalysisResult const result = analyze(std::move(options));
  ASSERT_FALSE(result.hasErrors());
  bool found = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == AnalysisDiagnosticSeverity::Warning && d.message == "Unreachable code") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(WarningDiagnostics, DoesNotFlagCastAfterFlowNarrowingAsRedundant) {
  constexpr auto source = R"(class Box {
    var value: int

    func new(value: int) {
        self.value = value
    }
}

func readValue(boxed: any) -> int {
    if boxed is not Box {
        return 0
    }
    return (boxed as Box).value
}
)";
  auto options = std::make_unique<Options>();
  options->sourceType = SourceType::STRING;
  options->source = source;
  options->implicitFilePath = "cast_flow_narrowing_warn_test.les";
  options->suppressWarnings = false;
  AnalysisResult const result = analyze(std::move(options));
  ASSERT_FALSE(result.hasErrors());
  bool found = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == AnalysisDiagnosticSeverity::Warning &&
        d.message == "Redundant cast: expression already has type Box") {
      found = true;
      break;
    }
  }
  EXPECT_FALSE(found);
}

TEST(AnalysisParseRecoveryTests, CollectsMultipleParserDiagnostics) {
  constexpr auto source = R"(let x = 1
+
let y = 2
*
let z = 3
)";
  AnalysisResult const result = analyzeSource(source);
  unsigned unknownLiteralErrors = 0;
  for (const auto& d : result.diagnostics) {
    if (d.severity == AnalysisDiagnosticSeverity::Error &&
        d.message.find("Unknown literal") != std::string::npos) {
      ++unknownLiteralErrors;
    }
  }
  EXPECT_GE(unknownLiteralErrors, 2U);
}

TEST(AnalysisParseRecoveryTests, CollectsMultipleLexerDiagnostics) {
  constexpr auto source = R"(let x = 1
@
let y = 2
`
let z = 3
)";
  AnalysisResult const result = analyzeSource(source);
  unsigned unexpectedCharErrors = 0;
  for (const auto& d : result.diagnostics) {
    if (d.severity == AnalysisDiagnosticSeverity::Error &&
        d.message.find("Unexpected character") != std::string::npos) {
      ++unexpectedCharErrors;
    }
  }
  EXPECT_GE(unexpectedCharErrors, 2U);
}

TEST(AnalysisTypecheckRecoveryTests, CollectsMultipleErrorsInIfChain) {
  constexpr auto source = R"(class Foo {
    var value: int

    func new(value: int) {
        self.value = value
    }
}
class Bar {
    var value: int

    func new(value: int) {
        self.value = value
    }
}

var foo = Foo(1)
var bar = Bar(1)

if foo < bar {
    let error = true
} else if foo > bar {
    let error = true
}

let x: bool = 4
)";
  AnalysisResult const result = analyzeSource(source);
  unsigned errors = 0;
  unsigned comparisonErrors = 0;
  unsigned assignabilityErrors = 0;
  for (const auto& d : result.diagnostics) {
    if (d.severity != AnalysisDiagnosticSeverity::Error) {
      continue;
    }
    ++errors;
    if (d.message.find("Comparison requires operands of the same enum or class type") !=
        std::string::npos) {
      ++comparisonErrors;
    }
    if (d.message.find("Variable initializer type") != std::string::npos &&
        d.message.find("assignable") != std::string::npos) {
      ++assignabilityErrors;
    }
  }
  EXPECT_GE(errors, 3U);
  EXPECT_EQ(comparisonErrors, 2U);
  EXPECT_EQ(assignabilityErrors, 1U);
}

TEST(AnalysisIndexTests, IndexesOnlyResolvedEnumMemberAccesses) {
  constexpr auto source = R"(enum Status {
    READY
}

class Holder {
    var ready: int

    func new(value: int) {
        self.ready = value
    }
}

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
  constexpr auto source = R"(enum Status {
    READY
}

class Holder {
    var ready: int

    func new(value: int) {
        self.ready = value
    }
}

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
    if (occurrence.name == "READY" &&
        occurrence.fallbackTokenKind == IndexedTokenKind::EnumMember) {
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

TEST(AnalysisIndexTests, ValuePositionGenericEnumConstructorIsIndexed) {
  constexpr auto source = R"(func wrap() -> Result<int, int> {
  return Result<int, int>.Ok(1)
}
)";

  AnalysisResult const result = analyzeSource(source);
  ASSERT_FALSE(result.hasErrors())
      << (result.diagnostics.empty() ? std::string("unknown analysis error")
                                     : result.diagnostics.front().message);

  int resultTypeOccurrenceCount = 0;
  bool sawOkConstructorUsage = false;

  for (const IndexedSymbolOccurrence& occurrence : result.index.symbolOccurrences) {
    if (occurrence.name == "Result" && occurrence.isTypePosition) {
      ASSERT_TRUE(occurrence.declaration.has_value());
      ++resultTypeOccurrenceCount;
    }
    if (occurrence.name == "Ok" && occurrence.isMemberAccess) {
      ASSERT_TRUE(occurrence.declaration.has_value());
      sawOkConstructorUsage = true;
    }
  }

  EXPECT_GE(resultTypeOccurrenceCount, 2);
  EXPECT_TRUE(sawOkConstructorUsage);
}

TEST(AnalysisIndexTests, NestedMemberAccessPreservesOuterReceiverName) {
  constexpr auto source = R"(class Payload {
    var value: int

    func new(value: int) {
        self.value = value
    }
}

class Holder {
    var payload: Payload

    func new(value: int) {
        self.payload = Payload(value)
    }
}

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
      std::filesystem::path(__FILE__).parent_path() / "lesma" / "success" /
      "import_class_method.les";

  AnalysisResult const result = analyzeFile(importClassMethodPath);
  ASSERT_FALSE(result.hasErrors());

  std::filesystem::path const importedModulePath =
      std::filesystem::path(__FILE__).parent_path() / "lesma" / "success" / "class.les";
  auto importedIt =
      result.importedModules.find(std::filesystem::weakly_canonical(importedModulePath).string());
  ASSERT_NE(importedIt, result.importedModules.end());
  ASSERT_NE(importedIt->second, nullptr);
  EXPECT_FALSE(importedIt->second->index.symbolOccurrences.empty());

  bool sawGetXDeclaration = false;
  for (const IndexedSymbolOccurrence& occurrence : importedIt->second->index.symbolOccurrences) {
    if (occurrence.name == "getX" && occurrence.fallbackTokenKind == IndexedTokenKind::Method &&
        (occurrence.modifiers & analysis_index_modifier::DECLARATION) != 0U) {
      sawGetXDeclaration = true;
    }
  }
  EXPECT_TRUE(sawGetXDeclaration);
}

TEST(TypeIdentity, FunctionTraitBoundsAffectEquality) {
  // fn<T: Iterable>(T) vs fn<T>(T) must not compare equal (same parameter/return shape).
  lesma::Type voidTy(BaseType::TY_VOID);
  lesma::Type genericT(std::string("T"));

  auto makeFn = [&](std::vector<std::vector<std::string>> bounds) -> std::unique_ptr<lesma::Type> {
    std::vector<std::unique_ptr<Field>> fields;
    fields.push_back(std::make_unique<Field>("x", &genericT));
    auto fn = std::make_unique<lesma::Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    fn->setReturnType(&voidTy);
    fn->setGenericParams({"T"});
    fn->setGenericParamTraitBounds(std::move(bounds));
    return fn;
  };

  std::unique_ptr<lesma::Type> bounded = makeFn({{"Iterable"}});
  std::unique_ptr<lesma::Type> unbounded = makeFn({{}});
  EXPECT_FALSE(bounded->isEqual(unbounded.get()));

  std::unique_ptr<lesma::Type> boundedAgain = makeFn({{"Iterable"}});
  EXPECT_TRUE(bounded->isEqual(boundedAgain.get()));
}
} // namespace

// Google Test main function
auto main(int argc, char** argv) -> int {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
