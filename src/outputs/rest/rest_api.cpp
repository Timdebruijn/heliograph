// SPDX-License-Identifier: MIT
//
// ESPAsyncWebServer wiring. API read from the library sources (ESPAsyncWebServer.h, v3.11.2).

#include "rest_api.h"

#if defined(ESP32)

#include <ESPAsyncWebServer.h>
#include <WiFi.h>  // softAPIP() for the captive-portal redirect target

#include <cstdlib>
#include <cstring>

#include "diagnostics/log_buffer.h"
#include "diagnostics/logger.h"
#include "outputs/prometheus/prometheus_metrics.h"
#include "outputs/rest/rest_payloads.h"
#include "config/configuration_store.h"
#include "ota/ota_manager.h"
#include "web/assets/index_html.h"
#include "web/assets/setup_html.h"

namespace heliograph::rest {
namespace {

AsyncWebServer*   g_server = nullptr;
AsyncEventSource* g_events = nullptr;
ota::OtaManager   g_ota;

constexpr const char* kJson = "application/json";

void sendError(AsyncWebServerRequest* request, const ApiError& error) {
    std::string body;
    if (!buildErrorPayload(error, "", body)) {
        request->send(error.httpStatus, kJson, "{\"error\":{\"code\":\"internal\"}}");
        return;
    }
    // Never a 200 with an error inside: a client must be able to trust the status code.
    request->send(error.httpStatus, kJson, body.c_str());
}

}  // namespace

RestApi::RestApi(RestContext context, uint16_t port) : context_(std::move(context)), port_(port) {}

/// Accumulates a chunked request body. Returns true when the body is complete and `out` holds
/// it; false means "keep waiting" or "already answered with an error".
///
/// Auth is NOT checked here. The body handler runs before the request handler, so refusing
/// here left the request handler to fire afterwards and overwrite the 401 with its own
/// error -- which is exactly how "a JSON body is required" ended up masking an auth failure.
/// Authorisation belongs in the request handler, where the response is actually formed.
bool RestApi::collectBody(AsyncWebServerRequest* request, const uint8_t* data, size_t len,
                          size_t index, size_t total, std::string*& out) {
    out = nullptr;
    if (total > kMaxRequestBytes) {
        sendError(request, {413, "body_too_large", "request body exceeds 4096 bytes"});
        return false;
    }
    if (index == 0) {
        if (bodyOwner_ != nullptr && bodyOwner_ != request) {
            sendError(request, {409, "busy", "another request is already being processed"});
            return false;
        }
        bodyOwner_ = request;
        bodyBuffer_.clear();
        bodyBuffer_.reserve(total);
        // A client that vanishes mid-body (WiFi drop on the setup AP is the realistic case)
        // never reaches the request handler, so nothing would release the buffer: every later
        // body-carrying request then answered 409 "busy" until a reboot. Same lesson the OTA
        // route already learned. After a normal completion the handler has already released
        // and the owner check makes this a no-op.
        request->onDisconnect([this, request] {
            if (bodyOwner_ == request) {
                releaseBody();
            }
        });
    }
    if (bodyOwner_ != request) {
        return false;
    }
    bodyBuffer_.append(reinterpret_cast<const char*>(data), len);
    if (bodyBuffer_.size() < total) {
        return false;  // more chunks coming
    }
    out = &bodyBuffer_;
    return true;
}

void RestApi::releaseBody() {
    bodyOwner_ = nullptr;
    bodyBuffer_.clear();
    bodyBuffer_.shrink_to_fit();
}

RestApi::~RestApi() { stop(); }

void RestApi::notifyState(const DeviceState& state, uint64_t nowMs) {
    (void)state;
    if (g_events == nullptr || !started_) {
        return;
    }
    // Bounded: at most one event per second regardless of how fast polls land, and the
    // library caps client count. A page left open must not become a load source.
    if (nowMs - lastSseMs_ < kSseMinIntervalMs) {
        return;
    }
    lastSseMs_ = nowMs;
    g_events->send("", "state", static_cast<uint32_t>(nowMs));
}

bool RestApi::begin() {
    if (started_) {
        return true;
    }
    g_server = new AsyncWebServer(port_);
    g_events = new AsyncEventSource("/api/v1/events");
    // Enforce the client bound instead of merely declaring it: every SSE client holds a TCP
    // socket and a send queue for as long as the tab lives, and a device that is up for
    // months meets a lot of abandoned tabs. Refused clients fall back to polling /status --
    // SSE is an optimisation, not a contract (see notifyState).
    g_events->authorizeConnect(
        [](AsyncWebServerRequest*) { return g_events->count() < kMaxSseClients; });

    // Authentication for every mutating endpoint. Without a configured password these are
    // refused outright rather than left open -- an unprovisioned device must not be
    // controllable by whoever finds it first.
    auto authorised = [this](AsyncWebServerRequest* request) -> bool {
        const auto& sec = context_.config->security;
        if (sec.adminPassword.empty()) {
            sendError(request, {401, "not_configured",
                                "no admin password is set; configure one before making changes"});
            return false;
        }
        if (!request->authenticate(sec.adminUsername.c_str(), sec.adminPassword.c_str())) {
            request->requestAuthentication();
            return false;
        }
        return true;
    };

    auto rateLimited = [this](AsyncWebServerRequest* request) -> bool {
        const uint64_t now = context_.clock();
        if (now - lastActionMs_ < kActionRateLimitMs) {
            sendError(request, {429, "rate_limited", "too many actions; try again shortly"});
            return true;
        }
        lastActionMs_ = now;
        return false;
    };

    auto firstDevice = [this]() -> StateHandle {
        const auto ids = context_.devices->devices();
        if (ids.empty()) {
            return nullptr;
        }
        return context_.devices->state(ids.front());
    };

    auto activeDescriptor = [this](const DeviceState& state) -> const DriverDescriptor* {
        return context_.registry->find(state.identity.driverId);
    };

    g_server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        // While the portal is up there is no network, no device and no data. Serving the
        // dashboard would show a page full of dashes; serve the one page that can help.
        const bool portal = context_.portalActive && context_.portalActive();
        // Stream straight from flash. The plain beginResponse(code, type, const char*) form
        // copies the whole literal into a heap String on every load -- ~46 KB for the
        // dashboard, competing with the MQTT/JSON/RS485 buffers under memory pressure (review,
        // 2026-07-21). The chunked filler serves it in place, no copy.
        const char*  html    = portal ? web::kSetupHtml : web::kIndexHtml;
        const size_t htmlLen = (portal ? sizeof(web::kSetupHtml) : sizeof(web::kIndexHtml)) - 1;
        auto* response = request->beginResponse(
            "text/html", htmlLen,
            [html, htmlLen](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                const size_t remaining = htmlLen - index;
                const size_t n         = remaining < maxLen ? remaining : maxLen;
                std::memcpy(buffer, html + index, n);
                return n;
            });
        // no-cache, or an OTA that changes the UI keeps showing the old page until the user
        // happens to hard-refresh (seen live: the freshly added update card stayed invisible).
        // The browser may still cache but must revalidate; the page is a few KB on a LAN.
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });

    // During setup: unauthenticated by necessity (no password exists yet on first boot).
    // Outside the portal: admin-only, so the settings page can offer the same network
    // picker as the wizard (asked for on 2026-07-22) without exposing the neighbourhood's
    // SSIDs to anyone on the LAN. Note the scan itself blocks the web task for a couple of
    // seconds and briefly takes the radio off-channel -- fine for a deliberate button press.
    g_server->on("/api/v1/wifi/scan", HTTP_GET,
                 [this, authorised](AsyncWebServerRequest* request) {
        const bool portal = context_.portalActive && context_.portalActive();
        if (!portal && !authorised(request)) {
            return;  // authorised() answered with 401
        }
        request->send(200, kJson, context_.scanNetworks().c_str());
    });

    g_server->on(
        "/api/v1/provision", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            // The request handler forms the response; the body handler only collects. If the
            // body never completed, nothing was queued and this is a genuinely empty POST.
            if (bodyOwner_ != request) {
                sendError(request, {400, "empty_body", "a JSON body is required"});
                return;
            }
            if (!context_.portalActive || !context_.portalActive()) {
                releaseBody();
                sendError(request, {403, "portal_only", "only available during setup"});
                return;
            }

            Configuration updated = *context_.config;
            ConfigError   error;
            const bool    parsed = applyConfigPatch(bodyBuffer_, updated, error);
            releaseBody();
            if (!parsed) {
                sendError(request, {400, "invalid_config",
                                    error.field.empty() ? error.message
                                                        : error.field + ": " + error.message});
                return;
            }
            // Refuse to finish setup without an admin password: everything after this
            // (settings, OTA, reboot) is gated on it, and a device without one is a device
            // anyone on the LAN can reconfigure.
            if (updated.security.adminPassword.empty()) {
                sendError(request, {400, "password_required",
                                    "an admin password is required to complete setup"});
                return;
            }
            if (updated.wifi.ssid.empty()) {
                sendError(request, {400, "ssid_required", "a WiFi network is required"});
                return;
            }
            if (!context_.saveConfig(updated)) {
                sendError(request, {500, "save_failed", "could not persist the configuration"});
                return;
            }
            context_.applyConfig(updated);  // lock-guarded publish; see RestContext
            // Echo the hostname: after the reboot this AP is gone and the user needs a
            // concrete address to go to. The setup page turns this into a clickable link.
            //
            // Built, not concatenated. hostname is validated down to [A-Za-z0-9-] before it
            // reaches here, so the previous hand-spliced JSON was safe today -- but it was the
            // one hand-rolled payload in the firmware, and the value lands in the setup page's
            // innerHTML, so any future relaxation of that validation turned a string splice
            // into a broken payload. Now escaped by the same builder path as everything else.
            std::string body;
            if (!buildProvisionPayload(updated.wifi.hostname, body)) {
                sendError(request, {500, "encode_failed", "could not build the response"});
                return;
            }
            request->send(200, kJson, body.c_str());
            context_.requestReboot();
        },
        nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
               size_t total) {
            std::string* body = nullptr;
            collectBody(request, data, len, index, total, body);
        });

    g_server->on("/api/v1/status", HTTP_GET,
                 [this, firstDevice, activeDescriptor](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();
        const auto snapshot = firstDevice();
        if (!snapshot) {
            sendError(request, {503, "not_ready", "no device is configured yet"});
            return;
        }
        // The registered id: the key the per-device routes actually answer on.
        const auto ids = context_.devices->devices();
        std::string body;
        if (!buildStatusPayload(*snapshot, ids.empty() ? std::string{} : ids.front(),
                                context_.bridgeInfo(), context_.diagnostics->snapshot(),
                                activeDescriptor(*snapshot), context_.clock(), body)) {
            sendError(request, {500, "payload_too_large", "status payload exceeded its bound"});
            return;
        }
        request->send(200, kJson, body.c_str());
    });

    // AsyncURIMatcher::exact, not a bare string: a plain string URI defaults to
    // "BackwardCompatible" matching (^uri(/.*)?$), so "/api/v1/devices" would also swallow
    // "/api/v1/devices/<id>/capabilities" and answer it with the device *list*. It did exactly
    // that, and the web UI crashed on the wrong shape rather than getting a clean 404.
    g_server->on(AsyncURIMatcher::exact("/api/v1/devices"), HTTP_GET,
                 [this](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();
        std::string body;
        buildDevicesPayload(context_.devices->devices(), body);
        request->send(200, kJson, body.c_str());
    });

    // /api/v1/devices/<id>[/measurements|/capabilities]
    //
    // Prefix matching plus manual parsing rather than AsyncURIMatcher::regex: the regex
    // matcher needs -D ASYNCWEBSERVER_REGEX, which drags std::regex into the image. That is
    // a lot of flash and heap for three routes with one path segment.
    g_server->on(AsyncURIMatcher::prefix("/api/v1/devices/"), HTTP_GET,
                 [this, activeDescriptor](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();

        std::string path = request->url().c_str();
        path.erase(0, std::strlen("/api/v1/devices/"));

        std::string deviceId = path;
        std::string sub;
        if (const size_t slash = path.find('/'); slash != std::string::npos) {
            deviceId = path.substr(0, slash);
            sub      = path.substr(slash + 1);
        }
        if (deviceId.empty()) {
            sendError(request, {404, "not_found", "no such endpoint"});
            return;
        }

        const auto snapshot = context_.devices->state(deviceId);
        if (!snapshot) {
            sendError(request, {404, "device_not_found", "no device with id '" + deviceId + "'"});
            return;
        }

        std::string body;
        bool        ok = false;
        if (sub.empty()) {
            ok = buildDevicePayload(*snapshot, deviceId, activeDescriptor(*snapshot), body);
        } else if (sub == "measurements") {
            ok = buildMeasurementsPayload(*snapshot, body);
        } else if (sub == "capabilities") {
            ok = buildCapabilitiesPayload(snapshot->capabilities, body);
        } else {
            sendError(request, {404, "not_found", "no such endpoint"});
            return;
        }
        if (!ok) {
            sendError(request, {500, "payload_too_large", "response exceeded its bound"});
            return;
        }
        request->send(200, kJson, body.c_str());
    });

    g_server->on("/api/v1/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();
        std::string body;
        buildDiagnosticsPayload(context_.diagnostics->snapshot(), context_.bridgeInfo(), body);
        request->send(200, kJson, body.c_str());
    });

    // Admin-gated: the sanitized diagnostics are public, but raw log lines carry protocol
    // traffic and internal state. They contain no credentials -- the logger may never emit one
    // -- yet they are still richer than anything else this API hands out unauthenticated.
    g_server->on("/api/v1/logs", HTTP_GET, [this, authorised](AsyncWebServerRequest* request) {
        if (!authorised(request)) {
            return;
        }
        context_.diagnostics->recordRestRequest();
        size_t limit = 40;
        if (request->hasParam("limit")) {
            const long requested = strtol(request->getParam("limit")->value().c_str(), nullptr, 10);
            if (requested > 0) {
                limit = static_cast<size_t>(requested) > log::kLogBufferLines
                            ? log::kLogBufferLines
                            : static_cast<size_t>(requested);
            }
        }
        std::string body;
        if (!buildLogsPayload(log::recentLines(limit), log::totalLines(),
                              logLevelName(log::level()), body)) {
            sendError(request, {500, "payload_too_large", "log page exceeded its bound"});
            return;
        }
        request->send(200, kJson, body.c_str());
    });

    g_server->on("/api/v1/discovery", HTTP_GET, [this](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();
        std::string body;
        if (!buildDiscoveryPayload(context_.discoveryReport(), context_.clock(), body)) {
            sendError(request, {500, "payload_too_large", "discovery report exceeded its bound"});
            return;
        }
        request->send(200, kJson, body.c_str());
    });

    g_server->on("/api/v1/drivers", HTTP_GET, [this](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();
        std::string body;
        buildDriversPayload(context_.registry->availableDrivers(), body);
        request->send(200, kJson, body.c_str());
    });

    // Unauthenticated on purpose: it contains no secrets by construction (serializeConfig
    // omits every password), and the UI needs it to render the settings form.
    g_server->on("/api/v1/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
        context_.diagnostics->recordRestRequest();
        std::string body;
        if (!serializeConfig(*context_.config, body)) {
            sendError(request, {500, "payload_too_large", "config payload exceeded its bound"});
            return;
        }
        request->send(200, kJson, body.c_str());
    });

    g_server->on(
        "/api/v1/config", HTTP_PATCH,
        [this, authorised](AsyncWebServerRequest* request) {
            // Auth here, not in the body handler. The body handler runs first; refusing there
            // left this handler to fire afterwards and replace the 401 with its own error.
            if (!authorised(request)) {
                releaseBody();
                return;
            }
            if (bodyOwner_ != request) {
                sendError(request, {400, "empty_body", "a JSON body is required"});
                return;
            }

            // Snapshot before merging: reboot_required in the response is computed against
            // this, and applyConfig() below overwrites what context_.config points at.
            const Configuration before  = *context_.config;
            Configuration       updated = before;
            ConfigError         error;
            const bool          parsed  = applyConfigPatch(bodyBuffer_, updated, error);
            releaseBody();
            if (!parsed) {
                sendError(request, {400, "invalid_config",
                                    error.field.empty() ? error.message
                                                        : error.field + ": " + error.message});
                return;
            }
            if (!context_.saveConfig(updated)) {
                sendError(request, {500, "save_failed", "could not persist the configuration"});
                return;
            }
            context_.applyConfig(updated);  // lock-guarded publish; see RestContext
            const bool  rebootRequired = configChangeRequiresReboot(before, updated);
            std::string body;
            serializeConfig(updated, body, 4096, &rebootRequired);
            request->send(200, kJson, body.c_str());
        },
        nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
               size_t total) {
            std::string* body = nullptr;
            collectBody(request, data, len, index, total, body);
        });

    g_server->on("/api/v1/actions/poll", HTTP_POST,
                 [this, authorised, rateLimited](AsyncWebServerRequest* request) {
        if (!authorised(request) || rateLimited(request)) {
            return;
        }
        // requestPoll only *requests*: the RS485 task owns the bus and runs the poll on its
        // next cycle (<=250 ms). It refuses solely when no driver is running at all.
        if (!context_.requestPoll()) {
            sendError(request, {503, "no_driver", "no driver is running"});
            return;
        }
        request->send(202, kJson, "{\"status\":\"accepted\"}");
    });

    // Bridge relay control (DRM contacts). One explicit route per possible index (max 8)
    // rather than a regex route: ESPAsyncWebServer only matches regex paths when built
    // with ASYNCWEBSERVER_REGEX, and eight fixed strings are cheaper than that flag. The
    // state travels in the query (?on=true|false) -- a JSON body would need the whole
    // collectBody dance for one boolean. Admin-gated and rate-limited like every other
    // mutation; the RelayController's gates (read-only mode, relays.enabled) decide next.
    for (uint8_t relayIndex = 0; relayIndex < 8; ++relayIndex) {
        const std::string path = "/api/v1/relays/" + std::to_string(relayIndex) + "/set";
        g_server->on(path.c_str(), HTTP_POST,
                     [this, authorised, rateLimited, relayIndex](AsyncWebServerRequest* request) {
        if (!authorised(request) || rateLimited(request)) {
            return;
        }
        if (!context_.setRelay) {
            sendError(request, {404, "no_relays", "this board has no relays"});
            return;
        }
        if (!request->hasParam("on")) {
            sendError(request, {400, "missing_parameter", "expected ?on=true or ?on=false"});
            return;
        }
        const String v = request->getParam("on")->value();
        if (v != "true" && v != "false") {
            sendError(request, {400, "invalid_parameter", "expected ?on=true or ?on=false"});
            return;
        }
        switch (context_.setRelay(relayIndex, v == "true")) {
            case CommandResult::Ok:
                request->send(200, kJson, "{\"status\":\"ok\"}");
                return;
            case CommandResult::ReadOnlyMode:
                sendError(request, {403, "read_only_mode",
                                    "security.read_only_mode is on; relays cannot move"});
                return;
            case CommandResult::Rejected:
                sendError(request, {403, "relays_disabled",
                                    "relays.enabled is off in the configuration"});
                return;
            case CommandResult::OutOfRange:
                sendError(request, {404, "no_such_relay", "relay index out of range"});
                return;
            case CommandResult::RateLimited:
                sendError(request, {429, "rate_limited", "too many relay commands"});
                return;
            default:
                sendError(request, {500, "relay_error", "relay command failed"});
                return;
        }
        });
    }

    // DRM mode: assert one named demand-response mode, release everything else. The mode
    // vocabulary comes from the configured relay roles (src/relays/drm.h).
    g_server->on("/api/v1/drm/set", HTTP_POST,
                 [this, authorised, rateLimited](AsyncWebServerRequest* request) {
        if (!authorised(request) || rateLimited(request)) {
            return;
        }
        if (!context_.setDrmMode) {
            sendError(request, {404, "no_relays", "this board has no relays"});
            return;
        }
        if (!request->hasParam("mode")) {
            sendError(request, {400, "missing_parameter", "expected ?mode=<name>"});
            return;
        }
        // Same bound as the MQTT path: mode names are short ("normal", "drm0".."drm8"),
        // so anything empty or oversized is rejected before it is copied around.
        const String& raw = request->getParam("mode")->value();
        if (raw.length() == 0 || raw.length() > 16) {
            sendError(request, {400, "invalid_mode", "mode names are short strings"});
            return;
        }
        const std::string mode = raw.c_str();
        switch (context_.setDrmMode(mode)) {
            case CommandResult::Ok:
                request->send(200, kJson, "{\"status\":\"ok\"}");
                return;
            case CommandResult::ReadOnlyMode:
                sendError(request, {403, "read_only_mode",
                                    "security.read_only_mode is on; relays cannot move"});
                return;
            case CommandResult::Rejected:
                sendError(request, {403, "relays_disabled",
                                    "relays.enabled is off in the configuration"});
                return;
            case CommandResult::OutOfRange:
                sendError(request, {400, "invalid_mode",
                                    "not a mode for the configured relay roles"});
                return;
            case CommandResult::RateLimited:
                sendError(request, {429, "rate_limited", "too many relay commands"});
                return;
            default:
                sendError(request, {500, "relay_error", "drm command failed"});
                return;
        }
    });

    g_server->on("/api/v1/actions/discover", HTTP_POST,
                 [this, authorised, rateLimited](AsyncWebServerRequest* request) {
        if (!authorised(request) || rateLimited(request)) {
            return;
        }
        const bool extended = request->hasParam("extended") &&
                              request->getParam("extended")->value() == "true";
        if (!context_.requestDiscovery(extended)) {
            sendError(request, {409, "already_running", "discovery is already in progress"});
            return;
        }
        request->send(202, kJson, "{\"status\":\"accepted\"}");
    });

    g_server->on("/api/v1/actions/factory-reset", HTTP_POST,
                 [this, authorised, rateLimited](AsyncWebServerRequest* request) {
        if (!authorised(request) || rateLimited(request)) {
            return;
        }
        if (g_ota.running()) {  // same reasoning as the reboot action
            sendError(request, {409, "ota_in_progress",
                                "an OTA upload is in progress; retry when it has finished"});
            return;
        }
        if (!context_.requestFactoryReset()) {
            sendError(request, {500, "reset_failed", "could not clear stored configuration"});
            return;
        }
        request->send(202, kJson, "{\"status\":\"reset\",\"rebooting\":true}");
        context_.requestReboot();
    });

    // OTA. Authenticated, size-checked, and the image magic is verified before a single byte
    // reaches the inactive partition. The running image stays bootable until Update.end()
    // succeeds -- that is what the dual-app partition table is for.
    // The completion handler below always runs once the body is parsed, even when the
    // upload callback already answered with 401/409/a specific write error. send() replaces
    // any previously stored response, so an unconditional g_ota.end() here used to overwrite
    // the real error with a generic "not_finished" -- which also broke the UI's 401 retry.
    // A per-request marker in _tempObject (freed by the library with the request) records
    // that a definitive error response was already stored.
    g_server->on(
        "/api/v1/ota", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            if (request->_tempObject != nullptr) {
                return;  // the upload callback already stored the specific error response
            }
            const auto result = g_ota.end();
            if (result != ota::OtaResult::Ok) {
                std::string body;
                buildErrorPayload({400, ota::otaResultName(result), g_ota.lastError()}, "", body);
                request->send(400, kJson, body.c_str());
                return;
            }
            // end() only flips the boot partition; without this the response promised
            // "rebooting" while the old image kept running until someone power-cycled.
            // Deferred like every other reboot so the response still reaches the client.
            context_.requestReboot();
            request->send(200, kJson, "{\"status\":\"ok\",\"rebooting\":true}");
        },
        [this, authorised](AsyncWebServerRequest* request, const String& filename, size_t index,
                           uint8_t* data, size_t len, bool final) {
            (void)filename;
            (void)final;
            const auto failed = [request] { request->_tempObject = malloc(1); };
            if (request->_tempObject != nullptr) {
                return;  // an earlier chunk failed; drain the rest of the body silently
            }
            if (index == 0) {
                if (!authorised(request)) {
                    failed();  // authorised() stored the 401
                    return;
                }
                const auto begun = g_ota.begin(request->contentLength());
                if (begun != ota::OtaResult::Ok) {
                    sendError(request, {409, ota::otaResultName(begun), g_ota.lastError()});
                    failed();
                    return;
                }
                // A client that vanishes mid-upload (WiFi drop, phone sleep, closed tab)
                // never delivers a final chunk; without this the update stayed `running`
                // forever and every later OTA attempt got 409 until a manual reboot --
                // silently defeating remote recovery (review, 2026-07-21). After a normal
                // completion the disconnect still fires, but running() is false by then
                // and abort() is a no-op.
                request->onDisconnect([] {
                    if (g_ota.running()) {
                        g_ota.abort();
                    }
                });
            }
            const auto wrote = g_ota.write(data, len);
            if (wrote != ota::OtaResult::Ok) {
                // write() aborts the update internally; nothing to clean up here.
                sendError(request, {400, ota::otaResultName(wrote), g_ota.lastError()});
                failed();
                return;
            }
        });

    g_server->on("/api/v1/actions/reboot", HTTP_POST,
                 [this, authorised, rateLimited](AsyncWebServerRequest* request) {
        if (!authorised(request) || rateLimited(request)) {
            return;
        }
        // A reboot 1.5 s from now would tear down an OTA upload running on another
        // connection; the half-written image is harmless (the boot pointer only flips on a
        // successful end()) but the wasted upload is not. Refuse instead.
        if (g_ota.running()) {
            sendError(request, {409, "ota_in_progress",
                                "an OTA upload is in progress; retry when it has finished"});
            return;
        }
        request->send(202, kJson, "{\"status\":\"rebooting\"}");
        context_.requestReboot();
    });

#if ENABLE_PROMETHEUS
    g_server->on("/metrics", HTTP_GET, [this, firstDevice](AsyncWebServerRequest* request) {
        const auto snapshot = firstDevice();
        if (!snapshot) {
            request->send(503, "text/plain", "# no device configured\n");
            return;
        }
        const auto body = prometheus::buildMetrics(*snapshot, context_.bridgeInfo(),
                                                   context_.diagnostics->snapshot());
        request->send(200, "text/plain; version=0.0.4", body.c_str());
    });
#endif

    g_server->onNotFound([this](AsyncWebServerRequest* request) {
        // Captive portal: while the setup AP is up, the DNS server resolves every name to
        // us, so the phone's connectivity probes (Android /generate_204, Apple
        // /hotspot-detect.html, Windows /connecttest.txt) arrive here as unknown paths.
        // Redirecting them to the setup page is what makes the OS pop it up by itself.
        // API paths keep their honest 404 -- a typo'd endpoint must not return HTML.
        const bool portal = context_.portalActive && context_.portalActive();
        if (portal && !request->url().startsWith("/api/")) {
            request->redirect("http://" + WiFi.softAPIP().toString() + "/");
            return;
        }
        sendError(request, {404, "not_found", "no such endpoint"});
    });

    g_server->addHandler(g_events);
    g_server->begin();
    started_ = true;
    return true;
}

void RestApi::stop() {
    if (!started_) {
        return;
    }
    g_server->end();
    delete g_server;
    g_server = nullptr;
    delete g_events;
    g_events = nullptr;
    started_ = false;
}

}  // namespace heliograph::rest

#else  // !ESP32

namespace heliograph::rest {

// Host builds have no TCP stack to bind to. Everything worth testing -- the payloads, the
// config redaction, the Prometheus text -- lives in pure units and is covered by test_rest.

RestApi::RestApi(RestContext context, uint16_t port) : context_(std::move(context)), port_(port) {}
RestApi::~RestApi() = default;
bool RestApi::begin() { return false; }
void RestApi::stop() { started_ = false; }
void RestApi::notifyState(const DeviceState&, uint64_t) {}

bool RestApi::collectBody(AsyncWebServerRequest*, const uint8_t*, size_t, size_t, size_t,
                          std::string*& out) {
    out = nullptr;
    return false;
}
void RestApi::releaseBody() {
    bodyOwner_ = nullptr;
    bodyBuffer_.clear();
}

}  // namespace heliograph::rest

#endif
