#include "pch.h"

using namespace std;
using namespace Common;

class DiskGauage
{
public:
    DiskGauage() :
        diskHandle(INVALID_HANDLE_VALUE),
        avgSectorReadTimeUs(0),
        avgSectorWriteTimeUs(0),
        allSectorsReadTimeUs(0),
        allSectorsWriteTimeUs(0),
        maxSectorReadTimeUs(0),
        maxSectorWriteTimeUs(0)
    {
    }

    ~DiskGauage()
    {
        SAFE_CLOSE(diskHandle);
    }

    static int main(int argc, wchar_t **argv)
    {
        LogInfo("DiskGauge built " __TIMESTAMP__);

        DiskGauage dg;

        if (argc < 3) {
            DiskGauage::usage();
            return 0;
        }

        wchar_t* physicalDiskPath(argv[1]);
        dg.Init(physicalDiskPath);

        if (!wcscmp(argv[2], L"-g")) {
            dg.gauge();
        } else if (!wcscmp(argv[2], L"-b")) {
            if (argc < 4) {
                dg.usage();
                return 0;
            }

            LONGLONG sectorIdx = std::stoll(wstring(argv[3]));
            dg.burn(sectorIdx);
        } else {
            dg.usage();
        }

        return 0;
    }

    void Init(wchar_t* physicalDiskPath)
    {
        LogInfo("Physical Disk Path: %s", physicalDiskPath);

        diskHandle = CreateFile(
            physicalDiskPath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING,
            NULL);
        if (diskHandle == INVALID_HANDLE_VALUE) {
            LogError("CreateFile failed. Error=%ld", GetLastError());
        }

        if (!getDiskGeometry()) {
            return;
        }

        printDiskGeometry();

        sectorsCount =
            diskGeometry.Cylinders.QuadPart *
            (ULONG)diskGeometry.TracksPerCylinder *
            (ULONG)diskGeometry.SectorsPerTrack;

        LogInfo("Boosting process class and thread priority");

        if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
            LogError("SetPriorityClass failed. Error = %ld", GetLastError());
            return;
        }

        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
            LogError("SetThreadPriority failed. Error = %ld", GetLastError());
            return;
        }
    }

private:

    void burn(LONGLONG sectorIdx)
    {
        LogInfo("Burn start for sector %lld", sectorIdx);

        const LONGLONG maxCycles = 1000000;
        const size_t sectorBufferSize = diskGeometry.BytesPerSector;
        unique_ptr<BYTE[]> writeBuffer(new BYTE[sectorBufferSize]);
        unique_ptr<BYTE[]> readBuffer(new BYTE[sectorBufferSize]);
        DWORD numBytesWritten;
        LARGE_INTEGER distanceToMove;
        distanceToMove.QuadPart = sectorIdx * diskGeometry.BytesPerSector;

        for (LONGLONG cycle = 0; cycle < maxCycles; ++cycle) {
            if (!GenerateRandomDataBuffer(writeBuffer.get(), sectorBufferSize)) {
                LogError("Failed to generate random buffer");
                return;
            }

            if (!SetFilePointerEx(diskHandle, distanceToMove, NULL, FILE_BEGIN)) {
                LogError("SetFilePointerEx failed. Error = %ld", GetLastError());
            }

            if (!WriteFile(
                diskHandle,
                writeBuffer.get(),
                sectorBufferSize,
                &numBytesWritten,
                NULL)) {
                LogError(
                    "WriteFile failed at sector#%lld. Error = %ld",
                    sectorIdx,
                    GetLastError());
                break;
            }

            if (!SetFilePointerEx(diskHandle, distanceToMove, NULL, FILE_BEGIN)) {
                LogError("SetFilePointerEx failed. Error = %ld", GetLastError());
                break;
            }

            if (!ReadFile(
                diskHandle,
                readBuffer.get(),
                sectorBufferSize,
                &numBytesWritten,
                NULL)) {
                LogError(
                    "WriteFile failed at sector#%lld. Error = %ld",
                    sectorIdx,
                    GetLastError());
            }

            if (!isBuffersIdentical(writeBuffer.get(), readBuffer.get(), sectorBufferSize)) {
                LogError("Binary integrity failed at sector#%lld", sectorIdx);
                break;
            }

            // Display statistics every 512 block
            if (sectorIdx % 1000 == 0) {
                LogInfo(
                    "Burnt %lld write/read cycle(s)",
                    cycle + 1);
            }
        }
    }

    void gauge()
    {
        LogInfo("Gauging disk start for %lld sectors", sectorsCount);

        const size_t sectorBufferSize = diskGeometry.BytesPerSector;
        unique_ptr<BYTE[]> writeBuffer(new BYTE[sectorBufferSize]);
        unique_ptr<BYTE[]> readBuffer(new BYTE[sectorBufferSize]);
        DWORD numBytesWritten;

        HpcTimer readTimer;
        HpcTimer writeTimer;

        LARGE_INTEGER distanceToMove;

        for (LONGLONG sectorIdx = 0; sectorIdx < sectorsCount; ++sectorIdx) {

            if (!GenerateRandomDataBuffer(writeBuffer.get(), sectorBufferSize)) {
                LogError("Failed to generate random buffer");
                return;
            }

            distanceToMove.QuadPart = sectorIdx * diskGeometry.BytesPerSector;
            if (!SetFilePointerEx(diskHandle, distanceToMove, NULL, FILE_BEGIN)) {
                LogError("SetFilePointerEx failed. Error = %ld", GetLastError());
            }

            writeTimer.Start();
            if (!WriteFile(
                diskHandle,
                writeBuffer.get(),
                sectorBufferSize,
                &numBytesWritten,
                NULL)) {
                LogError(
                    "WriteFile failed at sector#%lld. Error = %ld",
                    sectorIdx,
                    GetLastError());
            }
            writeTimer.Stop();
            maxSectorWriteTimeUs = max(maxSectorWriteTimeUs, writeTimer.ElapsedMicroseconds());
            allSectorsWriteTimeUs += writeTimer.ElapsedMicroseconds();
            avgSectorWriteTimeUs = allSectorsWriteTimeUs / (sectorIdx + 1);

            distanceToMove.QuadPart = sectorIdx * diskGeometry.BytesPerSector;
            if (!SetFilePointerEx(diskHandle, distanceToMove, NULL, FILE_BEGIN)) {
                LogError("SetFilePointerEx failed. Error = %ld", GetLastError());
            }

            readTimer.Start();
            if (!ReadFile(
                diskHandle,
                readBuffer.get(),
                sectorBufferSize,
                &numBytesWritten,
                NULL)) {
                LogError(
                    "WriteFile failed at sector#%lld. Error = %ld",
                    sectorIdx,
                    GetLastError());
            }
            readTimer.Stop();
            allSectorsReadTimeUs += readTimer.ElapsedMicroseconds();
            avgSectorReadTimeUs = allSectorsReadTimeUs / (sectorIdx + 1);
            maxSectorReadTimeUs = max(maxSectorReadTimeUs, readTimer.ElapsedMicroseconds());

            if (!isBuffersIdentical(writeBuffer.get(), readBuffer.get(), sectorBufferSize)) {
                LogError("Binary integrity failed at sector#%lld", sectorIdx);
            }

            // Display statistics every 512 block
            if (sectorIdx % 512 == 0) {
                LogInfo(
                    "Verified %lldKB. Write: Max=%lldus, Avg=%lldus. Read: Max=%lldus, Avg=%lldus",
                    ((sectorIdx * diskGeometry.BytesPerSector) / 1024),
                    maxSectorWriteTimeUs,
                    avgSectorWriteTimeUs,
                    maxSectorReadTimeUs,
                    avgSectorReadTimeUs);
            }
        }
    }

    bool GenerateRandomDataBuffer(BYTE* BuffPtr, size_t BufferSize)
    {
        return BCRYPT_SUCCESS(
            BCryptGenRandom(
                nullptr,
                BuffPtr,
                BufferSize,
                BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    bool isBuffersIdentical(BYTE* Buff1Ptr, BYTE* Buff2Ptr, size_t BufferSize)
    {
        BYTE* Buff1EndPtr = Buff1Ptr + BufferSize;
        for (; Buff1Ptr != Buff1EndPtr; ++Buff1Ptr, ++Buff2Ptr) {
            if (*Buff1Ptr != *Buff2Ptr)
                return false;
        }

        return true;
    }

    BOOL getDiskGeometry()
    {
        DWORD dummy = 0;
        return DeviceIoControl(diskHandle,
            IOCTL_DISK_GET_DRIVE_GEOMETRY,
            NULL, 0,
            &diskGeometry,
            sizeof(diskGeometry),
            &dummy,
            (LPOVERLAPPED)NULL);
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

        LogInfo("Disk size       = %I64d (Bytes)\n"
            "                = %.2f (GB)",
            DiskSize, (double)DiskSize / (1024 * 1024 * 1024));
    }

    static void usage()
    {
        LogInfo(
            "DiskGauge <PhysicalDriverPath>\n"
            "Example: DiskGauge \\\\.\\PhysicalDrive3");
    }

    wchar_t* physicalDiskPath;
    HANDLE diskHandle;
    DISK_GEOMETRY diskGeometry;
    LONGLONG sectorsCount;

    LONGLONG avgSectorWriteTimeUs;
    LONGLONG allSectorsWriteTimeUs;
    LONGLONG maxSectorWriteTimeUs;
    LONGLONG avgSectorReadTimeUs;
    LONGLONG allSectorsReadTimeUs;
    LONGLONG maxSectorReadTimeUs;
};

int wmain(int argc, wchar_t **argv)
{
    return DiskGauage::main(argc, argv);
}