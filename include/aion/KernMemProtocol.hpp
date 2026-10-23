// The contract between the three pieces: kernmem.sys (kernel), kernmemd.exe (broker) and the
// overlay that consumes the broker's shared section.
//
// One header, so the numbers cannot drift apart: a mismatch between what the driver answers and
// what a caller asks for is silent garbage rather than an error, and this is exactly the kind of
// interface that gets edited in one place and forgotten in another. It is included from kernel mode
// (the driver) and from user mode (the broker and the overlay), which is why nothing here names a
// host header unless it has to.
#pragma once

#if defined(_KERNEL_MODE) || defined(_NTIFS_) || defined(_NTDDK_)
	// Already in a kernel translation unit: <windows.h> here would drag the user-mode declarations
	// in beside the kernel ones.
	#include <ntddk.h>
#else
	#include <windows.h>
	// CTL_CODE and the FILE_DEVICE_*/METHOD_* it is built from. Named outright because a consumer
	// compiled with WIN32_LEAN_AND_MEAN does not get <winioctl.h> through <windows.h>.
	#include <winioctl.h>
#endif

// How a caller reaches the driver. The driver creates no \DosDevices symlink, so there is no
// \\.\KernMem and nothing in the DOS namespace names this device at all; a scan of that namespace
// finds nothing and an attacker guessing at names has one less thing to guess. What is left is the
// device object in the object namespace, reached two ways: by its raw name for a caller that builds
// its own OBJECT_ATTRIBUTES (the guest client does), and through Win32's GLOBALROOT for CreateFileW
// (the broker does). Both are measured working on this machine, the second from an ordinary process.
#define KERNMEM_USER_PATH   L"\\\\?\\GLOBALROOT\\Device\\KernMem"
#define KERNMEM_DEVICE_NAME L"\\Device\\KernMem"

// Read:    in  = KERNMEM_REQUEST, out = the bytes, Information = bytes read.
// Write:   in  = KERNMEM_REQUEST followed by the bytes, out = nothing, Information = bytes written.
// Ping:    in  = nothing, out = KERNMEM_INFO, Information = sizeof(KERNMEM_INFO).
#define KERNMEM_IOCTL_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define KERNMEM_IOCTL_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define KERNMEM_IOCTL_PING  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define KERNMEM_VERSION 1

// One request cannot exceed this; a caller that wants more chunks it. Big enough for a page, small
// enough that a single bad request cannot ask the kernel to allocate something huge.
#define KERNMEM_MAX_TRANSFER (256 * 1024)

#pragma pack(push, 8)

typedef struct _KERNMEM_REQUEST
{
	unsigned long pid;          // 0 = the calling process
	unsigned long size;         // bytes to move, at most KERNMEM_MAX_TRANSFER
	unsigned long long address; // virtual address in that process
} KERNMEM_REQUEST;

typedef struct _KERNMEM_INFO
{
	unsigned long version;
	unsigned long max_transfer;
	unsigned long long features;   // bit 0: cross-process reads, bit 1: cross-process writes
} KERNMEM_INFO;

#pragma pack(pop)

// The broker's shared section. The overlay maps it read/write and never opens the device or the
// game: it writes a request into `request`, sets `state` to KERNMEM_STATE_REQUEST, and waits for
// `state` to become KERNMEM_STATE_DONE. The two named events are what makes that wait cheap.
#define KERNMEM_SECTION_NAME  L"Local\\KernMem"
#define KERNMEM_EVENT_REQUEST L"Local\\KernMemRequest"
#define KERNMEM_EVENT_DONE    L"Local\\KernMemDone"

#define KERNMEM_STATE_IDLE    0
#define KERNMEM_STATE_REQUEST 1
#define KERNMEM_STATE_DONE    2

#define KERNMEM_SECTION_MAGIC 0x4B4D4542u /* "BEMK" */

typedef struct _KERNMEM_SLOT
{
	unsigned long magic;       // KERNMEM_SECTION_MAGIC, written by the broker that created it
	volatile long state;       // KERNMEM_STATE_*
	unsigned long op;          // KERNMEM_IOCTL_* the broker should carry out
	// The error the last request ended with. Zero on success. On the driver path this is a Win32
	// error code: the broker reports GetLastError, and the I/O manager has already mapped whatever
	// NTSTATUS the driver returned (STATUS_PARTIAL_COPY arrives here as 299, the same condition
	// kernemul answers with 0xC0000005 directly). In --emulator mode it is whatever that transport
	// answered with, which is an NTSTATUS. Read it as "why this failed", not as a fixed namespace.
	unsigned long status;
	unsigned long transferred; // bytes moved by the last request
	KERNMEM_REQUEST request;
	unsigned char data[KERNMEM_MAX_TRANSFER];
} KERNMEM_SLOT;
