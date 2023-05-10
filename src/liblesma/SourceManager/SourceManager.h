#pragma once

#include <memory>

#include "SMLoc.h"
#include <llvm/Support/MemoryBuffer.h>


namespace lesma {
    using MemoryBuffer = llvm::MemoryBuffer;
    class SourceManager {
    private:
        struct SrcBuffer {
            /// The memory buffer for the file.
            std::unique_ptr<MemoryBuffer> Buffer;

            /// Look up a given \p Ptr in in the buffer, determining which line it came
            /// from.
            [[nodiscard]] unsigned getLineNumber(const char *Ptr) const;
            template<typename T>
            [[nodiscard]] unsigned getLineNumberSpecialized(const char *Ptr) const;

            /// Return a pointer to the first character of the specified line number or
            /// null if the line number is invalid.
            [[nodiscard]] const char *getPointerForLineNumber(unsigned LineNo) const;
            template<typename T>
            [[nodiscard]] const char *getPointerForLineNumberSpecialized(unsigned LineNo) const;

            SrcBuffer() = default;
            SrcBuffer(SrcBuffer &&Other) : Buffer(std::move(Other.Buffer)) {}
            SrcBuffer(const SrcBuffer &) = delete;
            SrcBuffer &operator=(const SrcBuffer &) = delete;
        };

        /// This is all of the buffers that we are reading from.
        std::vector<SrcBuffer> Buffers;

        [[nodiscard]] bool isValidBufferID(unsigned i) const { return i && i <= Buffers.size(); }

    public:
        SourceManager() = default;
        SourceManager(const SourceManager &) = delete;
        SourceManager &operator=(const SourceManager &) = delete;
        SourceManager(SourceManager &&) = default;
        SourceManager &operator=(SourceManager &&) = default;
        ~SourceManager() = default;


        [[nodiscard]] const SrcBuffer &getBufferInfo(unsigned i) const {
            assert(isValidBufferID(i));
            return Buffers[i - 1];
        }

        [[nodiscard]] const MemoryBuffer *getMemoryBuffer(unsigned i) const {
            assert(isValidBufferID(i));
            return Buffers[i - 1].Buffer.get();
        }

        [[nodiscard]] unsigned getNumBuffers() const { return Buffers.size(); }

        [[nodiscard]] unsigned getMainFileID() const {
            assert(getNumBuffers());
            return 1;
        }

        /// Add a new source buffer to this source manager. This takes ownership of
        /// the memory buffer.
        unsigned AddNewSourceBuffer(std::unique_ptr<MemoryBuffer> F) {
            SrcBuffer NB;
            NB.Buffer = std::move(F);
            Buffers.push_back(std::move(NB));
            return Buffers.size();
        }

        /// Takes the source buffers from the given source manager and append them to
        /// the current manager. `MainBufferIncludeLoc` is an optional include
        /// location to attach to the main buffer of `SrcMgr` after it gets moved to
        /// the current manager.
        void takeSourceBuffersFrom(SourceManager &SrcMgr) {
            if (SrcMgr.Buffers.empty())
                return;

            std::move(SrcMgr.Buffers.begin(), SrcMgr.Buffers.end(),
                      std::back_inserter(Buffers));
            SrcMgr.Buffers.clear();
        }

        /// Return the ID of the buffer containing the specified location.
        ///
        /// 0 is returned if the buffer is not found.
        [[nodiscard]] unsigned FindBufferContainingLoc(SMLoc Loc) const;

        /// Find the line number for the specified location in the specified file.
        /// This is not a fast method.
        [[nodiscard]] unsigned FindLineNumber(SMLoc Loc, unsigned BufferID = 0) const {
            return getLineAndColumn(Loc, BufferID).first;
        }

        /// Find the line and column number for the specified location in the
        /// specified file. This is not a fast method.
        [[nodiscard]] std::pair<unsigned, unsigned> getLineAndColumn(SMLoc Loc, unsigned BufferID = 0) const;

        /// Given a line and column number in a mapped buffer, turn it into an SMLoc.
        /// This will return a null SMLoc if the line/column location is invalid.
        [[nodiscard]] SMLoc FindLocForLineAndColumn(unsigned BufferID, unsigned LineNo,
                                                    unsigned ColNo);
    };
}// namespace lesma