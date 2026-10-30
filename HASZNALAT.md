# RSC NFC Sequencer – használati útmutató

Flipper Zero alkalmazás NFC-olvasók és beléptetőrendszerek **teszteléséhez**. A
program sorra veszi az elmentett `.nfc` kártyákat, és mindegyiket „lejátssza”
(emulálja) az olvasó felé – felügyelet nélkül, akár véletlenszerű időzítéssel.

> **Nyelv (0.8-tól):** a kezelőfelület angol, magyar vagy német lehet. Telepítés
> után angol; átállítani a **Settings** lista legelső sorában (**Language**) lehet –
> a választás azonnal érvényes, és megmarad. A képernyőn nincsenek ékezetek (a
> Flipper betűkészletében nincsenek ilyen betűk).
>
> Ez az útmutató a funkciókat az angol nevükkel említi; a magyar felületen így
> hívják őket:
>
> | Angol | Magyar | | Angol | Magyar |
> |---|---|---|---|---|
> | Start | Inditas | | Hold | Tartas |
> | Settings | Beallitasok | | Total | Osszesen |
> | Generate tags | Cimkek keszitese | | Source | Forras (App / Generalt / Tallozas) |
> | Delete generated | Generaltak torlese | | LED | LED |
> | About | Nevjegy | | Vibro | Rezges |
> | Language | Nyelv | | Beep | Hang |
> | Order (SEQ/ABC/RND) | Sorrend (SOR/ABC/VEL) | | Start delay | Kesleltetes |
> | Timing (Fixed/Burst) | Idozites (Fix/Sorozat) | | Data (Off/Name/Text/URL) | Adat (Ki/Nev/Szoveg/URL) |
> | Delay | Idokoz | | Off / On | Ki / Be |
> | Codes/period, Period | Kod/periodus, Periodus | | Min gap | Min. szunet |

## 1. Telepítés

A kész alkalmazás a `dist/rsc_nfc_sequencer.fap` fájl.

- **Egyszerű másolással:** másold a `.fap`-ot a Flipper SD-kártyáján az
  `apps/NFC/` mappába (pl. a qFlipper fájlkezelőjével).
- **Fejlesztői módon:** a projekt mappájában `ufbt launch` – lefordítja és a
  csatlakoztatott Flipperre tölti.

A `.fap` a firmware API-verziójához kötött: ha frissíted a firmware-t, előfordulhat,
hogy újra kell fordítani. (Fordítás: lásd a `README.md`-t.)

## 2. A kártyák (tagek) előkészítése

1. A Flipper beépített **NFC** alkalmazásával olvasd be a kártyákat, és mentsd el
   (**Save**). A mentett fájlok a `/ext/nfc/` mappába kerülnek.
2. Amit sorban le akarsz játszani, azt másold az alkalmazás saját mappájába:
   ```
   /ext/apps_data/rsc_nfc_sequencer/tags/
   ```
   (a mappát az app az első indításkor létrehozza). Legfeljebb 128 `.nfc` fájl.

Nem kötelező saját kártya: a program **generálni is tud** véletlen UID-ú
teszt­kártyákat (lásd lentebb).

## 3. A menü

Az app egy menüvel indul, a tetején a program nevével:

- **Start** – a sugárzás indítása.
- **Settings** – beállítások (lentebb részletesen).
- **Generate tags** – véletlen teszt­kártyák létrehozása.
- **Delete generated** – a generált kártyák törlése (rákérdez).
- **About** – rövid névjegy.

Gombok: fel/le a menüben lépkedés, **OK** választ, **Back** kilép.

## 4. Sugárzás (Start)

A sugárzó képernyőn látszik a **legutóbb kiküldött kártya neve nagy betűvel**,
alatta az **UID-ja**, fent a mód/időzítés és a számláló. A kiírás a következő
kódig kint marad, így kényelmesen leolvasható.

- **OK** vagy **Back** – leállítás, vissza a menübe.
- Ha egy kártya nem tölthető be, `!! load failed` jelenik meg, és a LED pirosra vált.

Minden kódot a program csak a beállított **Hold** ideig sugároz (a mező ekkor
„aktív”), utána a mezőt **lekapcsolja** – így az olvasó egy koppintásként, egyszer
olvassa be, nem pedig folyamatosan.

## 5. Beállítások (Settings)

A listában **balra/jobbra** állítod az értéket, **Back** menti és visszalép. A
beállítások újraindítás után is megmaradnak. A Timing szerint csak a lényeges sorok
látszanak.

- **Language** – a kezelőfelület nyelve: English (alapból), Magyar, Deutsch.
- **Order** – milyen sorrendben jönnek a kártyák:
  - **SEQ** – mappa szerinti sorrend
  - **ABC** – név szerinti (ábécé)
  - **RND** – minden lépésnél véletlen
- **Timing** – időzítés:
  - **Fixed** – egy kód minden **Delay** másodpercben.
  - **Burst** – periódusonként adott számú kód, véletlen pillanatokban.
- **Delay** (csak Fixed) – két kód közti idő, 1–60 mp.
- **Codes/period** (csak Burst) – hány kód menjen ki egy periódusban.
- **Period** (csak Burst) – a periódus hossza.
- **Min gap** (csak Burst) – két kód indítása közti legkisebb szünet (hogy ne
  essenek túl közel egymáshoz).
- **Hold** – meddig szól egy kód (0,2–3 mp). **Ezt kell belőni az olvasódhoz:**
  feljebb = biztosabb beolvasás; lejjebb = kevésbé olvassa újra ugyanazt.
- **Total** – ennyi kód után megáll („Off” = végtelen).
- **Source** – honnan jönnek a kártyák:
  - **App** – az app saját `tags/` mappája (a saját/minta kártyák)
  - **Generated** – a generált kártyák (`tags/generated/`)
  - **Browse** – nyomj **OK**-t, és a fájltallózóban válassz egy fájlt; annak a
    mappája lesz a forrás (ilyenkor a mappa neve jelenik meg).
- **LED** – a jelző-LED színe (Off, Red, Green, Blue, Yellow, Cyan, Magenta,
  White). A LED addig világít, **ameddig a kód éppen szól**.
- **Vibro** – rövid rezgés minden kódnál (Off/On).
- **Beep** – rövid sípolás minden kódnál (Off/On; hibánál mélyebb hang).
- **Start delay** – Off, vagy 1–10 mp: a **Start** megnyomása után visszaszámlálás
  indul, mielőtt a sugárzás elkezdődik, hogy legyen időd az olvasóhoz vinni a
  Flippert. A visszaszámlálás alatt a **Back** megszakítja.

## 6. Teszt­kártyák generálása

**Menü → Generate tags.**

- **↑ / ↓** – a darabszám (koppintás +1, nyomva tartva +5, hosszabban +10,
  legfeljebb 100).
- **← / →** – a **Data** (adat): mi kerüljön a címkébe (lásd lentebb).
- **OK** – elkészíti (a *Text* / *URL* módnál előbb a képernyős billentyűzet
  nyílik meg, ott írod be a szöveget/címet).

A program `GEN-1.nfc`, `GEN-2.nfc`, … néven hozza létre őket a `tags/generated/`
mappába (a számozást folytatja, semmit nem ír felül), a végén kiírja, hányat
készített, és a forrást **Generated**-re állítja.

### Data – mit lát az olvasó

- **Off** – sima, **csak UID** címke (véletlen 4 bájtos ISO14443-3A). Az olvasó
  csak a UID-t látja. Ez a klasszikus beléptető­teszt-címke.
- **Name** – a címke a UID mellett a **saját nevét** is viszi NDEF
  szövegrekordként (`GEN-1`, `GEN-2`, …).
- **Text** – beírt szöveg NDEF szövegrekordként.
- **URL** – beírt webcím NDEF URI-rekordként (a telefon felajánlja a megnyitását).

A **Name / Text / URL** módban a generált címke **Mifare Classic 1K**, az NDEF
adattal a szektoraiban (MAD + szabványos NDEF-kulcsok). Azért Mifare Classic: a
Flipper ezt megbízhatóan emulálja a telefon felé, míg az NTAG/Ultralight
emulációnál a telefon gyakran csak a UID-t kapja meg. Egy NDEF-olvasó app (NFC
Tools, TagInfo) megjeleníti a rekordot; a telefon **alapértelmezett** kezelője
viszont általában csak a **URL**-re (URI-rekord) ugrik fel magától, a csupasz
szövegrekordra nem – ha azt akarod, hogy a telefon magától reagáljon, válaszd a
**URL** módot. A **UID-alapú beléptető olvasó mindenképp csak a UID-t olvassa**,
az NDEF-adatot nem. Az adat csak az **újonnan generált** címkékbe kerül, a saját,
bemásolt kártyáidba nem.

**Delete generated** – rákérdezés után törli a generált kártyákat.

A generálás és a törlés **mindig csak az app saját mappáját** érinti, soha nem a
Browse-zal kiválasztott saját mappádat.

## 7. Tippek

- **„Nem olvassa a kódot”?** Told feljebb a **Hold**-ot (pl. 1–2 mp), amíg az
  olvasó biztosan beolvassa.
- **„Többször olvassa ugyanazt”?** Told lejjebb a **Hold**-ot, vagy használj
  Burst módot nagyobb **Min gap**-pel.
- **A LED USB-n folyamatosan zölden világít?** Az a **töltésjelző**, nem az app;
  húzd le USB-ről, és akkor csak a beállított jelzés látszik.

## 8. Mire való, és mire nem

- **UID-alapú** olvasók tesztjére készült: a Flipper az elmentett ISO14443-A
  kártya UID-ját hűen emulálja.
- A teljes MIFARE Classic **tartalom** (szektorok, kulcsok) emulációja a Flipper
  hardverén olvasófüggő, és **nem** ennek az appnak a célja – ha ott nem sikerül a
  nyitás, az lehet a Flipper időzítése, nem az olvasód vagy ez a program.
