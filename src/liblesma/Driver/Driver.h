#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace lesma {
    enum class SourceType : std::uint8_t {
        FILE,
        STRING,
    };

    enum Debug : std::uint8_t {
        NONE = 0x00, // 00000000
        LEXER = 0x01,// 00000001
        AST = 0x02,  // 00000010
        IR = 0x04,   // 00000100
        ALL = LEXER | AST | IR,
    };

    // Bitwise operators for Debug flags
    inline Debug operator|(Debug a, Debug b) {
        return static_cast<Debug>(static_cast<int>(a) | static_cast<int>(b));
    }

    inline Debug operator&(Debug a, Debug b) {
        return static_cast<Debug>(static_cast<int>(a) & static_cast<int>(b));
    }

    inline Debug &operator|=(Debug &a, Debug b) {
        return a = a | b;
    }

    struct Options {
        SourceType sourceType;
        std::string source;
        Debug debug = Debug::NONE;
        std::string output_filename = "output";
        bool timer = false;
    };

    class Driver {
    private:
        static int baseCompile(std::unique_ptr<lesma::Options> options, bool jit);

    public:
        static int run(std::unique_ptr<lesma::Options> options);
        static int compile(std::unique_ptr<lesma::Options> options);
    };
}// namespace lesma
