<#
.SYNOPSIS
    Shared functions of Lucent's performance harness (tools\bench, docs\performance, ADR 0021).

.DESCRIPTION
    - BenchProcess (C#, compiled once per session with Add-Type): starts a program with its standard
      output and error captured line by line (each line with its arrival time), samples the program
      and every process it starts (Lucent.exe and its lucent-worker.exe processes) at a fixed
      interval, and keeps a handle to each of them so that a process that has ended still reports
      its exact CPU time. Memory per process: commit (private bytes) and working set; their peaks
      are the operating system's own peak counters, read at every sample and once more after the
      process ended (the handle keeps its final values).
      System-wide: CPU use of every logical processor (GetSystemTimes) and available physical
      memory.
    - Invoke-BenchProcess: runs one program through BenchProcess with a timeout and an optional
      per-tick script block (fault injection, output watchers).
    - ConvertFrom-LucentLog: parses the "lucent: measure: ..." lines of Lucent.exe's developer
      automation (apps\lucent\src\developer_automation.hpp) and its batch progress lines.
    - Get-BenchStatistics: n, median, p95 (nearest rank), min, max, mean, sample standard deviation
      and coefficient of variation.
    - Test-BenchBatchReport: checks a batch report ("lucent.batch-report" v1) against the outputs
      a watcher hashed while the batch ran.
    - Machine facts, fixture lists, hard-link corpora, unbuffered copies and JSON helpers.
    - Get-BenchScenarioOrder: the scenarios in their fixed order ("startup" last), and
      Wait-BenchQuiet: waits for a quiet machine before measuring (W12 release phase, W13 note).

    Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.
#>

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:NativeSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace LucentBench
{
    public sealed class OutputLine
    {
        public double Seconds;  // arrival, since the program was started
        public string Text;
    }

    public sealed class Sample
    {
        public double Seconds;
        public double RootPrivateMiB;
        public double RootWorkingSetMiB;
        public double ChildrenPrivateMiB;     // every live descendant together
        public double ChildrenWorkingSetMiB;
        public int Children;
        public double TreeCpuPercent;         // the program and its descendants, of all logical processors
        public double SystemCpuPercent;       // every process of the machine, of all logical processors
        public double SystemAvailableMiB;
    }

    public sealed class ProcessRecord
    {
        public int Pid;
        public int ParentPid;
        public string Name;
        public double StartSeconds;           // since the program was started
        public double EndSeconds = -1.0;      // -1 while it runs
        public int ExitCode = -1;
        public double PeakWorkingSetMiB;
        public double PeakCommitMiB;
        public double LastPrivateMiB;
        public double LastWorkingSetMiB;
        public double UserSeconds;
        public double KernelSeconds;
        internal IntPtr Handle;
        internal long CreationTicks;
        internal long CpuTicks;
    }

    internal static class Native
    {
        public const uint TH32CS_SNAPPROCESS = 0x00000002;
        public const uint PROCESS_TERMINATE = 0x0001;
        public const uint PROCESS_VM_READ = 0x0010;
        public const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        public const uint SYNCHRONIZE = 0x00100000;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct PROCESSENTRY32W
        {
            public uint dwSize;
            public uint cntUsage;
            public uint th32ProcessID;
            public IntPtr th32DefaultHeapID;
            public uint th32ModuleID;
            public uint cntThreads;
            public uint th32ParentProcessID;
            public int pcPriClassBase;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szExeFile;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PROCESS_MEMORY_COUNTERS_EX
        {
            public uint cb;
            public uint PageFaultCount;
            public UIntPtr PeakWorkingSetSize;
            public UIntPtr WorkingSetSize;
            public UIntPtr QuotaPeakPagedPoolUsage;
            public UIntPtr QuotaPagedPoolUsage;
            public UIntPtr QuotaPeakNonPagedPoolUsage;
            public UIntPtr QuotaNonPagedPoolUsage;
            public UIntPtr PagefileUsage;
            public UIntPtr PeakPagefileUsage;
            public UIntPtr PrivateUsage;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct MEMORYSTATUSEX
        {
            public uint dwLength;
            public uint dwMemoryLoad;
            public ulong ullTotalPhys;
            public ulong ullAvailPhys;
            public ulong ullTotalPageFile;
            public ulong ullAvailPageFile;
            public ulong ullTotalVirtual;
            public ulong ullAvailVirtual;
            public ulong ullAvailExtendedVirtual;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern bool Process32FirstW(IntPtr snapshot, ref PROCESSENTRY32W entry);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern bool Process32NextW(IntPtr snapshot, ref PROCESSENTRY32W entry);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel,
                                                  out long user);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
        [DllImport("kernel32.dll", SetLastError = true, EntryPoint = "K32GetProcessMemoryInfo")]
        public static extern bool GetProcessMemoryInfo(IntPtr process, ref PROCESS_MEMORY_COUNTERS_EX counters,
                                                       uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX buffer);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool TerminateProcess(IntPtr process, uint exitCode);
    }

    // A synthetic CFA DNG (DNG 1.4, little endian, one uncompressed 16-bit RGGB strip; Make "Lucent",
    // Model "Synthetic DNG"), the tag set of Lucent's own test DNGs (src/raw/tests/synthetic_dng.hpp)
    // with a scene: a lit gradient with darker and brighter patches, per-channel camera gains and
    // seeded noise, so every file has its own bytes and a photograph's JPEG workload.
    public static class SyntheticDng
    {
        private const ushort Byte = 1, Ascii = 2, Short = 3, Long = 4, Rational = 5, SRational = 10;

        private sealed class Entry
        {
            public ushort Tag;
            public ushort Type;
            public uint Count;
            public byte[] Value;  // little endian
        }

        public static long Write(string path, int width, int height, int seed)
        {
            if (width < 32 || height < 32 || width % 2 != 0 || height % 2 != 0)
            {
                throw new ArgumentException("width and height must be even and at least 32");
            }
            long pixelBytes = 2L * width * height;
            List<Entry> entries = new List<Entry>();
            entries.Add(LongEntry(254, 0));
            entries.Add(LongEntry(256, (uint)width));
            entries.Add(LongEntry(257, (uint)height));
            entries.Add(ShortEntry(258, 16));
            entries.Add(ShortEntry(259, 1));
            entries.Add(ShortEntry(262, 32803));
            entries.Add(AsciiEntry(271, "Lucent"));
            entries.Add(AsciiEntry(272, "Synthetic DNG"));
            entries.Add(LongEntry(273, 0));  // StripOffsets, set below
            entries.Add(ShortEntry(274, 1));
            entries.Add(ShortEntry(277, 1));
            entries.Add(LongEntry(278, (uint)height));
            entries.Add(LongEntry(279, (uint)pixelBytes));
            entries.Add(ShortEntry(284, 1));
            entries.Add(ShortEntry(339, 1));
            entries.Add(new Entry { Tag = 33421, Type = Short, Count = 2, Value = new byte[] { 2, 0, 2, 0 } });
            entries.Add(new Entry { Tag = 33422, Type = Byte, Count = 4, Value = new byte[] { 0, 1, 1, 2 } });
            entries.Add(new Entry { Tag = 50706, Type = Byte, Count = 4, Value = new byte[] { 1, 4, 0, 0 } });
            entries.Add(AsciiEntry(50708, "Lucent Synthetic DNG"));
            entries.Add(new Entry { Tag = 50710, Type = Byte, Count = 3, Value = new byte[] { 0, 1, 2 } });
            entries.Add(ShortEntry(50711, 1));
            entries.Add(RationalEntry(50714, Rational, new int[] { 256, 1 }));
            entries.Add(LongEntry(50717, 16383));
            entries.Add(RationalEntry(50721, SRational, new int[] { 8000, 10000, -2000, 10000, -1000, 10000, -4000, 10000,
                12000, 10000, 1500, 10000, -500, 10000, 1800, 10000, 5500, 10000 }));
            entries.Add(RationalEntry(50728, Rational, new int[] { 1, 2, 1, 1, 2, 3 }));
            entries.Add(ShortEntry(50778, 21));

            // Header, IFD 0, out-of-line values, pixels.
            uint ifdSize = (uint)(2 + 12 * entries.Count + 4);
            uint next = 8 + ifdSize;
            uint[] offsets = new uint[entries.Count];
            for (int i = 0; i < entries.Count; i++)
            {
                if (entries[i].Value.Length > 4)
                {
                    offsets[i] = next;
                    next += (uint)((entries[i].Value.Length + 1) & ~1);
                }
            }
            uint pixelOffset = (next + 15u) & ~15u;
            entries[8].Value = BitConverter.GetBytes(pixelOffset);

            using (FileStream file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20))
            using (BinaryWriter writer = new BinaryWriter(file))
            {
                writer.Write((byte)'I'); writer.Write((byte)'I'); writer.Write((ushort)42); writer.Write((uint)8);
                writer.Write((ushort)entries.Count);
                for (int i = 0; i < entries.Count; i++)
                {
                    Entry e = entries[i];
                    writer.Write(e.Tag); writer.Write(e.Type); writer.Write(e.Count);
                    if (e.Value.Length > 4)
                    {
                        writer.Write(offsets[i]);
                    }
                    else
                    {
                        byte[] inline = new byte[4];
                        Array.Copy(e.Value, inline, e.Value.Length);
                        writer.Write(inline);
                    }
                }
                writer.Write((uint)0);
                for (int i = 0; i < entries.Count; i++)
                {
                    if (entries[i].Value.Length > 4)
                    {
                        writer.Write(entries[i].Value);
                        if ((entries[i].Value.Length & 1) != 0) { writer.Write((byte)0); }
                    }
                }
                while (file.Position < pixelOffset) { writer.Write((byte)0); }
                WritePixels(writer, width, height, seed);
            }
            return pixelOffset + pixelBytes;
        }

        private static void WritePixels(BinaryWriter writer, int width, int height, int seed)
        {
            uint state = (uint)seed * 2654435761u + 12345u;
            double[] gain = { 0.48, 1.0, 0.66 };  // RGGB: red, green, blue camera response
            double cx = 0.3 + 0.4 * ((seed * 7919) % 100) / 100.0;
            double cy = 0.3 + 0.4 * ((seed * 104729) % 100) / 100.0;
            byte[] row = new byte[2 * width];
            for (int y = 0; y < height; y++)
            {
                double v = (double)y / height;
                for (int x = 0; x < width; x++)
                {
                    double u = (double)x / width;
                    int channel = (y % 2) * 2 + (x % 2);  // 0 R, 1 G, 2 G, 3 B
                    int color = channel == 0 ? 0 : (channel == 3 ? 2 : 1);
                    double dx = u - cx, dy = v - cy;
                    double light = 0.08 + 0.55 * Math.Exp(-4.0 * (dx * dx + dy * dy)) + 0.15 * v;
                    int cell = ((int)(u * 8) + (int)(v * 6)) % 5;
                    double patch = cell == 0 ? 0.35 : (cell == 3 ? 1.6 : 1.0);
                    state = state * 1664525u + 1013904223u;
                    double noise = ((state >> 8) & 0xFFFF) / 65535.0 - 0.5;
                    double value = 256.0 + 15000.0 * light * patch * gain[color] + 90.0 * noise;
                    int sample = (int)Math.Max(256.0, Math.Min(16383.0, value));
                    row[2 * x] = (byte)(sample & 0xFF);
                    row[2 * x + 1] = (byte)(sample >> 8);
                }
                writer.Write(row);
            }
        }

        private static Entry LongEntry(ushort tag, uint value)
        {
            return new Entry { Tag = tag, Type = Long, Count = 1, Value = BitConverter.GetBytes(value) };
        }

        private static Entry ShortEntry(ushort tag, ushort value)
        {
            return new Entry { Tag = tag, Type = Short, Count = 1, Value = BitConverter.GetBytes(value) };
        }

        private static Entry AsciiEntry(ushort tag, string text)
        {
            byte[] bytes = Encoding.ASCII.GetBytes(text + "\0");
            return new Entry { Tag = tag, Type = Ascii, Count = (uint)bytes.Length, Value = bytes };
        }

        private static Entry RationalEntry(ushort tag, ushort type, int[] pairs)
        {
            byte[] bytes = new byte[4 * pairs.Length];
            for (int i = 0; i < pairs.Length; i++)
            {
                Array.Copy(BitConverter.GetBytes(pairs[i]), 0, bytes, 4 * i, 4);
            }
            return new Entry { Tag = tag, Type = type, Count = (uint)(pairs.Length / 2), Value = bytes };
        }
    }

    // Indents compact JSON two spaces per level, short arrays of numbers on one line (Windows
    // PowerShell's own ConvertTo-Json pads every level to its key's width).
    public static class JsonText
    {
        public static string Indent(string json)
        {
            StringBuilder output = new StringBuilder(json.Length + json.Length / 4);
            int depth = 0;
            bool inString = false;
            bool escaped = false;
            for (int i = 0; i < json.Length; i++)
            {
                char c = json[i];
                if (inString)
                {
                    output.Append(c);
                    if (escaped) { escaped = false; }
                    else if (c == '\\') { escaped = true; }
                    else if (c == '"') { inString = false; }
                    continue;
                }
                switch (c)
                {
                    case '"':
                        inString = true;
                        output.Append(c);
                        break;
                    case '{':
                    case '[':
                        int close = FindSimpleClose(json, i);
                        if (close > 0)
                        {
                            output.Append(json, i, close - i + 1);  // [] or a short array of scalars
                            i = close;
                            break;
                        }
                        depth++;
                        output.Append(c).Append('\n').Append(' ', 2 * depth);
                        break;
                    case '}':
                    case ']':
                        depth--;
                        output.Append('\n').Append(' ', 2 * depth).Append(c);
                        break;
                    case ',':
                        output.Append(c).Append('\n').Append(' ', 2 * depth);
                        break;
                    case ':':
                        output.Append(": ");
                        break;
                    default:
                        if (!char.IsWhiteSpace(c)) { output.Append(c); }
                        break;
                }
            }
            return output.ToString();
        }

        // The index of the bracket closing an empty container or an array of at most 12 numbers,
        // literals or short strings that starts at `open`; -1 otherwise.
        private static int FindSimpleClose(string json, int open)
        {
            char closing = json[open] == '{' ? '}' : ']';
            int items = 0;
            bool inString = false;
            bool escaped = false;
            for (int j = open + 1; j < json.Length && j - open < 160; j++)
            {
                char c = json[j];
                if (inString)
                {
                    if (escaped) { escaped = false; }
                    else if (c == '\\') { escaped = true; }
                    else if (c == '"') { inString = false; }
                    continue;
                }
                if (c == closing) { return (closing == '}' && j > open + 1) || items > 12 ? -1 : j; }
                if (c == '{' || c == '[' || c == '}' || c == ']') { return -1; }
                if (c == '"') { inString = true; }
                if (c == ',') { items++; }
            }
            return -1;
        }
    }

    // One program run: captured output, sampling of the process tree, termination.
    public sealed class BenchProcess : IDisposable
    {
        private const double MiB = 1024.0 * 1024.0;
        private readonly Process process;
        private readonly Stopwatch clock;
        private readonly object gate = new object();
        private readonly List<OutputLine> errorLines = new List<OutputLine>();
        private readonly List<OutputLine> outputLines = new List<OutputLine>();
        private int errorTaken;
        private readonly StreamWriter errorWriter;
        private readonly StreamWriter outputWriter;
        private readonly ManualResetEvent errorClosed = new ManualResetEvent(false);
        private readonly ManualResetEvent outputClosed = new ManualResetEvent(false);
        private readonly Dictionary<int, ProcessRecord> records = new Dictionary<int, ProcessRecord>();
        private readonly List<Sample> samples = new List<Sample>();
        private readonly int rootPid;
        private readonly long rootCreationTicks;
        private readonly int logicalProcessors = Environment.ProcessorCount;
        private Timer timer;
        private bool sampling;
        private bool disposed;
        private double lastSampleSeconds = -1.0;
        private long lastTreeTicks;
        private long lastIdle;
        private long lastKernel;
        private long lastUser;
        private bool haveSystemTimes;

        private BenchProcess(Process process, Stopwatch clock, StreamWriter outputWriter, StreamWriter errorWriter)
        {
            this.process = process;
            this.clock = clock;
            this.outputWriter = outputWriter;
            this.errorWriter = errorWriter;
            rootPid = process.Id;
            long creation, exit, kernel, user;
            Native.GetProcessTimes(process.Handle, out creation, out exit, out kernel, out user);
            rootCreationTicks = creation;
        }

        // Starts `fileName` with `arguments` (one command-line string). Output lines go to the two
        // files (UTF-8) and to the line lists. sampleIntervalMs 0: no sampling.
        public static BenchProcess Start(string fileName, string arguments, string workingDirectory,
                                         string stdoutPath, string stderrPath, int sampleIntervalMs)
        {
            ProcessStartInfo info = new ProcessStartInfo(fileName, arguments);
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            info.StandardOutputEncoding = new UTF8Encoding(false);
            info.StandardErrorEncoding = new UTF8Encoding(false);
            if (!string.IsNullOrEmpty(workingDirectory))
            {
                info.WorkingDirectory = workingDirectory;
            }
            StreamWriter outputWriter = new StreamWriter(stdoutPath, false, new UTF8Encoding(false));
            StreamWriter errorWriter = new StreamWriter(stderrPath, false, new UTF8Encoding(false));
            Process process = new Process();
            process.StartInfo = info;
            Stopwatch clock = Stopwatch.StartNew();
            try
            {
                process.Start();
            }
            catch
            {
                outputWriter.Dispose();
                errorWriter.Dispose();
                throw;
            }
            BenchProcess bench = new BenchProcess(process, clock, outputWriter, errorWriter);
            process.OutputDataReceived += bench.OnOutput;
            process.ErrorDataReceived += bench.OnError;
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            bench.Track(bench.rootPid, 0, Path.GetFileName(fileName));
            if (sampleIntervalMs > 0)
            {
                bench.timer = new Timer(bench.OnTimer, null, 0, sampleIntervalMs);
            }
            return bench;
        }

        // The machine's CPU use over `milliseconds`, percent of all logical processors (-1: unknown).
        public static double MeasureSystemCpu(int milliseconds)
        {
            long idle0, kernel0, user0, idle1, kernel1, user1;
            if (!Native.GetSystemTimes(out idle0, out kernel0, out user0))
            {
                return -1.0;
            }
            Thread.Sleep(milliseconds);
            if (!Native.GetSystemTimes(out idle1, out kernel1, out user1))
            {
                return -1.0;
            }
            long total = (kernel1 - kernel0) + (user1 - user0);  // kernel time includes idle time
            long busy = total - (idle1 - idle0);
            return total > 0 ? 100.0 * busy / total : 0.0;
        }

        public int Id { get { return rootPid; } }
        public double Elapsed { get { return clock.Elapsed.TotalSeconds; } }
        public bool HasExited { get { return process.HasExited; } }
        public int ExitCode { get { return process.ExitCode; } }

        public bool WaitForExit(int milliseconds)
        {
            return process.WaitForExit(milliseconds);
        }

        // Lines of standard error that arrived since the previous call.
        public OutputLine[] TakeNewErrorLines()
        {
            lock (gate)
            {
                OutputLine[] lines = errorLines.GetRange(errorTaken, errorLines.Count - errorTaken).ToArray();
                errorTaken = errorLines.Count;
                return lines;
            }
        }

        public OutputLine[] ErrorLines
        {
            get { lock (gate) { return errorLines.ToArray(); } }
        }

        public OutputLine[] OutputLines
        {
            get { lock (gate) { return outputLines.ToArray(); } }
        }

        public Sample[] Samples
        {
            get { lock (gate) { return samples.ToArray(); } }
        }

        public ProcessRecord[] Records
        {
            get
            {
                lock (gate)
                {
                    List<ProcessRecord> list = new List<ProcessRecord>(records.Values);
                    list.Sort(delegate(ProcessRecord a, ProcessRecord b) { return a.StartSeconds.CompareTo(b.StartSeconds); });
                    return list.ToArray();
                }
            }
        }

        // Live descendants named `name` (for example "lucent-worker.exe"), after a fresh look.
        public int[] LiveDescendants(string name)
        {
            lock (gate)
            {
                Discover();
                List<int> pids = new List<int>();
                foreach (ProcessRecord record in records.Values)
                {
                    if (record.Pid != rootPid && IsAlive(record) &&
                        string.Equals(record.Name, name, StringComparison.OrdinalIgnoreCase))
                    {
                        pids.Add(record.Pid);
                    }
                }
                return pids.ToArray();
            }
        }

        // Ends one descendant at once (an injected crash). False when it is not a live descendant.
        public bool TerminateDescendant(int pid, int exitCode)
        {
            lock (gate)
            {
                ProcessRecord record;
                if (pid == rootPid || !records.TryGetValue(pid, out record) || !IsAlive(record))
                {
                    return false;
                }
                return Native.TerminateProcess(record.Handle, unchecked((uint)exitCode));
            }
        }

        // Ends the program and every live descendant at once.
        public void KillTree()
        {
            lock (gate)
            {
                Discover();
                ProcessRecord root;
                if (records.TryGetValue(rootPid, out root) && IsAlive(root))
                {
                    Native.TerminateProcess(root.Handle, 1);
                }
                foreach (ProcessRecord record in records.Values)
                {
                    if (record.Pid != rootPid && IsAlive(record))
                    {
                        Native.TerminateProcess(record.Handle, 1);
                    }
                }
            }
        }

        // After the program ended: stops sampling, takes the final sample (every process's final CPU
        // time), waits up to `streamTimeoutMs` for the output streams to close and flushes the files.
        public void Finish(int streamTimeoutMs)
        {
            Timer stopped = null;
            lock (gate)
            {
                stopped = timer;
                timer = null;
            }
            if (stopped != null)
            {
                using (ManualResetEvent done = new ManualResetEvent(false))
                {
                    stopped.Dispose(done);
                    done.WaitOne(5000);
                }
            }
            SampleNow();
            // One handle at a time: WaitAll is not supported on an STA thread (Windows PowerShell).
            Stopwatch waited = Stopwatch.StartNew();
            errorClosed.WaitOne(streamTimeoutMs);
            outputClosed.WaitOne(Math.Max(0, streamTimeoutMs - (int)waited.ElapsedMilliseconds));
            lock (gate)
            {
                errorWriter.Flush();
                outputWriter.Flush();
            }
        }

        public void SampleNow()
        {
            lock (gate)
            {
                if (disposed)
                {
                    return;
                }
                TakeSample();
            }
        }

        public void Dispose()
        {
            Timer stopped;
            lock (gate)
            {
                if (disposed)
                {
                    return;
                }
                disposed = true;
                stopped = timer;
                timer = null;
            }
            if (stopped != null)
            {
                stopped.Dispose();
            }
            lock (gate)
            {
                foreach (ProcessRecord record in records.Values)
                {
                    if (record.Handle != IntPtr.Zero)
                    {
                        Native.CloseHandle(record.Handle);
                        record.Handle = IntPtr.Zero;
                    }
                }
                errorWriter.Dispose();
                outputWriter.Dispose();
            }
            process.Dispose();
        }

        private void OnOutput(object sender, DataReceivedEventArgs e)
        {
            Append(e.Data, outputLines, outputWriter, outputClosed);
        }

        private void OnError(object sender, DataReceivedEventArgs e)
        {
            Append(e.Data, errorLines, errorWriter, errorClosed);
        }

        private void Append(string text, List<OutputLine> lines, StreamWriter writer, ManualResetEvent closed)
        {
            if (text == null)
            {
                closed.Set();
                return;
            }
            lock (gate)
            {
                OutputLine line = new OutputLine();
                line.Seconds = clock.Elapsed.TotalSeconds;
                line.Text = text;
                lines.Add(line);
                if (!disposed)
                {
                    writer.WriteLine(text);
                }
            }
        }

        private void OnTimer(object state)
        {
            lock (gate)
            {
                if (disposed || sampling || timer == null)
                {
                    return;
                }
                sampling = true;
                try
                {
                    TakeSample();
                }
                finally
                {
                    sampling = false;
                }
            }
        }

        private static bool IsAlive(ProcessRecord record)
        {
            return record.Handle != IntPtr.Zero && Native.WaitForSingleObject(record.Handle, 0) != 0;
        }

        private void Track(int pid, int parentPid, string name)
        {
            IntPtr handle = Native.OpenProcess(Native.PROCESS_QUERY_LIMITED_INFORMATION | Native.PROCESS_VM_READ |
                                                   Native.PROCESS_TERMINATE | Native.SYNCHRONIZE,
                                               false, (uint)pid);
            if (handle == IntPtr.Zero)
            {
                return;
            }
            long creation, exit, kernel, user;
            if (!Native.GetProcessTimes(handle, out creation, out exit, out kernel, out user) ||
                (pid != rootPid && creation < rootCreationTicks))
            {
                // Not ours: a process id reused by a process older than the program.
                Native.CloseHandle(handle);
                return;
            }
            ProcessRecord record = new ProcessRecord();
            record.Pid = pid;
            record.ParentPid = parentPid;
            record.Name = name;
            record.Handle = handle;
            record.CreationTicks = creation;
            record.StartSeconds = pid == rootPid ? 0.0 : (creation - rootCreationTicks) / 1e7;
            records[pid] = record;
        }

        // Adds every descendant of the program not tracked yet (repeated passes: a snapshot does not
        // list parents before their children).
        private void Discover()
        {
            IntPtr snapshot = Native.CreateToolhelp32Snapshot(Native.TH32CS_SNAPPROCESS, 0);
            if (snapshot == new IntPtr(-1) || snapshot == IntPtr.Zero)
            {
                return;
            }
            List<Native.PROCESSENTRY32W> entries = new List<Native.PROCESSENTRY32W>();
            try
            {
                Native.PROCESSENTRY32W entry = new Native.PROCESSENTRY32W();
                entry.dwSize = (uint)Marshal.SizeOf(typeof(Native.PROCESSENTRY32W));
                if (Native.Process32FirstW(snapshot, ref entry))
                {
                    do
                    {
                        entries.Add(entry);
                        entry.dwSize = (uint)Marshal.SizeOf(typeof(Native.PROCESSENTRY32W));
                    } while (Native.Process32NextW(snapshot, ref entry));
                }
            }
            finally
            {
                Native.CloseHandle(snapshot);
            }
            bool added = true;
            while (added)
            {
                added = false;
                foreach (Native.PROCESSENTRY32W e in entries)
                {
                    int pid = (int)e.th32ProcessID;
                    int parent = (int)e.th32ParentProcessID;
                    if (records.ContainsKey(pid) || !records.ContainsKey(parent))
                    {
                        continue;
                    }
                    Track(pid, parent, e.szExeFile);
                    if (records.ContainsKey(pid))
                    {
                        added = true;
                    }
                }
            }
        }

        private void Read(ProcessRecord record)
        {
            if (record.Handle == IntPtr.Zero)
            {
                return;
            }
            long creation, exit, kernel, user;
            if (Native.GetProcessTimes(record.Handle, out creation, out exit, out kernel, out user))
            {
                record.CpuTicks = kernel + user;
                record.UserSeconds = user / 1e7;
                record.KernelSeconds = kernel / 1e7;
            }
            bool alive = Native.WaitForSingleObject(record.Handle, 0) != 0;
            // The peaks stay readable through the handle after the process ended (its final values).
            Native.PROCESS_MEMORY_COUNTERS_EX counters = new Native.PROCESS_MEMORY_COUNTERS_EX();
            counters.cb = (uint)Marshal.SizeOf(typeof(Native.PROCESS_MEMORY_COUNTERS_EX));
            if (Native.GetProcessMemoryInfo(record.Handle, ref counters, counters.cb))
            {
                record.PeakWorkingSetMiB = Math.Max(record.PeakWorkingSetMiB, counters.PeakWorkingSetSize.ToUInt64() / MiB);
                record.PeakCommitMiB = Math.Max(record.PeakCommitMiB, counters.PeakPagefileUsage.ToUInt64() / MiB);
                if (alive)
                {
                    record.LastPrivateMiB = counters.PrivateUsage.ToUInt64() / MiB;
                    record.LastWorkingSetMiB = counters.WorkingSetSize.ToUInt64() / MiB;
                }
            }
            if (!alive && record.EndSeconds < 0.0)
            {
                record.EndSeconds = exit > 0 ? (exit - rootCreationTicks) / 1e7 : clock.Elapsed.TotalSeconds;
                uint code;
                if (Native.GetExitCodeProcess(record.Handle, out code))
                {
                    record.ExitCode = unchecked((int)code);
                }
                record.LastPrivateMiB = 0.0;
                record.LastWorkingSetMiB = 0.0;
            }
        }

        private void TakeSample()
        {
            double now = clock.Elapsed.TotalSeconds;
            Discover();
            Sample sample = new Sample();
            sample.Seconds = now;
            long treeTicks = 0;
            foreach (ProcessRecord record in records.Values)
            {
                Read(record);
                treeTicks += record.CpuTicks;
                if (record.EndSeconds >= 0.0)
                {
                    continue;
                }
                if (record.Pid == rootPid)
                {
                    sample.RootPrivateMiB = record.LastPrivateMiB;
                    sample.RootWorkingSetMiB = record.LastWorkingSetMiB;
                }
                else
                {
                    sample.ChildrenPrivateMiB += record.LastPrivateMiB;
                    sample.ChildrenWorkingSetMiB += record.LastWorkingSetMiB;
                    sample.Children += 1;
                }
            }
            if (lastSampleSeconds >= 0.0 && now > lastSampleSeconds)
            {
                sample.TreeCpuPercent = 100.0 * (treeTicks - lastTreeTicks) / 1e7 / ((now - lastSampleSeconds) * logicalProcessors);
            }
            lastSampleSeconds = now;
            lastTreeTicks = treeTicks;
            long idle, kernel, user;
            if (Native.GetSystemTimes(out idle, out kernel, out user))
            {
                if (haveSystemTimes)
                {
                    long total = (kernel - lastKernel) + (user - lastUser);  // kernel time includes idle time
                    long busy = total - (idle - lastIdle);
                    sample.SystemCpuPercent = total > 0 ? 100.0 * busy / total : 0.0;
                }
                lastIdle = idle;
                lastKernel = kernel;
                lastUser = user;
                haveSystemTimes = true;
            }
            Native.MEMORYSTATUSEX status = new Native.MEMORYSTATUSEX();
            status.dwLength = (uint)Marshal.SizeOf(typeof(Native.MEMORYSTATUSEX));
            if (Native.GlobalMemoryStatusEx(ref status))
            {
                sample.SystemAvailableMiB = status.ullAvailPhys / MiB;
            }
            samples.Add(sample);
        }
    }
}
'@

if (-not ('LucentBench.JsonText' -as [type])) {
    Add-Type -TypeDefinition $script:NativeSource -Language CSharp
}

$script:Invariant = [System.Globalization.CultureInfo]::InvariantCulture

# ------------------------------------------------------------------------------------------------
# Command lines, numbers and JSON
# ------------------------------------------------------------------------------------------------

function ConvertTo-BenchArgumentString {
    <#
    .SYNOPSIS
        Joins arguments into one Windows command line that CommandLineToArgvW (and the MSVC CRT) split
        back into exactly these arguments.
    #>
    param([AllowEmptyCollection()][string[]]$Arguments = @())
    $parts = foreach ($argument in $Arguments) {
        if ($null -eq $argument) { $argument = '' }
        if ($argument.Length -gt 0 -and $argument -notmatch '[\s"]') {
            $argument
            continue
        }
        $backslash = [char]92
        $quote = [char]34
        $builder = New-Object System.Text.StringBuilder
        [void]$builder.Append($quote)
        $backslashes = 0
        foreach ($c in $argument.ToCharArray()) {
            if ($c -eq $backslash) {
                $backslashes++
                continue
            }
            if ($c -eq $quote) {
                [void]$builder.Append($backslash, 2 * $backslashes + 1)
                [void]$builder.Append($quote)
            } else {
                if ($backslashes -gt 0) { [void]$builder.Append($backslash, $backslashes) }
                [void]$builder.Append($c)
            }
            $backslashes = 0
        }
        if ($backslashes -gt 0) { [void]$builder.Append($backslash, 2 * $backslashes) }
        [void]$builder.Append($quote)
        $builder.ToString()
    }
    return ($parts -join ' ')
}

function ConvertTo-BenchNumber {
    # A number in invariant notation ("12", "-3.5", "1e-3"), or the text itself when it is not one.
    param([string]$Text)
    $value = 0.0
    if ([double]::TryParse($Text, [System.Globalization.NumberStyles]::Float, $script:Invariant, [ref]$value)) {
        return $value
    }
    return $Text
}

function ConvertFrom-BenchKeyValues {
    <#
    .SYNOPSIS
        Parses "key=value key=value ..." (the developer automation's measure fields) into an ordered
        dictionary; numeric values become doubles.
    #>
    param([string]$Text)
    $result = [ordered]@{}
    if ([string]::IsNullOrWhiteSpace($Text)) { return $result }
    foreach ($field in ($Text.Trim() -split '\s+')) {
        $equals = $field.IndexOf('=')
        if ($equals -le 0) { continue }
        $result[$field.Substring(0, $equals)] = ConvertTo-BenchNumber $field.Substring($equals + 1)
    }
    return $result
}

function Get-BenchPercentile {
    # Nearest-rank percentile of an ascending array (p in 0..1): the smallest value with at least
    # p of the values at or below it. With three values, p95 is the largest.
    param([double[]]$Sorted, [double]$P)
    if ($null -eq $Sorted -or $Sorted.Count -eq 0) { return $null }
    $rank = [math]::Ceiling($P * $Sorted.Count)
    if ($rank -lt 1) { $rank = 1 }
    return $Sorted[[int]$rank - 1]
}

function Get-BenchStatistics {
    <#
    .SYNOPSIS
        n, median, p95 (nearest rank), min, max, mean, sample standard deviation and coefficient of
        variation (stdev / mean) of a set of numbers; $null values are ignored.
    #>
    param([AllowNull()][AllowEmptyCollection()][object[]]$Values)
    $numbers = @($Values | Where-Object { $null -ne $_ -and $_ -isnot [string] } | ForEach-Object { [double]$_ })
    $stats = [ordered]@{ n = $numbers.Count; median = $null; p95 = $null; min = $null; max = $null; mean = $null
        stdev = $null; cv = $null }
    if ($numbers.Count -eq 0) { return $stats }
    $sorted = [double[]]($numbers | Sort-Object)
    $count = $sorted.Count
    if ($count % 2 -eq 1) {
        $median = $sorted[($count - 1) / 2]
    } else {
        $median = ($sorted[$count / 2 - 1] + $sorted[$count / 2]) / 2.0
    }
    $sum = 0.0
    foreach ($x in $sorted) { $sum += $x }
    $mean = $sum / $count
    $stats.median = $median
    $stats.p95 = Get-BenchPercentile -Sorted $sorted -P 0.95
    $stats.min = $sorted[0]
    $stats.max = $sorted[$count - 1]
    $stats.mean = $mean
    if ($count -ge 2) {
        $squares = 0.0
        foreach ($x in $sorted) { $squares += ($x - $mean) * ($x - $mean) }
        $stdev = [math]::Sqrt($squares / ($count - 1))
        $stats.stdev = $stdev
        if ($mean -ne 0) { $stats.cv = $stdev / [math]::Abs($mean) }
    }
    return $stats
}

function Get-BenchValue {
    <#
    .SYNOPSIS
        The value at a dotted path ("metrics.drag.shown_median_ms") in nested dictionaries and objects
        (ConvertFrom-Json output included); $null when any step is missing.
    #>
    param($InputObject, [Parameter(Mandatory)][string]$Path)
    $value = $InputObject
    foreach ($step in $Path.Split('.')) {
        if ($null -eq $value) { return $null }
        if ($value -is [System.Collections.IDictionary]) {
            if (-not $value.Contains($step)) { return $null }
            $value = $value[$step]
        } else {
            $property = $value.PSObject.Properties[$step]
            if ($null -eq $property) { return $null }
            $value = $property.Value
        }
    }
    return $value
}

function Write-BenchJson {
    param([Parameter(Mandatory)]$InputObject, [Parameter(Mandatory)][string]$Path)
    $folder = Split-Path -Parent $Path
    if ($folder -and -not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
    $json = [LucentBench.JsonText]::Indent(($InputObject | ConvertTo-Json -Depth 30 -Compress))
    [System.IO.File]::WriteAllText($Path, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
}

function Read-BenchJson {
    param([Parameter(Mandatory)][string]$Path)
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    return $text | ConvertFrom-Json
}

function ConvertTo-BenchRounded {
    # Rounds doubles inside nested dictionaries and arrays (JSON size and readability).
    param($Value, [int]$Digits = 3)
    if ($null -eq $Value) { return $null }
    if ($Value -is [double] -or $Value -is [single]) { return [math]::Round([double]$Value, $Digits) }
    if ($Value -is [System.Collections.IDictionary]) {
        $copy = [ordered]@{}
        foreach ($key in @($Value.Keys)) { $copy[$key] = ConvertTo-BenchRounded -Value $Value[$key] -Digits $Digits }
        return $copy
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        return , @($Value | ForEach-Object { ConvertTo-BenchRounded -Value $_ -Digits $Digits })
    }
    return $Value
}

# ------------------------------------------------------------------------------------------------
# Lucent.exe's developer automation output
# ------------------------------------------------------------------------------------------------

function ConvertFrom-LucentLog {
    <#
    .SYNOPSIS
        Parses Lucent.exe's stderr lines ("lucent: ...") into an ordered dictionary of measurements.
    .DESCRIPTION
        Keys (absent when the line was not printed): window_loaded_ms, first_frame_ms,
        first_thumbnail_ms, first_raw_preview_ms, fit_preview (visible_ms, width, height, engine_s, preview_s), zoom_preview
        (the same for "100 %"), ui (seconds, ticks, gap_median_ms, gap_p99_ms, gap_max_ms, over_50,
        over_100), edits (one per --set: label, quick, final, first_published_ms, last_published_ms,
        shown_ms), drag (label, moves, move_ms, quick, final, shown_median_ms, shown_p90_ms,
        shown_max_ms, frames, published_median_ms, published_max_ms, service_median_ms,
        final_after_release_ms, ui_gap_median_ms, ui_gap_p99_ms, ui_gap_max_ms, ui_over_50,
        ui_over_100), auto (finished_ms, message, and the "auto run" fields), auto_cancel,
        batch_id, batch_progress (seconds, done, total, completed, failed, skipped, remaining: one per
        progress line, with the line's arrival time when known), batch_finished (state, completed,
        skipped, failed, cancelled, review, folder), batch_report_html, batch_timeline,
        batch_scheduler, batch_cancel, batch_pause, process, import (added, failed), errors (lines
        that report a failure).
    .PARAMETER Lines
        Strings, or LucentBench.OutputLine objects (their arrival time is kept with progress lines).
    #>
    param([AllowEmptyCollection()][object[]]$Lines = @())
    $m = [ordered]@{ edits = @(); batch_progress = @(); errors = @() }
    $edits = New-Object System.Collections.ArrayList
    $progress = New-Object System.Collections.ArrayList
    $errors = New-Object System.Collections.ArrayList
    foreach ($item in $Lines) {
        if ($null -eq $item) { continue }
        $seconds = $null
        if ($item -is [string]) { $line = $item } else { $line = [string]$item.Text; $seconds = [double]$item.Seconds }
        if ($line -notmatch '^lucent: (.*)$') { continue }
        $text = $Matches[1]
        if ($text -match '^measure: window loaded (\d+) ms after the process started') {
            $m.window_loaded_ms = [double]$Matches[1]
        } elseif ($text -match '^measure: first frame presented (-?\d+) ms after the process started') {
            $m.first_frame_ms = [double]$Matches[1]
        } elseif ($text -match '^measure: lens database loaded (\d+) ms after the process started') {
            # W12 (APL-06): the lens database loads off the UI thread, beside the window's start.
            $m.lens_database_loaded_ms = [double]$Matches[1]
        } elseif ($text -match '^measure: first thumbnail visible (\d+) ms after the import started') {
            $m.first_thumbnail_ms = [double]$Matches[1]
        } elseif ($text -match '^measure: first RAW preview shown (\d+) ms after the import started') {
            $m.first_raw_preview_ms = [double]$Matches[1]
        } elseif ($text -match '^measure: (fit|100 %) preview visible (\d+) ms after it was requested(?:; latest render (\d+) x (\d+) px: engine ([\d.]+) s, preview ([\d.]+) s)?') {
            $preview = [ordered]@{ visible_ms = [double]$Matches[2] }
            if ($Matches[3]) {
                $preview.width = [double]$Matches[3]; $preview.height = [double]$Matches[4]
                $preview.engine_s = ConvertTo-BenchNumber $Matches[5]; $preview.preview_s = ConvertTo-BenchNumber $Matches[6]
            }
            if ($Matches[1] -eq 'fit') { $m.fit_preview = $preview } else { $m.zoom_preview = $preview }
        } elseif ($text -match '^measure: UI thread over ([\d.]+) s: (\d+) ticks of a (\d+) ms timer; gap median (\d+) ms, 99th percentile (\d+) ms, max (\d+) ms; (\d+) gaps over 50 ms, (\d+) over 100 ms') {
            $m.ui = [ordered]@{ seconds = ConvertTo-BenchNumber $Matches[1]; ticks = [double]$Matches[2]
                gap_median_ms = [double]$Matches[4]; gap_p99_ms = [double]$Matches[5]; gap_max_ms = [double]$Matches[6]
                over_50 = [double]$Matches[7]; over_100 = [double]$Matches[8] }
        } elseif ($text -match '^measure: edit (.+?): (\d+) quick and (\d+) final frames; first frame published (-?\d+) ms and last (-?\d+) ms after the change; last frame shown (-?\d+) ms after it') {
            # -1: no such frame was recorded (the window presented none meanwhile).
            $edit = [ordered]@{ label = $Matches[1]; quick = [double]$Matches[2]; final = [double]$Matches[3]
                first_published_ms = [double]$Matches[4]; last_published_ms = [double]$Matches[5]
                shown_ms = [double]$Matches[6] }
            foreach ($key in @('first_published_ms', 'last_published_ms', 'shown_ms')) {
                if ($edit[$key] -lt 0) { $edit[$key] = $null }
            }
            [void]$edits.Add($edit)
        } elseif ($text -match '^measure: drag of (\S+): (\d+) mouse moves over (\d+) ms; (\d+) quick and (\d+) final frames published') {
            if (-not $m.Contains('drag')) { $m.drag = [ordered]@{} }
            $m.drag.label = $Matches[1]; $m.drag.moves = [double]$Matches[2]; $m.drag.move_ms = [double]$Matches[3]
            $m.drag.quick = [double]$Matches[4]; $m.drag.final = [double]$Matches[5]
        } elseif ($text -match '^measure: slider-to-frame \(position to its frame shown\): median (\d+) ms, 90th percentile (\d+) ms, max (\d+) ms over (\d+) frames; to frame published on the UI thread: median (\d+) ms, max (\d+) ms; of which the develop service \(request to frame ready\): median (\d+) ms; final frame (\d+) ms after the release') {
            if (-not $m.Contains('drag')) { $m.drag = [ordered]@{} }
            $m.drag.shown_median_ms = [double]$Matches[1]; $m.drag.shown_p90_ms = [double]$Matches[2]
            $m.drag.shown_max_ms = [double]$Matches[3]; $m.drag.frames = [double]$Matches[4]
            $m.drag.published_median_ms = [double]$Matches[5]; $m.drag.published_max_ms = [double]$Matches[6]
            $m.drag.service_median_ms = [double]$Matches[7]; $m.drag.final_after_release_ms = [double]$Matches[8]
        } elseif ($text -match '^measure: UI thread during the drag \(press to final frame\): (\d+) ticks of a (\d+) ms timer; gap median (\d+) ms, 99th percentile (\d+) ms, max (\d+) ms; (\d+) gaps over 50 ms, (\d+) over 100 ms') {
            if (-not $m.Contains('drag')) { $m.drag = [ordered]@{} }
            $m.drag.ui_gap_median_ms = [double]$Matches[3]; $m.drag.ui_gap_p99_ms = [double]$Matches[4]
            $m.drag.ui_gap_max_ms = [double]$Matches[5]; $m.drag.ui_over_50 = [double]$Matches[6]
            $m.drag.ui_over_100 = [double]$Matches[7]
        } elseif ($text -match '^auto: finished after (\d+) ms: (.*)$') {
            if (-not $m.Contains('auto')) { $m.auto = [ordered]@{} }
            $m.auto.finished_ms = [double]$Matches[1]; $m.auto.message = $Matches[2]
        } elseif ($text -match '^measure: auto run: (.*)$') {
            if (-not $m.Contains('auto')) { $m.auto = [ordered]@{} }
            $fields = ConvertFrom-BenchKeyValues $Matches[1]
            foreach ($key in $fields.Keys) { $m.auto[$key] = $fields[$key] }
        } elseif ($text -match '^measure: auto cancel: (.*)$') {
            $m.auto_cancel = ConvertFrom-BenchKeyValues $Matches[1]
        } elseif ($text -match '^batch (\d+) created$') {
            $m.batch_id = [double]$Matches[1]
        } elseif ($text -match '^batch (\d+) found with work left: (\d+) of (\d+) done') {
            $m.batch_id = [double]$Matches[1]
            $m.batch_found = [ordered]@{ done = [double]$Matches[2]; total = [double]$Matches[3] }
        } elseif ($text -match '^batch (\d+): (\d+) of (\d+) done \((\d+) (?:exported|analysed), (\d+) failed, (\d+) skipped\), (\d+) remaining') {
            [void]$progress.Add([ordered]@{ seconds = $seconds; done = [double]$Matches[2]; total = [double]$Matches[3]
                    completed = [double]$Matches[4]; failed = [double]$Matches[5]; skipped = [double]$Matches[6]
                    remaining = [double]$Matches[7] })
        } elseif ($text -match '^batch (\d+) finished \((completed|cancelled)\): (\d+) (?:exported|analysed), (\d+) skipped, (\d+) failed, (\d+) cancelled, (\d+) recommended for review; output folder (.*)$') {
            $m.batch_finished = [ordered]@{ seconds = $seconds; state = $Matches[2]; completed = [double]$Matches[3]
                skipped = [double]$Matches[4]; failed = [double]$Matches[5]; cancelled = [double]$Matches[6]
                review = [double]$Matches[7]; folder = $Matches[8] }
        } elseif ($text -match '^batch (\d+) report: (.+\.html)$') {
            $m.batch_report_html = $Matches[2]
        } elseif ($text -match '^batch (\d+) report: (\d+) exported, (\d+) analysed, (\d+) skipped, (\d+) failed, (\d+) for review') {
            $m.batch_report = [ordered]@{ exported = [double]$Matches[2]; analysed = [double]$Matches[3]
                skipped = [double]$Matches[4]; failed = [double]$Matches[5]; review = [double]$Matches[6] }
        } elseif ($text -match '^measure: batch (\d+) timeline: (.*)$') {
            $m.batch_timeline = ConvertFrom-BenchKeyValues $Matches[2]
        } elseif ($text -match '^measure: batch (\d+) scheduler: (.*)$') {
            $m.batch_scheduler = ConvertFrom-BenchKeyValues $Matches[2]
        } elseif ($text -match '^measure: batch (\d+) cancel: (.*)$') {
            $m.batch_cancel = ConvertFrom-BenchKeyValues $Matches[2]
        } elseif ($text -match '^measure: batch (\d+) pause: (.*)$') {
            $m.batch_pause = ConvertFrom-BenchKeyValues $Matches[2]
        } elseif ($text -match '^measure: process: (.*)$') {
            $m.process = ConvertFrom-BenchKeyValues $Matches[1]
        } elseif ($text -match '^import finished: (\d+) added \((\d+) limited, (\d+) unsupported\), (\d+) already in library, (\d+) found again, (\d+) ignored, (\d+) failed') {
            $m.import = [ordered]@{ added = [double]$Matches[1]; limited = [double]$Matches[2]
                unsupported = [double]$Matches[3]; failed = [double]$Matches[7] }
        } elseif ($text -notmatch '^(report|auto):' -and
            $text -match 'did not (load|settle|finish) within|failed:|could not|not created|cannot start|is not available|not accepted|no batch of this library') {
            [void]$errors.Add($text)
        }
    }
    $m.edits = @($edits)
    $m.batch_progress = @($progress)
    $m.errors = @($errors)
    return $m
}

# ------------------------------------------------------------------------------------------------
# Running programs
# ------------------------------------------------------------------------------------------------

function Invoke-BenchProcess {
    <#
    .SYNOPSIS
        Runs one program through LucentBench.BenchProcess and returns what happened.
    .PARAMETER OnTick
        Called about every TickMs while the program runs, and once after it ended, as
        & $OnTick $process $newLines (LucentBench.BenchProcess, LucentBench.OutputLine[]).
    .OUTPUTS
        PSCustomObject: ExitCode, TimedOut, Seconds (start to exit, as this harness saw it), Lines
        (every stderr line with its arrival time), OutputLines, Records (the program and every
        process it started), Samples, StdErrPath, StdOutPath.
    #>
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [AllowEmptyCollection()][string[]]$ArgumentList = @(),
        [Parameter(Mandatory)][string]$LogPrefix,
        [string]$WorkingDirectory,
        [int]$TimeoutSec = 900,
        [int]$SampleMs = 0,
        [scriptblock]$OnTick,
        [int]$TickMs = 250
    )
    $folder = Split-Path -Parent $LogPrefix
    if ($folder -and -not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
    $stdout = "$LogPrefix.stdout.txt"
    $stderr = "$LogPrefix.stderr.txt"
    if (-not $WorkingDirectory) { $WorkingDirectory = (Get-Location).ProviderPath }
    $arguments = ConvertTo-BenchArgumentString -Arguments $ArgumentList
    [System.IO.File]::WriteAllText("$LogPrefix.command.txt", "`"$FilePath`" $arguments`n")
    $process = [LucentBench.BenchProcess]::Start($FilePath, $arguments, $WorkingDirectory, $stdout, $stderr, $SampleMs)
    $timedOut = $false
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
        while (-not $process.WaitForExit($TickMs)) {
            if ($OnTick) { & $OnTick $process $process.TakeNewErrorLines() }
            if ([DateTime]::UtcNow -gt $deadline) {
                $timedOut = $true
                $process.KillTree()
                [void]$process.WaitForExit(30000)
                break
            }
        }
        $exitSeconds = $process.Elapsed
        $process.Finish(15000)
        if ($OnTick) { & $OnTick $process $process.TakeNewErrorLines() }
        $exitCode = $null
        if ($process.HasExited) { $exitCode = $process.ExitCode }
        return [pscustomobject]@{
            Pid = $process.Id
            ExitCode = $exitCode
            TimedOut = $timedOut
            Seconds = $exitSeconds
            Lines = $process.ErrorLines
            OutputLines = $process.OutputLines
            Records = $process.Records
            Samples = $process.Samples
            StdErrPath = $stderr
            StdOutPath = $stdout
        }
    } finally {
        if (-not $process.HasExited) { $process.KillTree() }
        $process.Dispose()
    }
}

function Get-BenchResourceSummary {
    <#
    .SYNOPSIS
        Peaks and CPU use of an Invoke-BenchProcess result as an ordered dictionary: the program
        (root), each kind of descendant (for example lucent-worker.exe) and the samples' aggregates.
    .DESCRIPTION
        cpu_percent_of_machine: CPU time of the program and all its descendants divided by the
        program's wall time and the number of logical processors. Sampled values (the descendants'
        commit together, system CPU) depend on the sampling interval.
    #>
    param([Parameter(Mandatory)]$Result, [int]$LogicalProcessors = [Environment]::ProcessorCount)
    $records = @($Result.Records)
    $root = $records | Where-Object { $_.Pid -eq $Result.Pid } | Select-Object -First 1
    $children = @($records | Where-Object { $_ -ne $root })
    $wall = [double]$Result.Seconds
    $cpu = 0.0
    foreach ($r in $records) { $cpu += $r.UserSeconds + $r.KernelSeconds }
    $summary = [ordered]@{
        wall_s = $wall
        cpu_s = $cpu
        cpu_percent_of_machine = $null
        root = $null
        descendants = @()
        samples = 0
    }
    if ($wall -gt 0) { $summary.cpu_percent_of_machine = 100.0 * $cpu / ($wall * $LogicalProcessors) }
    if ($root) {
        $summary.root = [ordered]@{ name = $root.Name; peak_commit_mib = $root.PeakCommitMiB
            peak_working_set_mib = $root.PeakWorkingSetMiB; cpu_s = $root.UserSeconds + $root.KernelSeconds }
    }
    $groups = @($children | Group-Object -Property Name)
    $kinds = foreach ($g in $groups) {
        $items = @($g.Group)
        $cpuKind = 0.0
        foreach ($r in $items) { $cpuKind += $r.UserSeconds + $r.KernelSeconds }
        [ordered]@{ name = $g.Name; processes = $items.Count
            peak_commit_mib = Get-BenchAggregate -Values @($items | ForEach-Object { $_.PeakCommitMiB }) -Function Max
            peak_working_set_mib = Get-BenchAggregate -Values @($items | ForEach-Object { $_.PeakWorkingSetMiB }) -Function Max
            cpu_s = $cpuKind
            exit_codes = @($items | ForEach-Object { $_.ExitCode }) }
    }
    $summary.descendants = @($kinds)
    $samples = @($Result.Samples)
    $summary.samples = $samples.Count
    if ($samples.Count -gt 0) {
        $summary.sampled = [ordered]@{
            root_private_max_mib = Get-BenchAggregate -Values @($samples | ForEach-Object { $_.RootPrivateMiB }) -Function Max
            descendants_private_max_mib = Get-BenchAggregate -Values @($samples | ForEach-Object { $_.ChildrenPrivateMiB }) -Function Max
            tree_private_max_mib = Get-BenchAggregate -Function Max -Values @($samples | ForEach-Object {
                    $_.RootPrivateMiB + $_.ChildrenPrivateMiB })
            descendants_max_live = Get-BenchAggregate -Values @($samples | ForEach-Object { $_.Children }) -Function Max
            system_cpu_percent_mean = Get-BenchAggregate -Function Mean -Values @($samples | Select-Object -Skip 1 |
                    ForEach-Object { $_.SystemCpuPercent })
            tree_cpu_percent_max = Get-BenchAggregate -Values @($samples | ForEach-Object { $_.TreeCpuPercent }) -Function Max
            system_available_min_mib = Get-BenchAggregate -Values @($samples | ForEach-Object { $_.SystemAvailableMiB }) -Function Min
        }
    }
    return $summary
}

function Get-BenchAggregate {
    # Max, Min or Mean of numbers; $null for none.
    param([AllowNull()][AllowEmptyCollection()][object[]]$Values, [ValidateSet('Max', 'Min', 'Mean')][string]$Function)
    $numbers = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    if ($numbers.Count -eq 0) { return $null }
    switch ($Function) {
        'Max' { $result = $numbers[0]; foreach ($x in $numbers) { if ($x -gt $result) { $result = $x } }; return $result }
        'Min' { $result = $numbers[0]; foreach ($x in $numbers) { if ($x -lt $result) { $result = $x } }; return $result }
        default { $sum = 0.0; foreach ($x in $numbers) { $sum += $x }; return $sum / $numbers.Count }
    }
}

function Stop-BenchProcessesNamed {
    # Ends leftover processes of a previous interrupted run (only those started from $Folder).
    param([string[]]$Names, [string]$Folder)
    foreach ($name in $Names) {
        foreach ($p in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            $path = $null
            try { $path = $p.Path } catch { $path = $null }
            if ($path -and $Folder -and $path.StartsWith($Folder, [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

# ------------------------------------------------------------------------------------------------
# Fixtures, corpora and copies
# ------------------------------------------------------------------------------------------------

function Get-BenchFixtures {
    <#
    .SYNOPSIS
        The fixtures of fixtures\manifest.json that are cached, with their path, tier and the
        visible size LibRaw reports (fixtures\reference\<id>.parity.json): width, height, megapixels.
    #>
    param([Parameter(Mandatory)][string]$RepoRoot, [ValidateSet('smoke', 'all')][string]$Tier = 'all')
    $manifest = Read-BenchJson -Path (Join-Path $RepoRoot 'fixtures\manifest.json')
    foreach ($f in $manifest.fixtures) {
        if ($Tier -eq 'smoke' -and $f.tier -ne 'smoke') { continue }
        $path = Join-Path $RepoRoot ("fixtures\cache\{0}\{1}" -f $f.id, $f.file_name)
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $width = $null; $height = $null
        $reference = Join-Path $RepoRoot ("fixtures\reference\{0}.parity.json" -f $f.id)
        if (Test-Path -LiteralPath $reference) {
            $r = Read-BenchJson -Path $reference
            if ($r.PSObject.Properties['parity'] -and $r.parity -and $r.parity.PSObject.Properties['width']) {
                $width = [double]$r.parity.width; $height = [double]$r.parity.height
            }
        }
        $mp = $null
        if ($width -and $height) { $mp = $width * $height / 1e6 }
        [pscustomobject]@{ Id = $f.id; Tier = $f.tier; Make = $f.make; Model = $f.model; FileName = $f.file_name
            Path = $path; SizeBytes = (Get-Item -LiteralPath $path).Length; Width = $width; Height = $height
            Megapixels = $mp }
    }
}

function New-BenchHardLink {
    # A hard link at $Path to $Target (same volume). Falls back to a copy when links are refused.
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Target)
    $folder = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
    try {
        New-Item -ItemType HardLink -Path $Path -Target $Target -ErrorAction Stop | Out-Null
        return 'link'
    } catch {
        Copy-Item -LiteralPath $Target -Destination $Path
        return 'copy'
    }
}

function Copy-BenchTreeUnbuffered {
    <#
    .SYNOPSIS
        Copies a folder tree with unbuffered I/O (robocopy /J), so the new files' contents are not in
        the system file cache afterwards: the first program start from the copy reads them from disk.
    #>
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    & robocopy.exe $Source $Destination /E /J /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE): $Source -> $Destination" }
    $global:LASTEXITCODE = 0
}

# ------------------------------------------------------------------------------------------------
# Machine facts
# ------------------------------------------------------------------------------------------------

function Get-BenchMachineFacts {
    <#
    .SYNOPSIS
        Facts about the machine and the build that a measurement depends on.
    #>
    param([string]$RepoRoot, [string]$BuildDir)
    $facts = [ordered]@{ collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
    try {
        $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
        $facts.cpu = [ordered]@{ name = $cpu.Name.Trim(); cores = $cpu.NumberOfCores
            logical_processors = $cpu.NumberOfLogicalProcessors; max_clock_mhz = $cpu.MaxClockSpeed }
    } catch { $facts.cpu = $null }
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $cs = Get-CimInstance Win32_ComputerSystem
        $facts.memory = [ordered]@{ total_gib = [math]::Round($cs.TotalPhysicalMemory / 1GB, 2)
            available_gib_at_start = [math]::Round($os.FreePhysicalMemory * 1KB / 1GB, 2) }
        $facts.os = [ordered]@{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber
            architecture = $os.OSArchitecture }
    } catch { }
    try {
        $gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name })
        $facts.gpu = $gpu
    } catch { }
    try {
        $partition = Get-Partition -DriveLetter C -ErrorAction Stop
        $disk = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq [string]$partition.DiskNumber } | Select-Object -First 1
        if ($disk) {
            $facts.system_disk = [ordered]@{ model = $disk.FriendlyName; media = [string]$disk.MediaType
                bus = [string]$disk.BusType; size_gb = [math]::Round($disk.Size / 1e9, 0) }
        }
        $volume = Get-Volume -DriveLetter C
        $facts.system_volume = [ordered]@{ file_system = $volume.FileSystem
            free_gib = [math]::Round($volume.SizeRemaining / 1GB, 1) }
    } catch { }
    try {
        $scheme = (& powercfg.exe /getactivescheme) -join ' '
        $facts.power_scheme = ($scheme -replace '^.*:\s*', '').Trim()
    } catch { }
    try {
        $battery = Get-CimInstance Win32_Battery | Select-Object -First 1
        if ($battery) {
            # BatteryStatus 2: on AC power (Win32_Battery).
            $facts.on_ac_power = ($battery.BatteryStatus -eq 2)
            $facts.battery_percent = $battery.EstimatedChargeRemaining
        } else {
            $facts.on_ac_power = $true
        }
    } catch { }
    try {
        $defender = Get-MpComputerStatus -ErrorAction Stop
        $facts.antivirus = [ordered]@{ product = 'Microsoft Defender'
            real_time_protection = $defender.RealTimeProtectionEnabled }
    } catch { $facts.antivirus = 'unknown' }
    if ($RepoRoot) {
        try {
            $facts.git = [ordered]@{
                commit = (& git -C $RepoRoot rev-parse HEAD 2>$null)
                branch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null)
                uncommitted_paths = @(& git -C $RepoRoot status --porcelain 2>$null).Count
            }
        } catch { }
    }
    if ($BuildDir) {
        $cache = Join-Path $BuildDir 'CMakeCache.txt'
        if (Test-Path -LiteralPath $cache) {
            $values = [ordered]@{}
            foreach ($line in [System.IO.File]::ReadAllLines($cache)) {
                if ($line -match '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER_VERSION|VCPKG_TARGET_TRIPLET|LUCENT_BUILD_UI|LUCENT_WARNINGS_AS_ERRORS):[A-Z]+=(.*)$') {
                    $values[$Matches[1]] = $Matches[2]
                }
            }
            $facts.build = $values
        }
    }
    # How busy the other programs were, without naming them (the JSON is committed with a baseline;
    # Get-BenchBusyProcesses -Names shows them on the console).
    $facts.process_count = @(Get-Process).Count
    $facts.busiest_other_processes_percent = @(Get-BenchBusyProcesses -Seconds 2 | ForEach-Object { [math]::Round($_.Percent, 0) })
    return $facts
}

function Get-BenchBusyProcesses {
    # The processes that used the most CPU over $Seconds, as objects with Name and Percent (of one
    # processor); with -Names, as text "name (percent)".
    param([int]$Seconds = 2, [int]$Top = 5, [switch]$Names)
    $before = @{}
    foreach ($p in @(Get-Process)) {
        try { $before[$p.Id] = $p.TotalProcessorTime.TotalSeconds } catch { }
    }
    Start-Sleep -Seconds $Seconds
    $usage = foreach ($p in @(Get-Process)) {
        if (-not $before.ContainsKey($p.Id)) { continue }
        try {
            $delta = $p.TotalProcessorTime.TotalSeconds - $before[$p.Id]
            [pscustomobject]@{ Name = $p.ProcessName; Percent = 100.0 * $delta / $Seconds }
        } catch { }
    }
    $busiest = @($usage | Sort-Object -Property Percent -Descending | Select-Object -First $Top)
    if ($Names) { return @($busiest | ForEach-Object { '{0} ({1:N0} %)' -f $_.Name, $_.Percent }) }
    return $busiest
}

function Measure-BenchIdleCpu {
    # The machine's CPU use over $Seconds before a run (background load), percent of all processors.
    param([int]$Seconds = 5)
    return [LucentBench.BenchProcess]::MeasureSystemCpu($Seconds * 1000)
}

# The scenarios of tools\bench\Invoke-LucentBench.ps1 in the order they always run (W13 note, W12
# release phase; ADR 0021 amendment): "startup" last. Its cold starts copy the installed layout afresh
# (about 390 files) for every repetition, and the antivirus scans those copies while the next scenarios
# run: in the W13 baseline every 100 % view of the preview scenario, which ran right after, was 0.25
# to 0.68 s slower than when measured again ten minutes later.
$script:BenchScenarioOrder = @('thumbnails', 'preview', 'edit', 'auto-editor', 'auto-cancel', 'auto-analysis', 'export',
    'batch', 'batch-cancel', 'startup')

function Get-BenchScenarioOrder {
    <#
    .SYNOPSIS
        The scenarios to run, in the harness's fixed order ($script:BenchScenarioOrder: "startup" last),
        whatever order -Scenario names them in; each once. Accepts comma-separated strings (the way
        powershell -File passes -Scenario a,b). Throws for an unknown scenario. Without -Requested: all.
    #>
    param([string[]]$Requested)
    $names = @($Requested | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($names.Count -eq 0) { return $script:BenchScenarioOrder }
    foreach ($name in $names) {
        if ($script:BenchScenarioOrder -notcontains $name) { throw "Unknown scenario '$name': use $($script:BenchScenarioOrder -join ', ')." }
    }
    return @($script:BenchScenarioOrder | Where-Object { $names -contains $_ })
}

function Wait-BenchQuiet {
    <#
    .SYNOPSIS
        Waits until the machine is quiet before measuring: until $Consecutive samples in a row of the
        system's CPU use (every logical processor, $SampleMs each) are at most $MaxCpuPercent, or
        $TimeoutSeconds have passed (W13 note: an antivirus still scanning freshly copied files, an
        update or indexer at work). Returns what happened: waited_s, samples (the CPU percentages),
        quiet (false when it timed out), and the limits. $Sampler replaces the measurement (tests: a
        script block taking the sample's milliseconds and returning a percentage).
    #>
    param([double]$MaxCpuPercent = 10, [int]$Consecutive = 3, [int]$SampleMs = 2000, [int]$TimeoutSeconds = 300,
        [scriptblock]$Sampler)
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $samples = New-Object System.Collections.ArrayList
    $run = 0
    $quiet = $false
    while ($true) {
        $cpu = if ($Sampler) { [double](& $Sampler $SampleMs) } else { [LucentBench.BenchProcess]::MeasureSystemCpu($SampleMs) }
        [void]$samples.Add([math]::Round($cpu, 1))
        if ($cpu -le $MaxCpuPercent) { $run++ } else { $run = 0 }
        if ($run -ge $Consecutive) { $quiet = $true; break }
        if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { break }
    }
    return [ordered]@{ quiet = $quiet; waited_s = [math]::Round($clock.Elapsed.TotalSeconds, 1); samples = @($samples)
        max_cpu_percent = $MaxCpuPercent; consecutive = $Consecutive; sample_ms = $SampleMs; timeout_s = $TimeoutSeconds }
}

# ------------------------------------------------------------------------------------------------
# Regression budgets (ADR 0021, decision 5)
# ------------------------------------------------------------------------------------------------

function Get-BenchSignificantRound {
    # $Value rounded to three significant digits, up (or down with -Down).
    param([double]$Value, [switch]$Down)
    if ($Value -le 0) { return $Value }
    $unit = [math]::Pow(10, [math]::Floor([math]::Log10($Value)) - 2)
    if ($Down) { return [math]::Floor($Value / $unit) * $unit }
    return [math]::Ceiling($Value / $unit) * $unit
}

function New-BenchBudget {
    <#
    .SYNOPSIS
        The regression budget of one measurement from its baseline statistics (ADR 0021, decision 5).
    .DESCRIPTION
        A measurement becomes a budget only when the baseline has at least three values and its
        coefficient of variation is small: times (lower is better) and rates (higher is better)
        need a CV of at most 10 %, memory peaks at most 5 %. Then
          time:   max = max(1.25 x median, 1.10 x largest value)
          memory: max = 1.10 x largest value
          rate:   min = min(0.80 x median, 0.90 x smallest value)
        rounded to three significant digits away from the baseline. Otherwise, or with a
        $TrackedReason, the measurement is tracked: reported against its baseline, never failed.
    #>
    param(
        [Parameter(Mandatory)][ValidateSet('time', 'memory', 'rate')][string]$Kind,
        $Median, $Min, $Max, $Cv, [int]$N, [string]$TrackedReason
    )
    $budget = [ordered]@{ budgeted = $false; max = $null; min = $null; reason = $null }
    if ($TrackedReason) { $budget.reason = $TrackedReason; return $budget }
    if ($null -eq $Median -or $N -lt 3) { $budget.reason = 'fewer than three values'; return $budget }
    $limit = 0.10
    if ($Kind -eq 'memory') { $limit = 0.05 }
    if ($null -eq $Cv) { $budget.reason = 'no variation known'; return $budget }
    if ([double]$Cv -gt $limit) {
        $budget.reason = 'coefficient of variation {0:N0} % above {1:N0} %' -f (100.0 * [double]$Cv), (100.0 * $limit)
        return $budget
    }
    if ($null -eq $Max) { $Max = $Median }
    if ($null -eq $Min) { $Min = $Median }
    switch ($Kind) {
        'time' { $budget.max = Get-BenchSignificantRound ([math]::Max(1.25 * [double]$Median, 1.10 * [double]$Max)) }
        'memory' { $budget.max = Get-BenchSignificantRound (1.10 * [double]$Max) }
        'rate' { $budget.min = Get-BenchSignificantRound ([math]::Min(0.80 * [double]$Median, 0.90 * [double]$Min)) -Down }
    }
    $budget.budgeted = $true
    return $budget
}

function Test-BenchBudgetValue {
    # 'ok', 'over' (above a max), 'under' (below a min), 'tracked' or 'missing' for one measured value.
    param([Parameter(Mandatory)]$Budget, $Value)
    if ($null -eq $Value) { return 'missing' }
    if (-not $Budget.budgeted) { return 'tracked' }
    if ($null -ne $Budget.max -and [double]$Value -gt [double]$Budget.max) { return 'over' }
    if ($null -ne $Budget.min -and [double]$Value -lt [double]$Budget.min) { return 'under' }
    return 'ok'
}

# ------------------------------------------------------------------------------------------------
# Batch report checks
# ------------------------------------------------------------------------------------------------

function Test-BenchBatchReport {
    <#
    .SYNOPSIS
        Checks a "lucent.batch-report" document against what a watcher saw in the output folder.
    .PARAMETER Report
        The parsed report (ConvertFrom-Json).
    .PARAMETER Observed
        Hashtable: final output file name (case-insensitive, no folder) -> @{ sha256; size }; every
        file the watcher found in the output folder (hashed before it was removed, or at the end).
    .PARAMETER Leftovers
        Names of temporary files ("~lucent-*") or scratch folders found in the folder at the end.
    .OUTPUTS
        Ordered dictionary: entries, outcomes (counts), exported, problems (strings; empty when the
        report and the folder agree), attempts (histogram).
    #>
    param([Parameter(Mandatory)]$Report, [Parameter(Mandatory)][hashtable]$Observed,
        [AllowEmptyCollection()][string[]]$Leftovers = @(), [int]$ExpectedEntries = -1)
    $problems = New-Object System.Collections.ArrayList
    $entries = @($Report.entries)
    $outcomes = [ordered]@{}
    $attempts = [ordered]@{}
    $outputs = @{}
    $sources = @{}
    $exported = 0
    if ($Report.schema -ne 'lucent.batch-report') { [void]$problems.Add("schema is '$($Report.schema)'") }
    if ($ExpectedEntries -ge 0 -and $entries.Count -ne $ExpectedEntries) {
        [void]$problems.Add("the report has $($entries.Count) entries, expected $ExpectedEntries")
    }
    foreach ($e in $entries) {
        $outcome = [string]$e.outcome
        if ($outcomes.Contains($outcome)) { $outcomes[$outcome]++ } else { $outcomes[$outcome] = 1 }
        $a = [string]$e.attempts
        if ($attempts.Contains($a)) { $attempts[$a]++ } else { $attempts[$a] = 1 }
        $source = [string]$e.source
        if ($source) {
            if ($sources.ContainsKey($source)) { [void]$problems.Add("source listed twice: $source") }
            $sources[$source] = $true
        }
        if ($outcome -eq 'exported') {
            $exported++
            if (-not $e.output) { [void]$problems.Add("exported without an output: $source"); continue }
            $name = [System.IO.Path]::GetFileName([string]$e.output)
            $key = $name.ToLowerInvariant()
            if ($outputs.ContainsKey($key)) { [void]$problems.Add("output listed twice: $name") }
            $outputs[$key] = $true
            if (-not $Observed.ContainsKey($key)) {
                [void]$problems.Add("exported output never seen in the folder: $name")
            } else {
                $seen = $Observed[$key]
                if (([string]$seen.sha256).ToLowerInvariant() -ne ([string]$e.sha256).ToLowerInvariant()) {
                    [void]$problems.Add("output bytes differ from the receipt: $name")
                }
                if ([int64]$seen.size -ne [int64]$e.size_bytes) {
                    [void]$problems.Add("output size differs from the receipt: $name ($($seen.size) vs $($e.size_bytes))")
                }
            }
        } elseif ($outcome -ne 'skipped' -and $e.output -and $outcome -ne 'failed') {
            [void]$problems.Add("a $outcome entry names an output: $source")
        }
        if (($outcome -eq 'failed' -or $outcome -eq 'skipped') -and -not $e.reason) {
            [void]$problems.Add("a $outcome entry has no reason: $source")
        }
    }
    foreach ($key in $Observed.Keys) {
        if (-not $outputs.ContainsKey($key)) { [void]$problems.Add("output in the folder without a receipt: $key") }
    }
    foreach ($leftover in $Leftovers) { [void]$problems.Add("left in the output folder: $leftover") }
    $summary = $Report.summary
    if ($summary -and [int]$summary.exported -ne $exported) {
        [void]$problems.Add("summary says $($summary.exported) exported, the entries $exported")
    }
    return [ordered]@{ entries = $entries.Count; outcomes = $outcomes; exported = $exported
        attempts = $attempts; problems = @($problems) }
}

Export-ModuleMember -Function ConvertTo-BenchArgumentString, ConvertTo-BenchNumber, ConvertFrom-BenchKeyValues, Get-BenchValue,
    Get-BenchPercentile, Get-BenchStatistics, Write-BenchJson, Read-BenchJson, ConvertTo-BenchRounded, ConvertFrom-LucentLog,
    Invoke-BenchProcess, Get-BenchResourceSummary, Stop-BenchProcessesNamed, Get-BenchFixtures, New-BenchHardLink,
    Copy-BenchTreeUnbuffered, Get-BenchMachineFacts, Get-BenchBusyProcesses, Get-BenchAggregate, Get-BenchSignificantRound,
    New-BenchBudget, Test-BenchBudgetValue, Measure-BenchIdleCpu, Test-BenchBatchReport, Get-BenchScenarioOrder, Wait-BenchQuiet
