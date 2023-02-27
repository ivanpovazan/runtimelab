// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Adapted from a Fuzzlyn example:
// Generated by Fuzzlyn v1.5 on 2021-10-31 16:57:02
// Run on X64 Windows
// Seed: 701457804295464207
// Reduced from 37.8 KiB to 0.3 KiB in 00:00:50
// Hits JIT assert in Release:
// Assertion failed '!parentStruct->lvUndoneStructPromotion' in 'Program:M2(S0)' during 'Mark local vars' (IL size 33)
// 
//     File: D:\a\_work\1\s\src\coreclr\jit\lclvars.cpp Line: 4039
// 
using System;
using System.Runtime.CompilerServices;

public struct S0
{
    public long F0;
    public long F1;
}

public class Runtime_61074
{
    public static int Main()
    {
        S0 vr4 = new S0 { F0 = 10, F1 = -1 };
        long result = WeirdAnd(vr4);
        if (result == 10)
        {
            Console.WriteLine("PASS");
            return 100;
        }

        Console.WriteLine("FAIL: Result {0} != 10 as expected", result);
        return -1;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static long WeirdAnd(S0 arg4)
    {
        long result = (-arg4.F0 * -1) & arg4.F1--;
        return result;
    }
}
