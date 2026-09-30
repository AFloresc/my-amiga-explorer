# 🚀 ApolloExplorer - Roadmap & Plan de Trabajo

Este documento detalla las mejoras planificadas tanto para el servidor de AmigaOS (`ApolloExplorerSrv`) como para el cliente de escritorio en Qt (`ApolloExplorer`).

---

## 💻 1. Servidor AmigaOS (`ApolloExplorerSrv` / C Nativo)

### 🔴 Prioridad Alta (Fiabilidad & Protocolo)
- [ ] **Soporte para Byte Offset / Reanudación (Resume)**
  - [ ] Añadir campo `byte_offset` en las estructuras de control (`protocolTypes.h`).
  - [ ] Implementar `Seek()` mediante `dos.library` en `ReceiveFile.c` y `SendFile.c` para reanudar descargas/subidas interrumpidas.
- [ ] **Validación de Integridad de Datos (Checksums)**
  - [ ] Implementar cálculo de checksum rápido (CRC32 o FNV-1a) por paquete o por archivo.
  - [ ] Enviar checksum en la cabecera del protocolo para verificar bloques corruptos en redes inestables.
- [ ] **Cancelación Activa de Operaciones (`CMD_ABORT`)**
  - [ ] Interceptar señales de aborto dentro del bucle de I/O en `ReceiveFile.c` y `SendFile.c`.
  - [ ] Garantizar el cierre limpio de sockets y descriptores de archivo de `dos.library` al abortar desde el cliente.

### 🟡 Prioridad Media (Rendimiento & Memoria)
- [ ] **Búfer de I/O Adaptativo y Gestión de Memoria**
  - [ ] Detectar tipo de RAM disponible (`AvailMem(MEMF_FAST)` vs `MEMF_CHIP`) para ajustar el tamaño del buffer de transferencia.
  - [ ] Implementar pool de buffers con `AllocVecTaskPooled` para minimizar la fragmentación de memoria en Amiga.
- [ ] **Compresión Ligera sobre la Marcha**
  - [ ] Evaluar e integrar un algoritmo ligero de compresión en tiempo real (RLE o Mini-LZ4) para transferencias de texto/archivos binarios no comprimidos.

---

## 🖥️ 2. Cliente Desktop (`ApolloExplorer` / Qt & C++)

### 🔴 Prioridad Alta (UI/UX & Estabilidad)
- [ ] **Gestor y Cola de Transferencias (Transfer Queue)**
  - [ ] Crear un panel inferior en la GUI para listar transferencias activas, pendientes, pausadas y completadas.
  - [ ] Permitir pausar, reanudar y cancelar transferencias individuales.
  - [ ] Añadir control de límite de ancho de banda (Bandwidth Throttling) para no saturar la pila TCP/IP del Amiga.
- [ ] **Arrastrar y Soltar (Drag & Drop)**
  - [ ] Permitir arrastrar archivos y directorios desde el Explorador de Windows/sistema directamente al navegador de archivos de Amiga.

### 🟡 Prioridad Media (Visualización & Integración con AmigaOS)
- [ ] **Previsualización Nativa de Formatos de Amiga**
  - [ ] Implementar decodificador IFF/ILBM a `QImage` para previsualizar imágenes clásicas de Amiga directamente en el cliente.
  - [ ] Integrar visor/explorador de imágenes de disco `.adf` y archivos comprimidos `.lha` / `.lzx`.
- [ ] **Mejoras en la Terminal Remota (`ash`)**
  - [ ] Añadir emulación VT100 / ANSI para interpretar correctamente los códigos de escape de colores de la Shell de Amiga.
  - [ ] Editor integrado con resaltado de sintaxis para revisar y editar archivos de texto remoto (`Startup-Sequence`, scripts).
- [ ] **Libreta de Direcciones Persistente (Bookmark Manager)**
  - [ ] Guardar perfiles de distintas Amigas (IP, puerto, nombre, icono personalizado y perfil de máquina/CPU).

---

## 🛠️ Flujo de Trabajo Recomendado

1. **Paso 1:** Elegir una tarea del Servidor o del Cliente.
2. **Paso 2:** Definir la estructura de datos o mensajes de protocolo necesarios (`protocolTypes.h`).
3. **Paso 3:** Implementar y probar localmente con Docker (servidor) y CMake/Qt (cliente).