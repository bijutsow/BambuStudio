# ADNL Texture Studio — fork do Bambu Studio

Fork do Bambu Studio (base: `bambulab/BambuStudio` master de 17/11/2026, commit `8e524a3`) com duas funcionalidades **nativas**. As duas aparecem no preview do slicer, porque atuam durante a geração dos perímetros e não num pós-processamento.

## 1. Pintura "Same plane" (mesmo plano)

**Onde:** pintura de fuzzy skin (`Ctrl+H`) → ferramenta **Fill**, e também na pintura de cor → **Fill**, como terceira opção ao lado de *Edge detection* e *Connected same color*.

- **Edge detection** (comportamento original): espalha por triângulos *ligados* até encontrar uma aresta mais vincada do que o ângulo definido.
- **Same plane** (novo): um clique pinta **todos os triângulos complanares** com a face clicada, **estejam ou não ligados**. Isso inclui as ilhas dentro de O, A, R, D, P, B, Q, 0, 6, 8, 9…
  - O fundo das letras gravadas (ou o topo das letras em relevo) é paralelo à face mas está a outra profundidade, por isso **não** é pintado.
  - **Plane tolerance** (mm): distância máxima ao plano. 0,05 mm chega para STL exportados de CAD. Sobe para 0,1–0,2 se a malha for ruidosa. Tem de ficar **abaixo da profundidade do texto**.
  - **Smart fill angle**: desvio máximo da normal (≥ 1°).
  - O hover mostra a pré-seleção antes do clique, como no smart fill. O botão direito apaga com a mesma regra.

**Capa com texto (fluxo de 1 a 3 cliques):** `Ctrl+H` → Fill → Same plane → clique na face com texto. Repete na face de trás e nos bordos, se também os quiseres com textura.

## 2. Texturas de fuzzy skin (Process → Others → *Fuzzy skin* + *Fuzzy skin texture*)

O menu **Fuzzy skin noise type** passa a ter, além de Classic / Perlin / Billow / Ridged / Voronoi:

| Tipo | Uso típico | Notas |
|---|---|---|
| **Ribs** | canelado, frisos, linhas de camada intencionais | `direction` 90° = vertical |
| **Waves** | areia, ondulação | `relief ratio` = altura da onda |
| **Knurl** | serrilhado, pega antiderrapante | pirâmides em diamante |
| **Hexagon** | favo | `relief ratio` alto = juntas finas |
| **Bricks** | tijolo, azulejo, aparelho | `feature size` = comprimento do tijolo |
| **Dots** | pontos, braille decorativo | `profile` Sine = cúpula |
| **Woodgrain** | veio de madeira | combina bem com `stretch` 2–4 |
| **Cracks** | fendas, argila seca, calçada | Voronoi por bordas |
| **Weave** | entrançado, cestaria | |
| **Scales** | escamas, telhas | `Sawtooth` = mais vincado |
| **Image (height map)** | o teu próprio padrão | PNG/JPG em tons de cinzento, branco = relevo |

Os ruídos antigos também ganham **direção e estiramento** (por exemplo Perlin a 45° com stretch 4 dá um aspeto escovado).

### Parâmetros

| Parâmetro | Efeito |
|---|---|
| **Fuzzy skin thickness** | amplitude (mm) do relevo, ± |
| **Fuzzy skin feature size** | período / tamanho da célula / largura do tile da imagem (mm) |
| **Fuzzy skin point distance** | máximo. Nos padrões, o motor desce sozinho até `feature/12` (mín. 0,08 mm) para manter a forma |
| **Texture projection** | *Auto* (ruídos = 3D original), *Box* (caixas, painéis), *Cylindrical* (candeeiros, vasos), *Planar* (uma direção) |
| **Texture direction** | 0° = linhas paralelas às camadas, 90° = verticais |
| **Texture stretch** | alonga ao longo da direção |
| **Texture profile** | Sine (suave), Triangle (chanfro), Square (degrau nítido), Sawtooth (assimétrico) |
| **Texture relief ratio** | % do padrão em relevo |
| **Invert texture** | troca relevo e sulco |
| **Texture jitter** | mistura de fuzzy clássico, para um aspeto feito à mão |
| **Texture repeats around** | (Cylindrical) número de repetições numa volta, para fechar sem costura |
| **Projection azimuth / elevation** | (Planar) direção e inclinação da face |
| **Texture image** | caminho completo do height map |

O relevo aponta sempre **para fora do material**, também nos furos. Por isso o *Invert* tem um significado consistente.

### Receitas

**Capa ADNL Editions impressa a 45° (texto liso, resto texturizado)**
1. Pintura: `Ctrl+H` → Fill → **Same plane** → clique na face. Fuzzy skin do objeto: **None**, e não *Disabled*, que bloqueia também a pintura. As zonas pintadas passam a *All* e herdam os parâmetros de textura do objeto.
2. Noise type **Cracks** · feature 8 mm · thickness 0,35 · profile Square · relief 85 %.
3. Projection **Planar** · azimuth = direção para onde a face aponta (−90° se a face olha para −Y) · **elevation 45°**. Sem isto, o padrão fica esticado 1,41× na vertical.

**Abajur ADNL em vase mode, canelado vertical**
Ribs · Cylindrical · repeats 48 · direction 90° · profile Square · relief 40 % · thickness 0,5 · feature 3.

**Candeeiro "madeira"**
Woodgrain · Cylindrical · repeats 0 · direction 90° · stretch 3 · feature 4 · thickness 0,3 · jitter 10 %.

**Padrão ADNL próprio (do gerador de padrões do ADNL Editions)**
Exporta o padrão em PNG de 1024 px, em tons de cinzento (sulco = preto). Usa Image · feature = largura do tile em mm · Planar ou Box.

### Diferentes texturas no mesmo objeto
A pintura só liga e desliga o fuzzy. A textura vem das definições da região. Para ter duas texturas, usa **modificadores** (Add modifier → Fuzzy skin), cada um com o seu tipo e parâmetros. Para faixas em altura, usa o *Height range modifier*.

### Limites físicos (convém saber)
- A textura vive nas **paredes**. Topos e fundos não recebem textura, tal como no fuzzy original.
- Em altura, a resolução é a **altura de camada**. Detalhes horizontais abaixo de ~4 camadas (0,8 mm a 0,2 mm/camada) perdem-se. Detalhes na horizontal podem ir até ~0,3 mm.
- Thickness acima de ~0,6 × largura de linha pode abrir espaços entre a parede exterior e a interior. Para relevos fundos usa o modo **Combined**.
- O caminho da imagem é absoluto e não vai dentro do 3MF. Guarda os height maps numa pasta fixa (por exemplo `ADNL/heightmaps`).

## Ficheiros alterados
- `src/libslic3r/FuzzyTexture.{hpp,cpp}` — **novo** motor de texturas (sem dependência de PrintConfig, testável à parte)
- `src/libslic3r/FuzzySkin.{hpp,cpp}` — usa o motor e a orientação para fora; o caminho legado mantém-se intacto
- `src/libslic3r/TriangleSelector.{hpp,cpp}` — `plane_fill_select_triangles()`
- `src/slic3r/GUI/Gizmos/GLGizmoPainterBase.{hpp,cpp}`, `GLGizmoFuzzySkin.cpp`, `GLGizmoMmuSegmentation.cpp` — modo Same plane
- `src/libslic3r/PrintConfig.{hpp,cpp}`, `Preset.cpp`, `PrintObject.cpp`, `PerimeterGenerator.cpp` — 11 opções novas e invalidação
- `src/slic3r/GUI/Tab.cpp`, `ConfigManipulation.cpp`, `ParamTooltip.cpp` — interface
- `resources/profiles/BBL/process/fdm_process_common.json` — valores por defeito
- `.github/workflows/build_adnl.yml` — build só para Windows

Os projetos antigos dão o mesmo resultado: com Classic, ou com Perlin/Billow/Ridged/Voronoi em projeção Auto sem transformações, o código antigo corre sem alterações.

## Como obter o executável (Windows)

**Opção A: GitHub Actions (sem instalar nada)**
1. No GitHub, faz **Fork** de `bambulab/BambuStudio`.
2. No teu fork, cria a branch `adnl-texture-studio` e aplica `adnl-texture-studio.patch` (`git am adnl-texture-studio.patch`). Em alternativa, copia a pasta `src/` deste zip por cima.
3. O push corre o workflow **"Build ADNL Texture Studio (Windows)"**. A primeira vez demora ~2 h, porque compila as dependências; depois fica em cache.
4. Em *Actions → o run → Artifacts*, descarrega `BambuStudio_Windows_*_portable`. É uma pasta portátil que corre ao lado do Bambu Studio oficial e usa as mesmas impressoras e perfis.

**Opção B: compilar localmente** (Visual Studio 2022 + CMake): `build_release_vs2022.bat` na raiz do fork. A primeira vez demora 1–2 h.

> Este fork é uma cópia tua. Quando a Bambu lançar uma versão nova, faz *Sync fork* e volta a aplicar o patch. Os ficheiros com mais probabilidade de dar conflito são `PrintConfig.cpp` e `Tab.cpp`.
