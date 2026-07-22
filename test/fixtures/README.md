# Test fixtures

## Wat deze fixtures wél en niet bewijzen

`eversolar_frames.h` wordt gegenereerd door `tools/gen_fixtures.py`. De frames erin zijn
**geconstrueerd** uit het protocol zoals gereverse-engineerd uit eversolar-monitor
(zie `docs/eversolar-protocol.md`).

Dat betekent:

| Wel bewezen | Niet bewezen |
|---|---|
| De parser implementeert **onze interpretatie** van het protocol correct | Dat onze interpretatie **klopt** |
| Schaalfactoren, bytevolgorde en signedness zijn consistent toegepast | Dat de omvormer die schaalfactoren echt gebruikt |
| Beschadigde frames worden verworpen | Dat echte busfouten er zo uitzien |
| De layout-autodetectie werkt op 28/32 bytes | Welke layout de TL3000-20 stuurt |

Een groene testsuite hier is dus **geen** bewijs dat de firmware de omvormer correct uitleest.
Dat bewijs komt pas in Fase 3, tegen de echte TL3000-20, door vergelijking met
eversolar-monitor.

Zodra Fase 3 draait worden **opgenomen** frames hier toegevoegd, duidelijk gescheiden van de
geconstrueerde. Pas dan verschuift het bewijs van "consistent met onze aanname" naar
"consistent met de hardware".

## Regenereren

```bash
python3 tools/gen_fixtures.py
```

Handmatig bewerken heeft geen zin: het bestand wordt overschreven. Pas het script aan.

## Inhoud

| Fixture | Doel |
|---|---|
| `kReqQueryNormalInfo` | Referentie voor `buildRequest`, met de hand geverifieerd: `AA 55 01 00 00 10 11 02 00 01 23` (som = 0x123) |
| `kReqOfflineQuery`, `kReqReRegister`, `kReqSendAddress` | Registratieprocedure |
| `kRespNormalInfoSingle` | 28-byte payload, 1 string |
| `kRespNormalInfoDual` | 32-byte payload, 2 strings, zelfde fysieke waarden |
| `kRespNormalInfoNight` | Negatieve temperatuur (`0xFFFF` = -0,1 °C) en echte nullen |
| `kRespOfflineQuery`, `kRespRegisterAck`, `kRespRegisterNak`, `kRespInverterId` | Registratie-responses |
| `kRespBadChecksum` | Laatste checksumbyte geflipt |
| `kRespWrongSource` | Geldige checksum, andere inverter |
| `kRespWrongDestination` | Geldige checksum, niet aan ons geadresseerd |
| `kRespBadPayloadLength` | 30 bytes: geen van beide layouts |
| `kRespBadHeader` | Header is niet `AA 55` |
| `kAllZeroFrame` | 11 nullen — checksum "klopt" (0 == 0) |
| `kRespPartial` | Eerste 12 bytes van een geldig frame |

## Over `kRespNormalInfoNight`

Deze fixture legt het onderscheid vast dat de hele opdracht doortrekt: `PAC = 0` en
`E_TODAY = 0` zijn **echte metingen** ('s nachts wordt er niets geproduceerd), terwijl
`E_TOTAL` gewoon doorloopt. Onbekend-zijn wordt uitgedrukt in de validity-vlaggen, nooit in de
waarde. Een parser die hier `0` als "geen data" behandelt is fout.

## Over `kAllZeroFrame`

Bewaard, ook al is de bijbehorende `csum > 0`-guard uit de referentie **niet** overgenomen.
Reden: wij valideren de `AA 55`-header, waardoor de bytesom altijd ≥ 255 is en een all-zero
frame al eerder sneuvelt (`BadHeader`). De guard zou bovendien het ~1-op-65536 legitieme frame
verwerpen waarvan de som precies op 65536 uitkomt en naar 0 truncateert — dat geval staat als
aparte test in `test_eversolar_checksum`.
