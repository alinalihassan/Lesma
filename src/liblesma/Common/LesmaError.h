#pragma once

namespace lesma {
    class LesmaError : public std::exception {
    public:
        LesmaError() = default;
    };
}// namespace lesma