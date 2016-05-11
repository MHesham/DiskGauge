#include "pch.h"

using namespace std;
using namespace Common;

class DiskGauage
{
public:
    DiskGauage() :
        diskHandle(INVALID_HANDLE_VALUE)
    {
        ZeroMemory(&stats, sizeof(stats));
    }

    ~DiskGauage()
    {
        SAFE_CLOSE(diskHandle);
    }

    static int main(int argc, wchar_t **argv)
    {
        LogInfo("DiskGauge built " __TIMESTAMP__);

        DiskGauage dg;

        if (argc < 2) {
            DiskGauage::usage();
            return 0;
        }

        struct _PARAMS {
            wchar_t* PhysicalDiskPath;
            enum {
                ListDisks,
                Gauge,
                Burn
            } Command;
            LONGLONG SectorIdx;
        } params;


        if (!wcscmp(argv[1], L"-l")) {
            params.Command = _PARAMS::ListDisks;
        } else if (!wcscmp(argv[1], L"-g")) {
            params.Command = _PARAMS::Gauge;
        } else if (!wcscmp(argv[2], L"-b")) {
            params.Command = _PARAMS::Burn;
        } else {
            dg.usage();
            return 0;
        }

        switch (params.Command) {
        case _PARAMS::ListDisks:
            dg.enumPhysicalDisks();
            break;

        case _PARAMS::Gauge:
        case _PARAMS::Burn:
            if (argc < 3) {
                DiskGauage::usage();
                return 0;
            }

            params.PhysicalDiskPath = argv[2];

            if (params.Command == _PARAMS::Gauge) {
                dg.gauge(params.PhysicalDiskPath);
            } else {
                params.SectorIdx = std::stoll(wstring(argv[3]));
                dg.burn(params.PhysicalDiskPath, params.SectorIdx);
            }
            break;

        default:
            LogError("Invalid command-line parameters");
            DiskGauage::usage();
        }

        return 0;
    }

private:

    void enumPhysicalDisks()
    {
        wchar_t physicalDiskNPath[32];
        for (ULONG n = 0; n < 16; ++n) {
            swprintf_s(physicalDiskNPath, L"\\\\.\\PhysicalDrive%lu", n);

            if (!openPhysicalDisk(physicalDiskNPath, false)) {
                continue;
            }

            if (getDiskGeometry()) {
                LogInfo("\n%s", physicalDiskNPath);
                printDiskGeometry();
            }

            SAFE_CLOSE(diskHandle);
        }
    }

    bool openPhysicalDisk(wchar_t* PhysicalDiskPath, bool Verbose)
    {
        diskHandle = CreateFile(
            PhysicalDiskPath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING,
            NULL);
        if (diskHandle == INVALID_HANDLE_VALUE) {
            if (Verbose) {
                LogError("CreateFile failed. Error = %ld", GetLastError());
            }
            return false;
        }

        return true;
    }

    bool burn(wchar_t* PhysicalDiskPath, LONGLONG SectorIdx)
    {
        LogInfo("Burn start on %s disk for sector %lld", PhysicalDiskPath, SectorIdx);

        if (!openPhysicalDisk(PhysicalDiskPath, true)) {
            return false;
        }

        if (!getDiskGeometry()) {
            return false;
        }

        printDiskGeometry();

        writeBuffer.reset(new BYTE[diskGeometry.BytesPerSector]);
        readBuffer.reset(new BYTE[diskGeometry.BytesPerSector]);

        if (!boostPriority()) {
            return false;
        }

        const LONGLONG maxCycles = MAXLONGLONG;

        for (LONGLONG cycle = 0; cycle < maxCycles; ++cycle) {

            if (!sectorWriteReadVerify(SectorIdx)) {
                return false;
            }

            if (SectorIdx % 1000 == 0) {
                LogInfo(
                    "Burnt %lld write/read cycle(s)",
                    cycle + 1);
            }
        }

        return true;
    }

    bool gauge(wchar_t* PhysicalDiskPath)
    {
        LogInfo("Gauging %s disk start for %lld sectors", PhysicalDiskPath, sectorsCount);

        if (!openPhysicalDisk(PhysicalDiskPath, true)) {
            return false;
        }

        if (!getDiskGeometry()) {
            return false;
        }

        printDiskGeometry();

        writeBuffer.reset(new BYTE[diskGeometry.BytesPerSector]);
        readBuffer.reset(new BYTE[diskGeometry.BytesPerSector]);

        if (!boostPriority()) {
            return false;
        }

        for (LONGLONG sectorIdx = 0; sectorIdx < sectorsCount; ++sectorIdx) {

            (void)sectorWriteReadVerify(sectorIdx);

            // Display statistics every 512 block
            if (sectorIdx % 512 == 0) {
                LogInfo(
                    "Verified %lldKB. Write: Max=%lldus, Avg=%lldus. Read: Max=%lldus, Avg=%lldus",
                    ((sectorIdx * diskGeometry.BytesPerSector) / 1024),
                    stats.MaxSectorWriteTimeUs,
                    stats.AvgSectorWriteTimeUs,
                    stats.MaxSectorReadTimeUs,
                    stats.AvgSectorReadTimeUs);
            }
        }
        
        return true;
    }

    bool sectorWriteReadVerify(LONGLONG SectorIdx)
    {
        DWORD numBytesWritten;
        LARGE_INTEGER distanceToMove;
        distanceToMove.QuadPart = SectorIdx * diskGeometry.BytesPerSector;

        if (!generateRandomDataBuffer(writeBuffer.get(), diskGeometry.BytesPerSector)) {
            LogError("Failed to generate random buffer");
            return false;
        }

        distanceToMove.QuadPart = SectorIdx * diskGeometry.BytesPerSector;
        if (!SetFilePointerEx(diskHandle, distanceToMove, NULL, FILE_BEGIN)) {
            LogError("SetFilePointerEx failed. Error = %ld", GetLastError());
            return false;
        }

        writeTimer.Start();
        if (!WriteFile(
            diskHandle,
            writeBuffer.get(),
            diskGeometry.BytesPerSector,
            &numBytesWritten,
            NULL)) {
            LogError(
                "WriteFile failed at sector#%lld. Error = %ld",
                SectorIdx,
                GetLastError());
            return false;
        }
        writeTimer.Stop();

        stats.MaxSectorWriteTimeUs = max(stats.MaxSectorWriteTimeUs, writeTimer.ElapsedMicroseconds());
        stats.AllSectorsWriteTimeUs += writeTimer.ElapsedMicroseconds();
        stats.AvgSectorWriteTimeUs = stats.AllSectorsWriteTimeUs / (SectorIdx + 1);

        distanceToMove.QuadPart = SectorIdx * diskGeometry.BytesPerSector;
        if (!SetFilePointerEx(diskHandle, distanceToMove, NULL, FILE_BEGIN)) {
            LogError("SetFilePointerEx failed. Error = %ld", GetLastError());
            return false;
        }

        readTimer.Start();
        if (!ReadFile(
            diskHandle,
            readBuffer.get(),
            diskGeometry.BytesPerSector,
            &numBytesWritten,
            NULL)) {
            LogError(
                "WriteFile failed at sector#%lld. Error = %ld",
                SectorIdx,
                GetLastError());
            return false;
        }
        readTimer.Stop();

        stats.AllSectorsReadTimeUs += readTimer.ElapsedMicroseconds();
        stats.AvgSectorReadTimeUs = stats.AllSectorsReadTimeUs / (SectorIdx + 1);
        stats.MaxSectorReadTimeUs = max(stats.MaxSectorReadTimeUs, readTimer.ElapsedMicroseconds());

        if (!isBinaryIdentical(writeBuffer.get(), readBuffer.get(), diskGeometry.BytesPerSector)) {
            LogError("Binary integrity failed at sector#%lld", SectorIdx);
            return false;
        }

        return true;
    }

    bool generateRandomDataBuffer(BYTE* BuffPtr, size_t BufferSize)
    {
        return BCRYPT_SUCCESS(
            BCryptGenRandom(
                nullptr,
                BuffPtr,
                ULONG(BufferSize),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    bool isBinaryIdentical(BYTE* Buff1Ptr, BYTE* Buff2Ptr, size_t BufferSize)
    {
        BYTE* Buff1EndPtr = Buff1Ptr + BufferSize;
        for (; Buff1Ptr != Buff1EndPtr; ++Buff1Ptr, ++Buff2Ptr) {
            if (*Buff1Ptr != *Buff2Ptr)
                return false;
        }

        return true;
    }

    bool getDiskGeometry()
    {
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(diskHandle,
            IOCTL_DISK_GET_DRIVE_GEOMETRY,
            NULL, 0,
            &diskGeometry,
            sizeof(diskGeometry),
            &bytesReturned,
            (LPOVERLAPPED)NULL)) {
            LogError("DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY) failed. Error = %ld", GetLastError());
            return false;
        }

        sectorsCount =
            diskGeometry.Cylinders.QuadPart *
            (LONGLONG)diskGeometry.TracksPerCylinder *
            (LONGLONG)diskGeometry.SectorsPerTrack;

        return true;
    }

    bool boostPriority()
    {
        LogInfo("Boosting process class and thread priority");

        if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
            LogError("SetPriorityClass failed. Error = %ld", GetLastError());
            return false;
        }

        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
            LogError("SetThreadPriority failed. Error = %ld", GetLastError());
            return false;
        }

        return true;
    }

    void printDiskGeometry()
    {
        LogInfo("Cylinders       = %I64d", diskGeometry.Cylinders);
        LogInfo("Tracks/cylinder = %ld", (ULONG)diskGeometry.TracksPerCylinder);
        LogInfo("Sectors/track   = %ld", (ULONG)diskGeometry.SectorsPerTrack);
        LogInfo("Bytes/sector    = %ld", (ULONG)diskGeometry.BytesPerSector);

        ULONGLONG  DiskSize =
            diskGeometry.Cylinders.QuadPart *
            (ULONG)diskGeometry.TracksPerCylinder *
            (ULONG)diskGeometry.SectorsPerTrack *
            (ULONG)diskGeometry.BytesPerSector;

        LogInfo(
            "Disk size       = %I64d (Bytes)\n"
            "                = %.2f (GB)",
            DiskSize, (double)DiskSize / (1024 * 1024 * 1024));
    }

    static void usage()
    {
        LogInfo(
            "DiskGauge [-l|[-g|-b <PhysicalDriverPath>]]\n"
            "Example: DiskGauge -g \\\\.\\PhysicalDrive3");
    }

    HANDLE diskHandle;
    DISK_GEOMETRY diskGeometry;
    LONGLONG sectorsCount;

    struct _STATISTICS {
        LONGLONG AvgSectorWriteTimeUs;
        LONGLONG AllSectorsWriteTimeUs;
        LONGLONG MaxSectorWriteTimeUs;
        LONGLONG AvgSectorReadTimeUs;
        LONGLONG AllSectorsReadTimeUs;
        LONGLONG MaxSectorReadTimeUs;
    } stats;

    HpcTimer readTimer;
    HpcTimer writeTimer;
    unique_ptr<BYTE[]> writeBuffer;
    unique_ptr<BYTE[]> readBuffer;;
};

int wmain(int argc, wchar_t **argv)
{
    return DiskGauage::main(argc, argv);
}