/* ************************************************************************
 * Copyright (C) 2016-2022 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell cop-
 * ies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IM-
 * PLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNE-
 * CTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ************************************************************************ */
#include "logging.hpp"
#include "rocblas_hemv_symv.hpp"

namespace
{
    template <typename>
    constexpr char rocblas_hemv_name[] = "unknown";
    template <>
    constexpr char rocblas_hemv_name<rocblas_float_complex>[] = "rocblas_chemv_batched";
    template <>
    constexpr char rocblas_hemv_name<rocblas_double_complex>[] = "rocblas_zhemv_batched";

    template <typename T>
    rocblas_status rocblas_hemv_batched_impl(rocblas_handle handle,
                                             rocblas_fill   uplo,
                                             rocblas_int    n,
                                             const T*       alpha,
                                             const T* const A[],
                                             rocblas_int    lda,
                                             const T* const x[],
                                             rocblas_int    incx,
                                             const T*       beta,
                                             T* const       y[],
                                             rocblas_int    incy,
                                             rocblas_int    batch_count)
    {
        if(!handle)
            return rocblas_status_invalid_handle;
        auto check_numerics = handle->check_numerics;
        if(!handle->is_device_memory_size_query())
        {
            auto layer_mode = handle->layer_mode;
            if(layer_mode
               & (rocblas_layer_mode_log_trace | rocblas_layer_mode_log_bench
                  | rocblas_layer_mode_log_profile))
            {
                auto uplo_letter = rocblas_fill_letter(uplo);

                if(layer_mode & rocblas_layer_mode_log_trace)
                    log_trace(handle,
                              rocblas_hemv_name<T>,
                              uplo,
                              n,
                              LOG_TRACE_SCALAR_VALUE(handle, alpha),
                              A,
                              lda,
                              x,
                              incx,
                              LOG_TRACE_SCALAR_VALUE(handle, beta),
                              y,
                              incy,
                              batch_count);

                if(layer_mode & rocblas_layer_mode_log_bench)
                    log_bench(handle,
                              "./rocblas-bench -f hemv_batched -r",
                              rocblas_precision_string<T>,
                              "--uplo",
                              uplo_letter,
                              "-n",
                              n,
                              LOG_BENCH_SCALAR_VALUE(handle, alpha),
                              "--lda",
                              lda,
                              "--incx",
                              incx,
                              LOG_BENCH_SCALAR_VALUE(handle, beta),
                              "--incy",
                              incy,
                              "--batch_count",
                              batch_count);

                if(layer_mode & rocblas_layer_mode_log_profile)
                    log_profile(handle,
                                rocblas_hemv_name<T>,
                                "uplo",
                                uplo_letter,
                                "N",
                                n,
                                "lda",
                                lda,
                                "incx",
                                incx,
                                "incy",
                                incy,
                                "batch_count",
                                batch_count);
            }
        }

        rocblas_status arg_status = rocblas_hemv_symv_arg_check<T>(handle,
                                                                   uplo,
                                                                   n,
                                                                   alpha,
                                                                   0,
                                                                   A,
                                                                   0,
                                                                   lda,
                                                                   0,
                                                                   x,
                                                                   0,
                                                                   incx,
                                                                   0,
                                                                   beta,
                                                                   0,
                                                                   y,
                                                                   0,
                                                                   incy,
                                                                   0,
                                                                   batch_count);
        if(arg_status != rocblas_status_continue)
            return arg_status;

        size_t dev_bytes = rocblas_internal_hemv_symv_kernel_workspace_size<T>(n, batch_count);
        if(handle->is_device_memory_size_query())
            return handle->set_optimal_device_memory_size(dev_bytes);

        auto w_mem = handle->device_malloc(dev_bytes);
        if(!w_mem)
            return rocblas_status_memory_error;

        // flag to check whether the kernel function being called is for hemv or symv
        // For hemv IS_HEMV = true and for SYMV IS_HEMV = false
        static constexpr bool IS_HEMV = true;

        if(check_numerics)
        {
            bool           is_input = true;
            rocblas_status hemv_check_numerics_status
                = rocblas_hemv_check_numerics(rocblas_hemv_name<T>,
                                              handle,
                                              uplo,
                                              n,
                                              A,
                                              0,
                                              lda,
                                              0,
                                              x,
                                              0,
                                              incx,
                                              0,
                                              y,
                                              0,
                                              incy,
                                              0,
                                              batch_count,
                                              check_numerics,
                                              is_input);
            if(hemv_check_numerics_status != rocblas_status_success)
                return hemv_check_numerics_status;
        }

        rocblas_status status = rocblas_internal_hemv_symv_template<IS_HEMV>(handle,
                                                                             uplo,
                                                                             n,
                                                                             alpha,
                                                                             0,
                                                                             A,
                                                                             0,
                                                                             lda,
                                                                             0,
                                                                             x,
                                                                             0,
                                                                             incx,
                                                                             0,
                                                                             beta,
                                                                             0,
                                                                             y,
                                                                             0,
                                                                             incy,
                                                                             0,
                                                                             batch_count,
                                                                             (T*)w_mem);
        if(status != rocblas_status_success)
            return status;

        if(check_numerics)
        {
            bool           is_input = false;
            rocblas_status hemv_check_numerics_status
                = rocblas_hemv_check_numerics(rocblas_hemv_name<T>,
                                              handle,
                                              uplo,
                                              n,
                                              A,
                                              0,
                                              lda,
                                              0,
                                              x,
                                              0,
                                              incx,
                                              0,
                                              y,
                                              0,
                                              incy,
                                              0,
                                              batch_count,
                                              check_numerics,
                                              is_input);
            if(hemv_check_numerics_status != rocblas_status_success)
                return hemv_check_numerics_status;
        }
        return status;
    }

} // namespace

/*
* ===========================================================================
*    C wrapper
* ===========================================================================
*/

extern "C" {

rocblas_status rocblas_chemv_batched(rocblas_handle                     handle,
                                     rocblas_fill                       uplo,
                                     rocblas_int                        n,
                                     const rocblas_float_complex*       alpha,
                                     const rocblas_float_complex* const A[],
                                     rocblas_int                        lda,
                                     const rocblas_float_complex* const x[],
                                     rocblas_int                        incx,
                                     const rocblas_float_complex*       beta,
                                     rocblas_float_complex* const       y[],
                                     rocblas_int                        incy,
                                     rocblas_int                        batch_count)
try
{
    return rocblas_hemv_batched_impl(
        handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy, batch_count);
}
catch(...)
{
    return exception_to_rocblas_status();
}

rocblas_status rocblas_zhemv_batched(rocblas_handle                      handle,
                                     rocblas_fill                        uplo,
                                     rocblas_int                         n,
                                     const rocblas_double_complex*       alpha,
                                     const rocblas_double_complex* const A[],
                                     rocblas_int                         lda,
                                     const rocblas_double_complex* const x[],
                                     rocblas_int                         incx,
                                     const rocblas_double_complex*       beta,
                                     rocblas_double_complex* const       y[],
                                     rocblas_int                         incy,
                                     rocblas_int                         batch_count)
try
{
    return rocblas_hemv_batched_impl(
        handle, uplo, n, alpha, A, lda, x, incx, beta, y, incy, batch_count);
}
catch(...)
{
    return exception_to_rocblas_status();
}

} // extern "C"
