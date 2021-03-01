/* ************************************************************************
 * Copyright 2019-2021 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#pragma once

#include "check_numerics_matrix.hpp"
#include "check_numerics_vector.hpp"
#include "gemv_device.hpp"
#include "handle.hpp"

// gemvt_sn is skinny n matrix optimizations

constexpr int rocblas_gemvt_sn_WIN()
{
    return 4;
}

constexpr int rocblas_gemvt_sn_NB()
{
    return 256;
}
inline size_t rocblas_gemvt_sn_kernel_block_count(rocblas_int m)
{
    if(m <= 0)
        m = 1; // avoid sign loss issues
    return size_t(m - 1) / (rocblas_gemvt_sn_NB() * rocblas_gemvt_sn_WIN()) + 1;
}

// gemvt_sn_crossover is n threshold to crossover back to normal (non-skinny) algorithm
template <typename T>
inline size_t rocblas_gemvt_sn_crossover()
{
    return 256;
}
template <>
inline size_t rocblas_gemvt_sn_crossover<rocblas_double>()
{
    return 128;
}
template <>
inline size_t rocblas_gemvt_sn_crossover<rocblas_float_complex>()
{
    return 64;
}
template <>
inline size_t rocblas_gemvt_sn_crossover<rocblas_double_complex>()
{
    return 16;
}

template <typename T>
inline bool rocblas_gemvt_skinny_n(rocblas_operation transA, rocblas_int m, rocblas_int n)
{
    size_t    cross_over_n    = rocblas_gemvt_sn_crossover<T>();
    const int skinny_constant = 2048;
    if(transA != rocblas_operation_none && n < cross_over_n && m >= skinny_constant * n)
        return true;
    else
        return false;
}

/*! \brief rocblas_gemv_kernel_workspace_size
    Currently only transpose/conj skinny n matrices use workspace memory, so usually returns 0
    Work buffer for column reductions: number of blocks * cols * batch_count

    @param[in]
    outputType To*
        Type of output values
    @param[in]
    m rocblas_int
        Number of rows
    @param[in]
    n rocblas_int
        Number of columns
    @param[in]
    batch_count rocblas_int
        Number of batches
    ********************************************************************/
template <typename To>
ROCBLAS_EXPORT_NOINLINE size_t rocblas_gemv_kernel_workspace_size(rocblas_operation transA,
                                                                  rocblas_int       m,
                                                                  rocblas_int       n,
                                                                  rocblas_int       batch_count = 1)
{
    if(m <= 0 || n <= 0 || batch_count <= 0)
        return 0;

    if(!rocblas_gemvt_skinny_n<To>(transA, m, n))
        return 0; // workspace only used for skinny n kernel transpose/conj. transpose

    auto blocks = rocblas_gemvt_sn_kernel_block_count(m);
    return sizeof(To) * blocks * n * batch_count;
}

template <typename T, typename U, typename V, typename W>
ROCBLAS_EXPORT_NOINLINE rocblas_status rocblas_gemv_template(rocblas_handle    handle,
                                                             rocblas_operation transA,
                                                             rocblas_int       m,
                                                             rocblas_int       n,
                                                             const U*          alpha,
                                                             rocblas_stride    stride_alpha,
                                                             const V*          A,
                                                             rocblas_int       offseta,
                                                             rocblas_int       lda,
                                                             rocblas_stride    strideA,
                                                             const V*          x,
                                                             rocblas_int       offsetx,
                                                             rocblas_int       incx,
                                                             rocblas_stride    stridex,
                                                             const U*          beta,
                                                             rocblas_stride    stride_beta,
                                                             W*                y,
                                                             rocblas_int       offsety,
                                                             rocblas_int       incy,
                                                             rocblas_stride    stridey,
                                                             rocblas_int       batch_count,
                                                             T*                work = nullptr)
{
    //quick return
    if(!m || !n || !batch_count)
        return rocblas_status_success;

    hipStream_t rocblas_stream = handle->get_stream();

    // in case of negative inc shift pointer to end of data for negative indexing tid*inc
    auto shiftx
        = incx < 0 ? offsetx - ptrdiff_t(incx) * (transA == rocblas_operation_none ? n - 1 : m - 1)
                   : offsetx;
    auto shifty
        = incy < 0 ? offsety - ptrdiff_t(incy) * (transA == rocblas_operation_none ? m - 1 : n - 1)
                   : offsety;

    bool i64_indices = n * size_t(lda) > std::numeric_limits<rocblas_int>::max();

    if(transA == rocblas_operation_none)
    {
#define gemvn_KARGS(alpha_, beta_)                                                             \
    gemvn_grid, gemvn_threads, 0, rocblas_stream, m, n, alpha_, stride_alpha, A, offseta, lda, \
        strideA, x, shiftx, incx, stridex, beta_, stride_beta, y, shifty, incy, stridey

        if(n <= 128 && m >= 2048 * n)
        {
            // skinny tuned block size

            static constexpr int GEMVN_DIM_X = 64;
            static constexpr int GEMVN_DIM_Y = 4;
            rocblas_int          blocks      = (m - 1) / (GEMVN_DIM_X * 4) + 1;
            if(std::is_same<T, rocblas_double_complex>{})
                blocks = (m - 1) / (GEMVN_DIM_X) + 1;
            dim3 gemvn_grid(blocks, batch_count);
            dim3 gemvn_threads(GEMVN_DIM_X, GEMVN_DIM_Y);

            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                if(!i64_indices)
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, rocblas_int, T>),
                                       gemvn_KARGS(alpha, beta));
                else
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, size_t, T>),
                                       gemvn_KARGS(alpha, beta));
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                if(!i64_indices)
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, rocblas_int, T>),
                                       gemvn_KARGS(*alpha, *beta));
                else
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, size_t, T>),
                                       gemvn_KARGS(*alpha, *beta));
            }
        }
        else // non-skinny
        {
            // GEMVN_DIM_Y must be at least 4, 8 * 8 is very slow only 40Gflop/s
            static constexpr int GEMVN_DIM_X = 64;
            static constexpr int GEMVN_DIM_Y = 16;
            rocblas_int          blocks      = (m - 1) / (GEMVN_DIM_X * 4) + 1;
            if(std::is_same<T, rocblas_double_complex>{})
                blocks = (m - 1) / (GEMVN_DIM_X) + 1;
            dim3 gemvn_grid(blocks, batch_count);
            dim3 gemvn_threads(GEMVN_DIM_X, GEMVN_DIM_Y);

            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                if(!i64_indices)
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, rocblas_int, T>),
                                       gemvn_KARGS(alpha, beta));
                else
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, size_t, T>),
                                       gemvn_KARGS(alpha, beta));
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                if(!i64_indices)
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, rocblas_int, T>),
                                       gemvn_KARGS(*alpha, *beta));
                else
                    hipLaunchKernelGGL((gemvn_kernel<GEMVN_DIM_X, GEMVN_DIM_Y, size_t, T>),
                                       gemvn_KARGS(*alpha, *beta));
            }
        }
#undef gemvn_KARGS
    }
    else if(transA == rocblas_operation_transpose)
    {
        // transpose
        static constexpr bool CONJ = false;
        if(m <= 64 && batch_count > 8) // few rows, e.g. qmcpack
        {
            // number of columns on the y-dim of the grid
            static constexpr int NB = 256;
            dim3                 gemvtsm_grid(batch_count);
            dim3                 gemvtsm_threads(NB);
            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                hipLaunchKernelGGL((gemvtsm_kernel<CONJ, NB, T>),
                                   gemvtsm_grid,
                                   gemvtsm_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                hipLaunchKernelGGL((gemvtsm_kernel<CONJ, NB, T>),
                                   gemvtsm_grid,
                                   gemvtsm_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   *alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   *beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
        }
        else if(work && rocblas_gemvt_skinny_n<T>(transA, m, n))
        {
            static constexpr int NB     = rocblas_gemvt_sn_NB();
            static constexpr int WIN    = rocblas_gemvt_sn_WIN();
            int                  blocks = rocblas_gemvt_sn_kernel_block_count(m);
            dim3                 gemvt_grid(blocks, batch_count);
            dim3                 gemvt_threads(NB);

#define gemvt_sn_KARGS(alpha_)                                                                 \
    gemvt_grid, gemvt_threads, 0, rocblas_stream, m, n, alpha_, stride_alpha, A, offseta, lda, \
        strideA, x, shiftx, incx, stridex, (T*)work

            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                if(!i64_indices)
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, rocblas_int, T>),
                                       gemvt_sn_KARGS(alpha));
                else
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, size_t, T>),
                                       gemvt_sn_KARGS(alpha));

                hipLaunchKernelGGL((rocblas_gemvt_sn_reduce<NB, 8>),
                                   dim3(1, n, batch_count),
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   blocks,
                                   beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey,
                                   (T*)work);
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                if(!i64_indices)
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, rocblas_int, T>),
                                       gemvt_sn_KARGS(*alpha));
                else
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, size_t, T>),
                                       gemvt_sn_KARGS(*alpha));

                hipLaunchKernelGGL((rocblas_gemvt_sn_reduce<NB, 8>),
                                   dim3(1, n, batch_count),
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   blocks,
                                   *beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey,
                                   work);
            }

#undef gemvt_sn_KARGS
        }
        else
        {
            // number of columns on the y-dim of the grid
            static constexpr int NB = 256;
            dim3                 gemvt_grid(n, batch_count);
            dim3                 gemvt_threads(NB);
            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                hipLaunchKernelGGL((gemvt_kernel<CONJ, NB, T>),
                                   gemvt_grid,
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                hipLaunchKernelGGL((gemvt_kernel<CONJ, NB, T>),
                                   gemvt_grid,
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   *alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   *beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
        }
    }
    else // conjugate transpose
    {
        static constexpr bool CONJ = true;
        // conjugate transpose

        if(m <= 64 && batch_count > 8) // few rows, e.g. qmcpack
        {
            // number of columns on the y-dim of the grid
            static constexpr int NB = 256;
            dim3                 gemvtsm_grid(batch_count);
            dim3                 gemvtsm_threads(NB);
            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                hipLaunchKernelGGL((gemvtsm_kernel<CONJ, NB, T>),
                                   gemvtsm_grid,
                                   gemvtsm_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                hipLaunchKernelGGL((gemvtsm_kernel<CONJ, NB, T>),
                                   gemvtsm_grid,
                                   gemvtsm_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   *alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   *beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
        }
        else if(work && rocblas_gemvt_skinny_n<T>(transA, m, n))
        {
            static constexpr int NB     = rocblas_gemvt_sn_NB();
            static constexpr int WIN    = rocblas_gemvt_sn_WIN();
            int                  blocks = rocblas_gemvt_sn_kernel_block_count(m);
            dim3                 gemvt_grid(blocks, batch_count);
            dim3                 gemvt_threads(NB);

#define gemvt_sn_KARGS(alpha_)                                                                 \
    gemvt_grid, gemvt_threads, 0, rocblas_stream, m, n, alpha_, stride_alpha, A, offseta, lda, \
        strideA, x, shiftx, incx, stridex, (T*)work

            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                if(!i64_indices)
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, rocblas_int, T>),
                                       gemvt_sn_KARGS(alpha));
                else
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, size_t, T>),
                                       gemvt_sn_KARGS(alpha));

                hipLaunchKernelGGL((rocblas_gemvt_sn_reduce<NB, 8>),
                                   dim3(1, n, batch_count),
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   blocks,
                                   beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey,
                                   (T*)work);
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                if(!i64_indices)
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, rocblas_int, T>),
                                       gemvt_sn_KARGS(*alpha));
                else
                    hipLaunchKernelGGL((gemvt_sn_kernel<CONJ, NB, WIN, size_t, T>),
                                       gemvt_sn_KARGS(*alpha));

                hipLaunchKernelGGL((rocblas_gemvt_sn_reduce<NB, 8>),
                                   dim3(1, n, batch_count),
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   blocks,
                                   *beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey,
                                   work);
            }

#undef gemvt_sn_KARGS
        }
        else
        {
            // number of columns on the y-dim of the grid
            static constexpr int NB = 256;
            dim3                 gemvt_grid(n, batch_count);
            dim3                 gemvt_threads(NB);

            if(handle->pointer_mode == rocblas_pointer_mode_device)
            {
                hipLaunchKernelGGL((gemvt_kernel<CONJ, NB, T>),
                                   gemvt_grid,
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
            else
            {
                if(!*alpha && *beta == 1)
                    return rocblas_status_success;

                hipLaunchKernelGGL((gemvt_kernel<CONJ, NB, T>),
                                   gemvt_grid,
                                   gemvt_threads,
                                   0,
                                   rocblas_stream,
                                   m,
                                   n,
                                   *alpha,
                                   stride_alpha,
                                   A,
                                   offseta,
                                   lda,
                                   strideA,
                                   x,
                                   shiftx,
                                   incx,
                                   stridex,
                                   *beta,
                                   stride_beta,
                                   y,
                                   shifty,
                                   incy,
                                   stridey);
            }
        }
    }
    return rocblas_status_success;
}

template <typename T, typename U>
rocblas_status rocblas_gemv_check_numerics(const char*       function_name,
                                           rocblas_handle    handle,
                                           rocblas_operation trans_a,
                                           rocblas_int       m,
                                           rocblas_int       n,
                                           T                 A,
                                           rocblas_int       offset_a,
                                           rocblas_int       lda,
                                           rocblas_stride    stride_a,
                                           T                 x,
                                           rocblas_int       offset_x,
                                           rocblas_int       inc_x,
                                           rocblas_stride    stride_x,
                                           U                 y,
                                           rocblas_int       offset_y,
                                           rocblas_int       inc_y,
                                           rocblas_stride    stride_y,
                                           rocblas_int       batch_count,
                                           const int         check_numerics,
                                           bool              is_input)
{
    rocblas_status check_numerics_status
        = rocblas_check_numerics_ge_matrix_template(function_name,
                                                    handle,
                                                    rocblas_operation_none,
                                                    m,
                                                    n,
                                                    A,
                                                    offset_a,
                                                    lda,
                                                    stride_a,
                                                    batch_count,
                                                    check_numerics,
                                                    is_input);
    if(check_numerics_status != rocblas_status_success)
        return check_numerics_status;

    //Checking trans_a to transpose a vector 'x'
    rocblas_int n_x = trans_a == rocblas_operation_none ? n : m;

    check_numerics_status = rocblas_check_numerics_vector_template(function_name,
                                                                   handle,
                                                                   n_x,
                                                                   x,
                                                                   offset_x,
                                                                   inc_x,
                                                                   stride_x,
                                                                   batch_count,
                                                                   check_numerics,
                                                                   is_input);
    if(check_numerics_status != rocblas_status_success)
        return check_numerics_status;

    //Checking trans_a to transpose a vector 'y'
    rocblas_int n_y       = trans_a == rocblas_operation_none ? m : n;
    check_numerics_status = rocblas_check_numerics_vector_template(function_name,
                                                                   handle,
                                                                   n_y,
                                                                   y,
                                                                   offset_y,
                                                                   inc_y,
                                                                   stride_y,
                                                                   batch_count,
                                                                   check_numerics,
                                                                   is_input);

    return check_numerics_status;
}
