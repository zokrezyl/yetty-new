@echo off
REM Run the Windows-built qemu-system-riscv64.exe against the runtime/ assets
REM (kernel + opensbi + alpine rootfs disk image). Mirrors run-qemu-smp.sh
REM but adapted for the slirp-less, virtfs-less Windows minimal build:
REM   * no -netdev user (slirp disabled)
REM   * no virtio-9p (virtfs disabled) -> use virtio-blk on alpine-rootfs.img

setlocal

set SCRIPT_DIR=%~dp0
set QEMU_ROOT=%SCRIPT_DIR%..\..\..
set RUNTIME=%SCRIPT_DIR%..\runtime
set BUILD=%QEMU_ROOT%\build-windows-minimal

set QEMU=%BUILD%\qemu-system-riscv64.exe
set BIOS=%RUNTIME%\opensbi-fw_jump.elf
set KERNEL=%RUNTIME%\kernel-riscv64.bin
set ROOTFS=%RUNTIME%\alpine-rootfs.img

if not exist "%QEMU%"   echo ERROR: %QEMU% not found    & exit /b 1
if not exist "%BIOS%"   echo ERROR: %BIOS% not found    & exit /b 1
if not exist "%KERNEL%" echo ERROR: %KERNEL% not found  & exit /b 1
if not exist "%ROOTFS%" echo ERROR: %ROOTFS% not found  & exit /b 1

"%QEMU%" ^
  -machine virt ^
  -smp 1 ^
  -m 256 ^
  -bios "%BIOS%" ^
  -kernel "%KERNEL%" ^
  -append "earlycon=sbi console=hvc0 root=/dev/vda rw init=/init" ^
  -drive file="%ROOTFS%",format=raw,if=none,id=hd0 ^
  -device virtio-blk-device,drive=hd0 ^
  -device virtio-serial-device ^
  -device virtconsole,chardev=char0 ^
  -chardev stdio,id=char0,signal=off ^
  -serial none ^
  -display none

endlocal
