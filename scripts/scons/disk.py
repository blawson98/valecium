# SPDX-License-Identifier: BSD-3-Clause

import os
import shutil
import struct
import subprocess
import textwrap

from scripts.scons.bootloader import (
    CreateElTorito,
    ValidateBootSetup,
)

VolumeLabel = "VALECIUM"
IsoSectorSize = 2048
IsoBootRecordLba = 17
IsoPvdLba = 16
IsoPvdLabelOffset = 0x28
IsoPvdUuidOffset = 0x32D
IsoBootCatalogLbaOffset = 0x47
IsoBootCatalogInitialEntryOffset = 0x20
IsoBootImageSectorCountOffset = 0x06
IsoBootImageLbaOffset = 0x08
CoreFsPatchSignature = b"VLSF"


def ReadIsoPvdFields(IsoPath: str) -> tuple[bytes, bytes]:
    PvdOffset = IsoPvdLba * IsoSectorSize
    with open(IsoPath, "rb") as FileHandle:
        FileHandle.seek(PvdOffset + IsoPvdLabelOffset)
        Label = FileHandle.read(32)
        FileHandle.seek(PvdOffset + IsoPvdUuidOffset)
        Uuid = FileHandle.read(16)
    if len(Label) != 32:
        raise ValueError("Partition label read failed")
    if len(Uuid) != 16:
        raise ValueError("Partition UUID read failed")
    return Label, Uuid


def PatchIsoBootImageCoreFs(IsoPath: str, Label: bytes, Uuid: bytes) -> None:
    if len(Label) != 32:
        raise ValueError(f"Partition label must be 32 bytes, got {len(Label)}")
    if len(Uuid) != 16:
        raise ValueError(f"Partition UUID must be 16 bytes, got {len(Uuid)}")

    with open(IsoPath, "rb") as FileHandle:
        Data = bytearray(FileHandle.read())

    BootRecordOff = IsoBootRecordLba * IsoSectorSize
    BootCatalogLba = struct.unpack_from(
        "<I", Data, BootRecordOff + IsoBootCatalogLbaOffset
    )[0]
    BootCatalogOff = BootCatalogLba * IsoSectorSize
    InitialEntryOff = BootCatalogOff + IsoBootCatalogInitialEntryOffset
    BootImageSectors = struct.unpack_from(
        "<H", Data, InitialEntryOff + IsoBootImageSectorCountOffset
    )[0]
    BootImageLba = struct.unpack_from(
        "<I", Data, InitialEntryOff + IsoBootImageLbaOffset
    )[0]

    BootImageOff = BootImageLba * IsoSectorSize
    BootImageSize = BootImageSectors * 512
    SearchEnd = BootImageOff + BootImageSize if BootImageSize else len(Data)

    SigOff = Data.find(CoreFsPatchSignature, BootImageOff, SearchEnd)
    if SigOff == -1:
        raise ValueError("CoreFS patch signature not found")
    if Data.find(CoreFsPatchSignature, SigOff + 1, SearchEnd) != -1:
        raise ValueError("CoreFS patch signature appears multiple times")

    LabelOff = SigOff + 8
    UuidOff = LabelOff + 32
    Data[LabelOff : LabelOff + 32] = Label
    Data[UuidOff : UuidOff + 16] = Uuid

    with open(IsoPath, "wb") as FileHandle:
        FileHandle.write(Data)


def RunCommand(Arguments: list, InputText: str = None, **kwargs):
    subprocess.run(
        Arguments,
        check=True,
        input=InputText,
        text=(InputText is not None),
        **kwargs,
    )


def CreateBootableIso(
    StagingDirectory: str,
    OutputIso: str,
    VolumeLabelName: str = VolumeLabel,
    Architecture: str = "i686",
    BootType: str = "bios",
    BootSystem: str = "grub",
    BootloaderComponents: dict = None,
):
    """Create a bootable ISO 9660 image.

    When *BootSystem* is ``'system'`` and *BootloaderComponents* provides Stage1
    and Stage2, the system bootloader is embedded via El Torito "no emulation"
    boot using ``xorriso`` directly.  Otherwise ``grub-mkrescue`` is used.
    """

    ValidateBootSetup(
        Architecture=Architecture,
        BootType=BootType,
        Bootloader=BootSystem,
    )

    UseSystemBootloader = (
        BootSystem == "system"
        and BootloaderComponents
        and BootloaderComponents.get("Stage1")
        and BootloaderComponents.get("Stage2")
    )

    if not UseSystemBootloader:
        print("   GRUB-MKRESCUE")
        RunCommand(
            [
                "grub-mkrescue",
                "-o",
                OutputIso,
                StagingDirectory,
                "--",
                "-volid",
                VolumeLabelName,
            ]
        )
        return

    Stage1Path = str(BootloaderComponents["Stage1"])
    Stage2Path = str(BootloaderComponents["Stage2"])

    PartitionLabelBytes = VolumeLabelName.encode("ascii", errors="replace").ljust(
        32, b" "
    )[:32]
    ElToritoPath = CreateElTorito(
        Stage1Path,
        Stage2Path,
        FileSystemType="iso9660",
        CoreFsBinaries=BootloaderComponents.get("CoreFsBinaries")
        if BootloaderComponents
        else None,
        PartitionLabel=PartitionLabelBytes,
    )
    LoadSectors = (os.path.getsize(ElToritoPath) + 511) // 512

    print(
        f"   XORRISO (El Torito: {os.path.basename(ElToritoPath)}, {LoadSectors} sectors)"
    )

    BootImageInStage = os.path.join(
        StagingDirectory, "boot", os.path.basename(ElToritoPath)
    )
    os.makedirs(os.path.dirname(BootImageInStage), exist_ok=True)
    shutil.copy2(ElToritoPath, BootImageInStage)

    RunCommand(
        [
            "xorriso",
            "-as",
            "mkisofs",
            "-o",
            OutputIso,
            "-b",
            os.path.relpath(BootImageInStage, StagingDirectory),
            "-no-emul-boot",
            "-boot-load-size",
            str(LoadSectors),
            "-boot-info-table",
            "-volid",
            VolumeLabelName,
            StagingDirectory,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    PartitionLabel, PartitionUuid = ReadIsoPvdFields(OutputIso)
    PatchIsoBootImageCoreFs(OutputIso, PartitionLabel, PartitionUuid)


def BuildGrubConfigContent(
    Config: str = "release",
    KernelName: str = "valeciumx",
    VolumeLabelName: str = VolumeLabel,
) -> str:
    Timeout = "0" if Config == "debug" else "10"

    return textwrap.dedent(f"""\
# Set a variable to prevent recursion loops
if [ -z "$configLoaded" ]; then
    set configLoaded=1

    # Force standard PC keyboard and console output
    terminal_input console
    terminal_output console

    set timeout_style=menu
    set timeout={Timeout}
    set default=0

    menuentry "Valecium OS" {{
        search --no-floppy --label {VolumeLabelName} --set=root
        multiboot /boot/{KernelName} root=LABEL={VolumeLabelName}
        boot
    }}

    menuentry "Reboot" {{
        reboot
    }}
fi
""")


def GenerateGrubConfig(
    GrubDirectory: str,
    Config: str = "release",
    KernelName: str = "valeciumx",
    VolumeLabelName: str = VolumeLabel,
) -> str:
    os.makedirs(GrubDirectory, exist_ok=True)
    ConfigPath = os.path.join(GrubDirectory, "grub.cfg")
    Content = BuildGrubConfigContent(Config, KernelName, VolumeLabelName)
    with open(ConfigPath, "w", encoding="utf-8") as FileHandle:
        FileHandle.write(Content)
    return ConfigPath
