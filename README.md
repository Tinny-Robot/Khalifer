# Cold-room Ambient Sensor

An ESP32-based telemetry node that measures cold-room temperature and humidity with a DHT22 sensor and sends the readings to a backend over a SIM800L cellular connection.

## What it does

1. Starts the ESP32 watchdog and DHT22 sensor.
2. Initializes the SIM800L modem and detects its UART baud rate.
3. Registers on the cellular network and selects a Nigerian APN from the SIM/operator when possible.
4. Reads temperature and relative humidity every 60 seconds.
5. Sends each valid reading as an HTTPS `POST` request.
6. Recovers from failures by reconnecting GPRS, resetting the modem, or rebooting the ESP32.

The Arduino sketch is at [ambient_sensor/ambient_sensor.ino](ambient_sensor/ambient_sensor.ino).

## Hardware

- ESP32 development board
- DHT22 temperature and humidity sensor
- SIM800L GSM/GPRS modem
- Two LM2596 buck converters
- A working 2G SIM card and suitable antenna
- Shared ground between the ESP32 and SIM800L power supplies

### Wiring

| Component | Connection |
| --- | --- |
| DHT22 VCC | ESP32 3.3 V |
| DHT22 GND | GND |
| DHT22 DATA | ESP32 GPIO 21 |
| SIM800L TX | ESP32 GPIO 16 (UART RX) |
| SIM800L RX | ESP32 GPIO 17 (UART TX) |
| SIM800L VCC | Dedicated 4.0-4.2 V supply |
| SIM800L GND | Common GND |
| SIM800L RST | Not used by default |

See [docs/wiring-and-power.md](docs/wiring-and-power.md) for power and level-shifting notes.

## Software setup

### Arduino IDE

1. Install the ESP32 board package.
2. Select an ESP32 board such as **ESP32 Dev Module**.
3. Install these libraries through Library Manager:
   - TinyGSM by Volodymyr Shymanskyy
   - ESP_SSLClient by Mobizt
   - DHT sensor library by Adafruit
   - Adafruit Unified Sensor
4. Open `ambient_sensor/ambient_sensor.ino`.
5. Review the device and network settings near the top of the sketch.
6. Upload the sketch and open Serial Monitor at `115200` baud.

The sketch uses insecure TLS certificate validation (`setInsecure()`) so a backend certificate rotation does not stop the device. This protects availability but does not authenticate the server certificate. See [docs/configuration-and-api.md](docs/configuration-and-api.md).

## Configuration

Edit the constants in the **user config** section before deploying:

- `DEVICE_ID`: identifier sent as the `unit` field.
- `HOST`: backend hostname.
- `PATH`: telemetry endpoint.
- `APN_OVERRIDE`, `APN_USER_OVERRIDE`, and `APN_PASS_OVERRIDE`: optional manual cellular settings.
- `PIN_MODEM_PWRKEY` and `PIN_MODEM_RST`: set these only when the corresponding modem pins are physically wired.

Do not publish production device identifiers or other deployment-specific values unless the task owner expects them to be public.

## Telemetry request

The device sends JSON in this shape:

```json
{
  "unit": "device-id",
  "temperatureC": 4.25,
  "humidityPercent": 78
}
```

The response is accepted only when the HTTP status is `200` or `201` and the response body contains `"success": true`. See [docs/configuration-and-api.md](docs/configuration-and-api.md) for the complete request contract.

## Repository layout

```text
.
├── ambient_sensor/
│   └── ambient_sensor.ino
├── docs/
│   ├── configuration-and-api.md
│   └── wiring-and-power.md
├── .gitignore
└── README.md
```

## Troubleshooting

- **No AT response:** check SIM800L power stability, common ground, antenna, UART wiring, and baud output in Serial Monitor.
- **SIM not ready:** verify the SIM is inserted, active, and supports 2G service.
- **Network registration fails:** confirm coverage and inspect the reported signal quality (`CSQ`).
- **GPRS fails:** use `APN_OVERRIDE` with the APN supplied by the carrier.
- **DHT22 reads fail:** check the data wire, ground, sensor power, and the sensor's required pull-up resistor.
- **TLS or HTTP fails:** confirm the modem has GPRS data and that `HOST`/`PATH` match the backend.

## License

No license has been selected yet. Add a license before making this repository public if the task owner requires one.