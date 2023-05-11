#pragma once

#include <memory>
#include <utility>

#include "SMLoc.h"
#include <llvm/Support/MemoryBuffer.h>


namespace lesma {
    using MemoryBuffer = llvm::MemoryBuffer;
    class SourceManager {
    private:
        class SrcBuffer {
        public:
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

            [[nodiscard]] std::string getFilename() const { return getBasename(Filepath); };

            SrcBuffer() = default;
            SrcBuffer(SrcBuffer &&Other) noexcept;
            SrcBuffer(const SrcBuffer &) = delete;
            SrcBuffer &operator=(const SrcBuffer &) = delete;
            ~SrcBuffer();

            /// The memory buffer for the file.
            std::unique_ptr<MemoryBuffer> Buffer;

            /// Vector of offsets into Buffer at which there are line-endings
            /// (lazily populated). Once populated, the '\n' that marks the end of
            /// line number N from [1..] is at Buffer[OffsetCache[N-1]]. Since
            /// these offsets are in sorted (ascending) order, they can be
            /// binary-searched for the first one after any given offset (eg. an
            /// offset corresponding to a particular SMLoc).
            ///
            /// Since we're storing offsets into relatively small files (often smaller
            /// than 2^8 or 2^16 bytes), we select the offset vector element type
            /// dynamically based on the size of Buffer.
            mutable void *OffsetCache = nullptr;

            /// The filepath of the memory buffer (for diagnostic purposes)
            std::string Filepath;

        private:
            static std::string getBasename(const std::string &file_path) {
                auto filename = file_path.substr(file_path.find_last_of("/\\") + 1);
                auto filename_wo_ext = filename.substr(0, filename.find_last_of('.'));

                return filename_wo_ext;
            }
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
        unsigned AddNewSourceBuffer(std::unique_ptr<MemoryBuffer> F, std::string filepath) {
            SrcBuffer NB;
            NB.Buffer = std::move(F);
            NB.Filepath = std::move(filepath);
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
        [[nodiscard]] unsigned FindLineNumber(SMLoc Loc) const {
            return getLineAndColumn(Loc).first;
        }

        /// Find the line and column number for the specified location in the
        /// specified file. This is not a fast method.
        [[nodiscard]] std::pair<unsigned, unsigned> getLineAndColumn(SMLoc Loc) const;

        /// Given a line and column number in a mapped buffer, turn it into an SMLoc.
        /// This will return a null SMLoc if the line/column location is invalid.
        [[nodiscard]] SMLoc FindLocForLineAndColumn(unsigned BufferID, unsigned LineNo,
                                                    unsigned ColNo) const;
    };
}// namespace lesma