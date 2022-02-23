#ifndef CLESMA_TOKEN_TYPE_H
#define CLESMA_TOKEN_TYPE_H

namespace lesma {

enum class TokenType {
    // Single-character tokens.
    LEFT_PAREN, RIGHT_PAREN, LEFT_BRACE, RIGHT_BRACE,
    LEFT_SQUARE, RIGHT_SQUARE, COLON, ARROW, FAT_ARROW,
    COMMA, DOT, MINUS, PLUS, SEMICOLON, SLASH, STAR, MOD,
    POWER, RANGE, ELLIPSIS,

    // One or two character tokens.
    BANG, BANG_EQUAL,
    EQUAL, EQUAL_EQUAL,
    GREATER, GREATER_EQUAL,
    LESS, LESS_EQUAL,

    PLUS_EQUAL, MINUS_EQUAL, SLASH_EQUAL, STAR_EQUAL,
    MOD_EQUAL, POWER_EQUAL,

    // Literals.
    IDENTIFIER, STRING, INTEGER, DOUBLE, BOOL,

    // Types
    INT_TYPE, FLOAT_TYPE, STRING_TYPE, BOOL_TYPE, VOID_TYPE, CUSTOM_TYPE,

    // Keywords.
    AND, CLASS, ELSE, ELSE_IF, FALSE_, DEF, FOR, IF, NIL, OR, NOT, EXTERN,
    PRINT, RETURN, SUPER, THIS, TRUE_, VAR, LET, WHILE, BREAK, CONTINUE,
    CAST, IS, IN,

    EOF_TOKEN,
};

}

#endif //CLESMA_TOKEN_TYPE_H