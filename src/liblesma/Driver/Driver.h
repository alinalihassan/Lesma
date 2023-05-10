#pragma once

#include <string>
#include <vector>

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

#include "plf_nanotimer.h"

namespace lesma {
    class Driver {
    public:
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

        static int Run(std::unique_ptr<Options> options);
        static int Compile(std::unique_ptr<Options> options);

    private:
        static int BaseCompile(std::unique_ptr<Options> options, bool jit);
    };
}// namespace lesma
