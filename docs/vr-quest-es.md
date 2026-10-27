# LAS Recomp VR en Meta Quest 3 — guía completa (WSL / Ubuntu)

Todo lo de este documento está ya implementado en el repositorio, en la rama **`vr-quest3`** (commit `5cae77e`,
12 archivos). No hay que copiar código a mano.

---

## 0. Qué hay dentro (y dónde)

| archivo | qué hace |
| --- | --- |
| `runtime/platform/xr_quest.c` | Motor VR: instancia y sesión OpenXR, el panel del juego (un *swapchain* sobre una Surface de Android), las capas por modo, el refresco del visor, el passthrough, **los mandos Touch** y el puntero de los menús. |
| `runtime/platform/xr_quest.h` | Lo que el resto del motor puede preguntarle al lado VR (`vr_stereo_on`, `vr_stereo_split_panel`, `vr_mode_preset`...). |
| `runtime/nvn/vulkan/vr_stereo.c` | Los modos estéreo: cada ojo recibe su mitad del panel, desplazada píxel a píxel por la profundidad de la escena. Dos shaders nuevos (`vr_stereo.frag`, `vr_stereo_mask.frag`). |
| `runtime/nvn/vulkan/vr_stereo.h` | Su interfaz para `device.c`. |
| `runtime/nvn/vulkan/device.c` | Enlace con el present: el panel se crea en la Surface de OpenXR, el tamaño real se comunica, el juego dibuja en la mitad de un ojo y el warping rellena las dos. |
| `runtime/nvn/vulkan/framegen.c` | Cede a los modos estéreo la profundidad, la cámara y la copia de la escena sin HUD; la generación de fotogramas se apaga en estéreo. |
| `runtime/nvn/vulkan/exec.c` | Copia la escena justo al componer (también cuando solo el estéreo la necesita). |
| `runtime/settings.{c,h}` | Los ajustes nuevos (`vr_mode`, `vr_panel`, `vr_refresh`, `vr_distance`, `vr_size`, `vr_curve`, `vr_tilt`, `vr_depth`) y su guardado en `larecomp.ini`. |
| `runtime/ui/menu.mm` | La sección **VR (Meta Quest)** de la pestaña Vídeo y el puntero del mando en los menús (`menu_vr_pointer`). |
| `docs/vr-quest.md` | La documentación técnica (en inglés, como el resto de `docs/`). |
| `.gitignore` | Ignora también los ficheros de la clave (`android/*.jks`). |

Los ficheros de Android que hacen que exista la entrada **LAS Recomp VR** (el `activity-alias` en
`android/app/src/main/AndroidManifest.xml` y el `LARECOMP_XR=1` en `LasActivity.java`) ya estaban en el repositorio: el
cambio de VR no los toca.

### Cómo traer los cambios

```bash
cd ~/LASRecompQ                              # tu copia del repositorio
git fetch origin
git switch -c vr-quest3 --track origin/vr-quest3
git log --oneline -1                         # 5cae77e Quest VR: cinema / curved / ...
```

Si prefieres un parche suelto: `vr-quest-quest3.patch` (12 archivos) se aplica con
`git apply --check ../vr-quest-quest3.patch && git apply ../vr-quest-quest3.patch` sobre una copia limpia de `main`.

---

## 1. FASE 0 — Entorno en WSL y firma

### 1.1 Paquetes del sistema

```bash
sudo apt update
sudo apt install -y openjdk-17-jdk unzip zip git python3 python3-venv python3-pip \
                    build-essential cmake ninja-build pkg-config wget curl xxd file
java -version      # debe decir 17.x (AGP 8.11 + Gradle 8.14.3: JDK 17, no 21)
```

### 1.2 Android SDK (herramientas de línea de comandos, sin Android Studio)

```bash
mkdir -p ~/android-sdk/cmdline-tools
cd /tmp
wget https://dl.google.com/android/repository/commandlinetools-linux-16111833_latest.zip
unzip -q commandlinetools-linux-16111833_latest.zip -d ~/android-sdk/cmdline-tools
mv ~/android-sdk/cmdline-tools/cmdline-tools ~/android-sdk/cmdline-tools/latest   # la ruta debe ser .../cmdline-tools/latest/bin
```

> Si esa descarga da 404, entra en <https://developer.android.com/studio#command-line-tools-only>, copia el enlace
> del ZIP de Linux y sustituye la URL (el nombre cambia de versión en versión).

Variables de entorno (añádelas al final de `~/.bashrc` y luego `source ~/.bashrc`):

```bash
export ANDROID_HOME=$HOME/android-sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH
```

### 1.3 Los paquetes del SDK que pide el proyecto

```bash
source ~/.bashrc
yes | sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-36" "build-tools;36.0.0" \
           "ndk;27.2.12479018" "cmake;3.22.1"
sdkmanager --list_installed     # comprobación
adb version
```

Gradle **no** se instala: el repo trae su *wrapper* (`android/gradlew`, Gradle 8.14.3) y se lo descarga solo.

Ruta del SDK para Gradle (fichero ignorado por git):

```bash
cd ~/LASRecompQ
echo "sdk.dir=$HOME/android-sdk" > android/local.properties
```

### 1.4 Tu firma (keystore) en modo Release

El `build.gradle` del proyecto ya lee `android/keystore.properties` si existe y firma con esa clave; si no existe, usa
la clave de depuración. Crear tu clave (una sola vez, **guárdala**: sin ella no podrás actualizar la app instalada):

```bash
cd ~/LASRecompQ
keytool -genkeypair -v -keystore android/lasrelease.jks -alias lasrecomp \
        -keyalg RSA -keysize 4096 -validity 10000 -storetype PKCS12
```

Te pedirá una contraseña (dos veces) y tus datos (nombre, organización...: da igual lo que pongas). Después crea
`android/keystore.properties` con exactamente esto:

```properties
storeFile=../lasrelease.jks
storePassword=LA_CONTRASENA_QUE_PUSISTE
keyAlias=lasrecomp
keyPassword=LA_CONTRASENA_QUE_PUSISTE
```

Notas importantes:

* `storeFile` es relativo a `android/app/`, por eso `../lasrelease.jks` apunta a `android/lasrelease.jks`.
* Los dos ficheros están en `.gitignore`: **no se suben** al repositorio.
* Mientras uses siempre la misma clave, cada APK nuevo **actualiza** la app instalada en las Quest conservando partidas
  y ajustes. Si algún día cambias de clave, Android rechazará la instalación (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) y
  tendrás que desinstalar antes (se pierde `Android/data/com.lasrecomp.android/files/`: copia la carpeta `save`).

### 1.5 Dependencias nativas del proyecto (una sola vez, tarda)

```bash
cd ~/LASRecompQ
tools/android/build_deps.sh        # FFmpeg, shaderc, cabeceras Vulkan, VMA, volk, SDL3  -> build/android/deps
tools/android/ondevice/build_all.sh # el compilador que corre EN el visor (Python, zig, kit del generador)
```

`build_deps.sh` necesita red y unos 30–60 minutos la primera vez. `build_all.sh` es lo que permite que el APK compile
el código del juego dentro de las Quest (la ruta más cómoda para ti, ver 1.6).

### 1.6 Compilar el APK Release firmado

```bash
cd ~/LASRecompQ/android
./gradlew assembleRelease -PonDeviceBuild=true
```

Resultado: `android/app/build/outputs/apk/release/app-release.apk` (≈76 MB, firmado con **tu** clave, sin código del
juego dentro: el visor lo genera a partir de tus propios archivos la primera vez que abras el juego).

Comprobación opcional de que el APK no lleva datos del juego:

```bash
tools/android/ondevice/check_public_apk.sh android/app/build/outputs/apk/release/app-release.apk
```

> Alternativa (APK "personal" con el código del juego ya dentro, compilado en el PC):
> `tools/android/build_game.sh <carpeta del juego> --apk`. Ojo: ese script genera un APK **debug** (clave de
> depuración), incompatible con el release firmado por ti. Elige una ruta y no las mezcles.

---

## 2. FASE 1 — Mandos Touch de Quest 3

Dónde se gestiona la entrada en este port, para que sepas qué se ha tocado y qué no:

| entrada | archivo | qué hace |
| --- | --- | --- |
| Mando/joystick del sistema | `runtime/hle/input.c` (`pad_poll`, `SDL_GetGamepadButton`) | Traduce un gamepad SDL a los botones de la consola (A, B, L, R, ZL, ZR, +, -, cruceta). **No se ha tocado**: es el sitio correcto por el que entra cualquier mando. |
| Toque / ratón | `runtime/ui/menu.mm` (`menu_handle_event`, `drain_events`) | Convierte eventos SDL en la cola de eventos de ImGui (menús, teclado en pantalla, controles táctiles). |
| VR | `runtime/platform/xr_quest.c` (`make_actions`, `pad_update`, `pointer_update`) | **Nuevo**: lee los mandos por OpenXR y los inyecta como gamepad virtual SDL + puntero de rayo. |

La idea: los Touch no se traducen a "toques de pantalla", sino a un **gamepad virtual de SDL** con los ids de un mando
de Xbox. Así funcionan sin cambios los enlaces de botones del juego, sus textos ("Pulsa A"), la reasignación en
Controles y los menús propios del juego.

Mapeo (por defecto):

| Quest 3 | Juego |
| --- | --- |
| Stick izquierdo | Movimiento |
| Stick derecho | Cruceta (menús del juego y navegación de los menús de la app) |
| A | A (confirmar, hablar, levantar) |
| B | B (espada) |
| X / Y | X / Y |
| Gatillos L / R | ZL / ZR (analógicos) |
| Empuñaduras (grip) L / R | L / R (escudo, botas Pegaso) |
| Clic stick izquierdo | Menos (mapa) |
| Botón menú | Más (objetos / pausa) |
| Clic stick derecho | Menú de ajustes de la app |
| Menú + clic stick izquierdo | Menú de ajustes |
| Gatillo derecho (rayo al panel) | Clic en los menús de la app mientras están abiertos |

Detalles que importan en la práctica:

* Zona muerta radial del 15 % en los sticks (los Touch no vuelven exactamente al centro).
* Mientras un menú de la app está abierto, el juego no recibe entrada: el mismo gatillo derecho puede hacer clic en el
  menú sin pulsar nada del juego.
* El rayo funciona con el panel plano y con el curvo (en el curvo se aproxima por el segmento central).

---

## 3. FASE 2 — Resolución y modos gráficos

### 3.1 El problema que se corrige

Antes, el panel era la ventana del visor tal cual (Quest 3 declara una ventana de más de 4000 px de ancho) y, en los
modos estéreo, el juego habría tenido que dibujar eso **por ojo**. Ahora el panel se crea a la resolución elegida
(1080p por ojo por defecto), y como el motor ajusta la resolución interna al tamaño de la ventana, el juego renderiza
exactamente eso: **1920x1080 por ojo** (3840x1080 el panel completo en estéreo) en vez de ~4000x2000 por ojo.

### 3.2 Dónde se elige

En el visor: **menú de la app (clic stick derecho) → pestaña Vídeo → sección "VR (Meta Quest)"**.

| ajuste | qué hace | valores |
| --- | --- | --- |
| VR mode | el modo gráfico | Cinema (flat) / Cinema (curved) / Window (3D depth) / Diorama (table) |
| VR panel resolution | píxeles por ojo = resolución de render | 720p / **1080p** / 1440p / Native (se aplica al reiniciar) |
| VR refresh rate | refresco del visor | el del visor / 72 / 90 / 120 Hz |
| VR distance | distancia del panel | 0,5–6,0 m |
| VR size | ancho del panel | 0,8–8,0 m |
| VR tilt | inclinación hacia atrás | 0–80° (el diorama usa 42°) |
| VR curve | arco de la pantalla curva | 20–160° |
| VR depth | separación estéreo (ojo) | −150 %…+150 % (negativo intercambia los ojos) |

Al cambiar de modo se aplican valores de distancia/tamaño/inclinación/profundidad que le van bien; después puedes
afinar cada uno.

### 3.3 Los tres modos que pediste

1. **Pantalla gigante curva (Cine)** → `Cinema (curved)`: la imagen se reparte en 6 capas (`XrCompositionLayerQuad`)
   formando un arco alrededor tuyo, con radio y ángulo según `VR curve` y `VR size`. Solo capas estándar de OpenXR: no
   depende de extensiones de cilindro, así que funciona en cualquier runtime.
2. **Pantalla con profundidad (Ventana 3D)** → `Window (3D depth)`: el panel se parte en dos mitades, una por ojo; cada
   mitad se genera desplazando cada píxel según la profundidad de la escena (la misma cámara y el mismo búfer de
   profundidad que usa la generación de fotogramas). El HUD (corazones, iconos) se detecta y se deja fijo en el plano
   del panel para que no "nade" con el escenario.
3. **Maqueta / Diorama** → `Diorama (table)`: el mismo par estéreo, inclinado 42° hacia atrás y colgado por debajo de
   los ojos, como una mesa con el mundo encima.

### 3.4 Lo que estos modos NO son (importante, para no llevarte una sorpresa)

* **No cambian la cámara del juego.** La proyección del juego vive dentro del código recompilado; ponerla en
  ortográfica/isométrica obligaría a reescribir su proyección y sus matrices por frame (otro proyecto entero). El modo
  maqueta consigue el efecto de "mundo sobre una mesa" con geometría de presentación (panel inclinado y bajo + estéreo),
  no con una cámara nueva.
* **El estéreo es reproyección por profundidad de un único render**, no dos renders reales: el escenario, las paredes y
  las plataformas se ven con volumen correcto; los sprites (personajes) se ven algo más planos porque el juego los dibuja
  como carteles 2D, y en bordes muy finos puede aparecer algún artefacto de 1–2 píxeles.
* **La generación de fotogramas (60 fps generados) se desactiva** en los modos estéreo: las dos mitades deben ser el
  mismo instante. En los modos planos sigue igual que en el móvil.
* En estéreo el panel completo mide el doble de ancho: 1080p por ojo = 3840x1080 reales. Si las Quest no mantienen el
  ritmo, baja a 720p por ojo o a 72 Hz.

---

## 4. Instalar y probar en las Quest 3

1. Activa el **modo desarrollador** en el visor (app Meta Horizon del móvil → Dispositivo → Modo desarrollador).
2. En WSL, ADB por Wi-Fi (lo más fiable; el USB en WSL necesita `usbipd-win` en Windows):

```bash
adb pair 192.168.1.XX:PUERTO    # IP y puerto que muestra el visor en Configuración > Sistema > Desarrollador
adb connect 192.168.1.XX:5555
adb devices                      # debe aparecer "device"
```

3. Instala (la primera vez y en cada compilación nueva, `-r` conserva datos):

```bash
adb install -r android/app/build/outputs/apk/release/app-release.apk
```

4. Abre **LAS Recomp VR** en *Orígenes desconocidos* (o `adb shell am start -n com.lasrecomp.android/.LasVrActivity`).
   El APK release no lleva el juego dentro: la primera vez, dentro del visor, eliges tu `.xci`/`.nsp` + `prod.keys` y el
   propio visor genera el código (el proceso tarda y se ve una notificación; en Quest 3 es más rápido que los ~11 min de
   un Snapdragon 865).
5. Ajustes: **clic en el stick derecho** → Vídeo → VR (Meta Quest).

### Si algo va mal, el log lo dice

```bash
adb logcat -s LASRecomp-XR
```

* `OpenXR did not start` → se usa la ventana normal (plana); el motivo está en las líneas anteriores.
* `panel 3840x1080 (stereo, 1920 x 1080 per eye)` → el panel es el esperado.
* `[vr] stereo: P00 ..., HUD mask yes/no` → el estéreo está funcionando; si dice `no stereo this frame (...)` explica
  qué falta (sin escena, sin profundidad o sin cámara) y muestra el panel plano en vez de una imagen rota.

Variables (en `env.txt`, una por línea, en `Android/data/com.lasrecomp.android/files/`):

```
LARECOMP_XR=1                    # arrancar en OpenXR (lo pone la entrada VR sola)
LARECOMP_XR_PASSTHROUGH=0        # sin passthrough (fondo negro en vez de tu habitación)
```

---

## 5. Qué se ha verificado aquí y qué falta probar en el visor

Verificado en este entorno (sin dispositivo):

* Los dos shaders nuevos compilan con **glslang 16.6.0** para Vulkan 1.1 (el mismo generador que usa la app vía
  shaderc) y **enlazan** con el vertex shader real de la app (`nv12.vert`): interfaz correcta.
* Los 7 archivos de código (más las dos cabeceras nuevas) compilan **sin errores ni avisos** con **clang 18**, el compilador del NDK,
  con los `-D` reales del proyecto (`VK_NO_PROTOTYPES`, `VK_USE_PLATFORM_ANDROID_KHR`, `__ANDROID__`).
* Aplicados sobre `main`, los cambios dan exactamente los archivos verificados (el parche que generé aplica limpio y su
  resultado coincide byte a byte con la rama).

Pendiente (necesita las gafas): arranque de la sesión OpenXR en Horizon OS, imagen por ojo, seguimiento de los mandos,
puntero de los menús, y el aspecto/rendimiento de cada modo. Es lo primero que hay que mirar en el log tras instalarlo.
