// Scheduled device monitoring (spec §8.3; protocol §7.6). `set_monitor` persists
// its config to NVS so the schedule survives reboots and server restarts; the
// worker calls monitorTick() while idle to run a due cycle — ICMP ping and/or TCP
// port probes per device — publishing {cycle_ts, results[]} on the node's
// `monitor` topic through the outbox.
//
// Cycles that fall due during a survey are skipped, not queued (§6.2): the node
// reports only observations it actually made, so a gap in the history is honest.
#pragma once

// Loads any persisted monitor config from NVS. Call once at boot.
void monitorInit();

// Applies and persists a new config from `set_monitor` args (serialized JSON).
// Called by the MQTT task when a set_monitor command arrives.
void monitorSetFromArgs(const char* argsJson);

// Runs one monitor cycle if the schedule is enabled and due and the radio is in
// STATION mode. Cheap and non-blocking when nothing is due. Called from the idle
// worker loop.
void monitorTick();
