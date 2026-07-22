# Hardware — Waveshare ESP32-S3-Relay-1CH

Status: **geverifieerd** tegen officieel schema en officiële demo. Geen enkele pin in dit
document is geraden.

## Bronnen

| Bron | Locatie | Gebruikt voor |
|---|---|---|
| Wiki | <https://www.waveshare.com/wiki/ESP32-S3-Relay-1CH> | Boardoverzicht, jumper, FAQ |
| Schema (PDF) | <https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-schematic.pdf> | Netlijsten, chipkeuze, isolatie |
| Demo (ZIP) | <https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-Demo.zip> | `WS_GPIO.h`, `WS_RS485.cpp`, `I2C_Driver.h` |

De wiki zelf bevat **geen** pin-tabel. Alle pinnen hieronder komen uit de demo-broncode en zijn
kruisgecontroleerd tegen het schema.

## Pinout

| Functie | GPIO | Bron (primair) | Bron (bevestiging) |
|---|---|---|---|
| RS485 TX (UART1) | **17** | `WS_GPIO.h: #define TXD1 17` | Schema net `TXD1` → isolator → SP3485 `DI` |
| RS485 RX (UART1) | **18** | `WS_GPIO.h: #define RXD1 18` | Schema net `RXD1` ← isolator ← SP3485 `RO` |
| RS485 DE/RE (richting) | **21** | `WS_GPIO.h: #define TXD1EN 21` | Schema net `RS485_EN` → SP3485 `RE`(2)+`DE`(3) |
| Relais CH1 | **47** | `WS_GPIO.h: #define GPIO_PIN_CH1 47` | Schema blok `Relay` / `CH1` |
| I2C SCL (RTC) | **38** | `I2C_Driver.h: #define I2C_SCL_PIN 38` | Schema `GPIO38` |
| I2C SDA (RTC) | **39** | `I2C_Driver.h: #define I2C_SDA_PIN 39` | Schema `IO39` |

## Componenten (uit schema)

| Component | Type | Relevantie |
|---|---|---|
| Module | ESP32-S3-**WROOM-1U** (chip `ESP32-S3R8`) | 8 MB PSRAM embedded |
| Flash | `W25Q128JVSI` | **16 MB** extern → board is functioneel N16R8 |
| RS485 transceiver | `SP3485EN` | `RE`(pin 2) + `DE`(pin 3) doorverbonden op net `RS485_EN` |
| Digitale isolator | `π131M31` | 3 kanalen: TXD, RS485_EN (uitgaand), RXD (inkomend) |
| Geïsoleerde voeding | `B0505S-1WR3` | Galvanische scheiding RS485-zijde (`SGND` ≠ `GND`) |
| Terminatie | `R23 = 120R` + header `H1` | Jumper-selecteerbaar, zie §Terminatie |
| RTC | `PCF85063AT` @ I2C `0x51` | Optioneel, niet nodig voor MVP |
| Beveiliging | `SMF12CA`, `SM712`, `BSMD1206-050` | TVS / surge / resettable fuses op RS485 |
| USB | direct `USB_N`/`USB_P` → `D_N`/`D_P` | **Geen** CH340/CH343 → native USB-CDC |

## Kritieke correctie: RS485-richtingsbesturing is NIET passief-automatisch

De opdracht ging uit van "automatische hardwarematige RS485-richtingsbesturing" en van de
aanname dat softwarematige DE/RE-aansturing achterwege kan blijven. Het schema en de demo
weerleggen dat:

- `SP3485EN` heeft losse `DE`/`RE`-pinnen, doorverbonden naar het net `RS485_EN`.
- `RS485_EN` wordt via de isolator aangestuurd door **GPIO21**.
- Er is geen auto-direction-circuit (geen TX-detectie op DI, geen RC-timing).

De officiële demo lost dit op door de **UART-hardware** de richting te laten sturen:

```c
// ws-demo/Arduino/examples/MAIN_WIFI_AP/WS_RS485.cpp
lidarSerial.begin(Baudrate, SERIAL_8N1, RXD1, TXD1);
lidarSerial.setPins(-1, -1, -1, TXD1EN);          // 4e argument = RTS-pin
lidarSerial.setMode(UART_MODE_RS485_HALF_DUPLEX); // UART stuurt RTS zelf, bit-exact
```

De wiki-claim "automatic direction control" klopt dus in de zin dat er **geen software-toggle
per byte** nodig is — maar alleen omdat de ESP32-S3 UART-peripheral in
`UART_MODE_RS485_HALF_DUPLEX` de RTS-lijn zelf op de juiste bit-tijd omschakelt.

**Consequentie voor de transportlaag:** GPIO21 moet als RTS worden geconfigureerd en de UART
moet expliciet in RS485-half-duplexmodus worden gezet. Dit is precies één keer configureren
bij `begin()`, daarna is het transparant. Handmatig togglen in software is expliciet
**niet** gewenst (te traag, race met de laatste stopbit).

## Relais — veiligheidsregels voor de MVP

Het relais zit op GPIO47 en is in de MVP **buiten scope**.

- ESP32-S3 GPIO's zijn na reset input/hi-Z; het relais is dan niet bekrachtigd.
- De firmware zet GPIO47 als eerste actie in `setup()` op `OUTPUT` + `LOW`, vóór wifi,
  RS485 of enige andere initialisatie.
- Geen enkele output-adapter (MQTT/REST/Modbus/web) krijgt een pad naar deze pin.
- Bij elke faalroute blijft de pin `LOW`; er is geen code die hem ooit `HIGH` zet.

Motivering `LOW` = uit: `WS_Relay.cpp` definieert `Relay_Open()` als `digitalWrite(HIGH)` en
`Relay_Closs()` [sic] als `digitalWrite(LOW)`. Actief-hoog dus.

## Terminatie (120 Ω)

Header `H1` (3-pin) schakelt `R23` (120 Ω) over de RS485-lijn. Plaats de jumper op de
120R-stand **alleen** wanneer de bridge fysiek aan het uiteinde van de RS485-bus zit. De
wiki-FAQ noemt ontbrekende terminatie expliciet als oorzaak van onbetrouwbare communicatie.

## Gevolgen voor de build

| Feit | Gevolg |
|---|---|
| 16 MB flash | Ruimte voor dual-app OTA-partities + LittleFS. Geen drivers hoeven weggelaten. |
| 8 MB PSRAM (octal) | Vereist `-DBOARD_HAS_PSRAM` en octal-PSRAM-config. |
| Native USB-CDC | Vereist `-DARDUINO_USB_CDC_ON_BOOT=1` voor seriële logging over USB-C. |
| Geïsoleerde RS485 | `SGND` niet doorverbinden met `GND`; isolatie niet ondermijnen bij bedrading. |

## Openstaand / te valideren op hardware

1. **Boot-knop GPIO** — de FAQ noemt BOOT+RESET, maar de tekstextractie van het schema gaf de
   BOOT-pin niet eenduidig prijs. ESP32-S3 gebruikt standaard GPIO0, maar dit is **niet
   geverifieerd** en wordt daarom nog niet als pin-constante vastgelegd. Voor de
   provisioning-reset (§29) is dit nodig — te bevestigen bij Fase 3 op echte hardware.
2. **RS485 TX/RX-status-LED's** — genoemd in de productomschrijving; de aansturing is niet in
   de demo terug te vinden en is vermoedelijk hardwarematig op de transceiver-lijnen. Geen
   firmware-actie nodig.
