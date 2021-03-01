/* ************************************************************************
 * Copyright 2016-2021 Advanced Micro Devices, Inc.
 * ************************************************************************ */
#include "rocblas_trmm.hpp"
#include "handle.hpp"
#include "logging.hpp"
#include "rocblas.h"
#include "utility.hpp"

#define STRMM_STOPPING_NB 32
#define DTRMM_STOPPING_NB 32
#define CTRMM_STOPPING_NB 16
#define ZTRMM_STOPPING_NB 16

// clang-format off
rocblas_int rocblas_get_trmm_recursive_nb(rocblas_int n)
{
    if(n > 8192) return 8192;
    else if(n > 4096) return 4096;
    else if(n > 2048) return 2048;
    else if(n > 1024) return 1024;
    else if(n >  512) return 512;
    else if(n >  256) return 256;
    else if(n >  128) return 128;
    else if(n >   64) return 64;
    else if(n >   32) return 32;
    else if(n >   16) return 16;
    else if(n >    8) return 8;
    else if(n >    4) return 4;
    else if(n >    2) return 2;
    else              return 1;
}
// clang-format on

namespace
{
    template <typename>
    constexpr char rocblas_trmm_name[] = "unknown";
    template <>
    constexpr char rocblas_trmm_name<float>[] = "rocblas_strmm";
    template <>
    constexpr char rocblas_trmm_name<double>[] = "rocblas_dtrmm";
    template <>
    constexpr char rocblas_trmm_name<rocblas_float_complex>[] = "rocblas_ctrmm";
    template <>
    constexpr char rocblas_trmm_name<rocblas_double_complex>[] = "rocblas_ztrmm";

    template <int STOPPING_NB, typename T>
    rocblas_status rocblas_trmm_impl(rocblas_handle    handle,
                                     rocblas_side      side,
                                     rocblas_fill      uplo,
                                     rocblas_operation transa,
                                     rocblas_diagonal  diag,
                                     rocblas_int       m,
                                     rocblas_int       n,
                                     const T*          alpha,
                                     const T*          a,
                                     rocblas_int       lda,
                                     T*                b,
                                     rocblas_int       ldb)
    {
        if(!handle)
            return rocblas_status_invalid_handle;

        RETURN_ZERO_DEVICE_MEMORY_SIZE_IF_QUERIED(handle);

        // Copy alpha and beta to host if on device. This is because gemm is called and it
        // requires alpha and beta to be on host
        T        alpha_h, beta_h;
        const T* beta = nullptr;
        RETURN_IF_ROCBLAS_ERROR(
            copy_alpha_beta_to_host_if_on_device(handle, alpha, beta, alpha_h, beta_h, m && n));
        auto saved_pointer_mode = handle->push_pointer_mode(rocblas_pointer_mode_host);

        auto layer_mode = handle->layer_mode;
        if(layer_mode
               & (rocblas_layer_mode_log_trace | rocblas_layer_mode_log_bench
                  | rocblas_layer_mode_log_profile)
           && (!handle->is_device_memory_size_query()))
        {
            auto side_letter   = rocblas_side_letter(side);
            auto uplo_letter   = rocblas_fill_letter(uplo);
            auto transa_letter = rocblas_transpose_letter(transa);
            auto diag_letter   = rocblas_diag_letter(diag);

            if(layer_mode & rocblas_layer_mode_log_trace)
                log_trace(handle,
                          rocblas_trmm_name<T>,
                          side,
                          uplo,
                          transa,
                          diag,
                          m,
                          n,
                          LOG_TRACE_SCALAR_VALUE(handle, alpha),
                          a,
                          lda,
                          b,
                          ldb);

            if(layer_mode & rocblas_layer_mode_log_bench)
                log_bench(handle,
                          "./rocblas-bench -f trmm -r",
                          rocblas_precision_string<T>,
                          "--side",
                          side_letter,
                          "--uplo",
                          uplo_letter,
                          "--transposeA",
                          transa_letter,
                          "--diag",
                          diag_letter,
                          "-m",
                          m,
                          "-n",
                          n,
                          LOG_BENCH_SCALAR_VALUE(handle, alpha),
                          "--lda",
                          lda,
                          "--ldb",
                          ldb);

            if(layer_mode & rocblas_layer_mode_log_profile)
                log_profile(handle,
                            rocblas_trmm_name<T>,
                            "side",
                            side_letter,
                            "uplo",
                            uplo_letter,
                            "transa",
                            transa_letter,
                            "diag",
                            diag_letter,
                            "m",
                            m,
                            "n",
                            n,
                            "lda",
                            lda,
                            "ldb",
                            ldb);
        }

        rocblas_int nrowa = rocblas_side_left == side ? m : n;

        if(m < 0 || n < 0 || lda < nrowa || ldb < m)
            return rocblas_status_invalid_size;

        if(m == 0 || n == 0)
            return rocblas_status_success;

        if(!alpha || !b)
            return rocblas_status_invalid_pointer;

        rocblas_int    offset_a     = 0;
        rocblas_int    offset_b     = 0;
        rocblas_stride stride_a     = 0;
        rocblas_stride stride_b     = 0;
        rocblas_stride stride_mem   = 0;
        rocblas_int    batch_count  = 1;
        rocblas_stride stride_alpha = 0;

        if(rocblas_pointer_mode_host == handle->pointer_mode && 0 == *alpha)
        {
            PRINT_AND_RETURN_IF_ROCBLAS_ERROR(set_matrix_zero_if_alpha_zero_template(
                handle, m, n, alpha, 0, b, offset_b, ldb, stride_b, batch_count));
            return rocblas_status_success;
        }
        else if(rocblas_pointer_mode_device == handle->pointer_mode)
        {
            // set matrix to zero and continue calculation. This will give
            // the same functionality as Legacy BLAS. alpha is on device and
            // it should not be copied from device to host because this is
            // an asynchronous function and the copy would make it synchronous.
            PRINT_AND_RETURN_IF_ROCBLAS_ERROR(set_matrix_zero_if_alpha_zero_template(
                handle, m, n, alpha, 0, b, offset_b, ldb, stride_b, batch_count));
        }

        if(rocblas_pointer_mode_host == handle->pointer_mode && !a)
            return rocblas_status_invalid_pointer;

        rocblas_int a_row       = rocblas_side_left == side ? m : n;
        bool        i64_indices = (a_row * size_t(lda) > std::numeric_limits<rocblas_int>::max())
                           || (m * size_t(ldb) > std::numeric_limits<rocblas_int>::max());

        if(i64_indices)
        {
            rocblas_trmm_recursive_template<STOPPING_NB, false, T>(handle,
                                                                   side,
                                                                   uplo,
                                                                   transa,
                                                                   diag,
                                                                   m,
                                                                   n,
                                                                   alpha,
                                                                   stride_alpha,
                                                                   a,
                                                                   size_t(offset_a),
                                                                   size_t(lda),
                                                                   stride_a,
                                                                   b,
                                                                   size_t(offset_b),
                                                                   size_t(ldb),
                                                                   stride_b,
                                                                   batch_count);
        }
        else
        {
            rocblas_trmm_recursive_template<STOPPING_NB, false, T>(handle,
                                                                   side,
                                                                   uplo,
                                                                   transa,
                                                                   diag,
                                                                   m,
                                                                   n,
                                                                   alpha,
                                                                   stride_alpha,
                                                                   a,
                                                                   offset_a,
                                                                   lda,
                                                                   stride_a,
                                                                   b,
                                                                   offset_b,
                                                                   ldb,
                                                                   stride_b,
                                                                   batch_count);
        }

        return rocblas_status_success;
    }

} // namespace

/*
 * ===========================================================================
 *    C wrapper
 * ===========================================================================
 */

extern "C" {

rocblas_status rocblas_strmm(rocblas_handle    handle,
                             rocblas_side      side,
                             rocblas_fill      uplo,
                             rocblas_operation transa,
                             rocblas_diagonal  diag,
                             rocblas_int       m,
                             rocblas_int       n,
                             const float*      alpha,
                             const float*      a,
                             rocblas_int       lda,
                             float*            b,
                             rocblas_int       ldb)
try
{
    return rocblas_trmm_impl<STRMM_STOPPING_NB>(
        handle, side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
}
catch(...)
{
    return exception_to_rocblas_status();
}

rocblas_status rocblas_dtrmm(rocblas_handle    handle,
                             rocblas_side      side,
                             rocblas_fill      uplo,
                             rocblas_operation transa,
                             rocblas_diagonal  diag,
                             rocblas_int       m,
                             rocblas_int       n,
                             const double*     alpha,
                             const double*     a,
                             rocblas_int       lda,
                             double*           b,
                             rocblas_int       ldb)
try
{
    return rocblas_trmm_impl<DTRMM_STOPPING_NB>(
        handle, side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
}
catch(...)
{
    return exception_to_rocblas_status();
}

rocblas_status rocblas_ctrmm(rocblas_handle               handle,
                             rocblas_side                 side,
                             rocblas_fill                 uplo,
                             rocblas_operation            transa,
                             rocblas_diagonal             diag,
                             rocblas_int                  m,
                             rocblas_int                  n,
                             const rocblas_float_complex* alpha,
                             const rocblas_float_complex* a,
                             rocblas_int                  lda,
                             rocblas_float_complex*       b,
                             rocblas_int                  ldb)
try
{
    return rocblas_trmm_impl<CTRMM_STOPPING_NB>(
        handle, side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
}
catch(...)
{
    return exception_to_rocblas_status();
}

rocblas_status rocblas_ztrmm(rocblas_handle                handle,
                             rocblas_side                  side,
                             rocblas_fill                  uplo,
                             rocblas_operation             transa,
                             rocblas_diagonal              diag,
                             rocblas_int                   m,
                             rocblas_int                   n,
                             const rocblas_double_complex* alpha,
                             const rocblas_double_complex* a,
                             rocblas_int                   lda,
                             rocblas_double_complex*       b,
                             rocblas_int                   ldb)
try
{
    return rocblas_trmm_impl<ZTRMM_STOPPING_NB>(
        handle, side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
}
catch(...)
{
    return exception_to_rocblas_status();
}

} // extern "C"

/* ============================================================================================ */
