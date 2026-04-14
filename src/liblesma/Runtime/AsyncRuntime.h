#pragma once

#include <cstdint>

using LesmaAsyncResumeFn = void (*)(void*);
using LesmaAsyncDoneFn = bool (*)(void*);
using LesmaAsyncDestroyFn = void (*)(void*);

extern "C" {

void lesma_async_runtime_init(std::uint64_t workerCount);
void lesma_async_runtime_shutdown();
void lesma_async_runtime_register_task(void* taskHandle, LesmaAsyncResumeFn resumeFn,
                                       LesmaAsyncDoneFn doneFn, LesmaAsyncDestroyFn destroyFn);
void lesma_async_runtime_start_task(void* taskHandle);
void lesma_async_runtime_wait_task(void* taskHandle);
void lesma_async_runtime_release_task(void* taskHandle);

}
