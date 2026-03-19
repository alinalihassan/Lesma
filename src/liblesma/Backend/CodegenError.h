#pragma once

#include <sysexits.h>

#include "liblesma/Common/LesmaError.h"

namespace lesma {
class CodegenError : public LesmaErrorWithExitCode<EX_DATAERR> {
public:
  using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
};
} // namespace lesma
