# EverSolar / Zeversolar legacy RS485-protocol

Status: gereconstrueerd uit de referentie-implementatie; **sinds 2026-07-19 deels geverifieerd
tegen een echte TL3000-20** (Fase 3, eerste contact). Hardware-bevindingen zijn als zodanig
gemarkeerd; twee gecapturede frames staan byte-voor-byte in `tools/gen_fixtures.py`, dat
zichzelf tegen de captures verifieert. Waar iets onbekend is, staat dat er expliciet bij — er
zijn geen velden verzonnen.

## Herkomst en licentie

| | |
|---|---|
| Referentie | <https://github.com/solmoller/eversolar-monitor> |
| Commit | `784c2fc2f6b6ed4e70bf7519d0939ec1195a1813` (2026-02-21) |
| Licentie | **MIT** — © 2021 Henrik Møller Jørgensen |
| Oorspronkelijke basis | Steve Cliffe `<steve@sjcnet.id.au>`, Eversolar PMU logger |

MIT is permissief; hergebruik van protocolkennis en herimplementatie in C++ is toegestaan mits
de copyrightvermelding en licentietekst behouden blijven. Concreet:

- `LICENSE-THIRD-PARTY.md` in de repo-root krijgt de volledige MIT-tekst van eversolar-monitor.
- Elk bestand in `src/drivers/eversolar_legacy/` krijgt een header die vermeldt dat de
  protocolkennis is afgeleid van eversolar-monitor (MIT) en van Steve Cliffe's PMU logger.
- Er wordt **geen** Perl-code vertaald of gekopieerd; alleen het protocol wordt
  geherimplementeerd. De applicatiestructuur (sqlite, pvoutput, FTP-upload) blijft buiten beeld.

## Fysieke laag

Uit `serial_connect()` (`eversolar.pl:559-576`) — hardcoded, niet configureerbaar:

| Parameter | Waarde |
|---|---|
| Baudrate | **9600** |
| Databits | 8 |
| Pariteit | none |
| Stopbits | 1 |
| Handshake | none |

Dit is het **enige** relevante seriële profiel voor deze driver. De driver-descriptor geeft
daarom precies één `SerialProfile` op: `9600 8N1`. Andere baudrates worden niet geprobeerd.

## Frameformaat

Uit de headercommentaar (`eversolar.pl:102-110`) en `send_request()` (`:395-408`):

```
Offset  Lengte  Veld
------  ------  --------------------------------------------------
0       2       Header: 0xAA 0x55
2       2       Bronadres
4       2       Bestemmingsadres
6       1       Control code
7       1       Function code
8       1       Datalengte N
9       N       Data
9+N     2       Checksum (uint16, big-endian)
------  ------  --------------------------------------------------
Totaal: 11 + N bytes
```

### Adressering

De PMU (onze bridge) is de master. Uit `send_request()`:

```perl
@tmp_packet = ( 0xAA, 0x55, OUR_ADDRESS, 0x00, 0x00, $destination_address, ... );
#                           ^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^
#                           bron = 0x01 0x00   bestemming = 0x00 <addr>
```

| | Waarde |
|---|---|
| Bronadres PMU | `0x01 0x00` (`OUR_ADDRESS = 0x01`, `eversolar.pl:268`) |
| Bestemming broadcast | `0x00 0x00` |
| Bestemming inverter | `0x00 <toegewezen adres>` |
| Eerste toegewezen adres | `0x10` (`START_INVERTER_ADDRESS`, `:267`) |

De PMU wijst adressen zelf toe, oplopend vanaf `0x10`. Dit is dus **geen** vast slave-adres
zoals bij Modbus: het adres is het resultaat van een registratieprocedure.

### Checksum

Uit `send_request()` (`:403-406`) en `validate_checksum()` (`:750-760`):

```
checksum = (som van alle bytes vanaf offset 0 t/m 8+N) mod 65536
```

Een **eenvoudige 16-bits optelsom over álle voorafgaande bytes, inclusief de 0xAA 0x55-header**,
big-endian weggeschreven. Geen CRC-16, geen Modbus-CRC.

De referentie voegt één validatie toe (`:759`):

```perl
return ( $csum > 0 ) && ( $csum == ( ( $packet[$len-2] << 8 ) + $packet[$len-1] ) );
```

> "packets with all zeroes pass checksum test, but are invalid"

Een frame van louter nullen heeft checksum 0 en zou dus geldig lijken — precies het geval dat
anders als "geldige nulmeting" wordt gepubliceerd.

**Deze `csum > 0`-guard nemen we bewust níét over.** Hij bestaat alleen omdat de referentie de
`AA 55`-header nergens controleert. Wij doen dat wel, en daarmee is de bytesom van een geldig
frame altijd minstens `0xAA + 0x55 = 255`: een all-zero frame sneuvelt al eerder, op een
sterkere grond (`BadHeader`).

Erger nog, de guard is licht fout: met een echte header kán de som alsnog op precies 65536
uitkomen en naar 0 truncateren. Dat is dan een correcte checksum, en de guard zou zo'n frame
ten onrechte verwerpen — grofweg 1 op de 65536 frames. Beide gevallen liggen vast als test in
`test_eversolar_checksum`.

## Control- en function codes

Uit `%CTRL_FUNC_CODES` (`eversolar.pl:190-223`):

| Control | Function | Naam | Request → Response |
|---|---|---|---|
| `0x10` REGISTER | `0x00` | Offline query | `10 00` → `10 80` |
| `0x10` REGISTER | `0x01` | Send register address | `10 01` → `10 81` |
| `0x10` REGISTER | `0x04` | Re-register | `10 04` → *(geen response)* |
| `0x11` READ | `0x00` | Query description | `11 00` → `11 80` |
| `0x11` READ | `0x02` | Query normal info | `11 02` → `11 82` |
| `0x11` READ | `0x03` | Query inverter ID | `11 03` → `11 83` |
| `0x12` WRITE | — | *leeg in referentie* | — |
| `0x13` EXECUTE | — | *leeg in referentie* | — |

Responses zetten bit 7 van de function code (`0x02` → `0x82`).

**`0x12` WRITE en `0x13` EXECUTE zijn in de referentie leeg.** Er is dus geen enkele bekende
schrijfoperatie voor deze omvormer. Dat sluit naadloos aan op de read-only MVP: de
EverSolar-driver geeft voor elk commando `Unsupported` terug, en dat is geen tijdelijke
beperking maar een eigenschap van wat we van dit protocol weten.

## Registratieprocedure

Alle registratieverkeer gaat naar broadcast (`0x00`).

```
1. RE_REGISTER (10 04) → broadcast, 8× herhaald, geen response verwacht
   eversolar.pl:582-594. Commentaar zegt "3 times as per protocol specs",
   de code stuurt er 8. Alle inverters vergeten hun registratie.

2. OFFLINE_QUERY (10 00) → broadcast
   Response (10 80): serienummer als ASCII-bytes, variabele lengte.
   eversolar.pl:602-608

3. SEND_REGISTER_ADDRESS (10 01) → broadcast
   Data = <serienummer-bytes> + <1 byte toe te wijzen adres>
   Response (10 81): 1 byte ACK. Verwacht: 0x06 (ASCII ACK).
   eversolar.pl:610-620

4. QUERY_INVERTER_ID (11 03) → <toegewezen adres>
   Response (11 83): ASCII identificatiestring.
   eversolar.pl:625-628
```

Daarna wordt periodiek `QUERY_NORMAL_INFO` (`11 02`) naar het toegewezen adres gestuurd.
De referentie herhaalt stap 2-4 elke minuut om nieuwe omvormers te vinden (`:1037-1040`).

**Alle vier de stappen zijn read-only.** Er wordt niets in de omvormer geconfigureerd behalve
een vluchtig bus-adres, dat de omvormer zelf bij spanningsloos worden vergeet. Deze procedure
is dus veilig als probe (zie `docs/architecture.md`, discovery).

## Meetdata — `QUERY_NORMAL_INFO` (`11 02` → `11 82`)

De payload is een array van **16-bits big-endian words** (`parse_packet()`, `:766-777`):

```perl
$data[$j++] = ( $packet[$i] << 8 ) + $packet[$i+1];
```

De indices hieronder zijn dus **word-indices**, niet byte-offsets.

### Layout bij 1 string (14 words = 28 bytes payload)

`eversolar.pl:226-241`

| Word | Veld | Type | Schaal | Eenheid | Measurement-ID |
|---|---|---|---|---|---|
| 0 | TEMP | **int16** | ÷10 | °C | `inverter.temperature` |
| 1 | E_TODAY | uint16 | ÷100 | kWh | `energy.today` |
| 2 | VPV | uint16 | ÷10 | V | `dc.mppt_1.voltage` |
| 3 | IPV | uint16 | ÷10 | A | `dc.mppt_1.current` |
| 4 | IAC | uint16 | ÷10 | A | `ac.phase_l1.current` |
| 5 | VAC | uint16 | ÷10 | V | `ac.phase_l1.voltage` |
| 6 | FREQUENCY | uint16 | ÷100 | Hz | `ac.frequency` |
| 7 | PAC | uint16 | **×1** | W | `ac.power.total` |
| 8 | IMPEDANCE | uint16 | ? | ? | *niet gepubliceerd, zie onder* |
| 9 | E_TOTAL_HI | uint16 | — | — | *hoog word van energy.total* |
| 10 | E_TOTAL_LO | uint16 | ÷10 | kWh | `energy.total` |
| 11 | NA_2 | uint16 | ? | ? | *onbekend, niet gepubliceerd* |
| 12 | HOURS_UP | uint16 | ×1 | h | `inverter.operating_hours` |
| 13 | OP_MODE | uint16 | — | — | `status_code` |

### Layout bij 2 strings (16 words = 32 bytes payload)

`eversolar.pl:244-261`

| Word | Veld | Measurement-ID |
|---|---|---|
| 0 | TEMP | `inverter.temperature` |
| 1 | E_TODAY | `energy.today` |
| 2 | VPV | `dc.mppt_1.voltage` |
| 3 | VPV2 | `dc.mppt_2.voltage` |
| 4 | IPV | `dc.mppt_1.current` |
| 5 | IPV2 | `dc.mppt_2.current` |
| 6 | IAC | `ac.phase_l1.current` |
| 7 | VAC | `ac.phase_l1.voltage` |
| 8 | FREQUENCY | `ac.frequency` |
| 9 | PAC | `ac.power.total` |
| 10 | NA_0 | *onbekend* |
| 11 | E_TOTAL_HI | *hoog word* |
| 12 | E_TOTAL_LO | `energy.total` |
| 13 | NA_2 | *onbekend* |
| 14 | HOURS_UP | `inverter.operating_hours` |
| 15 | OP_MODE | `status_code` |

Let op: de volgorde van `IAC`/`VAC` en de positie van `PAC` verschillen tussen beide layouts.
Een verkeerde layoutkeuze levert dus plausibel-ogende maar volstrekt foute waarden op.

### Layout-autodetectie — en de hypothese eronder

De referentie laat de gebruiker de layout kiezen via een ini-optie (`strings = 1` of `2`,
`eversolar.ini:8`) en `die`t bij een andere waarde. Wij leiden de layout in plaats daarvan af
uit de payloadlengte:

| Payloadlengte | Words | Layout |
|---|---|---|
| 28 bytes | 14 | 1 string |
| 32 bytes | 16 | 2 strings |
| anders | — | **frame verwerpen**, geen data publiceren |

**Dit is een hypothese, geen vastgesteld feit.** Ze volgt uit de twee index-maps van 14 en 16
entries in de referentie. Maar er is tegenbewijs: als de lengte de layout eenduidig bepaalt,
waarom is het dan een config-optie?

Twee verklaringen, en we weten nog niet welke klopt:

1. De auteur heeft er niet aan gedacht. Plausibel: de referentie valideert vrijwel niets — geen
   header-check, geen lengtecontrole, en bij een verkeerde `strings=` indexeert Perl stilzwijgend
   buiten de array (`undef` → 0) in plaats van te falen.
2. De lengte is niet onderscheidend en alleen de interpretatie verschilt. Dan is de detectie stuk.

#### Wat de TL3000-20 échT stuurt — hypothese weerlegd (2026-07-19)

De echte 1-string TL3000-20 stuurt **44 bytes**: de 14 single-string-words gevolgd door een
8-word-staart van nullen en `0xFFFF`, betekenis onbekend. Niet 28. De waarden in de eerste 14
words zijn tegen de werkelijkheid geverifieerd (38,6 °C, 346,4 V × 2,0 A DC ≈ 655 W AC,
50,03 Hz, 35.445,9 kWh lifetime — onderling consistent en fysisch plausibel; frame als
`kRespNormalInfoCaptured` in de fixtures).

Daarmee valt het kwartje over de config-optie in de referentie: `eversolar.pl` indexeert words
en negeert alles erachter, dus de payloadlengte was voor de auteur nooit relevant — en dus ook
nooit beslissend. Verklaring 2 uit de vorige revisie van dit document was de juiste.

`Auto` accepteert nu 28 én 44 als single (44 is hardware-waarheid, geen gok), 32 als dual, en
verwerpt al het andere. Of 2-string-firmware werkelijk 32 stuurt (of 44, of iets anders) blijft
op deze hardware **niet te valideren**; het dual-pad blijft geconstrueerd. `LayoutSelection`
blijft het ontsnappingsluik.

#### Ontsnappingsluik

Omdat de hypothese onbewezen is, kent `decodeNormalInfo()` een `LayoutSelection`:

| Selectie | Gedrag |
|---|---|
| `Auto` | 28 → single, 32 → dual, anders `UnknownLayout` (standaard) |
| `ForceSingleString` | interpreteert als 1 string; eist `len >= 28` |
| `ForceDualString` | interpreteert als 2 strings; eist `len >= 32` |

Een geforceerde layout eist alleen dat de payload lang genoeg is, niet dat de lengte exact
klopt. Zo blijft een toestel dat de hypothese tegenspreekt uitleesbaar zonder firmwarewijziging.
Een te korte payload geeft `LayoutMismatch` — er wordt nooit buiten de buffer gelezen.

Dit is als generieke driveroptie ontsloten, niet als veld in het configmodel:

```json
{ "driver": { "id": "eversolar_legacy", "options": { "layout": "auto" } } }
```

De driver declareert de sleutel `layout` met toegestane waarden `auto|single|dual` in zijn
descriptor; `validateDriverOptions()` toetst daarop en de webinterface rendert hem generiek.
Zo blijft `Configuration` vrij van merkspecifieke velden. Conform §19 "handmatige configuratie
is altijd mogelijk". Zie `docs/architecture.md`.

### Schaalfactoren en signedness

- **TEMP is signed.** `eversolar.pl:1059-1061`: `if ($data >= 0x8000) { $data -= 0x10000 }`,
  met commentaar "Temperature is signed, -0.1 = 0xFFFF". Alle overige velden worden als
  unsigned behandeld.
- **PAC heeft geen schaalfactor** — het ruwe word is watt. (`:1058`, `:1097`)
- **E_TODAY is ÷100**, terwijl **E_TOTAL ÷10** is. Verschillende resoluties, makkelijk fout.

### Bevestigde fout in de referentie: `energy.total`

`eversolar.pl:1057`:

```perl
my $e_total = $data[E_TOTAL] / 10 + $data[NA_1] * 65535 / 10;
```

Het hoge word wordt met **65535** vermenigvuldigd. Correct is **65536** (`1 << 16`). De juiste
berekening is een gewone uint32:

```
energy.total [kWh] = ((E_TOTAL_HI << 16) | E_TOTAL_LO) / 10.0
```

**Gevolg voor hardwarevalidatie (Fase 3/§42):** onze waarde wijkt systematisch af van
eversolar-monitor met `E_TOTAL_HI × 0.1 kWh`. Bij een totaal rond 18.452,7 kWh is
`E_TOTAL_HI = 2`, dus onze meting valt **0,2 kWh hoger** uit. Dat is geen fout in onze
implementatie en het acceptatiecriterium "energie exact binnen schaalfactor" moet hierop
worden aangepast: verwacht een afwijking van exact `HI × 0.1 kWh`, en verifieer dat het
verschil precies die waarde is.

Dat `NA_1`/`E_TOTAL` samen een uint32 vormen is een reverse-engineering-conclusie van de
oorspronkelijke auteur (commentaar `:256`: "NA_1 is actually upper bytes of total production"),
niet een gedocumenteerde specificatie. Vertrouwen: **hoog maar te bevestigen** — het is op
hardware pas hard te maken zodra `E_TOTAL_LO` een keer over `0xFFFF` heen rolt, of door te
controleren dat `HI` klopt met het cumulatieve totaal op het display van de omvormer.

### `IMPEDANCE` en `NA_*`

`IMPEDANCE` (word 8, alleen in de 1-string-layout) wordt door de referentie wel gelogd maar
zonder eenheid of schaalfactor; vermoedelijk de isolatieweerstand in mΩ of Ω. `NA_0` en `NA_2`
zijn geheel onbekend.

Voor deze velden geldt de regel uit de opdracht: **onbekende waarden worden niet als nul
gepubliceerd.** Ze worden in Fase 3 wel op TRACE-niveau gelogd zodat de betekenis eventueel
alsnog kan worden vastgesteld, maar ze verschijnen niet in het canonieke meetmodel zolang
eenheid en schaal onbekend zijn.

## Statuscodes (`OP_MODE`) — twee codes gemeten, rest onbekend

De referentie leest `OP_MODE` uit en publiceert het als ruw getal, zonder waardetabel.
Er wordt hier geen tabel verzonnen — maar twee codes zijn inmiddels **gemeten** op een echte
TL3000-20 (dag/nacht-cyclus via HA-history, 2026-07-19 t/m 2026-07-22):

| Code | Betekenis (gemeten) | Bewijs |
|---|---|---|
| 1 | Grid-connected (normal) | Volledige productiedagen code 1; onafhankelijk bevestigd door de gekalibreerde Zeversolar 2000s-capture (ha-zeversolar-modbus) |
| 0 | Standby (not feeding) | Vier onafhankelijke events: schemering 19/20/21 juli (laatste minuten vóór afschakeling code 0) en dageraad 22 juli 06:01 (vier minuten code 0, daarna code 1 bij eerste productie) |

Mapping in `eversolar_parser.cpp::opModeText()`; elke andere code blijft eerlijk
`"Unknown (<n>)"` tot hij daadwerkelijk is waargenomen (kandidaat: een foutcode bij een
netstoring).

Aanpak:
- De MVP publiceert `status_code` als ruwe uint16 en zet `status_text` op `"Unknown (<n>)"`.
- In Fase 3 wordt de waarde over een volledige dag gelogd (nacht → opstart → productie →
  afschakeling). Uit die reeks volgen de daadwerkelijk voorkomende codes.
- Pas daarna wordt een mapping toegevoegd, met de meting als bron.

Hetzelfde geldt voor `error_code`: **het protocol kent geen apart foutcodeveld** dat in de
referentie wordt uitgelezen. `DeviceState::errorCode` blijft daarom op 0 met
`supported = false`; er wordt geen fictief veld gevuld.

## Response-validatie

De referentie valideert mager (`send_request()`, `:456-467`): alleen checksum + control/function
code. Bron- en bestemmingsadres worden niet gecontroleerd, en `$len = length($response) - 11`
gaat ervan uit dat er precies één frame in de buffer zit.

Onze parser valideert strenger:

| Controle | Regel |
|---|---|
| Minimale lengte | `>= 11` bytes |
| Maximale lengte | `11 + 255`; buffer begrensd op 300 bytes |
| Header | `0xAA 0x55` |
| Bestemming | moet `0x01 0x00` zijn (= wij) |
| Bron | geadresseerde query's: `0x00 <toegewezen adres>`. **Registratie-broadcasts: óók `0x00 0x00`** — zie hieronder |
| Datalengte | `N` moet kloppen met de werkelijke framelengte |
| Checksum | som over `[0 .. 8+N]`, én `> 0` |
| Control/function | moet de verwachte response-code zijn |
| Payloadlengte | per functie: 28/32/**44** voor `11 82` (zie layout-sectie) |

**Hardware-correctie (2026-07-19):** een ongeregistreerde omvormer beantwoordt de offline query
vanaf bron `00 00` — hij hééft nog geen adres; dat is precies wat de query komt regelen. De
oorspronkelijke aanname (antwoord vanaf het nog-toe-te-wijzen adres) was een geconstrueerde
gok, en de referentie kon het niet vertellen omdat die bronnen überhaupt niet controleert.
De registratiestappen accepteren daarom beide bronnen; de ACK-bron (`10 81`) is nog niet op
hardware waargenomen (dit toestel registreerde in één keer via `00 00`-pad). Geadresseerde
query's (`11 02`, `11 03`) blijven strikt op het toegewezen adres — op hardware bevestigd:
de normal-info-respons komt netjes van `00 10`.

Faalt één controle, dan wordt het frame verworpen en telt `invalid_frame_total` /
`checksum_error_total` op. Er wordt **geen** gedeeltelijke of gecorrigeerde data gepubliceerd.

## Timing

De referentie doet `sleep 1` na elk verzoek en leest dan blind 256 bytes (`:430-446`). Dat is
grof maar bewijst dat de omvormer ruim binnen een seconde antwoordt.

Onze transportlaag doet het netjes: lees tot het frame compleet is (lengte volgt uit byte 8),
met een responstimeout van **1000 ms** en **3** retries. Deze waarden staan in het
`SerialProfile` van de driver en zijn configureerbaar.

## Wat we nog niet weten

| Onbekend | Impact | Aanpak |
|---|---|---|
| Stuurt 2-string-firmware echt 32 bytes? | Autodetectie zou kunnen misgokken | Niet valideerbaar op deze hardware; `LayoutSelection` als ontsnappingsluik |
| Betekenis `OP_MODE`-codes | `status_text` blijft "Unknown (n)" | Loggen over een etmaal, Fase 3 |
| Betekenis `IMPEDANCE`, `NA_0`, `NA_2` | Niet gepubliceerd | TRACE-log, Fase 3 |
| Format `QUERY_DESCRIPTION` (`11 00`) | Model-info mogelijk hieruit | Uitproberen in Fase 3 (read-only, veilig) |
| Format `QUERY_INVERTER_ID` (`11 03`) | ASCII, structuur onbekend | Ruw loggen, dan pas parsen |
| Bevestiging uint32 `energy.total` | 0,1 kWh-precisie | Vergelijken met display |
| Gedrag 's nachts | Timeouts vs. foutframes | Fase 9 |

Voor geen van deze punten wordt in Fase 2 code geschreven die een aanname hardcodeert.
