# Guía para agentes de IA · MTTN PaperCraft Studio

Contexto rápido para asistentes de código que trabajen en `la-matatena/mttn-papercraft-studio`. La fuente de verdad completa de la organización está en [`la-matatena/.github/CONVENTIONS.md`](https://github.com/la-matatena/.github/blob/main/CONVENTIONS.md) y [`la-matatena/.github/AGENTS.md`](https://github.com/la-matatena/.github/blob/main/AGENTS.md).

## Reglas esenciales

- **Producto y entregables**: `MTTN PaperCraft Studio` (`la-matatena/mttn-papercraft-studio`), paquete `MTTN_PaperCraft_Studio_Full_Bundle.zip`, plantillas `MTTN_PaperCraft_Studio.drfx` y bundle OpenFX `PaperCraftStudioPro.ofx.bundle` (categoría `MTTN PaperCraft Studio` en DaVinci Resolve 20 y 21).
- **Estabilidad de identificadores OpenFX**: conserva siempre los identificadores de plugins (`com.papercraft.CutoutFX` y `com.papercraft.PaperFoldTransition`) y los nombres internos de parámetros (`transitionAngle`, `paperColor`, `shadowOpacity`, etc.) para no romper proyectos existentes en DaVinci Resolve.
- **100 % CPU OpenFX 1.4 C++17 y sin marcas de agua**: compatible con DaVinci Resolve Free y Studio en macOS Universal (`arm64` + `x86_64`) y Linux `x86_64`, sin dependencias de red ni shaders propietarios.
- **Audio Foley**: todos los WAV en `Paper_SFX/` deben ser grabaciones en dominio público (`CC0` / `Public Domain`) a 48 kHz y 24 bits estéreo, documentadas en `Paper_SFX/CREDITS.md`.
- **Idioma y paridad**: `README.md` y `site/index.html` en español; `README.en.md` y `site/en/index.html` sincronizados en inglés; código e identificadores en inglés.

## Verificación antes de terminar

- Ejecuta la suite de validación en Python (`.drfx`, miniaturas PNG, audio WAV, instaladores y enlaces):
  ```bash
  python3 -B papercraft_pro_test.py -v
  ```
- Si modificas `ofx/`, verifica la compilación del bundle OpenFX en macOS:
  ```bash
  ./ofx/Install_PaperCraft_OpenFX_macOS.command --build-only /tmp/papercraft_build
  ```
- Usa Conventional Commits en inglés (`feat:`, `fix:`, `docs:`, `chore:`, `refactor:`, `test:`, `perf:`).
- No publiques releases, tags ni hagas `push` sin confirmación explícita.
