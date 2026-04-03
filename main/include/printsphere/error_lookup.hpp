#pragma once

#include <cstdint>
#include <string>

#include "printsphere/printer_state.hpp"

namespace printsphere {

enum class ErrorLookupDomain : uint8_t {
  kPrintError,
  kDeviceHms,
};

bool initialize_error_lookup_storage();
std::string lookup_error_text(ErrorLookupDomain domain, uint64_t code, PrinterModel model);
std::string format_resolved_error_detail(int print_error_code, int hms_count, PrinterModel model);

}  // namespace printsphere
