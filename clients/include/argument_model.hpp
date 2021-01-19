/* ************************************************************************
 * Copyright 2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#pragma once

#include "rocblas_arguments.hpp"

namespace ArgumentLogging
{
    const double NA_value = -1.0; // invalid for time, GFlop, GB
}

// ArgumentModel template has a variadic list of argument enums
template <rocblas_argument... Args>
class ArgumentModel
{
    // Whether model has a particular parameter
    // TODO: Replace with C++17 fold expression ((Args == param) || ...)
    static constexpr bool has(rocblas_argument param)
    {
        for(auto x : {Args...})
            if(x == param)
                return true;
        return false;
    }

public:
    void log_perf(rocblas_ostream& name_line,
                  rocblas_ostream& val_line,
                  const Arguments& arg,
                  double           gpu_us,
                  double           gflops,
                  double           gbytes,
                  double           cpu_us,
                  double           norm1,
                  double           norm2,
                  double           norm3,
                  double           norm4)
    {
        constexpr bool has_batch_count = has(e_batch_count);
        rocblas_int    batch_count     = has_batch_count ? arg.batch_count : 1;
        rocblas_int    hot_calls       = arg.iters < 1 ? 1 : arg.iters;

        // gpu time is total cumulative over hot calls, cpu is not
        if(hot_calls > 1)
            gpu_us /= hot_calls;

        // per/us to per/sec *10^6
        double rocblas_gflops = gflops * batch_count / gpu_us * 1e6;
        double rocblas_GBps   = gbytes * batch_count / gpu_us * 1e6;

        // append performance fields
        if(gflops != ArgumentLogging::NA_value)
        {
            name_line << ",rocblas-Gflops";
            val_line << ", " << rocblas_gflops;
        }

        if(gbytes != ArgumentLogging::NA_value)
        {
            // GB/s not usually reported for non-memory bound functions
            name_line << ",rocblas-GB/s";
            val_line << ", " << rocblas_GBps;
        }

        name_line << ",us";
        val_line << ", " << gpu_us;

        if(arg.unit_check || arg.norm_check)
        {
            if(cpu_us != ArgumentLogging::NA_value)
            {
                if(gflops != ArgumentLogging::NA_value)
                {
                    double cblas_gflops = gflops * batch_count / cpu_us * 1e6;
                    name_line << ",CPU-Gflops";
                    val_line << "," << cblas_gflops;
                }

                name_line << ",CPU-us";
                val_line << "," << cpu_us;
            }
            if(arg.norm_check)
            {
                if(norm1 != ArgumentLogging::NA_value)
                {
                    name_line << ",norm_error_1";
                    val_line << "," << norm1;
                }
                if(norm2 != ArgumentLogging::NA_value)
                {
                    name_line << ",norm_error_2";
                    val_line << "," << norm2;
                }
                if(norm3 != ArgumentLogging::NA_value)
                {
                    name_line << ",norm_error_3";
                    val_line << "," << norm3;
                }
                if(norm4 != ArgumentLogging::NA_value)
                {
                    name_line << ",norm_error_4";
                    val_line << "," << norm4;
                }
            }
        }
    }

    template <typename T>
    void log_args(rocblas_ostream& str,
                  const Arguments& arg,
                  double           gpu_us,
                  double           gflops,
                  double           gpu_bytes = ArgumentLogging::NA_value,
                  double           cpu_us    = ArgumentLogging::NA_value,
                  double           norm1     = ArgumentLogging::NA_value,
                  double           norm2     = ArgumentLogging::NA_value,
                  double           norm3     = ArgumentLogging::NA_value,
                  double           norm4     = ArgumentLogging::NA_value)
    {
        rocblas_ostream name_list;
        rocblas_ostream value_list;

        // Output (name, value) pairs to name_list and value_list
        auto print = [&, delim = ""](const char* name, auto&& value) mutable {
            name_list << delim << name;
            value_list << delim << value;
            delim = ",";
        };

        // Args is a parameter pack of type:   rocblas_argument...
        // The rocblas_argument enum values in Args correspond to the function arguments that
        // will be printed by rocblas_test or rocblas_bench. For example, the function:
        //
        //  rocblas_ddot(rocblas_handle handle,
        //                                 rocblas_int    n,
        //                                 const double*  x,
        //                                 rocblas_int    incx,
        //                                 const double*  y,
        //                                 rocblas_int    incy,
        //                                 double*        result);
        // will have <Args> = <e_N, e_incx, e_incy>
        //
        // print is a lambda defined above this comment block
        //
        // arg is an instance of the Arguments struct
        //
        // apply is a templated lambda for C++17 and a templated fuctor for C++14
        //
        // For rocblas_ddot, the following template specialization of apply will be called:
        // apply<e_N>(print, arg, T{}), apply<e_incx>(print, arg, T{}),, apply<e_incy>(print, arg, T{})
        //
        // apply in turn calls print with a string corresponding to the enum, for example "N" and the value of N
        //

#if __cplusplus >= 201703L
        // C++17
        (ArgumentsHelper::apply<Args>(print, arg, T{}), ...);
#else
        // C++14. TODO: Remove when C++17 is used
        (void)(int[]){(ArgumentsHelper::apply<Args>{}()(print, arg, T{}), 0)...};
#endif

        if(arg.timing)
            log_perf(name_list,
                     value_list,
                     arg,
                     gpu_us,
                     gflops,
                     gpu_bytes,
                     cpu_us,
                     norm1,
                     norm2,
                     norm3,
                     norm4);

        str << name_list << "\n" << value_list << std::endl;
    }
};
