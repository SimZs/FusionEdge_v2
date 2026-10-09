# Állomáslogók készítése – TuneIn map builder

Ez a Windowsos segédprogram a saját `playlist.csv` alapján keres állomáslogókat, majd a kiválasztott képekből a rádió számára használható PNG-ket és `map.csv` fájlt készít. A rádió lejátszás közben nem keres logót a TuneIn-en: az elkészült fájlokat LittleFS-re kell feltölteni.

A FusionEdge V2.0 nyilvános kiadása nem tartalmaz előre letöltött állomáslogókat. Ha saját logókat szeretnél látni, ezt a munkamenetet **az első LittleFS-feltöltés előtt** végezd el. A négy `plmode*.png` fallback ikon maradjon a `data/images/stations` mappában.

## Indítás

Windows alatt indítsd el a `tools/tunein_map_builder/run_windows.bat` fájlt. Python 3 szükséges. Első alkalommal a program a képfeldolgozáshoz szükséges csomagokat a felhasználói profilodba telepíti, ezért internetkapcsolat kell. Később a telepítés nem ismétlődik meg.

## A szokásos munkamenet

1. Nyisd meg a rádió saját `playlist.csv` fájlját. A program a TAB-, pontosvessző- és vesszőelválasztást is felismeri.
2. Kattints a **Find station logos on TuneIn** gombra. A találatokat ellenőrizd: hasonló nevű rádiók könnyen összekeverhetők.
3. Hibás találatnál válassz másik TuneIn-logót, adj meg kézzel egy kép-URL-t, vagy használd a **Clear logo** gombot. JPG-forrás is megadható: a program valódi PNG-vé alakítja. Ha megszakítanád a munkát, **Save work…** / **Resume work…** segítségével később folytathatod.
4. Kattints a **Download logos to staging** gombra. Az alapértelmezett `Resize to 120×120` beállítás kisebb, átlátszó hátterű PNG-ket készít.
5. A **Choose data folder…** gombbal válaszd ki a firmware projekt `data` könyvtárát. A program kiszámolja a LittleFS helyigényét, beleértve a kötelező fallback ikonokat is.
6. Ha a méretkeret túl nagy, a lista **Use** jelölőivel hagyj ki néhány logót. Ezután válaszd az **Export selected logos + fallback icons** műveletet. Az export az `images/stations` mappába teszi a PNG-ket és a `map.csv` fájlt; a korábbi mappáról a program külön mentést készít.
7. Töltsd fel a projekt `data` tartalmát LittleFS-re a szokásos PlatformIO fájlrendszer-feltöltéssel. A firmware külön újrafordítása a logók cseréjéhez nem szükséges.

A `map.csv` második oszlopában szereplő internetes URL csak a számítógépes keresés/újraletöltés forrása. A rádió a harmadik oszlopban megadott **helyi PNG-fájlt** nyitja meg. Ha egy logó hiányzik, a megfelelő mód fallback ikonja jelenik meg.

A TuneIn találatai és kiszolgálói változhatnak; minden export előtt nézd át a képeket és a helyigényt. Az export méretellenőrzése nem helyettesíti a PlatformIO LittleFS-feltöltés eredményének ellenőrzését.
