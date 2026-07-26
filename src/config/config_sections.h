// SPDX-License-Identifier: MIT
//
// The part of the configuration document that both writers agree on.
//
// A Configuration is serialised twice, for two audiences: serializeConfigForStorage writes the
// blob that goes into NVS, serializeConfig writes the body of GET /api/v1/config. They differ
// in exactly one respect -- what they do with credentials. The store keeps them, because the
// bridge has to reconnect after a reboot; the API replaces each with a `*_set` flag, because
// that endpoint is unauthenticated and a mask is not a redaction.
//
// Everything else -- what the bridge is called, which driver, which broker, the poll interval,
// the line settings -- is the same field written the same way, and until now it was written
// twice in two files. The failure mode of that is specific and quiet: add a setting, remember
// the API writer, forget the storage writer, and the UI shows the field, accepts a change,
// reports success, and loses it on the next reboot. Nothing throws, nothing logs.
//
// An internal header rather than part of configuration.h: the two callers live in different
// translation units, but nobody else has any business calling this, and configuration.h is
// included widely enough that dragging ArduinoJson into it would be a real cost.

#pragma once

#include <ArduinoJson.h>

#include "configuration.h"

namespace heliograph::config_sections {

/// Writes every section that is identical in both documents. Callers add `version` and their
/// own credential handling on top; the objects this creates (`wifi`, `mqtt`) are left open for
/// exactly that.
void writeCommon(JsonDocument& doc, const Configuration& config);

}  // namespace heliograph::config_sections
