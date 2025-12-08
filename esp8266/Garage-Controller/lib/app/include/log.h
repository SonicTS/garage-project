#pragma once
/* Lightweight logging macros. Define ENABLE_LOG at build (e.g. -DENABLE_LOG=1) to enable. */
#ifdef ENABLE_LOG
#include <stdio.h>
#define LOGF(...) printf(__VA_ARGS__)
#else
#define LOGF(...) ((void)0)
#endif

/* Optional stack watermark logging: enable with -DENABLE_STACK_MON */
#if defined(ENABLE_STACK_MON)
/* Include FreeRTOS headers; if type not visible fallback to unsigned. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline void LOG_STACK(const char *tag) {
#ifdef ENABLE_LOG
    /* Use generic unsigned to avoid issues if FreeRTOS base type not visible */
    unsigned hw = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    LOGF("stack-hwm[%s]=%u words\n", tag, hw);
#endif
}
#else
#define LOG_STACK(tag) ((void)0)
#endif
