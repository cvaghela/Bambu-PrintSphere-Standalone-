#include "printsphere/setup_portal.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/ui.hpp"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.portal";
constexpr size_t kMaxRequestBody = 4096;
constexpr char kPortalSessionCookieName[] = "printsphere_portal_session";
constexpr uint64_t kPortalPinLifetimeMs = 2ULL * 60ULL * 1000ULL;
constexpr uint64_t kPortalSessionLifetimeMs = 10ULL * 60ULL * 1000ULL;
constexpr char kFaviconSvg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
    "<rect width=\"64\" height=\"64\" rx=\"16\" fill=\"#121a23\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"18\" fill=\"none\" stroke=\"#f0a64b\" stroke-width=\"8\"/>"
    "</svg>";

std::string json_escape(const std::string& input) {
  std::string output;
  output.reserve(input.size());

  for (const char ch : input) {
    switch (ch) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }

  return output;
}

std::string read_string_field(const cJSON* object, const char* key) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return {};
  }
  return item->valuestring;
}

std::string trim_copy(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
    ++start;
  }

  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }

  return input.substr(start, end - start);
}

WifiCredentials merge_wifi_credentials(WifiCredentials submitted,
                                       const WifiCredentials& stored) {
  if (submitted.password.empty() && !submitted.ssid.empty() && submitted.ssid == stored.ssid) {
    submitted.password = stored.password;
  }
  return submitted;
}

BambuCloudCredentials merge_cloud_credentials(BambuCloudCredentials submitted,
                                              const BambuCloudCredentials& stored) {
  if (submitted.password.empty() && !submitted.email.empty() && submitted.email == stored.email) {
    submitted.password = stored.password;
  }
  return submitted;
}

PrinterConnection merge_printer_connection(PrinterConnection submitted,
                                           const PrinterConnection& stored) {
  if (submitted.access_code.empty() && !submitted.serial.empty() &&
      submitted.serial == stored.serial) {
    submitted.access_code = stored.access_code;
  }
  return submitted;
}

bool can_reuse_cloud_session(const BambuCloudCredentials& submitted,
                             const BambuCloudCredentials& stored,
                             const std::string& access_token) {
  return !access_token.empty() && !submitted.email.empty() && submitted.email == stored.email &&
         submitted.region == stored.region;
}

std::string color_to_html_hex(uint32_t color) {
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "#%06X", static_cast<unsigned int>(color & 0xFFFFFFU));
  return buffer;
}

bool parse_html_color(const std::string& input, uint32_t* color) {
  if (color == nullptr) {
    return false;
  }

  std::string normalized = trim_copy(input);
  if (!normalized.empty() && normalized.front() == '#') {
    normalized.erase(normalized.begin());
  }
  if (normalized.size() != 6U) {
    return false;
  }

  for (const char ch : normalized) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }

  *color = static_cast<uint32_t>(std::strtoul(normalized.c_str(), nullptr, 16));
  return true;
}

bool is_valid_ipv4(const std::string& host) {
  int dots = 0;
  int octet_value = 0;
  int octet_digits = 0;

  for (size_t i = 0; i < host.size(); ++i) {
    const char ch = host[i];
    if (ch == '.') {
      if (octet_digits == 0 || octet_value > 255) {
        return false;
      }
      ++dots;
      octet_value = 0;
      octet_digits = 0;
      continue;
    }

    if (ch < '0' || ch > '9') {
      return false;
    }

    octet_value = (octet_value * 10) + (ch - '0');
    ++octet_digits;
    if (octet_digits > 3) {
      return false;
    }
  }

  return dots == 3 && octet_digits > 0 && octet_value <= 255;
}

bool is_valid_hostname(const std::string& host) {
  if (host.empty() || host.size() > 253) {
    return false;
  }

  bool saw_alpha = false;
  size_t label_len = 0;
  char prev = '\0';
  for (const char ch : host) {
    const bool is_alpha = std::isalpha(static_cast<unsigned char>(ch)) != 0;
    const bool is_digit = std::isdigit(static_cast<unsigned char>(ch)) != 0;
    if (is_alpha) {
      saw_alpha = true;
    }

    if (is_alpha || is_digit || ch == '-') {
      ++label_len;
      if (label_len > 63) {
        return false;
      }
    } else if (ch == '.') {
      if (label_len == 0 || prev == '-') {
        return false;
      }
      label_len = 0;
    } else {
      return false;
    }

    prev = ch;
  }

  return label_len > 0 && prev != '-' && saw_alpha;
}

bool is_valid_printer_host(const std::string& host) {
  return is_valid_ipv4(host) || is_valid_hostname(host);
}

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

uint32_t remaining_seconds(uint64_t expiry_ms, uint64_t current_ms) {
  if (expiry_ms <= current_ms) {
    return 0;
  }
  return static_cast<uint32_t>((expiry_ms - current_ms + 999ULL) / 1000ULL);
}

std::string duration_text(uint32_t total_seconds) {
  const uint32_t minutes = total_seconds / 60U;
  const uint32_t seconds = total_seconds % 60U;
  char buffer[32] = {};
  if (minutes > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%um %us", static_cast<unsigned int>(minutes),
                  static_cast<unsigned int>(seconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%us", static_cast<unsigned int>(seconds));
  }
  return buffer;
}

std::string cookie_value(const std::string& header, const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return {};
  }

  const std::string needle = std::string(key) + "=";
  size_t start = 0;
  while (start < header.size()) {
    size_t end = header.find(';', start);
    if (end == std::string::npos) {
      end = header.size();
    }

    std::string part = trim_copy(header.substr(start, end - start));
    if (part.rfind(needle, 0) == 0) {
      return part.substr(needle.size());
    }

    start = end + 1U;
  }
  return {};
}

std::string request_cookie(httpd_req_t* request, const char* key) {
  if (request == nullptr) {
    return {};
  }

  const size_t header_len = httpd_req_get_hdr_value_len(request, "Cookie");
  if (header_len == 0U) {
    return {};
  }

  std::vector<char> buffer(header_len + 1U, '\0');
  if (httpd_req_get_hdr_value_str(request, "Cookie", buffer.data(), buffer.size()) != ESP_OK) {
    return {};
  }

  return cookie_value(buffer.data(), key);
}

std::string session_cookie_header(const std::string& token, uint32_t max_age_s) {
  return std::string(kPortalSessionCookieName) + "=" + token + "; Max-Age=" +
         std::to_string(max_age_s) + "; HttpOnly; SameSite=Strict; Path=/";
}

void append_portal_access_fields(std::string* body, const PortalAccessSnapshot& access) {
  if (body == nullptr) {
    return;
  }

  *body += ",\"portal_locked\":";
  *body += access.request_authorized ? "false" : "true";
  *body += ",\"portal_session_active\":";
  *body += access.session_active ? "true" : "false";
  *body += ",\"portal_pin_active\":";
  *body += access.pin_active ? "true" : "false";
  *body += ",\"portal_pin_remaining_s\":";
  *body += std::to_string(access.pin_remaining_s);
  *body += ",\"portal_session_remaining_s\":";
  *body += std::to_string(access.session_remaining_s);
  *body += ",\"portal_detail\":\"" + json_escape(access.detail) + "\"";
}

void send_json(httpd_req_t* request, const std::string& body) {
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t receive_json_body(httpd_req_t* request, cJSON** root_out) {
  if (root_out == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  *root_out = nullptr;
  if (request->content_len <= 0 || request->content_len > static_cast<int>(kMaxRequestBody)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid body length");
  }

  std::unique_ptr<char[]> body(new char[request->content_len + 1]);
  int received = 0;
  while (received < request->content_len) {
    const int ret = httpd_req_recv(request, body.get() + received, request->content_len - received);
    if (ret <= 0) {
      return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "body read failed");
    }
    received += ret;
  }
  body[request->content_len] = '\0';

  cJSON* root = cJSON_Parse(body.get());
  if (root == nullptr) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid json");
  }

  *root_out = root;
  return ESP_OK;
}

bool parse_arc_colors_from_json(const cJSON* root, ArcColorScheme* colors) {
  if (root == nullptr || colors == nullptr) {
    return false;
  }

  const auto parse_arc_field = [&](const char* key, uint32_t* value) -> bool {
    const std::string raw = trim_copy(read_string_field(root, key));
    if (raw.empty()) {
      return true;
    }
    return parse_html_color(raw, value);
  };

  return parse_arc_field("arc_printing", &colors->printing) &&
         parse_arc_field("arc_done", &colors->done) &&
         parse_arc_field("arc_error", &colors->error) &&
         parse_arc_field("arc_idle", &colors->idle) &&
         parse_arc_field("arc_preheat", &colors->preheat) &&
         parse_arc_field("arc_clean", &colors->clean) &&
         parse_arc_field("arc_level", &colors->level) &&
         parse_arc_field("arc_cool", &colors->cool) &&
         parse_arc_field("arc_idle_active", &colors->idle_active) &&
         parse_arc_field("arc_filament", &colors->filament) &&
         parse_arc_field("arc_setup", &colors->setup) &&
         parse_arc_field("arc_offline", &colors->offline) &&
         parse_arc_field("arc_unknown", &colors->unknown);
}

std::string cloud_verify_label(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "2FA Code" : "Email Code";
}

std::string cloud_verify_placeholder(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "Only needed if Bambu requests a 2FA code"
                               : "Only needed if Bambu requests an email code";
}

std::string cloud_verify_note(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "Bambu is currently waiting for a 2FA code."
                               : "Bambu is currently waiting for an email code. The cloud login completes after that step.";
}

SourceMode parse_source_mode_field(const cJSON* root) {
  if (root == nullptr) {
    return SourceMode::kHybrid;
  }

  std::string value = trim_copy(read_string_field(root, "source_mode"));
  if (value.empty()) {
    value = trim_copy(read_string_field(root, "state_source"));
  }
  return parse_source_mode(value);
}

std::string source_mode_badge_value(SourceMode mode) {
  switch (mode) {
    case SourceMode::kCloudOnly:
      return "Cloud only";
    case SourceMode::kLocalOnly:
      return "Local only";
    case SourceMode::kHybrid:
    default:
      return "Hybrid (recommended)";
  }
}

void append_cloud_status_fields(std::string* body, const BambuCloudSnapshot& cloud) {
  if (body == nullptr) {
    return;
  }

  *body += ",\"cloud_connected\":";
  *body += (cloud.connected ? "true" : "false");
  *body += ",\"cloud_verification_required\":";
  *body += (cloud.verification_required ? "true" : "false");
  *body += ",\"cloud_tfa_required\":";
  *body += (cloud.tfa_required ? "true" : "false");
  *body += ",\"cloud_configured\":";
  *body += (cloud.configured ? "true" : "false");
  *body += ",\"cloud_detail\":\"" + json_escape(cloud.detail) + "\"";
  *body += ",\"cloud_resolved_serial\":\"" + json_escape(cloud.resolved_serial) + "\"";
}

void append_local_status_fields(std::string* body, const PrinterSnapshot& local, bool local_configured) {
  if (body == nullptr) {
    return;
  }

  *body += ",\"local_error\":";
  *body += (local.connection == PrinterConnectionState::kError ? "true" : "false");
  *body += ",\"local_connected\":";
  *body += (local.connection == PrinterConnectionState::kOnline ? "true" : "false");
  *body += ",\"local_configured\":";
  *body += (local_configured ? "true" : "false");
  *body += ",\"local_detail\":\"" + json_escape(local.detail) + "\"";
}

}  // namespace

void SetupPortal::request_unlock_pin() {
  const uint64_t current_ms = now_ms();
  std::lock_guard<std::mutex> lock(access_mutex_);
  prune_access_state_locked(current_ms);
  unlock_pin_ = generate_unlock_pin();
  unlock_pin_expiry_ms_ = current_ms + kPortalPinLifetimeMs;
}

PortalAccessSnapshot SetupPortal::access_snapshot(bool request_authorized) {
  const bool lock_required = is_lock_required();
  const uint64_t current_ms = now_ms();
  std::lock_guard<std::mutex> lock(access_mutex_);
  prune_access_state_locked(current_ms);

  PortalAccessSnapshot snapshot;
  snapshot.request_authorized = lock_required ? request_authorized : true;
  snapshot.session_active = !session_token_.empty();
  snapshot.pin_active = !unlock_pin_.empty();
  snapshot.session_remaining_s = remaining_seconds(session_expiry_ms_, current_ms);
  snapshot.pin_remaining_s = remaining_seconds(unlock_pin_expiry_ms_, current_ms);
  snapshot.pin_code = unlock_pin_;

  if (!lock_required) {
    if (wifi_manager_.is_setup_access_point_active()) {
      snapshot.detail = "Web Config is open while the setup access point is active.";
    } else {
      snapshot.detail = "Web Config is open while PrintSphere is waiting for Wi-Fi credentials.";
    }
  } else if (snapshot.pin_active) {
    snapshot.detail =
        "Unlock PIN is visible on the device display for " +
        duration_text(std::max<uint32_t>(snapshot.pin_remaining_s, 1U)) + ".";
  } else if (snapshot.session_active && request_authorized) {
    snapshot.detail =
        "Web Config unlocked for " +
        duration_text(std::max<uint32_t>(snapshot.session_remaining_s, 1U)) + ".";
  } else if (snapshot.session_active) {
    snapshot.detail = "A browser session is active. Hold the display to show a new unlock PIN.";
  } else {
    snapshot.detail =
        "Hold anywhere on the device display for one second to show an unlock PIN.";
  }

  return snapshot;
}

void SetupPortal::prune_access_state_locked(uint64_t current_ms) {
  if (unlock_pin_expiry_ms_ != 0 && current_ms >= unlock_pin_expiry_ms_) {
    unlock_pin_.clear();
    unlock_pin_expiry_ms_ = 0;
  }
  if (session_expiry_ms_ != 0 && current_ms >= session_expiry_ms_) {
    session_token_.clear();
    session_expiry_ms_ = 0;
  }
}

std::string SetupPortal::generate_unlock_pin() {
  char buffer[7] = {};
  std::snprintf(buffer, sizeof(buffer), "%06u",
                static_cast<unsigned int>(esp_random() % 1000000U));
  return buffer;
}

std::string SetupPortal::generate_session_token() {
  char buffer[33] = {};
  std::snprintf(buffer, sizeof(buffer), "%08x%08x%08x%08x",
                static_cast<unsigned int>(esp_random()),
                static_cast<unsigned int>(esp_random()),
                static_cast<unsigned int>(esp_random()),
                static_cast<unsigned int>(esp_random()));
  return buffer;
}

bool SetupPortal::is_lock_required() const {
  if (wifi_manager_.is_setup_access_point_active()) {
    return false;
  }

  const PrinterSnapshot local = printer_client_.snapshot();
  return local.connection != PrinterConnectionState::kWaitingForCredentials;
}

bool SetupPortal::is_request_authorized(httpd_req_t* request) {
  if (!is_lock_required()) {
    return true;
  }

  const std::string presented = request_cookie(request, kPortalSessionCookieName);
  if (presented.empty()) {
    return false;
  }

  const uint64_t current_ms = now_ms();
  std::lock_guard<std::mutex> lock(access_mutex_);
  prune_access_state_locked(current_ms);
  return !session_token_.empty() && presented == session_token_;
}

esp_err_t SetupPortal::send_locked_response(httpd_req_t* request) {
  PortalAccessSnapshot access = access_snapshot(false);
  const std::string clear_cookie = session_cookie_header("", 0);
  httpd_resp_set_status(request, "423 Locked");
  httpd_resp_set_hdr(request, "Set-Cookie", clear_cookie.c_str());

  std::string body = "{\"error\":\"Portal locked\"";
  append_portal_access_fields(&body, access);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::send_unlock_page(httpd_req_t* request) {
  const PortalAccessSnapshot access = access_snapshot(false);
  const std::string clear_cookie = session_cookie_header("", 0);

  std::string html;
  html.reserve(5000);
  html += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>PrintSphere Unlock</title><style>";
  html += "body{margin:0;font-family:'Segoe UI',sans-serif;background:radial-gradient(circle at top,#172633,#071018 62%);color:#f8fafc;min-height:100vh;display:grid;place-items:center;padding:20px;}";
  html += ".card{width:min(420px,100%);background:rgba(6,18,26,.92);border:1px solid rgba(240,166,75,.28);border-radius:26px;padding:28px;box-shadow:0 22px 80px rgba(0,0,0,.45);}";
  html += "h1{margin:0 0 10px;font-size:32px;}p{margin:0 0 14px;line-height:1.45;color:#cbd5e1;}label{display:block;margin:18px 0 8px;color:#f8fafc;font-weight:600;}";
  html += "input{width:100%;box-sizing:border-box;border-radius:16px;border:1px solid #365064;background:#0f1e29;color:#f8fafc;padding:16px 18px;font-size:26px;letter-spacing:.22em;text-align:center;}";
  html += "button{width:100%;margin-top:16px;border:0;border-radius:16px;background:#f0a64b;color:#071018;padding:14px 18px;font-size:18px;font-weight:700;cursor:pointer;}";
  html += ".status{margin:10px 0 0;color:#f0a64b;font-weight:700;}.micro{margin-top:18px;font-size:14px;color:#94a3b8;}</style></head><body><main class=\"card\">";
  html += "<h1>Portal Locked</h1>";
  html += "<p id=\"detail\">";
  html += json_escape(access.detail);
  html += "</p>";
  html += "<form id=\"unlock-form\"><label for=\"pin\">Unlock PIN</label><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"one-time-code\" maxlength=\"6\" placeholder=\"000000\">";
  html += "<button type=\"submit\" id=\"unlock-button\">Unlock Web Config</button></form>";
  html += "<div class=\"status\" id=\"status\">Waiting for PIN</div>";
  html += "<p class=\"micro\">Long-press anywhere on the PrintSphere display for one second. The 6-digit PIN appears on the device, not in the browser.</p>";
  html += "</main><script>";
  html += "const form=document.getElementById('unlock-form');const pin=document.getElementById('pin');const statusEl=document.getElementById('status');const detailEl=document.getElementById('detail');const button=document.getElementById('unlock-button');";
  html += "async function refresh(){try{const response=await fetch('/api/health',{cache:'no-store'});const body=await response.json().catch(()=>({}));if(body.portal_locked===false){window.location.reload();return;}detailEl.textContent=body.portal_detail||'Hold anywhere on the device display for one second to show an unlock PIN.';statusEl.textContent=body.portal_pin_active?'PIN visible on the display':'Portal locked';}catch(error){statusEl.textContent='Portal unreachable';}}";
  html += "form.addEventListener('submit',async(event)=>{event.preventDefault();const code=pin.value.trim();if(!code){statusEl.textContent='Enter the PIN from the display';return;}button.disabled=true;statusEl.textContent='Unlocking...';try{const response=await fetch('/api/unlock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({pin:code})});const body=await response.json().catch(()=>({}));if(response.ok){window.location.reload();return;}statusEl.textContent=body.error||'Unlock failed';detailEl.textContent=body.detail||'The PIN was rejected.';pin.select();}catch(error){statusEl.textContent='Unlock request failed';}finally{button.disabled=false;}});";
  html += "refresh();setInterval(refresh,2000);</script></body></html>";

  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Set-Cookie", clear_cookie.c_str());
  return httpd_resp_send(request, html.c_str(), html.size());
}

esp_err_t SetupPortal::start() {
  if (server_ != nullptr) {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.max_uri_handlers = 13;

  ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), kTag, "httpd_start failed");

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = &SetupPortal::handle_root;
  root_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &root_uri), kTag, "root handler failed");

  httpd_uri_t favicon_uri = {};
  favicon_uri.uri = "/favicon.ico";
  favicon_uri.method = HTTP_GET;
  favicon_uri.handler = &SetupPortal::handle_favicon;
  favicon_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &favicon_uri), kTag,
                      "favicon handler failed");

  httpd_uri_t health_uri = {};
  health_uri.uri = "/api/health";
  health_uri.method = HTTP_GET;
  health_uri.handler = &SetupPortal::handle_health;
  health_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &health_uri), kTag,
                      "health handler failed");

  httpd_uri_t unlock_uri = {};
  unlock_uri.uri = "/api/unlock";
  unlock_uri.method = HTTP_POST;
  unlock_uri.handler = &SetupPortal::handle_unlock;
  unlock_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &unlock_uri), kTag,
                      "unlock handler failed");

  httpd_uri_t wifi_scan_uri = {};
  wifi_scan_uri.uri = "/api/wifi/scan";
  wifi_scan_uri.method = HTTP_GET;
  wifi_scan_uri.handler = &SetupPortal::handle_wifi_scan;
  wifi_scan_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &wifi_scan_uri), kTag,
                      "wifi scan handler failed");

  httpd_uri_t config_get_uri = {};
  config_get_uri.uri = "/api/config";
  config_get_uri.method = HTTP_GET;
  config_get_uri.handler = &SetupPortal::handle_config_get;
  config_get_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &config_get_uri), kTag,
                      "config get handler failed");

  httpd_uri_t config_post_uri = {};
  config_post_uri.uri = "/api/config";
  config_post_uri.method = HTTP_POST;
  config_post_uri.handler = &SetupPortal::handle_config_post;
  config_post_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &config_post_uri), kTag,
                      "config post handler failed");

  httpd_uri_t arc_preview_uri = {};
  arc_preview_uri.uri = "/api/arc/preview";
  arc_preview_uri.method = HTTP_POST;
  arc_preview_uri.handler = &SetupPortal::handle_arc_preview;
  arc_preview_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &arc_preview_uri), kTag,
                      "arc preview handler failed");

  httpd_uri_t arc_commit_uri = {};
  arc_commit_uri.uri = "/api/arc/commit";
  arc_commit_uri.method = HTTP_POST;
  arc_commit_uri.handler = &SetupPortal::handle_arc_commit;
  arc_commit_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &arc_commit_uri), kTag,
                      "arc commit handler failed");

  httpd_uri_t source_mode_uri = {};
  source_mode_uri.uri = "/api/source-mode";
  source_mode_uri.method = HTTP_POST;
  source_mode_uri.handler = &SetupPortal::handle_source_mode_post;
  source_mode_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &source_mode_uri), kTag,
                      "source mode handler failed");

  httpd_uri_t cloud_connect_uri = {};
  cloud_connect_uri.uri = "/api/cloud/connect";
  cloud_connect_uri.method = HTTP_POST;
  cloud_connect_uri.handler = &SetupPortal::handle_cloud_connect;
  cloud_connect_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &cloud_connect_uri), kTag,
                      "cloud connect handler failed");

  httpd_uri_t cloud_verify_uri = {};
  cloud_verify_uri.uri = "/api/cloud/verify";
  cloud_verify_uri.method = HTTP_POST;
  cloud_verify_uri.handler = &SetupPortal::handle_cloud_verify;
  cloud_verify_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &cloud_verify_uri), kTag,
                      "cloud verify handler failed");

  httpd_uri_t local_connect_uri = {};
  local_connect_uri.uri = "/api/local/connect";
  local_connect_uri.method = HTTP_POST;
  local_connect_uri.handler = &SetupPortal::handle_local_connect;
  local_connect_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &local_connect_uri), kTag,
                      "local connect handler failed");

  ESP_LOGI(kTag, "Setup portal started");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_root(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_unlock_page(request);
  }

  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials cloud = portal->config_store_.load_cloud_credentials();
  const SourceMode source_mode = portal->config_store_.load_source_mode();
  const PrinterConnection printer = portal->config_store_.load_printer_config();
  const ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const BambuCloudSnapshot cloud_snapshot = portal->cloud_client_.snapshot();
  const PrinterSnapshot local_snapshot = portal->printer_client_.snapshot();
  const bool wifi_connected = portal->wifi_manager_.is_station_connected();
  const bool setup_ap_active = portal->wifi_manager_.is_setup_access_point_active();
  const bool wifi_configured = wifi.is_configured();
  const bool wifi_password_saved = !wifi.password.empty();
  const bool cloud_password_saved = !cloud.password.empty();
  const bool printer_access_code_saved = !printer.access_code.empty();
  const std::string wifi_ip = portal->wifi_manager_.station_ip();
  const bool local_configured = portal->printer_client_.is_configured();
  const bool show_connection_steps = wifi_connected && !setup_ap_active;
  const std::string wifi_password_placeholder =
      wifi_password_saved ? "Leave empty to keep saved Wi-Fi password" : "Enter Wi-Fi password";
  const std::string cloud_password_placeholder =
      cloud_password_saved ? "Leave empty to keep saved Bambu password" : "Enter Bambu password";
  const std::string printer_access_code_placeholder =
      printer_access_code_saved ? "Leave empty to keep saved access code" : "Enter printer access code";

  const std::string wifi_badge_value =
      wifi_connected ? ("Connected - " + wifi_ip) : "Not connected";
  const char* wifi_badge_class = wifi_connected ? "ok" : "warn";

  std::string cloud_badge_value = "Not configured";
  const char* cloud_badge_class = "idle";
  if (cloud_snapshot.connected) {
    cloud_badge_value = "Connected";
    cloud_badge_class = "ok";
  } else if (cloud_snapshot.verification_required) {
    cloud_badge_value = "Code required";
    cloud_badge_class = "warn";
  } else if (cloud_snapshot.configured) {
    cloud_badge_value = "Configured";
    cloud_badge_class = "info";
  }

  const std::string source_badge_value = source_mode_badge_value(source_mode);
  std::string local_badge_value = "Not configured";
  const char* local_badge_class = "idle";
  if (local_snapshot.connection == PrinterConnectionState::kOnline) {
    local_badge_value = "Connected";
    local_badge_class = "ok";
  } else if (local_snapshot.connection == PrinterConnectionState::kError) {
    local_badge_value = "Error";
    local_badge_class = "warn";
  } else if (local_configured) {
    local_badge_value = "Configured";
    local_badge_class = "info";
  }
  const std::string initial_status_line =
      show_connection_steps ? "Step 2: Connect Bambu Cloud" : "Step 1: Save Wi-Fi";
  const std::string initial_status_detail =
      show_connection_steps
          ? ("ESP on home network: " + wifi_ip)
          : (!wifi_configured
                 ? "Enter your home Wi-Fi, save it and let the ESP reboot."
                 : "Update Wi-Fi if needed, save, then reopen the portal on the ESP home-network IP.");
  const std::string cloud_code_label = cloud_verify_label(cloud_snapshot);
  const std::string cloud_code_placeholder = cloud_verify_placeholder(cloud_snapshot);
  const std::string cloud_code_note = cloud_verify_note(cloud_snapshot);
  const std::string save_button_label =
      show_connection_steps ? "Save and Restart" : "Save Wi-Fi and Restart";
  const std::string save_button_hint =
      show_connection_steps
          ? "Use this mainly when Wi-Fi changes. Cloud and local connect buttons apply those paths live without a reboot."
          : "This first save writes Wi-Fi to NVS and restarts the ESP.";

  const auto add_badge = [](std::string* html, const char* id, const std::string& label,
                            const std::string& value, const char* state_class) {
    *html += "<div class=\"badge ";
    *html += state_class;
    *html += "\"";
    if (id != nullptr && id[0] != '\0') {
      *html += " id=\"";
      *html += id;
      *html += "\"";
    }
    *html += "><span class=\"badge-label\">";
    *html += label;
    *html += "</span><span class=\"badge-value\">";
    *html += json_escape(value);
    *html += "</span></div>";
  };

  std::string html;
  html += "<!doctype html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>PrintSphere Web Config</title>";
  html += "<style>";
  html += ":root{--bg:#0b1015;--panel:#121a23;--panel-2:#172230;--line:#263548;--text:#eef4fb;"
          "--muted:#99a9bb;--accent:#f0a64b;--accent-2:#42c291;--warn:#f5c24f;--danger:#ff6b6b;}";
  html += "*{box-sizing:border-box;}html,body{margin:0;}body{font-family:'Segoe UI',system-ui,"
          "sans-serif;background:radial-gradient(circle at top,#182433 0,#0b1015 55%);color:var(--text);"
          "padding:24px;}main{max-width:1040px;margin:0 auto;display:grid;gap:18px;}";
  html += "h1,h2,h3,p{margin:0;}a{color:inherit;}label{display:block;margin:0 0 8px;font-size:14px;"
          "font-weight:600;color:#d5e1ed;}input,select{width:100%;padding:13px 14px;border-radius:14px;"
          "border:1px solid var(--line);background:#0f1721;color:var(--text);outline:none;}"
          "input:focus,select:focus{border-color:#4d86c7;box-shadow:0 0 0 3px rgba(77,134,199,0.18);}";
  html += "button{border:none;border-radius:999px;padding:13px 18px;font-weight:700;cursor:pointer;}"
          "button:disabled{opacity:.65;cursor:wait;}button.primary{background:linear-gradient(135deg,"
          "#f0a64b,#ffd07a);color:#1b1203;}button.secondary{background:#203044;color:var(--text);"
          "border:1px solid #34506e;}";
  html += ".hero,.section,.footer-card{background:rgba(18,26,35,0.94);border:1px solid var(--line);"
          "border-radius:24px;box-shadow:0 14px 32px rgba(0,0,0,.2);} .hero{padding:24px;display:grid;gap:18px;}"
          ".section{padding:22px;display:grid;gap:16px;} .footer-card{padding:18px;display:grid;gap:8px;}"
          ".stack{display:grid;gap:18px;}";
  html += ".hero-top{display:grid;gap:8px;} .eyebrow{font-size:12px;letter-spacing:.14em;text-transform:uppercase;"
          "color:#8fb2d3;} .title{font-size:34px;line-height:1.05;} .subtitle{max-width:720px;color:var(--muted);"
          "line-height:1.55;} .section-head{display:grid;gap:6px;} .section-head p,.hint,.micro{color:var(--muted);"
          "line-height:1.5;} .micro{font-size:13px;} .hint-box{padding:14px 16px;border-radius:18px;background:#0e1620;"
          "border:1px solid #2b3e54;color:var(--muted);} .hint-box strong{color:var(--text);}";
  html += ".badge-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;}"
          ".badge{padding:14px 16px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:6px;}"
          ".badge-label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#91a6bc;}"
          ".badge-value{font-size:15px;font-weight:700;line-height:1.35;color:var(--text);}"
          ".badge.ok{border-color:#22614c;background:#10231d;} .badge.warn{border-color:#7d6222;background:#241c0e;}"
          ".badge.info{border-color:#31557d;background:#111b29;} .badge.idle{border-color:#334456;background:#121a23;}";
  html += ".grid-2{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;}"
          ".grid-3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}"
          ".field{display:grid;gap:8px;} .actions{display:flex;flex-wrap:wrap;gap:12px;align-items:center;}"
          ".actions .micro{min-width:220px;} .color-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}"
          ".color-grid input{min-height:54px;padding:6px 10px;background:#0c131b;}";
  html += ".status-line{font-size:16px;font-weight:700;} .status-detail{color:var(--muted);}"
          ".hidden{display:none !important;}";
  html += "@media(max-width:840px){.badge-grid,.color-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}";
  html += "@media(max-width:640px){body{padding:16px;} .title{font-size:28px;} .badge-grid,.grid-2,.grid-3,.color-grid"
          "{grid-template-columns:1fr;} .hero,.section,.footer-card{padding:18px;}}";
  html += "</style></head><body><main>";

  const auto add_color_field = [&html](const char* id, const char* label, uint32_t color) {
    html += "<div class=\"field\"><label for=\"";
    html += id;
    html += "\">";
    html += label;
    html += "</label><input type=\"color\" id=\"";
    html += id;
    html += "\" value=\"";
    html += color_to_html_hex(color);
    html += "\"></div>";
  };

  html += "<section class=\"hero\">";
  html += "<div class=\"hero-top\"><div class=\"eyebrow\">PrintSphere</div>";
  html += "<h1 class=\"title\">Web Config</h1>";
  html += "<p class=\"subtitle\">";
  html += show_connection_steps
              ? "The ESP is on your home network. You can now finish the full setup with cloud, local printer access and UI tuning."
              : "In setup AP mode this page only asks for your home Wi-Fi. Save it, let the ESP reboot, then reopen the portal on the home-network IP.";
  html += "</p></div>";
  html += "<div class=\"badge-grid\">";
  add_badge(&html, "wifi-badge", "Wi-Fi", wifi_badge_value, wifi_badge_class);
  if (show_connection_steps) {
    add_badge(&html, "cloud-badge", "Cloud", cloud_badge_value, cloud_badge_class);
    add_badge(&html, "source-badge", "Source", source_badge_value, "info");
    add_badge(&html, "local-badge", "Local Path", local_badge_value, local_badge_class);
  }
  html += "</div>";
  html += "<div class=\"hint-box\"><strong>Note:</strong> ";
  html += show_connection_steps
              ? "Wi-Fi is up. You can connect Bambu Cloud and the local MQTT path now without rebooting."
              : "Only the Wi-Fi step is available while the ESP is running its setup access point.";
  html += "</div>";
  html += "</section>";

  html += "<form id=\"config-form\" class=\"stack\">";

  html += "<section class=\"section\">";
  html += "<div class=\"section-head\"><h2>Step 1 - Wi-Fi</h2><p>This network is used for cloud access, local printer access and the setup portal after first boot.</p></div>";
  html += "<div class=\"grid-2\">";
  html += "<div class=\"field\"><label for=\"wifi_ssid\">Wi-Fi SSID</label><input id=\"wifi_ssid\" value=\"";
  html += json_escape(wifi.ssid);
  html += "\" autocomplete=\"off\"></div>";
  html += "<div class=\"field\"><label for=\"wifi_password\">Wi-Fi Password</label><input id=\"wifi_password\" type=\"password\" value=\"\" placeholder=\"";
  html += json_escape(wifi_password_placeholder);
  html += "\" autocomplete=\"off\"></div>";
  html += "</div>";
  html += "<div class=\"grid-2\">";
  html += "<div class=\"field\"><label for=\"wifi_ssid_select\">Detected Networks</label><select id=\"wifi_ssid_select\"><option value=\"\">Select detected Wi-Fi...</option></select></div>";
  html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"wifi-scan-button\">Scan Networks</button>";
  html += "<div class=\"micro\" id=\"wifi-scan-detail\">Pick a detected SSID or type one manually for hidden networks.</div></div>";
  html += "</div>";
  html += "<p class=\"micro\">Current network status: <span id=\"wifi-detail\">";
  html += json_escape(wifi_badge_value);
  html += "</span></p>";
  html += "</section>";

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Connection Mode</h2><p>This decides which printer path drives the UI. Changing it needs a reboot because the active runtime wiring changes between cloud, local and hybrid.</p></div>";
    html += "<div class=\"field\"><label for=\"source_mode\">Connection Mode</label><select id=\"source_mode\">";
    html += "<option value=\"hybrid\"";
    if (source_mode == SourceMode::kHybrid) {
      html += " selected";
    }
    html += ">Hybrid (recommended): auto-pick the best status path for your printer, keep local camera when available</option>";
    html += "<option value=\"cloud_only\"";
    if (source_mode == SourceMode::kCloudOnly) {
      html += " selected";
    }
    html += ">Cloud only: cloud monitoring and preview, local MQTT/camera disabled</option>";
    html += "<option value=\"local_only\"";
    if (source_mode == SourceMode::kLocalOnly) {
      html += " selected";
    }
    html += ">Local only</option></select></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"primary hidden\" id=\"source-mode-apply-button\">Apply + Restart</button>";
    html += "<div class=\"micro hidden\" id=\"source-mode-apply-hint\">A mode change rewires the active clients, so the ESP restarts right away after saving it.</div></div>";
    html += "</section>";
  }

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Step 2 - Bambu Cloud</h2><p>Primary source for cloud monitoring, cover image, project metadata and cloud lifecycle. "
            "Use Connect to start the login immediately. If Bambu asks for an email code or 2FA code, you can complete that step here.</p></div>";
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"cloud_email\">Bambu Email</label><input id=\"cloud_email\" value=\"";
    html += json_escape(cloud.email);
    html += "\" autocomplete=\"username\"></div>";
    html += "<div class=\"field\"><label for=\"cloud_password\">Bambu Password</label><input id=\"cloud_password\" type=\"password\" value=\"\" placeholder=\"";
    html += json_escape(cloud_password_placeholder);
    html += "\" autocomplete=\"current-password\"></div>";
    html += "</div>";
    html += "<div class=\"field\"><label for=\"cloud_region\">Cloud Region</label><select id=\"cloud_region\">";
    html += "<option value=\"us\"";
    if (cloud.region == CloudRegion::kUS) {
      html += " selected";
    }
    html += ">US</option>";
    html += "<option value=\"eu\"";
    if (cloud.region == CloudRegion::kEU) {
      html += " selected";
    }
    html += ">EU</option>";
    html += "<option value=\"cn\"";
    if (cloud.region == CloudRegion::kCN) {
      html += " selected";
    }
    html += ">CN</option></select></div>";
    html += "<div class=\"hint-box\"><strong>Printer Status:</strong> <span id=\"cloud-detail\">";
    html += json_escape(cloud_snapshot.detail);
    html += "</span></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"cloud-connect-button\">Connect Cloud</button>";
    html += "<div class=\"micro\">This saves the cloud credentials immediately. In Hybrid and Cloud only it also starts the login without rebooting.</div></div>";
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"cloud_verification_code\" id=\"cloud-verification-label\">";
    html += json_escape(cloud_code_label);
    html += "</label><input id=\"cloud_verification_code\" value=\"\" autocomplete=\"one-time-code\" placeholder=\"";
    html += json_escape(cloud_code_placeholder);
    html += "\"></div>";
    html += "<div class=\"field\"><label>&nbsp;</label><button type=\"button\" class=\"secondary\" id=\"verify-button\">Submit Code</button></div>";
    html += "</div>";
    html += "<p class=\"micro";
    if (!cloud_snapshot.verification_required) {
      html += " hidden";
    }
    html += "\" id=\"cloud-verify-note\">";
    html += json_escape(cloud_code_note);
    html += "</p>";
    html += "</section>";
  }

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Step 3 - Local Printer Path</h2><p>The local MQTT path provides live status, layers, temperatures and also powers camera snapshots on page 3.</p></div>";
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"printer_host\">Printer IP or Hostname</label><input id=\"printer_host\" value=\"";
    html += json_escape(printer.host);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"field\"><label for=\"printer_serial\">Printer Serial Number</label><input id=\"printer_serial\" value=\"";
    html += json_escape(printer.serial);
    html += "\" autocomplete=\"off\"></div>";
    html += "</div>";
    html += "<div class=\"field\"><label for=\"printer_access_code\">Access Code</label><input id=\"printer_access_code\" type=\"password\" value=\"\" placeholder=\"";
    html += json_escape(printer_access_code_placeholder);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"hint-box\"><strong>Local Status:</strong> <span id=\"local-detail\">";
    html += json_escape(local_snapshot.detail);
    html += "</span></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"local-connect-button\">Connect Local</button>";
    html += "<div class=\"micro\">This saves the local printer credentials immediately. In Hybrid and Local only it also reconnects MQTT and camera without rebooting.</div></div>";
    html += "<p class=\"micro\">In Hybrid mode PrintSphere auto-picks the better status path for your printer and still uses the local camera when available.</p>";
    html += "</section>";
  }

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Arc Colors</h2><p>These groups control ring colors, pulsing states and status colors in the UI. "
            "The values map directly to your native PrintSphere interface.</p></div>";
    html += "<p class=\"micro\">Color changes preview live immediately and are saved automatically. No restart is needed for color tuning.</p>";
    html += "<div class=\"color-grid\">";
    add_color_field("arc_printing", "Printing", arc_colors.printing);
    add_color_field("arc_done", "Done", arc_colors.done);
    add_color_field("arc_error", "Error", arc_colors.error);
    add_color_field("arc_idle", "Idle", arc_colors.idle);
    add_color_field("arc_preheat", "Preheat", arc_colors.preheat);
    add_color_field("arc_clean", "Clean", arc_colors.clean);
    add_color_field("arc_level", "Level", arc_colors.level);
    add_color_field("arc_cool", "Cool", arc_colors.cool);
    add_color_field("arc_idle_active", "Idle Active", arc_colors.idle_active);
    add_color_field("arc_filament", "Filament", arc_colors.filament);
    add_color_field("arc_setup", "Setup", arc_colors.setup);
    add_color_field("arc_offline", "Offline", arc_colors.offline);
    add_color_field("arc_unknown", "Fallback", arc_colors.unknown);
    html += "</div>";
    html += "</section>";
  }

  html += "<section class=\"footer-card\">";
  html += "<div class=\"actions\">";
  html += "<button type=\"submit\" class=\"primary\" id=\"save-button\">";
  html += json_escape(save_button_label);
  html += "</button>";
  html += "<div class=\"micro\">";
  html += json_escape(save_button_hint);
  html += "</div>";
  html += "</div>";
  html += "<div class=\"status-line\" id=\"status\">";
  html += json_escape(initial_status_line);
  html += "</div><div class=\"status-detail\" id=\"status-detail\">";
  html += json_escape(initial_status_detail);
  html += "</div>";
  html += "</section>";
  html += "</form>";

  html += "<script>";
  html += "const statusEl=document.getElementById('status');";
  html += "const statusDetailEl=document.getElementById('status-detail');";
  html += "const saveButton=document.getElementById('save-button');";
  html += "const wifiScanButton=document.getElementById('wifi-scan-button');";
  html += "const wifiSsidSelect=document.getElementById('wifi_ssid_select');";
  html += "const wifiScanDetail=document.getElementById('wifi-scan-detail');";
  html += "const sourceModeSelect=document.getElementById('source_mode');";
  html += "const sourceModeApplyButton=document.getElementById('source-mode-apply-button');";
  html += "const sourceModeApplyHint=document.getElementById('source-mode-apply-hint');";
  html += "const cloudConnectButton=document.getElementById('cloud-connect-button');";
  html += "const localConnectButton=document.getElementById('local-connect-button');";
  html += "const verifyButton=document.getElementById('verify-button');";
  html += "const arcInputIds=['arc_printing','arc_done','arc_error','arc_idle','arc_preheat','arc_clean','arc_level','arc_cool','arc_idle_active','arc_filament','arc_setup','arc_offline','arc_unknown'];";
  html += "let statusLockUntil=0;";
  html += "let arcPreviewTimer=null;";
  html += "let healthTimer=null;";
  html += "let healthInFlight=false;";
  html += "let wifiScanInFlight=false;";
  html += "const savedConfig={cloud_email:\"";
  html += json_escape(cloud.email);
  html += "\",cloud_region:\"";
  html += json_escape(to_string(cloud.region));
  html += "\",source_mode:\"";
  html += to_string(source_mode);
  html += "\",printer_host:\"";
  html += json_escape(printer.host);
  html += "\",printer_serial:\"";
  html += json_escape(printer.serial);
  html += "\",wifi_password_saved:";
  html += wifi_password_saved ? "true" : "false";
  html += ",cloud_password_saved:";
  html += cloud_password_saved ? "true" : "false";
  html += ",printer_access_code_saved:";
  html += printer_access_code_saved ? "true" : "false";
  html += "};";
  html += "function setStatus(line,detail,lockMs){statusEl.textContent=line||'';statusDetailEl.textContent=detail||'';"
          "statusLockUntil=lockMs?Date.now()+lockMs:0;}";
  html += "function valueOf(id){const el=document.getElementById(id);return el?el.value:'';}";
  html += "function trimmedValue(id){return valueOf(id).trim();}";
  html += "function applyResolvedSerial(body){const serial=((body&&body.cloud_resolved_serial)||'').trim();"
          "const input=document.getElementById('printer_serial');if(!serial||!input)return;"
          "const current=input.value.trim();if(!current||current===savedConfig.printer_serial){input.value=serial;savedConfig.printer_serial=serial;}}";
  html += "function populateWifiNetworks(body){if(!wifiSsidSelect)return;"
          "const previousValue=wifiSsidSelect.value;const currentSsid=trimmedValue('wifi_ssid');"
          "const networks=Array.isArray(body&&body.networks)?body.networks:[];"
          "wifiSsidSelect.innerHTML='<option value=\"\">Select detected Wi-Fi...</option>';"
          "networks.forEach((ssid)=>{const option=document.createElement('option');option.value=ssid;option.textContent=ssid;wifiSsidSelect.appendChild(option);});"
          "if(currentSsid&&networks.includes(currentSsid)){wifiSsidSelect.value=currentSsid;}"
          "else if(previousValue&&networks.includes(previousValue)){wifiSsidSelect.value=previousValue;}}";
  html += "async function refreshWifiScan(){if(!wifiSsidSelect||wifiScanInFlight)return;"
          "wifiScanInFlight=true;"
          "if(wifiScanButton){wifiScanButton.disabled=true;}if(wifiScanDetail){wifiScanDetail.textContent='Scanning nearby Wi-Fi networks...';}"
          "try{const response=await fetch('/api/wifi/scan',{cache:'no-store'});const body=await response.json().catch(()=>({}));"
          "if(response.ok){populateWifiNetworks(body);if(wifiScanDetail){const count=Array.isArray(body.networks)?body.networks.length:0;"
          "wifiScanDetail.textContent=count?('Found '+count+' visible network'+(count===1?'':'s')+'. Hidden SSIDs can still be typed manually.'):'No visible networks found right now. Hidden SSIDs can still be typed manually.';}}"
          "else if(wifiScanDetail){wifiScanDetail.textContent=body.error||'Wi-Fi scan failed right now.';}}"
          "catch(error){if(wifiScanDetail){wifiScanDetail.textContent='Wi-Fi scan request failed.';}}"
          "finally{wifiScanInFlight=false;if(wifiScanButton){wifiScanButton.disabled=false;}}}";
  html += "function setBadge(id,label,value,stateClass){const badge=document.getElementById(id);if(!badge)return;"
          "badge.className='badge '+stateClass;badge.innerHTML='<span class=\"badge-label\">'+label+'</span><span class=\"badge-value\">'+value+'</span>';}";
  html += "function updateSourceModeControls(){"
          "if(!sourceModeSelect||!sourceModeApplyButton||!sourceModeApplyHint)return;"
          "const selected=valueOf('source_mode')||'hybrid';"
          "const changed=selected!==(savedConfig.source_mode||'hybrid');"
          "sourceModeApplyButton.classList.toggle('hidden',!changed);"
          "sourceModeApplyHint.classList.toggle('hidden',!changed);"
          "if(!changed){sourceModeApplyButton.disabled=false;}}";
  html += "function updateCloudVerification(body){const note=document.getElementById('cloud-verify-note');"
          "const label=document.getElementById('cloud-verification-label');"
          "const input=document.getElementById('cloud_verification_code');"
          "if(!note||!label||!input)return;"
          "const tfa=!!body.cloud_tfa_required;"
          "const required=!!body.cloud_verification_required;"
          "label.textContent=tfa?'2FA Code':'Email Code';"
          "input.placeholder=tfa?'Only needed if Bambu requests a 2FA code':'Only needed if Bambu requests an email code';"
          "note.textContent=tfa?'Bambu is currently waiting for a 2FA code.':'Bambu is currently waiting for an email code. The cloud login completes after that step.';"
          "note.classList.toggle('hidden',!required);}";
  html += "async function updateHealth(){if(healthInFlight)return;healthInFlight=true;try{const response=await fetch('/api/health',{cache:'no-store'});"
          "if(!response.ok)return;const body=await response.json();if(body.portal_locked){window.location.reload();return;}"
          "const wifiValue=body.wifi_connected?('Connected - '+(body.wifi_ip||'')):'Not connected';"
          "setBadge('wifi-badge','Wi-Fi',wifiValue,body.wifi_connected?'ok':'warn');"
          "document.getElementById('wifi-detail').textContent=wifiValue;"
          "let cloudValue='Not configured';let cloudState='idle';"
          "if(body.cloud_connected){cloudValue='Connected';cloudState='ok';}"
          "else if(body.cloud_verification_required){cloudValue='Code required';cloudState='warn';}"
          "else if(body.cloud_configured||(trimmedValue('cloud_email')&&valueOf('cloud_password'))){cloudValue='Configured';cloudState='info';}"
          "setBadge('cloud-badge','Cloud',cloudValue,cloudState);"
          "const cloudDetail=document.getElementById('cloud-detail');if(cloudDetail){cloudDetail.textContent=body.cloud_detail||'No cloud response yet';}"
          "applyResolvedSerial(body);"
          "const sourceMode=body.source_mode||savedConfig.source_mode||'hybrid';"
          "const sourceValue=sourceMode==='local_only'?'Local only':(sourceMode==='cloud_only'?'Cloud only':'Hybrid (recommended)');"
          "setBadge('source-badge','Source',sourceValue,'info');"
          "let localValue='Not configured';let localState='idle';"
          "if(body.local_connected){localValue='Connected';localState='ok';}"
          "else if(body.local_error){localValue='Error';localState='warn';}"
          "else if(body.local_configured){localValue='Configured';localState='info';}"
          "setBadge('local-badge','Local Path',localValue,localState);"
          "const localDetail=document.getElementById('local-detail');if(localDetail){localDetail.textContent=body.local_detail||'No local response yet';}"
          "updateCloudVerification(body);"
          "if(Date.now()>statusLockUntil){"
          "if(body.wifi_connected){setStatus(trimmedValue('cloud_email')||body.cloud_connected||body.cloud_verification_required?'Step 2: Connect Bambu Cloud':'Setup ready',body.cloud_detail||('ESP on home network: '+(body.wifi_ip||'')),0);}"
          "else{setStatus('Step 1: Save Wi-Fi','Save Wi-Fi and restart to continue provisioning.',0);}}}"
          "catch(error){if(Date.now()>statusLockUntil){setStatus('Portal reachable','Live status could not be refreshed right now.',0);}}"
          "finally{healthInFlight=false;}}";
  html += "function buildArcPayload(){const payload={};arcInputIds.forEach((id)=>{const input=document.getElementById(id);if(input){payload[id]=input.value;}});return payload;}";
  html += "function buildPayload(){return Object.assign({wifi_ssid:trimmedValue('wifi_ssid'),"
          "wifi_password:valueOf('wifi_password'),"
          "cloud_email:(document.getElementById('cloud_email')?trimmedValue('cloud_email'):savedConfig.cloud_email),"
          "cloud_region:(document.getElementById('cloud_region')?valueOf('cloud_region'):savedConfig.cloud_region||'eu'),"
          "cloud_password:(document.getElementById('cloud_password')?valueOf('cloud_password'):''),"
          "source_mode:(document.getElementById('source_mode')?valueOf('source_mode'):savedConfig.source_mode)||'hybrid',"
          "printer_host:(document.getElementById('printer_host')?trimmedValue('printer_host'):savedConfig.printer_host),"
          "printer_serial:(document.getElementById('printer_serial')?trimmedValue('printer_serial'):savedConfig.printer_serial),"
          "printer_access_code:(document.getElementById('printer_access_code')?trimmedValue('printer_access_code'):'' )},buildArcPayload());}";
  html += "async function postArcColors(url){const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildArcPayload())});"
          "const body=await response.json().catch(()=>({}));if(!response.ok){throw new Error(body.error||'Arc color request failed');}return body;}";
  html += "function queueArcPreview(){clearTimeout(arcPreviewTimer);arcPreviewTimer=setTimeout(async()=>{try{await postArcColors('/api/arc/preview');}"
          "catch(error){setStatus('Arc preview failed',error.message||'Live color preview could not be applied.',4000);}},140);}";
  html += "async function commitArcColors(){clearTimeout(arcPreviewTimer);try{await postArcColors('/api/arc/commit');"
          "setStatus('Arc colors saved live','The current color palette is active immediately and stored without a reboot.',4000);}"
          "catch(error){setStatus('Arc color save failed',error.message||'Live color save could not be completed.',5000);}}";
  html += "document.getElementById('config-form').addEventListener('submit',async(event)=>{event.preventDefault();"
          "saveButton.disabled=true;setStatus('Saving configuration...','The ESP restarts automatically after a successful save.',15000);"
          "try{const response=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildPayload())});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Saving failed','Please review the fields and try again.',8000);saveButton.disabled=false;}}"
          "catch(error){setStatus('Saving failed','The request to the ESP could not be completed.',8000);saveButton.disabled=false;}});";
  html += "if(sourceModeSelect){sourceModeSelect.addEventListener('change',updateSourceModeControls);}";
  html += "if(sourceModeApplyButton){sourceModeApplyButton.addEventListener('click',async()=>{const source_mode=valueOf('source_mode')||'hybrid';"
          "if(source_mode===(savedConfig.source_mode||'hybrid')){updateSourceModeControls();return;}"
          "sourceModeApplyButton.disabled=true;setStatus('Applying connection mode...','Saving the new mode and restarting the ESP now.',15000);"
          "try{const response=await fetch('/api/source-mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({source_mode})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){savedConfig.source_mode=source_mode;updateSourceModeControls();setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Mode change failed',body.detail||'The new connection mode could not be saved.',8000);sourceModeApplyButton.disabled=false;updateSourceModeControls();}}"
          "catch(error){setStatus('Mode change failed','The request to the ESP could not be completed.',8000);sourceModeApplyButton.disabled=false;updateSourceModeControls();}});}";
  html += "if(cloudConnectButton){cloudConnectButton.addEventListener('click',async()=>{const cloud_email=trimmedValue('cloud_email');"
          "const cloud_region=(document.getElementById('cloud_region')?valueOf('cloud_region'):'eu')||'eu';"
          "const cloud_password=valueOf('cloud_password');"
          "const source_mode=savedConfig.source_mode||'hybrid';"
          "if(!cloud_email){setStatus('Cloud credentials missing','Enter the Bambu email first.',5000);return;}"
          "cloudConnectButton.disabled=true;setStatus('Connecting cloud...','Saving credentials and starting the login now.',8000);"
          "try{const response=await fetch('/api/cloud/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cloud_email,cloud_password,cloud_region,source_mode})});"
          "const body=await response.json().catch(()=>({}));updateCloudVerification(body);applyResolvedSerial(body);"
          "if(response.ok){savedConfig.cloud_email=cloud_email;savedConfig.cloud_region=cloud_region;"
          "if(body.cloud_connected){setStatus('Cloud connected',body.detail||'Connected to Bambu Cloud.',7000);}"
          "else if(body.cloud_verification_required){setStatus(body.cloud_tfa_required?'2FA required':'Email code required',body.detail||'Enter the requested code to finish the login.',10000);}"
          "else{setStatus('Cloud login started',body.detail||'Waiting for cloud response...',7000);}}"
          "else{setStatus(body.error||'Cloud connect failed',body.detail||'Please review the credentials and try again.',8000);}}"
          "catch(error){setStatus('Cloud request failed','The request to the ESP did not complete successfully.',7000);}finally{cloudConnectButton.disabled=false;updateHealth();}});}";
  html += "if(localConnectButton){localConnectButton.addEventListener('click',async()=>{const printer_host=trimmedValue('printer_host');"
          "const printer_serial=trimmedValue('printer_serial');"
          "const printer_access_code=trimmedValue('printer_access_code');"
          "const source_mode=savedConfig.source_mode||'hybrid';"
          "if(!printer_host||!printer_serial){setStatus('Local credentials missing','Enter printer host and serial first.',5000);return;}"
          "localConnectButton.disabled=true;setStatus('Connecting local path...','Saving printer credentials and reconnecting MQTT now.',8000);"
          "try{const response=await fetch('/api/local/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({printer_host,printer_serial,printer_access_code,source_mode})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){if(body.local_connected){setStatus('Local path connected',body.detail||'Connected to local Bambu MQTT.',7000);}"
          "else{setStatus('Local path started',body.detail||'Waiting for local printer response...',7000);}}"
          "else{setStatus(body.error||'Local connect failed',body.detail||'Please review the local printer fields and try again.',8000);}}"
          "catch(error){setStatus('Local request failed','The request to the ESP did not complete successfully.',7000);}finally{localConnectButton.disabled=false;updateHealth();}});}";
  html += "if(verifyButton){verifyButton.addEventListener('click',async()=>{const code=trimmedValue('cloud_verification_code');"
          "if(!code){setStatus('Cloud code missing','Please enter the requested code first.',5000);return;}"
          "verifyButton.disabled=true;setStatus('Connecting cloud...','Completing the cloud login now.',8000);"
          "try{const response=await fetch('/api/cloud/verify',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code})});"
          "const body=await response.json().catch(()=>({}));updateCloudVerification(body);applyResolvedSerial(body);"
          "if(response.ok){setStatus('Cloud code accepted',body.detail||'The cloud should connect within a few seconds.',7000);"
          "const codeInput=document.getElementById('cloud_verification_code');if(codeInput){codeInput.value='';}}"
          "else{setStatus(body.error||'Cloud code was rejected',body.detail||'Please check the code and try again.',7000);}}"
          "catch(error){setStatus('Cloud request failed','The request to the ESP did not complete successfully.',7000);}finally{verifyButton.disabled=false;updateHealth();}});}";
  html += "if(wifiSsidSelect){wifiSsidSelect.addEventListener('change',()=>{if(wifiSsidSelect.value){const wifiInput=document.getElementById('wifi_ssid');if(wifiInput){wifiInput.value=wifiSsidSelect.value;}}});}";
  html += "if(wifiScanButton){wifiScanButton.addEventListener('click',refreshWifiScan);}";
  html += "arcInputIds.forEach((id)=>{const input=document.getElementById(id);if(!input)return;"
          "input.addEventListener('input',queueArcPreview);input.addEventListener('change',commitArcColors);});";
  html += "updateSourceModeControls();updateHealth();healthTimer=setInterval(updateHealth,4000);window.addEventListener('beforeunload',()=>{if(healthTimer){clearInterval(healthTimer);healthTimer=null;}});";
  html += "</script>";
  html += "</main></body></html>";

  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, html.c_str(), html.size());
}

esp_err_t SetupPortal::handle_favicon(httpd_req_t* request) {
  httpd_resp_set_type(request, "image/svg+xml");
  httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
  return httpd_resp_send(request, kFaviconSvg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SetupPortal::handle_health(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  const bool request_authorized = portal->is_request_authorized(request);
  const PortalAccessSnapshot access = portal->access_snapshot(request_authorized);

  std::string body = "{";
  body += "\"status\":\"ok\",";
  body += "\"portal\":\"setup\",";
  append_portal_access_fields(&body, access);
  if (request_authorized) {
    body += ",\"source_mode\":\"";
    body += to_string(portal->config_store_.load_source_mode());
    body += "\",";
    body += "\"wifi_connected\":";
    body += (portal->wifi_manager_.is_station_connected() ? "true" : "false");
    body += ",";
    body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\"";
    const BambuCloudSnapshot cloud = portal->cloud_client_.snapshot();
    const PrinterSnapshot local = portal->printer_client_.snapshot();
    append_cloud_status_fields(&body, cloud);
    append_local_status_fields(&body, local, portal->printer_client_.is_configured());
  }
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_unlock(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  if (!portal->is_lock_required()) {
    const PortalAccessSnapshot access = portal->access_snapshot(true);
    std::string body = "{\"status\":\"open\",\"detail\":\"";
    body += json_escape(access.detail);
    body += "\"";
    append_portal_access_fields(&body, access);
    body += "}";
    send_json(request, body);
    return ESP_OK;
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string pin = trim_copy(read_string_field(root, "pin"));
  cJSON_Delete(root);
  if (pin.empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "unlock pin missing");
  }

  const uint64_t current_ms = now_ms();
  bool unlocked = false;
  {
    std::lock_guard<std::mutex> lock(portal->access_mutex_);
    portal->prune_access_state_locked(current_ms);
    if (!portal->unlock_pin_.empty() && pin == portal->unlock_pin_) {
      portal->session_token_ = generate_session_token();
      portal->session_expiry_ms_ = current_ms + kPortalSessionLifetimeMs;
      portal->unlock_pin_.clear();
      portal->unlock_pin_expiry_ms_ = 0;
      unlocked = true;
    }
  }

  if (!unlocked) {
    return portal->send_locked_response(request);
  }

  const PortalAccessSnapshot access = portal->access_snapshot(true);
  const std::string session_cookie =
      session_cookie_header(portal->session_token_,
                            static_cast<uint32_t>(kPortalSessionLifetimeMs / 1000ULL));
  httpd_resp_set_hdr(request, "Set-Cookie", session_cookie.c_str());

  std::string body = "{\"status\":\"unlocked\",\"detail\":\"";
  body += json_escape(access.detail);
  body += "\"";
  append_portal_access_fields(&body, access);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_wifi_scan(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  const std::vector<std::string> networks = portal->wifi_manager_.scan_visible_networks();
  std::string body = "{\"status\":\"ok\",\"networks\":[";
  for (size_t i = 0; i < networks.size(); ++i) {
    if (i > 0U) {
      body += ",";
    }
    body += "\"";
    body += json_escape(networks[i]);
    body += "\"";
  }
  body += "]}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_get(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials cloud = portal->config_store_.load_cloud_credentials();
  const SourceMode source_mode = portal->config_store_.load_source_mode();
  const PrinterConnection printer = portal->config_store_.load_printer_config();
  const ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();

  std::string body = "{";
  body += "\"wifi_ssid\":\"" + json_escape(wifi.ssid) + "\",";
  body += "\"cloud_email\":\"" + json_escape(cloud.email) + "\",";
  body += "\"cloud_region\":\"" + json_escape(to_string(cloud.region)) + "\",";
  body += "\"printer_host\":\"" + json_escape(printer.host) + "\",";
  body += "\"printer_serial\":\"" + json_escape(printer.serial) + "\",";
  body += "\"source_mode\":\"";
  body += to_string(source_mode);
  body += "\",";
  body += "\"state_source\":\"";
  body += to_string(source_mode);
  body += "\",";
  body += "\"wifi_connected\":";
  body += (portal->wifi_manager_.is_station_connected() ? "true" : "false");
  body += ",";
  body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\"";
  const BambuCloudSnapshot cloud_snapshot = portal->cloud_client_.snapshot();
  const PrinterSnapshot local_snapshot = portal->printer_client_.snapshot();
  append_cloud_status_fields(&body, cloud_snapshot);
  append_local_status_fields(&body, local_snapshot, portal->printer_client_.is_configured());
  body += ",\"arc_printing\":\"" + color_to_html_hex(arc_colors.printing) + "\"";
  body += ",\"arc_done\":\"" + color_to_html_hex(arc_colors.done) + "\"";
  body += ",\"arc_error\":\"" + color_to_html_hex(arc_colors.error) + "\"";
  body += ",\"arc_idle\":\"" + color_to_html_hex(arc_colors.idle) + "\"";
  body += ",\"arc_preheat\":\"" + color_to_html_hex(arc_colors.preheat) + "\"";
  body += ",\"arc_clean\":\"" + color_to_html_hex(arc_colors.clean) + "\"";
  body += ",\"arc_level\":\"" + color_to_html_hex(arc_colors.level) + "\"";
  body += ",\"arc_cool\":\"" + color_to_html_hex(arc_colors.cool) + "\"";
  body += ",\"arc_idle_active\":\"" + color_to_html_hex(arc_colors.idle_active) + "\"";
  body += ",\"arc_filament\":\"" + color_to_html_hex(arc_colors.filament) + "\"";
  body += ",\"arc_setup\":\"" + color_to_html_hex(arc_colors.setup) + "\"";
  body += ",\"arc_offline\":\"" + color_to_html_hex(arc_colors.offline) + "\"";
  body += ",\"arc_unknown\":\"" + color_to_html_hex(arc_colors.unknown) + "\"";
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const WifiCredentials stored_wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials stored_cloud = portal->config_store_.load_cloud_credentials();
  const PrinterConnection stored_printer = portal->config_store_.load_printer_config();
  const std::string stored_cloud_access_token = portal->config_store_.load_cloud_access_token();

  const WifiCredentials wifi = merge_wifi_credentials({
      .ssid = trim_copy(read_string_field(root, "wifi_ssid")),
      .password = read_string_field(root, "wifi_password"),
  }, stored_wifi);

  const BambuCloudCredentials cloud = merge_cloud_credentials({
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
      .region = parse_cloud_region(trim_copy(read_string_field(root, "cloud_region"))),
  }, stored_cloud);
  const SourceMode source_mode = parse_source_mode_field(root);

  const PrinterConnection printer = merge_printer_connection({
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  }, stored_printer);

  ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const bool colors_valid = parse_arc_colors_from_json(root, &arc_colors);

  cJSON_Delete(root);

  if (!colors_valid) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid arc color value");
  }

  const bool local_requires_complete_fields =
      !printer.host.empty() || !printer.access_code.empty();
  const bool cloud_any = !cloud.email.empty() || !cloud.password.empty();
  const bool local_ready = printer.is_ready();
  const bool cloud_ready =
      cloud.is_configured() || can_reuse_cloud_session(cloud, stored_cloud, stored_cloud_access_token);

  if (!local_ready && local_requires_complete_fields) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "local printer fields incomplete");
  }
  if (!cloud_ready && cloud_any) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "cloud account fields incomplete");
  }
  if (!wifi.is_configured() && !local_ready && !cloud_ready) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "configure Wi-Fi, cloud or local printer access");
  }
  if (!printer.host.empty() && !is_valid_printer_host(printer.host)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "printer host must be full IPv4 or hostname");
  }

  ESP_LOGI(kTag,
           "Saving config: wifi_ssid=%s cloud_email_len=%u source_mode=%s local_host=%s serial_len=%u access_len=%u",
           wifi.ssid.c_str(), static_cast<unsigned int>(cloud.email.size()),
           to_string(source_mode), printer.host.c_str(),
           static_cast<unsigned int>(printer.serial.size()),
           static_cast<unsigned int>(printer.access_code.size()));

  ESP_RETURN_ON_ERROR(portal->config_store_.save_wifi_credentials(wifi), kTag, "save wifi failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_cloud_credentials(cloud), kTag,
                      "save cloud failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_printer_config(printer), kTag, "save printer failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_arc_color_scheme(arc_colors), kTag,
                      "save arc colors failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_arc_preview(httpd_req_t* request) {
  return handle_arc_update(request, false);
}

esp_err_t SetupPortal::handle_arc_commit(httpd_req_t* request) {
  return handle_arc_update(request, true);
}

esp_err_t SetupPortal::handle_arc_update(httpd_req_t* request, bool persist) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const bool colors_valid = parse_arc_colors_from_json(root, &arc_colors);
  cJSON_Delete(root);

  if (!colors_valid) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid arc color value");
  }

  portal->ui_.set_arc_color_scheme(arc_colors);
  if (persist) {
    const esp_err_t save_err = portal->config_store_.save_arc_color_scheme(arc_colors);
    if (save_err != ESP_OK) {
      ESP_LOGE(kTag, "save live arc colors failed: %s", esp_err_to_name(save_err));
      return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "save live arc colors failed");
    }
  }

  std::string body = "{\"status\":\"";
  body += persist ? "saved" : "previewed";
  body += "\",\"persisted\":";
  body += persist ? "true" : "false";
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_source_mode_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving source mode only: %s", to_string(source_mode));
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_cloud_connect(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const BambuCloudCredentials stored_cloud = portal->config_store_.load_cloud_credentials();
  const std::string stored_cloud_access_token = portal->config_store_.load_cloud_access_token();
  const BambuCloudCredentials cloud = merge_cloud_credentials({
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
      .region = parse_cloud_region(trim_copy(read_string_field(root, "cloud_region"))),
  }, stored_cloud);
  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  if (!cloud.is_configured() &&
      !can_reuse_cloud_session(cloud, stored_cloud, stored_cloud_access_token)) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Cloud credentials incomplete\",\"detail\":\"Enter both Bambu email and password first.\"}");
    return ESP_OK;
  }

  if (!portal->wifi_manager_.is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request,
              "{\"error\":\"Wi-Fi not ready\",\"detail\":\"Save Wi-Fi and reboot first. Cloud login starts after the ESP is on your home network.\"}");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(portal->config_store_.save_cloud_credentials(cloud), kTag, "save cloud failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");
  if (source_mode == SourceMode::kLocalOnly) {
    send_json(
        request,
        "{\"status\":\"saved\",\"detail\":\"Cloud credentials saved. Switch source mode to Hybrid or Cloud only to connect.\",\"cloud_connected\":false,\"cloud_verification_required\":false,\"cloud_tfa_required\":false,\"cloud_configured\":true,\"cloud_detail\":\"Cloud credentials saved. Switch source mode to Hybrid or Cloud only to connect.\",\"cloud_resolved_serial\":\"\"}");
    return ESP_OK;
  }
  portal->cloud_client_.request_reload_from_store();

  const BambuCloudSnapshot before = portal->cloud_client_.snapshot();
  BambuCloudSnapshot current = before;
  for (int attempt = 0; attempt < 40; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = portal->cloud_client_.snapshot();
    if (current.connected || current.verification_required || current.detail != before.detail ||
        current.configured != before.configured) {
      break;
    }
  }

  std::string body = "{\"status\":\"";
  body += current.connected ? "connected"
                            : (current.verification_required ? "verification_required" : "queued");
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_cloud_status_fields(&body, current);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_cloud_verify(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string code = trim_copy(read_string_field(root, "code"));
  cJSON_Delete(root);
  if (code.empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "verification code missing");
  }

  portal->cloud_client_.submit_verification_code(code);
  BambuCloudSnapshot snapshot = portal->cloud_client_.snapshot();
  const std::string previous_detail = snapshot.detail;
  const bool previous_verification_required = snapshot.verification_required;
  for (int attempt = 0; attempt < 40; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    snapshot = portal->cloud_client_.snapshot();
    if (snapshot.connected || snapshot.detail != previous_detail ||
        snapshot.verification_required != previous_verification_required) {
      break;
    }
  }

  std::string body = "{\"status\":\"accepted\",\"detail\":\"";
  body += json_escape(snapshot.detail);
  body += "\"";
  append_cloud_status_fields(&body, snapshot);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_local_connect(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const PrinterConnection stored_printer = portal->config_store_.load_printer_config();
  const PrinterConnection printer = merge_printer_connection({
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  }, stored_printer);
  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  if (!printer.is_ready()) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Local printer fields incomplete\",\"detail\":\"Enter printer host, serial and access code first.\"}");
    return ESP_OK;
  }
  if (!is_valid_printer_host(printer.host)) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Invalid printer host\",\"detail\":\"Printer host must be a full IPv4 address or hostname.\"}");
    return ESP_OK;
  }
  if (!portal->wifi_manager_.is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request,
              "{\"error\":\"Wi-Fi not ready\",\"detail\":\"Save Wi-Fi and reboot first. The local path starts after the ESP is on your home network.\"}");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(portal->config_store_.save_printer_config(printer), kTag, "save printer failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");
  if (source_mode == SourceMode::kCloudOnly) {
    send_json(
        request,
        "{\"status\":\"saved\",\"detail\":\"Local printer credentials saved. Switch source mode to Hybrid or Local only to connect.\",\"local_error\":false,\"local_connected\":false,\"local_configured\":true,\"local_detail\":\"Local printer credentials saved. Switch source mode to Hybrid or Local only to connect.\"}");
    return ESP_OK;
  }

  const PrinterSnapshot before = portal->printer_client_.snapshot();
  portal->printer_client_.configure(printer);
  portal->camera_client_.configure(printer);

  PrinterSnapshot current = before;
  for (int attempt = 0; attempt < 40; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = portal->printer_client_.snapshot();
    if (current.connection == PrinterConnectionState::kOnline ||
        current.connection == PrinterConnectionState::kError ||
        current.detail != before.detail || current.stage != before.stage) {
      break;
    }
  }

  std::string body = "{\"status\":\"";
  body += current.connection == PrinterConnectionState::kOnline ? "connected" : "queued";
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_local_status_fields(&body, current, true);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

void SetupPortal::reboot_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
}

}  // namespace printsphere
