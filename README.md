# FusionEdge V2.0

ESP32-S3 alapú internetes rádió, helyi és hálózati lejátszással, valamint kétirányú Bluetooth-audióval. Aktuális kiadás: **v2.0.2**.

Ez a FusionEdge továbbfejlesztett, PlatformIO/Arduino-alapú firmware-e.

## Mi változott a 2.0.2-ben?

- Első indításkor a firmware a mentett lejátszási módhoz igazítja az FSC-BT1058 szerepét. A gyári Sink állapotú modul WEB/SD/DLNA módban magától Source módra vált; a WebUI keresőgombja a beállítás és ellenőrzés után kézi oda-vissza módváltás nélkül aktiválódik.
- Bluetooth Input módból indulva az audióhíd megvárja a modul szerepének és I2S-beállításainak ellenőrzését.

## Mi változott a 2.0.1-ben?

- Bluetooth-kimenetnél a párosított hangszórók között közvetlenül lehet váltani. A rádió előbb bontja a meglévő kapcsolatot, majd az újhoz csatlakozik; sikertelen váltás után megpróbálja visszaállítani az előzőt.
- Kijelző- és érintésigazítások az időjárás és a Bluetooth-eszközlista körül.
- Javítottuk a DLNA-worker fordítási figyelmeztetését.
- A [hardware mappába](hardware/README.md) bekerült az FSC-BT1058 adapter kapcsolási rajza, NYÁK-terve és gyártási csomagja.

## Mi új a 2.0.0-ban?

- Az FSC-BT1058 modul Bluetooth-bemenetként és -kimenetként is használható, a két szerep között váltva. Bemenetként telefonról vagy PC-ről fogad hangot; kimenetként a rádió hangja párosított Bluetooth-hangszóróra küldhető. Az elérhető kodek a kapcsolódó eszköztől és a modul firmware-étől függ.
- Átalakult a 480×320-as lejátszóképernyő: nagyobb állomáslogó/borító, külön előző–lejátszás/szünet–következő gombok, szöveges audióinformáció, külön mód- és csatornaszám-mező, valamint új információs nézetek.
- A beállítások, a képernyővédő-választás és több korábbi főképernyős kapcsoló egy közös kijelzőmenübe került. Érintőképernyő nélkül, enkóderrel is kezelhető.
- A WebUI önálló Bluetooth-részt kapott. A Last.fm borítókeresés engedélyezése és API-kulcsa, valamint a lejátszás közbeni képernyővédő stílusa már a WebUI-ban állítható.
- A mellékelt logótérkép-készítő a rádiólistából állomáslogókat keres, ellenőriz, PNG-re alakít és a LittleFS méretkeretéhez igazítva exportál.
- Opcionális, MQTT-alapú Home Assistant-integráció és saját FusionEdge oldalsávos kezelőoldal készült.

## Érintőképernyő – a 480×320-as elrendezés

| Érintett terület | Művelet |
| --- | --- |
| Metaadat és első címsor | Állomás-presetek megnyitása; a presetnézet fejlécével lehet visszalépni. |
| Bal felső állomáslogó / borító | A nagy coverart-nézet megnyitása; újabb érintéssel vissza a lejátszóhoz. Az automatikus képernyővédő beállítása nem változik. |
| Előző / lejátszás–szünet / következő ikon | Lejátszás vezérlése; Bluetooth-bemenetnél a csatlakozott telefon/PC felé küld vezérlést. |
| Bal alsó Bluetooth-ikon | Kapcsolat és párosított eszközök megtekintése, kapcsolódás/bontás. |
| `MODE` mező | Lejátszási mód választása. |
| Középső menüikon | Kijelzőbeállítások megnyitása. |
| `Ch.:` mező | Aktuális lejátszási lista megnyitása. |
| Jobb alsó Wi-Fi-ikon | SSID, IP-cím és jelerősség. |
| Időjárásikon | Ötnapos előrejelzés. |
| Jobb oldali függőleges hangerőskála | Hangerő állítása érintéssel. |

A korábbi, általános fel/le húzás nem vált módot vagy állomást: a listákban görgetésre szolgál. A kijelzőbeállítások között `MODE`, `FADE`, `TTS`, `RGB`, `VOLUME` és `SCREENSAVER` található. A menü 5 másodperc tétlenség után záródik; egy beállítás megváltoztatása önmagában nem léptet ki.

## Enkóder

A fő lejátszóképernyőn a forgatás hangerőt állít, az egyszeri gombnyomás lejátszás/szünet, a dupla kattintás a kijelzőbeállításokat, a hosszú nyomás a lejátszási listát nyitja meg. Nyitott listában vagy választómenüben a forgatás kijelöl, az egyszeri nyomás megerősít. A dupla és hosszú nyomás nem írja felül a már nyitott Settings, Volume, Wi-Fi, Bluetooth vagy Forecast nézetet. A Wi-Fi-adatok a Settings alsó sorában érintőképernyő nélkül is elérhetők.

## WebUI

- **Bluetooth:** Input oldalon név (alias) és felfedezhetőség; Output oldalon `Bluetooth only`, `Local + Bluetooth` és `Local only`. A csatlakoztatott/párosított eszközök és az új hangszórók keresése külön kezelhető.
- **Extras → Last.fm cover art:** a funkció engedélyezése és a saját, 32 karakteres API-kulcs megadása. Érvényes kulcs nélkül nincs Last.fm-lekérés; a borítókeresés MusicBrainz-forrást is használhat. Az API-kulcsot ne tedd nyilvános repóba.
- **Screens → Screensaver:** a szokásos képernyővédő mellett a lejátszás közbeni működés külön kapcsolható. Stílusok: `CoverArt`, `Cassette`, `Analog VU Meter`.

A helyi állomáslogókhoz lásd a [TuneIn map builder útmutatóját](tools/tunein_map_builder/README.md). A logók letöltése a számítógépen történik; a rádió a LittleFS-re feltöltött PNG-ket használja, nem tölti le a `map.csv` távoli URL-jeit lejátszás közben.

Az állomások egyedi logói nem részei ennek a kiadásnak. **Az első LittleFS-feltöltés előtt a saját vagy a mellékelt `playlist.csv` fájlból készítsd el a kívánt logókat és a hozzájuk tartozó `map.csv` fájlt** a mellékelt segédprogrammal. A `plmodeweb.png`, `plmodesd.png`, `plmodedlna.png` és `plmodebt.png` fallback ikonokat ne töröld: ezek logó nélkül is szükségesek. Ha nem készítesz állomáslogókat, a rádió ezekkel a fallback ikonokkal működik.

## Home Assistant

Az opcionális [Home Assistant-integráció](HA/README.md) MQTT-n keresztül automatikusan felismerhető rádiót, média-lejátszó entitást és FusionEdge oldalsávos kezelőoldalt ad. A Bluetooth-eszközök keresése és párosítása továbbra is a rádió WebUI-jában történik.

## Bluetooth-adapter

Az FSC-BT1058 modulhoz tartozó adapter kapcsolási rajza, NYÁK-terve és gyártási fájljai a [hardware mappában](hardware/README.md) találhatók. Az ottani rövid útmutató a bekötés célját és a modul beszerzési linkjét is tartalmazza.

## Telepítési megjegyzések

A jelenlegi PlatformIO-mintakörnyezet az ESP32-S3 és a 480×320-as ST7796 kijelző használatához készült. Más panelhez a megfelelő, ellenőrzött hardverkonfiguráció szükséges. Feltöltés előtt ellenőrizd, hogy a `myoptions.h` beállításai a saját hardveredhez illenek. A firmware mellett a projekt `data` tartalmát is fel kell tölteni LittleFS-re, különben a WebUI és a képek hiányozhatnak.

Az első feltöltés előtt másold a `data/data/wifi.example.csv` fájlt `data/data/wifi.csv` néven, és írd bele a saját hálózataidat. MQTT/Home Assistant használatához másold a `mqttoptions.example.h` fájlt `mqttoptions.h` néven, majd állítsd be a saját brokeredet; MQTT nélkül ez a fájl nem szükséges. A `wifi.csv` és a `mqttoptions.h` személyes adatokat tartalmazhat, ezért ne töltsd fel őket GitHubra. Feltöltés vagy teljes törlés előtt készíts mentést róluk, valamint a saját beállításaidról és API-kulcsaidról.

A mellékelt `data/data/playlist.csv` használható kiinduló rádiólista, amelyet szabadon lecserélhetsz a sajátodra. Az `ircodes.csv` nem része a kiadásnak: a saját infratávirányító betanítása után a kódokat a rádióból tudod elmenteni.
