using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

class DumpInfo
{
    const int MiniDumpExceptionStream = 6;
    const int MiniDumpModuleListStream = 4;

    [StructLayout(LayoutKind.Sequential)]
    struct MINIDUMP_LOCATION_DESCRIPTOR { public uint DataSize; public uint Rva; }

    [StructLayout(LayoutKind.Sequential)]
    struct MINIDUMP_DIRECTORY { public uint StreamType; public MINIDUMP_LOCATION_DESCRIPTOR Location; }

    [StructLayout(LayoutKind.Sequential)]
    struct MINIDUMP_EXCEPTION
    {
        public uint ExceptionCode;
        public uint ExceptionFlags;
        public ulong ExceptionRecord;
        public ulong ExceptionAddress;
        public uint NumberParameters;
        public uint __unusedAlignment;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 15)] public ulong[] ExceptionInformation;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MINIDUMP_EXCEPTION_STREAM
    {
        public uint ThreadId;
        public uint __alignment;
        public MINIDUMP_EXCEPTION ExceptionRecord;
        public MINIDUMP_LOCATION_DESCRIPTOR ThreadContext;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MINIDUMP_STRING_HEADER { public uint Length; }

    [StructLayout(LayoutKind.Sequential)]
    struct VS_FIXEDFILEINFO
    {
        public uint dwSignature; public uint dwStrucVersion; public uint dwFileVersionMS; public uint dwFileVersionLS;
        public uint dwProductVersionMS; public uint dwProductVersionLS; public uint dwFileFlagsMask; public uint dwFileFlags;
        public uint dwFileOS; public uint dwFileType; public uint dwFileSubtype; public uint dwFileDateMS; public uint dwFileDateLS;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MINIDUMP_MODULE
    {
        public ulong BaseOfImage;
        public uint SizeOfImage;
        public uint CheckSum;
        public uint TimeDateStamp;
        public uint ModuleNameRva;
        public VS_FIXEDFILEINFO VersionInfo;
        public MINIDUMP_LOCATION_DESCRIPTOR CvRecord;
        public MINIDUMP_LOCATION_DESCRIPTOR MiscRecord;
        public ulong Reserved0;
        public ulong Reserved1;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpReadDumpStream(IntPtr BaseOfDump, int StreamNumber, out IntPtr Dir, out IntPtr StreamPointer, out uint StreamSize);

    static T PtrTo<T>(IntPtr p) where T : struct => Marshal.PtrToStructure<T>(p);

    static string ReadMinidumpString(IntPtr basePtr, uint rva)
    {
        if (rva == 0) return "";
        var header = PtrTo<MINIDUMP_STRING_HEADER>(IntPtr.Add(basePtr, (int)rva));
        var strPtr = IntPtr.Add(basePtr, (int)rva + 4);
        return Marshal.PtrToStringUni(strPtr, (int)header.Length / 2) ?? "";
    }

    static void Main(string[] args)
    {
        if (args.Length == 0) { Console.WriteLine("usage: DumpInfo <dump>"); return; }
        var path = args[0];
        using var fs = File.OpenRead(path);
        using var mm = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateFromFile(fs, null, 0, System.IO.MemoryMappedFiles.MemoryMappedFileAccess.Read, null, System.IO.HandleInheritability.None, false);
        using var view = mm.CreateViewAccessor(0, 0, System.IO.MemoryMappedFiles.MemoryMappedFileAccess.Read);
        SafeMemoryMappedViewHandle handle = view.SafeMemoryMappedViewHandle;
        handle.AcquirePointer(ref bytePtr);
        try {
            IntPtr basePtr = (IntPtr)bytePtr;
            if (MiniDumpReadDumpStream(basePtr, MiniDumpExceptionStream, out _, out var exPtr, out _)) {
                var ex = PtrTo<MINIDUMP_EXCEPTION_STREAM>(exPtr);
                Console.WriteLine($"ThreadId: {ex.ThreadId}");
                Console.WriteLine($"ExceptionCode: 0x{ex.ExceptionRecord.ExceptionCode:X8}");
                Console.WriteLine($"ExceptionAddress: 0x{ex.ExceptionRecord.ExceptionAddress:X16}");
                ulong addr = ex.ExceptionRecord.ExceptionAddress;
                if (MiniDumpReadDumpStream(basePtr, MiniDumpModuleListStream, out _, out var modPtr, out _)) {
                    uint count = (uint)Marshal.ReadInt32(modPtr);
                    IntPtr cur = IntPtr.Add(modPtr, 4);
                    string bestName = "";
                    ulong bestBase = 0; uint bestSize = 0;
                    for (uint i = 0; i < count; ++i) {
                        var mod = PtrTo<MINIDUMP_MODULE>(cur);
                        if (addr >= mod.BaseOfImage && addr < mod.BaseOfImage + mod.SizeOfImage) {
                            bestName = ReadMinidumpString(basePtr, mod.ModuleNameRva);
                            bestBase = mod.BaseOfImage;
                            bestSize = mod.SizeOfImage;
                            break;
                        }
                        cur = IntPtr.Add(cur, Marshal.SizeOf<MINIDUMP_MODULE>());
                    }
                    if (bestName.Length > 0) {
                        Console.WriteLine($"Module: {bestName}");
                        Console.WriteLine($"ModuleBase: 0x{bestBase:X16}");
                        Console.WriteLine($"Offset: 0x{(addr - bestBase):X}");
                        Console.WriteLine($"ModuleSize: 0x{bestSize:X}");
                    } else {
                        Console.WriteLine("Module: <not found>");
                    }
                }
            } else {
                Console.WriteLine("MiniDumpReadDumpStream for exception stream failed");
                Console.WriteLine(Marshal.GetLastWin32Error());
            }
        }
        finally {
            handle.ReleasePointer();
        }
    }
    static unsafe byte* bytePtr;
}
