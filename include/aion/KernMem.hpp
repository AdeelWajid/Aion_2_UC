// Reading game memory through kernmemd.exe.
//
// The overlay holds a mapping of a shared section and two event handles -- never a handle to a
// driver and never a handle to the game. That is the point of the split: whatever is sitting in
// this process' handle table is a section, which is a thing every process has, rather than
// \Device\KernMem and a handle to the target.
//
// Header-only on purpose: the overlay's build lists its sources one by one, and a reader that is
// one more file to add is a reader that gets forgotten at the next rebuild.
#pragma once

#include "KernMemProtocol.hpp"

#include <cstdint>
#include <cstring>

namespace aion
{

class KernMem
{
public:
	KernMem() = default;

	~KernMem()
	{
		detach();
	}

	KernMem(const KernMem&) = delete;
	KernMem& operator=(const KernMem&) = delete;

	// Waits for the broker to publish its section, then maps it. False when the broker is not up.
	bool attach(const unsigned timeout_ms = 5000)
	{
		detach();

		timeout_ms_ = timeout_ms;

		const ULONGLONG deadline = GetTickCount64() + timeout_ms;

		for (;;)
		{
			mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, KERNMEM_SECTION_NAME);

			if (mapping_)
				break;

			if (GetTickCount64() >= deadline)
				return false;

			Sleep(20);
		}

		auto* const base = static_cast<unsigned char*>(
			MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));

		if (!base)
		{
			detach();
			return false;
		}

		slot_ = reinterpret_cast<KERNMEM_SLOT*>(base);

		// The events are created by whichever side gets there first; both sides then share one
		// pair. Auto-reset, so neither side can consume a signal that was meant for the other.
		request_event_ = CreateEventW(nullptr, FALSE, FALSE, KERNMEM_EVENT_REQUEST);
		done_event_ = CreateEventW(nullptr, FALSE, FALSE, KERNMEM_EVENT_DONE);

		if (!request_event_ || !done_event_ || slot_->magic != KERNMEM_SECTION_MAGIC)
		{
			detach();
			return false;
		}

		return true;
	}

	void detach()
	{
		if (slot_)
		{
			UnmapViewOfFile(slot_);
			slot_ = nullptr;
		}

		if (mapping_)
		{
			CloseHandle(mapping_);
			mapping_ = nullptr;
		}

		if (request_event_)
		{
			CloseHandle(request_event_);
			request_event_ = nullptr;
		}

		if (done_event_)
		{
			CloseHandle(done_event_);
			done_event_ = nullptr;
		}
	}

	[[nodiscard]] bool attached() const { return slot_ != nullptr; }

	// 0 after a successful transfer, otherwise what the broker reported for the last one.
	[[nodiscard]] unsigned long last_status() const { return status_; }

	bool read(const unsigned long pid, const std::uint64_t address, void* const out,
		const unsigned long size, unsigned long* const got = nullptr)
	{
		unsigned long moved = 0;
		const bool ok = request(KERNMEM_IOCTL_READ, pid, address, size, nullptr, moved);

		if (ok && out && moved)
			std::memcpy(out, slot_->data, moved);

		if (got)
			*got = ok ? moved : 0;

		return ok;
	}

	bool write(const unsigned long pid, const std::uint64_t address, const void* const in,
		const unsigned long size, unsigned long* const moved = nullptr)
	{
		unsigned long wrote = 0;
		const bool ok = request(KERNMEM_IOCTL_WRITE, pid, address, size, const_cast<void*>(in), wrote);

		if (moved)
			*moved = ok ? wrote : 0;

		return ok;
	}

private:
	bool request(const unsigned long op, const unsigned long pid, const std::uint64_t address,
		const unsigned long size, void* const payload, unsigned long& moved)
	{
		if (!slot_)
			return false;

		if (size > KERNMEM_MAX_TRANSFER)
		{
			status_ = 0x57;   // ERROR_INVALID_PARAMETER, decided here rather than by the driver
			return false;
		}

		slot_->op = op;
		slot_->status = 0;
		slot_->transferred = 0;
		slot_->request.pid = pid;
		slot_->request.address = address;
		slot_->request.size = size;

		if (payload && op == KERNMEM_IOCTL_WRITE && size)
			std::memcpy(slot_->data, payload, size);

		ResetEvent(done_event_);
		slot_->state = KERNMEM_STATE_REQUEST;
		SetEvent(request_event_);

		if (WaitForSingleObject(done_event_, timeout_ms_) != WAIT_OBJECT_0)
		{
			status_ = 0x5B4;   // ERROR_OPERATION_ABORTED: the broker did not answer in time
			slot_->state = KERNMEM_STATE_IDLE;
			return false;
		}

		status_ = slot_->status;
		moved = slot_->transferred;
		slot_->state = KERNMEM_STATE_IDLE;

		return status_ == 0;
	}

	HANDLE mapping_ = nullptr;
	HANDLE request_event_ = nullptr;
	HANDLE done_event_ = nullptr;
	KERNMEM_SLOT* slot_ = nullptr;
	unsigned long status_ = 0;
	unsigned long timeout_ms_ = 5000;
};

} // namespace aion
