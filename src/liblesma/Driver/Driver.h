#pragma once

#include <string>
#include <vector>

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

#include "llvm/Support/SourceMgr.h"

namespace lesma {
    enum SourceType {
        FILE,
        STRING,
    };

    enum Debug {
        NONE = 0x00, // 00000000
        LEXER = 0x01,// 00000001
        AST = 0x02,  // 00000010
        IR = 0x04,   // 00000100
    };

    struct Options {
        SourceType sourceType;
        std::string source;
        Debug debug = NONE;
        std::string output_filename = "output";
        bool timer = false;
    };

    class Driver {
    private:
        static int BaseCompile(Options *options, bool jit);

    public:
        static int Run(Options *options);
        static int Compile(Options *options);
    };
}// namespace lesma
