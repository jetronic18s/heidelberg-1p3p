# Local patches to esp-modbus 2.1.4

This is the registry package `espressif/esp-modbus` 2.1.4 (component code
only: no examples, tests or tools) with two patches, marked
`heidelberg-1p3p patch` in the code. Drop the copy once upstream fixes it.

## Problem

A wallbox answer that arrives just around the master's respond timeout
(`CONFIG_FMB_MASTER_TIMEOUT_MS_RESPOND`, 500 ms here) puts the master out of
step for good:

1. `EV_FRAME_RECEIVED` is queued, but the respond timer fires before
   `mbm_poll()` disables it and queues `EV_ERROR_PROCESS` (timeout).
2. `EV_FRAME_RECEIVED` queues `EV_EXECUTE` behind it.
3. `EV_ERROR_PROCESS` ends the transaction: timeout bit, `curr_trans_id = 0`.
   The caller returns with a timeout, correctly.
4. `EV_EXECUTE` no longer matches ("execution is expired") and queues another
   `EV_ERROR_PROCESS`, which sets `EV_ERROR_EXECUTE_FUNCTION` in the event
   group although nobody waits for it.
5. The next request returns at once with that stale bit
   (`ESP_ERR_INVALID_RESPONSE`) while its own transaction is still running.
   Its answer comes later and is written through `reg_buffer_ptr` into the
   caller's buffer, which may already be gone (stack corruption), and sets an
   OK bit nobody waits for.
6. From then on every request returns the result of the previous one, until
   a restart.

Seen on an ESP32 during OTA updates (flash writes delay the UART ISR and the
Modbus task): wrong register values, LoadProhibited and double exceptions.
Reproduced without OTA by answering 485-515 ms late (bench test 35).

## Patches

- `modbus/mb_objects/mb_master.c`, `EV_EXECUTE`: an expired execution only
  logs and drops the answer, it no longer queues `EV_ERROR_PROCESS`.
- `modbus/mb_ports/common/port_event.c`, `mb_port_event_res_take()`: clears
  the request result bits when a new request takes the resource. This assumes
  serialized requests (the firmware holds `_rtuMutex` around every request).

The firmware also never hands esp-modbus a caller's stack buffer
(`PhaseSwitch::rtuRequest()`), so a late write cannot corrupt memory.
