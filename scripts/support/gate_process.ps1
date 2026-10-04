if ('WeaveGateProcess' -as [type]) { return }
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class WeaveGateProcess {
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinWorkingSet, MaxWorkingSet;
        public uint ActiveProcesses;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters {
        public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
        public BasicLimits Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct Startup {
        public uint Size;
        public string Reserved, Desktop, Title;
        public uint X, Y, Width, Height, XChars, YChars, Fill, Flags;
        public ushort Show, ReservedSize;
        public IntPtr ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInfo {
        public IntPtr Process, Thread;
        public uint ProcessId, ThreadId;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int type, ref ExtendedLimits limits, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string application, StringBuilder command, IntPtr processAttributes,
        IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string directory,
        ref Startup startup, out ProcessInfo info);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

    // Quote an argv element for Windows' command-line parser, not for a shell.
    static string Quote(string argument) {
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in argument) {
            if (c == '\\') { ++slashes; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            result.Append(c);
            slashes = 0;
        }
        return result.Append('\\', slashes * 2).Append('"').ToString();
    }
    static void Check(bool condition) {
        if (!condition) throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    // -1 means deadline exceeded. The job owns the worker and all descendants.
    public static int Run(string executable, string[] arguments, string directory, string log, int timeoutMs) {
        if (timeoutMs <= 0) return -1;
        IntPtr job = IntPtr.Zero;
        var process = new ProcessInfo();
        bool assigned = false;
        using (var output = new FileStream(log, FileMode.CreateNew, FileAccess.Write, FileShare.ReadWrite)) {
            try {
                job = CreateJobObject(IntPtr.Zero, null);
                Check(job != IntPtr.Zero);
                var limits = new ExtendedLimits();
                limits.Basic.Flags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                Check(SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits))));
                IntPtr file = output.SafeFileHandle.DangerousGetHandle();
                Check(SetHandleInformation(file, 1, 1));
                var startup = new Startup();
                startup.Size = (uint)Marshal.SizeOf(typeof(Startup));
                startup.Flags = 0x100; // STARTF_USESTDHANDLES
                startup.Output = startup.Error = file;
                var command = new StringBuilder(Quote(executable));
                foreach (string argument in arguments) command.Append(' ').Append(Quote(argument));
                Check(CreateProcess(executable, command, IntPtr.Zero, IntPtr.Zero, true,
                    0x08000004, IntPtr.Zero, directory, ref startup, out process)); // hidden, suspended
                Check(AssignProcessToJobObject(job, process.Process));
                assigned = true;
                Check(ResumeThread(process.Thread) != uint.MaxValue);
                uint wait = WaitForSingleObject(process.Process, (uint)timeoutMs);
                if (wait == 258) return -1;
                Check(wait == 0);
                uint code;
                Check(GetExitCodeProcess(process.Process, out code));
                return unchecked((int)code);
            } finally {
                if (job != IntPtr.Zero) TerminateJobObject(job, 4);
                if (process.Process != IntPtr.Zero) {
                    if (!assigned) TerminateProcess(process.Process, 4);
                    WaitForSingleObject(process.Process, 1000);
                    CloseHandle(process.Process);
                }
                if (process.Thread != IntPtr.Zero) CloseHandle(process.Thread);
                if (job != IntPtr.Zero) CloseHandle(job);
            }
        }
    }
}
'@
