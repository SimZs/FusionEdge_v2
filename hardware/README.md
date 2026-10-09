# FSC-BT1058 Bluetooth-adapter

A FusionEdge V2.0 kétirányú Bluetooth-audiójához FSC-BT1058 modul kell. Ez a kis adapter-nyák a modul beültetését és a rádióhoz csatlakoztatását egyszerűsíti: egy 8 tűs JST-XH csatlakozóra vezeti ki a 3,3 V-os tápot, az UART-ot és az I²S hangjeleket. A szükséges `SYS_CTRL` bekapcsolóáramkör (2N7002) és a táp szűrőelemei is az adapteren vannak. A `RST` és `SYS_CTRL` külön tesztpadon is elérhető; az `MCLK` nem része a csatlakozónak.

A mappában az Eagle kapcsolási rajz (`.sch`), a NYÁK-terv (`.brd`), a gyártási Gerber- és fúrófájlokat tartalmazó ZIP, valamint az adapter képe található. A csatlakozó bekötését és a beültetendő alkatrészeket mindig a mellékelt kapcsolási rajz alapján ellenőrizd; a modult ne kösd közvetlenül 5 V-ra.

FSC-BT1058 modul: [Feasycom vásárlási oldal az Alibabán](https://www.alibaba.com/product-detail/Feasycom-FSC-BT1058-Qualcomm-QCC5181-Bluetooth_1601272259871.html?spm=a2756.trade-list-buyer.0.0.f34876e92KzOtz).
