﻿#ifndef GODOTJS_BENCHMARK_H
#define GODOTJS_BENCHMARK_H
#include "jsb_internal_pch.h"

#if JSB_BENCHMARK
#   define JSB_BENCHMARK_SCOPE(RegionName, DetailName) \
    static const char* __String__##RegionName##DetailName = #RegionName "." #DetailName; \
    ::jsb::internal::Benchmark __Benchmark__##RegionName##DetailName(__String__##RegionName##DetailName, __FILE__, __LINE__)
#else
#   define JSB_BENCHMARK_SCOPE(RegionName, DetailName) (void) 0
#endif

namespace jsb::internal
{
    // simple implementation of benchmark
    struct Benchmark
    {
        Benchmark(const char* p_name, const char* p_file, int p_line): name_(p_name), file_(p_file), line_(p_line)
        {
            start_ = OS::get_singleton()->get_ticks_usec();
            // OS::get_singleton()->benchmark_begin_measure(name_);
        }

        ~Benchmark()
        {
            const uint64_t total = OS::get_singleton()->get_ticks_usec() - start_;
            // ignore if finished in a jiffy
            if (total > 20000)
            {
                const double total_f = (double)total / 1000000.0;
                JSB_LOG(Debug, "slow process %s: %f s (%s:%d)", name_, total_f, file_, line_);
            }
            // OS::get_singleton()->benchmark_end_measure(name_);
        }

    private:
        const char* name_;
        const char* file_;
        int line_;
        uint64_t start_;
    };
}
#endif
