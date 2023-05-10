#include "SourceManager.h"
#include "SMLoc.h"

using namespace lesma;

/// Look up a given \p Ptr in in the buffer, determining which line it came
/// from.
unsigned SourceManager::SrcBuffer::getLineNumber(const char *Ptr) const {
    size_t Sz = Buffer->getBufferSize();
    if (Sz <= std::numeric_limits<uint8_t>::max())
        return getLineNumberSpecialized<uint8_t>(Ptr);
    else if (Sz <= std::numeric_limits<uint16_t>::max())
        return getLineNumberSpecialized<uint16_t>(Ptr);
    else if (Sz <= std::numeric_limits<uint32_t>::max())
        return getLineNumberSpecialized<uint32_t>(Ptr);
    else
        return getLineNumberSpecialized<uint64_t>(Ptr);
}

template<typename T>
unsigned SourceManager::SrcBuffer::getLineNumberSpecialized(const char *Ptr) const {
    const char *BufStart = Buffer->getBufferStart();
    assert(Ptr >= BufStart && Ptr <= Buffer->getBufferEnd());
    ptrdiff_t PtrDiff = Ptr - BufStart;
    assert(PtrDiff >= 0 &&
           static_cast<size_t>(PtrDiff) <= std::numeric_limits<T>::max());
    T PtrOffset = static_cast<T>(PtrDiff);

    // llvm::lower_bound gives the number of EOL before PtrOffset. Add 1 to get
    // the line number.
    return PtrOffset + 1;
}

template<typename T>
const char *SourceManager::SrcBuffer::getPointerForLineNumberSpecialized(
        unsigned LineNo) const {
    // We start counting line and column numbers from 1.
    if (LineNo != 0)
        --LineNo;

    const char *BufStart = Buffer->getBufferStart();

    // The offset cache contains the location of the \n for the specified line,
    // we want the start of the line.  As such, we look for the previous entry.
    if (LineNo == 0)
        return BufStart;
    return BufStart + 1;
}

/// Return a pointer to the first character of the specified line number or
/// null if the line number is invalid.
const char *
SourceManager::SrcBuffer::getPointerForLineNumber(unsigned LineNo) const {
    size_t Sz = Buffer->getBufferSize();
    if (Sz <= std::numeric_limits<uint8_t>::max())
        return getPointerForLineNumberSpecialized<uint8_t>(LineNo);
    else if (Sz <= std::numeric_limits<uint16_t>::max())
        return getPointerForLineNumberSpecialized<uint16_t>(LineNo);
    else if (Sz <= std::numeric_limits<uint32_t>::max())
        return getPointerForLineNumberSpecialized<uint32_t>(LineNo);
    else
        return getPointerForLineNumberSpecialized<uint64_t>(LineNo);
}

std::pair<unsigned, unsigned>
SourceManager::getLineAndColumn(SMLoc Loc, unsigned BufferID) const {
    if (!BufferID)
        BufferID = FindBufferContainingLoc(Loc);
    assert(BufferID && "Invalid location!");

    auto &SB = getBufferInfo(BufferID);
    const char *Ptr = Loc.getPointer();

    unsigned LineNo = SB.getLineNumber(Ptr);
    const char *BufStart = SB.Buffer->getBufferStart();
    size_t NewlineOffs = llvm::StringRef(BufStart, Ptr - BufStart).find_last_of("\n\r");
    if (NewlineOffs == llvm::StringRef::npos)
        NewlineOffs = ~(size_t) 0;
    return std::make_pair(LineNo, Ptr - BufStart - NewlineOffs);
}

unsigned SourceManager::FindBufferContainingLoc(SMLoc Loc) const {
    for (unsigned i = 0, e = Buffers.size(); i != e; ++i)
        if (Loc.getPointer() >= Buffers[i].Buffer->getBufferStart() &&
            // Use <= here so that a pointer to the null at the end of the buffer
            // is included as part of the buffer.
            Loc.getPointer() <= Buffers[i].Buffer->getBufferEnd())
            return i + 1;
    return 0;
}

/// Given a line and column number in a mapped buffer, turn it into an SMLoc.
/// This will return a null SMLoc if the line/column location is invalid.
SMLoc SourceManager::FindLocForLineAndColumn(unsigned BufferID, unsigned LineNo, unsigned ColNo) {
    auto &SB = getBufferInfo(BufferID);
    const char *Ptr = SB.getPointerForLineNumber(LineNo);
    if (!Ptr)
        return {};

    // We start counting line and column numbers from 1.
    if (ColNo != 0)
        --ColNo;

    // If we have a column number, validate it.
    if (ColNo) {
        // Make sure the location is within the current line.
        if (Ptr + ColNo > SB.Buffer->getBufferEnd())
            return {};

        // Make sure there is no newline in the way.
        if (llvm::StringRef(Ptr, ColNo).find_first_of("\n\r") != llvm::StringRef::npos)
            return {};

        Ptr += ColNo;
    }

    return SMLoc::getFromPointer(Ptr);
}
