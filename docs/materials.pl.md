# Materiały

*[English version](materials.md)*

Materiał w fumarze jest fizycznie oparty (PBR), w modelu metallic-roughness, którego używa glTF. Ma pięć opcjonalnych map:

| Mapa | Przestrzeń kolorów | Co zawiera |
|---|---|---|
| kolor | sRGB | kolor powierzchni (i alfa, dla wycięć) |
| metal-rough | liniowa | roughness w **G**, metaliczność w **B** (układ glTF) |
| normal | liniowa | normalne w przestrzeni stycznej, konwencja OpenGL (+Y w górę) |
| occlusion | liniowa | przyciemnienie zagłębień w **R** |
| emisja | sRGB | światło, które powierzchnia emituje |

Każda mapa jest mnożona przez współczynnik: kolor, metallic, roughness, siła normal mapy, siła occlusion, kolor emisji × natężenie. Brakująca mapa liczy się jako biała (dla normal mapy jako płaska normalna), więc decyduje sam współczynnik.

Do tego dwa ustawienia:
- **Alfa:** nieprzezroczysty albo **cutout**, czyli wycinanie pikseli poniżej progu.
- **Tiling:** ile razy mapy powtarzają się na siatce.

Materiały pochodzą z trzech źródeł.

## 1. Biblioteka stockowa

Dziesięć zestawów tekstur 2K z [ambientCG](https://ambientcg.com), wszystkie CC0 (domena publiczna): cegła, deski, trawa, beton, metal, tynk, kafle, kamień, asfalt, tkanina. Pochodzenie każdego jest w `assets/materials/SOURCES.md`.

Zajmują ok. 110 MB, więc leżą na osobnym branchu **`materials`**, a nie na `main`. Pobranie:

```sh
tools/get-materials.sh        # Linux, Git Bash
tools\get-materials.ps1       # Windows PowerShell
```

Skrypt pobiera branch i rozpakowuje `assets/materials` do twojego checkoutu. Nie przełącza brancha i nie rusza indeksu, bo `assets/` jest ignorowany na `main`. Uruchom go ponownie, żeby zaktualizować.

**Bez biblioteki:**
- scena startowa wraca do zwykłych tekstur koloru z `assets/textures`;
- panel Materials podpowiada, gdzie leży biblioteka.

**Buildy release** (zipy z CI) też nie zawierają biblioteki.

**Klonowanie:** zwykły `git clone` pobiera wszystkie branche, razem z biblioteką. Żeby wziąć sam `main`:

```sh
git clone --single-branch https://github.com/c-airr/Fumar.git
```

## 2. Foldery z teksturami

Upuść na edytor folder z mapami, a zrobi się z niego materiał. Mapy są rozpoznawane po końcówce nazwy pliku. Wielkość liter nie ma znaczenia. Obsługiwane formaty to `.jpg`, `.jpeg`, `.png` i `.tga`.

| Mapa | Końcówki, w kolejności pierwszeństwa |
|---|---|
| kolor | `_Color`, `_BaseColor`, `_Albedo`, `_diff`, `_Diffuse` |
| normal (OpenGL) | `_NormalGL`, `_nor_gl`, `_Normal` |
| normal (DirectX, odwracana przy wczytaniu) | `_NormalDX`, `_nor_dx` |
| roughness | `_Roughness`, `_rough` |
| metaliczność | `_Metalness`, `_Metallic`, `_metal` |
| occlusion | `_AmbientOcclusion`, `_AO` |
| emisja | `_Emission`, `_Emissive` |

Końcówka liczy się tylko wtedy, gdy kończy nazwę albo stoi po niej `_`, `-` lub `.`. Dzięki temu `_Normal` nie łapie `_NormalGL`, a `brick_wall_diff_2k.jpg` z Poly Haven pasuje do `_diff`. Reszta zawartości folderu jest pomijana: displacement, podglądy, pliki `.blend`. Folder bez mapy koloru i bez normal mapy nie daje materiału.

Co robi import:
- Osobne obrazy roughness i metaliczności są łączone w jedną teksturę w układzie glTF.
- Wynik trafia do **`<folder>/<nazwa folderu>.fmat`** i jest z niego wczytywany.
- Ponownie upuszczony ten sam folder wczytuje ten plik. Import nigdy go nie nadpisuje, więc twoje edycje nie giną.
- Upuszczony przy zaznaczonym obiekcie materiał od razu trafia na ten obiekt.

## 3. Pliki glTF

Modele z Blendera (i skądkolwiek) przynoszą swoje materiały:
- wszystkie pięć map;
- `KHR_materials_emissive_strength`;
- tryb alfa. `BLEND` jest rysowany jako nieprzezroczysty, bo prawdziwej przezroczystości jeszcze nie ma.

Te materiały należą do swojego pliku i w edytorze są **tylko do odczytu**. **Duplicate as .fmat** w panelu Materials robi edytowalną kopię i zapisuje osadzone w pliku mapy obok niej jako PNG.

## Edycja: panel Materials

Panel Materials jest w **Modelerze**, tam gdzie w Level jest Content. Ma dwie połowy.

**Lewa, biblioteka:**
- materiały sceny, a pod nimi jeszcze niewczytane pliki `.fmat` ze stocka i od użytkownika;
- pole filtra;
- **New**, czyli nowy `materials/New Material.fmat` obok edytora;
- klik w materiał nakłada go na zaznaczony obiekt.

**Prawa, edytor** pokazuje materiał zaznaczonego obiektu albo ostatnio klikniętego:
- **Pięć slotów na mapy:** `...` otwiera systemowe okno wyboru pliku, `×` czyści slot. Slot normal mapy ma przełącznik DirectX.
- **Każdy współczynnik** jako suwak albo próbnik koloru. Zmiany widać od razu w viewporcie.
- **Undo:** każdą zmianę cofa Ctrl+Z, jeden krok na całe przeciągnięcie.
- **Zapis:** Ctrl+S zapisuje każdy edytowany `.fmat`, a potem scenę. „Saved” pojawia się tylko wtedy, gdy wszystko się zapisało.

W buildzie deweloperskim biblioteka stockowa jest czytana z `assets/materials` w drzewie źródeł. Edycja materiału stockowego zmienia więc twoją lokalną kopię, której żaden build nie nadpisze.

## Format `.fmat`

JSON. Ścieżki są względne wobec pliku `.fmat`, więc folder z materiałami można przenieść w całości.

```json
{
  "fumarMaterial": 1,
  "name": "Bricks",
  "baseColor": { "texture": "Bricks_Color.jpg", "factor": [1, 1, 1, 1] },
  "metallicRoughness": {
    "packed": null,
    "roughness": "Bricks_Roughness.jpg",
    "metallic": null,
    "roughnessFactor": 1.0,
    "metallicFactor": 0.0
  },
  "normal": { "texture": "Bricks_NormalGL.jpg", "scale": 1.0, "convention": "gl" },
  "occlusion": { "texture": "Bricks_AmbientOcclusion.jpg", "strength": 1.0 },
  "emissive": { "texture": null, "color": [0, 0, 0], "intensity": 1.0 },
  "alpha": { "mode": "opaque", "cutoff": 0.5 },
  "uvScale": [1, 1]
}
```

- **Metal-rough:** albo jeden obraz `packed` w układzie glTF, albo osobne `roughness` i/lub `metallic`.
- **Brakujące pola** przyjmują wartości domyślne.
- **Odrzucane pliki:** plik z inną wersją `fumarMaterial` albo z polem złego typu jest odrzucany z ostrzeżeniem.
- **Brakująca tekstura:** tekstura, której nie da się wczytać, zostawia slot neutralny, z ostrzeżeniem. Ścieżka zostaje, więc po przywróceniu pliku materiał znowu działa.

**W scenach** materiał z `.fmat` jest zapisywany jako odwołanie, `{"name": ..., "fmat": <ścieżka>}`, względem katalogu exe, jak każda inna ścieżka w pliku sceny. Scena, której `.fmat` zniknął, i tak się wczyta, a obiekty, które go używały, dostaną materiał domyślny.

## Ograniczenia

- **Wycięcia w ray tracingu:** materiał z wycięciem rzuca pełny cień i blokuje odbicia oraz odbite światło tak, jakby nie miał dziur. Promienie traktują całą geometrię jako nieprzezroczystą.
- **Brak przezroczystości:** alpha blending jest na razie rysowany jako nieprzezroczysty.
- **Brak displacement i parallax mappingu.**
- **Limity rozmiaru:** mapy większe niż limit tekstur GPU są pomijane z ostrzeżeniem. Scena sięga najwyżej 1024 tekstur; kolejne czytane są jako białe, z ostrzeżeniem.
