# SPDX-License-Identifier: BSD-3-Clause

import os
import subprocess
import textwrap

from scripts.scons.bootloader import InstallSystemBootloader, ValidateBootSetup

VolumeLabel = 'VALECIUM'
EfiSystemPartitionGuid = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
LinuxFilesystemPartitionGuid = '0FC63DAF-8483-4772-8E79-3D69D8477DE4'
BiosBootPartitionGuid = '21686148-6449-6E6F-744E-656564454649'

FilesystemConfigurations = {
    'fat12': {
        'PartedType': 'fat12',
        'PartitionTypeIdentifier': '0x04',
        'MakeFilesystemCommand': 'mkfs.fat',
        'MakeFilesystemArguments': ['-F', '12'],
        'SupportsSymlinks': False,
    },
    'fat16': {
        'PartedType': 'fat16',
        'PartitionTypeIdentifier': '0x06',
        'MakeFilesystemCommand': 'mkfs.fat',
        'MakeFilesystemArguments': ['-F', '16'],
        'SupportsSymlinks': False,
    },
    'fat32': {
        'PartedType': 'fat32',
        'PartitionTypeIdentifier': '0x0c',
        'MakeFilesystemCommand': 'mkfs.fat',
        'MakeFilesystemArguments': ['-F', '32'],
        'SupportsSymlinks': False,
    },
    'ext2': {
        'PartedType': 'ext2',
        'PartitionTypeIdentifier': '0x83',
        'MakeFilesystemCommand': 'mkfs.ext2',
        'MakeFilesystemArguments': [],
        'SupportsSymlinks': True,
    },
}

PartitionMapModules = ['mbr', 'gpt']

EfiDefaultBinaryByArch = {
    'i686': 'BOOTIA32.EFI',
    'x86_64': 'BOOTx86_64.EFI',
    'aarch64': 'BOOTAA64.EFI',
}


def GetFilesystemConfig(Filesystem: str) -> dict:
    if Filesystem not in FilesystemConfigurations:
        raise ValueError(f"Unsupported filesystem: {Filesystem}. "
                        f"Supported: {list(FilesystemConfigurations.keys())}")
    return FilesystemConfigurations[Filesystem]


def GetSupportedFilesystems() -> list:
    return list(FilesystemConfigurations.keys())


def GetSupportedPartitionMaps() -> list:
    return list(PartitionMapModules)


def GetPartitionTypeIdentifier(Filesystem: str) -> str:
    return GetFilesystemConfig(Filesystem)['PartitionTypeIdentifier']


def GetEfiDefaultBinaryName(Architecture: str) -> str:
    if Architecture not in EfiDefaultBinaryByArch:
        raise ValueError(f"Unsupported EFI architecture: {Architecture}")
    return EfiDefaultBinaryByArch[Architecture]


def GetGptPartitionTypeGuid(BootType: str) -> str:
    if BootType == 'efi':
        return EfiSystemPartitionGuid
    return LinuxFilesystemPartitionGuid


def RunCommand(Arguments: list, InputText: str = None):
    subprocess.run(
        Arguments,
        check=True,
        input=InputText,
        text=(InputText is not None),
    )


def CreateBootableIso(StagingDirectory: str, OutputIso: str, VolumeLabelName: str = VolumeLabel):
    print("   GRUB-MKRESCUE")
    RunCommand(['grub-mkrescue', '-o', OutputIso, StagingDirectory, '--', '-volid', VolumeLabelName])


def CreateBootableDisk(
    Stage: str,
    ImagePath: str,
    Volume: str,
    Filesystem: str,
    PartMb: int,
    TotalMb: int,
    PartStartSector: int,
    PartitionTypeIdentifier: str,
    BootType: str,
    Architecture: str,
    PartitionMap: str,
    BootloaderComponents: dict,
):
    ValidateBootSetup(Architecture, PartitionMap, BootType)

    NeedsBiosBootPartition = (PartitionMap == 'gpt' and BootType == 'bios')
    RootPartitionIndex = 2 if NeedsBiosBootPartition else 1

    try:
        print(f"   CREATE DISK ({PartitionMap.upper()} + {Filesystem})")
        RunCommand(['truncate', '-s', f'{TotalMb}M', ImagePath])

        PartitionEndSector = '-1'
        if PartitionMap == 'gpt':
            PartitionEndSector = str((TotalMb * 2048) - 34)

        GfFsType = {
            'mkfs.fat': 'fat',
            'mkfs.ext2': 'ext2',
        }[GetFilesystemConfig(Filesystem)['MakeFilesystemCommand']]

        Commands = ['run']
        Commands.append(f'part-init /dev/sda {PartitionMap}')

        if NeedsBiosBootPartition:
            if PartStartSector - 1 < 34:
                raise RuntimeError(
                    'Not enough room before root partition for GPT BIOS boot partition'
                )
            Commands.extend([
                f'part-add /dev/sda p 34 {PartStartSector - 1}',
                f'part-set-gpt-type /dev/sda 1 {BiosBootPartitionGuid}',
            ])

        Commands.append(f'part-add /dev/sda p {PartStartSector} {PartitionEndSector}')

        if PartitionMap == 'mbr':
            Commands.append(f'part-set-mbr-id /dev/sda 1 {PartitionTypeIdentifier}')
        elif PartitionMap == 'gpt' and not NeedsBiosBootPartition:
            Commands.append(f'part-set-gpt-type /dev/sda 1 {GetGptPartitionTypeGuid(BootType)}')
        elif PartitionMap == 'gpt' and NeedsBiosBootPartition:
            Commands.append(f'part-set-gpt-type /dev/sda 2 {GetGptPartitionTypeGuid(BootType)}')

        FsPartition = f'/dev/sda{RootPartitionIndex}'
        Commands.append(f'mkfs {GfFsType} {FsPartition}')
        Commands.append(f'set-label {FsPartition} {Volume}')

        Commands.extend([
            f'mount {FsPartition} /',
            f'copy-in {Stage}/. /',
        ])
        Commands.extend(['quit', ''])
        RunCommand(['guestfish', '-a', ImagePath], InputText='\n'.join(Commands))

        InstallSystemBootloader(
            ImagePath,
            BootType,
            BootloaderComponents,
            PartStartSector,
        )
    finally:
        pass


def BuildGrubConfigContent(
    Config: str = 'release',
    KernelName: str = 'valeciumx',
    VolumeLabelName: str = VolumeLabel,
) -> str:
    Timeout = '0' if Config == 'debug' else '10'

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
    OutputFormat: str = 'img',
    Config: str = 'release',
    KernelName: str = 'valeciumx',
    VolumeLabelName: str = VolumeLabel,
) -> str:
    _ = OutputFormat
    os.makedirs(GrubDirectory, exist_ok=True)
    ConfigPath = os.path.join(GrubDirectory, 'grub.cfg')
    Content = BuildGrubConfigContent(Config, KernelName, VolumeLabelName)
    with open(ConfigPath, 'w', encoding='utf-8') as FileHandle:
        FileHandle.write(Content)
    return ConfigPath
