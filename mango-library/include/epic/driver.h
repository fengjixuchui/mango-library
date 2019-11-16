#pragma once

#include <Windows.h>
#include <stdint.h>
#include <string>


namespace mango {
	class Driver {
	public:
		struct SetupOptions {
			// dwDesiredAccess
			uint32_t m_access = GENERIC_READ | GENERIC_WRITE;

			// dwFlagsAndAttributes
			uint32_t m_attributes = FILE_ATTRIBUTE_NORMAL;
		};

	public:
		Driver() = default;
		Driver(const std::string& name, const SetupOptions& options = SetupOptions()) {
			this->setup(name, options);
		}
		~Driver() { this->release(); }

		// open a handle to the driver
		void setup(const std::string& name, const SetupOptions& options = SetupOptions());

		// close the handle to the driver
		void release() noexcept;

		// write() read() iocontrol() return number of bytes successfully written/read/returned

		// IRP_MJ_WRITE
		uint32_t write(const void* const buffer, const uint32_t size) const;

		// IRP_MJ_READ
		uint32_t read(void* const buffer, const uint32_t size) const;

		// IRP_MJ_DEVICE_CONTROL
		uint32_t iocontrol(const uint32_t control_code, void* const in_buffer, 
			const uint32_t in_buffer_size, void* const out_buffer, const uint32_t out_buffer_size) const;

		// if class is not setup or has been released
		bool is_valid() const noexcept { return this->m_is_valid; }

		// get the underlying win32 handle
		HANDLE get_handle() const noexcept { return this->m_handle; }

	private:
		HANDLE m_handle = nullptr;
		bool m_is_valid = false;
	};

	// register and start a service using the service control manager
	SC_HANDLE create_and_start_service(const std::string& service_name, const std::string& file_path);

	// stop and remove a running service
	void stop_and_delete_service(const SC_HANDLE service);
} // namespace mango