// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#nullable enable

using System;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;

internal static partial class Interop
{
    internal static partial class procfs
    {
        private const string IoFileName = "/io";

        // Counters from /proc/<pid>/io. All are cumulative for the
        // process itself (unlike stat's cutime/cstime, there is no
        // child accumulation here, so children are summed explicitly
        // by the caller, like VmRSS).
        // - RChar/WChar: bytes moved through read/write syscalls,
        //   including pipes, terminals, and page-cache hits. This is
        //   the primary IO signal: what the program asked to move.
        // - ReadBytes/WriteBytes: bytes that actually reached the
        //   block layer. A fully cached read can show ~0 here, so
        //   these are diagnostic only, not the scoring signal.
        // - Syscr/Syscw: read/write syscall counts.
        internal struct ParsedIo
        {
            internal ulong RChar;
            internal ulong WChar;
            internal ulong Syscr;
            internal ulong Syscw;
            internal ulong ReadBytes;
            internal ulong WriteBytes;
        }

        internal static string GetIoFilePathForProcess(int pid)
        {
            return RootPath + pid.ToString(CultureInfo.InvariantCulture) + IoFileName;
        }

        internal static bool TryReadIoFile(int pid, out ParsedIo result)
        {
            if (!TryReadFile(GetIoFilePathForProcess(pid), out string? fileContents))
            {
                // Between the time that we get an ID and the time that
                // we try to read the associated io file, the process
                // could be gone.
                result = default(ParsedIo);
                return false;
            }

            ParsedIo results = default(ParsedIo);
            ReadOnlySpan<char> contents = fileContents.AsSpan();
            while (!contents.IsEmpty)
            {
                int colon = contents.IndexOf(':');
                if (colon == -1)
                {
                    break;
                }

                ReadOnlySpan<char> title = contents.Slice(0, colon);
                contents = contents.Slice(colon + 1);
                int end = contents.IndexOf('\n');
                if (end == -1)
                {
                    end = contents.Length;
                }

                ReadOnlySpan<char> value = contents.Slice(0, end).Trim();
                if (title.SequenceEqual("rchar".AsSpan()))
                {
                    ulong.TryParse(value, out results.RChar);
                }
                else if (title.SequenceEqual("wchar".AsSpan()))
                {
                    ulong.TryParse(value, out results.WChar);
                }
                else if (title.SequenceEqual("syscr".AsSpan()))
                {
                    ulong.TryParse(value, out results.Syscr);
                }
                else if (title.SequenceEqual("syscw".AsSpan()))
                {
                    ulong.TryParse(value, out results.Syscw);
                }
                else if (title.SequenceEqual("read_bytes".AsSpan()))
                {
                    ulong.TryParse(value, out results.ReadBytes);
                }
                else if (title.SequenceEqual("write_bytes".AsSpan()))
                {
                    ulong.TryParse(value, out results.WriteBytes);
                }

                contents = end < contents.Length ? contents.Slice(end + 1) : default;
            }

            result = results;
            return true;
        }
    }
}
