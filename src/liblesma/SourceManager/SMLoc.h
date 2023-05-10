#pragma once

#include <cassert>
#include <optional>

namespace lesma {
    /// Represents a location in source code.
    class SMLoc {
        const char *Ptr = nullptr;

    public:
        SMLoc() = default;

        [[nodiscard]] bool isValid() const { return Ptr != nullptr; }

        bool operator==(const SMLoc &RHS) const { return RHS.Ptr == Ptr; }
        bool operator!=(const SMLoc &RHS) const { return RHS.Ptr != Ptr; }

        [[nodiscard]] const char *getPointer() const { return Ptr; }

        static SMLoc getFromPointer(const char *Ptr) {
            SMLoc L;
            L.Ptr = Ptr;
            return L;
        }
    };

    /// Represents a range in source code.
    ///
    /// SMRange is implemented using a half-open range, as is the convention in C++.
    /// In the string "abc", the range [1,3) represents the substring "bc", and the
    /// range [2,2) represents an empty range between the characters "b" and "c".
    class SMRange {
    public:
        SMLoc Start, End;

        SMRange() = default;
        SMRange(SMLoc St, SMLoc En) : Start(St), End(En) {
            assert(Start.isValid() && End.isValid() && "Start and End should both be valid!");
        }

        [[nodiscard]] bool isValid() const { return Start.isValid(); }
    };

}// namespace lesma