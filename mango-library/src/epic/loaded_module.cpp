#include "../../include/epic/loaded_module.h"

#include <iostream>
#include <algorithm>

#include "../../include/epic/process.h"
#include "../../include/misc/scope_guard.h"
#include "../../include/misc/error_codes.h"

#undef min


namespace mango {
	// setup (parse the pe header mostly)
	void LoadedModule::setup(const Process& process, const uintptr_t address) {
		// reset
		m_exported_funcs.clear();
		m_imported_funcs.clear();

		this->m_image_base = address;
		this->m_is_valid = false; // not valid yet, setup_internal() could throw exceptions

		// setup
		if (process.is_64bit()) {
			this->setup_internal<true>(process, address);
		} else {
			this->setup_internal<false>(process, address);
		}

		// setup_internal() didn't throw any exceptions, we're valid
		this->m_is_valid = true;
	}

	template <bool is64bit>
	void LoadedModule::setup_internal(const Process& process, const uintptr_t address) {
		// architecture dependent types
		using ImageNtHeaders = std::conditional_t<is64bit, IMAGE_NT_HEADERS64, IMAGE_NT_HEADERS32>;
		using ImageOptionalHeader = std::conditional_t<is64bit, IMAGE_OPTIONAL_HEADER64, IMAGE_OPTIONAL_HEADER32>;
		using ImageThunkData = std::conditional_t<is64bit, uint64_t, uint32_t>;

		const auto dos_header{ process.read<IMAGE_DOS_HEADER>(address) };
		const auto nt_header{ process.read<ImageNtHeaders>(address + dos_header.e_lfanew) };

		// not a PE signature
		if (nt_header.Signature != IMAGE_NT_SIGNATURE)
			throw InvalidPEHeader{};

		// why not /shrug
		if (nt_header.FileHeader.SizeOfOptionalHeader != sizeof(ImageOptionalHeader))
			throw InvalidPEHeader{};

		// make sure the image architecture matches
		if constexpr (is64bit) {
			if (nt_header.FileHeader.Machine == IMAGE_FILE_MACHINE_I386)
				throw UnmatchingImageArchitecture{ enc_str("x86 image detected.") };
		} else {
			if (nt_header.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64)
				throw UnmatchingImageArchitecture{ enc_str("x64 image detected.") };
		}

		// size of image
		this->m_image_size = nt_header.OptionalHeader.SizeOfImage;

		// section sizes are a multiple of this
		this->m_section_alignment = nt_header.OptionalHeader.FileAlignment;

		// iterate through each section
		for (uintptr_t i{ 0 }; i < nt_header.FileHeader.NumberOfSections; ++i) {
			// the section headers are right after the pe header in memory
			const auto section_header{ process.read<IMAGE_SECTION_HEADER>(
				address + dos_header.e_lfanew + sizeof(ImageNtHeaders) + (i * sizeof(IMAGE_SECTION_HEADER))) };

			PeSection section{};

			// the section name
			char name[9]{ 0 };
			*reinterpret_cast<uint64_t*>(name) = *reinterpret_cast<const uint64_t*>(section_header.Name);
			section.name = name;

			// address
			section.address = address + section_header.VirtualAddress;

			// size
			section.rawsize = section_header.SizeOfRawData;
			section.virtualsize = section_header.Misc.VirtualSize;

			// characteristics
			section.characteristics = section_header.Characteristics;

			this->m_sections.emplace_back(section);
		}

		// export data directory
		const auto ex_dir{ process.read<IMAGE_EXPORT_DIRECTORY>(address +
			nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress) };

		// iterate through each function in the export address table
		for (size_t i{ 0 }; i < std::min(ex_dir.NumberOfFunctions, ex_dir.NumberOfNames); i++) {
			const auto name_addr{ process.read<uint32_t>(address + ex_dir.AddressOfNames + (i * 4)) };

			// get the function name
			char name[256];
			process.read(address + name_addr, name, sizeof(name));
			name[255] = '\0';

			const auto ordinal{ process.read<uint16_t>(address + ex_dir.AddressOfNameOrdinals + (i * 2)) };

			// write to pe_header for EAT hooking
			const auto table_addr{ address + ex_dir.AddressOfFunctions + (ordinal * 4) };

			// address of the function
			const auto addr{ address + process.read<uint32_t>(table_addr) };

			this->m_exported_funcs[name] = PeEntry{ addr, table_addr };
		}

		const auto imports_directory{ nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] };
		const auto iat_directory{ nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] };

		// read all at once
		const auto imports_directory_data{ std::make_unique<uint8_t[]>(imports_directory.Size) };
		process.read(address + imports_directory.VirtualAddress, imports_directory_data.get(), imports_directory.Size);

		// read all at once
		const auto iat_directory_data{ std::make_unique<uint8_t[]>(iat_directory.Size) };
		process.read(address + iat_directory.VirtualAddress, iat_directory_data.get(), iat_directory.Size);

		// iterate through each function in the import address table
		for (uintptr_t i{ 0 }; i < imports_directory.Size; i += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
			const auto iat_entry{ PIMAGE_IMPORT_DESCRIPTOR(&imports_directory_data[i]) };
			if (!iat_entry->OriginalFirstThunk)
				break;

			// ex. KERNEL32.DLL
			char module_name[256];
			process.read(address + iat_entry->Name, module_name, 256);
			module_name[255] = '\0';

			str_tolower(module_name);

			// we fill this with entries
			auto& imported_funcs{ this->m_imported_funcs[module_name] };

			// iterate through each thunk
			for (uintptr_t j{ 0 }; true; j += sizeof(ImageThunkData)) {
				const auto orig_thunk{ process.read<ImageThunkData>(address + iat_entry->OriginalFirstThunk + j) };
				if (!orig_thunk || orig_thunk > this->m_image_size)
					break;

				const auto thunk{ reinterpret_cast<ImageThunkData*>(
					&iat_directory_data[iat_entry->FirstThunk + j - iat_directory.VirtualAddress]) };

				// IMAGE_IMPORT_BY_NAME::Name
				char func_name[256];
				process.read(address + uintptr_t(orig_thunk) + 2, func_name, 256);
				func_name[255] = '\0';

				// cache the data
				imported_funcs[func_name] = PeEntry{
					uintptr_t(*thunk),
					address + iat_entry->FirstThunk + j
				};
			}
		}
	}

	// get exported functions
	std::optional<LoadedModule::PeEntry> LoadedModule::get_export(const std::string_view func_name) const {
		if (const auto it{ this->m_exported_funcs.find(std::string{ func_name }) }; it != m_exported_funcs.end())
			return it->second;
		return {};
	}

	// get imported functions
	std::optional<LoadedModule::PeEntry> LoadedModule::get_import(const std::string_view module_name, const std::string_view func_name) const {	
		if (const auto it{ m_imported_funcs.find(std::string{ module_name }) }; it != m_imported_funcs.end())
			if (const auto it2{ it->second.find(std::string{ func_name }) }; it2 != it->second.end())
				return it2->second;
		return {};
	}
} // namespace mango