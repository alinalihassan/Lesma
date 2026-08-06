

<h1 align="center">
  <img src="tools/docs/public/logo.svg" height="180px" style="height: 180px" alt="Lesma Programming Language" title="Lesma Programming Language">
  <br>
  Lesma
</h1>

<div align="center">

[![Licencia: MIT](https://img.shields.io/github/license/alinalihassan/Lesma?color=yellow)](https://github.com/alinalihassan/Lesma/blob/main/LICENSE.txt)
[![Versión](https://img.shields.io/github/v/release/alinalihassan/Lesma?color=blue)](https://github.com/alinalihassan/Lesma/releases)
[![Plataformas](https://img.shields.io/badge/platforms-%20Linux%20|%20macOS-green.svg?color=lightgrey)](https://github.com/alinalihassan/Lesma/releases)
[![Construcción](https://img.shields.io/github/actions/workflow/status/alinalihassan/Lesma/ci.yaml?branch=main)](https://github.com/alinalihassan/Lesma/actions/workflows/ci.yaml)

</div>

**Lesma** es un lenguaje de programación compilado, estáticamente tipado, imperativo y orientado a objetos con un enfoque en la expresividad, la elegancia y la simplicidad sin sacrificar el rendimiento.

## 📝 Características

- 🚀 Compilación rápida: compila a una velocidad de ≈230k
  loc/s, [porque esperar a que el código se compile es cosa del pasado](https://xkcd.com/303/)
- ⚡ Ejecución ultra rápida: porque debería ser así, es tan rápido como C, utilizando las optimizaciones de última generación de LLVM, pero nunca te obligará a hacer un esfuerzo adicional solo por rendimiento
- 🔬 Estáticamente tipado: porque la autocompletación del IDE es como el cielo, mientras que el comportamiento desconocido y las excepciones en tiempo de ejecución son como el infierno
- 🧑‍🎨 Simple: porque el código debe ser fácilmente legible, y no debería hacerte adivinar lo que hace ni tardar mucho en aprender

## ✍️ Ejemplo

![Fibonacci en Lesma](imgs/lesma_fib.svg)

## 📖 Documentación

- [Documentación oficial](https://lesma-lang.com/)
- [Ejemplos](https://github.com/alinalihassan/Lesma/blob/main/tests/lesma)
- Fuente de la documentación en el repositorio: `tools/docs/content/docs/` (ej. [language/types.mdx](tools/docs/content/docs/language/types.mdx))

## Instalación

Cada versión de Lesma contiene archivos con el binario y la biblioteca estándar que puedes descargar. Alternativamente, puedes usar el script de instalación para que haga todo el trabajo por ti. El script [get-lesma.sh](scripts/get-lesma.sh) descarga e instala la última versión.

Ejecuta lo siguiente en tu terminal:

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/alinalihassan/Lesma/main/scripts/get-lesma.sh)"
```

## 🔧 Construcción

Para construir Lesma, necesitas tener instalados un compilador C++23, LLVM (21 recomendado; ver **AGENTS.md**), `lld`, y Ninja. Recomendamos usar Clang como compilador C++ anfitrión. Actualmente solo es compatible con Linux y macOS.
Para una guía más completa y más información sobre cómo instalar los prerrequisitos,
lee la documentación en [Primeros pasos](https://lesma-lang.com/docs/getting-started/install/)

### Prerrequisitos

**Requerido:**
- CMake 3.24+
- Ninja
- Compilador C++23 (se recomienda Clang)
- LLVM 21 (recomendado; misma generación que las bibliotecas vinculadas)
- lld

### vcpkg (submódulo)

vcpkg está incluido como un submódulo de git para la gestión de dependencias. Después de clonar Lesma, inicializa y configura el entorno:

```bash
git submodule update --init --recursive
cd vcpkg
./bootstrap-vcpkg.sh  # En Linux/macOS
cd ..
```

### Instalación de LLVM

#### Opción 1: Homebrew (macOS)
```bash
brew install llvm lld
export LLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm
```

#### Opción 2: Administrador de paquetes (Linux)
```bash
# Ubuntu/Debian
sudo apt-get install llvm-dev lld clang

# O para una versión específica (ej., LLVM 17)
sudo apt-get install llvm-17-dev lld-17 clang-17
```

#### Opción 3: Compilar LLVM mediante vcpkg (Cualquier plataforma)
Esta opción compila LLVM desde el código fuente usando vcpkg. Toma un tiempo considerable (~1-2 horas), pero funciona en cualquier plataforma.

```bash
# Configurar con la compilación de LLVM habilitada
cmake . -Bbuild -DLESMA_BUILD_LLVM=ON -G Ninja
cmake --build build
```

### Compilar Lesma

1. Clona el repositorio e inicializa el submódulo de vcpkg
    ```bash
    git clone --recurse-submodules https://github.com/alinalihassan/Lesma
    cd Lesma
    ```
    Si ya clonaste sin `--recurse-submodules`, ejecuta `git submodule update --init --recursive` y configura vcpkg (ver arriba).

2. Ejecuta CMake para configurar y compilar
    ```bash
    # Usando preconfiguraciones (recomendado)
    cmake --preset Debug
    cmake --build --preset Debug

    # O manualmente
    cmake . -Bbuild -DCMAKE_TOOLCHAIN_FILE="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake" -G Ninja
    cmake --build build
    ```

3. Ejecutar pruebas (opcional)
    ```bash
    cd build/Debug  # o build/Release
    ctest --output-on-failure
    ```

### Suite de benchmarks (opcional)

Habilita **`LESMA_BUILD_BENCHMARKS`** en CMake, compila el objetivo **`benchmark`** y, desde la **raíz del repositorio**, ejecuta el harness de integración de tiempo real (ejecuta `lesma run` por cada prueba y escribe un JSON junto con una especificación opcional de gráfico Vega-Lite):

```bash
./build/Debug/benchmark suite ./build/Debug/lesma \
  --vega-lite-out suite.vl.json \
  --json-out bench.json
```

Los tiempos en `bench.json` están en **milisegundos** (`milliseconds` por prueba; `total_wall_milliseconds` / `mean_milliseconds_per_test` en `aggregate`). Renderiza un SVG estático con la CLI de Vega-Lite (no requiere `canvas` nativo); **redirige stdout** para que el SVG no se imprima en la terminal:

```bash
npx -p vega-lite vl2svg suite.vl.json > chart.svg
```

Consulta **AGENTS.md** para las banderas (`--suite`, `--opt`, JSON de GitHub Actions y opciones PNG).

## 💬 Contribuciones

Los pull requests son bienvenidos. Para cambios importantes, abre un issue para discutir tu propuesta y lo que te gustaría cambiar.

Las herramientas del repositorio están bajo `tools/`:

- `tools/docs` contiene el sitio de documentación de Fumadocs (Vite + React Router), incluido el playground integrado en `/playground`, y se compila en la imagen de contenedor **unificada**.
- `tools/playground` incluye **`wrangler.jsonc`** + **`worker.ts`** + **`Dockerfile`** para **Cloudflare Workers + Containers**: una imagen sirve la SPA de documentación combinada en `/` (ruta de playground incluida) y la API del compilador en `/api`.
- `tools/vscode` contiene la extensión de VS Code que inicia el servidor nativo `lesma-lsp`.

- Para mantenerte actualizado con los lanzamientos, considera marcar el proyecto con una estrella.
- Consulta el [código de conducta](CODE_OF_CONDUCT.md) y las [guías de contribución](CONTRIBUTING.md)

## 📎 Licencia

Este software está licenciado bajo [MIT](https://github.com/alinalihassan/Lesma/blob/main/LICENSE.txt)
© [Alin Ali Hassan](https://github.com/alinalihassan).
