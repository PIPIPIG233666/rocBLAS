/* ************************************************************************
 * Copyright 2016-2022 Advanced Micro Devices, Inc.
 * ************************************************************************ */
#include "logging.hpp"
#include "rocblas_syr.hpp"
#include "utility.hpp"

namespace
{
    template <typename>
    constexpr char rocblas_syr_batched_name[] = "unknown";
    template <>
    constexpr char rocblas_syr_batched_name<float>[] = "rocblas_ssyr_batched";
    template <>
    constexpr char rocblas_syr_batched_name<double>[] = "rocblas_dsyr_batched";
    template <>
    constexpr char rocblas_syr_batched_name<rocblas_float_complex>[] = "rocblas_csyr_batched";
    template <>
    constexpr char rocblas_syr_batched_name<rocblas_double_complex>[] = "rocblas_zsyr_batched";

    template <typename T>
    rocblas_status rocblas_syr_batched_impl(rocblas_handle handle,
                                            rocblas_fill   uplo,
                                            rocblas_int    n,
                                            const T*       alpha,
                                            const T* const x[],
                                            rocblas_int    shiftx,
                                            rocblas_int    incx,
                                            T* const       A[],
                                            rocblas_int    shiftA,
                                            rocblas_int    lda,
                                            rocblas_int    batch_count)
    {
        if(!handle)
            return rocblas_status_invalid_handle;
        RETURN_ZERO_DEVICE_MEMORY_SIZE_IF_QUERIED(handle);

        auto layer_mode     = handle->layer_mode;
        auto check_numerics = handle->check_numerics;
        if(layer_mode
           & (rocblas_layer_mode_log_trace | rocblas_layer_mode_log_bench
              | rocblas_layer_mode_log_profile))
        {
            auto uplo_letter = rocblas_fill_letter(uplo);

            if(layer_mode & rocblas_layer_mode_log_trace)
                log_trace(handle,
                          rocblas_syr_batched_name<T>,
                          uplo,
                          n,
                          LOG_TRACE_SCALAR_VALUE(handle, alpha),
                          x,
                          incx,
                          A,
                          lda);

            if(layer_mode & rocblas_layer_mode_log_bench)
                log_bench(handle,
                          "./rocblas-bench -f syr_batched -r",
                          rocblas_precision_string<T>,
                          "--uplo",
                          uplo_letter,
                          "-n",
                          n,
                          LOG_BENCH_SCALAR_VALUE(handle, alpha),
                          "--incx",
                          incx,
                          "--lda",
                          lda,
                          "--batch_count",
                          batch_count);

            if(layer_mode & rocblas_layer_mode_log_profile)
                log_profile(handle,
                            rocblas_syr_batched_name<T>,
                            "uplo",
                            uplo_letter,
                            "N",
                            n,
                            "incx",
                            incx,
                            "lda",
                            lda,
                            "batch_count",
                            batch_count);
        }

        rocblas_status arg_status
            = rocblas_syr_arg_check<T>(uplo, n, alpha, 0, x, 0, incx, 0, A, 0, lda, 0, batch_count);
        if(arg_status != rocblas_status_continue)
            return arg_status;

        if(check_numerics)
        {
            bool           is_input = true;
            rocblas_status syr_check_numerics_status
                = rocblas_syr_check_numerics(rocblas_syr_batched_name<T>,
                                             handle,
                                             n,
                                             A,
                                             0,
                                             lda,
                                             0,
                                             x,
                                             0,
                                             incx,
                                             0,
                                             batch_count,
                                             check_numerics,
                                             is_input);
            if(syr_check_numerics_status != rocblas_status_success)
                return syr_check_numerics_status;
        }

        rocblas_status status = rocblas_internal_syr_template<T>(
            handle, uplo, n, alpha, 0, x, 0, incx, 0, A, 0, lda, 0, batch_count);
        if(status != rocblas_status_success)
            return status;
        if(check_numerics)
        {
            bool           is_input = false;
            rocblas_status syr_check_numerics_status
                = rocblas_syr_check_numerics(rocblas_syr_batched_name<T>,
                                             handle,
                                             n,
                                             A,
                                             0,
                                             lda,
                                             0,
                                             x,
                                             0,
                                             incx,
                                             0,
                                             batch_count,
                                             check_numerics,
                                             is_input);
            if(syr_check_numerics_status != rocblas_status_success)
                return syr_check_numerics_status;
        }
        return status;
    }

}

/*
 * ===========================================================================
 *    C wrapper
 * ===========================================================================
 */

extern "C" {

#ifdef IMPL
#error IMPL ALREADY DEFINED
#endif

#define IMPL(routine_name_, T_)                                          \
    rocblas_status routine_name_(rocblas_handle  handle,                 \
                                 rocblas_fill    uplo,                   \
                                 rocblas_int     n,                      \
                                 const T_*       alpha,                  \
                                 const T_* const x[],                    \
                                 rocblas_int     incx,                   \
                                 T_* const       A[],                    \
                                 rocblas_int     lda,                    \
                                 rocblas_int     batch_count)            \
    try                                                                  \
    {                                                                    \
        return rocblas_syr_batched_impl(                                 \
            handle, uplo, n, alpha, x, 0, incx, A, 0, lda, batch_count); \
    }                                                                    \
    catch(...)                                                           \
    {                                                                    \
        return exception_to_rocblas_status();                            \
    }

IMPL(rocblas_ssyr_batched, float);
IMPL(rocblas_dsyr_batched, double);
IMPL(rocblas_csyr_batched, rocblas_float_complex);
IMPL(rocblas_zsyr_batched, rocblas_double_complex);

#undef IMPL

} // extern "C"
