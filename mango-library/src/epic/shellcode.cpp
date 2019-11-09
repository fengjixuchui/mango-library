#include "../../include/epic/shellcode.h"

#include <iomanip>

#include "../../include/epic/process.h"
#include "../../include/misc/misc.h"


namespace mango {
	// allocate memory in the target process
	uintptr_t Shellcode::allocate(const Process& process) const {
		return process.alloc_virt_mem(this->m_data.size(), PAGE_EXECUTE_READWRITE);
	}

	// copy the shellcode to the address
	void Shellcode::write(const Process& process, uintptr_t address) const {
		process.write(address, this->m_data.data(), this->m_data.size());
	}

	// free shellcode that was previously allocated with Shellcode::allocate()
	// NOTE: do not modify (.push or .clear) shellcode between allocate() and free() calls
	void Shellcode::free(const Process& process, const uintptr_t address) {
		process.free_virt_mem(address);
	}

	// execute the shellcode in the process, basically just calls
	// Shellcode::allocate()
	// Shellcode::write()
	// Process::create_remote_thread()
	// Shellcode::free()
	void Shellcode::execute(const Process& process, const uintptr_t argument) const {
		const auto address = this->allocate(process);

		// free the memory when we're done
		ScopeGuard _guard(&Shellcode::free, std::ref(process), address);

		this->write(process, address);
		process.create_remote_thread(address, argument);
	}

	// push raw data, used by push()
	Shellcode& Shellcode::push_raw(const void* const data, const size_t size) {
		// resize
		const auto old_size = this->m_data.size();
		this->m_data.resize(this->m_data.size() + size);

		// write
		memcpy(this->m_data.data() + old_size, data, size);
		return *this;
	}

	// lets you do stuff like std::cout << shellcode << std::endl;
	std::ostream& operator<<(std::ostream& stream, const Shellcode& shellcode) {
		stream << "[ ";

		// output every byte in a pretty format
		for (const uint8_t b : shellcode.get_data())
			// hex, uppercase, left padded with 0s
			stream << "0x" << std::setfill('0') << std::setw(2) << std::uppercase << std::hex << +b << ' ';

		stream << "]";
		return stream;
	}
	std::wostream& operator<<(std::wostream& stream, const Shellcode& shellcode) {
		stream << L"[ ";

		// output every byte in a pretty format
		for (const uint8_t b : shellcode.get_data())
			// hex, uppercase, left padded with 0s
			stream << L"0x" << std::setfill(L'0') << std::setw(2) << std::uppercase << std::hex << +b << L' ';

		stream << L"]";
		return stream;
	}
} // namespace mango