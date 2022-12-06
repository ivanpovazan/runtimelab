// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;

public class Test_greenthread_perf_YieldResume
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Recurse(int depth, bool rcvFirst, Socket socket, byte[] buffer)
    {
        if (depth > 0)
        {
            Recurse(depth - 1, rcvFirst, socket, buffer);
            return;
        }

        if (rcvFirst)
            socket.Receive(buffer);
        socket.Send(buffer);
        if (!rcvFirst)
            socket.Receive(buffer);
    }

    public static int Main(string[] args)
    {
        if (args.Length < 2 || args[0] != "run")
        {
            Console.WriteLine("Pass argument 'run' to run benchmark.");
            return 100;
        }

        int recurseDepth = int.Parse(args[1]);

        ThreadPool.SetMinThreads(2, 1);
        ThreadPool.SetMaxThreads(2, 1);

        var sw = new Stopwatch();
        int count = 0;

        Task.RunAsGreenThread(() =>
        {
            var buffer = new byte[1];
            var listener = new TcpListener(IPAddress.Loopback, 55555);
            listener.Start();
            var socket = listener.AcceptSocket();
            listener.Stop();

            while (true)
            {
                Recurse(recurseDepth, true, socket, buffer);
            }
        });

        Thread.Sleep(100);

        Task.RunAsGreenThread(() =>
        {
            var buffer = new byte[1];
            var client = new TcpClient();
            client.Connect(IPAddress.Loopback, 55555);
            var socket = client.Client;

            while (true)
            {
                Recurse(recurseDepth, false, socket, buffer);
                ++count;
            }
        });

        for (int i = 0; i < 8; i++)
        {
            int initialCount = count;
            sw.Restart();
            Thread.Sleep(500);
            int finalCount = count;
            sw.Stop();
            Console.WriteLine($"{sw.Elapsed.TotalNanoseconds / (finalCount - initialCount),15:0.000} ns");
        }

        return 100;
    }
}
