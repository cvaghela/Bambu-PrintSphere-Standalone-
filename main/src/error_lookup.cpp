#include "printsphere/error_lookup.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "esp_err.h"
#include "esp_log.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.lookup";
constexpr char kLookupPath[] = BSP_SPIFFS_MOUNT_POINT "/error_lookup.tsv";

enum class StorageState : uint8_t {
  kUnknown,
  kReady,
  kUnavailable,
};

struct LookupCacheEntry {
  bool valid = false;
  ErrorLookupDomain domain = ErrorLookupDomain::kPrintError;
  uint64_t code = 0;
  PrinterModel model = PrinterModel::kUnknown;
  std::string text;
};

struct ParsedLookupLine {
  bool valid = false;
  char domain = '\0';
  std::string_view code;
  std::string_view models;
  std::string_view message;
};

std::mutex g_lookup_mutex;
StorageState g_storage_state = StorageState::kUnknown;
LookupCacheEntry g_cache{};

char lookup_domain_marker(ErrorLookupDomain domain) {
  switch (domain) {
    case ErrorLookupDomain::kDeviceHms:
      return 'H';
    case ErrorLookupDomain::kPrintError:
    default:
      return 'E';
  }
}

std::string format_lookup_code(ErrorLookupDomain domain, uint64_t code) {
  char buffer[24] = {};
  switch (domain) {
    case ErrorLookupDomain::kDeviceHms:
      std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(code));
      break;
    case ErrorLookupDomain::kPrintError:
    default:
      std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(code));
      break;
  }
  return buffer;
}

std::string format_generic_print_error_detail(int print_error_code, int hms_count) {
  if (print_error_code == 0 && hms_count == 0) {
    return {};
  }

  if (print_error_code != 0) {
    char error_buffer[32] = {};
    std::snprintf(error_buffer, sizeof(error_buffer), "%08X",
                  static_cast<unsigned int>(print_error_code));
    std::string detail = "Print error ";
    detail += std::string(error_buffer, 4);
    detail += "_";
    detail += std::string(error_buffer + 4, 4);
    if (hms_count > 0) {
      detail += " + HMS ";
      detail += std::to_string(hms_count);
    }
    return detail;
  }

  return "HMS alerts: " + std::to_string(hms_count);
}

bool ensure_storage_ready_locked() {
  if (g_storage_state == StorageState::kReady) {
    return true;
  }
  if (g_storage_state == StorageState::kUnavailable) {
    return false;
  }

  const esp_err_t mount_result = bsp_spiffs_mount();
  if (mount_result != ESP_OK && mount_result != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "SPIFFS mount failed: %s", esp_err_to_name(mount_result));
    g_storage_state = StorageState::kUnavailable;
    return false;
  }

  FILE* file = std::fopen(kLookupPath, "rb");
  if (file == nullptr) {
    ESP_LOGW(kTag, "Lookup file missing at %s", kLookupPath);
    g_storage_state = StorageState::kUnavailable;
    return false;
  }
  std::fclose(file);
  g_storage_state = StorageState::kReady;
  return true;
}

ParsedLookupLine parse_lookup_line(char* line) {
  ParsedLookupLine parsed{};
  if (line == nullptr) {
    return parsed;
  }

  size_t len = std::strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  if (len == 0 || line[0] == '#') {
    return parsed;
  }

  char* tab1 = std::strchr(line, '\t');
  if (tab1 == nullptr) {
    return parsed;
  }
  char* tab2 = std::strchr(tab1 + 1, '\t');
  if (tab2 == nullptr) {
    return parsed;
  }
  char* tab3 = std::strchr(tab2 + 1, '\t');
  if (tab3 == nullptr) {
    return parsed;
  }

  *tab1 = '\0';
  *tab2 = '\0';
  *tab3 = '\0';
  if (line[0] == '\0' || tab1[1] == '\0' || tab3[1] == '\0') {
    return parsed;
  }

  parsed.valid = true;
  parsed.domain = line[0];
  parsed.code = tab1 + 1;
  parsed.models = tab2 + 1;
  parsed.message = tab3 + 1;
  return parsed;
}

bool model_list_matches(std::string_view models, std::string_view model_token) {
  if (models.empty() || models == "-" || model_token.empty() || model_token == "UNKNOWN") {
    return false;
  }

  size_t cursor = 0;
  while (cursor < models.size()) {
    const size_t comma = models.find(',', cursor);
    const size_t end = (comma == std::string_view::npos) ? models.size() : comma;
    const std::string_view token = models.substr(cursor, end - cursor);
    if (token == model_token) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    cursor = comma + 1;
  }
  return false;
}

std::string lookup_error_text_uncached(ErrorLookupDomain domain, uint64_t code, PrinterModel model) {
  if (code == 0 || !ensure_storage_ready_locked()) {
    return {};
  }

  FILE* file = std::fopen(kLookupPath, "rb");
  if (file == nullptr) {
    return {};
  }
  std::setvbuf(file, nullptr, _IONBF, 0);

  const char target_domain = lookup_domain_marker(domain);
  const std::string target_code = format_lookup_code(domain, code);
  const std::string_view model_token = to_string(model);

  std::string default_message;
  std::string matched_message;
  std::string fallback_message;
  char line[1024];

  while (std::fgets(line, sizeof(line), file) != nullptr) {
    ParsedLookupLine entry = parse_lookup_line(line);
    if (!entry.valid) {
      continue;
    }
    if (entry.domain < target_domain) {
      continue;
    }
    if (entry.domain > target_domain) {
      break;
    }

    const int cmp = entry.code.compare(target_code);
    if (cmp < 0) {
      continue;
    }
    if (cmp > 0) {
      break;
    }

    if (fallback_message.empty()) {
      fallback_message.assign(entry.message);
    }
    if (entry.models.empty() || entry.models == "-") {
      if (default_message.empty()) {
        default_message.assign(entry.message);
      }
      continue;
    }
    if (model_list_matches(entry.models, model_token)) {
      matched_message.assign(entry.message);
      break;
    }
  }

  std::fclose(file);
  if (!matched_message.empty()) {
    return matched_message;
  }
  if (!default_message.empty()) {
    return default_message;
  }
  return fallback_message;
}

}  // namespace

bool initialize_error_lookup_storage() {
  std::lock_guard<std::mutex> lock(g_lookup_mutex);
  return ensure_storage_ready_locked();
}

std::string lookup_error_text(ErrorLookupDomain domain, uint64_t code, PrinterModel model) {
  if (code == 0) {
    return {};
  }

  std::lock_guard<std::mutex> lock(g_lookup_mutex);
  if (g_cache.valid && g_cache.domain == domain && g_cache.code == code && g_cache.model == model) {
    return g_cache.text;
  }

  std::string result = lookup_error_text_uncached(domain, code, model);
  g_cache.valid = true;
  g_cache.domain = domain;
  g_cache.code = code;
  g_cache.model = model;
  g_cache.text = result;
  return result;
}

std::string format_resolved_error_detail(int print_error_code, int hms_count, PrinterModel model) {
  if (print_error_code == 0) {
    return hms_count > 0 ? "HMS alerts: " + std::to_string(hms_count) : std::string{};
  }

  std::string detail = lookup_error_text(ErrorLookupDomain::kPrintError,
                                         static_cast<uint32_t>(print_error_code), model);
  if (detail.empty()) {
    return format_generic_print_error_detail(print_error_code, hms_count);
  }
  if (hms_count > 0) {
    detail += " + HMS ";
    detail += std::to_string(hms_count);
  }
  return detail;
}

}  // namespace printsphere
