#pragma once

/**
 * Identifiers for SCS Telemetry gameplay events (scs_telemetry_gameplay_event_t::id).
 * Defined in the SDK as SCS_TELEMETRY_GAMEPLAY_EVENT_* in
 * include/common/scssdk_telemetry_common_gameplay_events.h.
 *
 * A plugin registers one callback with register_for_event(SCS_TELEMETRY_EVENT_gameplay, ...);
 * the delivered id string selects which of these occurred.
 *
 * Other telemetry callbacks use scs_event_t constants from scssdk_telemetry_event.h:
 * SCS_TELEMETRY_EVENT_frame_start, frame_end, paused, started, configuration, gameplay.
 */
enum GameEvent {
    JobCancelled,
    JobDelivered,
    PlayerFined,
    PlayerTollgatePaid,
    PlayerUseFerry,
    PlayerUseTrain,
    Unknown,
};
