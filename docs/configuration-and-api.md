# Configuration and API

## Device configuration

The deployment settings are constants near the top of `ambient_sensor/ambient_sensor.ino`:

| Setting | Purpose | Default |
| --- | --- | --- |
| `DEVICE_ID` | Value sent as `unit` | Existing device identifier in the sketch |
| `HOST` | HTTPS backend hostname | `soltech-backend-drab.vercel.app` |
| `PATH` | Telemetry route | `/api/v1/telemetry` |
| `POST_INTERVAL_MS` | Time between cycles | 60 seconds |
| `APN_OVERRIDE` | Optional manual APN | Empty, auto-detect enabled |

The automatic APN selection recognizes Nigerian MNCs for Airtel, MTN, Glo, and 9mobile, then tries a small generic fallback list. A manual APN override takes precedence over automatic selection.

## Request

```http
POST /api/v1/telemetry HTTP/1.1
Host: soltech-backend-drab.vercel.app
Content-Type: application/json
Connection: close

{"unit":"device-id","temperatureC":4.25,"humidityPercent":78}
```

`temperatureC` is sent with two decimal places. Humidity is rounded to an integer and constrained to `0` through `100`.

## Accepted response

The device considers a request successful only when:

- the HTTP status is `200` or `201`; and
- the response body contains a JSON-like field with `"success": true`.

The response buffer is limited to 1023 bytes of content. Keep the endpoint response compact.

## Reliability behavior

- A sensor read is attempted up to three times.
- A telemetry POST is attempted twice per cycle.
- After two failed cycles, the device tries to restore GPRS.
- After four failed cycles, it performs a full modem reset.
- After six failed cycles, it restarts the ESP32.
- During startup, modem bring-up is retried and the ESP32 reboots after eight failed attempts.

## Security note

The sketch currently calls `sslClient.setInsecure()`. Traffic is encrypted, but the server certificate is not validated. For a production deployment, prefer a CA certificate or certificate pinning strategy that can be updated without physically bricking deployed nodes.