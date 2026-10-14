# Tryby obciążenia

*[English version](workload.md)*

fumar potrafi przenosić pracę klatki między procesorami. Przełącznik to **Runs on**, w sekcji **Workload** panelu Details (odznacz wszystko, żeby zobaczyć ustawienia świata). Ma trzy tryby:
- **Balanced**, domyślny: zwykły renderer.
- **GPU**: CPU robi niewiele więcej niż nagrywanie komend.
- **CPU**: CPU odpowiada na każde pytanie „co widzi ten punkt”; GPU tylko rasteryzuje i liczy materiały.

Chodzi o to, żeby zobaczyć, co daje przeniesienie pracy. Scena i obraz zostają (prawie) takie same, a panel Statistics pokazuje, gdzie poszły milisekundy.

## Co gdzie działa

| Etap | GPU | Balanced | CPU |
|---|---|---|---|
| macierze węzłów | compute shader | CPU | CPU |
| odrzucanie poza widokiem | compute shader, na każde rysowanie | brak | CPU, na każde rysowanie |
| instancje do ray tracingu | zapisuje je compute shader | CPU | nie są budowane |
| transformacja wierzchołków | vertex shader | vertex shader | CPU, wszystkie rdzenie |
| cienie słońca i lamp | ray query, na piksel | ray query, na piksel | promienie na CPU, na wierzchołek |
| AO i światło odbite | ray query, na piksel | ray query, na piksel | promienie na CPU, na wierzchołek, przez kilka klatek |
| odbicia | śledzone | śledzone | tylko niebo |
| rasteryzacja, BRDF, post | GPU | GPU | GPU |

**Tryb GPU.** Przebieg compute buduje macierze węzłów. Drugi przycina każde rysowanie do widoku, zapisując pośrednią komendę rysowania z zerem lub jedną instancją, i zapisuje instancję obiektu do ray tracingu z jego macierzy świata. CPU nagrywa jedno pośrednie rysowanie na obiekt i nie dotyka żadnej macierzy.

**Tryb CPU.** Wszystko jest śledzone na puli wątków roboczych (po jednym na wątek sprzętowy, minus jeden), na własnej kopii sceny po stronie CPU: drzewie brył otaczających dla każdej siatki i płaskiej liście instancji. Odpowiedzi trafiają na GPU jako drugi strumień wierzchołków. Na GPU nie powstaje żadna struktura do ray tracingu, więc ten tryb działa także bez sprzętowego ray tracingu. Obiekty poza widokiem nie są rysowane, ale dalej rzucają cień.

## Jak czytać profiler

Panel Statistics ma tabelę **Where the frame goes**, z wierszem na każdy etap klatki:

| Wiersz | Co mierzy |
|---|---|
| Waiting for the GPU | CPU czeka na fence slotu klatki |
| Node matrices | macierze świata, na CPU albo w przebiegu compute |
| Culling | test widoczności, na CPU (tryb CPU) albo w przebiegu compute (tryb GPU) |
| Vertex transform | transformacja i wysyłka wierzchołków na CPU (tryb CPU) |
| Shadows, AO, bounce | śledzenie promieni na CPU (tryb CPU) |
| Materials, uniforms | wypełnianie buforów na klatkę |
| Ray structure | wypełnianie instancji na CPU i budowa struktury do ray tracingu na GPU |
| Scene pass, Temporal filter, Bloom, Tone mapping, Interface | przebiegi GPU |
| Submit, present | oddanie klatki |

Jak to czytać:
- **CPU ms** to czas, który CPU spędziło na etapie. Dla przebiegu GPU to tylko czas *nagrania* jego komend, który wynosi mikrosekundy niezależnie od tego, ile przebieg kosztuje.
- **GPU ms** to czas przebiegu na GPU, mierzony znacznikami czasu, które GPU zapisuje samo. Są odczytywane z opóźnieniem dwóch klatek, po fence klatki, więc pomiar nigdy na nic nie czeka.
- **„-”** oznacza, że etap nie działał na tym procesorze w tym trybie. Etap, który nie działa na żadnym, znika z tabeli.
- **Każda liczba jest wygładzona** mniej więcej z ostatnich 25 klatek, bo czas jednej klatki skacze zbyt mocno, żeby go odczytać.
- **Work, total** sumuje czas pracy każdego procesora. Czekanie i prezentacja to CPU stojące w miejscu, więc są pominięte: inaczej klatka ograniczona przez GPU wyglądałaby na obciążającą CPU.
- **Pasek** pod tabelą pokazuje te same sumy jako udział, np. `CPU 72% / GPU 28%`. Procesory pracują jednocześnie, więc klatka trwa mniej więcej tyle, ile bardziej zajęty z nich, a nie sumę.

Jeśli kolejka GPU nie umie zapisywać znaczników czasu, kolumna GPU pokazuje „-”, suma `n/a`, a pasek jest ukryty.

## Ustawienia trybu CPU

Gdy **Runs on** jest na CPU, pod spodem pojawiają się trzy linie:
- **Vertex spacing** (0,1 do 2 m, domyślnie 0,5 m). CPU oświetla każdy *wierzchołek*, a płaszczyzna gruntu z czterech wierzchołków nie pokazałaby żadnego cienia. Dlatego każda siatka jest rysowana z podzielonej kopii, dzielonej, aż żadna krawędź nie jest dłuższa niż ta wartość, mierzona w jednostkach świata po skalowaniu. Podział zatrzymuje się na 262 144 trójkątach na siatkę. Mniejszy odstęp: dokładniejsze cienie, ale więcej wierzchołków do transformacji i śledzenia.
- **Ray budget** (1 do 30 ms, domyślnie 8 ms). Czas rzeczywisty na klatkę, który CPU poświęca na śledzenie, na wszystkich rdzeniach. Więcej to szybsza zbieżność po zmianie, kosztem liczby klatek na sekundę.
- **Próbki**: linia tylko do odczytu, `16 threads, lighting 40/64 samples`. Oświetlenie jest progresywne: każda klatka dokłada próbki na wierzchołek, do 64, i trzyma bieżącą średnią. Liczba to najmniejsza spośród widocznych wierzchołków; `(done)` pojawia się, gdy wszystkie mają 64.

Przesunięcie obiektu, słońca lub lampy albo zmiana ustawienia promieni zaczyna próbki od nowa. Stare wartości zostają na ekranie, dopóki nie zostaną zastąpione, więc przeciąganie wygląda zgrubnie zamiast migać i uspokaja się w kilka klatek po zatrzymaniu. Wzorce próbkowania są te same co na GPU, więc obraz zbiega mniej więcej do tego, co pokazuje Balanced.

## Ograniczenia trybu CPU

- **Cienie na wierzchołek.** Cienie i zacienienie są tak ostre, jak pozwala odstęp wierzchołków. Krawędź cienia mniejsza niż trójkąt rozmywa się na nim.
- **Odbicia tylko nieba.** Odbicia na wierzchołek byłyby rozmazem, więc żadne nie są śledzone: powierzchnie odbijające pokazują niebo, nie inne obiekty.
- **Kolor teksturowany liczony jako 0,5 × współczynnik.** CPU nie ma kopii pikseli tekstur. Gdy promień trafi w materiał z teksturą, jego kolor jest brany jako połowa współczynnika koloru, w zastępstwie środkowego tonu typowej tekstury. To samo dotyczy teksturowanej emisji. Wpływa to tylko na kolor światła odbitego, które jest na tyle rozproszone i słabe, że różnicę rzadko widać.
- **Cień rzucają tylko pierwsze cztery lampy.** Lampy po czwartej oświetlają scenę bez cieni.
