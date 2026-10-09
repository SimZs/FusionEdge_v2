# FusionEdge a Home Assistantban

Az opcionális integráció a rádió MQTT-kapcsolatát használja. Hozzáadás után megjelenik egy FusionEdge kezelőoldal a Home Assistant oldalsávjában; nem kell külön dashboard-kártyát vagy JavaScript-erőforrást beállítani.

## Amire szükség van

- Működő MQTT-broker és a hozzá csatlakozó Home Assistant MQTT-integráció.
- Ugyanahhoz a brokerhez csatlakozó FusionEdge V2.0 rádió. A rádió projektgyökerében a `mqttoptions.example.h` alapján készített `mqttoptions.h` tartalmazza a broker adatait. Ennek módosítása után a rádió firmware-ét újra kell fordítani és feltölteni; LittleFS-t emiatt nem kell újratölteni.
- Hálózati kapcsolat a Home Assistant és a rádió között. A lejátszási lista megnyitásához a HA-nak a rádió helyi HTTP-címét is el kell érnie.

## Telepítés

1. Másold a teljes `HA/custom_components/fusionedge` mappát a Home Assistant konfigurációs könyvtárában lévő `custom_components/fusionedge` helyre. A `frontend` almappa is kell. Ha a `custom_components` könyvtár még nincs meg, hozd létre.
2. Indítsd újra **teljesen** a Home Assistantot. Nem kell `configuration.yaml` bejegyzés.
3. Indítsd újra a rádiót, hogy elküldje az MQTT-felfedezési üzenetét.
4. A Home Assistantban nyisd meg a **Beállítások → Eszközök és szolgáltatások** oldalt, és add hozzá a felismert FusionEdge eszközt.
5. Az oldalsávban megjelenő **FusionEdge** oldalon próbáld ki a lejátszást és a hangerőt. A hangerő a rádió 0–21 lépéséhez igazodik.

Az oldalsávos oldalon állomásváltás, lejátszási mód, Bluetooth-kimenet, Clock TTS, LED-szalag és az automatikus be-/kikapcsolás négy időpontja is kezelhető, ha az adott funkció a rádión engedélyezett. A Home Assistant külön entitásokat is létrehoz, így automatizálásban használhatók. Több rádió egy oldalsávos oldalon választható; mindegyiknek külön MQTT root topic kell.

Az időzítőben külön állítható a hétköznapi és hétvégi be-, illetve kikapcsolás. Az üres időpont kikapcsolja az adott eseményt; a HA `text` entitásoknál ugyanezt `off` jelenti. Az időpontok 24 órás `HH:MM` formátumúak. A beállításokat a rádió menti, ezért a Home Assistant kiesése után is érvényben maradnak. Az időzítő szerkesztéséhez a friss rádiófirmware és a frissített `custom_components/fusionedge` mappa együtt szükséges; a HA frissítése után teljes újraindítás kell, de a már hozzáadott rádiót nem kell törölni.

## Ha nem jelenik meg

- Ellenőrizd, hogy az MQTT-integráció működik-e, és a rádió ugyanarra a brokerre csatlakozik-e.
- Ellenőrizd, hogy a **teljes** `fusionedge` mappát másoltad-e át, majd indítsd újra a Home Assistantot.
- Ha az entitások látszanak, de az oldalsávos oldal nem frissült, frissítsd a böngészőlapot is.
- A Bluetooth-eszközök keresését és párosítását a rádió WebUI-jában végezd; ez nem része a HA-integrációnak.
- A lejátszási lista betöltéséhez a Home Assistantnak el kell érnie a rádió helyi HTTP-címét. HTTP Basic Auth-tal védett WebUI esetén a lista ezen a felületen jelenleg nem tölthető be.

Frissítéskor a `fusionedge` mappát másold át újra és indítsd újra a Home Assistantot. A már hozzáadott rádiót nem kell törölni. A broker-jelszót és a saját `mqttoptions.h` fájlt ne oszd meg.
