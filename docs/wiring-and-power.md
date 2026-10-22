# Wiring and Power

## Connections

| Signal | ESP32 | Module | Notes |
| --- | --- | --- | --- |
| Sensor power | 3.3 V | DHT22 VCC | Use a stable supply. |
| Sensor ground | GND | DHT22 GND | Common ground is required. |
| Sensor data | GPIO 21 | DHT22 OUT | Add the sensor pull-up recommended by its module/datasheet. |
| Modem receive | GPIO 16 / UART2 RX | SIM800L TX | 3.3 V ESP32 input is suitable. |
| Modem transmit | GPIO 17 / UART2 TX | SIM800L RX | Confirm the modem board's input-level requirements. |
| Modem power | Dedicated 4.0-4.2 V rail | SIM800L VCC | Do not power the modem from the ESP32 3.3 V pin. |
| Modem reset | Optional | SIM800L RST | Only configure `PIN_MODEM_RST` if wired. |

## Recommended power arrangement

```text
12 V input
  ├── LM2596 #1: 5 V    -> ESP32 VIN
  └── LM2596 #2: 4.0 V -> SIM800L VCC
```

The SIM800L can draw high transient current while transmitting. Keep its supply wires short and low impedance, place bulk capacitance close to the modem, and connect the grounds together at a sensible common point.

The sketch cannot compensate for modem brownouts. Unstable modem power commonly appears as repeated AT timeouts, dropped GPRS sessions, or unexplained resets.