#pragma once

#include "liblesma/Support/SMLoc.h"
#include <string>

namespace lesma {
    class Diagnostic {
    public:
        enum Severity {
            Info,
            Warning,
            Error,
        };

        Diagnostic(Severity severity, SMRange loc, std::string msg)
            : severity(severity), message(std::move(msg)), location(loc) {}

        /// Return the severity of this diagnostic.
        [[nodiscard]] Severity getSeverity() const { return severity; }

        /// Return the message of this diagnostic.
        [[nodiscard]] std::string getMessage() const { return message; }

        /// Return the location of this diagnostic.
        [[nodiscard]] SMRange getLocation() const { return location; }

        /// Return the notes of this diagnostic.
        [[nodiscard]] auto getNotes() const { return &notes; }

        /// Attach a note to this diagnostic.
        Diagnostic &attachNote(const std::string &msg,
                               std::optional<SMRange> noteLoc) {
            assert(getSeverity() != Severity::Info &&
                   "cannot attach a Note to a Note");
            notes.emplace_back(
                    new Diagnostic(Severity::Info, noteLoc.value_or(location), msg));
            return *notes.back();
        }

    private:
        /// The severity of this diagnostic.
        Severity severity;
        /// The message held by this diagnostic.
        std::string message;
        /// The raw location of this diagnostic.
        SMRange location;
        /// Any additional note diagnostics attached to this diagnostic.
        std::vector<std::unique_ptr<Diagnostic>> notes;
    };
}// namespace lesma