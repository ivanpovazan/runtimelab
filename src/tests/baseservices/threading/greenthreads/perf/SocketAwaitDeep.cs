// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

public class Test_greenthread_perf_YieldResume
{
    public static async Task Recurse(int depth, bool rcvFirst, Socket socket, byte[] buffer)
    {
        if (depth > 0)
        {
            await Recurse(depth - 1, rcvFirst, socket, buffer);
            return;
        }

        if (rcvFirst)
            await socket.ReceiveAsync(buffer);
        await socket.SendAsync(buffer);
        if (!rcvFirst)
            await socket.ReceiveAsync(buffer);
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

        Task.Run(async () =>
        {
            var buffer = new byte[1];
            var listener = new TcpListener(IPAddress.Loopback, 55555);
            listener.Start();
            var socket = await listener.AcceptSocketAsync();
            listener.Stop();

            while (true)
            {
                await Recurse(recurseDepth, true, socket, buffer);
            }
        });

        Thread.Sleep(100);

        Task.Run(async () =>
        {
            var buffer = new byte[1];
            var client = new TcpClient();
            await client.ConnectAsync(IPAddress.Loopback, 55555);
            var socket = client.Client;

            while (true)
            {
                await Recurse(recurseDepth, false, socket, buffer);
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
