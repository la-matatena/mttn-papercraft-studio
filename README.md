# MTTN PaperCraft Studio

> 🌐 **Español** | [English](README.en.md)

[![CI & Release](https://github.com/la-matatena/mttn-papercraft-studio/actions/workflows/ci-release.yml/badge.svg)](https://github.com/la-matatena/mttn-papercraft-studio/actions/workflows/ci-release.yml)
[![Latest Release](https://img.shields.io/github/v/release/la-matatena/mttn-papercraft-studio?color=8367c7&label=release)](https://github.com/la-matatena/mttn-papercraft-studio/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-8367c7.svg)](LICENSE)
[![OpenFX 1.4 C++17](https://img.shields.io/badge/OpenFX-1.4%20C%2B%2B17-6a4fb3.svg)](ofx/Info.plist)
[![DaVinci Resolve 20 | 21](https://img.shields.io/badge/DaVinci%20Resolve-20%20%7C%2021%20Free%20%26%20Studio-8367c7.svg)](docs/user_guide.md)
[![Platform: macOS & Linux](https://img.shields.io/badge/platform-macOS%20Universal%20%7C%20Linux%20x86__64-6a4fb3.svg)](docs/developer_guide.md)
[![Brand: La Matatena](https://img.shields.io/badge/La%20Matatena-MTTN-8367c7.svg)](https://github.com/la-matatena)

**MTTN PaperCraft Studio** (desarrollado por **[La Matatena](https://la-matatena.com)** · [@la-matatena](https://github.com/la-matatena)) añade recortes de papel estilo stop-motion orgánico, bordes de cartulina rasgada, transiciones con dobleces 3D y efectos de sonido de papel Foley en estéreo de 24 bits directamente sobre la página de Edición (Edit) de DaVinci Resolve 20 y 21 sin necesidad de abrir Fusion. Tanto DaVinci Resolve Free como DaVinci Resolve Studio ejecutan el plugin OpenFX 1.4 nativo en C++17 sin marcas de agua.

## Contenido del paquete

| Componente | Ruta del paquete | Propósito |
| :--- | :--- | :--- |
| Paquete OpenFX nativo | `PaperCraftStudioPro.ofx.bundle` | Efectos, generadores y transiciones directas en C++17 para la página Edit (categoría `MTTN PaperCraft Studio`) |
| Instalador OpenFX para macOS | `Install_PaperCraft_OpenFX_macOS.command` | Compila el paquete Universal (`x86_64` + `arm64`) o nativo de macOS, firma ad-hoc e instala en `/Library/OFX/Plugins` |
| Instalador OpenFX para Linux | `install_linux_ofx.sh` | Instala `PaperCraftStudioPro.ofx.bundle` en la carpeta OpenFX de Linux |
| Código fuente OpenFX portátil | `OpenFX_Source/` | Código C++17 independiente y cabeceras de OpenFX 1.4 para compilación en macOS y Linux |
| Librería de sonidos Paper SFX | `Paper_SFX/` (24 archivos WAV estéreo) | Sonidos Foley a 48 kHz y 24 bits estéreo organizados en 6 categorías táctiles de papel |
| Documentación y arquitectura | `docs/` (`user_guide.md`, `design_doc.md`, `developer_guide.md`) | Guía de usuario, guía de compilación para desarrolladores y arquitectura técnica detallada |
| Plantillas `.drfx` clásicas | `MTTN_PaperCraft_Studio.drfx` / `Templates_Unpacked/` | Plantillas Fusion `MacroOperator` validadas con miniaturas de identidad púrpura La Matatena |

## Documentación detallada

Consulta las guías completas para la instalación, flujos de trabajo en la página Edit y diseño de ingeniería:

- [Guía de usuario](docs/user_guide.md): Pasos de instalación, flujos de trabajo de recortes y transiciones en la página Edit, explicación de parámetros del Inspector, tabla de mezcla para Paper SFX y resolución de problemas.
- [Documento de diseño de ingeniería](docs/design_doc.md): Visión general de la arquitectura, integración del host OpenFX 1.4, campo de distancia con signo (SDF) en tiempo lineal, síntesis de audio Foley y decisiones de ingeniería.
- [Guía para desarrolladores y compilación](docs/developer_guide.md): Estructura del repositorio, compilación independiente en Linux y macOS y suites de verificación.

## Instalación del plugin nativo OpenFX para la página Edit

### Instalación en macOS

Se recomienda utilizar DaVinci Resolve descargado desde el sitio oficial de Blackmagic Design en lugar de la edición de la Mac App Store, ya que las restricciones de la sandbox de Apple impiden a la versión de la App Store cargar plugins OpenFX de terceros desde `/Library/OFX/Plugins`.

1. Descarga y extrae `MTTN_PaperCraft_Studio_Full_Bundle.zip` desde [GitHub Releases](https://github.com/la-matatena/mttn-papercraft-studio/releases/latest) (o clona este repositorio).
2. Haz doble clic en `Install_PaperCraft_OpenFX_macOS.command`, o ejecuta el script instalador desde **Terminal**:

   ```bash
   ./ofx/Install_PaperCraft_OpenFX_macOS.command
   ```

   Si prefieres una instalación a nivel de usuario sin requerir permisos de administrador (`sudo`), añade `--user` para instalar en `${HOME}/Library/OFX/Plugins`:

   ```bash
   ./ofx/Install_PaperCraft_OpenFX_macOS.command --user
   ```

3. Si `clang++` no está instalado aún, autoriza la descarga de las Command Line Tools de macOS y vuelve a ejecutar el instalador.
4. Reinicia DaVinci Resolve para que detecte el paquete OpenFX.

### Instalación en Linux

1. Extrae `MTTN_PaperCraft_Studio_Full_Bundle.zip`.
2. Ejecuta el script instalador para Linux:

   ```bash
   ./install_linux_ofx.sh
   ```

   Añade `--user` para instalar en `${HOME}/.local/share/OFX/Plugins` sin permisos de root, o `--dest DIRECTORIO_PERSONALIZADO` para elegir otra ruta:

   ```bash
   ./install_linux_ofx.sh --dest DIRECTORIO_PERSONALIZADO
   ```

3. Reinicia DaVinci Resolve para cargar la suite OpenFX.

## Uso de MTTN PaperCraft Studio en la página Edit

El paquete OpenFX registra cuatro herramientas dentro de la categoría **MTTN PaperCraft Studio**:

- **Paper Cutout FX** (filtro): Abre **Effects** > **Open FX** > **MTTN PaperCraft Studio** y arrastra **Paper Cutout FX** directamente sobre cualquier clip de la línea de tiempo, título, archivo PNG transparente o toma con pantalla verde.
- **Paper Cutout FX** (generador): Abre **Effects** > **Generators** > **MTTN PaperCraft Studio** y arrastra **Paper Cutout FX** sobre una pista de video vacía para generar una estampa/sticker animado independiente con el logo de La Matatena.
- **Paper Unfold Transition**, **Paper Crumple Transition** y **Paper Rip Wipe Transition**: Abre **Effects** > **Video Transitions** > **MTTN PaperCraft Studio** y arrastra cualquiera de las transiciones sobre el corte entre dos clips.

## Referencia de controles del Inspector

| Sección | Controles clave | Comportamiento |
| :--- | :--- | :--- |
| **1. Media Source and Placement** | **Source Mode**, **Premultiplied Source Alpha**, **Media Scale**, **Media Position**, **Media Rotation** | Alterna entre la entrada del clip de la línea de tiempo y la estampa integrada de La Matatena, permitiendo encuadrar la imagen dentro del recorte |
| **2. Green Screen and Matte Cleanup** | **Enable Green Screen Key**, **Screen Color**, **Key Gain**, **Key Balance**, **Edge Choke (px)** | Extrae la máscara del sujeto sobre fondos verdes o azules uniformes con supresión de contaminación de color |
| **3. Torn Paper Cutout Border** | **Enable Paper Border**, **Border Width**, **Torn Edge Roughness**, **Tear Frequency**, **Tear Fiber Detail**, **Edge Boil Speed**, **Paper Cardstock Color** | Genera un borde perimetral procedural de cartulina rasgada alrededor de la silueta alfa |
| **4. Paper Grain and Fold Creases** | **Paper Grain Strength**, **Grain Scale**, **Fold Crease Strength**, **Crease Scale**, **Media Texture Overlay**, **Texture Seed** | Añade textura de fibra de papel multicapa y pliegues marcados de doblado |
| **5. Intro and Outro Paper Animations** | **Intro Style**, **Intro Duration (Frames)**, **Outro Style**, **Outro Duration (Frames)**, **Glide Angle**, **Glide Distance**, **Mirror Animation H**, **Mirror Animation V** | Anima entradas y salidas con estilos **Paper Unfold (3D)**, **Crumple Ball**, **Paper Airplane Glide** o **Flat Cutout Pop** |
| **6. 3D Drop Shadow** | **Shadow Opacity**, **Shadow Angle**, **Shadow Distance**, **Shadow Softness** | Proyecta una sombra suave y direccional con profundidad detrás del recorte en rotación |
| **7. Stop-Motion and Master Transform** | **Enable Stop-Motion**, **Hold Frames (Step)**, **Position Jitter**, **Rotation Jitter**, **Master Position**, **Master Scale**, **Master Rotation**, **Flip Horizontal**, **Flip Vertical** | Cuantiza el movimiento en fotogramas sostenidos y añade una vibración táctil fotograma a fotograma |

## Librería de sonidos Paper SFX

Importa la carpeta `Paper_SFX/` dentro de un Power Bin de DaVinci Resolve para tener disponibles los 24 archivos WAV a 48 kHz y 24 bits estéreo en todos tus proyectos:

- `01_Page_Turns/`: Cuatro giros nítidos de hojas de libro, cartulina, periódico y cambio rápido.
- `02_Paper_Hits_Impacts/`: Cuatro golpes secos sobre mesa, caídas de cartón, pegado de stickers y apoyos de pilas de papel.
- `03_Crumples_Uncrumples/`: Cuatro arrugados apretados, crujidos de periódico y desarrugados suaves o rápidos.
- `04_Rips_Tears/`: Cuatro rasgados rápidos, desgarros lentos de fibra, roturas de kraft grueso y cortes con tijeras.
- `05_Little_Bits_Rustles/`: Cuatro dispersiones de confeti, balanceos de stop-motion, roces de mesa y pliegues manuales.
- `06_Designed_Transitions/`: Cuatro barridos de papel en capas, planeos de avión, impactos de arrugado y apariciones pop stop-motion.

## Plantillas clásicas `.drfx` opcionales

El archivo validado `MTTN_PaperCraft_Studio.drfx` se incluye para quienes prefieren trabajar con plantillas de Fusion. Haz doble clic en `MTTN_PaperCraft_Studio.drfx` para instalar las copias `MacroOperator` en **Effects** > **Toolbox** > **MTTN PaperCraft Studio**. Para líneas de tiempo cotidianas se recomienda el plugin OpenFX nativo, ya que se ejecuta en C++17 multihilo sin la sobrecarga del grafo de nodos de Fusion.

## Optimizar la fluidez de reproducción y renderizado

- Cambia **Playback** > **Render Cache** a **Smart** o **User** una vez que hayas ajustado los parámetros del recorte.
- Reduce temporalmente **Playback** > **Timeline Proxy Resolution** a **Half** o **Quarter** al ajustar los tiempos de las transiciones stop-motion.

## Compilación y verificación desde el código fuente

Ejecuta la suite de pruebas en Python para validar el `.drfx`, miniaturas PNG, archivos de audio Foley WAV y el empaquetado general:

```bash
python3 -B papercraft_pro_test.py -v
```

Compila el paquete OpenFX Universal para macOS en un directorio temporal sin modificar `/Library/OFX/Plugins`:

```bash
./ofx/Install_PaperCraft_OpenFX_macOS.command --build-only ./dist
```

Construye el paquete completo de lanzamiento (`.drfx`, 24 WAVs de Foley, instaladores, documentación y código fuente):

```bash
python3 -B build_papercraft_bundle.py \
  --output_dir=dist \
  --readme_path=README.md \
  --linux_ofx_path=PaperCraftStudioPro.ofx
```

## Licencia

Publicado bajo la [Licencia MIT](LICENSE) — Copyright (c) 2026 [La Matatena](https://la-matatena.com). Las cabeceras C de OpenFX 1.4 incluidas mantienen su licencia permisiva en [`ofx/openfx_include/LICENSE`](ofx/openfx_include/LICENSE).
