# Skrypty: Lua i bindingi C++

*[English version](scripting.md)*

fumar uruchamia logikę gry w dwóch językach, w jednym modelu:

- **Lua** (LuaJIT 2.1): pliki tekstowe w `scripts/lua/`, kompilowane w milisekundach przy działającym edytorze.
- **Komponenty C++**: klasy w `scripts/cpp/`, budowane do biblioteki współdzielonej `fumar_game` i podmieniane pod działającym edytorem.

Węzeł wskazuje skrypt Lua, komponent C++ albo oba naraz. Silnik w obu przypadkach wywołuje te same dwie funkcje, `on_start` i `on_update`, a oba języki mają dostęp do tego samego:

- węzła,
- sceny,
- klawiatury i myszy,
- kamery,
- raycastu.

W tym dokumencie:

1. [Szybki start](#1-szybki-start)
2. [Jak działa skrypt](#2-jak-działa-skrypt)
3. [API Lua](#3-api-lua)
4. [Konwencje: jednostki, przestrzenie, kąty](#4-konwencje-jednostki-przestrzenie-kąty)
5. [Błędy i pułapki](#5-błędy-i-pułapki)
6. [Jak działają bindingi (strona C++)](#6-jak-działają-bindingi-strona-c)
7. [Dodawanie bindingu](#7-dodawanie-bindingu)
8. [Komponenty C++](#8-komponenty-c)
9. [Lua i C++ obok siebie](#9-lua-i-c-obok-siebie)
10. [Build i układ plików](#10-build-i-układ-plików)

---

## 1. Szybki start

1. W trybie **Level** otwórz panel **Scripts** i wpisz nazwę w pole *new script name*. Nowy plik startuje z szablonu, który pokazuje obie funkcje.
   - Możesz też samemu wrzucić plik `.lua` do `scripts/lua/`.
2. Napisz skrypt:

   ```lua
   -- scripts/lua/spin.lua
   local degrees_per_second = 45.0

   function on_start(node)
       fumar.log("spin started on " .. node:name())
   end

   function on_update(node, dt)
       local pitch, yaw, roll = node:rotation()
       node:set_rotation(pitch, yaw + degrees_per_second * dt, roll)
   end
   ```

3. Naciśnij **Compile** (albo **F5**). Jeśli są błędy, pasek narzędzi pokaże ich liczbę.
4. Zaznacz węzeł i wybierz skrypt w **Details → Script**. Lista pokazuje tylko skrypty, które się skompilowały.
5. Naciśnij **Play**. Skrypty działają tylko w trakcie gry, więc skrypt nie walczy z tobą, kiedy ustawiasz obiekt ręcznie. **Stop** kończy grę.

Nazwa skryptu to nazwa pliku bez `.lua`. To ona trafia do pliku sceny (`"script": "spin"`), więc scenę da się wczytać i zapisać bez środowiska skryptów.

---

## 2. Jak działa skrypt

### Funkcje wywoływane przez silnik

| Funkcja | Kiedy | Argumenty |
|---|---|---|
| `on_start(node)` | Raz na węzeł, przy jego pierwszej aktualizacji po Play, Compile albo wczytaniu sceny / New Scene | węzeł |
| `on_update(node, dt)` | W każdej klatce w trakcie gry | węzeł, sekundy od poprzedniej klatki |

Obie są opcjonalne. Skrypt bez żadnej z nich kompiluje się i nic nie robi.

### Kolejność w klatce

1. Edytor wypełnia stan wejścia, kopiuje kamerę edytora do kontekstu skryptów i ustawia raycast.
2. **Przebieg Lua.** Węzły ze skryptem są odwiedzane w kolejności drzewa sceny, w głąb, czyli tak jak w Outlinerze. Węzeł, który jeszcze nie wystartował, dostaje najpierw `on_start`, a potem `on_update`.
3. **Przebieg C++.** To samo dla węzłów z komponentem, na tym samym kontekście.
4. Jeśli coś zapisało kamerę, edytor bierze widok z kontekstu i przechwytuje mysz.

Lista węzłów jest zbierana przed przebiegiem. Węzły utworzone w jego trakcie dostają aktualizację dopiero w następnej klatce.

### Osobne środowisko dla każdego skryptu

Każdy plik `.lua` jest kompilowany do własnej tabeli środowiska, więc dwa skrypty mogą definiować `on_update` bez konfliktu. Globalne nazwy, których skrypt sam nie definiuje (`math`, `string`, `print`, `fumar`, …), są brane z `_G` przez metatabelę.

**Jeden plik skryptu jest wspólny dla wszystkich węzłów, które go używają.** Zmienne `local` na górnym poziomie pliku, a także globalne zmienne przypisane w skrypcie, istnieją raz na *skrypt*, a nie raz na węzeł. Stan należący do węzła trzeba więc trzymać pod jego kluczem:

```lua
-- scripts/lua/bob.lua (skrócone)
local origins = {}

function on_start(node)
    local x, y, z = node:position()
    origins[node:id()] = { x = x, y = y, z = z }
end

function on_update(node, dt)
    local o = origins[node:id()]
    if not o then return end
    -- ...
end
```

Wspólną zmienną dla wielu skryptów trzeba jawnie zapisać w `_G` (`_G.score = 0`). Obowiązują zwykłe powody, żeby unikać globali.

### Co kasuje stan

**Compile / F5** kompiluje każdy plik `.lua` od nowa, do **świeżego** środowiska:

- każda zmienna z górnego poziomu startuje od zera,
- usunięty plik przestaje działać,
- przemianowana funkcja nie zostaje w pamięci,
- `on_start` odpala się ponownie dla każdego węzła.

**Play, wczytanie sceny i New Scene** tylko sprawiają, że `on_start` odpali się ponownie. Środowiska Lua i wszystko, co w nich zapisano, zostają.

Komponenty C++ różnią się w jednym miejscu: Play ich nie restartuje. Ich `onStart` odpala się ponownie dopiero po Compile, wczytaniu sceny albo New Scene. Instancja komponentu razem z polami przeżywa Stop i Play.

### Biblioteka standardowa

Otwarta jest cała biblioteka standardowa LuaJIT, w tym:

- `io` i `os`,
- `jit`, `ffi` i `bit`.

To świadoma decyzja na czas rozwoju: możliwość odczytania pliku czy wypisania czegoś jest teraz cenniejsza niż piaskownica. Wydana gra powinna wyłączyć `io`, `os` i `ffi`; patrz komentarz w `ScriptEngine::ScriptEngine`.

---

## 3. API Lua

### Uchwyty węzłów

Węzeł przychodzi jako pierwszy argument każdej funkcji wywoływanej przez silnik, albo z `fumar.find`, `node:child` lub `node:parent`. To userdata z **identyfikatorem** węzła, a nie wskaźnik. Każda metoda szuka węzła od nowa i zgłasza błąd, jeśli już go nie ma.

Metody wywołuje się przez `:`.

| Metoda | Zwraca | Uwagi |
|---|---|---|
| `node:id()` | liczba całkowita | Stała, dopóki węzeł istnieje. Używaj jej jako klucza tabeli ze stanem węzła. |
| `node:name()` | string | |
| `node:set_name(s)` | | |
| `node:position()` | `x, y, z` | Lokalna, względem rodzica. Metry. |
| `node:set_position(x, y, z)` | | |
| `node:rotation()` | `pitch, yaw, roll` | Stopnie wokół X, Y i Z. Patrz [§4](#4-konwencje-jednostki-przestrzenie-kąty). |
| `node:set_rotation(pitch, yaw, roll)` | | |
| `node:scale()` | `x, y, z` | |
| `node:set_scale(x, y, z)` | | |
| `node:visible()` | boolean | |
| `node:set_visible(b)` | | Ukrycie węzła ukrywa całe jego poddrzewo i wyłącza je z raycastów. |
| `node:child_count()` | liczba całkowita | |
| `node:child(i)` | węzeł albo `nil` | **Indeksowanie od 1**, jak wszędzie w Lua. `nil` poza zakresem. |
| `node:parent()` | węzeł albo `nil` | Dla węzłów najwyższego poziomu zwraca korzeń sceny (o nazwie `root`). `nil` zwraca tylko sam korzeń. |
| `tostring(node)` | string | `node<Block A #7>`, a po usunięciu `node<dead #7>`. |

Nie ma metod do tworzenia, usuwania ani przepinania węzłów. Siatka, materiał i światło nie są jeszcze dostępne z Lua.

### Tabela `fumar`

| Funkcja | Zwraca | Uwagi |
|---|---|---|
| `fumar.log(msg)` | | Linia info w logu, z prefiksem `[lua]`. |
| `fumar.warn(msg)` | | Ostrzeżenie. |
| `fumar.find(name)` | węzeł albo `nil` | Pierwszy węzeł o dokładnie tej nazwie, w kolejności w głąb. Przeszukiwanie liniowe, więc znajdź węzeł raz w `on_start` i zapamiętaj, zamiast szukać co klatkę. |
| `fumar.key(name)` | boolean | Prawda, **dopóki** klawisz jest trzymany. Na „właśnie wciśnięty” porównaj z poprzednią klatką. Nazwy klawiszy są niżej. |
| `fumar.mouse_delta()` | `dx, dy` | Piksele od poprzedniej klatki. Ma sens tylko przy przechwyconej myszy, czyli od chwili, gdy skrypt przejmie kamerę. |
| `fumar.camera()` | `x, y, z, yaw, pitch` | Widok w tej klatce. Poza aktualizacją nic nie zwraca. |
| `fumar.set_camera(x, y, z, [yaw], [pitch])` | | Przejmuje widok do Stop. Pominięte kąty zostają bez zmian. |
| `fumar.raycast(ox, oy, oz, dx, dy, dz, [max])` | odległość albo `nil` | Przestrzeń świata. Kierunek jest normalizowany za ciebie. `max` domyślnie 1000 m. `nil` oznacza brak trafienia. Zastrzeżenia są w [§5](#5-błędy-i-pułapki). |

### Nazwy klawiszy

`fumar.key` przyjmuje:

| Grupa | Nazwy |
|---|---|
| Litery | `"w"`, `"a"`, `"s"`, `"d"`, `"q"`, `"e"` |
| Modyfikatory | `"space"`, `"shift"`, `"ctrl"` |
| Strzałki | `"up"`, `"down"`, `"left"`, `"right"` |
| Przyciski myszy | `"mouse_left"`, `"mouse_right"` |

Shift i Ctrl to lewe klawisze. **Nieznana nazwa nie jest błędem: taki klawisz po prostu nigdy nie jest wciśnięty.** Literówka objawia się więc klawiszem, który nic nie robi. Jak dodać klawisz, opisuje [§7](#7-dodawanie-bindingu).

---

## 4. Konwencje: jednostki, przestrzenie, kąty

- **Jednostki:** metry, sekundy, stopnie. Oś Y w górę.
- **Transformacje są lokalne.** `position`, `rotation` i `scale` są względem rodzica, dokładnie jak w panelu Details. Dla węzła bezpośrednio pod korzeniem lokalne i światowe współrzędne są te same.
- **`raycast` działa w przestrzeni świata.** Dla węzła-dziecka jego pozycja i wynik raycastu nie są w tej samej przestrzeni.
- **Obrót węzła:**
  - `set_rotation(pitch, yaw, roll)` składa obrót jako `yaw(Y) * pitch(X) * roll(Z)`.
  - `rotation()` przelicza zapisany kwaternion z powrotem.
  - Zwrócone wartości mogą się różnić od ustawionych (np. `-90` zamiast `270`), choć opisują tę samą orientację. Jeśli potrzebujesz ciągłej wartości, licz kąt sam.
  - Pitch w okolicy ±90° to zwykła osobliwość kątów Eulera.
- **Kąty kamery** odpowiadają `Camera::forward`:

  ```lua
  forward = (cos(yaw) * cos(pitch), sin(pitch), sin(yaw) * cos(pitch))
  ```

  - Yaw 0 patrzy wzdłuż **+X**, a yaw 90 wzdłuż **+Z**.
  - Dodatni pitch patrzy w górę.
  - Trzymaj pitch w granicach ±89°: przy dokładnie ±90° macierz widoku się sypie.
- **`dt`** to rzeczywisty czas klatki. Mnóż przez niego prędkości.

---

## 5. Błędy i pułapki

### Błędy kompilacji

Błędy składni wychodzą przy Compile, zanim cokolwiek się uruchomi. Plik z błędem jest:

- pomijany,
- logowany,
- doliczany do licznika na pasku narzędzi,
- wypisywany na dole panelu Scripts.

Kod na górnym poziomie pliku też wykonuje się przy kompilacji. Błąd w nim liczy się jako błąd kompilacji.

### Błędy w trakcie działania

Błąd w `on_start` albo `on_update` jest łapany (`lua_pcall`), logowany z nazwą skryptu, funkcji i numerem linii, i dopisywany do listy błędów. **Skrypt zostaje podpięty i jest wywoływany w następnej klatce**, więc błąd powtarzający się co klatkę loguje się co klatkę. Popraw go i naciśnij Compile.

### Martwe węzły

Wywołanie metody na usuniętym węźle zgłasza `node N no longer exists`. To celowe: skrypt trzymający usunięty obiekt ma błąd, a komunikat ze wskazaną linią jest lepszy niż cisza.

### Identyfikatory węzłów są używane ponownie

Scena to slot map. Identyfikator usuniętego węzła dostaje następny utworzony węzeł. Tabela z kluczami `node:id()` może więc po usunięciach opisywać inny węzeł. Undo/redo na poziomie sceny też odbudowuje węzły z nowymi identyfikatorami.

W praktyce: buduj stan węzła w `on_start`, a w `on_update` sprawdzaj `nil`, tak jak robi `bob.lua`.

### Wywołania poza aktualizacją

Scena i kontekst są ustawione tylko w trakcie aktualizacji. Kod z górnego poziomu pliku wykonuje się przy kompilacji, czyli poza nią, więc tam:

- `fumar.find` zwraca `nil`,
- `fumar.key` zwraca `false`,
- `fumar.camera` nic nie zwraca,
- `fumar.raycast` zwraca `nil`,
- metody węzła zgłaszają `no scene is active`.

Taką pracę rób w `on_start`.

### Zastrzeżenia do raycastu

`fumar.raycast` sprawdza każdy **widoczny węzeł z siatką**:

- Sprawdzany jest **prostopadłościan otaczający** węzła, a nie jego trójkąty. Promień może trafić w pusty róg pudełka wokół kuli.
- **Pudełko węzła, który woła raycast, też się liczy.** Promień, który **zaczyna się w środku** pudełka, trafia je w odległości `0`. Zacznij promień poza pudełkiem własnego węzła albo nie dawaj skryptowanemu węzłowi siatki.
- Ukryte węzły i wszystko pod nimi są pomijane.

### Kto ma kamerę

Pierwsze wywołanie `fumar.set_camera` w danej sesji Play oddaje widok skryptom i przechwytuje mysz. Escape zwalnia mysz. Stop oddaje widok edytorowi.

Jeśli skrypt Lua i komponent C++ zapiszą kamerę w tej samej klatce, wygrywa C++, bo działa drugi.

---

## 6. Jak działają bindingi (strona C++)

### Pliki

| Plik | Co zawiera |
|---|---|
| `engine/script/include/fumar/script/script_engine.hpp` | `ScriptEngine`: kompilacja, uruchamianie i restart skryptów. Publiczny interfejs. |
| `engine/script/include/fumar/script/script_context.hpp` | `ScriptContext`, `ScriptInput`, `ScriptCameraState`, `ScriptKey`: wszystko, do czego skrypt sięga poza sceną. |
| `engine/script/src/script_engine.cpp` | Stan Lua, środowiska skryptów, wywołania funkcji skryptu. |
| `engine/script/src/lua_bindings.cpp` | Każda funkcja dostępna z Lua: tabela `fumar` i metody `Node`. |
| `engine/script/src/script_context.cpp` | Nazwy klawiszy → `ScriptKey`. |

### Zależności

`fumar_script` zależy tylko od `fumar_core` i `fumar_scene`, **nie od renderera**. Dzięki temu skrypty da się uruchomić w teście bez okna i bez GPU.

To, co wymaga renderera, trafia do bindingów przez callback podany przez aplikację. Tak działa `ScriptContext::raycast`.

`lua_State` nie pojawia się w żadnym publicznym nagłówku: `ScriptEngine` jest pimplem. Kod, który tylko trzyma `ScriptEngine`, nie kompiluje się z nagłówkami LuaJIT.

### Stan Lua

`ScriptEngine` ma jeden `lua_State`, otwarty przez `luaL_openlibs`, po czym wywołuje `script::registerBindings`. Ta funkcja:

- tworzy metatabelę `"fumar.Node"`, która jest swoim własnym `__index` i ma `__tostring`, i wypełnia ją z `kNodeMethods`;
- tworzy globalną tabelę `fumar` z `kFumarFunctions`.

LuaJIT implementuje **API C z Lua 5.1**, więc kod używa:

- `luaL_register`, a nie `luaL_setfuncs` z 5.2;
- `lua_setfenv`, a nie `_ENV`.

### Środowiska skryptów

`compileAll()` robi dla każdego pliku `.lua`:

1. `luaL_loadfile`: sama kompilacja, która łapie błędy składni.
2. Tworzy tabelę środowiska z metatabelą `__index = _G`.
3. `lua_setfenv` ustawia ją jako środowisko chunka.
4. `lua_pcall` wykonuje chunk, więc definicje `function on_update…` lądują w środowisku.
5. Zapisuje środowisko pod nazwą skryptu w tabeli `"fumar.scripts"` w rejestrze. Skrypty nie mają do rejestru dostępu.

Ponowna kompilacja podmienia całą tę tabelę. Dzięki temu znikają usunięte pliki i stare funkcje.

### Wywołanie funkcji skryptu

`ScriptEngine::update` robi kolejno:

1. `setActiveScene` i `setActiveContext`: zapisuje wskaźniki jako **light userdata** w rejestrze (`"fumar.activeScene"`, `"fumar.activeContext"`).
2. Zbiera węzły ze skryptem i dla każdego wywołuje `callHandler`:
   - wkłada środowisko na stos;
   - pobiera funkcję przez `lua_getfield` i kończy, jeśli jej nie ma;
   - wkłada węzeł, a dla `on_update` także `dt`;
   - woła `lua_pcall` i zapisuje ewentualny błąd.
3. Ustawia oba wskaźniki z powrotem na `nullptr`, żeby między klatkami nic nie sięgnęło do nieaktualnej sceny.

Wskaźniki są w rejestrze, a nie w upvalue, bo bindingi są instalowane raz, a scena i kontekst zmieniają się co klatkę.

### Uchwyty węzłów

`pushNode` tworzy pełne userdata z `NodeHandle { NodeId id; }` i nadaje mu metatabelę `"fumar.Node"`.

Uchwyt trzyma identyfikator, a nie `Node*`, bo scena realokuje pamięć przy dodawaniu węzłów. Wskaźnik trzymany przez taką realokację czytałby zwolnioną pamięć.

`checkNode(lua, index)` robi trzy rzeczy:

1. sprawdza typ userdata (`luaL_checkudata`),
2. pobiera aktywną scenę,
3. zgłasza błąd Lua, jeśli identyfikator jest martwy.

Od niej zaczyna się każda metoda.

### Konwencje zwracania

| Rodzaj wartości | Jak przechodzi do Lua |
|---|---|
| Wektory | Kilka liczb, a nie tabela: `position()` wkłada trzy liczby i zwraca `3`. Nie alokuje się tabeli przy każdym wywołaniu, a `local x, y, z = node:position()` czyta się naturalnie. |
| Wyniki opcjonalne | `nil` (`find`, `child`, `parent`, `raycast`). |
| Błędy programisty | `luaL_error` (martwy węzeł, zły typ argumentu przez `luaL_check*`). |
| Wartości, które silnik wybacza | Po cichu ignorowane (nieznane nazwy klawiszy). |

---

## 7. Dodawanie bindingu

### Funkcja w tabeli `fumar`

Przykład: `fumar.time()`, czyli sekundy od naciśnięcia Play.

1. Jeśli wartość pochodzi spoza sceny, dodaj ją do `ScriptContext` (`script_context.hpp`):

   ```cpp
   struct ScriptContext {
       ScriptInput input;
       ScriptCameraState camera;
       f64 playSeconds = 0.0;   // new
       std::function<f32(Vec3, Vec3, f32)> raycast;
   };
   ```

2. Napisz binding w `lua_bindings.cpp`, w anonimowej przestrzeni nazw:

   ```cpp
   /// fumar.time() -> seconds since Play was pressed.
   int fumarTime(lua_State* lua) {
       const ScriptContext* context = contextOrNull(lua);
       lua_pushnumber(lua, context != nullptr ? context->playSeconds : 0.0);
       return 1;   // how many values were pushed
   }
   ```

3. Zarejestruj go w `kFumarFunctions`, przed terminatorem `{nullptr, nullptr}`:

   ```cpp
   {"time", fumarTime},
   ```

4. Wypełnij wartość tam, gdzie edytor wypełnia kontekst (`editor/src/main.cpp`, blok `if (state.scriptsRunning)`).
5. Komponenty C++ od razu widzą ją jako `context.script.playSeconds`, bo dzielą `ScriptContext`. Opisz ją w obu językach; patrz [§9](#9-lua-i-c-obok-siebie).

### Metoda węzła

Przykład: `node:world_position()`.

```cpp
int nodeWorldPosition(lua_State* lua) {
    checkNode(lua, 1);   // raises if dead
    auto* handle = static_cast<NodeHandle*>(lua_touserdata(lua, 1));
    const Vec3 p = xyz(sceneOrNull(lua)->worldTransform(handle->id) * point(Vec3{}));
    lua_pushnumber(lua, p.x);
    lua_pushnumber(lua, p.y);
    lua_pushnumber(lua, p.z);
    return 3;
}
```

Potem dopisz `{"world_position", nodeWorldPosition},` do `kNodeMethods`.

`worldTransform` pochodzi z ostatniego `updateWorldTransforms()`, które działa przed renderowaniem. Węzeł przesunięty wcześniej w tej samej klatce poda pozycję z początku klatki.

### Klawisz

Trzy zmiany, każda w innym pliku:

1. Nowa wartość w `ScriptKey`, przed `Count` (`script_context.hpp`).
2. Jej nazwa w `kNames` (`script_context.cpp`). Rozszerzaj wpisy razem z enumem: tablica ma rozmiar `ScriptKey::Count`, a brakujący wpis nie jest błędem kompilacji. Zostaje pusta nazwa, więc klawisz jest nieosiągalny.
3. Linia w `fillScriptInput` (`editor/src/main.cpp`).

### Zasady dla bindingów

- **Używaj API Lua 5.1.** Na przykład `lua_objlen` zamiast `lua_rawlen`.
- **Nie trzymaj wskaźników do sceny między wywołaniami.** Szukaj węzła po identyfikatorze za każdym razem; robi to `checkNode`.
- **`luaL_error` robi `longjmp`.** Zgłoś błąd, zanim utworzysz w funkcji jakikolwiek obiekt C++ z destruktorem, bo inaczej destruktor się nie wykona. Z tego powodu istniejące bindingi najpierw sprawdzają argumenty, a dopiero potem budują wartości.
- **Nie dołączaj nagłówków renderera do `fumar_script`.** To, czego potrzeba, przekazuj przez `ScriptContext`, jako wartość albo `std::function`.
- **Nazwy w Lua pisz w `snake_case`** (`set_position`, `mouse_delta`), niezależnie od nazw po stronie C++.
- **Licz zwracane wartości.** Liczba zwrócona przez funkcję musi się równać liczbie wartości włożonych na stos.

---

## 8. Komponenty C++

### Interfejs

`engine/native/include/fumar/native/component.hpp`, całość dostępna przez `#include <fumar.hpp>`:

```cpp
class Component {
public:
    virtual ~Component() = default;
    virtual void onStart(Node& node, const ComponentContext& context) {}
    virtual void onUpdate(Node& node, const ComponentContext& context) = 0;
};

struct ComponentContext {
    Scene& scene;            // the whole scene
    ScriptContext& script;   // input, camera, raycast - the same object Lua gets
    f32 deltaSeconds;
};
```

### Pisanie komponentu

Zadeklaruj go w `scripts/cpp/public/components.h`:

```cpp
namespace game {
class Spinner final : public fumar::Component {
public:
    void onUpdate(fumar::Node& node, const fumar::ComponentContext& context) override;
private:
    fumar::f32 m_degreesPerSecond = 45.0f;
};
}
```

Zaimplementuj go w `scripts/cpp/private/*.cpp` i zarejestruj w jedynej eksportowanej funkcji:

```cpp
void game::Spinner::onUpdate(fumar::Node& node, const fumar::ComponentContext& context) {
    const fumar::f32 turn = fumar::radians(m_degreesPerSecond * context.deltaSeconds);
    node.transform.rotation = fumar::normalize(
        fumar::fromAxisAngle(fumar::Vec3{0.0f, 1.0f, 0.0f}, turn) * node.transform.rotation);
}

extern "C" FUMAR_GAME_EXPORT void fumarRegisterComponents(fumar::ComponentRegistry& registry) {
    FUMAR_REGISTER_COMPONENT(registry, game::Spinner);
}
```

Kilka szczegółów:

- Nazwa w edytorze to typ zapisany w makrze, zamieniony na tekst (`#Type`). Zarejestruj go jako `using namespace game; FUMAR_REGISTER_COMPONENT(registry, Spinner);`, żeby dostać `Spinner` zamiast `game::Spinner`; tak robi `components.cpp`.
- Nowe pliki `.cpp` w `scripts/cpp/private/` są dołączane automatycznie (glob z `CONFIGURE_DEPENDS`).
- Komponent podpinasz w **Details → C++**. Scena zapisuje `"component": "Spinner"`.

### Hot reload

**Compile / F5** kompiluje Lua *i* przebudowuje `fumar_game`:

1. Usuwa wszystkie instancje komponentów, potem wyładowuje bibliotekę.
2. Uruchamia `cmake --build … --target fumar_game`. Edytor stoi, dopóki działa kompilator; jego wyjście idzie do terminala, z którego uruchomiono fumara.
3. Kopiuje `fumar_game.dll` do `fumar_game.live.dll` i ładuje kopię. Windows nie pozwala nadpisać załadowanego DLL-a, więc oryginał zostaje wolny dla następnego builda. Na Linuksie dzieje się to samo, żeby była jedna ścieżka kodu.
4. Wywołuje `fumarRegisterComponents`.

Przebudowa działa tylko w drzewie deweloperskim. Wydany build nie ma kompilatora i mówi o tym wprost.

### Zasady, dzięki którym przeładowanie jest bezpieczne

- **Pola komponentu nie przeżywają przeładowania.** Obiekty są niszczone, zanim zostanie wyładowany kod, który je niszczy. Wszystko, co ma przetrwać, trzymaj w scenie: transformację, nazwę, dzieci. Dlatego `Hover` w `onStart` czyta wysokość bazową z węzła.
- **Nie przekazuj kontenerów silnika przez granicę biblioteki.** fumar linkuje CRT statycznie, więc plik wykonywalny i biblioteka mają **osobne sterty**. Nazwy przechodzą jako `const char*`, fabryki to zwykłe wskaźniki na funkcje, a jedyny obiekt, który przechodzi, to `Component`, którego wirtualny destruktor zwalnia go w bibliotece, która go zaalokowała.
- **`fumarRegisterComponents` musi być `extern "C"`**, bo jest wyszukiwana po niezmanglowanej nazwie.
- **Jedna instancja na węzeł.** Zmiana nazwy komponentu w Details tworzy nową instancję. Instancje usuniętych węzłów są sprzątane na końcu każdej aktualizacji.
- **Uważaj na martwe węzły.** `onStart` i `onUpdate` dostają bezpośrednio `Node&`. Jeśli usuwasz węzły albo trzymasz `NodeId`, sprawdzaj `context.scene.isAlive(id)`; silnik robi to między `onStart` a `onUpdate`.

---

## 9. Lua i C++ obok siebie

| Lua | C++ (`context` to `ComponentContext`) |
|---|---|
| `on_start(node)` / `on_update(node, dt)` | `onStart(Node&, ctx)` / `onUpdate(Node&, ctx)` |
| `dt` | `context.deltaSeconds` |
| `node:position()` / `set_position` | `node.transform.position` (`Vec3`) |
| `node:rotation()` (stopnie) | `node.transform.rotation` (kwaternion; `fromAxisAngle`, `radians`) |
| `node:scale()` | `node.transform.scale` |
| `node:visible()` | `node.visible` |
| `node:name()` | `node.name` |
| `node:child(i)` (od 1) / `node:parent()` | `node.children[i]` (od 0) / `node.parent` (`kInvalidNode` = brak) |
| `node:id()` | `NodeId` z `scene.traverse` albo ten, który zapamiętałeś |
| `fumar.find(name)` | `context.scene.traverse(...)` i porównanie `node(id).name`; patrz `Follow` |
| `fumar.key("w")` | `context.script.input.down(fumar::ScriptKey::W)` |
| `fumar.mouse_delta()` | `context.script.input.mouseDelta` |
| `fumar.camera()` | `context.script.camera.position / .yaw / .pitch` |
| `fumar.set_camera(...)` | zapis do `context.script.camera.*` **oraz** `.controlled = true` |
| `fumar.raycast(...)` | `if (context.script.raycast) d = context.script.raycast(origin, normalize(dir), max);` (ujemne = pudło; kierunek normalizujesz sam) |
| `fumar.log` / `fumar.warn` | `FUMAR_INFO(...)` / `FUMAR_WARN(...)` (składnia std::format) |

Różnice między językami są celowe, ale mają pozostać małe. Nowa możliwość trafia do `ScriptContext`, żeby dostały ją oba języki.

---

## 10. Build i układ plików

```
scripts/
  lua/                 pliki .lua, czytane w trakcie działania
  cpp/public/          nagłówki, które mogą dołączać inne pliki gry
  cpp/private/         pliki .cpp kompilowane do fumar_game
engine/script/         fumar_script (bindingi LuaJIT) + fumar_script_api
engine/native/         fumar_native (ładowanie biblioteki gry) + <fumar.hpp>
cmake/FumarLuaJIT.cmake
```

### LuaJIT

LuaJIT jest pobierany i budowany przez `cmake/FumarLuaJIT.cmake` za pomocą `ExternalProject`:

- Wersja jest przypięta do commita z gałęzi 2.1.
- Biblioteka jest statyczna: `msvcbuild.bat static` na Windowsie, `make BUILDMODE=static` gdzie indziej.
- Na Windowsie jest budowana z tym samym statycznym CRT co silnik (`/MT` albo `/MTd`, wstrzykiwane przez zmienną `CL`).

Jest importowana jako `fumar_luajit` i linkowana `PRIVATE` do `fumar_script`.

### `fumar_script_api`

`fumar_script_api` to cel `INTERFACE`: same nagłówki skryptów, bez implementacji.

`fumar_game` i `fumar_native` używają go, żeby dostać `ScriptContext` bez linkowania LuaJIT. Ma to znaczenie, bo `ExternalProject` uruchamia swój krok przy każdym buildzie czegokolwiek, co od niego zależy, i inaczej przycisk Compile czekałby na niego za każdym razem.

### Skąd czytane są skrypty

| Build | Katalog |
|---|---|
| Deweloperski | `scripts/lua/` w drzewie źródeł (`FUMAR_LUA_SOURCE_DIR`), więc zmiany z edytora trafiają do repozytorium. |
| Wydanie | `<katalog exe>/scripts/lua/`. Build kopiuje tam skrypty, a `install()` dołącza je do paczki. |

Brak katalogu nie jest błędem: projekt po prostu nie ma jeszcze skryptów.

### Hot reload `fumar_game` wymaga drzewa deweloperskiego

Plik wykonywalny ma wpisany katalog builda (`FUMAR_GAME_BUILD_DIR`). Na Windowsie build idzie przez `tools/dev.ps1`, który ustawia środowisko clang-cl i Ninja, z którym skonfigurowano katalog builda.
