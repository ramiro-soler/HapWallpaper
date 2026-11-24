<p align="center"><img src="icon/logo.png" width="128" alt=""></p>

# HapWallpaper

[English](README.md) · **Español**

Fondo de pantalla animado para Windows 11 que reproduce video **HAP directamente con Direct3D 11**: los cuadros ya vienen en el formato de textura comprimida que la placa de video lee de forma nativa (BC1/DXT1), así que **no usa el decodificador de video ni un reproductor multimedia**.

[**Descargar la última versión**](https://github.com/ramiro-soler/HapWallpaper/releases/latest): un `.exe` chico, sin nada que instalar.

## Qué tiene de distinto

[HAP](https://hap.video/) no es nuevo: es un formato abierto de Vidvox para reproducir video como texturas comprimidas por GPU, usado hace años en shows en vivo y software de VJ (Resolume, TouchDesigner, VDMX…).

HapWallpaper lleva esa ruta al **escritorio de Windows**, en lugar de depender de un reproductor generalista (mpv, VLC, navegador o Media Foundation) como otros programas de fondos animados:

| | Fondo animado típico | HapWallpaper |
|---|---|---|
| Decodificación | Decodificador de video de la placa (NVDEC, VCN, Quick Sync) o CPU | **Ninguna**: el cuadro se copia tal cual a una textura BC1 |
| Conversión de color | YUV → RGB en cada cuadro | No hace falta: BC1 ya es RGB |
| Monitores G-Sync/FreeSync de alta frecuencia | Un video de 30/60 fps baja la pantalla a 30/60 Hz | Presenta en cada refresco; si el cuadro no cambió vuelve a presentar el mismo buffer (`DXGI_PRESENT_DO_NOT_SEQUENCE`) sin dibujar ni copiar nada, así el monitor sigue a su frecuencia máxima |
| Ventanas maximizadas o a pantalla completa | Varía | Se pausa **por monitor** (al instante cuando cambiás de ventana, si no en menos de un segundo; un juego en pantalla completa exclusiva pausa todos); detrás de la barra transparente queda el último cuadro |
| En pausa | Varía | 0 % de GPU |

Medido en una RTX 3060, 2560×1440 a 200 Hz, video de 30 fps:

| | H.264 (Media Foundation) | AV1 (Media Foundation) | **HAP** |
|---|---|---|---|
| Decodificador de video | 28–44 % | 6–18 % | **0 %** |
| CPU | 2,4 % | 0,4 % | 0,5 % |
| Motor 3D de la placa (fondo + compositor) | | | **~2 %** |
| Consumo de la placa (sin fondo: 18 W) | 22,2 W | 20,7 W | **18,7 W** |
| RAM (working set) | | | **51 MB** (1 MB en pausa) |

La contra: un archivo HAP es grande (~1,8 MB por cuadro a 1440p, ~1 GB para 18 s a 30 fps) en disco, y se lee a ~55 MB/s mientras se reproduce (nada para un SSD; evitá ponerlo en un disco rígido). Si preferís un archivo chico, también reproduce cualquier MP4/AV1 que Windows sepa decodificar, con la misma presentación.

### ¿Por qué los archivos HAP son tan grandes?

H.264, HEVC y AV1 logran archivos chicos guardando sobre todo las *diferencias* entre cuadros, que después hay que decodificar de vuelta a imágenes completas mientras se reproduce. HAP no: cada cuadro se guarda entero, ya en el formato de textura comprimida que la placa de video lee de forma nativa (BC1, 4 bits por píxel). Justamente por eso no necesita decodificador de video, pero también hace que el tamaño dependa solo de la resolución, los cuadros por segundo y la duración, no de lo que muestre el video:

| Resolución | Por cuadro | Por segundo a 30 fps | Bucle de 20 s |
|---|---|---|---|
| 1920×1080 | ~1 MB | ~30 MB | ~0,6 GB |
| 2560×1440 | ~1,8 MB | ~53 MB | ~1,1 GB |
| 3840×2160 | ~4 MB | ~119 MB | ~2,4 GB |

Así que el costo real no está en la placa ni en el procesador, sino en **leer el archivo del disco** a ese ritmo mientras el fondo se reproduce (en pausa se detiene). Un SSD lo maneja sin problema, y no lo desgasta: los SSD se gastan con las *escrituras*, no con las lecturas. Para que el archivo ocupe menos sin romper el bucle, usá 30 fps (60 lo duplica) y la resolución de tu monitor (`prepare_video.ps1` hace las dos cosas por defecto), o `-Format av1` para un MP4 chico que usa el decodificador de video.

## Medido contra otro programa de fondos

Misma PC (RTX 3060, Ryzen 7 5800X, Windows 11, un monitor 2560×1440 a 200 Hz), **mismo video a 2560×1440 y 30 fps**, escritorio a la vista, medido con los contadores de GPU de Windows y `nvidia-smi`:

| | **HapWallpaper 1.3.0** | Wallpaper Alive (Steam) |
|---|---|---|
| Decodificador de video | **0 %** | 30,5 % |
| Motor 3D | 1,9 % con la placa en reposo | 1,8 % con la placa a máxima velocidad |
| Velocidad de la placa al reproducir | **Reposo (P8/P5)** | Máxima (P0, 1567 MHz núcleo / 7501 MHz memoria) |
| Consumo de la placa (sin fondo: 18 W) | **18,7 W** | 41,1 W |
| RAM (working set) | **51 MB**, 1 proceso | 552 MB, 5 procesos |
| CPU | 0,5 % | 0,65 % |

El porcentaje de 3D del Administrador de tareas se calcula sobre la velocidad actual de la placa, así que un porcentaje parecido a máxima velocidad es varias veces más trabajo real que en reposo; el consumo de la placa es la comparación más clara. Una sola PC y un solo video: en otro hardware los resultados pueden variar.
## Funciones

- **Todos los monitores**, cada uno con su ventana y su propia pausa. Un mismo archivo HAP llena cualquier resolución y proporción (se escala y recorta al centro en la placa).
- **Pausa automática** con ventanas maximizadas o a pantalla completa, juegos en pantalla completa exclusiva, sesión bloqueada o pantalla apagada. Antes de pausarse siempre muestra un cuadro, así la barra nunca queda en negro.
- **Barra de tareas transparente**, prendida por defecto; se apaga o prende desde el menú del ícono de la bandeja (ver la nota de abajo).
- **Respaldo con video común:** sin `.hap`, reproduce `wallpaper.mp4` (H.264, AV1…) con Media Foundation y decodificación por hardware.
- Ícono en la bandeja (doble clic pausa/reanuda, y avisa si el `.hap` no se puede usar) y arranque con Windows.

## Preparar el video

Lo más complicado de HAP es armar el archivo, así que `prepare_video.ps1` lo hace en un paso con [ffmpeg](https://ffmpeg.org/). Hace falta un ffmpeg con el codificador HAP (que necesita libsnappy): el build *full* de Gyan lo trae, pero los builds *essentials*, los de BtbN y muchos que vienen con otros programas no. El script revisa todos los ffmpeg que haya en la PC y usa uno que sirva; si no encuentra ninguno, ofrece instalar el full con `winget install --id Gyan.FFmpeg -e`. También podés indicarle uno con `-FFmpeg C:\ruta\ffmpeg.exe`.

```powershell
.\prepare_video.ps1 -Video C:\videos\mi_wallpaper.mp4
```

Detecta la resolución del monitor principal, escala y recorta el video para llenarlo, lo pasa a 30 fps, genera `wallpaper.hap` junto al script, lo verifica y avisa cuánto va a ocupar.

| Opción | Para qué |
|---|---|
| `-Format hap` / `av1` / `both` | `hap` = 0 % de decodificador, archivo grande. `av1` = `wallpaper.mp4` chico que usa el decodificador de la placa. `both` deja el MP4 de respaldo |
| `-Fps 30` | Cuadros por segundo del archivo (el reproductor los lee). 60 duplica el tamaño |
| `-Start 5 -Duration 20` | Usar solo un tramo del video (en segundos). Menos duración = archivo más chico |
| `-ClockShadow` | Oscurece suavemente la esquina del reloj y la bandeja para leerlos mejor con la barra transparente |
| `-Width 1920 -Height 1080` | Forzar una resolución (por ejemplo, la de tu monitor más grande) |
| `-Output C:\ruta` | Carpeta de salida (por defecto, la del script) |
| `-FFmpeg C:\ruta\ffmpeg.exe` | Usar un ffmpeg concreto en vez de buscarlo |

Usá videos propios o que tengas permiso de usar. Los que hacen bucle sin corte se ven mejor.

## Uso

Dejá `wallpaper.hap` (y/o `wallpaper.mp4`) junto a `HapWallpaper.exe` y abrilo. Si están los dos, usa el `.hap`.

```
HapWallpaper.exe [video.mp4 | wallpaper.hap] [--debug] [--nopause] [--notaskbar] [--forcetaskbar] [--noautostart]
```

| Opción | Efecto |
|---|---|
| `--debug` | Escribe `log.txt` (y `tb_log.txt` de la DLL de la barra) junto al `.exe` |
| `--nopause` | No se pausa con ventanas maximizadas ni pantalla completa |
| `--notaskbar` | No toca la barra de tareas en esta sesión (no se carga la DLL) |
| `--forcetaskbar` | Prende la barra transparente aunque haya otro programa manejándola (puede romperlo) |
| `--noautostart` | No se anota en el inicio de Windows |

Para cerrarlo: clic derecho en el ícono de la bandeja → Exit.

### Sobre la DLL de la barra

La barra transparente funciona como [TranslucentTB](https://github.com/TranslucentTB/TranslucentTB): `HapWallpaperTB.dll` se carga en Explorer con la API de diagnóstico XAML de Windows y pone en 0 la opacidad del fondo de la barra. Como corre dentro de Explorer, **algún antivirus puede marcarla**. El código está en `hapwallpaper_tb.cpp`.

- **Se apaga desde la bandeja:** clic derecho en el ícono → *Transparent taskbar*. La elección se guarda en `HapWallpaper.ini` junto al `.exe`, y apagada no se inyecta nada (también sirve `--notaskbar` o borrar la DLL). Si la DLL ya estaba cargada, queda en Explorer hasta que Explorer se reinicie, así que al apagarla ofrece reiniciarlo en el momento; si no, la barra vuelve del todo a la normalidad la próxima vez que inicies sesión.
- **Convive con otros programas de barra:** Windows deja que un solo programa use esa API dentro de Explorer. Si ya tenés **TranslucentTB** o un mod de barra de **Windhawk** (como *Windows 11 Taskbar Styler*), HapWallpaper lo detecta y no toca la barra: la manejás con ese programa, que también puede dejarla transparente. Si la prendés a mano desde el menú, te avisa que no son compatibles. El fondo animado funciona igual en todos los casos.
- **Puede romperse con una actualización de Windows:** la API de diagnóstico XAML es pública, pero la estructura interna de la barra (los elementos `BackgroundFill` y `BackgroundStroke` que oculta) no, y Microsoft puede cambiarla en cualquier momento. TranslucentTB y los mods de Windhawk dependen de lo mismo. Si pasa, la barra simplemente deja de ponerse transparente; el fondo animado sigue funcionando.

## Compilar

Con MSYS2 (UCRT64):

```powershell
windres icon/app.rc -O coff -o app_res.o
g++ -O2 -s -municode -mwindows -static -o HapWallpaper.exe hapwallpaper.cpp app_res.o -ld3d11 -ldxgi -lmfplat -lmfuuid -ldwmapi -lshell32 -lwtsapi32 -lrstrtmgr -lole32 -loleaut32 -luuid -luser32 -lgdi32
g++ -O2 -s -shared -static -municode -o HapWallpaperTB.dll hapwallpaper_tb.cpp -lole32 -loleaut32 -luuid -luser32
```

## Créditos y licencia

- [HAP](https://hap.video/) — formato abierto creado por Tom Butterworth para Vidvox.
- [ffmpeg](https://ffmpeg.org/) — codificador HAP usado por `prepare_video.ps1`.

Código bajo licencia [MIT](LICENSE). No incluye videos.
